/**
 * VitaPlex - Picture-in-Picture helper
 *
 * See include/utils/pip.h for design.
 */

#include "utils/pip.h"

#include <borealis.hpp>

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
#include "player/mpv_player.hpp"
#endif

// Desktop PiP requires SDL2 (borealis's GLFW backend does not expose a
// window handle we can manipulate). __SDL2__ is defined by borealis when
// building the SDL2 backend.
#if defined(__SDL2__) && !defined(__vita__) && !defined(__SWITCH__) && \
    !defined(__PS4__) && !defined(__ANDROID__)
#define VITAPLEX_PIP_DESKTOP 1
#include <SDL2/SDL.h>
#include <borealis/platforms/sdl/sdl_video.hpp>
#endif

namespace vitaplex {
namespace pip {

namespace {

bool g_pipActive = false;

#ifdef VITAPLEX_PIP_DESKTOP
// Saved window state so we can restore on exit.
struct DesktopWindowState {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool bordered = true;
    bool fullscreen = false;
    bool valid = false;
};
DesktopWindowState g_savedState;

SDL_Window* getDesktopWindow() {
    auto* videoCtx = dynamic_cast<brls::SDLVideoContext*>(
        brls::Application::getPlatform()->getVideoContext());
    if (!videoCtx) return nullptr;
    return videoCtx->getSDLWindow();
}
#endif

} // namespace

bool isAvailable() {
#if defined(__ANDROID__) || defined(VITAPLEX_PIP_DESKTOP)
    return true;
#else
    return false;
#endif
}

bool isActive() {
    return g_pipActive;
}

#ifdef __ANDROID__
static bool androidEnterPip(int videoWidth, int videoHeight) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) {
        brls::Logger::error("PiP: SDL_AndroidGetJNIEnv returned null");
        return false;
    }

    jclass activityCls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
    if (!activityCls) {
        brls::Logger::error("PiP: Failed to find VitaPlexActivity class");
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    jmethodID enterMid = env->GetStaticMethodID(activityCls, "enterPiP", "(II)Z");
    if (!enterMid) {
        brls::Logger::error("PiP: Failed to resolve enterPiP static method");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(activityCls);
        return false;
    }

    // Clamp aspect ratio to Android's allowed range [0.4183, 2.39]. Video
    // sometimes reports 0×0 early in playback; fall back to 16:9 so we
    // don't pass an invalid ratio to the system.
    if (videoWidth <= 0 || videoHeight <= 0) {
        videoWidth = 16;
        videoHeight = 9;
    }

    jboolean ok = env->CallStaticBooleanMethod(activityCls, enterMid,
                                               (jint)videoWidth, (jint)videoHeight);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        ok = JNI_FALSE;
    }
    env->DeleteLocalRef(activityCls);
    return ok == JNI_TRUE;
}
#endif

bool enter(int videoWidth, int videoHeight) {
    if (g_pipActive) return true;

#ifdef __ANDROID__
    if (androidEnterPip(videoWidth, videoHeight)) {
        g_pipActive = true;
        return true;
    }
    return false;
#elif defined(VITAPLEX_PIP_DESKTOP)
    SDL_Window* win = getDesktopWindow();
    if (!win) {
        brls::Logger::error("PiP: Could not obtain SDL window");
        return false;
    }

    // Save current state so leave() can restore it.
    g_savedState.fullscreen = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0 ||
                              (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    g_savedState.bordered = (SDL_GetWindowFlags(win) & SDL_WINDOW_BORDERLESS) == 0;

    if (g_savedState.fullscreen) {
        // Leave fullscreen before resizing so the OS treats the window as a
        // normal floating one.
        SDL_SetWindowFullscreen(win, 0);
    }
    SDL_GetWindowPosition(win, &g_savedState.x, &g_savedState.y);
    SDL_GetWindowSize(win, &g_savedState.w, &g_savedState.h);
    g_savedState.valid = true;

    // Compute PiP size: 480px wide, height preserves video aspect ratio
    // (fall back to 16:9 if the video hasn't reported dimensions yet).
    int pipW = 480;
    int pipH = 270;
    if (videoWidth > 0 && videoHeight > 0) {
        pipH = (int)((float)pipW * (float)videoHeight / (float)videoWidth);
    }

    // Position at bottom-right of the display containing the window.
    SDL_Rect displayBounds;
    int displayIndex = SDL_GetWindowDisplayIndex(win);
    if (displayIndex < 0 || SDL_GetDisplayUsableBounds(displayIndex, &displayBounds) != 0) {
        // Fall back to the primary display's bounds.
        if (SDL_GetDisplayUsableBounds(0, &displayBounds) != 0) {
            displayBounds = {0, 0, 1280, 720};
        }
    }
    int margin = 20;
    int pipX = displayBounds.x + displayBounds.w - pipW - margin;
    int pipY = displayBounds.y + displayBounds.h - pipH - margin;

    SDL_SetWindowBordered(win, SDL_FALSE);
    SDL_SetWindowResizable(win, SDL_FALSE);
    SDL_SetWindowSize(win, pipW, pipH);
    SDL_SetWindowPosition(win, pipX, pipY);
#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_SetWindowAlwaysOnTop(win, SDL_TRUE);
#endif
    SDL_RaiseWindow(win);

    g_pipActive = true;
    brls::Logger::info("PiP: Entered ({}x{} at {},{})", pipW, pipH, pipX, pipY);
    return true;
#else
    (void)videoWidth;
    (void)videoHeight;
    return false;
#endif
}

bool leave() {
    if (!g_pipActive) return false;

#ifdef __ANDROID__
    // Android handles PiP lifecycle itself. The app is returned to normal
    // when the user taps the PiP window. Here we just clear our flag.
    g_pipActive = false;
    return true;
#elif defined(VITAPLEX_PIP_DESKTOP)
    SDL_Window* win = getDesktopWindow();
    if (!win) return false;

#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_SetWindowAlwaysOnTop(win, SDL_FALSE);
#endif

    if (g_savedState.valid) {
        SDL_SetWindowBordered(win, g_savedState.bordered ? SDL_TRUE : SDL_FALSE);
        SDL_SetWindowResizable(win, SDL_TRUE);
        SDL_SetWindowSize(win, g_savedState.w, g_savedState.h);
        SDL_SetWindowPosition(win, g_savedState.x, g_savedState.y);
        if (g_savedState.fullscreen) {
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }
    g_savedState = {};

    g_pipActive = false;
    brls::Logger::info("PiP: Exited");
    return true;
#else
    g_pipActive = false;
    return false;
#endif
}

bool toggle(int videoWidth, int videoHeight) {
    if (g_pipActive) {
        return leave();
    }
    return enter(videoWidth, videoHeight);
}

void setVideoPlaybackState(bool playing, int videoWidth, int videoHeight) {
#ifdef __ANDROID__
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;
    jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(cls, "setVideoPlaybackState", "(ZII)V");
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return;
    }
    if (videoWidth <= 0 || videoHeight <= 0) {
        videoWidth = 16;
        videoHeight = 9;
    }
    env->CallStaticVoidMethod(cls, mid,
                              (jboolean)(playing ? JNI_TRUE : JNI_FALSE),
                              (jint)videoWidth, (jint)videoHeight);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
#else
    (void)playing;
    (void)videoWidth;
    (void)videoHeight;
#endif
}

} // namespace pip
} // namespace vitaplex

#ifdef __ANDROID__
// Bridge from Java BroadcastReceiver (VitaPlexActivity.onPipActionReceived)
// to the mpv player. Called on a background binder thread; forward to the
// main thread via brls::sync so we don't touch mpv concurrently with the
// render callback.
//
// Action codes (kept in sync with VitaPlexActivity.ACTION_*):
//   1 = seek back 10s
//   2 = toggle play/pause
//   3 = seek forward 10s
extern "C" JNIEXPORT void JNICALL
Java_org_VitaPlex_app_VitaPlexActivity_nativePipAction(JNIEnv*, jclass, jint code) {
    brls::sync([code]() {
        auto& player = vitaplex::MpvPlayer::getInstance();
        switch (code) {
            case 1:
                player.seekRelative(-10.0);
                break;
            case 2:
                if (player.isPaused()) {
                    player.play();
                } else {
                    player.pause();
                }
                break;
            case 3:
                player.seekRelative(10.0);
                break;
            default:
                break;
        }
    });
}
#endif
