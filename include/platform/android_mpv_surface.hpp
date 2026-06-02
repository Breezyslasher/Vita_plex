/**
 * VitaPlex — Android direct-surface internals exposed to C++ callers.
 *
 * Stage 4 of docs/android-direct-surface-playback.md needs a way for
 * MpvPlayer::init(), once it has a live mpv handle, to ask the JNI
 * layer "do you already have a Surface stashed from a previous Java
 * surfaceCreated callback?" If yes the layer re-applies the saved
 * wid right then, so mpv brings the VO up on the existing surface.
 *
 * The JNI bridge itself lives in src/platform/android_mpv_surface.cpp.
 * This header just exposes the small surface (no JNI types) that
 * non-JNI code needs to coordinate with it.
 */

#pragma once

#ifdef __ANDROID__

namespace vitaplex {
namespace android_mpv_surface {

// If MpvSurface's Java surfaceCreated has already fired and the JNI
// layer is holding a global ref to that Surface, re-apply it to
// MpvPlayer (which will set the mpv "wid" option and bring the VO
// up via force-window=yes). No-op if no surface is stashed.
//
// Safe to call from the player-init thread; the JNI layer's only
// other writers (Java surfaceCreated / surfaceDestroyed) run on the
// activity UI thread, and in practice player init happens well after
// the activity layout finishes so the read is stable.
void rebindIfReady();

} // namespace android_mpv_surface
} // namespace vitaplex

#endif // __ANDROID__
