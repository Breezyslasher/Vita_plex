# Android direct-surface video playback (design)

Status: **design / not yet implemented**
Target: Android + Android TV (arm64-v8a, armeabi-v7a)
Owner: playback
Branch origin: investigation into choppy Android TV playback (Bravia A1, Chromecast with Google TV)

---

## Problem (proven)

On weak Android TV SoCs, video playback drops hundreds of frames at the
**video-output stage** while the decoder keeps up fine. Captured from the
in-app MPV stats overlay during the investigation:

```
Drops: 0 decoder, 856 vo
```

0 decoder drops + hundreds of vo drops = the decode is fine, the render
path can't keep pace. Confirmed against switchfin (also mpv-based) showing
~1000 vo drops on the same hardware, and against the official Plex app
(ExoPlayer / SurfaceView) playing the identical file smoothly.

### Why our render path is the bottleneck

Current Android video path in `src/player/mpv_player.cpp`
(`setupNonVitaRender()` / `initRenderContext()`):

```
mpv decode → vo=libmpv → mpv_render_context renders into a GL FBO
           → FBO's color attachment is a NanoVG texture
           → NanoVG samples that texture and composites it into borealis'
             single GL surface every frame
           → SDL swaps the surface
```

That's **two extra full-frame GPU operations per frame** (mpv's FBO write +
NanoVG's textured-quad composite) plus the upscale to the panel's native
resolution. On an Adreno/Mali-class TV SoC at 4K output that exceeds the
per-frame GPU budget and mpv counts the backpressured frames as vo-dropped.

### Why mpv-android is smooth on the same hardware

mpv-android hands mpv a **dedicated Android `Surface`** and lets `vo=gpu`
render directly to it. From their `app/src/main/jni/render.cpp`:

```c
int64_t wid = reinterpret_cast<intptr_t>(surface);   // Java Surface jobject
mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);
```

mpv's `gpu-context=android` calls `ANativeWindow_fromSurface()` on that
jobject and creates its own EGL surface bound to it. Video goes straight to
the display compositor — **no FBO, no NanoVG, no per-frame composite.** The
GPU does one job: scan out decoded frames.

---

## Why the cheap fixes don't apply

Ruled out during investigation, in order:

| Tried | Result |
|---|---|
| `hwdec=mediacodec` / `mediacodec-copy` | HW decode engaged on CCwGTV (`HW: mediacodec-copy`); still choppy. Not the decoder. |
| Drop aggressive overrides (framedrop / video-sync / latency-hacks) | No change. |
| FBO render resolution 720p / 480p | vo drops continued — the composite itself is the cost, not the FBO size. |
| Direct play (skip Plex HLS transcode) | Removed per-segment + HLS-reassembly tax (worth keeping) but vo drops persisted. |
| `vo=gpu` while sharing SDL's window | Impossible: one `ANativeWindow` is single-producer for EGL; SDL already owns its `EGLSurface`. mpv can't `eglCreateWindowSurface` on the same window. |

The last row is the crux: **mpv needs its own surface.** Everything short of
that leaves the composite in the pipeline.

---

## Target architecture: dual surface

```
┌─────────────────────────────────────────────┐
│ Android window                                │
│  ┌─────────────────────────────────────────┐ │
│  │ SDL SurfaceView  (borealis UI)            │ │  Z = top
│  │  - PixelFormat.TRANSLUCENT                │ │  alpha EGL config
│  │  - setZOrderMediaOverlay(true)            │ │  draws UI, clears to
│  │  - borealis clears video region to alpha=0│ │  transparent over video
│  └─────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────┐ │
│  │ MPV SurfaceView  (video)                  │ │  Z = below
│  │  - mpv vo=gpu renders directly here       │ │  no GL sharing with SDL
│  └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

The Android SurfaceFlinger composites the two SurfaceViews in hardware —
which is the cheap path the official Plex app and mpv-android both ride.

---

## Component changes

### 1. Native: libmpv `wid` support (new JNI, or extend existing platform glue)

We already link `libmpv.so` (+ ffmpeg) for the C++ player. We need:

- `av_jni_set_java_vm()` called once at startup so mpv's
  `ANativeWindow_fromSurface` JNI works. ffmpeg's MediaCodec also needs
  this — wire it in `platform_android.cpp` init (currently only
  `SDL_main` forwarding lives there).
- A way to get the **Java `Surface` jobject** of the mpv SurfaceView down
  to the C++ player so it can `mpv_set_option(mpv, "wid", INT64, &wid)`.
  Path: Java SurfaceView → JNI call passing the `Surface` → store the
  global ref → hand to `MpvPlayer`.

### 2. Java/Kotlin: dedicated mpv SurfaceView

The app's gradle is `com.android.application`, single `SDLActivity`, no
Kotlin plugin. Minimal addition (Java keeps the gradle change small — no
Kotlin plugin needed):

- A `MpvSurfaceView extends SurfaceView implements SurfaceHolder.Callback`.
- Added to the activity's content view **below** the SDL surface.
- `surfaceCreated` → JNI `nativeAttachSurface(holder.getSurface())`.
- `surfaceDestroyed` → JNI `nativeDetachSurface()` after mpv VO teardown.
- `surfaceChanged` → JNI set `android-surface-size` = `WxH`.

SDL surface must become the overlay:
- `getHolder().setFormat(PixelFormat.TRANSLUCENT)`
- `setZOrderMediaOverlay(true)` on the SDL surface (so it sits above the
  mpv surface but below system UI).
- SDL/borealis GL context needs an **alpha-capable EGLConfig** (8888) and
  borealis must `glClear` the video area to `(0,0,0,0)` so the mpv surface
  shows through.

### 3. C++ `MpvPlayer` — Android branch swap

In `src/player/mpv_player.cpp`, behind `#ifdef __ANDROID__`:

- `init()`: set `vo=gpu`, `gpu-context=android`, `opengl-es=yes`,
  `hwdec=mediacodec-copy` (keep the Playback-Tuning override hook).
- **Remove** the FBO / NanoVG render-context path
  (`setupNonVitaRender()` GL FBO, `m_glFbo`, `m_mpvOpenGLFbo`,
  `m_nvgImage`) for Android.
- New `attachAndroidSurface(jobject)` / `detachAndroidSurface()` that set
  the `wid` option. mpv brings the VO up when the surface attaches.
- `getVideoImage()` returns 0 on Android (no NanoVG texture) — callers
  already null-check it.

### 4. `VideoView` — Android no-op

`src/view/video_view.cpp::draw()` currently composites the NanoVG video
texture. On Android it must instead **punch a transparent hole**: clear
its rect to `(0,0,0,0)` (or skip drawing and rely on borealis' transparent
clear) so the mpv SurfaceView behind shows through. Keep the existing
NanoVG composite for Vita / PS4 / Switch / desktop unchanged.

### 5. Lifecycle + threading

- Surface attaches/detaches on its own callback thread; marshal to the
  mpv command path carefully (mpv options are thread-safe; the VO bring-up
  is async).
- On `willDisappear` / player stop: detach surface *before* destroying the
  SurfaceView (mirror BaseMPVView's order — set `vo=null`,
  `force-window=no`, then `detachSurface`).
- PiP / backgrounding: the mpv SurfaceView must follow the same
  show/hide as the existing video path.

---

## What stays the same

- Vita (GXM render context), PS4, Switch, desktop: **unchanged.** All the
  new code is under `#ifdef __ANDROID__` and the dual-surface wiring is
  Android-only.
- Direct-play (`directPlay=1` → part URL, mpv `start=` seek): keep — it's
  an independent win and reduces server + network load regardless of VO.
- Plex client, queue, subtitle/audio selection, downloads: untouched.

---

## Risks / open questions

1. **SDL translucency on this SDL version.** Making the SDL surface
   `TRANSLUCENT` + `setZOrderMediaOverlay` may need patching SDL's
   `SDLActivity`/`SDLSurface` Java, or SDL hints. Needs a device test —
   some SDL builds force an opaque RGB565/RGBX surface.
2. **borealis alpha clear.** borealis must be told the background is
   transparent during playback. If borealis always clears to opaque
   black, the mpv surface never shows. May need a borealis-side hook.
3. **Z-order correctness across TV firmwares.** `setZOrderMediaOverlay`
   vs `setZOrderOnTop` behaviour differs across Android TV OEM builds
   (the Bravia A1 is Android 7/8-era). Needs testing on the actual A1.
4. **Touch/focus.** Two surfaces — input must continue going to the SDL
   surface (borealis). SurfaceViews don't take focus by default, so this
   should be fine, but verify.
5. **libmpv `wid` jobject lifetime.** Global-ref the Surface; release only
   after VO teardown (the race BaseMPVView's FIXME calls out).

---

## Build / CI changes

- `app/platform/android/app/build.gradle`: add the Java SurfaceView source
  (no Kotlin plugin needed if written in Java). No new native lib if the
  `wid` glue lives in the existing `jni/CMakeLists.txt` target.
- `jni/CMakeLists.txt`: ensure `libmediandk` + `libandroid` linked (already
  added during investigation), and the JNI surface functions are exported.
- No change to `scripts/android/Makefile` (libmpv already builds with
  `gpu-context=android`; the earlier `-Degl-android=enabled
  -Dandroid-media-ndk=enabled` flags from the dropped branch should be
  re-applied — they belong with this work).

---

## Staged implementation plan

Each stage is a separate, reviewable commit. Stages 1–2 can't be fully
validated without a device + CI build, so they land behind the existing
working path until Stage 4 flips the switch.

1. **Native wid plumbing** — `av_jni_set_java_vm` at init; JNI
   `nativeAttachSurface`/`nativeDetachSurface`; `MpvPlayer::attach/
   detachAndroidSurface`. No behaviour change yet (nothing calls it).
2. **Java MpvSurfaceView** — the view + holder callbacks + JNI calls.
   Not added to the layout yet.
3. **SDL translucency + Z-order** — make borealis' surface an overlay,
   alpha clear. Verify borealis still renders normally over a black
   background first.
4. **Flip the Android VO path** — `vo=gpu` + wid, drop the FBO/NanoVG
   composite on Android, `VideoView` transparent hole, wire the
   SurfaceView into the player activity lifecycle. This is the commit
   that should make playback smooth.

Validation after Stage 4: MPV stats overlay should show vo drops at ~0 on
the Bravia / CCwGTV, matching the official Plex app.
