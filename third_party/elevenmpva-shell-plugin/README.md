# ElevenMPV-A's shell plugin (bundled, unchanged)

`shell_plugin.suprx` is the quick menu plugin of
[ElevenMPV-A](https://github.com/GrapheneCt/ElevenMPV-A) by GrapheneCt,
licensed under the GNU General Public License, version 3 (`LICENSE`,
ElevenMPV-A's own copy). VitaPlexBG.vpk carries it as it is, as
`module/shell_plugin.suprx`, with that licence and `NOTICE.txt` beside it.
Nothing else here is built or packed.

## Where it comes from

- ElevenMPV-A's release v7.10: tag `v7.10`, commit
  `2229bd6fd20192c0898710a5e575dc5b153c1c71` (19 Nov 2022), asset
  `ElevenMPV-A.vpk` (1,730,875 bytes, sha256
  `9c820ce3f2b8c4dc5aff856ad647cc35a4dff84f099a486bfca330897a38501f`),
  file `module/shell_plugin.suprx`.
- 8,112 bytes, sha256
  `8db94e56f11a0e8eb4299f4c0862c9ad119f540dfee4c2681f85057e0d71aae7`. The
  same bytes (git blob `be0c2fda21039cb23a507220b87bc24654014026`) are
  committed in ElevenMPV-A's repository at that tag, as
  `ElevenMPV-A/CONTENTS/module/shell_plugin.suprx`, beside the source it is
  built from.

## Its source

`source/` is the plugin's source from that commit, unchanged and in
ElevenMPV-A's layout; each file has the same git blob id as upstream's at
v7.10:

- `ElevenMPV-A-ShellPlugin/`: `main.cpp`, `main.h`, `paf_runtime.cpp`, and
  the Visual Studio project that builds them (`.vcxproj`, `.filters`,
  `.user`). `$(SolutionDir)` in it is `source/`; its last step copies the
  plugin into ElevenMPV-A's own app folder, which is not here.
- `ElevenMPV-A-Common/include/ipc.h`: the messages it takes and sends.

It builds with Sony's PS Vita SDK (`SCE_PSP2_SDK_DIR` in the project), not
with vitasdk, which is why VitaPlex ships the release's file rather than
building it.

## How VitaPlex uses it

As a fallback, since build 2020: VitaPlex BG's own plugin
(`src/qm_plugin_psv`, based on this one) is loaded first, and this one only
when that will not load. As a program of its own. VitaPlex BG and its
background player load it into SceShell with taiHEN the way ElevenMPV-A
does, unless it is there already this boot, and talk to it through the two
message pipes it makes (`src/bgplayer_psv/qm_link.c`). None of it is linked
into VitaPlex, so VitaPlexBG.vpk is an aggregate: VitaPlex stays Apache-2.0,
and the plugin GPL-3.0, with its source here.

In SceShell it puts the song on the quick menu's Music row (the title, then
"artist / album") and sends the presses of the row's play/pause, next and
previous buttons back. It also has SceShell read the BGM port's priority as
no more than 0x80 (a hook on `sceAppMgrGetCurrentBgmState2`). It stays
loaded until the Vita restarts. Whichever of VitaPlex BG and ElevenMPV-A
loads it first, the other uses that copy.

## Updating it

Take `module/shell_plugin.suprx` from a newer release's `ElevenMPV-A.vpk`,
check that its bytes are the ones committed at that release's tag, replace
`source/` and `LICENSE` with that tag's files, check `ipc.h` against the
messages in `qm_link.c`, and update the hashes above.
