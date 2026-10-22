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

### Vita: background audio (settled in build 2047)

Where it ended up (build 2047, below): VitaPlex.vpk is a non-game app that
plays on after PS, and as the system closes it for a game, its quick menu
plugin plays the queue on from inside SceShell. What follows is how that was
found, test by test, in the order it was.

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

**PS hands the music over and it plays through a game; after the player was
ended, 20 could not read app0:; 21 crashed again as it went** (logs
`7d5ae5e7`, `0f24166b`, `5e625dcf`, `cf918ac6` and `35df39c3`, build 2011;
each run on its own clock).

- 20 was sent away with PS at 86.6 s. It copied its CA bundle (188,900 bytes
  in 0.21 s), the ask returned 0, and the rescue ended it 2 s later. The
  player started 1.29 s after the ask, used the copy and downloaded song 1
  (3.07 MB in 2.7 s), and the shell played it from 95.4 s. BG Test 8 opened
  at about 105 s, and the song played on beside it, as the user heard.
- 20 was opened again at 157.5 s. Its player took the request in 0.10 s,
  stopped the music at 1:07 and gave the BGM port back. The system ended it
  half a second later, and it was gone 0.74 s after the ask. Then X, 10.4 s
  later, found no `app0:cacert.pem`: the app could no longer open its own
  CA bundle. The ask was refused (0x80010013, then 0x80106501), five times
  over 15 s, and again on PS and on the quit request. Nothing read app0: in
  that run before X, so when it went is not known. That 0x80010013 is the
  errno range's ENODEV fits a start that cannot find eboot2.bin.
- Every refusal so far came after a player had ended while its app was
  open, or after its app had been opened over a player that ended later. The
  one X that went through after an ending came 0.9 s after it (log
  `753b3fb0`); the refused ones 6 s and 10.4 s after. A player that loses
  app0: when its app ends under it (log `5d35ef15`) and an app that loses it
  when its player ends would fit one mount shared by the two processes.
  That is a reading of the logs, not something they show.
- 21 was opened after 20 and found no player of its own, and the shell
  silent at 67.4 s. 158 s later PS: the copy was already there, the ask
  returned 0, and the rescue ended 21 at 160.25 s. The player started 1.13 s
  after the ask, and the shell played song 1 from 165.8 s. 21 stayed listed
  until 170.5 s, 10.3 s after it had ended itself; in build 2010 it was gone
  0.76 s after, its player idle then, and 20 here 0.4 s after. The player's
  last line is at 170.53 s, 19 ms after it saw 21 gone, 0:05 into song 1,
  with song 2 just downloaded. Then the console crashed, the user said.
- Both crashes came with 21, the app with extended memory, going while the
  shell played: 20's song as X ended 21 (build 2010), and 21's own here. In
  every run of 21 that played without a crash (logs `2aefa839`, twice, and
  `0b39eeea`), the player found 21 already gone at its first look, before it
  took the BGM port. No run of 20 crashed.

So build 2012 keeps the player alive while its app is open. Opened again, 20
and 21 ask the player to pause (a note of their own, like the stop): it
reads where the song is, stops the shell, gives the BGM port back and stays.
X, PS and the quit request then ask the running player for the music again,
in a third note, instead of starting another. The player waits for its app
to go and hands the song over again. It seeks back to where the song was
paused, unless the shell is there already: the shell kept its place through
a stop (67.4 s above). A player not asked again within 10 minutes quits. Triangle ends the player
through the system instead, as build 2011 did, to see whether app0: goes
with it and the next start is refused. Both the app and the player log
app0: as it changes, from `sceIoGetstat` on `app0:eboot2.bin`, with the
error code. The player of 20 and 21 does nothing until its app has gone,
for up to 30 s, at its start as well as before going on after a pause. Each
log line of 14 to 21 and their program is followed by `sceIoSync("ux0:")`,
so that a crash keeps what came before it. A host run covered these: pause
and play notes, the take-back and X with the player paused, Triangle, app0:
going, and the player paused part-way (with the shell keeping its place or
not), before song 1, with its app still there at the start, and never asked
again. The earlier paths still pass.

**Paused and asked again, the player goes on with no start refused; ending
it takes app0: from the app with the refusal's own code; 21 plays without a
crash** (logs `26b60329`, `c2cb7096`, `1aaa6411`, `3ddd1547` and `f8b59a55`,
build 2012; each run on its own clock). `sceIoSync("ux0:")` returned 0 in
every process.

- 20 was sent away with PS: the CA bundle was copied (188,900 bytes in
  0.23 s), and the ask returned 0. The player started 1.27 s later, found
  its app still there and waited. At 15.136 s app0: gave it 0x80010013, and
  1 ms later it found the app gone, 1.01 s into the wait. It used the copy
  and the shell played song 1 from 22.4 s.
- 20 was opened again three times. Each time the same player, process
  0x0146343D, took the pause request in 0.10 to 0.31 s and stopped the shell,
  which stayed at the song's place (state 2 at 24.7, 36.1 and 50.7 s). Then
  PS, X and PS asked it for the music again, and nothing was started or
  refused. After PS it waited 2.08 and 2.04 s for 20 to go; after X, 20 had
  gone already. Each time the shell started the file again at 0:00, not
  where it had stopped, and the seek event took it to 0:24, 0:36 and 0:50 in
  0.3 s.
- The fifth time, Triangle ended the player, paused at 1:20, through the
  system. It was gone 0.10 s later, and 3 ms after that, 20's own app0: gave
  0x80010013. PS 9 s later found no `app0:cacert.pem`, and the ask was
  refused with 0x80010013, then 0x80106501, as it was on the quit request.
  So the refusal's code is app0:'s own: an app loses app0: when its
  background program ends while it runs, and the start then cannot reach
  eboot2.bin. A program loses it as its app ends (15.136 s above). One
  started after its app had gone (21's X below) has app0: of its own.
- 21 was sent away with PS. Its player started 1.10 s after the ask and
  waited for 21, which went 1.00 s later, 0.2 s after its rescue's system
  call. It had taken 10.3 s in build 2011, while the player worked. The
  shell played song 1 from 24.7 s, and nothing crashed.
- 21 was opened again. Its player was gone, ended as 21 opened, and the
  shell played on at 26.8 s. 21 stopped it: 0.5 s later the shell was in
  state 2 at 27.2 s. X: the ask returned 0 and 21 quit. The new player found
  21 gone at its first look and app0: readable, with the CA bundle there
  too, and played song 1. Nothing crashed. The user went on with the music
  playing.

So for VitaPlex BG, whose param.sfo 21 has:

- The app never ends its background program while it is open. Asked to
  pause, the program stops the music and stays, and the app asks the same
  program for the music again as it goes.
- Opening VitaPlex BG ends the program by itself, as opening 21 did, since
  it has extended memory. The song plays on in the shell, where VitaPlex BG
  can read its place and go on with it.
- The program does nothing until its app has gone, and keeps everything it
  needs, the CA bundle and the songs, in ux0:data.

**VitaPlex BG's background player** (`src/bgplayer_psv`). VitaPlex BG now
carries the background tests' player as its own background program:
`eboot2.bin` in `VitaPlexBG.vpk`, with a `param2.sfo` giving it the title id
`VPLEXPLAY`. It is one package to install, with no bubble of its own. It
plays the app's queue, not two songs from the library. The two talk through
files in `ux0:data/VitaPlex`, each written beside its place and moved in
(`bgplayer.h`):

- The queue: the app's songs in the order they play, from the one playing,
  where it had got to, and the repeat mode. For each song it has the
  ratingKey, its length, a download on the card if there is one, and what
  the app shows (title, artist, album, cover, rating). So the app can set the
  queue up again without asking the server.
- The status: the song the player is on, its place in the queue and the
  song, and whether it plays. The player writes it every 2 s and at every
  change.
- A pause request and a play request, as 20 and 21 had. The player removes a
  request only after acting on it, so the status already says where the music
  stopped when the app sees the request taken.

The player waits for its app to go before anything else. For each song it
uses, in order:

1. the app's download, if the shell decodes it;
2. the file if one is already in `ux0:data/VitaPlex/bgplayer`;
3. the server's file if the shell decodes it;
4. else the server's MP3 behind a Xing header.

Downloads go to a `.part` file and are moved in once whole. The next song
downloads while one plays, and goes over the moment the song ends. Telling
the server it was played (`/:/scrobble`) waits until the next one is with the
shell. Repeat one and repeat all are kept. A song the server has nothing for
is skipped, and one that will not download is tried again after 10 s, up to
three times; three songs in a row that will not play stop the queue.

A pause stops the shell and any download, and the player stays. A play
request has it wait for the app to go and read the queue again. With no new
queue it goes on with the paused song where it stopped. It never ends while
its app is open. When the queue is over it waits, and it quits once the app
has gone without asking.

It keeps the power button handling, the Vita kept awake while a song plays,
the synced log (`ux0:data/VitaPlex/bgplayer.log`, wall-clock times) and the
raw exit (`raw_exit.S`, shared with VitaPlex). A host run covered 17 cases
under ASan and UBSan: the queue whole, a start part-way in, a file the app
made ready, a pause then a new queue, then the same queue, then the app going
without asking, a quit request, both repeat modes, songs that will not play,
a start at a song's last seconds, an app still there at start, the queue
over with the app open, a dropped connection, a pause during the first
download, and the power button. BG Tests 14 to 21's harness still passes
with `mp3_info.c` moved here.

**Which apps end a background program.** The test's player outlived
VitaShell, while opening 21 ended 21's own every time. That is no proof
against extended memory doing it: in the beep tests the beeps went on beside
VitaShell and went quiet beside BG Test 7 and VitaSurf. So VitaShell behaved
as an app without extended memory. VitaShell's CMake sets no `ATTRIBUTE2`;
vita-mksfoex now gives `MEM109` (12) by default, but the user's copy may
predate that. The player now reads every installed app's `param.sfo` once
and logs the ones with `ATTRIBUTE2` set, then logs every 2 s which apps start
and go beside it (`apps.c`). If the player is ended, the last app logged as
starting is the likely cause.

**When the app hands over.** VitaPlex BG alone plays on after PS, through the
BGM port, with the UI loop running (log `a93baa71`). What it cannot survive
is the system closing it to start a game. So the setting "Music After Leaving
VitaPlex" has three choices:

- Stops.
- Plays on, then the background player (the default). VitaPlex BG keeps
  playing after PS, with no gap, and hands the queue over when the system
  asks it to quit. The cost is the one 20 and 21 measured: once an app has
  used SceBgAppUtil on a quit request, the system starts the game about
  10.8 s after asking.
- Background player at once. PS hands the queue over (a deactivation within
  2 s of a PS press, as 20 and 21 had) and VitaPlex BG quits, so a game
  started next finds nothing to close.

The hand-over writes the queue, copies the CA bundle, and asks for the
player. A player still running, paused, gets a play request instead of being
started again. Only once the player has it does the app pause mpv and give
the BGM port back, so a refusal leaves the music playing in VitaPlex BG. The
app then quits. From then on the process ends through
`_sceKernelExitProcessForUser`, since `sceKernelExitProcess` did not return
in 18, 20 and 21; the watcher does so itself 10 s after the hand-over if
nothing else has.

Opened again, VitaPlex BG takes the music back once its first frame is up:

- If the player is listed, the app asks it to pause, and goes on from the
  song and place the player wrote.
- If it is not, the system most likely ended it as VitaPlex BG opened, as
  happened to 21's. If its last status said it played, in the 20 s before
  the app started, the app reads the shell's clock. When the song the shell
  plays has the player's song's length (to 3 s), the app stops it there.

The queue then plays in VitaPlex from that song and place, set as it was
played, with no server play queue or shuffle
(`PlayerActivity::createWithRestoredQueue`). Its first song starts part-way
in: a transcode with Plex's `offset` (`MusicController::setStreamStartOffset`),
or a seek into a download. A song the system Music app plays has another
length and is left alone. The request to quit is now logged with the event's
bytes, since the app cannot yet tell a game started over it from its
LiveArea closed: both hand the music over.

**Build 2014 on the console** (logs `24755753`, the player's, and `c0ea1fd8`,
VitaPlex BG's, from one session). VitaShell was opened over VitaPlex BG
playing, with the setting at "plays on":

- The player took the 15 songs over at 0:25 of the first. The seek took
  0.3 s, the shell had the song's length as the server has it (6:21), and the
  next song downloaded in 3.7 s while it played.
- VitaShell is `gd` with `ATTRIBUTE2` 0, an app without extended memory, as
  the paragraph above supposed. 32 of the 209 apps installed have it set.
- The silence: the request to quit came at 21:10:45.030, and the app had
  handed over by .886 (copying the CA bundle took 0.54 s of that). Its last
  line was at 21:10:47.110 and the player's first at 21:10:51.292. The song
  was not on the card, since the app streamed it, so the player downloaded
  it whole: 5.7 MB in 6.8 s. Music again at 21:11:02.6, 17.5 s after the
  request. The 4.2 s between the app's end and the player's start are the
  system's. The download is not: the app had been away 18 s before the
  request, time enough to have saved the song for the player.

**The quick menu's Music row** (log `c0ea1fd8`). All five music player client
types and the application-BGM service played a downloaded MP3. The user held
PS (power callback bit 0x10000000) during each, and the row stayed dimmed
for every one. The shell's track info for the file had its length and an
empty title.

The user remembers the row lit once, with VitaPlex's icon, in a build they
cannot name. What the builds in these notes did with the BGM port:

- mpv: 0x81, ElevenMPV-A's priority for its own decoders, and only with
  background playback on.
- The shell tests: 0x80, ElevenMPV-A's priority while the shell decodes,
  held from the first test until mpv plays again or the test is stopped.
- The released VitaPlex: never. mpv's Vita output opens a BGM port below
  48 kHz and the MAIN port at 48 (commit `ea50798`).

ElevenMPV-A's shell plugin caps the priority SceShell reads
(`sceAppMgrGetCurrentBgmState2`, SceAppMgrUser 0x62BEBD65) at 0x80, so a
priority over 0x80 is the likely reason the row is dimmed for an app. mpv
playing while a shell test still held 0x80 is one way an older build could
have put VitaPlex's icon there. VitaPlex BG now has a picker for it, "Music
Port for VitaPlex's Player (test)", not saved:

- 0x81, as before;
- 0x80;
- `sceAppMgrAcquireBgmPort`, with no priority, as EQVita's preview takes it;
- `sceAppMgrAcquireBgmPortForMusicPlayer` (SceAppMgr 0x44EC36FA, arguments
  unknown, one 0 passed);
- not taken.

Each but the first applies whenever mpv plays. For 30 s after PS is held (a
minute from the next build), VitaPlex BG and the player log the BGM owner
whenever it changes, as
`_sceAppMgrGetCurrentBgmState` (SceAppMgr 0x81569A58, `appmgr_bgm_stub.S`)
gives it, with mpv's state in VitaPlex BG's lines. App events nothing else
handles are logged with their bytes, so a button on the row that reaches the
app shows up.

**As a music player, the row shows VitaPlex** (build 2015, logs `9c16c0d6`
and `2114643f`). With `sceAppMgrAcquireBgmPortForMusicPlayer` (it returned 0)
and mpv playing, the quick menu's Music row had VitaPlex's icon, in front and
from the LiveArea. Its buttons were dimmed, and no app event came.
`_sceAppMgrGetCurrentBgmState` is refused to apps and to the background
program alike (0x8080201F), as `sceAppMgrGetCurrentBgmState2` was. So the
call names the music player the row is for. That the row's buttons stay
dimmed with nothing playing in the shell's music service fits them being that
service's controls, as they are for the system Music app. They have not yet
been tried with the shell playing for an app that took the port this way.
So the picker now applies to the system player tests too, and the queue
tells the background player how to take the port (`port=` in `bgplayer.h`).
"0x80" is the default, and "none" leaves it untaken. While PS is held and
for a minute after:

- VitaPlex BG logs the shell's state beside mpv's.
- The background player logs every jump in the shell's clock.
- A stop in that minute is taken as the row's pause: the player does not
  play the song again every 5 s as it does after the power button, and does
  not move on after two minutes. It waits for the shell to play again.

**As a music player, the shell will not play** (build 2016, log
`dbd9b556`). With the port held that way, and mpv paused for the system
player test, the shell took the file (`Open` returned 0, and its track info
then had the file's length) but stayed at state 10, with no sound for 12 s.
The tests before, with the port at 0x80, went to state 1 and played. The
Music row's buttons stayed dimmed with mpv playing and with the shell held
like that. So the call makes the system show VitaPlex as the music player,
and keeps the shell's own player from playing for it. ElevenMPV-A, whose
author knows SceShell best, never makes the call. It takes 0x81 or 0x80, and
lights the row's buttons from a SceShell plugin. The one case left is the
shell playing with no port taken at all, as it may then take the port
itself, as it does for the Music app. "Not taken" now means no port for the
system player tests too, as it does for mpv and the background player.

**The row through ElevenMPV-A's plugin.** No app-side call lit the row's
buttons, so VitaPlex BG uses the plugin ElevenMPV-A lights them with
(`qm_link.c`). The ElevenMPV-A app (GPL-3.0, GrapheneCt/ElevenMPV-A at
`ae6583e`) works like this:

- It loads `module/shell_plugin.suprx` from its own install into SceShell
  (`taiLoadStartModuleForPid` on `NPXS19999`), unless the plugin's pipe
  `ElevenMPVA::ShellIPC_RX` is there already.
- It sends whole `IpcDataRX` messages (1548 bytes) down that pipe: APP_START
  as it starts, ACTIVATE while it plays, INFO with the title, artist and
  album in UTF-16. The row's top line is the title, the bottom one
  "artist / album".
- It reads PLAY, FF and REW back from `ElevenMPVA::ShellIPC_TX`.

Build 2018 loaded the same file from ElevenMPV-A's install,
`ux0:app/GRVA00008/module/shell_plugin.suprx`, so ElevenMPV-A had to be
installed as well. From build 2019 VitaPlexBG.vpk carries it, as the v7.10
release has it, in the same place in its own folder:
`ux0:app/VPLEX0002/module/shell_plugin.suprx`. That copy is loaded first, and
ElevenMPV-A's is only tried after it.

- The plugin is GPL-3.0, VitaPlex Apache-2.0. The plugin is a program of its
  own that VitaPlex only talks to through pipes, so the package is an
  aggregate of the two, and each keeps its own licence.
- Shipping it means giving its licence and its source with it.
  `third_party/elevenmpva-shell-plugin` has the file, ElevenMPV-A's licence
  and the plugin's source from the v7.10 tag, each file the same git blob as
  upstream's.
- The package puts the licence and a note of where that source is beside
  the plugin (`module/shell_plugin.LICENSE.txt` and
  `module/shell_plugin.NOTICE.txt`).
- The release file's bytes are the ones committed at that tag, beside the
  source. The plugin builds only with Sony's SDK, so it is shipped as
  released, not built.

Whichever of VitaPlex BG and ElevenMPV-A loads it first, the other uses that
copy through the pipes. Three calls the SDK leaves out come through
`qm_link_stub.S`:

- `sceKernelOpenMsgPipe` and `sceKernelCloseMsgPipe`, from SceThreadmgr in
  vita-headers' db.
- `taiLoadStartModuleForPidForUser`, taihenUnsafe 0xB2B67326 and 0xE853ABCB,
  as `libtaihen_stub.a` has them in vitasdk's taihen package and in taiHEN
  v0.11 alike. The library is for unsafe homebrew, which both of VitaPlex's
  programs already are.

A whole message goes or none does with the pipe mode 0x1 (Vita3K's
`SCE_KERNEL_MSG_PIPE_MODE_FULL`). Each send waits 200 ms at most.

The plugin's buttons send a press with no time limit from SceShell's own
thread, into a pipe of one message's size. So a process that left the row
on and stopped reading would freeze the quick menu at the press after the
pipe filled. Whoever has the row therefore reads it four times a second, and
gives it back as the music leaves it:

- The app gives it back as it hands the queue over and as it is asked to
  quit.
- The player gives it back whenever it stops the shell.
- A press left in the pipe from before is dropped when the row is taken.

Not covered: the player ended by the system while it has the row, which an
app with extended memory may do. That has not been seen yet.

In VitaPlex BG, MusicController's now-playing bridge (`now_playing.hpp`)
feeds the row, as it feeds Android's media session, and the row's presses
come back through `dispatchTransport`. The queue tells the background
player (`qm=1`):

- **Play/pause** is the shell's STOP and PLAY, as ElevenMPV-A pauses songs
  the shell plays. The shell keeps its place through a stop, and the player
  leaves such a stop alone.
- **Next** stops the song at once, without marking it played, and goes on to
  the next, past repeat one.
- **Previous** goes back to the song's start after its first 3 s, and to the
  song before within them.

A host run of the player covered these three as well (23 cases), and a host
test of `qm_link.c` checked the messages byte by byte: the commands, the
UTF-16 text with a surrogate pair and a stray byte, a long title cut to fit,
and the presses. "Quick Menu Music Controls" in VitaPlex BG's settings turns
it off; it is on by default.

**Build 2019 on the console: the row works, its buttons stay dimmed** (logs
`530d89d8`, the player's, `bfd9c56c`, VitaPlex BG's, and a screenshot).

- VitaPlex BG loaded its own copy of the plugin
  (`ux0:app/VPLEX0002/module/shell_plugin.suprx`, module `0x403D09F3`).
- The row showed the title above and "artist / album" below, in white.
- Play/pause, previous and next all reached VitaPlex BG; next went on to the
  next song, whose title the row then showed.
- The three buttons stayed drawn dimmed throughout, though they worked. Play
  showed ▶ while the song played.

ElevenMPV-A's binary imports `ButtonBase::Enable(bool)` (ScePafWidget
`0x8F1A3A59`) and calls it with false. PAF's headers (below) give
`ButtonBase` an `Enable(bool)` and a `Disable(bool)`, a state
(`ST_BTN_NORMAL` 0, `ST_BTN_DISABLE` 1, then the pressed ones), a virtual
`ChangeButtonState` and a colour of its own for the dimmed state. So
`Enable(false)` most likely lets presses through and leaves the button
looking dimmed. The text was dimmed too, until the plugin set its colour.

Two more things in these logs:

- The hand-over at 23:10:30 was silent for about a minute and a half. The
  player fetched "Bad Day" (5.4 MB) at about 60 KB/s: 0.4 MB in 10 s, 1.2 MB
  in 20 s. Its status had it playing 1:10 in at about 23:12:36, having
  started at 0:32. Build 2014's player fetched 5.7 MB in 6.8 s.
- At 22:54 the player's log stops 0.3 s after it started, before it asked the
  server for the song, with no request to stop. Ended by the system, most
  likely: 35 of the 213 apps installed have extended memory.

**VitaPlex's own quick menu plugin** (`src/qm_plugin_psv`, from build 2020).
Lighting the buttons, the play/pause icon and the artwork all need code in
SceShell that ElevenMPV-A's plugin does not have. That plugin builds only
with Sony's SDK. Its replacement builds with vitasdk:

- **PAF, the UI library the quick menu is made of.** vita-headers declares
  about 20 of its C functions and none of its widgets. GrapheneCt's ScePaf-RE
  has the C++ API with its NIDs, restored from debug data.
  Princess-of-Sleeping's vitasdk-paf-component (MIT) patches it for vitasdk.
  `scripts/vita/fetch_paf_headers.sh` fetches both at pinned commits, since
  ScePaf-RE has no licence file; its install would also replace the SDK's
  own PAF stubs, so it is not used.
- **Imports.** `paf_stubs.S` has the 22 PAF imports the plugin needs, written
  by `scripts/vita/gen_paf_stubs.py` from ScePaf-RE's NID table. Each NID is
  its name's hash with PAF's key. 18 of them are ones ElevenMPV-A's binary
  imports. They cover the plugin built at -O0 as well as -O2, so a compiler
  that inlines less finds them all. Build 2020 failed on CI's GCC 15 (the
  local check was GCC 10): ScePaf-RE's vector uses `std::numeric_limits`
  without `<limits>`, which GCC 15 now finds in template bodies nothing
  instantiates. The plugin includes `<limits>` first and builds with
  `-Wno-template-body`.
- **Checked against ElevenMPV-A's binary**, unpacked and disassembled:
  - its virtual calls for colour and text go through slots `0x98` and
    `0x11C`, ScePaf-RE's `SetColor_core` and `SetString`;
  - it calls `_sceAppMgrGetAppState(&state, 0x80, 0x03570000)`;
  - its pipes are made with type `0x40` and attributes `0x81`;
  - its threads run at priority 191.

How it differs:

- **The buttons.** It switches them on with `Enable(true)` by default.
  "Quick Menu Buttons (test)" in VitaPlex BG's settings picks that, that
  plus `ChangeButtonState(normal)`, or ElevenMPV-A's way. While the quick
  menu is open it checks the buttons ten times a second and switches back on
  any that SceShell has switched off again, in case SceShell sets the row up
  after it does.
- **Its own protocol** (`qm_protocol.h`), with the version in the pipes'
  names, so a newer VitaPlex never talks to an older plugin still loaded
  that boot.
- **No freeze.** A press is sent without waiting and dropped if nobody reads
  it. The owner asks for the row again every second, and the plugin gives it
  back by itself after 5 s without, as for a player the system ended.
- **The log.** `ux0:data/VitaPlex/qm-plugin.log` holds each button's state
  before and after. Once a boot it lists the row's widgets: their type, id,
  place, size, colour and text, for the artwork and the play/pause icon
  next. It reads widget members only once the layout is shown to be the
  headers': the play button found by its id reports that id, and is among
  its parent's children.

It also caps the BGM priority SceShell reads at 0x80, as ElevenMPV-A's
plugin does. If ElevenMPV-A's plugin is in SceShell from earlier in the
boot, VitaPlex asks it once, without waiting, to give the row back.
VitaPlex falls back to ElevenMPV-A's plugin when its own is missing, will
not load, or opens no pipes. The plugin is GPL-3.0 like the one it is based
on; VitaPlexBG.vpk carries its licence and a note of where its source is.

**Build 2022 on the console: the plugin works, the row still looks dimmed**
(logs `4db8baa2`, the plugin's, `a334ea6b`, VitaPlex BG's, and `f68246cc`,
the player's).

- VitaPlex BG loaded its own plugin. ElevenMPV-A's was in SceShell too,
  from earlier that boot, and was asked to give the row back.
- The layout check passed: the play button, found by its id, was among its
  parent's 10 children.
- Each time the quick menu opened, the plugin took the row at once. The
  buttons went from enabled 0, state 1, to enabled 1, state 0 (normal).
- All three buttons worked, for VitaPlex BG and then for the background
  player, which got the row with the queue.
- With the player, SceShell switched the buttons off again 110 ms after the
  plugin had switched them on, and the plugin switched them back on.
- On the screen the buttons still looked dimmed. The song did not show when
  the quick menu opened, only after a song change while it stayed open.
- The hand-over was silent for about 6 s. The player fetched 5.31 MB in
  3.1 s and played 0.8 s after it handed the file to the system player.

What the plugin's list of the row says:

- Each button has a 64x64 plane at its place: previous `0xD639AB27`, play
  `0xE057A7D0`, next `0x6FA104CC`. A fourth, `0xB26E4562`, of size 0x0, is
  also at play's place. Each is listed just before its button, so they are
  most likely the icons. A button's state does not reach a plane beside it,
  which fits buttons that work and still look dimmed.
- They all read colour FFFFFFFF. PAF's headers give a widget's total alpha
  as its colour's alpha times two products of its own alphas and its
  parents': show and meta (`GetTotalAlpha`). "Active" in the list meant a
  show alpha of at least 95%. The meta alpha was not read.
- A third line, `0x5608C548`, "There are no songs.", is at the title's place.
- 390 ms after the open, the title line still held the title as set. So
  either SceShell hid it some other way, or text set that early was not
  drawn. ElevenMPV-A's plugin waits 100 ms after it sees the quick menu open
  before it looks for the buttons and sets the lines, and its lines showed
  (build 2019).

Build 2023:

- **Later, and kept.** The plugin takes the row 200 ms after it sees the
  quick menu open. While it stays open, ten times a second, it puts back
  what SceShell changed: buttons switched off, other text or colour on the
  lines, dimmed icons.
- **Icons undimmed.** A new look, "Enabled, icons undimmed", is the default.
  It switches the buttons on, and sets the icons and buttons back to white
  and full meta alpha wherever either was lowered. The plugin writes the
  meta alpha into the widget itself, as `SetMetaAlpha_core` would.
  ScePaf-RE has that export only from a prototype's debug data, and a NID
  the console lacks would keep the whole plugin from loading. It imports the
  same 22 NIDs as build 2022.
- **A timeline.** For the first 8 opens of a boot, the plugin logs the
  widgets of the row and its heading as the quick menu opens. It then logs
  each change to them for 3 s. Each line has the widget's state, colour,
  both alphas (its own and the product with its parents'), size, and
  texture with its size; a button's state and dimmed colour; a line's text.
  The plugin logs its own changes as it makes them, so the timeline shows
  SceShell's. It logs opens where the row is SceShell's too, so the system
  Music app playing gives the lit look to compare.
- **The BGM port as SceShell reads it**, its first 40 changes in a boot.
- **Protocol 2.** The pipes are `VitaPlexQM2::…` and the module
  `VitaPlexQM2`, so a build-2022 plugin still loaded that boot is left
  alone, and the new one loads beside it.

**Build 2023 on the console: the lines transparent, the fade elsewhere**
(logs `a41b1550`, the plugin's, `fc92649a`, VitaPlex BG's, `ee7ee49a`, the
player's, and two screenshots of the row).

- The plugin took the row 266–373 ms after each open, switched the buttons
  on (state 0), and every press worked.
- The song's lines did not show: the plugin itself had set their colour to
  00000000. Its white was a global object, whose constructor nothing ran,
  since the module has no C runtime, so it stayed all zeros. The plugin then
  "put back" that colour ten times a second.
- SceShell had set the row up before the plugin first saw it open: lines
  empty, buttons enabled 0, state 1. It changed nothing after the plugin
  took the row, unlike build 2022's take at 0 ms.
- The quick menu fades in: every widget's meta alpha product rose from
  14–26% to 100% in about 270 ms.
- Buttons and icons read colour FFFFFFFF, show and meta alpha 100%, in every
  open, the row SceShell's or the plugin's, so there was nothing to undim at
  that level. Each icon has a 64x64 texture of its own. The pause icon
  (`0xB26E4562`) has one too, at size 0x0. The three buttons share one,
  100x60, their background.
- "There are no songs." (`0x5608C548`) read visible in every open, yet the
  screenshot of the row with no music shows no text at all.
- Screenshots: with no music of SceShell's own, buttons and icons are faded
  and there is no text. With its Music app playing, the buttons are opaque
  with white icons, ⏸ in play's place, the title, and the album below.
- With the row given back and the quick menu still open, presses went to the
  plugin, which dropped them.
- Another of SceShell's overlays, the LiveArea's on the way out, also read as
  the quick menu open. The plugin found no buttons in 3 s.
- The hand-over was silent for about 6 s again: 2.96 MB in 3.4 s.

So SceShell fades the buttons, their icons and that line somewhere the
widgets do not show: in their draw objects (`graph::DrawObj`), most likely,
which have a colour and a visible bit of their own.

Build 2024:

- **White lines.** The colour is made where it is used. CMake now fails
  the plugin's build if it has static constructors
  (`cmake/check_no_static_init.cmake`).
- **"Enabled, lit"**, the default: `Enable(true)`, then
  `ChangeButtonState(normal)`, which `PlaneButton` has a version of its
  own. For each button and icon whose draw object is faded, the widget's
  colour is set with `SetColor_core`. The draw object's colour is then set
  to FFFFFFFE: white, with an alpha that marks it as the plugin's. That is
  done only if the draw object's size reads as its widget's, so that its
  layout is the headers'. The log has each colour before and after.
- **Put back.** When the row is given back with the quick menu open, the
  lines are emptied, the buttons switched off, and the draw objects get
  SceShell's colours back. The next time the quick menu opens with the row
  SceShell's, any draw object still in the plugin's colour gets SceShell's
  back.
- **The timeline** has each widget's place, and its first draw object's
  colour, visible bit and size, for 12 opens a boot.
- **Protocol 3**: pipes `VitaPlexQM3::…`, module `VitaPlexQM3`.

**Build 2024 on the console: the song shows, the buttons still look off**
(logs `0e16ecae`, the plugin's, and `84df1537`, VitaPlex BG's).

- The song's title and "artist / album" showed.
- Every draw object's size read as its widget's (64x64 icons, 106x46
  buttons, 290x40 lines), so their layout is the headers'.
- The icons' and buttons' draw objects read FFFFFFFF too, with the row
  looking off. So no colour or alpha the plugin can reach is what makes them
  look off, and the lit look found nothing to light. The visible bit read 0
  for every draw object, the visible heading's too, so it is not that bit.
- The lines' draw objects read D8FFFFFF and CCFFFFFF, which no widget
  colour gives: a text object keeps its colour in another form.
- The groups were at other places in the background player's open than in
  VitaPlex BG's: SceShell makes the row anew at least sometimes.
- The plugin had "playing" and "paused" from VitaPlex at each change. It
  showed nothing of them yet.

What is left between the two looks is the images and how they are drawn.
Each plane's draw object (`PlaneObj`) has four texture slots, and every draw
object has a shader. SceShell's own row lit by its Music app shows ⏸ where
play is, and the row has a second icon there, `0xB26E4562`, sized 0x0, with a
64x64 texture of its own: the pause icon, most likely.

Build 2025:

- **⏸ while VitaPlex plays.** The first time the row is taken in a boot, the
  plugin holds the play and pause icons' textures, keeping the reference
  `GetTexture` adds, so they outlive any row SceShell makes. While VitaPlex
  plays (`QM_CMD_PLAYING`), the play icon gets the pause texture
  (`SetTexture`, a virtual call), and its own back when paused, when the row
  is given back with the quick menu open, and at the next open with the row
  SceShell's and its play button off. `intrusive_ptr` is filled and emptied
  by hand, so the plugin imports neither `Surface::AddRef` nor `Release`.
- **The textures and shaders in the log**: each plane's four texture slots,
  and each draw object's class (its vtable) and shader, so that SceShell's
  lit row can be set beside its off one.
- **SceShell's lit row logged.** Past the 12 opens, opens with the row
  SceShell's are still logged, up to the 40th, until its row has been seen
  lit once, its play button on. That open is marked in the log.
- **Protocol 4**: pipes `VitaPlexQM4::…`, module `VitaPlexQM4`.

**Build 2025 on the console: ⏸ works, the buttons still look off** (log
`9beff030`, the plugin's).

- The play icon showed ⏸ while VitaPlex played and ▶ while paused, as each
  change came. The buttons and icons still looked faded.
- SceShell's own row was logged lit, with its Music app playing ("Burn It to
  the Ground", "Nickelback / Dark Horse"). Against the row the plugin took 19
  s later, every widget read the same: type, state, colour FFFFFFFF, show and
  meta alpha, size, each texture (the button background 100x60, each icon
  64x64, the heading's 80x80), each draw object's colour, size, class and
  shader. Its buttons read enabled 1, state 0, as the plugin's do once taken.
  The only differences were the lines' text and the play and pause icons'
  sizes.
- SceShell shows ⏸ by size, not by texture: the play icon (`0xE057A7D0`)
  shrinks to 0x0 while the pause icon (`0xB26E4562`) grows to 64x64, over
  about 0.3 s, and back at the next press.
- The groups were at new places in each of the three opens logged, so
  SceShell made the row anew each time.

So what SceShell lights is in nothing the plugin reads. One thing differs in
how the two rows are changed. SceShell changes its widgets on its own UI
thread, and both plugins changed them from threads of their own. ElevenMPV-A's
app makes every change to its own widgets on its UI thread
(`MainThreadCallList`).

Build 2026:

- **On SceShell's UI thread.** The plugin registers a function with PAF's
  `common::MainThreadCallList`, which SceShell's UI thread calls each time
  round its loop. All the plugin's work on the row now happens there: taking
  it, keeping it, giving it back, the timeline. It runs ten times a second
  while the quick menu is open, and never waits for the plugin's lock, so it
  cannot hold SceShell up. The plugin's own threads watch the quick menu,
  take VitaPlex's messages, and write the log. Lines are put together where
  they happen and kept in a ring of 128, which the watching thread writes,
  so no line waits for the memory card on a thread SceShell draws with. If
  SceShell's thread has not called the plugin within 3 s of its start, the
  watching thread ticks the row itself, as before, and the log says so.
- **The cover**, in place of the Music heading's icon (`0xE219E0DE`, an
  80x80 ♫ at size 0x0, drawn at its texture's size). VitaPlex BG fetches the
  song's thumbnail through Plex's transcoder at 128x128, or reads a
  download's local copy. It decodes it with the stb_image borealis builds,
  scales its middle square to 64x64, and writes it as a PNG whose image data
  is stored, not compressed. One cover after another goes to
  `qm-art-0.png` or `qm-art-1.png` by the lowest bit of its serial, so the
  file being written is never the one the plugin may be loading. Then
  VitaPlex BG sends the serial (`QM_CMD_ART`). The plugin loads the file with
  `LocalFile::Open` and `Surface::Load`, the calls ElevenMPV-A's app loads
  its covers with, into power_manage_plugin's texture pool. It gives the
  texture to the heading's icon, which then holds the only reference. The ♫
  comes back when the row is given back, when another owner takes it, or if
  a cover will not load.
- **Byte dumps.** Once a boot each, a second after the quick menu opens, the
  previous button and its icon, their draw objects, and the button's parent
  group are logged byte for byte: with SceShell's row off, with it lit, and
  with the row the plugin's. The button's parents up to the quick menu's
  root are logged as the timeline logs widgets, since a group's colour or
  alpha would fade everything in it, and the timeline lists no groups. What
  lights SceShell's row is in what differs between its two. A dump goes out
  a part a tick, about 20 lines each, so the log's ring keeps up.
- **Imports.** `MainThreadCallList::Register`, `LocalFile::Open` and
  `Surface::Load` are new, 23 in all. All three are in ElevenMPV-A's app,
  as are their libraries, `ScePafMisc` and `ScePafGraphics`.
- **Protocol 5**: pipes `VitaPlexQM5::…`, module `VitaPlexQM5`.

**Build 2026 on the console: the cover shows, the buttons still look off**
(logs `f9ff7735`, the plugin's, and `5bd97e89`, VitaPlex BG's, and a
screenshot of the row).

- The cover showed in place of the heading's ♫, at 64x64. VitaPlex BG made it
  in 116 ms; the plugin loaded it in 51–54 ms on SceShell's UI thread.
- SceShell's UI thread called the plugin from its first frame, and the
  plugin took the row there, 204 and 240 ms after the two opens. Every press
  worked, and ⏸ and ▶ followed them. The buttons and icons still looked
  off, so the thread was not it.
- In the byte dumps of the plugin's row, the previous icon's field at 0x1AC
  read 0.3 (`3E99999A`), with its show and meta alphas and their products at
  1.0. The button and its group read 1.0 there. ScePaf-RE's headers have
  that field as an unnamed int (`m_unk_1AC`), between the meta alpha's
  product and the colour; its older headers call it `alpha`, a float.
  ElevenMPV-A's app dims a page behind another with `SetAlpha(0.3f)`. The
  timeline had never read it.
- The row's parents, up to the quick menu's page: its group (728x70), a box,
  a scroll view's parent, a scroll view, a plane, the page. Every alpha was
  100%. The box and the scroll view read colour 00000000, the plane
  FF282828, the rest FFFFFFFF.
- Colours keep their alpha in the top byte, whatever the headers' `GetColor`
  says: the background plane's FF282828 is the quick menu's dark grey, and
  the volume slider read 4CCCCCCC as the quick menu opened, then FFFFFFFF.
- The second line, "Dolly Parton / Blue Smoke", was cut at the line's 290
  pixels.
- No open had the row SceShell's, so there are no dumps of its off or lit
  row yet.

Build 2028:

- **The icons' own alpha.** The lit look sets a button's or icon's own alpha
  (0x1AC) to 1 where it reads below 1, after `SetColor_core`. It puts
  SceShell's value back when the row is given back, where it still reads 1.
  The timeline logs each widget's own alpha where it is not 100%, and its
  changes, so SceShell's off and lit rows show theirs.
- **Colour bytes.** The plugin reads alpha from a colour's top byte when it
  looks for faded or hidden widgets.

**Build 2028 on the console: the buttons light** (logs `e1fa293a`, the
plugin's, and `50d172f7`, VitaPlex BG's, and a screenshot of the row).

- As SceShell set the row up, all four icons' own alpha read 30%, the
  pause icon's too. The plugin set the three it lights to 100% as it took
  the row, and the screenshot has ⏮ ⏸ ⏭ white, with the cover, the title,
  and "Various Artists / FIFA 14".
- SceShell uses that field itself. The volume slider opened at own alpha
  30% and colour 4CCCCCCC, and SceShell set both to full 108 ms later. The
  song lines' own alpha read 58% and 46%, then 0% at that time, until the
  plugin put the song on them. "There are no songs." read 0%: that is why it
  never shows, though every other alpha of it read 100%.

So the quick menu's Music row works for VitaPlex: the song, its cover, ⏸ and
▶ as it plays, and buttons that look and work as SceShell's own. Build 2029
drops the byte dumps; the timeline stays.

Build 2030: **the background player's covers.** With the queue handed over,
the row showed the ♫, as only VitaPlex BG made covers. The decoding, square
and PNG writing are now one C file both programs build
(`src/bgplayer_psv/qm_cover.c`); its output is byte for byte what build
2028's C++ wrote, on the host. The player reads each song's cover from the
queue: the server's thumb, which it already carried, and a new last field
with a download's copy (`BGPLAYER_TRACK_COVER`), which an older app's queue
leaves empty. A thread of its own makes the cover as the song changes: the
copy if there is one and it reads, else the server's photo transcoder at
128x128, as VitaPlex BG asks for it. The songs of an album share one. The
player has no borealis, so it builds stb_image itself, JPEG and PNG only
(`stb_image_impl.c`).

**Build 2030 on the console: the player's covers show** (logs `f5068799`,
the player's, and `13848dbc`, the plugin's).

- The quick menu was open as the player took the row, 6.4 s after it
  opened. The first cover, made from the server in 1556 ms, showed 71 ms
  after it was sent.
- Next from the quick menu: "Bad Day"'s cover was made in 567 ms and showed
  in 18 ms.
- An earlier session of the same log (build 2029's player) had a bug.
  - 11:37:01: a song was paused from the quick menu.
  - 14:03:19: as the Vita woke, the power callback's play-again (above)
    played the song again after 2 h 26 min.
  - The callback checked the app's pause, but not the quick menu's. Build
    2031 checks both.

**The header took longer than the download** (log `2114643f`). This
hand-over's first song was OGG, so the player fetched the server's MP3 (6.77
MB in 7.6 s). Inspecting it and copying it behind a Xing header then took
9.0 s more. BG Test 10's took about 5 s for 4.6 MB. Both passes went through
stdio's default buffer, so each read was small. `mp3_info.c` now gives each
file a 64 KB buffer. On the host, that took one song's reads from 416 to 76
and its writes from 276 to 36, with the same result and a byte-identical
file. The player started 0.14 s after VitaPlex BG's last line this time, not
4.2 s, so that wait varies. The silence was 19.8 s: 7.6 s of download, 9.0 s
of header, the rest starting up.

**EQVita** (shev0k/EQVita at `1279a7f`) has nothing for any of this. It is a
kernel plugin that hooks SceAudio's output calls in every process to
equalize them, and an app whose Music Preview decodes a file itself onto the
BGM port (`sceAppMgrAcquireBgmPort`) until stopped or closed.

Build 2032: **SceShell plays a file itself (an experiment).** The background
player is a process of its own. The system ends it as an app with extended
memory starts (35 of the 213 apps installed here), and as VitaPlex BG opens.
SceShell is never ended, and the quick menu plugin is already in it. So the
music could move there, in two steps:

1. The plugin plays the queue's songs that are already on the memory card,
   and goes on to the next song itself. The player and VitaPlex BG download
   ahead of it.
2. If Sony's HTTP library reaches the server from inside SceShell, the
   plugin downloads too, and the player goes.

Before either, code in SceShell has to drive the shell's music service. The
background player calls that service from outside, through libShellAudio
(`lib/libshellaudio`). Settings > "Test: SceShell Plays a File Itself" tries
it from inside, with a downloaded song:

- VitaPlex BG holds the BGM port, as for its system player tests (the BGM
  port test picker decides how). It loads the plugin if it is not in
  SceShell yet, and sends the song's path (`QM_CMD_MUSIC_TEST`, protocol 6,
  pipes `VitaPlexQM6::*`).
- The plugin's music thread, created the first time a test is asked for,
  finds `sceShellSvcGetSvcObj` (library
  `0x773242C7`, NID `0xB31E7F1C`) with taiHEN's `taiGetModuleExportFunc`,
  in a module of SceShell's named SceShellSvc or SceShellUtil. If neither
  has it, it loads `vs0:sys/external/libshellsvc.suprx` into SceShell and
  looks again.
- It then makes libShellAudio's calls, as the background player makes them,
  through the client's `invokeSyncMethod` (slot `0x14`):
  - start the music player service of type 0 (`0x30000`);
  - stop, open the file and play (`0x30006`, `0x30004`);
  - read the status four times a second (`0x30007`).
  If the clock runs on by 2 s within 8 s, it lets the song play until 30 s
  after the play call (`QM_MUSIC_TEST_S`). Only small steps between
  readings count, not a jump to where an earlier song left off. Then it
  stops and ends the service (`0x30001`).
- Every step's code goes to `qm-plugin.log` under "music test:".
  VitaPlex BG waits up to 45 s for "music test: done", then puts the lines
  in its report and its log.

The plugin's imports are not weak: a library or NID that SceShell does not
have keeps the whole plugin out. So this adds no library.
`sceShellSvcGetSvcObj` is looked up at run time. `taiGetModuleExportFunc` is
in taiHEN's own library, which the plugin already imports; it has been
there since taiHEN v0.9. The port stays with VitaPlex BG, since
`sceAppMgrAcquireBgmPortWithPriority` is in SceAppMgr. ElevenMPV-A's
plugin, the only one known to load in SceShell, does not import that
library, nor SceModulemgr, so the module is found by name.
`sceKernelLoadStartModule` is in SceLibKernel, which the plugin already
imports.

What the log can show:

- whether SceShell already has the client library, or will load it;
- whether a call from SceShell into its own service comes back;
- whether the service opens and plays a file for it, with VitaPlex BG
  holding the port.

If a call never comes back, only the test's thread waits. VitaPlex BG then
reports that the plugin never said it was done, and the last line is the
step it stopped at.

**Build 2032 on the console: SceShell plays a file itself** (logs
`96f10ce8`, the plugin's, and `fb1a0930`, VitaPlex BG's).

- VitaPlex BG took the BGM port at 0x80 and loaded the plugin, which had
  the test 43 ms later: `downloads/15756.mp3`.
- `sceShellSvcGetSvcObj` was in SceShell already, in its module SceShellSvc,
  so nothing was loaded. The client object came back at once.
- Start, stop, open and play each returned 0, within 9 ms.
- The state went from 0 to 1 in 0.27 s. The clock ran 1.1 s, 2.1 s, ...,
  29.1 s, one second each second. Then stop and end returned 0, and the
  plugin said "SceShell played it".
- As SceShell reads the BGM port (the plugin's hook):
  - VitaPlex BG's process had it at 0x80 before the play.
  - From 0.1 s after the play, the owner read 0x00011DC1, still at 0x80. That
    owner is also the one the port showed once VitaPlex BG was left for the
    LiveArea, so it is most likely SceShell.
  - VitaPlex BG's process was the owner again 0.1 s after the stop. So the
    service takes the port for SceShell itself while it plays for SceShell.

So code in SceShell can drive the music service, with the same calls and the
same results as the background player gets from its own process. Step 1 is
next.

Build 2033: **SceShell carries the queue on** (step 1, `qm_protocol.h`).
Until now, when the system ended the background player, the plugin gave the
row back 5 s after the player's last check-in, and the queue stopped after
the song then playing.

- **When.** The plugin has the row for the player, and the player's
  once-a-second `QM_CMD_ACTIVATE` stops for 5 s with no `QM_CMD_DEACTIVATE`.
  That happens when the system ends the player: an app with extended memory
  starts, or VitaPlex BG opens.
- **What it checks first.**
  - The player's status says a song plays.
  - The queue file is the one it plays (its `written=` against the status's
    `queue=`, to the microsecond).
  - The shell has a song of that song's length, to 3 s. The player may have
    handed the next song over just before it went, while its status still
    named the one before, so the song after the status's is accepted too.
  - Otherwise nothing is carried on, and the row goes back as before.
- **What it plays.** At the song's end, the next song in the queue's order
  that is on the card. The end is found as `watchSong` finds it: the clock
  going back in the last 10 s, or the state leaving 1.
  - First choice: the download the queue names, if the shell plays its format
    (MP3, M4A, AAC, WAV).
  - Second: the player's own file of it. The player now names its files to
    the plugin (`QM_CMD_SONG_FILE`): the song with the shell, and the next
    once it is ready.
  - Up to 20 songs are looked at, and songs on neither are passed over.
    Repeat one plays the song again; repeat all goes round.
  - A song the shell has not started within 15 s, counted in looks rather
    than by the clock, is passed over. Three in a row end it.
  - A song the shell starts part-way, as it can with a file it has had
    before, is taken back to 0:00.
- **The row.**
  - The song's lines and ⏸.
  - Its cover, if the player made one. The player now makes the next song's
    cover as soon as that song is ready (`bgplayer/<key>.png`, written beside
    its name and moved in whole).
  - Its buttons act on SceShell's music, as the player's do: play/pause,
    next, and previous. Previous within the first 3 s goes to the song
    before if it is still on the card, and otherwise to the start.
- **The status.** Written every 2 s and at each change, as the player writes
  it, with `by=SceShell`.
  - VitaPlex BG, opening, tells the plugin first (`QM_CMD_TAKE_BACK`).
    It only sends to a plugin already in SceShell, and never loads one.
  - It waits up to 2 s for the plugin's last status, then takes the music
    back as it does from an ended player: the song stopped in the shell, and
    played on in VitaPlex BG from the same place.
  - The ask also keeps a queue from being carried on behind VitaPlex BG
    while it is open, until the player takes the row again.
- **When it stops.**
  - The queue's end, or no further song on the card: the service is stopped
    and ended.
  - Something else playing in the shell, by the length it gives, checked
    every 2 s: that song is left playing.
  - A process taking the row: the music is left to it.
  - Paused from the row, it stays paused. Stopped otherwise, as the power
    button stops it, it is played again every 5 s, as the player does.
- **Files.** The player's file of a song is removed once the next has the
  shell, as the player removes its own; downloads never are.
- **Sleep.** The system clock runs on through a sleep. So after a gap of over
  2 s in its own loop, the plugin gives the row's owner its time again,
  rather than taking the player for gone on the first look after.

What it leaves out:

- **No downloads.** A queue of downloads is carried on to its end. With the
  player one song ahead, a streamed queue gets the song playing and the next
  one. Going further needs VitaPlex BG to save the next songs while it plays,
  or the plugin to download them itself (step 2).
- **No scrobbling** for songs played this way.
- **No power callback** (ScePower) or `sceKernelPowerTick` (SceProcessmgr).
  Those would be new libraries in SceShell. The 5 s retry stands in for the
  first. Whether the Vita sleeps by itself while SceShell plays is for the
  log to show.

The plugin gains two NIDs, `sceIoRead` and `sceIoClose`, from SceIofilemgr
(0xF2FF276E), the user library it already takes `sceIoWrite` and
`sceIoSyncByFd` from. It gains no library. Its PAF stubs are unchanged. It
no longer has a 64-bit division: build 2032's test had one, and times are
now split a byte at a time, in 32-bit steps (`splitUs`).

On the host, the carry-on code, taken whole from `main.cpp`, ran against a
simulated music service and real files, under ASan and UBSan, in 57 checks:

- a queue of five carried on from its second song through a download, a song
  not on the card and the player's file to its end;
- taken back part-way, and yielding to a process that takes the row;
- each button;
- the power button's stop;
- something else playing;
- repeat all and repeat one, with the shell starting a file part-way;
- a song that never starts;
- statuses and queues that do not match;
- CRLF lines, and a line longer than the reader holds.

`qm_link.c`'s test has the new messages, and `qmCoverWriteFile` writes the
same PNG as `qmCoverWrite` and leaves nothing behind on a bad image.

**Build 2033 on the console: nothing was handed over** (logs `4cff4881`, the
plugin's, and `b8cde643`, VitaPlex BG's).

- An album of downloaded songs was started from the Downloads tab. The
  player said "queue=true music=false", and mpv played
  `downloads/7561.m4a`.
- The system asked VitaPlex BG to quit 35 s in. It logged "with no music
  playing (mpv PLAYING at 35.3s)": "nothing handed over". So no background
  player ran, and SceShell had nothing to carry on.
- The Downloads tab built its queue items with no media type, in each of its
  six places. `MusicQueue::isMusicQueue` reads the current song's type, and
  the hand-over requires a music queue.
- Build 2034 gives them their type (`queueItem`). The refusal now says which
  check failed: no music playing, or a queue that is not music.
- The row showed the album, Doo-Wops & Hooligans, as the artist. A download
  keeps a parent title that is the album's, the artist's or the playlist's,
  by how it was downloaded, and the tab used it as both.
- So from build 2035 a track's download keeps its own artist, the track's
  `grandparentTitle` as the player shows it, and its album, at every place a
  song is downloaded (`DownloadItem::artist`, `albumTitle`).
  - Older downloads get both from the server the next time VitaPlex
    connects: `syncProgressFromServer` already fetches every completed
    download's details for its progress.
  - Until then `downloadArtist` and `downloadAlbum` read the parent title by
    the download's group: the artist for a single track or an artist's, the
    album for an album's, and neither for a playlist's.
  - The Downloads tab's queue items and rows, and the player's local-file
    path, use them.

**Build 2035 on the console: handed over, and nothing carried on** (logs
`f3042ca3`, the player's; `c1930c5a`, the plugin's; `34b19d01`, VitaPlex
BG's).

- The album of downloads handed over, and the row showed "Grenade" by Bruno
  Mars, from Doo-Wops & Hooligans. The player sent SceShell each song's file
  and made the next song's cover as it got it ready.
- SceShell carried nothing on, because VitaPlex BG was opened rather than a
  game. The system ended the player at about 19:16:03.
  - VitaPlex BG told the plugin it took the music back at 19:16:08.299, 12 ms
    before the plugin's 5 s were up.
  - The other order ends the same way. The plugin would have begun carrying
    on and then stopped at the take-back, its status written `by=SceShell`
    and the music left playing. VitaPlex BG reads that status (build 2033).
- The shell played song 2 on by itself after the player had gone, until
  VitaPlex BG stopped it at 0:31, 21 s later, and went on from there in mpv.
- These downloads are M4As that the shell does not seek to the second. MP3s
  and BG Test 11's M4A did, in 0.3 s each time.
  - The seek event took song 1 to 0:08 for 0:10.
  - After the second hand-over it left song 2 at 0:35 for 0:39 at the end of
    its 8 s. The clock never reached 0:37 on the way, so it either got there
    late or stood there.
  - `sceMusicPlayerServiceSeek`, the fallback, then stopped the shell at 0:00
    (the BGM port's owner went from SceShell back to the player), and song 2
    played again from its start.
  - A pause from the row at 1:58 went on at 1:23, 35 s back. The clock then
    ran to the song's end on time, so the music went back too.
  - mpv warns "timescale not set" on `7562.m4a`. The songs were downloaded
    through the server's download queue (log `b8cde643`).
- On a host, the player's flow harness (now 29 cases), given a shell that
  seeks these files as the console did, logs build 2035's lines exactly. It
  also showed a pause in a song's last 10 s taken as its end when played
  again, since the row's play cleared the pause before the shell played.
- So build 2036:
  - A jump to up to 10 s short of where a seek was sent counts as landed.
  - The first seek event gets 12 s, and a second follows only if the first
    made the clock jump nowhere. The seek call is gone.
  - A seek that did not land logs the clock's way: where it first jumped,
    and when it last moved.
  - A clock that stands 3 s after a seek lands is stopped and played again.
  - A song that plays again more than 3 s from where it stopped is sought
    back there, in the player and in the plugin's carry-on.
  - Neither the wait after the row's play nor the jump counts as the song's
    end.
- The plugin's watch thread once went 3.7 s without a look, and logged the
  Vita asleep. The quick menu's first opening was being logged, line by line
  to the card, while the player read every installed app's `param.sfo`
  (213 of them, from 20 s after it started). It now says either may be the
  cause.

**Build 2036 on the console: the player outlives VitaSurf** (logs
`46636387`, the player's; `a9f4a7db`, the plugin's; `fbc54add`, VitaPlex
BG's).

- VitaSurf (`VSRF00001`, `ATTRIBUTE2` 12) was opened from the LiveArea with
  VitaPlex BG playing. The system asked VitaPlex BG to quit, and it handed
  10 songs over at 0:04 into the second and started the player.
- The player's app watch had VitaSurf running beside it from its first look.
  It played song 2 to its end, and song 3 on, for the 4.5 minutes VitaSurf
  ran. Then VitaSurf was closed and VitaShell opened, and it played on.
- So the system ended no player started as VitaPlex BG closed for an app with
  extended memory. It ended one already running when such an app opened:
  VitaPlex BG in build 2035's log, and VitaSurf in BG Test 20's (build 2004).
  SceShell carrying the queue on is for that case, one game after another,
  and here it had nothing to do.
- Nothing was sought. The hand-over at 0:04 is under the 5 s past which the
  player seeks, so it started the song from 0:00. Nothing was paused from the
  row. So build 2036's changes to both were not exercised.
- The Vita had been restarted since the last session. VitaPlex BG read the
  player's status from before that as written "-1101.1 s before VitaPlex BG
  started", since the system clock counts from each start. It took that for
  under 20 s old, and looked for the player's song in the shell, which had
  none. From build 2037 a status written later than now is said to be from
  before the Vita last started, and set aside.
- VitaPlex BG was opened again at 17:43:56 (log `c8dab58b`), and its opening
  ended the player. The player had played on from the 17:32 hand-over: its
  song 5 at 0:50 fits that to the second.
  - VitaPlex BG found the shell playing that song at 0:50 and stopped it.
    Then it found the queue file "not the one it played", and went on with
    nothing, so the music stopped.
  - Nothing but a hand-over writes the queue file, so it was gone, not whole,
    or replaced from outside. The one line said all three alike.
  - From build 2037 the queue is checked before the shell's song is stopped,
    and a song with nothing to go on with plays on there. The line says
    which of the three it was, and the two `written` values.

**Build 2037 on the console: SceShell carries the queue on** (logs
`3980c597`, VitaPlex BG's; `60eb9584`, the player's; the status and queue
files; the plugin's log was not sent).

- The player took the queue on beside VitaSurf, as before, and the hand-over
  seeks landed exactly: 1:07 and 0:17 into M4A downloads, in 0.3 s.
- Opening a second app with extended memory ended the player, twice.
  - The first time, SceShell carried the queue on into song 2. VitaPlex BG,
    opened at 17:59:51, read SceShell's status written as it was told to
    stop, song 2 at 1:31. 20 s later it found the shell at 1:51 of that song,
    stopped it there, and went on in mpv from 1:51.
  - The second time the player's log stops 30 s into song 4. SceShell
    carried on, and the row's next took it to song 5: its status, written
    every 2 s, had song 5 at 1:38, 2:39 after the hand-over and 45 s before
    song 4 would have ended.
- So the take-back from SceShell works, and build 2037's check on the
  status's clock let SceShell's status, written just after VitaPlex BG
  started, through.
- 15 of that queue's 29 songs had no download, so SceShell would have passed
  them over. Fetching those from inside SceShell is the next step.

**Build 2040: SceShell fetches the start of a song (a test).** Before the
plugin fetches the queue's songs itself, the parts that are new are tried on
their own, as build 2032 tried playing (`QM_CMD_HTTP_TEST`, protocol 8; the
setting "Test: SceShell Fetches a Song Itself").

- Sony's SSL and HTTP libraries are looked up in SceShell at run time
  (`SceLibSsl`, `SceLibHttp`), not imported. A plugin's imports are matched
  against what SceShell has loaded as it loads, and one SceShell lacks keeps
  the whole plugin out, row and carry-on with it. If they are not there, the
  test loads `libssl.suprx` and `libhttp.suprx` from `vs0:sys/external`, as
  build 2032 loaded `libshellsvc.suprx`.
- SceShell uses these libraries itself, for PSN and the store. So the test
  starts them only if SceShell has not (`sceSslInit`, `sceHttpInit`), and ends
  only what it started. It leaves alone what holds for the whole process:
  `sceHttpsEnableOption`, `sceHttpsDisableOption` and `sceHttpsLoadCert`
  take no template and would change SceShell's own TLS.
- It asks for the server's `/identity` with no token, over https and over
  plain http. Only if https answers 200 does it ask for the song's first 64 KB
  with the token, as an `X-Plex-Token` header rather than in the URL. That
  goes to the memory card and is removed. On a failure it logs
  `sceHttpsGetSslError`'s reason: unknown CA, wrong name, expired. The
  background player brings its own CA bundle to curl because the Vita's
  built-in list is old, so an unknown CA is the likely failure.
- It runs on a thread of its own with a 64 KB stack, deleted as it ends. The
  40 KB below where that thread begins are painted and read back, so the log
  says how much stack the libraries used, and whether the music thread's
  32 KB would do for fetching.
- The token is copied out of the message and wiped from it, and wiped again
  as the test ends; it is never logged. VitaPlex's side wipes its copy once
  sent. On a host, the plugin's harness ran it against simulated libraries in
  12 checks. They cover SceShell's libraries already started, the HTTP library
  missing, TLS refused (no token sent), the test starting and ending the
  libraries, a server ignoring the range, and the token refused. The pipe
  test checks the message and its wiping.

**Build 2040 on the console: SceShell fetches** (logs `53b14f67`, the
plugin's; `e9ecf378`, VitaPlex BG's).

- `SceLibSsl` and `SceLibHttp` were in SceShell, and both already started:
  `sceSslInit` gave 0x80435020 and `sceHttpInit` 0x80431020 (already
  initialized). So fetching shares SceShell's own pools, and nothing is
  started or ended for it.
- The server's `/identity` answered 200 over https in 306 ms, with the
  Vita's own certificates: the plex.direct certificate's root is among them.
  Over plain http it took 50 ms.
- The song, an MP3 that is not downloaded, came as 206: 64 KB in 487 ms with
  its own TLS handshake, written to the memory card, its first bytes an MP3
  frame's (FF FB). The whole test took about a second.
- The libraries used 2 KB of the test thread's stack, so the music thread's
  32 KB will do for fetching.
- So the plugin can fetch the queue's songs that are not on the memory card
  from inside SceShell, with what SceShell has. They are still looked up at
  run time, since one boot's evidence does not show SceShell always has them
  loaded as the plugin loads.
- After the test, VitaPlex BG closed for a game and handed over its one song.
  The plugin saw the background player take the row 24 s later: the song was
  not downloaded, and the player fetches a song whole before the shell can
  play it.
- VitaPlex BG also set aside, as build 2037 has it, the player's status from
  before the Vita was restarted.

**Build 2042: SceShell fetches the queue's songs.** As it carries the queue
on, the plugin now fetches the songs that are not on the memory card, the way
the background player fetches them (protocol 9).

- What it fetches: the song's details from `/library/metadata/<key>`, and from
  them the Track's first Part. When the shell plays its format (MP3, M4A, AAC,
  WAV), the file comes as the server has it (`?download=1`) into the player's
  folder of songs, through a `.part` file moved in once whole. Then its cover
  comes from the server's photo transcoder, asked for as a 64-pixel PNG and
  kept only if it is one: nothing in the plugin can make a PNG from a JPEG.
  Each file goes once the shell has the next song, as the player's own do.
- When: the song after the one with the shell is fetched while that one
  plays. A song wanted before it has come (the one before ended first, or
  next or previous was pressed) is waited for: the shell is stopped, and the
  row shows the song. There is one fetch at a time, on a thread of its own
  with a 32 KB stack. A fetch for a song no longer wanted is stopped between
  reads.
- Failures: a song in a format the shell does not play, or one the server
  does not have (404), is passed over and remembered, so it is not asked for
  again. Any other failure is tried again once the song is wanted. A song
  that fails twice is passed over, and after three such songs in a row the
  carry-on stops.
- The server comes from VitaPlex's settings, and so does the token. The token
  is read for each fetch, sent as a header, wiped after it, and never logged.
  With no server in the settings, songs not on the card are passed over as in
  build 2037.
- If VitaPlex BG takes the music back while the shell waits for a song, the
  shell's clock says nothing. So the status names the song being fetched,
  from its start, with state playing and `fetching=1`. VitaPlex BG then plays
  it from there, without reading the shell.
- Still passed over: songs in other formats, such as FLAC, which the player
  has the server make MP3s of. Their length header (`giveHeader`) would have
  to come into the plugin with them.
- On a host, the plugin's harness has 62 new checks, 136 in all, under ASan
  and UBSan. They cover a song fetched while the one before plays, played
  from its file, and removed after it; the wait as one ends first, and the
  status during it; FLAC and 404 passed over; take-backs during a fetch and
  during a wait; a fetch stopped part-way and fetched again; play/pause, next
  and previous during a wait; previous fetching the song before; a failure
  tried again, then passed over; three in a row stopping the carry-on; a
  JPEG cover left out; and the token on every request and in no log line.
  The plugin builds for the Vita at 45490 bytes. Its new imports are
  `sceKernelDeleteThread` and libgcc's 32-bit divide with remainder.

**Build 2042 on the console** (logs `dcb5cc5d`, the plugin's; `52dbdee6`, the
player's; `b8fe534d`, VitaPlex BG's).

- The Vita was restarted first, so the plugin of protocol 9 was the only
  one loaded.
- VitaPlex BG played a playlist of 4012 songs and handed over 1000 of them.
  PS was pressed at 21:13:09, and the music played on. At 21:13:12 the
  system asked VitaPlex BG to quit for another app: VitaSurf, which the
  player found running beside it. The background player started as VitaPlex
  BG closed for it, and outlived it, as such a player did in build 2036. So
  the system never ended the player, SceShell carried nothing on, and the
  new fetching did not run: no line of the plugin's begins "carry on:".
- The player's first song, an MP3 of 3.6 MB that was not downloaded, came
  in 6.9 s. The music was silent for 10.8 s, from VitaPlex BG pausing mpv
  to the shell playing.
- Next was pressed 13 times from the row, and each press went on to the
  next song. A song not yet downloaded was waited for, 1.6 to 10.1 s (the
  longest for a 7.2 MB M4A). Downloads ran at 315 to 999 KB/s.
- Song 5 played from the app's download, as the queue named it: the
  hand-over looks up each song in VitaPlex's downloads by its ratingKey, and
  the player fetches only those that are not there.
- The row's lines and covers followed every song.

**Build 2043: VitaPlex BG hands its queue to SceShell (step 3, part one).**
Since build 2032 the plan has been that once the plugin fetches songs, the
background player goes. The player is a process of its own, which the
system ends when an app with extended memory opens while it plays; SceShell
is never ended. And on a quit request, the system started the next app
about 10.8 s after asking in an app that had used SceBgAppUtil (BG Tests 20
and 21). Handing to SceShell, VitaPlex BG never uses it, so the console may
show games starting sooner.

- The setting "Background Music Played By" picks SceShell (the default) or
  the background player.
- As VitaPlex BG hands over, with PS ("at once") or on a request to quit, it
  writes the queue as before and puts its row's cover beside the songs as
  the song playing's. It then pauses mpv, releases the BGM port, gives the
  row back, and sends `QM_CMD_PLAY_QUEUE` (protocol 10). The row and the
  queue go under the lock that each tick of the row's heartbeat takes, so
  that no heartbeat follows: the plugin leaves its music to any process
  that takes the row.
- The plugin plays from the queue's song (`index=`) and place
  (`positionMs=`). The song comes from the file the app made ready
  (`file=`), else from its download, else from the server, waited for (the
  status saying `fetching=1` with the place). Once the shell plays it, it
  is taken to the place. A landing up to 5 s short is near enough, since an
  M4A download's lands 2 to 4 s short (build 2035). The seek is asked for
  twice, then the song goes on from where it is.
- If VitaPlex's plugin will not load, or does not take the queue, the
  background player plays it, as before.
- Covers for the songs on the card: the player made them from the
  downloads' copies, but nothing in the plugin decodes a JPEG. So the plugin
  fetches the PNG from the server for the song playing and the next, after
  the next song's file.
- A song played to its end is marked played on the server, as the player
  does (`/:/scrobble`). It is done on a thread of its own, which sends what
  waits and ends.
- The harness found a fetched song asked for again: once a cover's fetch
  came after the next song's file, the plugin no longer knew that file was
  there. The file fetched is now kept apart from the fetch's job.
- Still skipped in SceShell: songs in other formats, such as FLAC. The
  background player plays them, as MP3s from the server.
- Review found two more. A stop asked of a carry-on that was about to
  begin as a queue was handed over would have stopped the new queue too. And
  a queue handed over but not begun would have begun under a process that
  had taken the row since. Both are mended.
- On a host, the plugin's harness passes 162 checks, 26 of them new, and the
  pipe test 37, 2 of them new. The plugin builds for the Vita at 50039 bytes,
  with the same imports as build 2042.
**Build 2043 on the console** (log `c6d126f7`, the plugin's; `dba74856` and
`981fe042`, VitaPlex BG's two sessions after it. The log of the session
that handed over was written over).

- SceShell took the queue at 21:55:40, 0.6 s after VitaPlex BG gave the row
  back, and played it with no background player.
- Songs 2, 3, 4 and 7 of 1000 played from files fetched as they came, at
  519 to 792 KB/s. Each was removed once the next one played.
- Song 2 played to its end and was marked played on the server. Song 3 was
  fetched while song 2 played and followed it with no wait, and so did
  song 4.
- Next, pressed while a song was being fetched, stopped that fetch and went
  on. A song fetched and then skipped was removed.
- Waits: 13.1 s for song 2 (next was pressed during song 1's fetch) and
  10.2 s for song 7.
- Covers came from the server as PNGs, one of them 65x64, and the row
  showed each.
- Song 1 was never heard: next came 2.7 s into its fetch, so the seek to
  VitaPlex BG's place was not tried.
- The take-back failed, in three parts:
  - VitaPlex BG opened at 22:03:43 and told SceShell at once to stop. So
    SceShell stopped going on, and left song 7 playing for VitaPlex BG to
    take.
  - VitaPlex BG's sync of its 147 downloads, one request each, ran on the
    UI thread before the main screen: 20.1 s in the next session. The
    take-back waits for the first frame, so it never came. PS was pressed
    at 22:03:48 and the system asked VitaPlex BG to quit at 22:03:51; it
    was ended 2 s later with the sync 7.6 s in and still going. Song 7
    played out in the shell with nothing after it.
  - Opened again at 22:04:14, VitaPlex BG reached its take-back 26.6 s
    later, the sync again in the way. It found SceShell's status 30.7 s
    old, past the 20 s it allows, and took nothing back. It was written as
    the session before told SceShell to stop.

**Build 2044: from that run.**

- The downloads' sync runs on a thread of its own, so the main screen, and
  the take-back after it, no longer wait for it.
- VitaPlex BG tells SceShell to stop only in its take-back, once it is
  ready to go on with the music. Until then SceShell plays the queue on, so
  an app closed before getting that far loses nothing.
- The downloads' list is saved beside the old one and moved in once whole.
  Saved in place, as before, a process ended part-way through would have
  cut it short; VitaPlex BG is ended 2 s after the system asks it to quit.
- When VitaPlex BG takes the row for music of its own while SceShell plays,
  SceShell stops its own (status "stopped") rather than play on under it.
- A song waited for plays as soon as its file is in. Its cover follows as a
  job of its own, where it held the song up 0.4 s before.
- On a host, the plugin's harness passes 163 checks. The plugin builds for
  the Vita at 50164 bytes, with the same imports as build 2042.

**Build 2045 on the console** (log `5ae2ab41`, the plugin's; `68588225` and
`7c97dfa5`, VitaPlex BG's two sessions).

- The main screen came 2.0 s and 1.8 s after VitaPlex BG started, with the
  downloads' sync of 147 going on beside it for 21.1 s. The take-back told
  SceShell to stop 3.4 s after the start, where build 2043's took 26.6 s.
  SceShell's status came 0.2 s later, written after it: song 1 of 1000 at
  0:30. VitaPlex BG stopped the shell at 0:32 and went on from there.
- The take-back's silence was 6.2 s. Of that, 2.7 s went on the UI thread,
  busy building the home screen, before it began the player, and 3.5 s on
  mpv starting.
- There were two hand-overs, both on a quit request. Each time, the song
  VitaPlex BG was playing was not on the card. It was fetched in 5.1 s and
  4.1 s (748 and 921 KB/s), and played 6.6 s and 5.9 s after VitaPlex BG
  gave up the BGM port. The next song was fetched while it played (614 and
  1202 KB/s), with its cover.
- The second hand-over fetched the song again. SceShell had had its file
  51 s before, but VitaPlex BG removed it as it took the music back.
- The seek to VitaPlex BG's place was checked against the status read
  before it, in the same look of the music thread that asked for it. So the
  time since the seek came out below 0, as 4294.9 s unsigned.
  - At 0:05, the 0:00 read before the seek counted as near enough. The song
    had in fact gone to 0:05: it was at 0:30 when taken back 25 s later.
  - At 1:14, the seek was asked for again at once. The second landed at
    1:15 in 0.5 s.
- mpv played the song taken back as the server's MP3 from 0:32. It lost its
  stream at byte 1290950, about 52 s in at 198 kb/s, and its three
  reconnects got HTTP 400. The hand-over came at 1:14, before what it had
  read ran out. Not looked into yet.

**Build 2046: SceShell plays every format, and the background player goes.**

- A seek is checked only on a status read after it: a look that asks for
  one ends there. The same stale reading did more harm, which the harness
  found once its clock moved on at each read, as the console's does:
  - A song the shell began part-way was taken back to its start, but 3:11
    was kept as the furthest it had got.
  - So the next look's 0:00 was taken for its end.
  - For repeat one, which hands the shell the same file again, that began
    the song over and over.
- Songs in formats the shell does not play now come as the server's MP3, as
  the background player had them:
  - The transcode decision, then start.mp3 in its session, the token as a
    header only.
  - The frames go to the shell behind a Xing frame giving their count,
    without the ID3 tag. This is mp3_info.c's logic, on sceIo in 32 bits,
    in two passes through the fetch's 16 KB buffer.
  - A server MP3 with a header of its own goes as it came. One with no
    frame is passed over for good.
- With that, SceShell does all the background player did, and the player
  goes, as planned since build 2032:
  - Its program, eboot2.bin with its param2.sfo, leaves VitaPlexBG.vpk.
    VitaPlex BG no longer starts it through SceBgAppUtil, nor ends through
    the raw exit that needed.
  - The setting "Background Music Played By" goes. The queue always goes to
    SceShell. When the plugin is not there to take it, the music plays on
    in VitaPlex BG.
  - The take-back reads SceShell's status alone.
  - In the plugin, the carry-on of the player's queue, its song files
    (QM_CMD_SONG_FILE) and its owner go. The protocol is version 11, so the
    new plugin loads beside an older one already in SceShell.
  - The queue loses port= and qm=, which only the player read, and the
    status loses wasPlaying.
- VitaPlex BG makes the song it plays ready for a hand-over:
  - 2 s into a song that could be handed over, its file is fetched in the
    background into the folder of songs, if it is not a download and the
    shell plays its format.
  - It is written through a file ending in .ready, never the plugin's
    .part, and moved in under the lock a hand-over takes.
  - Once whole, the queue names it (file=), and SceShell plays it at once.
  - A song taken back keeps its file for the next hand-over.
  - At the run's 614 to 1202 KB/s, a 3.73 MB song is ready 5 to 8 s after
    it starts. Build 2045's first hand-over came 6.7 s into its song.
- On a host, the plugin's harness passes 174 checks under ASan and UBSan.
  Its tests now begin from a queue handed over. The pipe test passes 35.
  The plugin builds for the Vita at 51125 bytes, with the same imports as
  build 2042.

**Build 2046 on the console** (logs `8e7a4b23`, the plugin's, and `89506fa9`
and `5ab8fb2f`, VitaPlex BG's; then `e18b5831`, `4e8bcb05` and `dd6a5eb9`
across a restart and the power button).

- Everything the build added worked: songs in other formats as the server's
  MP3, the song taken back keeping its file for the next hand-over, the
  row's next, previous and play/pause, repeat one, songs marked played.
- The first hand-over was of an OGG song, which VitaPlex BG does not make
  ready, so SceShell fetched the server's MP3 of it. It played 23.3 s after
  the hand-over:
  - 0.3 s for the decision, then 9.58 MB in 12.5 s (781 KB/s);
  - then 9.6 s for the two passes over it, counting the frames and copying
    them behind the header. The next song, OGG too, took 9.0 s for them
    after its 14.5 s fetch. At the card's speed they would take about 2 s;
    the fetch thread is SceShell's lowest priority, with a game running.
- The second hand-over named the file kept from the first (file=). SceShell
  played it 0.6 s after, from 0:00, and was taken to 1:13 1.3 s after that.
- The take-back's silence was 7.3 s: 1.4 s to the player, 0.6 s for the
  decision, 2.2 s between mpv's start and its load, and 2.8 s for the
  server's transcode from 1:03 to begin. The song's whole file was on the
  card, kept for the next hand-over, but mpv streamed it from the server.
- Next from the row waited 22.6 s for song 2. SceShell had fetched it ahead
  in the first carry-on, and removed it as the music was taken back.
- Previous from the row waited 37.5 s for song 1, whose file had gone as
  song 2 began: 23.0 s at 425 KB/s, and the two passes.
- Covers:
  - The two songs were of one album, and its 9190-byte cover was fetched
    for each, three times in all.
  - While a song was waited for, the row showed the heading's own icon:
    its cover comes after its file.
  - The downloads' covers were never used: they are JPEGs, and nothing in
    the plugin decodes one, so it fetches the server's PNG instead.
- A restart with SceShell playing: VitaPlex BG found SceShell's status from
  before it, took nothing back, and removed the 4 files left.
- "9 to 5", an MP3, came whole in 3.9 s as it played, 4.2 s before the
  hand-over, and was thrown away: 4,343,040 bytes against the 4,418,566 of
  the server's details, older than the file. SceShell fetched it again,
  with 11.5 s of silence.
- The power button with SceShell playing: four times the shell's music
  stopped (state 2) as the screen went off or on, and the plugin played it
  again, 0.5 s after, once 2.6 s (5 s between replays). The Vita stayed
  awake for it: the plugin's watch never missed a beat.

**Build 2047: VitaPlex itself.** VitaPlex.vpk takes VitaPlex BG's packaging,
and the tests go.

- VitaPlex.vpk (VPLEX0001) is a non-game app as VitaPlex BG was: category
  gdc, ATTRIBUTE 0x01089088, ATTRIBUTE_MINOR 16, ATTRIBUTE2 12. It carries
  the quick menu plugin in module/. VitaPlexBG.vpk goes; the updater stub's
  package stays a game, as it was.
  - Nothing is gated on the title id any more: music after leaving the app,
    by the setting, and the quick menu row are VitaPlex's on the Vita.
  - Leaving the app now pauses a video, which a game's freeze used to do,
    and music too with the setting at "Stops".
  - VitaPlex BG, if still installed, should be deleted, and the Vita
    restarted once, so that only the new plugin is in SceShell.
- What went:
  - The background tests: src/bgtest_psv and its 21 packages.
  - ElevenMPV-A's plugin as a fallback (third_party/elevenmpva-shell-plugin):
    without VitaPlex's own, there is no hand-over at all.
  - In settings: the system player tests, the SceShell play and fetch tests,
    BG Test 11's hand-off, and the music port, service and button pickers.
  - The watcher's reports: "Away 0:03…" and "frozen" on screen, the UI
    loop's count, the BGM owner, each power callback and app event.
  - appmgr_bgm_stub.S, which only those used, and mp3_info.c, whose logic
    the plugin has.
  - In the plugin, protocol 12 (module VitaPlexQM12): QM_CMD_LOOK,
    QM_CMD_MUSIC_TEST and QM_CMD_HTTP_TEST, the widget timeline, the BGM
    reads' log, and the logs of each step of the row. The buttons are always
    switched on and lit, the way build 2028 found to work.
- Covers:
  - A song whose cover on the server is the one before's, its album's, gets
    a copy of that one's: as it is fetched ahead, as it is waited for, and
    as the shell gets it.
  - The server makes a 64-pixel PNG of a download's cover as the track is
    downloaded, beside its JPEG (`<key>_cover64.png`). Tracks downloaded
    before get theirs once, after the progress sync at start. The queue
    names it (BGPLAYER_TRACK_COVER), and the plugin shows it as it is, with
    no server too.
- The song made ready is taken as whole by the length the server's answer
  gives, the details' size only with none.
- Still to do, from build 2046's logs: the passes over a server MP3, which
  could be made as it comes; the song before, kept for previous; songs
  fetched ahead, kept at a take-back; and the take-back playing the song's
  file from the card, not the server's stream.
- On a host, the plugin's harness passes 167 checks, the HTTP test's gone
  and the covers' added; the pipe test passes 23. The plugin builds at
  40361 bytes, with build 2042's imports less three.

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
