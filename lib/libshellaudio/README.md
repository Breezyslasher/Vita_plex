# libShellAudio (vendored)

[libShellAudio](https://github.com/GrapheneCt/libShellAudio) by GrapheneCt,
MIT licensed (see `LICENSE`), taken at commit `a2d01d5`. It drives SceShell's
music service, the one the system Music app plays through, so audio handed to
it keeps playing whatever happens to the app that handed it over.

Upstream builds with Sony's SDK. The changes for vitasdk are listed at the top
of `ShellAudio.c` and `ShellAudio.h`; none of them touch what is sent to the
shell. `shellsvc_stub.S` supplies the one import vitasdk has no stub for,
`sceShellSvcGetSvcObj`.

Used by the Vita background-audio experiment in
`src/utils/background_audio_psv.cpp`.
