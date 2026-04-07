# Android TV playback comparison: VitaPlex vs mpv-android

## What VitaPlex is currently doing on Android TV

From `src/player/mpv_player.cpp`, Android builds currently:

- run with `vo=libmpv` and create a **software** libmpv render context (`MPV_RENDER_API_TYPE_SW`),
- render video into a CPU RGBA buffer,
- upload that CPU buffer into a NanoVG texture every frame (`nvgUpdateImage` path),
- force an internal render target of **1280x720** for Android (`m_videoWidth=1280`, `m_videoHeight=720`).

This means each frame has CPU copy + GPU upload overhead on the UI/render loop.

## What mpv-android does differently

From mpv-android (`app/src/main/java/is/xyz/mpv/MPVView.kt` and `BaseMPVView.kt`), it:

- uses `vo=gpu`/`gpu-next`,
- uses `gpu-context=android` + `opengl-es=yes`,
- attaches Android `Surface` directly to mpv (`attachSurface()`),
- enables hardware decode modes (`mediacodec`, `mediacodec-copy`) and codec allow-list,
- applies `profile=fast` defaults and exposes runtime decoder switching.

In short, mpv-android takes a direct Android surface path, while VitaPlex currently takes a software-frame-copy path.

## Why this can look choppy on TV

For 60Hz TV playback, the software path has two likely bottlenecks:

1. frame generation + conversion in software,
2. per-frame texture upload on top of normal UI rendering.

Even with hardware decoding enabled, the output path still pays software-copy costs when using SW render API.

## Recommended direction

1. Implement an Android-native render path in VitaPlex (surface/wid style), similar to mpv-android.
2. Keep current SW path as fallback for devices/drivers where direct path is unstable.
3. Expose a "Decoder/Renderer" debug toggle on Android TV to quickly compare `mediacodec` vs `mediacodec-copy` vs software.
4. Keep large demux/cache values (already present) but pair them with direct surface rendering to remove upload bottlenecks.

## Quick validation checklist after renderer update

- Run same title with identical transcode settings and compare dropped-frame counters.
- Compare 23.976, 24, 25, 30, and 60fps sources on 60Hz TV output.
- Verify seek latency and subtitle rendering behavior across renderer modes.
