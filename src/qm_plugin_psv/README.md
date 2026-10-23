# VitaPlex's quick menu plugin

`vitaplex_qm.suprx`: the song and its cover, and play/pause, next and
previous buttons that work, in the Music row of the PS Vita's quick menu
(hold PS); and VitaPlex's music played on from inside SceShell, which no app
or game closes, once VitaPlex has gone. VitaPlex loads it into SceShell with
taiHEN the first time it wants the row or hands its music over in a boot
(`src/handover_psv/qm_link.c`), and talks to it through the two message
pipes `src/handover_psv/qm_protocol.h` lays out. It stays until the Vita
restarts. Nothing goes in taiHEN's config.txt.

It is based on the quick menu plugin of
[ElevenMPV-A](https://github.com/GrapheneCt/ElevenMPV-A) by GrapheneCt. Like
it, this folder is licensed under the GNU General Public License, version 3
(`LICENSE`). It is a program of its own, which VitaPlex (Apache-2.0) only
talks to through pipes. VitaPlex.vpk puts the licence and `NOTICE.txt` beside
the plugin.

## What it does differently

- **Buttons:** ElevenMPV-A's plugin switches the row's buttons on with
  `ButtonBase::Enable(false)`. Presses got through, but the buttons stayed
  dimmed. They stayed dimmed with `Enable(true)` too: their icons are planes
  of their own beside them, whose own alpha (a widget's float at 0x1AC,
  `m_unk_1AC` in ScePaf-RE's headers) SceShell leaves at 0.3 (build 2026).
  This one switches the buttons on, puts them in their normal state, and sets
  any faded own alpha, meta alpha or colour of the buttons and icons to full,
  which lights them (build 2028), and puts SceShell's back when the row is
  SceShell's again.
- **Where:** it changes SceShell's widgets on SceShell's own UI thread,
  through PAF's `common::MainThreadCallList`, as ElevenMPV-A's app changes
  its own (build 2026). ElevenMPV-A's plugin, and this one up to build 2025,
  changed them from threads of their own, and the buttons kept their off
  look. It never waits for its own lock there. Its own threads watch the
  quick menu, take VitaPlex's messages and write the log. If SceShell's
  thread never calls it, its watching thread does the work.
- **When:** it takes the row 200 ms after the quick menu opens, as
  ElevenMPV-A's plugin about does. A song set at once did not show. While
  the quick menu stays open, it puts back what SceShell changes.
- **The song's lines:** SceShell fades the row's two lines out by their own
  alpha (as the icons' above) when it sees no music of its own, and it saw
  none on the first open after VitaPlex began to play: the song showed only
  from the second open (build 2049). While the row is the plugin's, the
  lines are kept at the alphas SceShell gives them (1 and 0.8), and its
  "There are no songs." line at 0 (build 2050), which gets SceShell's own
  alpha back with the row (build 2052). The first opens of a boot,
  and the first after the row changes hands, are logged as they close: when
  the row was taken, what was put back after, and the lines 1 s in.
- **Play and pause:** while the music plays, the play icon shows the pause
  icon's texture (`0xB26E4562`, a second icon at play's place, sized 0x0),
  as SceShell's own row shows ⏸ while its Music app plays.
- **The cover:** the Music heading's icon (`0xE219E0DE`) shows the song's
  cover, a 64x64 PNG: VitaPlex's (`src/handover_psv/qm_cover.c`,
  `QM_ART_PATH_FMT`, `QM_CMD_ART`) while it plays, and as SceShell plays on,
  a download's own, which the server made as it was downloaded, or the
  server's, fetched before the song's file so that it shows while the song
  is waited for (build 2048), or the one of a song before it of the same
  album. The
  plugin loads it with PAF's own decoder (`LocalFile::Open`,
  `Surface::Load`) into power_manage_plugin's texture pool, and the icon
  keeps the only reference to it. The icon gets its ♫ back when the row is
  given back or a cover will not load.
- **Playing VitaPlex's queue:** VitaPlex hands its queue to SceShell as it
  goes (`QM_CMD_PLAY_QUEUE`). A thread of its own, created the first time,
  plays it through SceShell's music service, as VitaPlex's background player
  did through libShellAudio up to build 2045: from the queue's song and
  place in it, that song from the file VitaPlex made ready (`file=`), else
  from its download, else from the server, and the shell taken to the place
  so that the song's start is not heard: the seek time set before play, the
  seek asked for right after it, and, as the shell took neither in build
  2048, the music thread looking every 25 ms for 4 s and asking again the
  moment the shell plays (build 2049). It finds the service's client
  (`sceShellSvcGetSvcObj`) at run time with taiHEN's `taiGetModuleExportFunc`
  (`taihen_stub.S`), and loads `vs0:sys/external/libshellsvc.suprx` into
  SceShell if no module there has it. It writes the status
  (`src/handover_psv/handover.h`) with `by=SceShell` every 2 s and at every
  change, so that VitaPlex takes the music back where SceShell got to
  (`QM_CMD_TAKE_BACK`), which it asks for once its player is ready: the
  plugin then stops the shell's song and writes where (build 2049; up to
  build 2048 the song was left for VitaPlex to stop), and `paused=1` while
  the row's play/pause holds it, which VitaPlex keeps (build 2052). The row
  stays, its buttons acting on this music. A
  song that goes on away from where it stopped, as an M4A download went on
  35 s back after a pause, is sought back there (build 2036), and so is one
  that goes further back within 10 s of playing again, as M4As did 20 to 37 s
  once the looks came every 100 ms (build 2053). SceShell stops
  its song itself 0.1 to 0.4 s after the power button is pressed, as at four
  of seven presses in build 2002's test of VitaPlex's background player; and
  ElevenMPV-A plays it again too. The music thread looks every 100 ms while a
  song plays, and plays it again at once, and every 0.6 s while it stays
  stopped (build 2051; every half second and every second before). A song
  played to its end is marked played on the server. Its log lines begin
  "carry on:".
- **Fetching the queue's songs (build 2042):** songs not on the memory card
  are fetched from VitaPlex's server, the next while the one before plays.
  The plugin finds Sony's SSL and HTTP libraries in SceShell at run time, as
  it finds the music service, and loads their files from `vs0:sys/external`
  if they are not there (build 2040). It starts the libraries only if
  SceShell has not, and ends only what it started; the TLS options and
  certificates, which hold for all of SceShell, it leaves alone. Each song's
  file comes as the server has it when the shell plays its format, with its
  cover as a PNG from the server's photo transcoder. Both go into the folder
  of songs (`HANDOVER_SONGS`), where they stay while their song is near: the
  song before, for previous, the one playing, and the next. All of them stay
  as VitaPlex takes the music back, which plays on from them (build 2048),
  and a song's file found there is played as it is. A song wanted before it
  has come is waited for, with the shell stopped, and the status says
  `fetching=1` so that VitaPlex can take the music back. With VitaPlex's
  "Skip to Downloads When Offline" setting (`offlineSkip=1` in the queue,
  build 2054), a song the server does not answer for, as with Wi-Fi off, is
  fetched twice and then passed over for the nearest song after it on the
  memory card, looked for through the whole queue, repeat all going round;
  with none, it is held, paused on the row: the row's play fetches it again,
  and VitaPlex takes it back paused. Without the setting such songs are
  passed over, and three in a row stop the queue. One
  fetch runs at a time, on a thread of its own. The server and the token come
  from VitaPlex's settings; the token is read for each fetch, sent as a
  header, wiped after it, and never logged. From build 2046, a song in a
  format the shell does not play, such as FLAC, comes as the server's MP3 of
  it: the transcode decision, then start.mp3. A streamed transcode has no
  length header, so the frames go to the shell behind a Xing frame giving
  their count, without the ID3 tag; without one the shell read a 2:51 song
  as 19:17 long. From build 2048 the frames are put behind it as they come
  (`src/handover_psv/mp3_frames.h`, which VitaPlex uses too as it makes the
  server's MP3 of its song ready), not in two passes after.
- **Telling the server:** while SceShell plays the queue, the server is told
  where the music is as VitaPlex tells it (`/:/timeline`): the song, playing,
  paused, buffering while it is fetched, or stopped, and where in it, at each
  change and every 10 s, under VitaPlex's own client id, so that Plex shows
  the music going on as VitaPlex's (build 2048). One report waits at most; a
  server that does not answer is logged once and holds nothing up.
- **VitaPlex's updates (build 2056):** the system's installer,
  ScePromoterUtil, refuses a title while it runs. Up to build 2055 VitaPlex
  installed a second app for its updates, the updater stub (VPLXUPD01),
  launched it and quit. Now VitaPlex unpacks the update to
  `ux0:data/VitaPlex/update-stage`, forges its package head, asks the plugin
  (`QM_CMD_INSTALL_UPDATE`, protocol 15) and quits. The plugin waits for it
  to be gone (`sceAppMgrGetIdByName`, 30 s at most), then has the installer
  install the folder. It reaches the installer as
  [Vitacompanion](https://github.com/devnoname120/vitacompanion)'s SceShell
  module does: the module loaded through SceSysmodule unless SceShell has it,
  its functions found by NID, and both kept, as the client is SceShell's
  too. It drives it as the stub did: PromotePkg, the state looked at until
  idle, then the result; again 2 s on, up to three times, while the title is
  taken for running. The outcome goes to `ux0:data/VitaPlex/update-result.txt`
  and VitaPlex is launched again either way; on its next start VitaPlex shows
  a failure, and keeps the downloaded VPK for VitaShell.
- **VitaPlex's downloads (protocol 16):** as the system closes VitaPlex,
  as starting a game does, the downloads under way are listed in
  `ux0:data/VitaPlex/handover-downloads.txt` and handed over
  (`QM_CMD_DOWNLOADS`). A thread of its own waits for VitaPlex to be gone
  (30 s at most), then carries them on one at a time, songs and videos alike,
  each into the file VitaPlex had begun, from where that file ends, by HTTP
  Range. A download the server prepares in its download queue is waited for
  there as VitaPlex waits for it (its item's status every 5 s, every 30 s
  after 5 minutes, an hour at most), and one not yet in the queue is added
  to it under VitaPlex's own client id, with the query and profile VitaPlex
  would have sent. A try that fails is made again after 5, 15 and 60 s; one
  that gets some of the file starts the count again. It gives way to the
  music's fetches, and it shows its progress in SceShell's own progress
  notification, the one VitaPlex shows while it downloads, found as the HTTP
  library is. How far it has got goes to
  `ux0:data/VitaPlex/handover-downloads-status.txt` every 2 s; VitaPlex asks
  for them back as it opens (`QM_CMD_TAKE_DOWNLOADS`), and the thread stops
  after the piece it is reading, writing nothing more, with `running=0` in
  the status. The fetch, server and download threads share Sony's HTTP
  library: the first to start it starts it and the last to end it ends it.
  Its log lines begin "download:".
- **The download notification (protocol 17):** a progress notification is
  its process's, and no call names one, so VitaPlex's own could be gone on
  with by nothing once it had closed, and SceShell began a second. Now the
  plugin shows VitaPlex's too: VitaPlex sends its progress
  (`QM_CMD_DL_PROGRESS`, `QM_CMD_DL_PROGRESS_END`), so the one notification
  goes on as SceShell carries the downloads on, and again as VitaPlex takes
  them back. One taken back is left showing for VitaPlex to go on with, and
  ended if it has not in 30 s. VitaPlex uses its own only when the plugin is
  not there. Its question, "Pause downloads?", sends event 0 for Yes and
  nothing for No: Yes pauses the downloads, here or in VitaPlex
  (`QM_DOWNLOADS_PAUSE`). A notification shows its calling app's icon, so
  this one, begun from SceShell, would show none; SceShell takes no picture
  from the parameters (docs/DESIGN_NOTES.md). So the plugin hooks
  SceShell's import of `sceAppMgrGetStatusByAppId`, which it asks the owner
  with, and answers VitaPlex for the one lookup after its Begin that names
  no app: VitaPlex's icon.
- **No waiting in SceShell:** a press is sent without waiting, and dropped if
  nobody reads it. The row is given back by itself when no process has asked
  for it in 5 s (the owner asks again every second). So a process the system
  ends cannot hold the quick menu up.
- **The BGM port:** as ElevenMPV-A's plugin does, it has SceShell read the
  port's priority as no more than 0x80 (a hook on
  `sceAppMgrGetCurrentBgmState2`); VitaPlex's own player takes 0x81.
- **A log:** `ux0:data/VitaPlex/qm-plugin.log`, one file a boot, the boot
  before in `qm-plugin.prev.log`: what it does with the music SceShell
  carries on, and what fails.
- **No static constructors:** there is no C runtime to run them, so a global
  object that needs one stays all zeros. The build fails if it has any.
- **No 64-bit division:** libgcc's is built for another `wchar_t`, so times
  are split into seconds a byte at a time (`splitUs`), with 32-bit steps.

## Building

It is written against PAF, the Vita's UI library, which vitasdk declares only
a little of. `scripts/vita/fetch_paf_headers.sh <dir>` fetches GrapheneCt's
[ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE) headers, patched for
vitasdk by Princess-of-Sleeping's
[vitasdk-paf-component](https://github.com/Princess-of-Sleeping/vitasdk-paf-component).
Both are pinned to a commit. ScePaf-RE has no licence file, so it is fetched
at build time and not kept here. CMake builds the plugin when given
`-DVITAPLEX_PAF_INCLUDE_DIR=<dir>/include`, as the CI does, and otherwise
leaves it out of VitaPlex.vpk, which then has no Music row and no music once
a game starts.

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
one refuses the module. The plugin no longer calls all of them; a stub with
no caller is linked all the same, and has been loaded so since build 2022.

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
and the heading's icon, "Music"'s, `0xE219E0DE`. The texture pool a `Plugin`
loads its textures into is at offset `0xD4` of it (ScePaf-RE's `plugin.h`,
`surface_pool`). A widget's draw object comes from the virtual `GetDrawObj`
(slot `0xF8`, between `SetColor_core` and `GetTexture`, both checked on the
console); its colour is written only if its size reads as its widget's.
