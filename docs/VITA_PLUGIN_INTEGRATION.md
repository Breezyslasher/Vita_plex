# Using the VitaPlex SceShell plugin from another app

This guide is for a PS Vita homebrew app that wants what VitaPlex gets from
its quick menu plugin (`vitaplex_qm.suprx`):

- **Music row:** the song, cover and working play/pause, next and previous
  buttons in the quick menu's Music row (hold PS).
- **Background music:** the app's queue keeps playing after the app closes,
  including while a game runs.
- **Background downloads:** downloads continue after the app closes, with one
  progress notification throughout and a "Pause downloads?" answer.
- **Self-updates:** the app installs its own update and relaunches, with no
  second "updater" app.

The last part, [borealis performance](#part-2--borealis-performance), is
separate. It lists the build and framework changes that took VitaPlex's home
screen from about 30 fps to about 55–58 fps on the Vita. Any borealis app can
use them.

---

## Part 1 — The plugin

### 1. How it works

SceShell is the Vita's system UI process. No app or game closes it, so code
running inside it outlives your app. The plugin is a taiHEN user module that
your app loads into SceShell the first time it needs it. It stays there until
the Vita restarts.

```
 Your app (ux0:app/<TITLEID>)                    SceShell (NPXS19999)
 ───────────────────────────                     ──────────────────────────────
 qm_link.c                                       vitaplex_qm.suprx
   qmOpen() ── taiLoadStartModuleForPid ──────▶   module_start: creates 2 pipes
   QmMessage  ── "VitaPlexQM17::ToPlugin" ─────▶   receive thread
   QmPress   ◀── "VitaPlexQM17::FromPlugin" ───   (row button presses)

 files in ux0:data/VitaPlex/  ◀──── both read and write ────▶
   handover-queue.txt / handover-status.txt          (music)
   handover-downloads.txt / ...-status.txt           (downloads)
   update-stage/ + update-result.txt                 (update)
   qm-art-0.png / qm-art-1.png                       (row cover)
```

- **Pipes:** two kernel message pipes carry short commands
  (`src/handover_psv/qm_protocol.h`). The protocol version is part of the
  pipe name. An app built against a different version doesn't find matching
  pipes, so it loads its own copy of the plugin next to the old one.
- **Files:** anything large (a queue of 1000 songs, a list of 200 downloads,
  progress status) goes through files in the app's data folder. Every file is
  written in full under a temporary name and then renamed into place. It ends
  with an `end` line, and a reader ignores a file without one.
- **No waiting inside SceShell:** the plugin never blocks SceShell. Presses are
  sent without waiting and dropped if nobody reads them. The row goes back to
  the system by itself 5 s after your app stops sending its heartbeat. If your
  app crashes, the quick menu is unaffected.

### 2. What you need

- **taiHEN (HENkaku or Ensō):** the plugin is loaded with
  `taiLoadStartModuleForPidForUser`, which is in the `taihenUnsafe` library.
  Your app must be unsafe homebrew. VitaPlex and ElevenMPV-A both are.
- **No taiHEN `config.txt` entry:** the app loads the plugin itself at
  runtime.
- **Licence:** the plugin is GPL-3.0, based on GrapheneCt's ElevenMPV-A
  plugin. Ship `LICENSE` and `NOTICE.txt` next to it, as VitaPlex.vpk does
  under `module/`. Your app only talks to it through pipes and files, so your
  app's own licence isn't affected.

### 3. Today it is VitaPlex's plugin: choose how to use it

The plugin and its protocol were written for one app. Several things are
VitaPlex- or Plex-specific:

| Where | What is VitaPlex/Plex-specific |
|---|---|
| `VITAPLEX_TITLEID` (CMake define) | which app the update relaunches (`psgm:play?titleid=`), whose exit the download and update threads wait for, which icon the download notification shows |
| `HANDOVER_DIR` and every path in `qm_protocol.h` / `handover.h` | `ux0:data/VitaPlex/…` |
| Pipe names | `VitaPlexQM17::ToPlugin` / `::FromPlugin` |
| `QM_PLUGIN_LOG` | `ux0:data/VitaPlex/qm-plugin.log` |
| Music fetch, scrobble, timeline | Plex endpoints and `X-Plex-*` headers; the server and token are read from `ux0:data/VitaPlex/settings.json` |
| Download kinds `queue` / `add` | Plex's `/downloadQueue` API |
| Update | expects a package unpacked to `update-stage/` with a forged `sce_sys/package/head.bin` (`src/utils/vita_install.cpp`) |

That leaves two ways to use it.

**A. Fork and rebrand it (works today).** Copy `src/qm_plugin_psv/` and
`src/handover_psv/` into your app, then:

1. Pass your own title ID as `VITAPLEX_TITLEID`, for both the plugin and
   `qm_link.c`.
2. Change `HANDOVER_DIR` and every `ux0:data/VitaPlex/` path to your app's
   data folder.
3. **Rename the pipes** to something like `"MyAppQM1::ToPlugin"`. This step is
   required: if both apps used the same names, they would talk to each
   other's plugin.
4. Replace the Plex-specific parts you need:
   - **Background music:** the song fetch (`fetch…`), scrobble and timeline
     code.
   - **Background downloads:** the downloads' `queue` / `add` handling.

   Or drop the features you don't need. The Music row and updates need no
   server code at all.

5. Build it with the CMake rules VitaPlex uses: `vita_create_self` for the
   `.suprx`, and the PAF headers from `scripts/vita/fetch_paf_headers.sh`.
   See [§9](#9-building-the-plugin).

**B. One shared plugin for several apps (a proposal, not built).** A
multi-app plugin would need:

- An app ID in each message.
- A per-app data folder that the app registers once.
- A generic `url` download kind: a plain URL plus headers, with HTTP Range to
  resume.
- Generic music tracks given as URLs.
- An update command that names the title to install and relaunch.

That is a protocol 18 change, together with renaming the pipes away from
"VitaPlex". Ask if you want it built.

The rest of Part 1 shows the integration as VitaPlex does it. With option A
you follow the same steps against your renamed constants.

### 4. Loading the plugin and talking to it

Ship the plugin in your VPK at `module/<name>.suprx`. `qm_link.c` loads it
from `ux0:app/<TITLEID>/module/vitaplex_qm.suprx`.

```c
#include "qm_link.h"

qmInit();                       // once, before any other call

char what[256];
int rc = qmOpen(what, sizeof what);   // loads it into SceShell if its pipes aren't there yet
if (rc < 0) log("plugin not available: %s", what);   // e.g. no taiHEN: carry on without it
```

- **Threads:** every `qm*` call is thread-safe and returns quickly.
- **Already loaded:** `qmOpen` returns 0 straight away if the pipes are
  already open in this process.
- **Opening the plugin:** a plugin loaded in an earlier run, but not yet this
  boot by this process, is opened through its existing pipes; `what` says
  which happened.
- **Load it early:** VitaPlex calls `qmOpen` when a feature first needs it,
  for example at the start of a download run. Then the hand-over at exit
  doesn't have to load anything.
- **Stub file:** `qm_link_stub.S` provides `sceKernelOpenMsgPipe` /
  `sceKernelCloseMsgPipe`, which vitasdk doesn't declare, and the taiHEN
  loader call. Link it with `qm_link.c`.

### 5. The Music row

The row works for any app as-is: it has no server or Plex code.

```c
qmActivate(QM_OWNER_APP);                 // take the row whenever the quick menu opens

// every second while you want it (a timer or your audio thread):
qmHeartbeat(QM_OWNER_APP);                // after 5 s without one, the row goes back to the system

qmSetSong("Title", "Artist", "Album");    // UTF-8: line 1 = title, line 2 = "artist / album"
qmSetPlaying(1);                          // 1 playing (shows ⏸), 0 paused

// the cover: a 64x64 PNG written to QM_ART_PATH_FMT with this serial, then:
qmSetArt(serial);                          // 0 = no cover (the ♫ icon)

// presses, polled (e.g. once per frame):
switch (qmPoll()) {
    case QM_PLAY_PAUSE: togglePause(); break;
    case QM_NEXT:       next();        break;
    case QM_PREVIOUS:   previous();    break;
}

qmDeactivate();                           // give the row back (when playback stops / app exits)
```

**Covers:** the plugin decodes the cover with PAF while your app may be
writing the next one. So alternate between two files: the serial's lowest
bit picks `qm-art-0.png` or `qm-art-1.png`. Serials should only increase
within a boot, even across app restarts. `src/handover_psv/qm_cover.c` writes
a 64x64 PNG for you.

### 6. Background music (hand the queue to SceShell)

The plugin plays the queue through SceShell's own music service, which keeps
playing while games run. Today it fetches missing songs from a Plex server,
so with option A this is the part you rewrite. The app side of the
hand-over stays the same.

**When your app is closing.** The system asks your app to quit when the user
starts a game or closes it from the LiveArea. VitaPlex handles this in
`src/utils/background_audio_psv.cpp`: `onRequestQuit`, and `onDeactivate`
for the PS button.

1. Stop your own player.
2. Give the row back: `qmDeactivate()`.
3. Write `HANDOVER_QUEUE` (format below) to a temporary name, then rename it
   into place.
4. If you can, prepare the current song as a file, so the shell starts it
   without fetching. Write it as `HANDOVER_SONGS/<key>.<ext>.ready` and name
   it in `file=`.
5. Call `qmPlayQueue()`, then quit promptly. The system waits for your
   process to end before it starts the game.

```
written=12345.678901          # sceKernelGetSystemTimeWide()/1e6, %.6f
index=3                       # the song to start with (0-based)
positionMs=81234              # where in it
repeat=0                      # 0 off, 1 one, 2 all
file=ux0:data/VitaPlex/handover/123.mp3.ready
offlineSkip=1                 # optional
track=<key>|<durationMs>|<local file or empty>|<rating>|<thumb>|<title>|<artist>|<album>|<cover png>
track=…                       # up to HANDOVER_MAX_TRACKS (1000), in play order
end
```

**When your app opens again:**

1. Read `HANDOVER_STATUS`. If it contains `by=SceShell`, the shell is
   playing: take `index=`, `positionMs=`, `paused=` and `fetching=` from it.
2. Set your player up from the queue file and the status.
3. Once your player is ready to start, call `qmTakeBack()`. The plugin stops
   the shell's song and writes the status one last time with exactly where it
   stopped, so play on from that.

### 7. Background downloads

Only the two fetch kinds, `queue` and `add`, are Plex-specific. Everything
else in the flow is generic: the hand-over, resuming by HTTP Range, the
status file and the notification. With option A you would add a plain `url`
kind (see [§3](#3-today-it-is-vitaplexs-plugin-choose-how-to-use-it)).

**While your app downloads,** show progress through the plugin, so the same
notification carries on after your app closes:

```c
qmDownloadProgress("Downloading 3 of 12", "Movie title", permille /* 0..1000, or QM_PROGRESS_UNKNOWN */);
...
qmDownloadProgressEnd("Downloads complete", "12 items");
```

The notification's question is "Pause downloads?". If the user answers Yes
while your app is running, the plugin creates `QM_DOWNLOADS_PAUSE`. Check for
that file about once a second in your download thread, and pause when it's
there.

**When your app is closing:**

1. Save your own state first, marking which downloads are being handed over.
   The system's watchdog can kill a slow exit, and you mustn't lose that
   mark.
2. Write `QM_DOWNLOADS_LIST`:

   ```
   written=12345.678901
   notify=1
   client=<X-Plex-Client-Identifier>      # or your app's equivalent
   platform=PlayStation Vita
   device=PS Vita
   item=<key>\t<file>\t<size or 0>\t<title>\t<token>\tqueue\t<server>\t<queueId>\t<itemId>
   item=<key>\t<file>\t<size or 0>\t<title>\t<token>\tadd\t<server>\t<?query>\t<profileExtra>
   end
   ```

   - Items are fetched in the order listed, up to `QM_DOWNLOADS_MAX` (200).
   - Each one continues the file your app started, from its current size.

3. Call `qmCarryDownloads()`. If the plugin refuses, unmark the downloads.
4. Quit. Don't wait for your download thread. The plugin waits up to 30 s for
   your process to end before it touches the files.

**When your app opens again:**

1. Before you check or delete any partial file, read `QM_DOWNLOADS_STATUS` and
   count each of its `item=` keys as handed over.
2. Call `qmTakeDownloads()`.
3. Wait for `running=0` in the status. VitaPlex waits up to 120 s, and treats
   a status unchanged for 40 s as stopped.
4. Apply each item's state:

   | State | Meaning |
   |---|---|
   | `done` | finished |
   | `going` / `waiting` | resume it yourself, from the byte count |
   | `gone` | the server no longer has the queue item |
   | `failed` | gave up after its retries |
   | `paused=1` (whole file) | keep them paused |

5. Pick the notification back up with your next `qmDownloadProgress`. The
   plugin leaves it showing for up to 30 s so it stays one notification.

**Icon:** a notification shows its owner's icon. The plugin hooks SceShell's
owner lookup so the notification shows `VITAPLEX_TITLEID`'s icon. Under
option A it shows your title's.

**Notification internals:** the parameter layout SceShell actually reads was
found by disassembling SceShell 3.60. See `docs/DESIGN_NOTES.md`.

- **Begin:** 0x570 bytes. The question text is at 0x4F0, and the event
  handler is the word at 0x4E8.
- **Progress:** a float in percent (0–100), not 0–1.
- **Picture:** none. SceShell takes no picture from the parameters, so a
  poster or cover can't be shown.

### 8. Self-update without a second app

The system's installer, ScePromoterUtil, refuses to install over a title
that's running. So the plugin installs your update after your app has gone.

1. Unpack the update to `QM_UPDATE_DIR` and forge `sce_sys/package/head.bin`.
   See `src/utils/vita_install.cpp`: `makeHeadBin` and the `fpkg_hmac` code.
2. Call `qmInstallUpdate()`, then quit.
3. The plugin waits up to 30 s for your app to exit, then runs PromotePkg,
   retrying up to three times while the title still counts as running.
4. It writes `QM_UPDATE_RESULT`: `result=0x…`, and on failure a `why=` line,
   then `end`.
5. It relaunches your title either way, so on startup read the result file and
   report a failure to the user.

### 9. Building the plugin

- **PAF headers:** fetch them with `scripts/vita/fetch_paf_headers.sh <dir>`
  (ScePaf-RE, patched for vitasdk), then configure with
  `-DVITAPLEX_PAF_INCLUDE_DIR=<dir>/include`.
- **Compiler flags:** `-O2 -fno-rtti -fno-exceptions -fno-builtin
  -fshort-wchar -fno-threadsafe-statics -fno-sized-deallocation`, linked
  `-nostartfiles -nostdlib`. The plugin has no C runtime.
- **No static constructors:** nothing would run them, and the build fails if
  any are present.
- **No 64-bit division:** libgcc's helper is built for a different `wchar_t`,
  and pulling it in breaks the build.
- **New PAF calls:** `src/qm_plugin_psv/README.md` explains how to regenerate
  `paf_stubs.S`.

### 10. Debugging

- **Plugin log:** `ux0:data/VitaPlex/qm-plugin.log` holds the current boot,
  and `qm-plugin.prev.log` the boot before. Line prefixes: `carry on:` for
  music, `download:` for downloads, `notification:` for the notification.
- **Protocol version:** raise it whenever a message's meaning changes. The
  pipe names change with it, so an old plugin still in SceShell this boot
  simply goes unused.
- **One plugin per boot:** a changed plugin build only loads after a reboot
  if the pipe names stayed the same.

---

## Part 2 — borealis performance

These are what took VitaPlex's busiest screen, the home tab with about 25
poster cards, from about 30 fps to about 55–58 fps on the Vita. They also
removed most of its 0.5–2.6 s freezes. The patches live in `patches/` and
`CMakeLists.txt` copies them over the borealis submodule with `configure_file`
on every configure, so the submodule stays unmodified.

The frame profiler below measured everything.

### 1. Build optimised (the biggest single win)

With no `CMAKE_BUILD_TYPE`, CMake passes no `-O` flag at all. The app,
borealis, Yoga and nanovg were all compiled at `-O0` on every CI platform.
Set a default before `project()`:

```cmake
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()
```

`RelWithDebInfo` is `-O2 -g -DNDEBUG`, so crash dumps still resolve to source
lines. Check the flags with `ninja -C build -t commands | grep view.cpp`.

On the Vita:

| | Before (`-O0`) | After (`-O2`) |
|---|---|---|
| Building the frame's draw data | 30–31 ms | 2.6–10 ms |
| Frame rate | 29–30 fps | 53–58 fps |
| Main thread busy | 96–99% | 18–50% |

Each view got cheaper too:

| View | Before | After |
|---|---|---|
| Label | 0.18 ms | 0.04 ms |
| Box | 0.11 ms | 0.02 ms |
| Card | 0.43 ms | 0.19 ms |

### 2. Lay out on demand (`patches/brls_view.cpp`, `brls_application.*`)

`View::invalidate()` ran a Yoga layout of the whole tree immediately, every
time. Every `setText`, `addView` or GONE↔VISIBLE change cost one full pass
(about 10 ms on a big page):

- each focus move: 4–8 passes
- building a tab: 55 passes

**Patch:**

- On the UI thread, `invalidate()` only records the root
  (`Application::deferLayout`).
- `getX/getY/getWidth/getHeight` (and the debug wireframe) call
  `flushLayout()` first, so anything that reads layout still sees current
  values.
- The main loop flushes once before drawing.
- A deleted view is removed from the pending list.
- Off the UI thread it still lays out immediately.

**Result:** a focus move went from 40–110 ms to about 16–25 ms, and building
a tab from 55 passes to about 22.

### 3. Register XML attributes lazily (`patches/brls_view.cpp`)

Every `View` constructor registered about 80 XML attribute handlers: a name
plus a `std::function` in hash maps and a set. Each destructor freed them
again. Views built in code never use them. Deleting the home tab took 1.4 s,
and a plain `Rectangle` cost as much to delete as a `Label`.

**Patch:** `registerCommonAttributes()` runs the first time a view actually
receives XML attributes. That is the first call to `applyXMLAttribute`,
`isXMLAttributeValid` or the error message.

**Pitfall (build 2087 crashed at start):** the same name is registered for
several value kinds. `width` is registered as auto, as a percentage and as a
float. When registering late, skip a name only if *that kind's* map already
has it, which is what a subclass's override would have replaced. Don't skip
by name alone.

**Result:** deleting the home tab dropped from 1.4 s to 37–110 ms. The slow
row-building tasks dropped from 1.36 s to about 0.16 s.

### 4. Cull without re-walking the tree (`patches/brls_box.cpp`)

Before drawing each leaf child, `Box::draw` checked it against the culling
bounds of every ancestor. Each `getX`/`getY` walks up to the root, so a
card's labels cost depth × depth layout reads every frame.

**Patch:** collect the ancestor bounds once per `Box::draw`, then test each
child against that list. The result is the same.

### 5. Never block the UI thread

The frame log found each of these:

- **Log lines:** writing each line, line-buffered, from the calling thread
  cost about 10 ms per line on the memory card. A shared writer thread
  (`src/utils/log_file_writer.cpp`) now queues lines and flushes them in
  batches. Error lines are still written before the logger returns, so a
  crash keeps them.
- **Images:** `setImageFromMem` / `nvgCreateImageMem` decoded on the UI
  thread at about 50 ms per poster, which caused 0.6–2.6 s freezes. Now
  images are decoded to RGBA with stb_image on a worker thread, and the UI
  thread only does `nvgCreateImageRGBA` and `Image::innerSetImage`.
- **Building rows and grids:** creating dozens of cards in one `brls::sync`
  task took 0.4–1.4 s. Now the first screenful is built immediately and the
  rest a row or a few cards per frame. A generation number stops an outdated
  build, and moving onto unbuilt content builds it on the spot.
- **Network on the UI thread:** a 10 s timer sent a Plex timeline report
  synchronously, stalling for 0.3–1.2 s. Now it goes through a worker thread,
  numbered so an older report can't be sent after a newer one.

### 6. Smaller wins

- **Hints bar:** skip rebuilding it when the hints haven't changed
  (`patches/brls_hint.cpp`).
- **Static vector shapes:** draw them into a texture once, for example the
  rating badge icons, instead of tessellating them every frame.
- **Skip no-op changes:** compare before calling `setText` or
  `setVisibility`. Each one invalidates the layout.

### 7. The frame profiler (`src/utils/frame_profiler.*`)

Install it with `perf::install()` before the main loop. Every 5 s in which
the frame rate drops below 55 fps, or any frame takes over 50 ms, it logs a
`[perf]` summary:

- fps, the worst second, and the main thread's busy %
- per-phase averages and maximums: poll, input (and its focus-change parts),
  anim, layout, begin, views, nvg, swap, sync, runloop, delete
- draw calls and triangles
- once a second, the views' own drawing time by type
- running tickings by type
- the current screen and focus

It also logs, as they happen:

- **Slow frames:** a breakdown of any frame over 50 ms, plus what changed and
  what read the layout when it had 2 or more layout passes.
- **Slow sync tasks:** `brls::sync` tasks of 15 ms or more, named by their
  lambda's demangled type, which includes the function it was written in.
- **Slow deletions:** frames whose view deletion took 30 ms or more, by view
  type.

Supporting patches:

- `patches/brls_application.cpp`: phase timings
- `patches/brls_thread.cpp`: sync-task timing
- `patches/nanovg.c`: `nvgFrameCounts`
- Yoga's event subscription: layout passes and measure callbacks
