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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>
#include <set>

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

// A library the user hid (Settings, Manage Hidden Libraries): left out of the
// browser and its searches, as it is out of the sidebar and Home.
bool isHiddenLibrary(const std::string& sectionKey) {
    if (sectionKey.empty()) return false;
    const std::string& csv = Application::getInstance().getSettings().hiddenLibraries;
    size_t at = 0;
    while (at <= csv.size()) {
        size_t end = csv.find(',', at);
        if (end == std::string::npos) end = csv.size();
        if (csv.compare(at, end - at, sectionKey) == 0 && end - at == sectionKey.size()) return true;
        at = end + 1;
    }
    return false;
}

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
            if (isHiddenLibrary(s.key)) continue;
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

// An artist: its albums (/children), then its singles, EPs, compilations and
// the rest, which /children leaves out (PlexClient::artistReleaseCategories),
// each with its kind beside its year. One list, not a folder a kind: a car's
// screen is better with fewer levels to go through.
std::vector<BrowseRow> loadArtist(PlexClient& client, const std::string& artistKey) {
    std::vector<BrowseRow> rows = loadChildrenOf(client, artistKey, false);

    MediaItem artist;
    if (!client.fetchMediaDetails(artistKey, artist) || artist.librarySectionKey.empty())
        return rows;

    std::set<std::string> seen;
    for (const auto& r : rows) seen.insert(r.id);
    std::vector<BrowseRow> typed;
    for (const auto& cat : PlexClient::artistReleaseCategories()) {
        for (const char* filter : cat.filters) {
            std::vector<MediaItem> items;
            if (!client.fetchArtistAlbumsByFilter(artist.librarySectionKey, artistKey, filter,
                                                  items))
                continue;
            for (const auto& it : items) {
                const std::string id = "album/" + it.ratingKey;
                if (!seen.insert(id).second) continue;
                BrowseRow r;
                r.id       = id;
                r.title    = it.title;
                r.subtitle = it.year > 0 ? std::string(cat.label) + " \xC2\xB7 " +
                                               std::to_string(it.year)
                                         : std::string(cat.label);
                r.iconUri  = artUri(client, it.thumb);
                r.flags    = FLAG_BROWSABLE | FLAG_PLAYABLE;
                typed.push_back(std::move(r));
            }
        }
    }
    // /children can hold the typed releases too: shown once, with their kind.
    std::set<std::string> typedIds;
    for (const auto& r : typed) typedIds.insert(r.id);
    std::vector<BrowseRow> out;
    for (auto& r : rows)
        if (!typedIds.count(r.id)) out.push_back(std::move(r));
    for (auto& r : typed) out.push_back(std::move(r));
    return out;
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
        // Open it to pick a track, or play the whole playlist straight from the list — same reasoning as albums above.
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
        return loadArtist(client, suffixAfter(parentId, "artist/"));
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
// Requests to play from outside the app (Android Auto, the Assistant): each
// takes the next number, and only the newest is carried out. With the app
// closed, every tap and spoken request waits for it to start, and all of them
// then started at once, each in a player of its own, fighting over the one
// player underneath: five in 1.3 s, and no music (log 67c6ed49).
std::atomic<uint64_t> g_playRequest{0};

bool superseded(uint64_t request, const std::string& what) {
    if (request == g_playRequest.load()) return false;
    brls::Logger::info("MediaBrowser: {} passed over: a newer request came", what);
    return true;
}

std::string lowerCase(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Prefix on a media id from a voice request that asked for shuffle.
const char* const kShufflePrefix = "shuffle:";

// All of an artist's tracks, across their albums and other releases.
std::vector<MediaItem> fetchArtistTracks(PlexClient& client, const std::string& sectionKey,
                                         const std::string& artistKey) {
    std::vector<MediaItem> tracks;
    MediaItem artist;
    if (!client.fetchMediaDetails(artistKey, artist)) return tracks;

    auto byArtist = [&](const MediaItem& t) {
        return t.type == "track" &&
               (t.grandparentRatingKey == artistKey ||
                lowerCase(t.grandparentTitle) == lowerCase(artist.title));
    };

    // One request for the lot. The results are checked against the artist, so
    // a server that ignores the filter cannot hand back the whole library.
    if (!sectionKey.empty()) {
        std::vector<MediaItem> all;
        if (client.fetchLibraryContent(sectionKey, all, 10, 500, 0, nullptr,
                                       "&artist.id=" + artistKey)) {
            for (auto& t : all)
                if (byArtist(t)) tracks.push_back(std::move(t));
        }
        if (!tracks.empty()) return tracks;
    }

    // Otherwise album by album.
    std::vector<MediaItem> albums;
    if (!client.fetchChildren(artistKey, albums)) return tracks;
    for (size_t i = 0; i < albums.size() && i < 30; i++) {
        std::vector<MediaItem> albumTracks;
        if (!client.fetchChildren(albums[i].ratingKey, albumTracks)) continue;
        for (auto& t : albumTracks)
            if (t.type == "track") tracks.push_back(std::move(t));
    }
    return tracks;
}

// A random mix from the first music library that is not hidden.
std::vector<MediaItem> fetchLibraryMix(PlexClient& client) {
    std::vector<MediaItem> tracks;
    std::vector<LibrarySection> sections;
    if (!client.fetchLibrarySections(sections)) return tracks;
    for (const auto& s : sections) {
        if (s.type != "artist" || isHiddenLibrary(s.key)) continue;
        std::vector<MediaItem> items;
        if (client.fetchLibraryContent(s.key, items, 10, 100, 0, nullptr, "&sort=random")) {
            for (auto& t : items)
                if (t.type == "track") tracks.push_back(std::move(t));
        }
        if (!tracks.empty()) break;
    }
    return tracks;
}

void playMediaId(const std::string& requestedId, uint64_t request) {
    if (!ensureClientReady()) return;
    if (superseded(request, requestedId)) return;

    const bool shuffle = startsWith(requestedId, kShufflePrefix);
    const std::string mediaId = shuffle ? suffixAfter(requestedId, kShufflePrefix) : requestedId;

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
    } else if (startsWith(mediaId, "artistsongs/")) {
        // artistsongs/<section>/<artist>: everything by an artist, shuffled.
        const std::string rest = suffixAfter(mediaId, "artistsongs/");
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return;
        tracks = fetchArtistTracks(client, rest.substr(0, slash), rest.substr(slash + 1));
    } else if (mediaId == "librarymix") {
        tracks = fetchLibraryMix(client);
    }

    if (tracks.empty()) {
        brls::Logger::warning("MediaBrowser: nothing playable for {}", mediaId);
        return;
    }

    // A shuffle request mixes what is about to play. A single song stays
    // where it is: there is nothing to mix before it starts.
    if ((shuffle || startsWith(mediaId, "artistsongs/")) && !singlePick) {
        std::shuffle(tracks.begin(), tracks.end(), std::mt19937(std::random_device{}()));
        startIndex = 0;
    }
    brls::Logger::info("MediaBrowser: {} -> {} track(s){}", mediaId, tracks.size(),
                       shuffle ? ", shuffled" : "");

    brls::sync([tracks, startIndex, singlePick, request, mediaId]() {
        if (superseded(request, mediaId)) return;
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
            // singlePick doubles as "startIndex is a choice": asking a head unit
            // for a whole album should let shuffle pick where it opens, rather
            // than pinning whatever happens to be first.
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(tracks, startIndex, singlePick));
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

// Words of three letters or more, lowercased, punctuation dropped.
std::vector<std::string> wordsOf(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + " ") {
        if (std::isalnum((unsigned char)c)) {
            cur += (char)std::tolower((unsigned char)c);
        } else {
            if (cur.size() >= 3) out.push_back(cur);
            cur.clear();
        }
    }
    return out;
}

std::string trimSpaces(const std::string& s) {
    size_t a = s.find_first_not_of(" \t,.!?");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t,.!?");
    return s.substr(a, b - a + 1);
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Words that stand for "some music" rather than a title: "songs by Green Day",
// "play some music".
bool isGenericMusic(const std::string& lowered) {
    static const std::set<std::string> kGeneric = {
        "song", "songs", "music", "something", "anything", "stuff", "tracks", "track",
        "everything", "all", "all songs", "all music", "all the songs", "all the music",
        "some music", "some songs", "a song", "a few songs", "the songs", "the music",
        "my music", "my songs", "my library", "random", "random songs", "random music",
        "a mix", "a random mix"};
    return kGeneric.count(trimSpaces(lowered)) > 0;
}

// What a spoken request asked for, once the filler words are off.
struct SpokenQuery {
    std::string text;      // what to search for (lowercased)
    std::string plain;     // as said, less "shuffle" and the app's name
    std::string kind;      // "track", "album", "playlist", "artist" or "" for any
    bool shuffle = false;
};

SpokenQuery parseSpokenQuery(const std::string& query) {
    SpokenQuery q;
    std::string t = trimSpaces(lowerCase(query));

    auto dropFront = [&](const std::string& w) {
        if (startsWith(t, w)) { t = trimSpaces(t.substr(w.size())); return true; }
        return false;
    };
    auto dropBack = [&](const std::string& w) {
        if (endsWith(t, w)) { t = trimSpaces(t.substr(0, t.size() - w.size())); return true; }
        return false;
    };

    for (const char* app : {" on vitaplex", " on vita plex", " on vitastream", " on vita stream"})
        dropBack(app);

    dropFront("play ");  // the Assistant usually takes it off, not always
    if (dropFront("shuffle ")) q.shuffle = true;
    for (const char* w : {" on shuffle", " in shuffle", " shuffled", " on random", " randomly"})
        if (dropBack(w)) q.shuffle = true;
    if (t == "shuffle") { q.shuffle = true; t.clear(); }
    q.plain = t;

    // "the album X", "album X", "X album" and the like.
    // Titles can hold these words too ("Song 2", "Band of Horses"), which is
    // why the phrase as said is searched first.
    static const std::pair<const char*, const char*> kKinds[] = {
        {"song", "track"}, {"track", "track"}, {"album", "album"},
        {"playlist", "playlist"}, {"artist", "artist"}, {"band", "artist"}};
    for (const auto& k : kKinds) {
        const std::string w = k.first;
        const bool atBack = w == "song" || w == "album" || w == "playlist";
        if (dropFront("the " + w + " ") || dropFront(w + " ") ||
            (atBack && dropBack(" " + w))) {
            q.kind = k.second;
            break;
        }
    }
    // "my road trip playlist": "my" goes only with a playlist, so "My Way"
    // stays as it is.
    if (q.kind == "playlist") dropFront("my ");

    q.text = t;
    return q;
}

// Spellings to try when a search finds nothing: without a leading "the",
// with or without a trailing "s", and spelled-out letters joined ("ac dc").
std::vector<std::string> nearSpellings(const std::string& term) {
    std::vector<std::string> out;
    auto add = [&](const std::string& v) {
        const std::string t = trimSpaces(v);
        if (!t.empty() && t != term &&
            std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    };
    if (startsWith(term, "the ")) add(term.substr(4));
    if (endsWith(term, "s")) add(term.substr(0, term.size() - 1));
    else add(term + "s");

    // "a c d c" or "ac dc" -> "acdc".
    std::string joined;
    bool allShort = true;
    int parts = 0;
    for (const auto& w : [&] {
             std::vector<std::string> ws;
             std::string cur;
             for (char c : term + " ") {
                 if (c == ' ') { if (!cur.empty()) ws.push_back(cur); cur.clear(); }
                 else cur += c;
             }
             return ws;
         }()) {
        parts++;
        if (w.size() > 2) allShort = false;
        joined += w;
    }
    if (allShort && parts > 1) add(joined);

    // Punctuation dropped: "don't stop" -> "dont stop".
    std::string plain;
    for (char c : term)
        if (std::isalnum((unsigned char)c) || c == ' ') plain += c;
    add(plain);
    return out;
}

// How many of the spoken words appear in the item's artist or album name.
// Speech recognition mangles names ("Daniel rustige"), so this only ranks
// results; it never rules one out.
int hintWordsMatched(const MediaItem& m, const std::string& hint) {
    std::string owner;
    if (m.type == "track")       owner = m.grandparentTitle + " " + m.parentTitle;
    else if (m.type == "album")  owner = m.parentTitle;
    else if (m.type == "artist") owner = m.title;
    const std::string have = lowerCase(owner);
    int n = 0;
    for (const auto& w : wordsOf(hint))
        if (have.find(w) != std::string::npos) n++;
    return n;
}

std::string mediaIdFor(const MediaItem& m) {
    if (m.type == "album")    return "album/" + m.ratingKey;
    if (m.type == "playlist") return "playlist/" + m.ratingKey;
    if (m.type == "track")    return "track/" + m.ratingKey;
    if (m.type == "artist")   return "artistsongs/" + m.librarySectionKey + "/" + m.ratingKey;
    return {};
}

// Playlists by name. Plex's search does not always list playlists, and
// "my road trip playlist" is said by name.
std::string findPlaylist(PlexClient& client, const std::string& name) {
    std::vector<Playlist> playlists;
    if (!client.fetchMusicPlaylists(playlists)) return {};
    const std::string want = lowerCase(name);
    const Playlist* best = nullptr;
    int bestScore = 0;
    for (const auto& p : playlists) {
        const std::string t = lowerCase(p.title);
        int sc = 0;
        if (t == want) sc = 3;
        else if (startsWith(t, want)) sc = 2;
        else if (t.find(want) != std::string::npos || want.find(t) != std::string::npos) sc = 1;
        if (sc > bestScore) { bestScore = sc; best = &p; }
    }
    return best ? "playlist/" + best->ratingKey : std::string();
}

// One search against the library.
//  - With no hint, the broadest music hit wins: an artist (all their songs)
//    or an album is a whole listening session, a track three minutes.
//  - With a hint, from "<title> by <artist>" or "<title> from <album>", the
//    title is what was asked for, so the item whose title matches wins and
//    the hint breaks ties.
//  - kind, when said ("the album X"), keeps only that type. "*" is any type,
//    but the title has to match.
std::string resolveOneSearch(PlexClient& client, const std::string& query,
                             const std::string& hint, const std::string& kind) {
    std::vector<MediaItem> results;
    if (!client.search(query, results) || results.empty()) return {};

    const std::string want = lowerCase(query);

    // 3 exact, 2 starts with, 1 contains either way, 0 none.
    auto exactness = [&](const MediaItem& m) {
        const std::string t = lowerCase(m.title);
        if (t == want) return 3;
        if (startsWith(t, want)) return 2;
        if (t.find(want) != std::string::npos ||
            (!t.empty() && want.find(t) != std::string::npos)) return 1;
        return 0;
    };

    // Higher is better; -1 means not a candidate.
    auto score = [&](const MediaItem& m) -> int {
        int base;
        if (m.type == "artist")        base = 40;
        else if (m.type == "album")    base = 30;
        else if (m.type == "playlist") base = 20;
        else if (m.type == "track")    base = 10;
        else                           return -1;   // movies, shows: not music
        if (!kind.empty() && kind != "*" && m.type != kind) return -1;
        const int ex = exactness(m);
        if (hint.empty()) {
            // A kind was said, so the title has to match something.
            if (!kind.empty() && ex == 0) return -1;
            return base + ex;
        }

        // The title has to match something, and an artist named like the
        // title is not what was asked for.
        if (ex == 0 || m.type == "artist") return -1;
        return ex * 100 + hintWordsMatched(m, hint) * 10 + base / 10;
    };

    const MediaItem* best = nullptr;
    int bestScore = 0;
    for (const auto& m : results) {
        if (isHiddenLibrary(m.librarySectionKey)) continue;
        const int s = score(m);
        if (s > bestScore) { bestScore = s; best = &m; }
    }
    return best ? mediaIdFor(*best) : std::string();
}

// resolveOneSearch, then the near spellings of the term.
std::string searchWithSpellings(PlexClient& client, const std::string& term,
                                const std::string& hint, const std::string& kind) {
    std::string id = resolveOneSearch(client, term, hint, kind);
    if (!id.empty()) return id;
    for (const auto& v : nearSpellings(term)) {
        brls::Logger::info("MediaBrowser: trying \"{}\"", v);
        id = resolveOneSearch(client, v, hint, kind);
        if (!id.empty()) return id;
    }
    return {};
}

// An artist by name, for "songs by <artist>".
std::string findArtistSongs(PlexClient& client, const std::string& artist) {
    return searchWithSpellings(client, artist, "", "artist");
}

// Turn a spoken query into something playable.
//
// Assistant hands over free text. Plex's search matches the whole phrase, so
// anything extra in it ("the album", "by <artist>", a misheard name) finds
// nothing. In order:
//  1. Filler off: "shuffle", "the song/album/playlist", "my ... playlist".
//     The type said is kept, and so is shuffle.
//  2. Just "music", "something": a random mix of the library.
//  3. "<generic> by <artist>" ("songs by Green Day"): all of the artist's songs.
//  4. The whole phrase, as said and then as cleaned up.
//  5. "<title> by <artist>" / "<title> from <album>": the title, ranked by
//     how much of the rest matches; then the part after on its own.
//  6. Each of those with near spellings.
//
// Returns a media id for playMediaId(), with kShufflePrefix when shuffle was
// asked for, or empty when nothing in the music library matched.
std::string resolveSearchQuery(PlexClient& client, const std::string& query) {
    const SpokenQuery q = parseSpokenQuery(query);
    const std::string prefix = q.shuffle ? kShufflePrefix : "";
    auto done = [&](const std::string& id) { return id.empty() ? id : prefix + id; };

    if (q.text.empty() || isGenericMusic(q.text)) {
        brls::Logger::info("MediaBrowser: \"{}\" -> a mix of the library", query);
        return kShufflePrefix + std::string("librarymix");
    }

    // Split "<title> by <artist>" / "<title> from <album>" at the last one.
    std::string title, hint;
    bool byArtist = false;
    for (const char* sep : {" by ", " from "}) {
        const size_t at = q.text.rfind(sep);
        if (at == std::string::npos) continue;
        if (!title.empty() && at < title.size()) continue;
        title    = trimSpaces(q.text.substr(0, at));
        hint     = trimSpaces(q.text.substr(at + std::strlen(sep)));
        byArtist = std::string(sep) == " by ";
    }

    // "songs by Green Day": everything by the artist.
    if (byArtist && !hint.empty() && (title.empty() || isGenericMusic(title))) {
        brls::Logger::info("MediaBrowser: all songs by \"{}\"", hint);
        const std::string id = findArtistSongs(client, hint);
        if (!id.empty()) return done(id);
    }

    // The whole phrase, first as said (titles do start with "My" or "Song"),
    // then cleaned up, with the type said. As said, only a title match
    // counts: Plex's search can return near misses for a long phrase.
    std::string id;
    if (q.plain != q.text) id = resolveOneSearch(client, q.plain, "", "*");
    if (id.empty() && q.kind == "playlist") id = findPlaylist(client, q.text);
    if (id.empty()) id = resolveOneSearch(client, q.text, "", q.kind);
    if (!id.empty()) return done(id);

    if (!title.empty() && !wordsOf(title).empty()) {
        brls::Logger::info("MediaBrowser: trying the title \"{}\" ({} \"{}\")", title,
                           byArtist ? "artist" : "album", hint);
        id = searchWithSpellings(client, title, hint, q.kind);
        if (!id.empty()) return done(id);
    }
    if (!hint.empty() && !wordsOf(hint).empty()) {
        brls::Logger::info("MediaBrowser: trying the {} \"{}\"",
                           byArtist ? "artist" : "album", hint);
        id = searchWithSpellings(client, hint, "", byArtist ? "artist" : "album");
        if (!id.empty()) return done(id);
    }

    if (title.empty()) {
        for (const auto& v : nearSpellings(q.text)) {
            brls::Logger::info("MediaBrowser: trying \"{}\"", v);
            id = resolveOneSearch(client, v, "", q.kind);
            if (!id.empty()) return done(id);
        }
    }
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
            row[0].title = "Sign in to " APP_DISPLAY_NAME " to browse your library";
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
    const uint64_t request = ++g_playRequest;
    asyncRun([mediaId, request]() { playMediaId(mediaId, request); });
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
    const uint64_t request = ++g_playRequest;

    asyncRun([query, request]() {
        if (!ensureClientReady()) return;
        if (superseded(request, "\"" + query + "\"")) return;

        // "Play music": resume what is queued, or with nothing queued, a
        // random mix of the library.
        if (query.empty()) {
            brls::sync([request]() {
                if (!MusicQueue::getInstance().isEmpty()) {
                    MusicController::getInstance().playPause(true);
                    return;
                }
                asyncRun([request]() {
                    playMediaId(std::string(kShufflePrefix) + "librarymix", request);
                });
            });
            return;
        }

        const std::string mediaId = resolveSearchQuery(PlexClient::getInstance(), query);
        if (mediaId.empty()) {
            brls::Logger::warning("MediaBrowser: no music match for \"{}\"", query);
            return;
        }
        playMediaId(mediaId, request);
    });
}

/**
 * Android TV global search: the system's search app queries
 * SearchSuggestionProvider, which calls straight through to here.
 *
 * Synchronous on purpose — the provider is already on a binder thread and the
 * system blocks on the answer, so hopping to a worker would only add latency.
 * Rows come back flat, four strings each: rating key, title, subtitle, art URI.
 */
JNIEXPORT jobjectArray JNICALL
Java_org_VitaPlex_app_SearchSuggestionProvider_nativeSearchSuggestions(JNIEnv* env, jclass,
                                                                       jstring jQuery) {
    const char* raw = jQuery ? env->GetStringUTFChars(jQuery, nullptr) : nullptr;
    std::string query = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(jQuery, raw);

    jclass strCls = env->FindClass("java/lang/String");
    if (query.empty() || !ensureClientReady()) return env->NewObjectArray(0, strCls, nullptr);

    std::vector<MediaItem> results;
    if (!PlexClient::getInstance().search(query, results))
        return env->NewObjectArray(0, strCls, nullptr);

    // The search UI shows a handful of cards; sending the whole result set
    // would be rows nobody scrolls to, over a binder transaction.
    constexpr size_t kMaxRows = 20;
    PlexClient& client = PlexClient::getInstance();

    std::vector<std::string> flat;
    flat.reserve(kMaxRows * 4);
    for (const auto& m : results) {
        if (flat.size() >= kMaxRows * 4) break;
        if (m.ratingKey.empty()) continue;
        if (isHiddenLibrary(m.librarySectionKey)) continue;
        // People and other non-playable hits have nothing to open.
        if (m.type == "person" || m.type == "tag") continue;

        std::string subtitle;
        if (m.mediaType == MediaType::EPISODE) subtitle = m.grandparentTitle;
        else if (m.year > 0)                   subtitle = std::to_string(m.year);
        else                                   subtitle = m.type;

        flat.push_back(m.ratingKey);
        flat.push_back(m.title);
        flat.push_back(subtitle);
        flat.push_back(artUri(client, !m.thumb.empty() ? m.thumb : m.grandparentThumb));
    }

    jobjectArray arr = env->NewObjectArray((jsize)flat.size(), strCls, nullptr);
    if (!arr) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return nullptr;
    }
    for (jsize i = 0; i < (jsize)flat.size(); i++) {
        jstring s = env->NewStringUTF(flat[(size_t)i].c_str());
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

}  // extern "C"

#endif  // __ANDROID__
