# VitaPlex's quick menu plugin

`vitaplex_qm.suprx`: the song and its cover, and play/pause, next and
previous buttons that work, in the Music row of the PS Vita's quick menu
(hold PS). It is for
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
  dimmed. They stayed dimmed with `Enable(true)` too: their icons are planes
  of their own beside them, whose own alpha (a widget's float at 0x1AC,
  `m_unk_1AC` in ScePaf-RE's headers) SceShell leaves at 0.3 (build 2026).
  By default this one switches the buttons on and sets any faded own alpha,
  meta alpha or colour of the buttons and icons to full, and puts
  SceShell's back when the row is SceShell's again. VitaPlex BG's settings
  can pick the other ways (`QM_LOOK_*`), and the log has each button's state
  and colours before and after.
- **Where:** it changes SceShell's widgets on SceShell's own UI thread,
  through PAF's `common::MainThreadCallList`, as ElevenMPV-A's app changes
  its own (build 2026). ElevenMPV-A's plugin, and this one up to build 2025,
  changed them from threads of their own, and the buttons kept their off
  look, though nothing the log read of them differed from SceShell's lit
  row. It never waits for its own lock there. Its own threads watch the
  quick menu, take VitaPlex's messages and write the log. If SceShell's
  thread never calls it, its watching thread does the work, as before.
- **When:** it takes the row 200 ms after the quick menu opens, as
  ElevenMPV-A's plugin about does. A song set at once did not show. While
  the quick menu stays open, it puts back what SceShell changes.
- **Play and pause:** while VitaPlex plays, the play icon shows the pause
  icon's texture (`0xB26E4562`, a second icon at play's place, sized 0x0),
  as SceShell's own row shows ⏸ while its Music app plays.
- **The cover:** the Music heading's icon (`0xE219E0DE`) shows the song's
  cover, a 64x64 PNG VitaPlex BG writes (`QM_ART_PATH_FMT`, `QM_CMD_ART`).
  The plugin loads it with PAF's own decoder (`LocalFile::Open`,
  `Surface::Load`) into power_manage_plugin's texture pool, and the icon
  keeps the only reference to it. The icon gets its ♫ back when the row is
  given back or a cover will not load.
- **No waiting in SceShell:** a press is sent without waiting, and dropped if
  nobody reads it. The row is given back by itself when no process has asked
  for it in 5 s (the owner asks again every second). So a process the system
  ends cannot hold the quick menu up.
- **A log:** `ux0:data/VitaPlex/qm-plugin.log`, one file a boot, the boot
  before in `qm-plugin.prev.log`. For the first 12 opens of a boot it lists
  the widgets of the Music row and its heading, with their textures and draw
  objects, then their changes for 3 s, whoever has the row. Opens with the
  row SceShell's go on being logged until its own row has been seen lit.
  Once a boot each, it dumps the previous button and its icon byte for byte,
  with SceShell's row off, with it lit, and with the row the plugin's. It
  also logs the BGM port as SceShell reads it.
- **No static constructors:** there is no C runtime to run them, so a global
  object that needs one stays all zeros. The build fails if it has any.

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
2. Compile `main.cpp` with the flags in CMakeLists.txt, once at -O2 and once
   at -O0, so the stubs also cover a compiler that inlines less.
3. Run `gen_paf_stubs.py ScePaf.yml main.o main-O0.o > paf_stubs.S`.

Every NID in it is the hash of its name with PAF's key. 16 of the 23 are
ones ElevenMPV-A's plugin imports. 4 more are ones its app imports
(`MainThreadCallList::Register`, `LocalFile::Open`, `Surface::Load`,
`Widget::GetChild`), as are their libraries. The other 3 (`on_error`, and
`ButtonBase` and `Text`'s `TypeName`) have been in every build the console
has loaded since build 2022. The stubs are not weak, so a firmware missing
one refuses the module, and VitaPlex falls back.

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

From its own list of the row (build 2022): the icons' ids, previous
`0xD639AB27`, play `0xE057A7D0` and next `0x6FA104CC`, pause `0xB26E4562`,
and the heading's, "Music", `0x90E6251E`, with its icon `0xE219E0DE`. The
texture pool a `Plugin` loads its textures into is at offset `0xD4` of it
(ScePaf-RE's `plugin.h`, `surface_pool`). A widget's draw object comes from the virtual
`GetDrawObj` (slot `0xF8`, between `SetColor_core` and `GetTexture`, both
checked on the console); its colour is written only if its size reads as its
widget's.
