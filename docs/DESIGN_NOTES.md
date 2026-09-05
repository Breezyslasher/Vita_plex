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

## Security

mpv quotes the whole URL back in its stream errors, and every URL the client
hands it carries `X-Plex-Token`. The log bridge passed `msg->text` straight to
the logger, so a failed open printed a long-lived token in full — in a log the
user then pastes into a bug report. Every level now goes through
`redactTokensInUrl()`.
