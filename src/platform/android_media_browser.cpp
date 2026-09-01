/**
 * VitaPlex - Android media browsing bridge (Android Auto / Assistant / Wear)
 *
 * Native half of org.VitaPlex.app.LibraryBrowserService. Java receives
 * onLoadChildren, detaches the Result and calls nativeLoadChildren() with a
 * token; we resolve the node against PlexClient on a worker thread and hand the
 * rows back through LibraryBrowserService.deliverChildren(), which re-posts to
 * the Android main looper and completes the Result.
 *
 * JNI symbol names mangle to Java_org_VitaPlex_app_LibraryBrowserService_*, so
 * the Java package/class must stay org.VitaPlex.app.LibraryBrowserService.
 *
 * Scope is the music library: media browsing surfaces are audio-first, and the
 * app already owns a music queue + MediaSession that a browser client can drive.
 */

#ifdef __ANDROID__

#include <SDL2/SDL.h>
#include <jni.h>

#include <borealis.hpp>
#include <string>
#include <vector>

#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "app/music_queue.hpp"
#include "app/music_controller.hpp"
#include "activity/player_activity.hpp"
#include "platform/paths.hpp"
#include "utils/async.hpp"

#include <cctype>
#include <mutex>

using namespace vitaplex;

namespace {

// Cached at nativeInit() time from the service's onCreate, which runs on a Java
// thread with the app classloader. Worker threads cannot FindClass app classes
// (an attached native thread gets the system classloader), and going through
// brls::sync would tie browsing to the borealis main loop — which is exactly
// what is missing when a browser binds the service cold. Caching the class and
// method up front lets any thread deliver results with no loop and no lookup.
JavaVM*   g_vm            = nullptr;
jclass    g_serviceClass  = nullptr;   // global ref
jmethodID g_deliverMethod = nullptr;

// android.media.browse.MediaBrowser.MediaItem flags. Keep in sync with Java.
constexpr int FLAG_BROWSABLE = 1 << 0;
constexpr int FLAG_PLAYABLE  = 1 << 1;

// Browse-tree node ids. A node is "<prefix>/<key>"; the root and the playlist
// index are bare words. Ids are opaque to the browser client and only ever
// interpreted here.
constexpr const char* kRootId      = "__root__";
constexpr const char* kPlaylistsId = "playlists";

// Cover art size requested for browser rows. Auto/Assistant render small
// thumbnails, so this keeps the fetch cheap on the server side.
constexpr int kArtSize = 320;

struct BrowseRow {
    std::string id;
    std::string title;
    std::string subtitle;
    std::string iconUri;
    int flags = FLAG_BROWSABLE;
};

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// "artist/1234" -> "1234"
std::string suffixAfter(const std::string& s, const std::string& prefix) {
    return s.size() > prefix.size() ? s.substr(prefix.size()) : std::string();
}

std::string artUri(PlexClient& client, const std::string& thumb) {
    if (thumb.empty()) return {};
    return client.getThumbnailUrl(thumb, kArtSize, kArtSize);
}

// Make the Plex client usable in a process that never started the UI.
//
// A cold service bind has no borealis Application and no restored session, so
// the client has no server or token and every lookup would fail. The saved
// config already holds both; load it and connect once, guarded so concurrent
// browse requests don't race. Returns false when there is nothing saved (not
// signed in yet), which the caller turns into a visible row rather than an
// unexplained empty list.
bool ensureClientReady() {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);

    PlexClient& client = PlexClient::getInstance();
    if (!client.getServerUrl().empty()) return true;  // app already connected

    auto& app = vitaplex::Application::getInstance();
    if (app.getServerUrl().empty()) app.loadSettings();
    if (app.getServerUrl().empty()) {
        brls::Logger::warning("MediaBrowser: no saved server, cannot browse cold");
        return false;
    }

    client.setAuthToken(app.getAuthToken());
    const bool ok = client.connectToServer(app.getServerUrl());
    brls::Logger::info("MediaBrowser: cold client bootstrap {}", ok ? "ok" : "failed");
    return ok;
}

// Push rows back to Java. Uses the refs cached by nativeInit, so this works
// from any worker thread and without the borealis loop running.
void deliverRows(int token, const std::vector<BrowseRow>& rows) {
    if (!g_vm || !g_serviceClass || !g_deliverMethod) {
        brls::Logger::warning("MediaBrowser: JNI not initialised, dropping {} row(s)",
                              rows.size());
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
            brls::Logger::warning("MediaBrowser: AttachCurrentThread failed");
            return;
        }
        attached = true;
    }

    jclass    cls = g_serviceClass;
    jmethodID mid = g_deliverMethod;

    const jsize n = (jsize)rows.size();
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray ids   = env->NewObjectArray(n, strCls, nullptr);
    jobjectArray tits  = env->NewObjectArray(n, strCls, nullptr);
    jobjectArray subs  = env->NewObjectArray(n, strCls, nullptr);
    jobjectArray icons = env->NewObjectArray(n, strCls, nullptr);
    jintArray    flags = env->NewIntArray(n);

    std::vector<jint> flagBuf;
    flagBuf.reserve(rows.size());

    for (jsize i = 0; i < n; i++) {
        const BrowseRow& r = rows[(size_t)i];
        jstring jid   = env->NewStringUTF(r.id.c_str());
        jstring jtit  = env->NewStringUTF(r.title.c_str());
        jstring jsub  = env->NewStringUTF(r.subtitle.c_str());
        jstring jicon = env->NewStringUTF(r.iconUri.c_str());
        env->SetObjectArrayElement(ids,   i, jid);
        env->SetObjectArrayElement(tits,  i, jtit);
        env->SetObjectArrayElement(subs,  i, jsub);
        env->SetObjectArrayElement(icons, i, jicon);
        env->DeleteLocalRef(jid);
        env->DeleteLocalRef(jtit);
        env->DeleteLocalRef(jsub);
        env->DeleteLocalRef(jicon);
        flagBuf.push_back(r.flags);
    }
    if (n > 0) env->SetIntArrayRegion(flags, 0, n, flagBuf.data());

    env->CallStaticVoidMethod(cls, mid, (jint)token, ids, tits, subs, icons, flags);
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->DeleteLocalRef(ids);
    env->DeleteLocalRef(tits);
    env->DeleteLocalRef(subs);
    env->DeleteLocalRef(icons);
    env->DeleteLocalRef(flags);
    env->DeleteLocalRef(strCls);

    // Only threads we attached here get detached; the borealis/SDL threads are
    // owned by SDL and must keep their attachment.
    if (attached) g_vm->DetachCurrentThread();
}

// ---------------------------------------------------------------------------
// Node resolution. Runs on a worker thread; no UI or GL work here.
// ---------------------------------------------------------------------------

std::vector<BrowseRow> loadRoot(PlexClient& client) {
    std::vector<BrowseRow> rows;

    std::vector<LibrarySection> sections;
    if (client.fetchLibrarySections(sections)) {
        for (const auto& s : sections) {
            if (s.type != "artist") continue;  // music libraries only
            BrowseRow r;
            r.id      = "lib/" + s.key;
            r.title   = s.title;
            r.iconUri = artUri(client, s.thumb);
            r.flags   = FLAG_BROWSABLE;
            rows.push_back(std::move(r));
        }
    }

    std::vector<Playlist> playlists;
    if (client.fetchMusicPlaylists(playlists) && !playlists.empty()) {
        BrowseRow r;
        r.id    = kPlaylistsId;
        r.title = "Playlists";
        r.flags = FLAG_BROWSABLE;
        rows.push_back(std::move(r));
    }

    return rows;
}

std::vector<BrowseRow> loadArtists(PlexClient& client, const std::string& sectionKey) {
    std::vector<BrowseRow> rows;
    std::vector<MediaItem> items;
    // Plex metadata type 8 = artist.
    if (!client.fetchLibraryContent(sectionKey, items, 8)) return rows;

    for (const auto& it : items) {
        BrowseRow r;
        r.id      = "artist/" + it.ratingKey;
        r.title   = it.title;
        r.iconUri = artUri(client, it.thumb);
        r.flags   = FLAG_BROWSABLE;
        rows.push_back(std::move(r));
    }
    return rows;
}

// Artists -> albums and albums -> tracks are both /children.
std::vector<BrowseRow> loadChildrenOf(PlexClient& client, const std::string& ratingKey,
                                      bool childrenArePlayable) {
    std::vector<BrowseRow> rows;
    std::vector<MediaItem> items;
    if (!client.fetchChildren(ratingKey, items)) return rows;

    for (const auto& it : items) {
        BrowseRow r;
        r.title    = it.title;
        r.iconUri  = artUri(client, !it.thumb.empty() ? it.thumb : it.parentThumb);
        if (childrenArePlayable || it.mediaType == MediaType::MUSIC_TRACK) {
            r.id       = "track/" + it.ratingKey;
            r.subtitle = !it.grandparentTitle.empty() ? it.grandparentTitle : it.parentTitle;
            r.flags    = FLAG_PLAYABLE;
        } else {
            r.id       = "album/" + it.ratingKey;
            r.subtitle = it.year > 0 ? std::to_string(it.year) : std::string();
            // Browsable AND playable: tapping opens the track list, while a
            // client's play action (or "play <album>" by voice) plays the whole
            // album. Browsable-only would make the album unplayable as a unit.
            r.flags    = FLAG_BROWSABLE | FLAG_PLAYABLE;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<BrowseRow> loadPlaylists(PlexClient& client) {
    std::vector<BrowseRow> rows;
    std::vector<Playlist> playlists;
    if (!client.fetchMusicPlaylists(playlists)) return rows;

    for (const auto& p : playlists) {
        BrowseRow r;
        r.id       = "playlist/" + p.ratingKey;
        r.title    = p.title;
        r.subtitle = p.leafCount > 0 ? std::to_string(p.leafCount) + " tracks" : std::string();
        r.iconUri  = artUri(client, !p.thumb.empty() ? p.thumb : p.composite);
        // Open it to pick a track, or play the whole playlist straight from the
        // list — same reasoning as albums above.
        r.flags    = FLAG_BROWSABLE | FLAG_PLAYABLE;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<BrowseRow> loadPlaylistItems(PlexClient& client, const std::string& playlistId) {
    std::vector<BrowseRow> rows;
    std::vector<PlaylistItem> items;
    if (!client.fetchPlaylistItems(playlistId, items)) return rows;

    for (const auto& pi : items) {
        const MediaItem& t = pi.media;
        BrowseRow r;
        // Carry the playlist through the id. A browser hands back exactly the id
        // it was given, and a bare track/<key> would be indistinguishable from
        // the same track picked in an album — playback would then continue with
        // the album instead of the rest of the playlist.
        r.id       = "ptrack/" + playlistId + "/" + t.ratingKey;
        r.title    = t.title;
        r.subtitle = !t.grandparentTitle.empty() ? t.grandparentTitle : t.parentTitle;
        r.iconUri  = artUri(client, !t.thumb.empty() ? t.thumb : t.parentThumb);
        r.flags    = FLAG_PLAYABLE;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<BrowseRow> resolveNode(const std::string& parentId) {
    PlexClient& client = PlexClient::getInstance();

    if (parentId == kRootId)      return loadRoot(client);
    if (parentId == kPlaylistsId) return loadPlaylists(client);

    if (startsWith(parentId, "lib/"))
        return loadArtists(client, suffixAfter(parentId, "lib/"));
    if (startsWith(parentId, "artist/"))
        return loadChildrenOf(client, suffixAfter(parentId, "artist/"), false);
    if (startsWith(parentId, "album/"))
        return loadChildrenOf(client, suffixAfter(parentId, "album/"), true);
    if (startsWith(parentId, "playlist/"))
        return loadPlaylistItems(client, suffixAfter(parentId, "playlist/"));

    return {};
}

// Resolve a browse-tree media id to tracks and start them, honouring the user's
// default track action. Runs on a worker thread (it does network I/O), hopping
// to the UI loop only to touch the queue. Shared by the media-id and the
// voice-search entry points below.
void playMediaId(const std::string& mediaId) {
    if (!ensureClientReady()) return;

    PlexClient& client = PlexClient::getInstance();
    std::vector<MediaItem> tracks;
    int startIndex = 0;
    // Whether the user picked one track or a whole album/playlist. Only
    // matters for the queue-preserving actions: picking a single track must
    // enqueue that track, not the album it happens to sit on.
    bool singlePick = false;

    if (startsWith(mediaId, "ptrack/")) {
        // A track picked inside a playlist: play it with the playlist as
        // context so the queue continues through the playlist, not the
        // track's album.
        singlePick = true;
        const std::string rest = suffixAfter(mediaId, "ptrack/");
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return;
        const std::string playlistId = rest.substr(0, slash);
        const std::string trackKey   = rest.substr(slash + 1);

        std::vector<PlaylistItem> items;
        if (client.fetchPlaylistItems(playlistId, items)) {
            for (const auto& pi : items) tracks.push_back(pi.media);
            for (size_t i = 0; i < tracks.size(); i++) {
                if (tracks[i].ratingKey == trackKey) { startIndex = (int)i; break; }
            }
        }
        if (tracks.empty()) {
            // Playlist fetch failed — fall back to the track on its own.
            MediaItem track;
            if (client.fetchMediaDetails(trackKey, track)) tracks.push_back(track);
        }
    } else if (startsWith(mediaId, "track/")) {
        singlePick = true;
        // A single track: play it in the context of its album so the queue
        // continues past it, exactly as picking a track in the app does.
        MediaItem track;
        if (!client.fetchMediaDetails(suffixAfter(mediaId, "track/"), track)) return;

        if (!track.parentRatingKey.empty() &&
            client.fetchChildren(track.parentRatingKey, tracks) && !tracks.empty()) {
            for (size_t i = 0; i < tracks.size(); i++) {
                if (tracks[i].ratingKey == track.ratingKey) {
                    startIndex = (int)i;
                    break;
                }
            }
        } else {
            tracks.push_back(track);
        }
    } else if (startsWith(mediaId, "album/")) {
        client.fetchChildren(suffixAfter(mediaId, "album/"), tracks);
    } else if (startsWith(mediaId, "playlist/")) {
        std::vector<PlaylistItem> items;
        if (client.fetchPlaylistItems(suffixAfter(mediaId, "playlist/"), items)) {
            for (const auto& pi : items) tracks.push_back(pi.media);
        }
    }

    if (tracks.empty()) {
        brls::Logger::warning("MediaBrowser: nothing playable for {}", mediaId);
        return;
    }

    brls::sync([tracks, startIndex, singlePick]() {
        MusicQueue& queue = MusicQueue::getInstance();
        TrackDefaultAction action =
            vitaplex::Application::getInstance().getSettings().trackDefaultAction;

        // ASK_EACH_TIME cannot ask — the whole point of browsing from a
        // watch or head unit is that nobody is at the phone. Fall back to
        // PLAY_NOW_REPLACE rather than PLAY_NOW_CLEAR: the pick still starts
        // playing immediately, which is what "play" means in a media
        // browser, but a queue the user never asked to clear survives.
        if (action == TrackDefaultAction::ASK_EACH_TIME)
            action = TrackDefaultAction::PLAY_NOW_REPLACE;

        // Nothing to preserve when the queue is empty, and "clear" wants the
        // full album/playlist context so playback continues past the pick.
        // Both need the player activity.
        if (queue.isEmpty() || action == TrackDefaultAction::PLAY_NOW_CLEAR) {
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(tracks, startIndex));
            return;
        }

        // Queue-preserving actions operate on exactly what was picked.
        std::vector<MediaItem> picked;
        if (singlePick && startIndex < (int)tracks.size())
            picked.push_back(tracks[(size_t)startIndex]);
        else
            picked = tracks;
        if (picked.empty()) return;

        // insertTrackAfterCurrent puts each item directly after the current
        // track, so inserting back-to-front leaves them in playing order.
        auto insertAfterCurrent = [&]() {
            for (size_t i = picked.size(); i-- > 0;)
                queue.insertTrackAfterCurrent(picked[i]);
        };

        switch (action) {
            case TrackDefaultAction::PLAY_NEXT:
                insertAfterCurrent();
                brls::Application::notify("Playing next: " + picked.front().title);
                break;

            case TrackDefaultAction::ADD_TO_BOTTOM:
                queue.addTracks(picked);
                brls::Application::notify("Added to queue: " + picked.front().title);
                break;

            case TrackDefaultAction::PLAY_NOW_REPLACE:
            default: {
                // Jump to the inserted track by index rather than calling
                // playNext(): under RepeatMode::ONE playNext() deliberately
                // stays on the current track, so it would report success
                // while the pick never played. insertTrackAfterCurrent puts
                // the first item at currentIndex + 1 (and keeps the shuffle
                // order in step), so that is the target either way.
                const int target = queue.getCurrentIndex() + 1;
                insertAfterCurrent();
                if (queue.playTrack(target))
                    brls::Application::notify("Now playing: " + picked.front().title);
                break;
            }
        }
    });
}

// Turn a spoken query into something playable.
//
// Assistant hands over free text ("play <album> by <artist>"), so this searches
// the library and picks the best music hit, preferring the broadest thing that
// matched: an artist or album gives a full listening session, a bare track only
// gives three minutes. An exact title match wins over a partial one at the same
// type, so "play Kid A" doesn't land on a compilation that merely mentions it.
//
// Returns a browse-tree media id for playMediaId(), or empty when nothing in
// the music library matched.
std::string resolveSearchQuery(PlexClient& client, const std::string& query) {
    std::vector<MediaItem> results;
    if (!client.search(query, results) || results.empty()) return {};

    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string want = lower(query);

    // Higher is better. Type first (an artist beats a track), exactness second.
    auto score = [&](const MediaItem& m) -> int {
        int base;
        if (m.type == "artist")        base = 40;
        else if (m.type == "album")    base = 30;
        else if (m.type == "playlist") base = 20;
        else if (m.type == "track")    base = 10;
        else                           return -1;   // movies, shows: not music
        const std::string t = lower(m.title);
        if (t == want) return base + 3;
        if (startsWith(t, want)) return base + 2;
        if (t.find(want) != std::string::npos) return base + 1;
        return base;
    };

    const MediaItem* best = nullptr;
    int bestScore = 0;
    for (const auto& m : results) {
        const int s = score(m);
        if (s > bestScore) { bestScore = s; best = &m; }
    }
    if (!best) return {};

    if (best->type == "album")    return "album/" + best->ratingKey;
    if (best->type == "playlist") return "playlist/" + best->ratingKey;
    if (best->type == "track")    return "track/" + best->ratingKey;

    // An artist has no single playable node: pick their first album, which
    // playMediaId() expands into a full track list.
    std::vector<MediaItem> albums;
    if (client.fetchChildren(best->ratingKey, albums) && !albums.empty())
        return "album/" + albums.front().ratingKey;
    return {};
}

}  // namespace

extern "C" {

/**
 * Called from LibraryBrowserService.onCreate() — a Java thread with the app
 * classloader, so this is the one place the service class can be looked up
 * reliably. Caches what deliverRows() needs and seeds the data directory from
 * the service's Context, which is how a cold process finds the saved config
 * without SDL's JNI setup having run.
 */
JNIEXPORT void JNICALL
Java_org_VitaPlex_app_LibraryBrowserService_nativeInit(JNIEnv* env, jclass cls,
                                                       jstring jFilesDir) {
    if (env->GetJavaVM(&g_vm) != JNI_OK) g_vm = nullptr;

    if (!g_serviceClass) g_serviceClass = (jclass)env->NewGlobalRef(cls);
    if (!g_deliverMethod) {
        g_deliverMethod = env->GetStaticMethodID(
            cls, "deliverChildren", "(I[Ljava/lang/String;[Ljava/lang/String;"
                                    "[Ljava/lang/String;[Ljava/lang/String;[I)V");
        if (!g_deliverMethod) env->ExceptionClear();
    }

    if (jFilesDir) {
        const char* raw = env->GetStringUTFChars(jFilesDir, nullptr);
        if (raw) {
            setAndroidDataDir(raw);
            env->ReleaseStringUTFChars(jFilesDir, raw);
        }
    }

    brls::Logger::info("MediaBrowser: native bridge ready (vm={}, deliver={})",
                       g_vm != nullptr, g_deliverMethod != nullptr);
}

JNIEXPORT void JNICALL
Java_org_VitaPlex_app_LibraryBrowserService_nativeLoadChildren(JNIEnv* env, jclass,
                                                               jstring jParentId, jint token) {
    const char* raw = jParentId ? env->GetStringUTFChars(jParentId, nullptr) : nullptr;
    std::string parentId = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(jParentId, raw);

    // info, not debug: a cold service process never runs the app's
    // setLogLevel(), so it sits at borealis' LOG_INFO default and a debug line
    // here would be invisible — exactly when this trace matters most. Browse
    // requests are user-paced, so this is not a hot path.
    brls::Logger::info("MediaBrowser: loadChildren({}) token={}", parentId, (int)token);

    // Plex lookups are blocking HTTP, so they must not run on the binder thread
    // that delivered onLoadChildren. Deliver straight from the worker: the JNI
    // refs cached in nativeInit make that safe without the borealis loop, which
    // is what lets browsing work when the service was bound cold.
    asyncRun([parentId, token]() {
        if (!ensureClientReady()) {
            std::vector<BrowseRow> row(1);
            row[0].id    = "__signed_out__";
            row[0].title = "Sign in to VitaPlex to browse your library";
            row[0].flags = 0;  // neither browsable nor playable
            deliverRows((int)token, row);
            return;
        }
        std::vector<BrowseRow> rows = resolveNode(parentId);
        brls::Logger::info("MediaBrowser: {} -> {} row(s)", parentId, rows.size());
        deliverRows((int)token, rows);
    });
}

JNIEXPORT void JNICALL
Java_org_VitaPlex_app_LibraryBrowserService_nativePlayFromMediaId(JNIEnv* env, jclass,
                                                                  jstring jMediaId) {
    const char* raw = jMediaId ? env->GetStringUTFChars(jMediaId, nullptr) : nullptr;
    std::string mediaId = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(jMediaId, raw);

    brls::Logger::info("MediaBrowser: playFromMediaId({})", mediaId);
    asyncRun([mediaId]() { playMediaId(mediaId); });
}

/**
 * "Hey Google, play <something> on VitaPlex" — MediaSessionCompat's
 * onPlayFromSearch, routed through the browser service so it shares the same
 * cold-bind client bootstrap.
 *
 * An empty query means "play something": Assistant sends it for a bare "play
 * music", and the contract is to start *any* reasonable playback rather than
 * fail. Resuming what's already queued is the least surprising answer.
 */
JNIEXPORT void JNICALL
Java_org_VitaPlex_app_LibraryBrowserService_nativePlayFromSearch(JNIEnv* env, jclass,
                                                                 jstring jQuery) {
    const char* raw = jQuery ? env->GetStringUTFChars(jQuery, nullptr) : nullptr;
    std::string query = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(jQuery, raw);

    brls::Logger::info("MediaBrowser: playFromSearch({})", query);

    asyncRun([query]() {
        if (!ensureClientReady()) return;

        if (query.empty()) {
            brls::sync([]() {
                if (!MusicQueue::getInstance().isEmpty())
                    MusicController::getInstance().playPause(true);
            });
            return;
        }

        const std::string mediaId = resolveSearchQuery(PlexClient::getInstance(), query);
        if (mediaId.empty()) {
            brls::Logger::warning("MediaBrowser: no music match for \"{}\"", query);
            return;
        }
        playMediaId(mediaId);
    });
}

}  // extern "C"

#endif  // __ANDROID__
