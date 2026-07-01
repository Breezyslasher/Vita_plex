/**
 * VitaPlex - OS "Now Playing" media session bridge (implementation)
 *
 * See include/utils/now_playing.hpp. The Android path talks to the Java helper
 * org.VitaPlex.app.MediaNotification over JNI, mirroring the pattern in
 * src/utils/pip.cpp. All other platforms get no-op update()/clear().
 */

#include "utils/now_playing.hpp"

#include <borealis.hpp>
#include <mutex>

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
#endif

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string>
#endif

namespace vitaplex {
namespace nowplaying {

namespace {
std::mutex g_mutex;
std::function<void(Transport)> g_onTransport;
std::function<void(int64_t)> g_onSeek;
std::function<void(RepeatMode)> g_onSetRepeat;
std::function<void(bool)> g_onSetShuffle;

} // namespace

void setHandler(std::function<void(Transport)> onTransport,
                std::function<void(int64_t)> onSeekMs,
                std::function<void(RepeatMode)> onSetRepeat,
                std::function<void(bool)> onSetShuffle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_onTransport = std::move(onTransport);
    g_onSeek = std::move(onSeekMs);
    g_onSetRepeat = std::move(onSetRepeat);
    g_onSetShuffle = std::move(onSetShuffle);
}

void clearHandler() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_onTransport = nullptr;
    g_onSeek = nullptr;
    g_onSetRepeat = nullptr;
    g_onSetShuffle = nullptr;
}

void dispatchTransport(Transport t) {
    std::function<void(Transport)> fn;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fn = g_onTransport;
    }
    if (fn) fn(t);
}

void dispatchSeek(int64_t positionMs) {
    std::function<void(int64_t)> fn;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fn = g_onSeek;
    }
    if (fn) fn(positionMs);
}

void dispatchSetRepeat(RepeatMode mode) {
    std::function<void(RepeatMode)> fn;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fn = g_onSetRepeat;
    }
    if (fn) fn(mode);
}

void dispatchSetShuffle(bool on) {
    std::function<void(bool)> fn;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fn = g_onSetShuffle;
    }
    if (fn) fn(on);
}

#ifdef __ANDROID__

void update(const Info& info) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;

    jclass cls = env->FindClass("org/VitaPlex/app/MediaNotification");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(
        cls, "update",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJZZZIZZ)V");
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return;
    }

    jstring jTitle = env->NewStringUTF(info.title.c_str());
    jstring jArtist = env->NewStringUTF(info.artist.c_str());
    jstring jAlbum = env->NewStringUTF(info.album.c_str());
    jstring jArt = env->NewStringUTF(info.artUrl.c_str());

    // Repeat mode as an int the Java side maps to its drawables: 0 off, 1 all, 2 one.
    jint jRepeat = info.repeat == RepeatMode::All ? 1 : (info.repeat == RepeatMode::One ? 2 : 0);
    // Show the repeat/shuffle actions only when the publisher wants them (music,
    // not video).
    jboolean jShowModes = (info.showRepeat || info.showShuffle) ? JNI_TRUE : JNI_FALSE;

    env->CallStaticVoidMethod(cls, mid, jTitle, jArtist, jAlbum, jArt,
                              (jlong)info.durationMs, (jlong)info.positionMs,
                              (jboolean)info.playing, (jboolean)info.hasNext,
                              (jboolean)info.hasPrev, jRepeat,
                              (jboolean)info.shuffle, jShowModes);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jArtist);
    env->DeleteLocalRef(jAlbum);
    env->DeleteLocalRef(jArt);
    env->DeleteLocalRef(cls);
}

void clear() {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;

    jclass cls = env->FindClass("org/VitaPlex/app/MediaNotification");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(cls, "clear", "()V");
    if (mid) {
        env->CallStaticVoidMethod(cls, mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
}

#elif defined(VITAPLEX_MPRIS)  // ---- Linux desktop: MPRIS over D-Bus ----

// Implemented in now_playing_mpris.cpp (keeps the libdbus dependency isolated).
namespace detail {
void mprisUpdate(const Info& info);
void mprisClear();
}
void update(const Info& info) { detail::mprisUpdate(info); }
void clear() { detail::mprisClear(); }

#elif defined(VITAPLEX_SMTC)  // ---- Windows desktop: System Media Transport Controls ----

// Implemented in now_playing_smtc.cpp (keeps the WinRT/WRL dependency isolated).
namespace detail {
void smtcUpdate(const Info& info);
void smtcClear();
}
void update(const Info& info) { detail::smtcUpdate(info); }
void clear() { detail::smtcClear(); }

#elif defined(__vita__)  // ---- PS Vita: background-music helper control files ----

// The bundled background helper (eboot2.bin) plays audio while VitaPlex is in
// LiveArea. It's a separate process that can't see our memory, so we hand it
// state through two tiny files under ux0:data/VitaPlex (read by bgapp.c):
//   bgm_status : "<playing>\n<title>\n" — refreshed here on every state change.
//   bgm_tick   : a counter bumped 4x/sec by a RepeatingTimer while our main loop
//                runs; it freezes when the app is suspended, which is how the
//                helper detects backgrounding without us acting at suspend time.
namespace detail {

static void vitaWriteFile(const char* path, const std::string& data) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, data.data(), data.size());
    sceIoClose(fd);
}

static brls::RepeatingTimer g_bgmTick;
static bool g_bgmTickStarted = false;
static uint32_t g_bgmTickCount = 0;

static void vitaEnsureTick() {
    if (g_bgmTickStarted) return;
    g_bgmTickStarted = true;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaPlex", 0777);
    // 4 Hz liveness heartbeat, driven by the borealis main loop — so it freezes
    // the instant VitaPlex is suspended, which is exactly what the helper's
    // watchdog watches for.
    g_bgmTick.setCallback([]() {
        vitaWriteFile("ux0:data/VitaPlex/bgm_tick", std::to_string(++g_bgmTickCount));
    });
    g_bgmTick.start(250);
}

} // namespace detail

void update(const Info& info) {
    detail::vitaEnsureTick();
    // Line 1 playing, 2 title, 3 local file path (downloaded track, else empty),
    // 4 position ms. The background helper reads line 1 to decide whether to take
    // over and line 3 to know which file to decode. Keep this format in sync with
    // read_status() in app/platform/psv/bgapp/bgapp.c.
    std::string s = (info.playing ? "1\n" : "0\n")
                  + info.title + "\n"
                  + info.localPath + "\n"
                  + std::to_string(info.positionMs) + "\n";
    detail::vitaWriteFile("ux0:data/VitaPlex/bgm_status", s);
}

void clear() {
    detail::vitaWriteFile("ux0:data/VitaPlex/bgm_status", "0\n\n\n0\n");
}

#else  // ---- other platforms: no OS media session ----

void update(const Info&) {}
void clear() {}

#endif

} // namespace nowplaying
} // namespace vitaplex

#ifdef __ANDROID__
// Java -> native: a transport button was pressed in the OS media controls.
// Marshals onto the main thread (brls::sync) before touching playback, exactly
// like the PiP action trampoline in pip.cpp.
extern "C" JNIEXPORT void JNICALL
Java_org_VitaPlex_app_MediaNotification_nativeMediaAction(JNIEnv*, jclass, jint code) {
    // Codes shared with MediaNotification.java (keep in sync).
    brls::sync([code]() {
        using vitaplex::nowplaying::Transport;
        Transport t;
        switch ((int)code) {
            case 2:  t = Transport::Play;          break;
            case 3:  t = Transport::Pause;         break;
            case 4:  t = Transport::Next;          break;
            case 5:  t = Transport::Previous;      break;
            case 6:  t = Transport::Stop;          break;
            case 7:  t = Transport::CycleRepeat;   break;
            case 8:  t = Transport::ToggleShuffle; break;
            case 1:
            default: t = Transport::Toggle;        break;
        }
        vitaplex::nowplaying::dispatchTransport(t);
    });
}

// Java -> native: an absolute seek (ms) was requested from the OS controls.
extern "C" JNIEXPORT void JNICALL
Java_org_VitaPlex_app_MediaNotification_nativeMediaSeek(JNIEnv*, jclass, jlong positionMs) {
    brls::sync([positionMs]() {
        vitaplex::nowplaying::dispatchSeek((int64_t)positionMs);
    });
}
#endif
