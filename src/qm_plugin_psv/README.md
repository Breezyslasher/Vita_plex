# VitaPlex's quick menu plugin

`vitaplex_qm.suprx`: the song, and play/pause, next and previous buttons that
work, in the Music row of the PS Vita's quick menu (hold PS). It is for
VitaPlex BG and its background player. Both load it into SceShell with
taiHEN the first time they want the row in a boot
(`src/bgplayer_psv/qm_link.c`), and talk to it through the two message pipes
`src/bgplayer_psv/qm_protocol.h` lays out. It stays until the Vita restarts.
Nothing goes in taiHEN's config.txt.

It is based on the quick menu plugin of
[ElevenMPV-A](https://github.com/GrapheneCt/ElevenMPV-A) by GrapheneCt, which
VitaPlexBG.vpk also carries, unchanged, as a fallback
(`third_party/elevenmpva-shell-plugin`). Like it, this folder is licensed
under the GNU General Public License, version 3 (`LICENSE`). It is a program
of its own, which VitaPlex (Apache-2.0) only talks to through pipes.
VitaPlexBG.vpk puts the licence and `NOTICE.txt` beside the plugin.

## What it does differently

- **Buttons:** ElevenMPV-A's plugin switches the row's buttons on with
  `ButtonBase::Enable(false)`. Presses got through, but the buttons stayed
  dimmed. This one uses `Enable(true)` by default. VitaPlex BG's settings can
  pick the other ways (`QM_LOOK_*`), and the log has each button's state
  before and after.
- **No waiting in SceShell:** a press is sent without waiting, and dropped if
  nobody reads it. The row is given back by itself when no process has asked
  for it in 5 s (the owner asks again every second). So a process the system
  ends cannot hold the quick menu up.
- **A log:** `ux0:data/VitaPlex/qm-plugin.log`, one file a boot, the boot
  before in `qm-plugin.prev.log`. Once a boot it lists the Music row's
  widgets, for the artwork and play/pause icon still to come.

## Building

It is written against PAF, the Vita's UI library, which vitasdk declares only
a little of. `scripts/vita/fetch_paf_headers.sh <dir>` fetches GrapheneCt's
[ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE) headers, patched for
vitasdk by Princess-of-Sleeping's
[vitasdk-paf-component](https://github.com/Princess-of-Sleeping/vitasdk-paf-component).
Both are pinned to a commit. ScePaf-RE has no licence file, so it is fetched
at build time and not kept here. CMake builds the plugin when given
`-DVITAPLEX_PAF_INCLUDE_DIR=<dir>/include`, and otherwise leaves it out, in
which case VitaPlex uses ElevenMPV-A's plugin.

`paf_stubs.S` holds the PAF imports, which `scripts/vita/gen_paf_stubs.py`
writes from ScePaf-RE's NID table:

1. Build vitasdk-paf-component's `libpaf/emd2yml` and run it next to the
   patched ScePaf-RE to get `ScePaf.yml`.
2. Compile `main.cpp` with the flags in CMakeLists.txt.
3. Run `gen_paf_stubs.py ScePaf.yml main.o > paf_stubs.S`.

Every NID in it is the hash of its name with PAF's key, and 18 of the 21 are
ones ElevenMPV-A's plugin imports. The stubs are not weak, so a firmware
missing one refuses the module, and VitaPlex falls back.

## Facts it relies on, all from ElevenMPV-A's plugin and its binary

- The quick menu's root comes from `power_manage_plugin`'s interface
  (`Plugin::GetInterface(1)`), a function at offset 0x54.
- The row's widgets have these id hashes:
  - rewind `0x9F6A44DA`, play `0x56734CD1`, fast-forward `0x6CA674D6`;
  - text lines `0x66FDAFE3` and `0xF099B450`.
- Its virtual calls match ScePaf-RE's table: colour at `0x98`
  (`SetColor_core`), text at `0x11C` (`SetString`).
- `_sceAppMgrGetAppState(&state, 0x80, 0x03570000)`, the quick menu being open
  at offset 8 (`isSystemUiOverlaid`).
- Pipes made with type `0x40` and attributes `0x81`; threads at priority 191.
