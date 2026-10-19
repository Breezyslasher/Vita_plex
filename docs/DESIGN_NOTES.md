# Design notes

Reasoning that does not fit in a one-line comment and cannot be recovered by
reading the code: Plex API behaviour that is not in the spec, workarounds for
mpv and for individual platforms, and the bugs that explain why some code looks
the way it does.

Code comments stay short and point here by name. This file is organised by
subsystem, not by file.

---

## Play queues

### `POST /playQueues` takes a URI *or* a playlist, never both

The operation accepts `uri` or `playlistID`, and they are mutually exclusive.
For a playlist the ID is the source; sending a URI built from the first track
instead makes the server build a queue of exactly that one track. A 4026-track
playlist came back as one item this way, and the client adopted it over the full
list it had already fetched.

Twelve call sites reach `createWithQueue` from a playlist. A first pass found
nine, and the three in `LibrarySectionTab::playPlaylist` and `MusicTab` were
missed until a device log proved it — walk every `fetchPlaylistItems` call that
reaches `createWithQueue` rather than picking likely ones.

### The source URI grammar

A media-server source URI is:

    server://{machineIdentifier}/com.plexapp.plugins.library/{path}

`library://` is the *other* shape and its authority is a library section UUID —
36 characters with dashes — not the 40-hex machine identifier. Mixing the two
produces a scheme the server cannot resolve, and it answers 400.

The path is left unescaped; the whole URI is percent-encoded once when it goes
in as a query parameter.

### The response is a window, not the whole queue

Plex returns `playQueueTotalCount` for the full queue and a couple of dozen
items. A 4026-track playlist answered with the full count and 21 items. Only
what actually arrived can be played, so the guard that decides whether to adopt
a server queue compares `items.size()`, never the count. The client-side list is
already complete, so it wins whenever the window is short.

Fetching the whole thing would need `GET /playQueues/{id}?window=N`, which is
about 7 MB for a queue that size. Not done.

### Albums and artists are still client-side

They have no ID parameter — a container URI is the only way to name them — and
the URI shape this server expands has not been established. They keep the
client-side queue that the count check falls back to: complete and correct, just
not server-backed.

### Nested arrays broke the parser

`parsePlayQueueItems` found the end of the `Metadata` array with
`json.find(']', arrStart)` — the first `]` after it opened. Every entry carries
nested `Media`, `Part`, `Stream` and `Genre` arrays, so that bracket sits inside
the *first* entry and the loop stopped there. 42 KB of response, one item parsed.

This was true of every play queue the client had ever read. It stayed invisible
while `createPlayQueue` was answering 400, because the single-item queues that
followed happened to have one item anyway. The scan now matches brackets by
depth and skips quoted strings, so a `]` in a track title cannot end it early.

### Reordering works in play order

While shuffle is on, play order lives in `m_shuffleOrder` and `m_queue` must not
be touched. `moveTrack` moves absolute indices and is wrong there;
`moveInPlayOrder` is the call that knows the difference. Both the drag path and
the L/R bumper path go through it, and both resolve the server-side anchor
through the shuffle order.

---

## Playback

### `keep-open=yes` means `END_FILE` never fires

mpv does not unload the file when playback finishes, so `MPV_EVENT_END_FILE`
only arrives for a stop, an error or a redirect. `eof-reached` is the only
end-of-track signal on every platform.

The demuxer raises `eof-reached` for a *truncated read* exactly as it does for a
real end of file. A connection reset, a Wi-Fi blip or a transcode session the
server tore down all arrive as "finished", at whatever second they happened, and
the queue advances. From the outside the music randomly skips.

### Judge the end against the server's duration, not mpv's

The first guard compared mpv's position against mpv's duration and failed:

    mpv ffmpeg: tcp: ffurl_read returned 0xdfb9b0bb
    mpv lavf: EOF reached.
    MpvPlayer: EOF reached at 156.2/156.6s
    MusicQueue: Next track 1414 - Westbound Sign

while every timeline report for that track said `duration=203000`. Both figures
come from the stream being read, so a truncated stream shrinks the duration to
meet the position — they agree *because* the stream broke.

`loadUrl` therefore takes the length the server reported, and the check prefers
it. Callers that do not know a length pass 0 and keep the old behaviour, which
is what live streams need.

### Direct play versus transcode

`getTranscodeUrl` asks the server for a decision, and the `/start` endpoints are
transcode endpoints: they answer 400 for a direct-play decision. You cannot ask
for an HLS playlist, or an mp3 transcode, of a file meant to be played as-is.

The direct-play branch was video-only for a long time, so audio fell through to
`start.mp3` and took that 400 — a track the server judged directly playable
simply would not start. Nothing about the request is platform-specific; what
differs is the decision returned for the track, which is why it looked like
"Linux versus Android".

Vita and PS4 opt out one step earlier by sending `directPlay=0` for audio: they
decode far less than mpv on a desktop, and the audio profile the client sends
declares only an mp3 transcode target, so there is nothing for the server to
judge direct-play capability by.

### Seeking a transcode

There is no seek endpoint. The whole documented transcode surface is `decision`,
`fallback`, `start.*` and `subtitles`; `offset=` on `start.*` is the only way to
move a transcode's start point, and that is by definition a new session.

Music uses `protocol=http`, a single progressive stream with no range support,
so mpv reports `seekable=no` and the only way to reach a target is to restart.
Video uses `protocol=hls`, whose playlist covers the whole item, so mpv can seek
inside it — which is why video seeks locally within 60 s and music does not.

Seeks are debounced ~350 ms so a burst of presses commits once. A jump before
the transcode's start, or far ahead of the play head, restarts; anything already
transcoded seeks locally.

### Transcode sessions are reaped

`/decision` opens a session, and Plex reaps one nobody has started streaming. A
device log caught a decision taken at 02:04:36 still being handed to mpv at
02:07:07 — answered 400, so the track after a long one would not play at all.
A prefetched entry older than a minute is discarded and resolved again.

### Prefetch

Resolving a stream URL costs two blocking round-trips (`/library/metadata`, then
`/decision`). Doing them when a track ends put both inside the silence between
songs and froze the UI thread for their duration.

The cache key is the pair (ratingKey, queue version): every queue mutation bumps
the version, so a reorder, add or remove simply makes the cache stop matching. A
cached entry with an empty URL records a *failed* attempt, so a track whose
resolve fails is not retried every second.

### `reset-on-next-file` restores, it does not reset

`reset-on-next-file=speed,pause` read as "every file starts unpaused". It does
not do that. mpv backs up each listed option when a file starts and restores the
backup when the file ends (`loadfile.c`: `m_config_backup_opt`, then
`m_config_restore_backups`; the same in 0.36 and master). The value restored is
whatever the option held when the file *started*.

So a pause that lands while nothing is loaded is captured by the next file and
handed on by every file after it. An Android log (build 1944) had one
`pause=true` at 16:23:16 with nothing loaded, then four songs in a row open
paused, each needing Play, with no pause command anywhere near them. Running
that sequence through libmpv 0.37 reproduces it: the next track starts paused,
and the one after starts paused again though Play was pressed in between.

`pause` is off the list now, and `loadUrl` sets it, and `start`, on every load.
That is also needed because `keep-open` pauses the player at the end of every
track; before, the restore was what undid that. A paused file is stopped before
it is replaced, so unpausing it first cannot let a moment of it out.

### Subtitle size

The Subtitle Size setting was saved and shown but never applied. Small, Medium
and Large are now 0.8, 1 and 1.25 times the default size
(`Application::subtitleScale`), multiplied by Android's own caption size where the
user set one. Medium therefore follows the system size exactly, as subtitles did
before. Subtitles reach the screen two ways, and the setting covers both:

- **mpv draws** the text subtitles it is given: embedded tracks, sidecars, and
  downloads, which embed theirs. They are sized by `sub-scale`. The mpv instance
  lives for the session once the first playback starts it, so the value is read
  when it starts and pushed to it again when the setting changes. In a host run
  with libmpv 2.2, an SRT line came out 146, 183 and 229 px wide at the three
  sizes, and changing the value during playback resized it in the frames after
  the change. Picture-based subtitles (PGS, VobSub) were not tried.
- **The server burns in** the subtitles the client cannot draw (`subtitles=auto`),
  and no client-side scale reaches those. They take `subtitleSize`, a
  percentage, on `decision` and `start.*`, which Plex's API documents for burned
  subtitles.

A missing `subtitleSize` key used to load as 0, which is Small. It now loads as
Medium, the default.

### Getting a track back after a lost connection

The same log has the music stopping twice while the phone lost the server for a
few minutes. Once the song had been fully read, played out, and the next one
failed to resolve (`Could not resolve hostname`). Once the stream broke at 3:34 of
4:09 and mpv's reconnects failed on DNS. Both times the connection came back on
its own, and nothing tried again until a track was skipped by hand.

`MusicController` now takes over from either failure, foreground or headless:

- It resumes from 2 s before where the audio stopped, or from the start of a
  track that never loaded.
- It checks the server with `GET /identity`, which needs no token and has no
  side effects. The checks run at 2, 3, 5 and 10 s, then every 15 s.
- Once the server answers, it resolves the track again at that point. A
  transcode is restarted on the server with `offset=`. A direct-play original is
  opened by mpv at the point (`start`).
- It gives up after 15 minutes without the server, but only after at least ten
  checks: the clock can run on while a Vita sleeps. It also gives up after three
  reloads that fail with the server answering, which is not a lost connection.
- While it waits, the OS session reads as playing. Pause calls it off, and Play
  starts it again from the same point. On Android, "playing" is also what keeps
  the CPU and Wi-Fi locks held for the checks.
- Next, a picked track, a video, or Stop drops it.

`"Lost connection to the server..."` shows when a check first fails. A lyrics
request that fails now says so, instead of "This track has no lyrics".

Why the connection went is a separate question that log could not answer.
Android now reports into the same log through `NetworkMonitor`:

- the default network appearing, going and changing;
- Android blocking this app's access (Android 10+);
- Doze, battery saver, Data Saver, and screen on/off;
- the app's restrictions at start.

### Android cut the app off, and curl kept the failure

A log from a Galaxy Z Fold6 (build 1961, on mobile data) answers that.

Android blocked VitaPlex's network 5 to 20 s after each trip to the background
(`MpvPlayer: detached Android surface`, then `access BLOCKED`). It allowed it
again as the app came back. A foreground service keeps an app on the network in
the background, and at first it did. At 07:44 the phone changed networks with
VitaPlex playing in the background, and access stayed allowed.

The music was paused at 07:45. At 07:58 access was blocked, with the screen on.
After that, the blocks also came with music playing: at 11:36 and at 11:39. The
log cannot say what happened to the service, since nothing logged it. Once the
service was gone, though, nothing would have started it again.
`MediaNotification.ensureService()` returned early while its flag was set. The
flag was set when it asked for the service, and only `clear()` reset it.

The song playing at 11:39 finished from mpv's buffer. The next one needed the
server's name looked up at 11:41:31, while Android blocked the app, and the
lookup failed. Every check after it failed in under 10 ms for the 2.5 minutes
the log ran on, including while access was allowed and the app was on screen.
That was curl, not Android:

- curl 8.16 caches a failed lookup as an entry with no address. Its
  `dnscache_entry_is_stale()` doubles the age of such an entry, so it is kept
  for half of `CURLOPT_DNS_CACHE_TIMEOUT`: 150 s at our 300.
- A cache hit fails at once, without asking the system or renewing the entry
  (`Curl_resolv`).
- The log ends 6 s before the entry would have expired.

This was replayed against libcurl 8.22 with a `getaddrinfo` that fails and then
recovers. Every request after the recovery failed in 0.0 ms, without a lookup.

Changes:

- `HttpClient`: after `CURLE_COULDNT_RESOLVE_HOST` for a host, its requests use
  `CURLOPT_DNS_CACHE_TIMEOUT` 0 until one gets past the lookup. curl then drops
  the entry and asks the system. When the transfer ends it also prunes the
  shared cache by that timeout, which clears it. In the same replay, the first
  request after the recovery succeeds.
- `MusicService` records whether it is in the foreground. `MediaNotification`
  starts it whenever it isn't, and again every time playback starts. A start
  while it runs only repeats `startForeground()`. Android 12+ refuses a start
  from the background unless the app is exempt: a tap on its media controls, or
  battery optimization off. A refused start is logged once, and retried the
  next time playback starts.
- vitaplex.log gets the service's starts, stops and failures. Each `access
  BLOCKED` or `allowed` line now ends with the app's process state, such as
  `foreground service 125` or `cached 400`.

The next log (build 1976) shows the service holding. It started on each play
(12:36:53, 12:36:58), and the song then played in the background with no block,
with the app at `foreground service 125`. Android does stop the service on its
own once nothing plays. Paused at 05:36, the app was at `background service 300`
by 06:06 and the service stopped at 06:09. After the queue ended at 12:41, the
service stopped at 12:55. The next play starts it again.

### A crash while typing in search

The same log ends mid-search, at 13:20:10, with no error. Each letter of "Day
dream" had started a library search and a Live TV search on threads of their
own, their answers came back out of order, and a native crash writes nothing to
this log. What crashed is not known. Three things changed:

- **How a run ended is now logged at the next start** (`ExitReasons`, Android
  11+). Android keeps a record of every process death: crash, ANR, low-memory
  kill, and so on, with the app's state and memory. For a native crash (Android
  12+) the record includes the tombstone, a protobuf. Its signal, cause and the
  crashing thread's backtrace are decoded with the field numbers from the
  framework's own `TombstoneProtos`. This covers a crash from before this
  shipped, while Android still holds the record. Java exceptions are now also
  written to the log as they happen (`CRASH:` lines).
- **`ImageLoader` could write into a freed `brls::Image`.** A reply was checked
  only against the caller's alive flag, which usually covers a whole screen.
  The player's queue deletes and rebuilds rows while its activity, and so its
  flag, lives on. A reply for a deleted row's image then reached
  `setImageFromMem()` on freed memory. Run against the shipped file, a host
  harness gets a heap-use-after-free from ASan at exactly that call.
  Each pending load now holds the image's own borealis deletion token, the one
  `ASYNC_RETAIN` uses. A reply is dropped unless it is still that image's current
  request and the image still exists. That also stops a late reply for a recycled
  view's old URL replacing its new picture.
- **A result card that had focus was freed with it.** Touching a card focuses
  it, and every answer rebuilds all the cards. borealis does not let go of a
  focused view when it is deleted: `View`'s destructor calls `giveFocus(nullptr)`,
  which `giveFocus()` ignores, so the next touch called `onFocusLost()` on the
  freed card. An activity opened from a card (the options menu a hold opens, the
  player, a detail page) keeps the card on borealis' focus stack instead, and
  closing it read the freed card in `popActivity()`. A host harness that builds
  the cards as search does and drives them through borealis' own touch path gets
  a heap-use-after-free from ASan at both. Whether this was the 13:20 crash is
  not known either. Search now moves focus out of the results before replacing
  them, and while another activity is on top it draws new results only once the
  tab is uncovered.

The same retry hook re-sent every thumbnail still missing after 0.9 s, up to
three times, even while its first request was still queued. Loads run one at a
time on borealis' single task thread, so on the "Day" search (136 results) that
queued 550 duplicate requests in three seconds. A load is now retried only
after its request fails.

---

## Platform notes

### Vita: GXM and NanoVG cannot overlap

`initRenderContext()` creates GXM resources and `loadUrl()` spawns decoder
threads that use the shared GXM context via `hwdec=vita-copy`. Both conflict
with NanoVG drawing during borealis' activity show phase, and the result is a
consistent SIGSEGV.

So MPV init is deferred in two phases: create the render context after the
transition completes, then schedule `loadUrl` via `brls::sync` for the *next*
main-loop iteration, so NanoVG draws one complete frame of fresh GXM state
before any decoder thread touches it.

### Android: uploads need a GL surface

`Image::setImageFromRes()` uploads there and then. With no GL surface — the app
backgrounded — the call silently does nothing and the icon stays blank, and the
OS notification can reach play/pause, shuffle and repeat while exactly that is
true. Losing the EGL context also invalidates textures the XML already loaded,
and `Image` keeps no copy of its path, so nothing can ask it to reload.

`m_iconRes` therefore remembers every icon's resource path: `registerIcons()`
seeds it from what the XML set, `setIconRes()` keeps it current, and
`reapplyIcons()` re-issues the lot when uploads become possible again. A swap
dropped while hidden and a texture lost with the context are indistinguishable
from there, and re-issuing a good icon only costs a cache lookup.

The mpv surface also composites *behind* the borealis frame and shows through
only where the frame is unpainted, so an opaque root hides the picture
completely — that was "video plays but the screen stays grey".

### Windows: the data directory

`getDesktopDataDir()` reads `$XDG_DATA_HOME`, then `$HOME`. Windows sets
neither — `HOME` exists only under MSYS or Git Bash — so every Windows build
fell through to `./VitaPlex`, relative to the working directory. Program Files
is not writable, and the working directory depends on how the app was launched.

It now resolves `%LOCALAPPDATA%\VitaPlex`, except that an existing `VitaPlex`
directory still wins so no one's settings move.

### Windows: progress is the taskbar button, and optionally the toast

`ITaskbarList3::SetProgressValue` draws on the taskbar button. That is the
native idiom for a download here — browsers and Steam do the same — it needs
nothing registered, and it is always available. It carries no text, though.

The text goes in a toast, and Windows solves the "one popup that updates"
problem the opposite way round from freedesktop. Rather than replacing a
notification by id, the toast is posted **once** with a `<progress>` element
whose fields are data bindings (`{progressValue}`, `{progressValueString}`,
`{progressStatus}`), and later values are pushed with
`IToastNotifier2::UpdateWithTag` against the toast's tag. Nothing is re-posted,
so there is no id to lose and no way to accidentally spawn a second popup.

`UpdateWithTag` returns `NotificationNotFound` once the user dismisses the
toast. Re-posting there would be the Linux "closing it reopened it" bug in
Windows dress, so that result is taken as final for the run and only the
taskbar bar continues.

`NotificationData`, `IToastNotification4::put_Data` and `IToastNotifier2` are
newer than the base toast interfaces and are absent from older mingw-w64 header
sets, so they sit behind their own `try_compile` probe
(`VITAPLEX_HAVE_TOAST_PROGRESS`). The probe deliberately mirrors the call
sequence in the source, so a probe that compiles means the real code compiles.
A gap there costs only the text: the taskbar bar and the completion toast are
unaffected.

The interface number matters, and getting it wrong is why Windows shipped for
a while with no progress toast. `put_Data` is on `IToastNotification4`;
`IToastNotification2` carries only `Tag`, `Group` and `SuppressPopup`. Asking 2
for `put_Data` made the probe fail, and that failure looked exactly like the
header set lacking the API — so the diagnosis was "mingw-w64 does not ship it"
when the headers had it all along. A probe that fails for a reason inside your
own code is indistinguishable from one that fails for a reason outside it,
which is worth remembering before blaming a toolchain.

### Windows: portable and installer, both per-user

The zip is a portable folder and stays supported. The NSIS installer is the same
payload plus a Start Menu entry, an Add/Remove Programs record and an
uninstaller.

It installs **per-user**, into `%LOCALAPPDATA%\Programs\VitaPlex`, and that is
not about avoiding a UAC prompt for its own sake: the app updates itself, and an
app under `Program Files` cannot rewrite its own exe without elevation or a
privileged helper. Per-user means no prompt at install, at update, or at
uninstall. Uninstalling deliberately leaves `%LOCALAPPDATA%\VitaPlex` alone —
an uninstall is not a request to delete a downloaded library.

The installer does not stamp the AppUserModelID onto the shortcut; that needs an
NSIS plugin absent from stock runners. The app does it at first run instead,
which also repairs a shortcut a user made by hand — the case that silently broke
toasts, since Windows finds such a shortcut, cannot attribute the app, and drops
every toast without a word.

### Windows: toasts need a Start Menu shortcut

Windows will not show a toast from an unpackaged app unless a Start Menu
shortcut exists carrying the same AppUserModelID the process declares. That
shortcut is the only thing this integration writes to the machine, which is why
it is a setting; with it off, notifications fall back to flashing the taskbar
button.

`propvarutil.h`'s `InitPropVariantFromString` is an inline that calls
`SHStrDupW`, so **shlwapi** is a link dependency of the shortcut code even
though nothing names it.

### Linux: two ways to show progress, and no desktop has both

The launcher progress bar is `com.canonical.Unity.LauncherEntry`, a Unity-era
protocol. KDE Plasma and the Ubuntu dock implement it; **Cinnamon, stock GNOME
and most tiling setups do not**, and since it is a bare signal nothing reports
that it went nowhere. On those desktops the notification is the only thing the
user sees, which is why it carries the text rather than leaving a bar to speak
for itself.

The progress notification is a single popup that updates in place, and getting
that right on a slow desktop took three goes.

`Notify` returns the id it assigned, and every later update must pass that id
back as `replaces_id`. The first attempt asked for it with
`dbus_connection_send_with_reply_and_block` and a 300 ms timeout. That is wrong
twice: it blocks the UI thread on a service that may be slow — on Cinnamon the
daemon lives inside the GJS shell process — and when the reply misses the
timeout the id stays 0, so every following tick posts a *new* notification.
One slow first reply and the user gets a popup a second for the whole download.

So the opening `Notify` is sent asynchronously and its reply polled from the
same once-a-second tick. Until the id arrives, later ticks are **dropped rather
than posted**, because posting them is exactly what creates the extra popups. If
the reply never comes, that latches and the progress notification is abandoned
for the run — the launcher bar carries on. Worst case is a stale popup; it is
never a stream of them.

Polling needs care. `dbus_connection_read_write_dispatch` dispatches only one
message per call, and the bus queues its own `NameAcquired` signal ahead of the
reply, so a single call collected nothing and the id took three ticks to appear
even from an instant daemon. Read once with a small bounded wait, then drain
with `dbus_connection_dispatch` until the reply lands or the queue empties.

Dismissing the popup destroys it, so the next `replaces_id` names nothing and
the daemon makes a *new* one — the notification appears to refuse to go away.
The backend watches `NotificationClosed(id, reason)` and latches on reason 2
(dismissed by the user), after which nothing more is posted for that run. Reason
3 is our own `CloseNotification` at the end and must not count. The signal only
arrives if something dispatches the connection, so the progress tick drains it
every second whether or not a reply is outstanding.

The progress popup carries the `transient` hint: it is a live indicator, not a
record, and should not pile up in the shell's notification list. Only the
completion notice is worth keeping there.

This is verified against a real session bus (`dbus-run-session`) and a stub
notification service that logs the `replaces_id` of every call, at daemon reply
delays of 0 ms, 800 ms and 3 s, plus a run where the stub emits
NotificationClosed part-way. In all of them, exactly one progress popup, and
after a dismissal nothing further is posted.

`expire_timeout` is 0 — until dismissed. A progress popup that expires after a
few seconds and returns a second later is worse than none.

The `value` hint (int32, 0-100) is not in the freedesktop spec, but it is the
long-standing convention for a progress bar inside a notification and is read by
Plasma, Xfce and dunst. A daemon that does not know it ignores it and shows the
text, so it costs nothing to send. `urgency = 0` keeps a download from
interrupting anything.

### Linux: Flatpak name ownership

The default session-bus policy lets an app own `$FLATPAK_ID` and its subnames.
`org.mpris.MediaPlayer2.vitaplex` is neither, so `RequestName` was refused and
no media controls appeared. GLFW derives WM_CLASS from the window title, which
is why `StartupWMClass=VitaPlex` matches `createWindow("VitaPlex")`.

### Vita: notifications yes, background downloads no

`SceNotificationUtil` gives the real thing — the system popup, an entry in the
notification list, and via `sceNotificationUtilProgressBegin/Update/Finish` the
same BGDL progress notification a system download draws. The header settles the
one question that would otherwise need a device: `sceNotificationUtilBgAppInitialize`
"does not need to be called for normal applications", so a plain VPK can drive
the progress form without being a registered background app.

Two shapes in that API are load-bearing and easy to get wrong.
`sceNotificationUtilSendNotification` wants a **0x410-byte buffer**, not a
pointer to a short string. The progress structs cap text at
`SCE_NOTIFICATIONUTIL_TEXT_MAX` (63) UTF-16 units, each array followed by a
separator that must be zero — so zero-initialise and truncate, and the
separator doubles as the terminator. Track titles exceed 63 units routinely, so
truncation is the normal path, not the error path.

What this does **not** do is keep downloading once VitaPlex is suspended, and
that gap is not closeable from here. Better Homebrew Browser manages it, and
looking at how is what settles the question: it ships `bhbb_dl`, a taiHEN
`.suprx` plugin linking `ScePaf*` and generating stubs for **`SceLsdb`** — the
system's own download-list database — built with `VDSuiteSignElf` and
`VDSuiteCreateStubs`, which are Sony's official SDK tools. Its downloads are
not an imitation of the store's; they are entries in the store's actual queue.
Matching that would mean a proprietary toolchain, a kernel plugin, and a
different distribution model for an Apache-2.0 repo. Notifications are the part
that is reachable with open vitasdk, so notifications are the part we do.

### Vita: background audio (an experiment, not yet settled)

The "Background Music" setting covers leaving the *player screen*; leaving the
app, for the LiveArea or another app, still stops the music. ElevenMPV-A plays on
in that situation, and it gets there two different ways, both reachable with
vitasdk. `utils/background_audio.hpp` tries each on real hardware; Settings >
Music carries the controls, and every step is logged under `[bgaudio]`.

**The BGM port.** mpv's Vita audio output (switchfin's `ao_vita`) opens
`sceAudioOutOpenPort(rate > 47999 ? MAIN : BGM, 1024, rate, …)`: read from the
disassembly of the libmpv the CI links, so music, nearly always 44.1 kHz, is on
the BGM port already. vita-headers documents `sceAppMgrAcquireBgmPort` as
obtaining that port "even when it is not in front". ElevenMPV-A does exactly this
for the formats it decodes in its own process (FLAC, Opus, its YouTube streams):
it acquires at priority 0x81 and outputs on a BGM port. Its main loop also keeps
running after `SCE_APP_EVENT_ON_DEACTIVATE`, where it merely lowers its own
priority, which is evidence that a deactivated app is not frozen as a matter of
course. One difference is untested: ElevenMPV-A's `param.sfo` says `CATEGORY`
`gdc`, a non-game application, where VitaPlex is a game (`gd`). If this route
works, it streams exactly as the app does now, with nothing to cache.

The toggle "Keep Playing Outside VitaPlex (test)" acquires the port at 0x81 when
music plays and has mpv resample to 44.1 kHz so 48 kHz music lands on it too. A
watcher thread reads the AppMgr events, which nothing else in the app does. It
logs a heartbeat every 4 s while the app is away, with the mpv position, how many
times the UI loop ran, and how many files mpv loaded. A tick that comes late by
more than 3 s means the process was held. On return a notification gives the
verdict: how long the app was away, whether VitaPlex ran for it, and whether the
track or the queue moved. The queue advances from the UI loop, so "UI stopped"
there means the current track may finish but the next will not start.

**SceShell's music service.** libShellAudio (`lib/libshellaudio`, MIT) drives
the service the system Music app plays through, so audio handed to it outlives
anything that happens to the app. The object `sceShellSvcGetSvcObj` returns is
an IPMI client. Vita3K's client vtable puts `invokeSyncMethod` at 0x14, the slot
libShellAudio calls, and its "events" are the service's method ids. vitasdk has
no stub for that one function; `shellsvc_stub.S` supplies it, checked against
the 3.60 firmware's exports and Vita3K's NID table. The shell decodes only MP3,
AAC/M4A, WAV and ATRAC9, and ElevenMPV-A sends it local files only; its streams
go through its own decoders and the BGM port. Whether the shell will open an
http(s) URL was unknown, and that is what "Test: System Player, Streaming" is
for; how it grew from build to build is below. "Test: System Player, Local
File" plays a download or a `test.mp3` on the picked service, so a service that
plays nothing at all can be told from one that plays no links.

What each outcome leads to:

- VitaPlex keeps running with the port held: this becomes the feature. The
  test label goes, the port is released when paused in the background (as
  ElevenMPV-A does, so the system Music app can take over), and the power button
  gets looked at.
- The process is held but the shell plays a URL: streaming goes to the shell,
  with the queue handed over one track at a time while the app is in front.
- The process is held and the shell plays only files: the current and next few
  tracks are cached as MP3 while the app is in front and handed over from disk.
  Advancing past them without the app running would need the shell's track list
  (`sceMusicPlayerServiceSetTrackList`, an undocumented 0x828-byte SQLite
  command buffer), so that part would stay out of reach.

**What the console showed** (a PS Vita, logs `ea96fd9a` and `01daccd2` on the
first test build, `3bba2674` and `e764091b` on the second):

- The shell plays a local file (a downloaded WAV). It carries on when VitaPlex is
  frozen, and when VitaPlex is closed by starting a game. The quick menu's Music
  controls stay dimmed for it, with play showing ▶ while it plays, at client
  type 0.
- The shell did not open **Plex's transcode link**. `Open` returned 0 for https
  and plain http alike and changed nothing, so PLAY resumed whatever file it
  had before. It keeps that file through stop, terminate and a fresh
  initialize, too. The first test build took the moving clock as the stream
  playing, a false positive the user caught by ear ("it keeps playing the old
  download"). The test now counts only a new source that replaced the old one:
  a different length, or the clock starting again from the top.
- The first build never tried the plain file: it read the format from
  `MediaItem::audioCodec`, which `fetchMediaDetails` never fills, so it always
  skipped, though the track was an MP3. The second build took the format from
  the part's extension, asked the server about each link with VitaPlex's own
  client, and ran a control: VitaPlex served a file the shell had played from
  disk on the plainest possible link (`http://127.0.0.1:port/…/track.wav`, with
  a length and range support), fetched from it itself, and logged every request.
- **Music player, type 0** (`3bba2674`): no request reached the control, on
  127.0.0.1 or on the Wi-Fi address, while VitaPlex's own fetch got a 206. The
  server answered Plex's MP3 file over http and https (206 `audio/mpeg`), and
  the shell switched to neither.
- **Application-BGM service** (`e764091b`, `SetUri`): no request reached the
  control either, and this time it served an MP3 (a download), on 127.0.0.1 and
  the Wi-Fi address. Plex's file over http and https gave no progress. This
  service reports no clock (`GetLastResult` stays at state 0, 0:00). Its
  playback status reply holds pieces of the URI that was set, at the URI's own
  offsets, so most of that reply is a buffer the shell leaves unwritten; its
  layout is unknown and nothing is read from it. Whether this service played
  even a file from disk was never measured.
- VitaPlex is **frozen** the moment PS takes it out of the foreground, with mpv
  playing and the BGM port held at 0x81. The watcher's first tick back was
  7.4 s late, the time away. No `ON_DEACTIVATE` or `ON_ACTIVATE` arrives, only
  `ON_RESUME` on return. ElevenMPV-A handles those two events. The power
  callback does see the PS button press itself.
- Plex's MP3 transcode is 48 kHz, so on the Vita mpv plays music on the **MAIN**
  port, not the BGM port assumed above. The 44.1 kHz resample only applies when
  mpv starts with the setting already on.

**What ElevenMPV-A's source shows.** It is the one other app that plays through
this service, written by libShellAudio's author, and it never hands the shell a
link. `GetDecoderType` sends any `https://` path to its own `YoutubeDecoder`,
which decodes in its own process onto a BGM port at 0x81; the shell only ever
gets local MP3 and AAC files (`ShellCommonDecoder`). Its quick-menu controls are
not the shell's either. It ships a SceShell plugin (`ElevenMPV-A-ShellPlugin`,
loaded by taiHEN) that finds the quick menu's Music widgets by hash (rewind,
play, fast-forward and two text lines), registers its own callbacks on the
buttons that message the app over a message pipe, and writes the title and
artist in. The plugin also hooks SceShell's import of
`sceAppMgrGetCurrentBgmState2` to cap the BGM port priority it sees at 0x80.
So dimmed controls are what a third-party source gets without code running
inside SceShell. ElevenMPV-A puts that code there itself: the plugin ships in
its package (`module/shell_plugin.suprx`), and at start the app loads it into
SceShell (`taiLoadStartModuleForPid` on NPXS19999's process), which any Vita
with taiHEN allows. The plugin is written against Sony's PAF UI framework, for
which vitasdk has no headers. Code in SceShell also keeps running whatever
happens to the app that loaded it, which is a route to background playback in
its own right, and an untried one here.

**The last test of links** ("Test: System Player, Streaming", third build)
closes what the two controls left open: the WAV, and four services never tried.
It downloads the first 2 MB of the current track as an MP3 (the file itself
when it is an MP3, else the transcode, since the start of an M4A need not play
without its index). Then on every service, the picked one first:

1. From disk: a copy of the MP3 under a name of its own for that service, so one
   service having opened it cannot count for the next. The clock runs to 3.5 s,
   so the next attempt can tell a new source by the clock going back. A first
   try that cannot tell (what was loaded before is as long and had not played
   3 s) is repeated once.
2. The same MP3 as a plain http link to VitaPlex's own server, and as an https
   link to it. The server has no certificate, so https cannot play, but it
   counts connections as they are accepted and recognises a TLS handshake
   (a first byte of 0x16), so "began https" is seen too.

Plex's own links are tried only on a service that fetched or began one of
those, since on any other they cannot work. The BGM port's state
(`sceAppMgrGetCurrentBgmState2`: owner, priority, state) is logged at every
attempt as a second reading that does not depend on the service's own status.
The report says, per service, whether it played the MP3 from disk and whether
it connected over http or https.

So the in-process route fails as VitaPlex is packaged, and on present evidence
the shell route covers local files only. What was left to try was the packaging
itself. vita-mksfoex names `ATTRIBUTE_BG_APP` (0x04000000), which neither
VitaPlex nor ElevenMPV-A sets, and ElevenMPV-A is a non-game application
(`gdc`). `src/bgtest_psv` is one beeping binary packaged as VitaPlex is, as
VitaPlex plus `BG_APP`, and as ElevenMPV-A is. On the console (logs `cfbcf9a7`,
`778d6cd7`, `3aa821e5`):

- As VitaPlex is (`VPLXBGT01`): frozen, 10.9 s with no tick, then only
  `ON_RESUME`, as VitaPlex itself.
- With `BG_APP` (`VPLXBGT02`): no line after the PS press, and not heard
  (the user heard only `VPLXBGT03` in the background).
- As ElevenMPV-A is (`VPLXBGT03`, `gdc`): **kept running**. It got
  `ON_ACTIVATE` at start and `ON_DEACTIVATE` at the PS press, then logged "alive"
  every 5 s, with the audio still going and heard, until it was closed.

So a non-game app plays on in the background; a game does not, flag or no flag.
What that allows for VitaPlex depends on what such an app is given. ElevenMPV-A
reads its memory budget and plans for 17 to 33 MB, where VitaPlex runs on a
172 MB heap. The test app now logs its budget (`sceAppMgrGetBudgetInfo`), what
is free, and the largest blocks it can really allocate, and asks the Plex
server in VitaPlex's settings for `/identity` over plain http every 5 s, in
front and away. A fourth package (`VPLXBGT04`) is the non-game app with
VitaPlex's extended memory (`ATTRIBUTE2=12`). Those numbers decide between
VitaPlex itself as a non-game app and a small player app beside it.

They came back (logs `9462d9ec`, `babbea68`): `sceAppMgrGetBudgetInfo` is
refused to these apps (0x8080201C), but what they could really allocate was
measured directly:

| Package | Main memory free | Largest block | CDRAM | Phycont |
|---|---|---|---|---|
| `VPLXBGT03` (gdc) | 231 MB | 233 MB | 89 MB | 26 MB |
| `VPLXBGT04` (gdc, `ATTRIBUTE2=12`) | 340 MB | 342 MB | 89 MB | 26 MB |

Both asked the Plex server for `/identity` every 5 s and got HTTP 200 in front
and away (21 to 261 ms), and both logged "alive, away" with the audio still
going. So the non-game package has room for VitaPlex's 172 MB heap with the
extended memory setting, and NanoVG's textures sit in main memory unless
told otherwise, so the 89 MB of CDRAM holds only framebuffers and ring
buffers. `VitaPlexBG.vpk` is therefore VitaPlex itself, the same eboot and
files, packaged as `VPLXBGT04` is, under its own title id (`VPLEX0002`) so it
installs beside VitaPlex. As a non-game app it gets the activate and
deactivate events, so the watcher's report on return ("Away 1:00. VitaPlex
kept running...") applies to it unchanged.

**VitaPlex BG works** (log `a93baa71`, build 1954). Sent away with PS 30.9 s
into a track streamed from Plex (through the server's remote address, as it
happened), it logged "away" every 4 s for almost five minutes with mpv playing
and its position moving, the UI loop running, and the timeline reported to
the server every 10 s. When the track ended it loaded the next from the play
queue, a fresh transcode, and played that too, all while away. Two things
remain:

- It ignored the system's request to quit. The user opened another app and
  confirmed the prompt to close VitaPlex BG; the request arrived, and it went
  on playing. A game is never asked, so VitaPlex never answered it. That turned
  out to hang the console (log `3ea218f5`, build 1957): asked to quit at
  17:29:09, VitaPlex released its audio port, so the music stopped, and then
  ran on for four minutes. Meanwhile the app the user had opened sat on its
  loading screen with its theme music playing, nothing else would open, and
  the console had to be held off with the power button. The test apps and
  ElevenMPV-A call `sceKernelExitProcess` on this request, and closing those
  the same way caused none of it. VitaPlex now quits too, the normal way
  first. The watcher runs whatever the background playback setting, since it
  is the only thing that reads this request; with the setting off it does
  nothing else. On the console (log `280b1532`, build 1964) the request came
  2.5 s after PS, VitaPlex was gone 1.2 s later, and the other app opened.
  That shutdown rewrites `settings.json` in place (at 1.14 s there), so
  ending the process during it could lose the login. The watcher therefore
  ends the process itself only after 2 s if the UI thread has not taken the
  request, which means it is stuck, and after 10 s if it has.
- Sleep (the power button) turns off Wi-Fi, so the stream stops once mpv's
  buffer runs out, about 30 s in that log.

Both test apps were asked to quit a few seconds after being sent to the
background a second time. That was the user: opening another app brought up
the system's prompt to close the running one, and they confirmed it. So a
non-game app plays on in the LiveArea, but opening an app it cannot run
beside closes it, with the user's say-so. Which apps those are (a game, other
homebrew, system apps) is the next thing to note. If games are among them,
the current song could be handed to the system player on the way out, which
does play on during a game, though only that one song.

The third run of the stream test (`668ce811`) went only as far as music player
type 0, because PS was pressed twice during it. The http link got no connection
again. It also showed two faults in the test, both fixed: the first clock
reading after an open can still be the old file's, stopped (state 2 at 0:08,
then state 1 at 0:00), which the test took for the new file's start and so
reported "clock stopped" for a file that played (the length changed from 1:19
to 3:58, so it had loaded); and an attempt VitaPlex is held through shows
nothing, so it is now noticed (a poll seconds late) and made again. The BGM
state call is refused to an app (0x8080201F), so it is no longer logged after
that.

The fourth run (`40f14e4c`, build 1964) went through all six services
uninterrupted, and settles it: **the shell plays files, not links.** Music
player types 0 to 4 each played the MP3 from disk (the clock ran 0:00 to 0:03)
and answered every link, http and https to 127.0.0.1 and, on type 0, http to
the Wi-Fi address, with open 0 and play 0 while going on with the file they
already had. The application-BGM service accepted the file and both links,
reporting no progress for any. The control server logged one request in all,
VitaPlex's own self-check, so no service so much as connected; Plex's own links
were therefore not tried. Music through the shell, which is what plays on
beside an app with extended memory, has to come from a file on the memory card.

**Playing while a game runs.** VitaPlex BG, and the test apps it was modelled
on, run on the game budget, which an eboot without a boot param gets:
`VPLXBGT03` and `VPLXBGT04` could allocate 233 and 342 MB of it. That amount is
set when the app starts, and freeing memory inside the app does not hand it
back. SceAppMgr's exports (vita-headers' NID table for 3.60) have
`sceAppMgrGrowMemory` and `sceAppMgrGrowMemory3` and nothing that shrinks. So
VitaPlex cannot drop to a small footprint when it leaves the foreground and
take its full memory back on return.

ElevenMPV-A is the other kind of app. Its eboot carries a boot param (SELF
control info 6, `ebootparam.bin` in its source) of attribute 2, no physically
contiguous memory and a 16 MB budget, which `vita-make-fself` documents as a
system-mode app (0x1000 to 0x12800 KB, where a normal app has 0). Its PAF
loader (`libScePafPreload`) then asks `sceAppMgrGrowMemory3` for 41 MB more, or
failing that 16 MB more, and the app sizes its texture pools from
`sceAppMgrGetBudgetInfo`. vitasdk's vita2d has a path for such an app: when
`sceAppMgrGetBudgetInfo` succeeds, which it does not on the game budget, it
starts GXM with `sceGxmVshInitialize` and a 2 MB parameter buffer (16 MB
otherwise) and draws into the shell's shared framebuffer.

`MEMSIZE 0x4000 ATTRIBUTE 2` on `vita_create_self` writes the same boot param:
the eboot's control info 6 matches ElevenMPV-A's 256-byte `ebootparam.bin`
byte for byte. The background test has a system-mode build that carries it,
packaged as `VPLXBGT05` (16 MB as it is) and `VPLXBGT06` (grown at start as
ElevenMPV-A grows). It has no screen, since vita2d's GPU buffers come to about
10 MB in that mode, and START does not quit it, since it is meant to run while
a game is in front. It logs its budget every 5 s, so memory the system takes
back when a game starts would show. If a system-mode app plays on through a
game, music during games takes two apps: VitaPlex with its full memory for
browsing and video, and a small player that does the playing and outlives it.
`VPLXBGT06` says whether that player can have 57 MB or only 16.

**A system-mode app plays through a game** (build 1968). Both test apps started
in system mode: `sceAppMgrGetBudgetInfo` answered them (mode 2; 16 MB of main
memory, 10 MB free; no physically contiguous memory; 16 MB of CDRAM, none
free). `VPLXBGT06`'s `sceAppMgrGrowMemory3(41 MB, 1)` returned 0 and raised its
budget to 57 MB, 51 MB of which it could allocate in one block, so the budget
is a real limit (log `46aa6630`). Both asked the Plex server for `/identity`
every 5 s, in front and away, and got HTTP 200.

`VPLXBGT05` kept beeping while LittleBigPlanet, a retail game, started and
reached its hub (log `b6e6f6f5`). Its log covers 88 s away, with "alive" and
the audio clock every 5 s and HTTP 200 from the server every 5 s throughout.
The budget stayed at 16 MB with 10 MB free. Starting the game asked to close
VitaShell, a game-budget app, and named nothing else. In an earlier run with
VitaShell open (`c801b94e`), `VPLXBGT05` was asked to quit 4.3 s after PS,
while the user started the game. In this run VitaShell was closed first and
nothing asked. `VPLXBGT06` beeped while a game loaded; whether it lasts into
the game is not yet known. Both logs end mid-run, with no quit request.

Opening VitaPlex silenced the beeps (`VPLXBGT06`, log `8098b625`), though the
test app ran on: "alive" every 5 s, the audio clock moving with the time, HTTP
200 from the server, and no quit request until the user closed it. VitaPlex
makes no audio call when it starts: mpv starts with the first playback, and
the BGM port is acquired only while mpv plays with the setting on. Its only
system call at start near this is `sceAppUtilInit`, the standard set-up for
save data and system settings. So if VitaPlex takes the music over when it
opens, the system does it on VitaPlex's behalf. The test apps now log
whether their BGM output is adopted and its private gain
(`sceAudioOutGetAdopt`, `sceAudioOutGetPortVolume_forUser`) whenever either
changes, to see whether the beeps are muted through one of them.

**Extended memory takes the BGM output** (build 1971). With `VPLXBGT05` beeping
in the background, the beeps went quiet while `VPLXBGT07` (a silent game with
`ATTRIBUTE2=12`, as VitaPlex is) or VitaSurf (another homebrew with extended
memory) was in front. They went on beside `VPLXBGT08` (the same silent game
with `ATTRIBUTE2=0`), VitaShell and LittleBigPlanet. `VPLXBGT07` had the BGM
output adopted for it (`sceAudioOutGetAdopt` 1) a moment after it started,
without opening a BGM port, and `VPLXBGT08` did not (0) (logs `f794d228`,
`b25e8414`). In the run `VPLXBGT05`'s log covers (`42dfdc2b`), its own
adoption (1) and gain (256) stayed as they were. This vitasdk's vita-mksfoex
names 12 `ATTRIBUTE2_MEM109` and uses it by default; `MEM29` (4) and `MEM77`
(8) are the smaller steps. The system Music app played on beside VitaSurf, so
the shell's own music is not silenced this way. `VPLXBGT09` is `VPLXBGT07`
giving the output up with `sceAudioOutSetAdopt_forUser` whenever it is
adopted. If the beeps come back beside it, VitaPlex can keep its memory and
do the same.

**A player through the shell** (`VPLXBGT10`, `src/bgtest_psv/player_test.c`).
The shell plays files and plays on beside extended memory, so the other route
is a small system-mode app that downloads songs and hands them over. The test
app does that once it is sent away. It picks two songs of 1 to 4 minutes from
the first music library. It downloads the first as the server has it, when the
shell decodes that (MP3, AAC, WAV), and hands it to the shell. Then, while the
first plays, it downloads the second as the server's MP3 and hands it over
when the first ends. It logs each download's speed and whether it started and
ended in the background. A song counts as playing once the shell's clock moves
with the length the server gives it, which tells it from an old file the
shell goes on with. Requests go over https as VitaPlex makes them (curl,
mbedtls, the CA bundle), the token from VitaPlex's `settings.json` is never
logged, and on the way out the app stops the shell's playback. A host harness
with fake responses and a scripted shell exercised the choice of songs, both
hand-overs and the end detection, the shell ignoring an open included.

**The shell played a downloaded song through a game** (log `468b487f`).
`VPLXBGT10` was sent away with PS 82 s after it started. While away, it
downloaded the first song (the original M4A, 3.07 MB in 2.5 s, 1283 KB/s). It
then handed the song over in four steps 5 s apart: the BGM port at 0x80,
initialize, open, play. Each returned 0, and no quit request followed any of
them, so the one in `7a912d67` was not an answer to a step. The shell played
the song at the server's length (3:23) while the app downloaded the second
song in the background as the server's MP3 (4.41 MB in 4.8 s, 948 KB/s). The
user heard the music go on through LittleBigPlanet, and when VitaSurf, which
has extended memory, was opened. The 16 MB budget kept 3 MB free with curl,
mbedtls and both downloads.

The BGM output was adopted for the app when it took the port, and stopped
being adopted once the shell played. The system asked the app to quit 10 s
after the third PS press, 1:21 into the song. The app quit and left the song
to the shell. The log does not say which app asked, and the second hand-over
never came. The user saw no prompt to close the test app, in that run or the
next: the prompts were for other apps. So the system asked it to quit
without one.

**The second hand-over works, and the server's MP3 has no length** (log
`1a1cef7f`). The first song played to its end (state 2 at 0:00, once it
reached 3:22 of 3:23). The app handed the second over at once, during the
game, and every call returned 0. The shell played it: state 1, the clock from
0:00 to 0:14, and the user heard it. But the shell gave that song as 19:17
long, where the server says 2:51, so the test took it for some other file and
stopped it after 15 s. Nothing asked the app to quit in this run.

19:17 is that file's 4.41 MB read at 32 kbps, MP3's lowest bitrate. That is
how a player works out the length of a variable-bitrate MP3 with no Xing,
Info or VBRI frame giving its frame count, when the first frame is at
32 kbps. On a host, ffmpeg writing VBR MP3 to a pipe, as a streamed transcode
has to, left that frame out; written to a file, it put one in. For a 2:51
file starting with a second of silence, the first frame's bitrate gave 9:21
and ffprobe estimated 5:34, while the file with the frame read 2:51.

So the test now logs, for each MP3 it downloads, the ID3 tag, the first
frame's bitrate, any length header, and the frames' real count and length
(`mp3_info.c`). It hands the shell a copy of the server's MP3 with a Xing
frame in front, giving that count, and without the ID3 tag. On the host that
copy decoded to the same audio, byte for byte, and ffprobe read its length
exactly. A length that is neither the server's nor the last song's now counts
as the new song as well.

**With a length header, the shell has the right length** (log `e822493b`).
The server's MP3 of the second song, as the console downloaded it, was a
45-byte ID3 tag, then 6577 frames at 32 to 320 kbps, making 2:51. The first
frame was at 32 kbps, and there was no Xing, Info or VBRI header. From that
first frame alone the song is 19:17 long, the length the shell gave it in
`1a1cef7f`. The copy with a Xing frame in front went to the shell as 2:51, the
server's length. It played during the game to its end: 2:50 of 2:51, then
state 2 at 0:00, and the user heard all of it. About 0.6 s passed between the
first song's end and the second's start. A player handing the shell the
server's MP3 has to write that header first.

The quick menu's Music controls stayed dimmed, as they are for any source but
the system's own without a SceShell plugin (ElevenMPV-A, above). Its music
volume did change the shell's playback.

**Only VitaPlex BG has the tests.** `VitaPlex.vpk`, the package that is
released, is the same eboot as `VitaPlexBG.vpk`, so its settings had the toggle,
the service picker and the three tests too, and a toggle saved on would have held
the BGM port there. The eboot now reads the title id it runs under
(`sceAppMgrAppParamGetString`, param 12) and offers them only as `VPLEX0002`,
the id CMake gives VitaPlex BG. As `VPLEX0001` the toggle counts as off,
whatever was saved. The watcher still starts in both, as the one reader of the
request to quit.

**Handing a song over** (`VPLXBGT11`). BG Test 10 showed the helper's half
working: downloads in the background, and songs through a game, back to back,
at the right length. A player built that way also needs VitaPlex to start the
helper and hand it the music. VitaPlex BG's settings have a button for that,
"Test: Hand This Song to BG Test 11". It pauses the song, noting where:
mpv's clock plus the stream's start offset, since mpv counts from 0 again once
a seek has restarted the transcode part-way in. It lets go of the BGM port. It
writes the song, the position and the queue's next song to
`bgtest11-handoff.txt`, then launches `VPLXBGT11` with
`sceAppMgrLaunchAppByUri`: 0x20000 first, the one value vita-headers says does
not open the LiveArea, else 0xFFFFF. VitaPlex's watcher logs whether VitaPlex
was deactivated, and so whether it stayed in front.

`VPLXBGT11` is BG Test 10's eboot under another title id. It reads the file
once and removes it, then looks both tracks up (`/library/metadata/<key>`). It
downloads each as the server has it when the shell decodes that, else as the
server's MP3 behind a length header. It hands the first over at once, without
BG Test 10's 5 s between steps, since no step was answered (`468b487f`). Once
the shell plays it, the helper seeks to where VitaPlex paused it. First it
tries `sceMusicPlayerServiceSetSeekTime` and then the seek event (0x11), as
`ShellAudio.h` documents them, and gives that 8 s. If that fails it tries
`sceMusicPlayerServiceSeek`. A seek counts only if the clock gets further than
playing alone would take it. So a song paused in its first 10 s, or its last
10 s, starts from the beginning. The next song follows as in BG Test 10. The
helper quits when they are over, because it reads a hand-over only as it
starts. On a host, the whole run went through against a scripted shell and
canned server replies: with the seek event working, with only the seek call
working, with neither, and with no next song.

What the console has to say: whether VitaPlex BG, a non-game app on the game
budget, can launch a system-mode app at all, and stay in front, or is asked to
close (as `VPLXBGT03` and `VPLXBGT04` were when another app opened). Also
whether either seek moves the shell's clock. VitaPlex itself is a game, so a
working hand-over from VitaPlex BG still leaves VitaPlex's own launch to try.

**The helper takes VitaPlex BG's place** (logs `0bbfcb94` and `6f40de92`,
build 1988). The launch with 0x20000 returned 0, and VitaPlex BG was
deactivated 0.3 s later, behind the system's prompt to close it. Once the user
confirmed, 2.8 s after the launch, the system asked it to quit, and it shut down
cleanly. `VPLXBGT11` started 1 to 2 s after that and read a hand-over written
5 s earlier. It was activated, so it came to the front with nothing to show,
and stayed there until PS was pressed 7 s in. So VitaPlex BG cannot keep
running beside an app it starts this way, whatever the flag.

The rest worked. The helper downloaded the first song, the original M4A
(2.67 MB in 3.6 s), and handed it over 4.7 s after it started. The shell
played it from 0:00 within 0.5 s. The first seek, `SetSeekTime` and then the
seek event, took the clock to 0:23, where VitaPlex had paused, in 0.3 s. The
second song went over 26 ms after the first ended, at the server's length,
and played away from the app as in BG Test 10. From the button to the song
again took about 10 s. About 5 s of that was the prompt and VitaPlex BG
closing, and most of the rest was the download.

VitaPlex is a game, and starting another game closes it anyway, so a hand-over
that ends VitaPlex costs nothing to someone leaving to play one. Still worth
trying:
- VitaPlex quitting by itself right after the launch, as its updater does
  (`app_update.cpp`), to see whether the prompt then goes;
- the helper started first, since `VPLXBGT06` ran on when VitaPlex was opened
  (`8098b625`), with hand-overs going to it while it runs;
- seeking before play, which would avoid the half second from 0:00;
- the first song taken from a file VitaPlex already has, which would save the
  download.

**One app, two programs** (`VPLXBGT12`, `VPLXBGT13`). A separate helper is a
second app for the user to have, and starting it closed VitaPlex BG.
`sceAppMgrLoadExec` replaces a running app's program with another SELF from
its own package. If the system then treats the app by the program it runs,
one app could be both: VitaPlex in front, and the small player once you
leave. Two packages try it. Each holds the test build with a screen as
`eboot.bin` and the player as `player.self`, with its system-mode boot param.
`VPLXBGT12` is a non-game app with VitaPlex's extended memory, as VitaPlex BG
is, and `VPLXBGT13` a game with it, as VitaPlex is. X switches to the player,
which does what BG Test 10 does: once sent away, it plays two songs from the
library. Opened again after PS, it switches back to `eboot.bin`.

Both programs write one log. The one switching leaves a note with its start
and the time it asked, on the system clock, and the next carries the log and
its clock on from there. A host check ran the note through both programs'
code, and also notes that were too old or broken, which are ignored. The log
answers:
- what `sceAppMgrLoadExec` returns, and how long the switch takes;
- the player's budget: a system-mode app's 16 MB (mode 2), or still the big
  one;
- whether the player keeps running and the songs play when a game starts, or
  the system holds the app or asks it to close;
- whether `eboot.bin`, switched back to, can allocate its big memory again.

**The player did not start** (`VPLXBGT12`, log `4a8451a1`, build 1991).
`eboot.bin` had the game budget (342 MB in one block) and asked for
`app0:player.self`. `sceAppMgrLoadExec` had not returned when the system asked
the app to quit, 69 ms later, and the app quit 10 ms after that. That is how
the switch frees the app for the next program. Then nothing: the player logged
nothing, and the note it reads and removes first thing was still there,
written 13.2 s after the start, as the log says. Either the system did not
start the player, perhaps refusing its system-mode boot param in an app with
the game budget, or the player started without a title id and so found neither
the note nor its log. Two changes tell these apart:
- `screen.self`, a copy of `eboot.bin` in the same package, is switched to
  with O, as a control for the switch itself;
- the note is at one path for every app and carries the app's title id, which
  a program the system tells none takes from it. One that has neither logs to
  `bgtest-noid.log`.

The second build (log `bfab5b72`, build 1992) went the same way: the quit
request came 44 ms after the switch was asked for, and the app quit. Then the
system showed "An error has occurred. (C1-2697-4)", and the note was again
left unread. So the system itself refused to start `player.self`. The code is
not in vitasdk's documentation, and a web search found nothing on it. The
control, O to `screen.self`, was not part of that run.

The control failed the same way (log `8063b843`, build 1992). The quit
request came 220 ms after the switch to `screen.self` was asked for, the app
quit, and the copy never started, and the user saw the same error. So the
switch itself fails here, whatever it switches to. These test apps are unusual in one way: they quit as soon as the
system asks, and an app that switches would not usually read that event at
all. The third build therefore leaves a switch's quit request to the system
for up to 5 s before quitting. It also adds triangle, which switches to
`eboot.bin` itself: the commonest use of the call, an app restarting itself.
On a host, the real watcher with a faked clock and events quit at once when
no switch was asked for, and 5 s after the request when one was.

That made no difference (log `6dfdec3c`, build 1994). Triangle asked to switch
to `eboot.bin` itself, and the quit request came 37 ms later. The system did
not end the app in the next 5 s, and `sceAppMgrLoadExec` never returned. Then
the app quit, and the same error followed. So the switch fails even to restart
the same program. The user also reported that closing BG Test 12 after the
error crashed the console, in this build and the ones before it. The 5 s wait
is gone again, since quitting at once never made that worse.

**What RetroArch does differently.** RetroArch switches between its cores,
separate SELFs in one package, with this call (`frontend_psp_exec` in
`frontend/drivers/platform_psp.c`). Its call is the same as ours: a path, no
argv, no options, and the process ends right after. Its setup differs in
three ways:
- its SELFs are safe (`vita-make-fself -s`, or `-ss`, in `Makefile.vita`),
  which `vita-make-fself` marks with authority id `0x2F00000000000002` (`…03`).
  Every SELF here is unsafe, `0x2F00000000000001`. vitasdk's tool describes a
  safe one as having no access "to restricted APIs and important parts of the
  filesystem";
- its paths are `app0:/<core>.self`, with a slash;
- it is a game with `ATTRIBUTE2=12`, as `VPLXBGT13` is.

The first is the likeliest to matter. If the switch only works between safe
SELFs, the player would also have to work as a safe one, which is not known
for the shell's music service. VitaPlex itself is built unsafe.

**The last try, as RetroArch does it.** `VPLXBGT12` and `VPLXBGT13` now carry
safe builds of both programs, separate targets made from the same sources
with the same settings. vitasdk's `vita_create_self` (`cmake_toolchain/vita.cmake`)
adds `-s` when `UNSAFE` is not given. It names its intermediate files after
the source target, so the same program cannot be made twice. The switch asks
for `app0:/<program>`, and the program quits as soon as the call returns
success, as RetroArch's does. A configure of these targets with vitasdk's own
macros, and stand-ins for its tools, generated `vita-make-fself -c -s` for the
program with the screen and `-c -at 2 -m 0x4000 -s` for the player. The
unsafe builds were left as they were, and both packages hold the safe
programs. The test is triangle on `VPLXBGT13`, the game, as RetroArch is.

**In a game the switch works, and changes nothing that matters** (logs
`6b8415c2` and `5571cc78`, build 1997). `VPLXBGT13`, the game, switched both
ways without an error. Triangle restarted `eboot.bin` 0.87 s after it was
asked for, and X started `player.self` 0.98 s after. Both picked the log up
from the note. But the player kept the game's memory: 348 MB of main memory
free, and the full 80 MB probe allocated, where a system-mode app has 16 MB.
Its system-mode boot param counted for nothing. When PS was pressed, 19 s in,
the log stopped: no deactivate event and no "away" lines followed, as for any
game, which is held the moment it leaves the foreground (`VPLXBGT01`). The
safe programs' budget calls were refused with 0x8080201F, "not authorized",
where the unsafe ones got 0x8080201C, which shows they were built safe.

`VPLXBGT12`, the non-game app, still failed with the same programs. The switch
to `eboot.bin` never returned and no quit request came. The program ran on
for 16 s until PS was pressed, and the user saw the error.

So one app cannot be both. The switch works only in a game, and the program
it starts runs as the app it is in: a game, with a game's memory, held when
it leaves the foreground. A player that plays on through another game has to
be an app of its own, as in BG Test 11.

The switch back from the player also moved off the watcher onto the main
thread. Run on the watcher, a switch that does not return before the quit
request would leave no one to answer it, and an app that did not quit once
hung the console (`3ea218f5`).

**What the other apps do.** Before going further, the setup was checked
against the apps that play on, from their sources:

- ElevenMPV-A's `param.sfx` has `CATEGORY` `gdc`, `ATTRIBUTE` 0x01089088,
  `ATTRIBUTE_MINOR` 16 and `ATTRIBUTE2` 0, and its `ebootparam.bin` gives
  attribute 2 and 16 MB. Those are the helper's settings exactly (`VPLXBGT05`,
  06, 10 and 11), so the helper is that app without its screen. ElevenMPV-A is
  one app because all of it fits that budget: its screens are Sony's PAF, built
  with Sony's SDK. Its quick-menu controls come from the plugin it loads into
  SceShell as it starts (`sceAppMgrGetIdByName` on NPXS19999, then
  `taiLoadStartModuleForPid`). ConsoleMods' wiki says its background playback
  works "with most of the games" as it is, and "with all games" with the
  LowMemMode plugin.
- vita-headers (September 2026) documents the call under `sceAppMgrLoadExec`
  as "Replace the current game process with another executable", which is what
  `VPLXBGT12` and 13 found.
- LowMemMode, also GrapheneCt's, is a SceShell plugin (`*main`) that makes one
  SceShell function, the memory budget check at app launch, return 0. It has
  offsets for retail 3.60, 3.65, 3.67 and 3.68. Its README: "Allows you to
  launch other applications in LOWPHYMEM mode while enlarged memory mode app
  (+29MiB, +77MiB and +109MiB) is in the background". VitaPlex and VitaPlex BG
  are such apps (`ATTRIBUTE2=12`, +109 MiB), and the helper is a small one, so
  that check is the likely source of the prompt to close VitaPlex BG
  (`0bbfcb94`). It also fits the other order passing: `VPLXBGT06` ran on when
  VitaPlex was opened (`8098b625`). Not proven yet.
- BGFTP, GrapheneCt's FTP server, is one package with two programs, using the
  system's background applications. `eboot.bin` with `sce_sys/param.sfo` (gdc,
  `ATTRIBUTE` 0x01009000, `ATTRIBUTE_MINOR` 17, `ATTRIBUTE2` 0, boot param
  attribute 0 and 4 MB) loads `SCE_SYSMODULE_BG_APP_UTIL`, calls
  `sceBgAppUtilStartBgApp(0)` and quits. `eboot2.bin` with
  `sce_sys/param2.sfo` (`CATEGORY` `gdd`, a title id of its own,
  `INSTALL_DIR_ADDCONT` and `INSTALL_DIR_SAVEDATA` set to the main app's,
  `ATTRIBUTE` 0x81000; boot param attribute 3 and 16 MB) is the server, on a
  14 MB heap. Its README says it works "simultaneously with any game, including
  enlarged memory mode games, and in sleep mode". It is ended when "LiveArea
  of the main application is peeled off" or an "Enlarged memory mode game is
  started", and with LowMemMode it can be started again after the latter.
  vitasdk has the call (`psp2/bgapputil.h`, `SceBgAppUtil_stub`, sysmodule
  0x34), and its header says the mode must be 1, where BGFTP passes 0.

So there is a second program for one package after all, started as a process
of its own with its own small budget, which is what the switch could not give.

**The player as a background program** (`VPLXBGT14` to 16). Three packages
carry BG Test 10's player as `eboot2.bin`, with BGFTP's background boot param
(`vita_create_self … MEMSIZE 0x4000 ATTRIBUTE 3`) and a `param2.sfo` that
vita-mksfoex writes with its defaults off. That file has the keys of BGFTP's
background program's and no others, under title ids of its own (`VPLXBGP14`
to 16) and pointing at its app's. Built with `BGTEST_BGAPP`, the player has no
screen, logs to `bgtest-<app>-bgapp.log` on the app's clock, plays two songs
from the library as soon as it starts, and quits 10 minutes after they end.
It also logs whether the app that started it is still running
(`sceAppMgrGetIdByName`), whenever that changes. Whether it has its app's
files at `app0:` is not known either, so it looks for the CA bundle there,
says whether it found it, and otherwise takes the one in the app's directory.

- `VPLXBGT14` is a game with VitaPlex's extended memory, as VitaPlex is. X
  asks for the player: with 0, as BGFTP does, then with 1 if that is refused.
- `VPLXBGT15` is the same game without extended memory.
- `VPLXBGT16` is the control, BGFTP's main app: its `param.sfo` values and a
  starter that asks for the player and quits (`bgstart.c`). It runs in system
  mode with attribute 0, as BGFTP's does, but on 16 MB where that has 4. The
  system loads its own modules into every vitasdk program, and BGFTP's main
  app turns that off to fit.

The logs say whether a game, with and without extended memory, can start a
background program, and what the program gets: its title id and its budget.
They also say whether it plays on when the app is left, when a game starts and
closes the app, and when the app's LiveArea is peeled off. If 16 fails too, the
packaging is wrong, whatever the app. A host check ran the note, the log names,
the order of the two calls and the player's run, which starts at once and
stops 10 minutes after its songs. The `param2.sfo` the build writes was
decoded and compared with BGFTP's.

**The control's background program plays; the games' give an error** (logs
`65082821`, `6b8dce16`, build 1999).

- `VPLXBGT16`'s starter loaded the module, asked with 0, got 0 in 0.04 s, and
  quit. The background program started 0.99 s later. The system gave it the
  title id `VPLXBGP16`, and `app0:` was its app's (the CA bundle was there).
  Its budget was mode 2: 16 MB of main memory, 4 MB free, and 16 MB of CDRAM.
  So attribute 3 in its boot param did not give it contiguous memory (mode 3).
- It played as BG Test 10's player does. It downloaded the first song
  (3.07 MB in 2.9 s), and the shell played it at its length while it fetched
  the second and gave that a length header. "Its app VPLXBGT16: not running"
  came at once, and it carried on, so it outlives the app that started it.
- After PS (0x20000000 at 51.4 s), it logged on at the home screen. Its log
  stops at 56.8 s, 0:44 into the first song, with no quit request and nothing
  after. The user started a game then, and VitaSurf later, and heard music
  through both. The shell plays a song it was given to its end without the
  app that gave it (BG Test 10), so that music does not show the program
  running. Whether it was ended or held at that point, and whether the second
  song followed, the log cannot say.
- `VPLXBGT14` and 15, the games, gave 0x80101104. The code is not
  documented. The ones beside it, 0x801010xx, are the app database's
  (SceLsdb), which records an app's foreground and background title ids
  (`_fg_title_id`, `_bg_title_id`), so that is where to look first.

Three more packages take a step from 14 at a time, all with X:

- `VPLXBGT17` has VitaPlex BG's `param.sfo`: a non-game app (`gdc`,
  `ATTRIBUTE` 0x01089088) on the game budget, with extended memory. That is
  the one app VitaPlex could be.
- `VPLXBGT18` is the same without extended memory (`VPLXBGT03`'s, ElevenMPV-A's).
- `VPLXBGT19` is the game of 14 with the `ATTRIBUTE` of BGFTP's main app
  (0x01009000, `ATTRIBUTE_MINOR` 17). If it works where 14 did not, the
  attributes decide, not the category.

The run that answers the open question: X, then PS and a game without
extended memory, listening into the second song, then the home screen for
20 s, then VitaSurf. A program the system held logs how long when it
resumes ("did not run for"), and one it ended logs nothing more. The user's
recollection that the music went on through LittleBigPlanet fits either: the
first song plays on in the shell either way, and only the second needs the
program. Since the packages are tried one after another, a player still
running quits as soon as another app asks for its player (the note
appearing), so that two do not play over each other.

**The power button stops the music; the program goes on** (log `91252932`,
`VPLXBGT16` again, build 1999). The times are the log's.

- The player started 1.08 s after the ask, and the shell played song 1 from
  0:08.
- At 1:56 the power callback carried 0x80401000: the power button
  (0x80000000) and apps suspending (0x00400000). Half a second later the
  shell's state was 2, stopped, at 1:47 of the song, and nothing played it
  again.
- The Vita went to sleep at 2:15 (0x00010000) and woke at 2:21. The player
  was held for those 6 s ("did not run for 6.0s"), then went on, and the first
  request after it took 2.4 s.
- From the PS press at 2:29 to the end of the log at 4:58 it ran without a
  gap. At 4:32 song 1's time ran out (its length and a minute), with the shell
  still stopped at 1:47, so the player handed over song 2 and the shell played
  it. That was the second song the user heard. So the program was running
  then, whatever was in front. The log does not say whether a game had been
  started.

**A non-game app on the game budget starts it, and the music survives the
power button** (logs `d942eac0`, `e98e760e`, `VPLXBGT18`, build 2002).

- `VPLXBGT18` (no extended memory, `VPLXBGT03`'s `param.sfo`) installed with
  the background program, where 14 and 15 gave 0x80101104. X asked with 0,
  which returned 0 in 0.02 s, and the program started 0.98 s later as
  `VPLXBGP18`, on 16 MB (mode 2).
- It played the first song to its end (3:23) and the second straight after
  it, with the app in the background from 0:17.
- At each press of the power button (0x80401080), the shell stopped the song
  within half a second, and the player's play brought it back after 0.5 to
  4.6 s. The user turned the screen off and on several times and heard the
  music go on.
- Then power was held (0x40000000, at 5:01) and the system suspended
  (0x00010000, at 5:03). The program ran on and so did the song (1:17 to
  1:47 by the end of the log), but the network was gone ("could not
  connect"). So only songs already downloaded can play then.
- The app was asked to quit at 0:50 and logged its quit, but its process went
  on. Its network check ran for another 4½ minutes, to the end of the log,
  and the program kept finding it running. The same call ended the process in
  every earlier test, so the background program is the difference. The last
  time an app did not end when asked (`3ea218f5`), the next app hung. What
  the user opened then, and whether it started, is the open question.

ElevenMPV-A's power callback handles exactly this. At bit 22 (0x00400000),
with a song in the shell, it sends play again. While it plays, it ticks
`sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND)`, so that the
Vita does not sleep by itself. The player now does both. A song that stops
part-way gets play again every 5 s, and the player moves on after two minutes;
time spent stopped is not counted against the song. A host run covered a song
stopped part-way, the same with the power callback, and play doing nothing,
and each went on to the end.

**The game opened over 18 hung, and 19 would not install** (the user, after
build 2002).

- The game started over `VPLXBGT18` at 0:50 was 2048, and it never came up;
  the console had to be restarted. 2048 waited for 18 to end, as the next app
  waited for VitaPlex in `3ea218f5`, and 18 never did. Its watcher logged
  "quit" and called `sceKernelExitProcess`, which did not return, while its
  other threads ran on. So an app cannot quit while its background program
  runs, at least on that path. 16 and BGFTP's main app never meet this: they
  are gone 0.04 s after they ask, a second before their program is up.
- 2048 is a vitasdk homebrew game, and vita-mksfoex gives those
  `ATTRIBUTE2_MEM109` unless told otherwise, so it most likely runs in
  enlarged memory mode. BGFTP's README says starting such a game ends a
  background program, so the hang may need that too. The next run does not
  open one.
- `VPLXBGT19`, the game of 14 with the `ATTRIBUTE` of BGFTP's main app, would
  not install either. So the category decides: a game cannot carry a
  background program, and VitaPlex would have to become a non-game app to have
  one.

So VitaPlex would do what BGFTP's main app does: start the program and quit at
once. Two more packages do that from the app with a screen:

- `VPLXBGT20` is 18 (no extended memory) with X starting the player and the
  app quitting at once. When the system asks it to quit, as when a game is
  started over it, it starts the player first. That is the hand-over VitaPlex
  would make as a game takes its place.
- `VPLXBGT21` is the same with extended memory: VitaPlex BG's `param.sfo`, as
  17 has. 17 itself would hang as 18 did, so it is not to be run.
- Opened again while their player plays, both take the music back, as
  VitaPlex would. They ask the player to stop and wait up to 10 s for it to
  go. The request is a second note, `bgtest-bgapp-quit.txt`, which says when
  it was made and why; the program drops one made before it started, which
  was for another program. Whether the system lists a background program by
  its title id (`sceAppMgrGetIdByName` on `VPLXBGP20`) is not known, so the
  request goes out even when it does not, and is taken back after 2 s if
  nothing has taken it.
- Every exit of 14 to 21 and of the program now has a rescue behind it. A
  thread started as the process begins to quit wakes 3 s later, which it only
  does if the process is still there. It starts just before the exit call,
  and on a quit request in 20 and 21 before they ask for the player, in case
  the system, switching apps, holds that call. In an app it asks the player
  to stop, in case the exit is waiting for that, and gives it 5 s and 1 s
  more. Then the app ends with `_sceKernelExitProcessForUser`, the system call
  `sceKernelExitProcess` (SceLibKernel) ends with, past whatever LibKernel
  does before it; the program's own rescue goes straight to that. vita-headers
  declares the call, but vitasdk 2026.08's stubs may not carry it, so
  `exit_stub.S` imports it (SceProcessmgr 0x2DD91812, 0xC053DC6B) under a name
  of this project's. Each step is logged, so a rescue that ran shows which
  step ended the process. 18 opened with this build would answer the rest for
  an app that stays: whether its exit finishes once the player has stopped.
- A program the system asks to quit (as its app's LiveArea is peeled off)
  now stops the music as it goes, where before it left the shell playing.

The run: open 20 and press X, and the music starts. Press PS and start BG Test
8 (a game without extended memory): does the music go on? Close it and open 20
again, and the music should stop as it opens. Press PS and start BG Test 8
again: 20 is closed and starts its player as it goes. Does the music start in
the game? Then 21 the same, though opening an app with extended memory may end
the player before 21 can ask; the logs would show that as a program that
stops with no request. Not 2048, BG Test 7 or VitaSurf yet. A host run covered
the request and its note (fresh, from before the program, half written), the
take-back (no player, one listed, one not listed, one that never takes the
request, one that takes it and stays), X and the quit request in 20 and 21, X
refused, 18 and 8 as before, both rescues, and the player stopped part-way
through a song, which plays nothing again and starts no second song. The
packages' `param.sfo` and `param2.sfo` were decoded from the configured build.

**The app hands over and is gone; the player plays on beside a game** (logs
`f82c4a2b`, `f7c0b792` and `c03d8023`, build 2005). The times are 20's clock.

- 20 looked for a player first and found none: its title id gave 0x80802012,
  and nothing took the request in 2 s. Then X: the module loaded, the ask
  returned 0 in 0.02 s, and 20 logged "quitting at once" and "quit" at 5.33 s.
  Nothing came after, so the rescue never woke and the process ended within
  3 s. The player found its app gone at 6.59 s. So the hand-over BGFTP's main
  app makes works from an app with a screen, a network thread and all the
  rest.
- The player started 1.02 s after the ask, as `VPLXBGP20` on 16 MB (mode 2),
  and the shell played song 1 from 11.3 s.
- BG Test 8, a game without extended memory, started at 25.0 s. Its PS press,
  11.45 s into its log, is the player's at 36.48 s. The player ran on beside
  it for those 11 s, with the network working.
- At 42.15 s, 5.7 s after that press and 0:30 into song 1, the player's log
  stops, with no quit request. One would have stopped the music and said so.
  The user had gone on to VitaSurf, which has extended memory, and heard the
  music go on there. That fits BGFTP's README, which says such an app ends a
  background program: the shell plays a song it was given to its end without
  the app that gave it (BG Test 10). If so, the second song never came.
  Opening 20 again would show whether the player is still listed.

**21 does the same with extended memory, and opening it again ends the
player** (logs `3edd00fd` and `2aefa839`, build 2005; each run on its own
clock).

- 21 had 340 MB free and could allocate 342 MB, as VitaPlex BG can. Both
  times it found no player, asked with 0, got 0 in 0.02 or 0.03 s, and quit
  (at 9.24 s and 7.69 s) with nothing after, so no rescue. VitaPlex BG's own
  `param.sfo` can carry the player and hand over to it.
- In the first run the player started 1.03 s after the ask. The shell played
  song 1 from 3:21, not 0:00, and it was over 2 s later. The player opens the
  same file every run, and the shell had played that one on by itself after
  20's player was ended, so it seems to have kept its place in it. The player
  went on to fetch song 2, and its log stops at 26.3 s, before song 2 was
  ready, with no quit request.
- 21 was opened again, and 4.5 s in the player was not listed by its title
  id; nothing took the request in 2 s. It had been ended before 21 could ask,
  as an app with extended memory started, which is what BGFTP's README says
  of enlarged memory mode games. So for VitaPlex, which has extended memory,
  opening VitaPlex takes the music back by itself, and the player gets no
  warning. What it was playing would have to come from the shell, which plays
  on, or from a note the player keeps up to date.
- The second run's player started 0.91 s after the ask and played song 1
  from 0:00. It ran on through a PS press at 50.5 s for another 31 s. Its log
  stops at 81.9 s, 1:05 into song 1, again with no quit request. The user
  heard the song in VitaShell, where the logs were copied. The shell would
  play it there either way, and the copy may simply have been made at 81.9 s.
  The log as it is on the Vita now would tell, since a player still running
  went on writing to it. VitaShell sets no `ATTRIBUTE2` (its CMakeLists.txt),
  so it gets vita-mksfoex's default, which is now `ATTRIBUTE2_MEM109`. If the
  user's copy was built with that default, opening it would end the player.

So the player now waits for each song it hands over to play, and if the shell
has it more than 5 s in, takes it back to 0:00 with the seek event (BG Test
11's, which worked) and plays it if it is not playing. A hand-off to a
position still seeks there. A host run covered the shell keeping its place at
3:21, and the earlier cases and BG Test 11's seeks still pass.

**The hand-over on a quit request works once the rescue ends the app; the
player plays the whole queue beside a game; taking the music back works**
(logs `10c46450`, `8c401491` and `96f3c9a7`, build 2007; 20's clock for each
run).

In the first run, 20 found no player and was sent away. At 52.3 s the system
asked it to quit as the user started BG Test 8.

- It asked for its player: the ask returned 0 in 0.03 s, and it logged
  "quit" at 52.43 s. The exit did not finish: its network thread ran on, and
  at 55.3 s the rescue found it still there. The player was listed as running
  then, but had not started, and took no request in 6 s. At 61.4 s the rescue
  ended the app through `_sceKernelExitProcessForUser`, which worked.
- The player started at 62.98 s, 10.64 s after the ask and 1.5 s after the
  app had gone, and found it gone. So the system starts the program only once
  its app has gone. The app's exit waited on something the system does only
  after the game the user had asked for, and BG Test 8 waited those 9 s.
- The player played song 1 whole beside the game and handed over song 2 at
  4:32, as song 1 ended, and the user heard it start. The network check ran
  throughout. Each request took about 1 s this time, in all three logs, so
  the server or the Wi-Fi was slower.

In the second run, the user opened 20 again.

- At 3.2 s the system listed the player by its title id, with its process.
  It took the request in 0.10 s, stopped the music, gave the BGM port back,
  and was gone 0.24 s after the ask, through its own `sceKernelExitProcess`.
  The user heard the music stop.
- X then failed four times: the ask returned 0x80010013 (the errno range:
  ENODEV) with 0, and 0x80106501 with 1. At 39.7 s the system asked 20 to quit
  from the background. It asked for the player, was refused the same way, and
  its exit stuck again: still there at 42.7 s with no player listed, it was
  ended through the system call at 48.75 s.

So on a quit request, `sceKernelExitProcess` does not return in an app that
has used SceBgAppUtil, whether its player is running (18), just asked for
(20's first run) or refused (its second). On X, in front, it returns: four
times for 20 and 21. The system call underneath ends the app every time, and
the system then goes on: the game opens and the player starts. Why the ask was
refused in the second run is open. The first run ended through the system
call, which skips whatever LibKernel does on the way out, and the player had
quit by itself. Either could have left something behind that only a restart
clears.

Now 20 and 21, asked to quit, start the player and end through the system call
at once. Their rescue, when they hand over, waits 2 s and leaves the player
be. A refusal logs whether the player is listed. A host run covered both, and
the earlier paths still pass.

**A player that quits by itself leaves its app's next start refused; on a
quit request the game waits about 10.8 s, whatever the app does** (logs
`0a521b78`, `f527e31b`, `befac425`, `0b39eeea` and `232303fb`, builds 2007
and 2009; each run on its own clock).

- 20 after a restart (build 2009): X got 0 in 0.02 s and 20 quit cleanly. The
  player started 1.09 s later and played. Opened again, 20 found the player
  listed; it took the request in 0.10 s and was gone 0.27 s later, by its own
  exit. Then X was refused nine times over 40 s, with 0x80010013 for mode 0
  and 0x80106501 for 1, and no player listed. The system's quit request was
  refused the same way. The refusal in log `10c46450` came after a run that
  had gone through the system call; this one came after a clean exit, so
  that is not the cause.
- 21 on build 2007 did the same. Asked to quit as a game started, it got 0
  in 0.10 s, stuck, and went through the rescue's system call 9.1 s later;
  its player started 2.1 s after that. Opened again at once, 21 found it
  listed. It took the request in 0.25 s and was gone 0.41 s later, by itself,
  and X was then refused twice, 48 s apart.
- After the system had ended a player, as an app with extended memory
  opened, the next start went through (log `3edd00fd`). So a player that
  quits by itself, through `sceKernelExitProcess`, leaves something that
  refuses its app's next start until a restart. One the system ends does not.
- 21 after the restart (build 2009): asked to quit as BG Test 8 started, it
  asked for the player (0 in 0.08 s) and went through the system call at
  once, at 12.08 s. BG Test 8 still started only at 22.74 s (its PS press,
  10.68 s into its log, is the player's at 33.42 s), and the player 0.85 s
  after it, 11.63 s after the ask. In the earlier runs, which went through
  the rescue 9.1 s in, the game also came 10.6 to 11.2 s after the request.
  So on a quit request, in an app that has used SceBgAppUtil, the system
  starts the next app about 10.8 s after asking, however soon the app has
  gone. Neither X nor the system call changes that. PS, which brings no quit
  request, might.
- In that run the player played on through the power button, sending play
  again each time. Then power was held (0x40000000, then 0x00011080), and the
  shell's player went to state 10 at 0:22, then to 0, refusing play with
  0x8002058E, while the network still worked. Whether the console was
  restarted then is not known.

So 20 and 21 now take the music back by ending the player through the
system. The player stops the music and waits to be ended, and quits by
itself only if that has not come in 3 s. Half a second after the player has
taken the request, the app calls `sceAppMgrDestroyAppByName` (SceAppMgrUser,
in `SceAppMgr_stub`). It ends a player that has not taken the request in
5 s all the same, and the rescue ends a player it asks to stop the same way.
They also hand the music over on PS as on X, when the deactivation comes
within 2 s of a PS press, so a game started next finds nothing to close. A
host run covered both, and the earlier paths still pass.

**On PS the player starts but loses its CA bundle; the system's ending did
not stop a refusal; X in 21 over 20's song crashed the console** (logs
`753b3fb0`, `7ba90482`, `8fe35d97`, `5d35ef15` and `39f8db61`, build 2010;
each run on its own clock).

- PS handed over three times (20 twice, 21 once). The deactivation came
  0.21 to 0.31 s after the press. The ask returned 0 in 0.03 or 0.04 s, the
  exit stuck, and the rescue ended the app through the system call 2 s later,
  as planned. Each player started 1.04 to 1.12 s after the ask and found the
  CA bundle at app0:. The app was gone 1.4 to 1.7 s into its log. The library
  request, whose connection was made before then, came back. Song 1's
  download, a new connection, failed at once with "Problem with the SSL CA
  cert", since curl reads the bundle for each connection. So app0: went with
  the app, and there was no music, as the user found. Each player then idled.
- 20 took the music back through the system twice. Each time its player took
  the request in 0.10 s, `sceAppMgrDestroyAppByName` returned 0 half a second
  later, and the player was gone 0.73 or 0.75 s after the ask. After the
  first, X 6 s later was refused (0x80010013, then 0x80106501), and so were PS
  and the quit request that came as 21 was started. After the second, X 0.9 s
  later went through, and that player played song 1. So ending the player
  through the system does not by itself keep the next start from being
  refused. A refusal also came 0.9 s after a player quit by itself (log
  `befac425`), so the time since is not it either. What causes it is open.
- The request to stop was one file for every app. Opened again while 21's
  idle player ran, 20 asked its own, and 21's took the request, waited 3 s to
  be ended and quit by itself. 20 had no player running, and its X, PS and
  quit request were all refused.
- 20's last player played song 1 from 0:01 at 12.85 s. Its log ends there
  with no request, as 21, with its extended memory, was opened, and the shell
  played on. 21 found no player of its own, X got 0 in 0.04 s and 21 quit.
  Then the console crashed, the user said. 21's player logged nothing of that
  run, but its lines may have been lost in the crash. It would have
  downloaded song 1 into `bgtest14-1.m4a`, the file the shell was playing for
  20's player, part-way in, and then handed that file to the shell over the
  song. In `3edd00fd` a player did the same with the shell at 3:21 of 3:23 in
  that file, near its end, and nothing crashed.

So the app now copies its CA bundle to `ux0:data/VitaPlex/bgtest-cacert.pem`
as it asks for its player. The copy is written beside the target and moved
in, and is skipped when one of the same size is there. The player uses that
copy first. Each app's request to stop is a file of its own
(`bgtest-<title id>-quit.txt`). Each player saves its songs under its app's
title id (`bgtest-<title id>-N.ext`), so no player writes another's file. As
it takes the music back, the app opens the shell's music service and reads
its clock twice, 0.5 s apart. If the clock moved, or the state is 1, it stops
the shell and logs both readings. A player that gives way to another app's
now stops the music as it goes. A host run covered these, and the earlier
paths still pass.


### PS4: the popup, and nothing to hang progress on

`sceKernelSendNotificationRequest` writes to `/dev/notification0`, which
SceShellUI listens on — the top-right popup, drawn over a running application.
It is in libkernel, already linked. `OrbisNotificationRequest` is a
reverse-engineered declaration, hence the configure probe.

There is deliberately no progress form. The PS4's progress lives in the system
download list, which a homebrew application cannot enter, and posting a fresh
popup every second would bury the notification area. `setProgress` is a no-op
here on purpose rather than by omission.

### Switch: not possible

libnx's `notif` service is an alarm scheduler — `notifRegisterAlarmSetting`,
`notifUpdateAlarmSetting` and so on. There is no call that displays a message
from a running application, so Switch falls through to the header's inline
no-ops. This is worth writing down only because "add notifications to Switch
too" looks like an oversight rather than a platform limit.

### Word-by-word lyrics

Two sources can carry word timing, and only one of them is documented.

**Enhanced LRC ("A2")** is the confirmed one. A line reads

```
[00:12.34]<00:12.34>I <00:12.61>would <00:12.90>never <00:13.40>
```

— a stamp before each word, and usually one more at the end marking where the
last word stops rather than starting another. Parsing these also fixed a bug
that predates the feature: nothing knew what the angle brackets were, so a file
like this displayed its own timestamps as part of the lyric. The stamps are
stripped now whether or not the highlight is switched on.

**Plex's own documents** are the unconfirmed one. Each `<Span>` is collected
with whatever offset it carries, using the same two attribute names
(`startOffset`, `startTimeOffset`) the enclosing `<Line>` already uses. Whether
Plex's lyricfind documents actually stamp their Spans could not be established
from anything published — so this reads the attributes if they are there and
does nothing whatsoever if they are not. No request changed shape to get them.

Word timing is used only when **every** span or word in a line is stamped, and
only when there are at least two. A half-stamped line would stall the highlight
partway across and read as a bug; a single stamped word is the line stamp again
and buys nothing but a pile of extra views.

The rendering is one `brls::Label` per word inside a wrapping row. borealis
exposes most of yoga's style but not flex-wrap, so `setFlexWrap` reaches the
node through `View::getYGNode()`, which is public — no subclass, no patch. The
row needs a definite width for wrapping to happen at all, which it gets from
`player/lyrics_list` being `alignItems="stretch"`.

Word labels are single-line, and have to be. A label decides whether it
wraps inside its measure function and keeps the answer as a flag, and yoga
often reuses a cached measurement without calling the function again. The
flag can then be left over from a trial pass that offered less width. The
label is laid out exactly as wide as its text but drawn as a wrapped text box,
and its last letter falls onto a line below the row. A desktop render of the
mobile list showed "hallway," and "humming" drawn as "hallway" and "hummin",
with the "," and the "g" under them. Single-line labels never take the
wrapped path, and the row does all the wrapping anyway.

In the classic sheet, a word row's wrapped lines get yoga's row gap,
(1.65 − 1) × the font size. A one-line label is only as tall as its text, so
without the gap a wrapped row stacks its lines about one font size apart. A
wrapped whole-line label spaces them by its line height, borealis' default
1.65. The row gap applies between wrapped lines only, so a row that fits on
one line is unchanged. The mobile layout keeps the spacing it shipped with.

Sung words hold the highlight rather than dimming behind the cursor: a line is
read as a whole, and one lit word between two greys is harder to follow than a
line filling up.

Tapping a word jumps to that word rather than to the start of the line. The
word is found from where the tap landed, not by making the words focusable: a
tap carries its position, and focusable words would turn one D-pad step per
line into one per word — several hundred down a song. A controller still gets
the line, since it has no way to point at a word.

Nearest is measured to a word's *edge*, not its centre. By centre, a tap a
pixel off the end of a long word goes to a short word further away, because the
long word's middle is further from the finger.

That choice turns out to matter for more than looks. Checked against a 4,299-file
library (227,425 lines, 395,685 words), including a file picked as a negative
control because its word timing runs backwards mid-line. It does — 20 lines of it — and
the cause is not corruption:

```
[00:09.96]<00:09.96>Ellen <00:10.67>Pope <00:09.96>(Huh)
```

The background vocal is sung *with* the line, not after "Pope", and Enhanced
LRC has no way to write "at the same time as", so the writer put the real time
in. A renderer that moves a single cursor jumps backwards here. A renderer that
fills a line up cannot: the lit set is a prefix, and a prefix only grows. Walked
at the real 80ms tick across all 20 lines, the highlight never regresses once —
`(Huh)` simply lights with "Pope" instead of before it.

So "it looks fine on the broken file" is not evidence the word tags are being
ignored, which is the obvious reading. The tags are parsed; this shape is
degrading gracefully.

Words are kept in the order written, never sorted. Sorting would silently
invent a performance the file does not describe.

**A stamp can land inside a word**, and this is common enough to be the first
thing to get wrong:

```
[00:09.93]<00:09.93>Tum<00:10.18>ble <00:10.32>out <00:10.50>of <00:10.64>bed
```

"Tum" and "ble" are separate stamps with no space between them, so the
highlight can cross a long syllable as it is sung. The line's own text comes
out right either way — it is the raw run of characters — but the pieces are
what get drawn, and drawing them evenly spaced spells "Tum ble". So each piece
records whether whitespace actually followed it (`LyricWord::spaceAfter`), and
the row's gap is applied only where it did. 34 of 40 lines in one sampled file
are affected; without this the track is unreadable rather than subtly off.

The invariant to hold on to, and what the tests check: joining a line's words,
with a space exactly where `spaceAfter` says, reproduces the line's text.

How common this is decides whether it is a nicety or the whole feature. Across
a 4,299-file library: 1,171 files carry word timing, and **789 of them — two
thirds — stamp inside words**. It is not an edge case.

This has a second consequence, and the first attempt got it wrong by leaving
it alone. With every piece a direct child of the wrapping row, the wrap can
fall *between* two halves of a word — "ne" ending one line and "ver" starting
the next. That does not read as hyphenation, it reads as a typo, and on a
syllable-timed track it happens on most lines rather than rarely.

Yoga cannot be told to keep two children together, but it does not need to be:
a wrap falls between the row's *items*, so anything that is one item cannot be
split. Syllables of one word therefore go in a nested non-wrapping row, and
that group is the item the outer row sees. A word written as a single stamp is
added straight to the row, so the ordinary case gains no views at all — across
14 sampled files only one stamps inside words, and it accounts for every one
of the 68 group boxes the corpus produces.

The label vector stays flat and parallel to `line.words` whichever shape the
tree takes, so the highlight is indifferent to the grouping.

Lines whose words run past the next line's stamp are common — 47 of 123 in one
sample, two singers at once. Only one line is active at a time, so the tail of
an overlapping line greys while it is still being sung. That is inherent to a
single-active-line view and is what every mainstream lyrics pane does.

A one-word line ("(What?)", "(Yeah)", "Darkchild") carries no word timing by
the two-word rule and falls back to lighting whole. Real files are full of
them: 29 of 170 lines in one sample.

What the library says about the rest of the format, so nobody re-derives it:
every one of the 4,299 files is valid UTF-8, none carry a BOM, six use CRLF.
No angle-bracket tag anywhere is anything but a time — there is no `<i>`/`<b>`
markup to strip. Every line carrying word tags also carries a line stamp. The
widest line is 29 stamps and the longest file 334 lines, which bounds a row and
a screen respectively.

Nine lines pair a stamp with a speaker cue in its own brackets —
`[00:00.19][Missy Elliott:]`. The second bracket is not a time, so the stamp
scan stops there and the cue survives as the line's text; only a genuine `[ar:
…]`-style tag is dropped. There are no genuinely repeated time stamps in the
whole library, so the rule that gives the words to the first one is untested by
real data and kept as insurance.

Two lines are lost, both `[00:49:00]` — a colon where the fraction separator
should be. Both are empty markers, so nothing readable goes missing, and
accepting a second colon would collide with `[hh:mm:ss]`. Left alone.

The sync timer runs at 80ms while any line is word-timed, against 250ms
otherwise. A line lasts seconds and 250ms sits comfortably inside that; words
arrive several a second and would visibly lag. A tick that crosses no word
boundary returns having touched nothing, and the faster rate is never paid by a
track without word timing.

It is a setting (`lyricsWordByWord`, on by default) because a word-timed line
costs one view per word. Galway Girl, 50 lines, is 499 labels where it used to
be 50; the worst of the sampled files is 196 lines and **2,212 labels**. That
number is the reason the setting exists, and the reason to reach for a single
custom view per line — one leaf that draws its own words with nanovg — if a
dense track ever hitches. Nothing here has been profiled on a device.

Two things keep that affordable, and both are worth knowing before anyone
moves this code:

Where word rows are built. At first that was the mobile layout only, so in
practice a phone. The classic sheet now builds them too, on desktop, tablets,
TV and the home consoles, at its own sizes (17 at rest, 19 active, the amber
`#E5A00D` highlight). The rows are left-aligned like its whole-line labels, and
`lyrics_list` is already `alignItems="stretch"`, so a row has the sheet's ~416
units of text width to wrap in. The exception is the Vita
(`classicWordRowsAffordable()`). The classic sheet is its default, and it is
the hardware the cost below was kept away from, so it stays whole lines there.
Forcing the Mobile layout in settings still gives word rows on any platform.

`Box::draw` culls only leaf children — "nested boxes will do that check
themselves" — so a row that is a Box no longer gets skipped wholesale when it
is offscreen; it is descended into, and its word labels are culled one by one
instead. The text drawing is still skipped, which is the expensive part.

The one thing that would hurt is resizing. `Label::setFontSize` calls
`invalidate()`, which walks to the root and relayouts the entire tree, so a
row changing size naively costs one full pass **per word**. Sizes are
therefore only written when they actually change, and `setLineHeight` is not
set on word labels at all — the flex row does the wrapping, so a one-word
label's line height changes nothing. Colour is a plain member assignment with
no invalidate, which is why the per-tick word highlight is free and only the
line change touches layout.

---

## Layout and UI

### Three layouts, one set of view ids

`player.xml`, `player_mobile.xml` and the video OSD all declare the same view
ids — every `BRLS_BIND` resolves by id and throws if one is missing — so only
the geometry differs and the activity is otherwise layout-agnostic. The two
mobile designs share one file and hide each other's views.

Video always uses the classic player. The mobile layout is a Now Playing screen
for music: big cover, queue sheet, and no room made for a video surface.

### Scaling code-built rows

Queue and lyrics rows are built in code at sizes written for the classic layout.
The mobile XML is scaled to a phone frame — 1280 logical units standing in for
the handoff's 412 — so anything shared has to be scaled the same way or it
renders at a third of the size.

The two mobile designs are drawn against *different* frames: 412-wide portrait
for music, 915-wide landscape for video. `ui()` picks the factor from whichever
is up. `uiRow()` is gentler, because at the full factor a queue row is ~161 units
tall and only two or three fit the sheet — the text is right at that size, the
box around it is not.

### The video OSD's scale factor

borealis' logical space is a fixed 1280 wide and only the height varies with
aspect, so every size in the layout is a constant fraction of the screen's
*width*. On the frame the OSD was drawn for — 915x412, which is 1280x576 here —
that lands correctly. On a shape with far more height per unit of width (an
unfolded foldable is about 1280x1436) the same controls sit in two and a half
times the vertical field and read as small islands.

Both obvious rules miss. A constant fraction of the short edge is what "the same
size" means on paper but overshoots badly on a physically larger display — a play
button around 150dp. A constant dp size is the other extreme and is what looked
too small. The square root of the aspect ratio sits between them: 1.0x on the
landscape phone the design was drawn for, about 1.5x on an unfolded foldable.

The cap is a judgement, not a derivation. If the OSD comes out too large or still
too small on some screen, that constant is the one to move.

### borealis traps worth knowing

- `setCustomNavigationRoute()` calls `fatal()` — an uncaught `std::logic_error` —
  when the receiving view is not focusable. That has cost three separate crashes.
  A route to a view that cannot take focus means nothing anyway, so skip it.
- `View::isFocusable()` tests only the view's own visibility, so a button inside
  a GONE box stays focusable and hit-testable, and the highlight lands on a
  zero-sized view.
- `hideControls()` cannot move focus off the controls, because `giveFocus()`
  resolves through `getDefaultFocus()`, which refuses a view that is not VISIBLE.
  The invisible button keeps focus and answers A, so its action is marked
  unavailable instead and the press falls through to the activity.
- A `ScrollingFrame` traps UP navigation at its first row; route that row
  explicitly to something stable above it.
- `PanGestureRecognizer` rebases `startPosition` when it promotes UNSURE to
  START, so both of those states report a zero delta. STAY is the only state
  that reports motion, and it fires every frame the finger is held.
- `giveFocus(nullptr)` does nothing, so deleting the focused view leaves
  `currentFocus` pointing at freed memory, and the focus stack does the same for
  a view that had focus when an activity was pushed over it. Move focus before
  freeing, and free nothing on a covered activity: pushing one tells the
  activity below nothing (no `willDisappear()`), so its async work runs on.
- When any recognizer returns START or END, borealis interrupts every recognizer
  still UNSURE from the touched view up. The pans start after 6 units, about
  5 px on a 1080p phone, so ordinary finger drift interrupts a hold inside a
  scroll view, and a tap's END interrupts the hold beside it.
  `LongPressGestureRecognizer` checked for INTERRUPTED before handling START,
  so one interruption disabled the cell's hold for good and its tap fired
  instead. It now starts afresh on every touch, and a hold stands while the
  finger stays within `MAX_MOVEMENT` (25 units).

### Reading the MPV stats overlay

Once a second, enough to tell why playback is choppy without an adb cable:

| Reading | Means | Try |
|---|---|---|
| `decoder-frame-drop-count` high | decode cannot keep up | hwdec change, `profile=fast`, `vd-lavc-fast` |
| `frame-drop-count` high | vo/display dropping frames | video-sync change, `vo=gpu` |
| `estimated-vf-fps` far below container fps | render path is the bottleneck | — |
| `paused-for-cache` true, low `cache-speed` | network cannot sustain the bitrate | lower quality, or use the local network |

---

## Live TV

`/livetv/epg/channels` is documented as available to a shared user, and this
server answers 403 for a non-owner account anyway. The provider-namespaced
`/{provider}/lineups/dvr/channels` works for the same account and carries the
channel logos, so it is the fallback.

Each `/:/timeline` ping resets the server's 300-second rolling-subscription
stop-grab timer. Without it the grab dies after about five minutes, the universal
transcode session it feeds is killed, and mpv starts getting 404s on the next
playlist refresh.

---

## Timeline reporting

Two rules, both learned from a log where the app pinged the server every ten
seconds for nine minutes after a queue had finished, with the screen off.

**A stopped session gets one report, not a heartbeat.** `playing` and `paused`
are repeated on purpose — Plex keeps the session alive off those pings, which is
what makes it resumable from another client and visible in Now Playing. `stopped`
carries no session to keep, so repeating it is pure churn. The periodic repeat is
therefore gated on the player still being in `playing` or `paused`; a resume is
picked up by the state-change branch, not the timer.

`LOADING` and `BUFFERING` are neither, so they used to fall through to `stopped`
and announce the end of playback every time the queue advanced between tracks.
They now report nothing and wait for the next real state.

**`time` may not exceed `duration`.** mpv parks a few tens of milliseconds past
the container length at EOF — 163085 against a 163000 ms track — and Plex answers
`400` to a timeline beyond its own duration. The final report of a finished
track, the one that sets the resume point, was the only one that ever hit this,
so it was the only one being thrown away. It is clamped now. The separate
`duration + 30s` guard still drops a genuinely corrupt transcode's position
spike, and is judged on the raw value before the clamp.

---

## Security

mpv quotes the whole URL back in its stream errors, and every URL the client
hands it carries `X-Plex-Token`. The log bridge passed `msg->text` straight to
the logger, so a failed open printed a long-lived token in full — in a log the
user then pastes into a bug report. Every level now goes through
`redactTokensInUrl()`.
