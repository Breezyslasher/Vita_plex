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
#include "activity/player_activity.hpp"
#include "platform/paths.hpp"
#include "utils/async.hpp"

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
            r.flags    = FLAG_BROWSABLE;
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
        r.flags    = FLAG_BROWSABLE;
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
        r.id       = "track/" + t.ratingKey;
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

    brls::Logger::debug("MediaBrowser: loadChildren({}) token={}", parentId, (int)token);

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
        deliverRows((int)token, resolveNode(parentId));
    });
}

JNIEXPORT void JNICALL
Java_org_VitaPlex_app_LibraryBrowserService_nativePlayFromMediaId(JNIEnv* env, jclass,
                                                                  jstring jMediaId) {
    const char* raw = jMediaId ? env->GetStringUTFChars(jMediaId, nullptr) : nullptr;
    std::string mediaId = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(jMediaId, raw);

    brls::Logger::info("MediaBrowser: playFromMediaId({})", mediaId);

    asyncRun([mediaId]() {
        if (!ensureClientReady()) return;

        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> tracks;
        int startIndex = 0;

        if (startsWith(mediaId, "track/")) {
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

        brls::sync([tracks, startIndex]() {
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(tracks, startIndex));
        });
    });
}

}  // extern "C"

#endif  // __ANDROID__
