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

`useMobileLayout()` is false on PSV, PS4, Switch and desktop, so word rows are
only ever built on a phone. The handhelds keep the classic sheet and pay
nothing at all.

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
