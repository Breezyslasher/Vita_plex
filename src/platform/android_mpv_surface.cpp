/**
 * VitaPlex — Android direct-surface JNI bridge.
 *
 * Connects a Java SurfaceView's Surface to libmpv so mpv can render
 * directly to the display via vo=gpu (no FBO / NanoVG composite). See
 * docs/android-direct-surface-playback.md for the why.
 *
 * Stage 1: these natives exist and are correct, but nothing calls them
 * yet — the Java MpvSurface view (Stage 2) and the VO-path flip
 * (Stage 4) come later. Building this now is inert.
 *
 * The Java side lives in package org.VitaPlex.app, class MpvSurface, so
 * the JNI symbol names below mangle to Java_org_VitaPlex_app_MpvSurface_*.
 * Keep them in sync with the Java class if it's renamed/moved.
 */

#ifdef __ANDROID__

#include <jni.h>
#include <cstdint>

#include <borealis.hpp>
#include "player/mpv_player.hpp"
#include "platform/android_mpv_surface.hpp"

// ffmpeg's MediaCodec decoder and mpv's ANativeWindow_fromSurface both
// need the JavaVM registered. SDL owns JNI_OnLoad in this .so, so we
// can't define our own — instead grab the VM lazily from the first
// native call's JNIEnv and hand it to ffmpeg once.
extern "C" {
#include <libavcodec/jni.h>
}

namespace {
// Global ref to the Java Surface. mpv reads its address as "wid" and
// calls ANativeWindow_fromSurface() on it, so it must outlive the VO.
// The JNI layer owns this ref; MpvPlayer never frees it.
jobject g_surfaceRef = nullptr;
bool    g_javaVmRegistered = false;

void ensureJavaVm(JNIEnv* env) {
    if (g_javaVmRegistered) return;
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) == 0 && vm) {
        av_jni_set_java_vm(vm, nullptr);
        g_javaVmRegistered = true;
        brls::Logger::info("android_mpv_surface: registered JavaVM with ffmpeg");
    } else {
        brls::Logger::error("android_mpv_surface: GetJavaVM failed");
    }
}
} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_org_VitaPlex_app_MpvSurface_nativeAttachSurface(JNIEnv* env, jclass, jobject surface) {
    ensureJavaVm(env);

    // Replace any stale ref (surfaceCreated without a matching destroy).
    if (g_surfaceRef) {
        env->DeleteGlobalRef(g_surfaceRef);
        g_surfaceRef = nullptr;
    }
    g_surfaceRef = env->NewGlobalRef(surface);
    if (!g_surfaceRef) {
        brls::Logger::error("android_mpv_surface: NewGlobalRef(surface) failed");
        return;
    }

    int64_t wid = reinterpret_cast<intptr_t>(g_surfaceRef);
    vitaplex::MpvPlayer::getInstance().attachAndroidSurface(wid);
}

JNIEXPORT void JNICALL
Java_org_VitaPlex_app_MpvSurface_nativeDetachSurface(JNIEnv* env, jclass) {
    // Tear the VO down first, then release the ref mpv was reading.
    vitaplex::MpvPlayer::getInstance().detachAndroidSurface();
    if (g_surfaceRef) {
        env->DeleteGlobalRef(g_surfaceRef);
        g_surfaceRef = nullptr;
    }
}

JNIEXPORT void JNICALL
Java_org_VitaPlex_app_MpvSurface_nativeSetSurfaceSize(JNIEnv*, jclass, jint w, jint h) {
    vitaplex::MpvPlayer::getInstance().setAndroidSurfaceSize((int)w, (int)h);
}

} // extern "C"

namespace vitaplex {
namespace android_mpv_surface {

void rebindIfReady() {
    // Activity layout creates MpvSurface before MpvPlayer ever exists,
    // so by the time playback starts the Java side has already fired
    // surfaceCreated → nativeAttachSurface → tried to attach against a
    // null m_mpv (warning logged, no-op). The Surface global ref is
    // still stashed in g_surfaceRef. MpvPlayer::init calls this once
    // mpv is alive so we can complete the attach now: same wid that
    // was logged earlier, this time the mpv set_option lands.
    if (!g_surfaceRef) {
        brls::Logger::debug("android_mpv_surface: rebindIfReady — no surface stashed");
        return;
    }
    int64_t wid = reinterpret_cast<intptr_t>(g_surfaceRef);
    brls::Logger::info("android_mpv_surface: rebindIfReady — re-attaching stashed surface");
    MpvPlayer::getInstance().attachAndroidSurface(wid);
}

} // namespace android_mpv_surface
} // namespace vitaplex

#endif // __ANDROID__
