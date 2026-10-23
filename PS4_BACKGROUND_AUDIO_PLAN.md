# VitaPlex on PS4 — a background companion, like the Vita plugin
### Design & feasibility scope

## 1. Goal
Give the PS4 the two things `vitaplex_qm.suprx` gives the Vita:

1. **Updates without the second app.** VitaPlex stages the pkg, hands it over and
   quits; something that outlives VitaPlex installs it **and relaunches VitaPlex**.
2. **Music after leaving.** VitaPlex's queue keeps playing, from the song and
   position it had reached, after you leave VitaPlex or start a game.

Plus one thing the Vita plugin doesn't do: **true streaming.** The plugin
fetches a whole song to the card before it plays (or frames the server's MP3 as
it downloads). On the PS4 it should decode while the bytes arrive, with no
file on disk.

**Out of scope for v1:** controls in the PS4's own quick menu (see §6), video,
gapless.

## 2. Why the Vita design doesn't carry over directly
On the Vita the plugin loads into **SceShell**, which never exits. It owns the
quick menu's Music row and SceShell's music service, so "load a module into the
shell" gives you UI, audio and persistence in one go.

The PS4's shell is split in two, and neither half is a good home:

- **SceShellUI** draws the home screen and quick menu, but it is a **Mono (C#)
  process**. Getting into it means injecting managed code into a Mono runtime,
  not loading a native module. Nothing open-source does this in a reusable way
  that I could find, and it breaks across firmware versions.
- **SceShellCore** is native and runs everything, but nobody publishes a way to
  load a module into it. GoldHEN's plugin loader targets **game processes**
  (`/data/GoldHEN/plugins.ini`, `[CUSAxxxxx]` / `[default]` sections), not
  system processes.

So the PS4 version is **a separate process that VitaPlex talks to**, not code
injected into the shell. Two kinds of process are available:

| | **Daemon** (system-app homebrew) | **GoldHEN game plugin** (`.prx`) |
|---|---|---|
| Runs on the home screen | yes | no |
| Runs during a game | yes, if the system keeps it running (**needs a device test**) | yes, inside the game |
| Survives a game quitting | yes | no, it dies with the game |
| Can install updates and relaunch VitaPlex | yes | no |
| Prior art | ItemzDaemon (Itemzflow), FTP/telnet daemons | GoldHEN_Plugins, Rock Band 4 Deluxe |
| Needs | GoldHEN (any HEN with fpkg) | GoldHEN 2.3+ with plugins enabled |

**Recommendation: a daemon.** It is the only option that covers both jobs, and
it is the PS4 counterpart of what the Vita plugin really is: something that
keeps running after VitaPlex is gone. A game plugin is a fallback for audio
only, if §7's first device test shows the system stops or mutes a daemon while a
game runs.

## 3. Architecture
```
  VitaPlex (VPLX00002)                          vitaplexd (daemon, VPLX00003)
  ─────────────────────                         ─────────────────────────────
  writes /data/VitaPlex/handover.bin  ───────▶  reads queue, position, server,
  (queue, index, position, server URL,          token, "playing" flag
   token, settings)

  local socket  127.0.0.1:<port>  ◀─────────▶  same commands as QM protocol v15:
   ACTIVATE / SONG / PLAYING / PLAY_QUEUE /     PLAY_QUEUE, TAKE_BACK, INSTALL_UPDATE,
   TAKE_BACK / INSTALL_UPDATE                   status back (index, position, state)

                                                 ┌─ HTTP stream (plain HTTP to the server)
                                                 ├─ ring buffer (~1–2 MB)
                                                 ├─ decoder (minimp3; dr_flac for FLAC direct play)
                                                 └─ sceAudioOut, 48 kHz stereo
```

- **Title.** Reuse **VPLX00003**, the updater helper's id. The daemon replaces the
  helper outright, as the Vita plugin replaced the Vita stub app (build 2046).
- **Transport.** A loopback TCP socket instead of the Vita's message pipes. It is
  the same idea (versioned, non-blocking, VitaPlex can never hang on it) and
  needs nothing PS4-specific. Put the protocol version in the hello message the
  way the Vita puts it in the pipe name.
- **Protocol.** Reuse `QM_CMD_*` from `src/handover_psv/qm_protocol.h` where they
  mean the same thing. The handover record (`handover_psv/handover.h`) is plain
  data and can be shared too. A small `src/handover_common/` split would let both
  platforms use one definition.
- **Starting it.** VitaPlex launches the daemon at start if it isn't answering on
  the socket (`sceSystemServiceLaunchApp`, already used in `utils/ps4_install.cpp`,
  or `sceLncUtilLaunchApp` the way Itemzflow starts its daemon). It stays up
  until the console restarts, like the Vita module.

## 4. The two jobs

### 4a. Updates (replaces the VPLX00003 helper)
The helper today: VitaPlex launches it and quits; it registers the pkg with BGFT
(uninstalling first only on `0x80990088`), then exits. It **cannot relaunch
VitaPlex**, because it exits before the install finishes (relaunching early
gave CE-36329-3), and it can't draw anything (it never gets video-out).

The daemon keeps all the BGFT/AppInstUtil code from `src/updater_ps4/main.cpp`
as it is, and adds what the helper couldn't do:

1. VitaPlex sends `INSTALL_UPDATE` and quits.
2. The daemon waits until VPLX00002 is no longer running, then registers and
   starts the BGFT task (unchanged logic).
3. It polls `sceAppInstUtilAppIsInInstalling` / `sceBgftServiceDownloadGetProgress`
   until the install has **finished**, not just started.
4. It writes the outcome to `/data/VitaPlex/update-result` and relaunches
   VPLX00002, so VitaPlex can report the result on its next start, as on the Vita.
5. It posts progress or result with `sceKernelSendNotificationRequest` (already
   used in `utils/shell_integration_ps4.cpp`), so it never needs to draw.

This solves a problem we already have, it needs no new research, and it is
the safest first deliverable.

### 4b. Music after leaving, with true streaming
- **Hand-over.** Same as the Vita: VitaPlex writes the queue and position, sends
  `PLAY_QUEUE` and quits (or the user starts a game). The daemon carries on from
  that song and position. `TAKE_BACK` when VitaPlex starts again returns the
  queue at the daemon's current position.
- **True streaming.** The daemon never writes a song to disk:
  - Formats the decoder handles (MP3, FLAC) **direct play**, with an HTTP
    `Range` request to start mid-song.
  - Anything else comes from Plex's `/music/:/transcode/universal` with
    `protocol=http, container=mp3` (VitaPlex's `getTranscodeUrl` already builds
    it) and an `offset=` to start mid-song.
  - A network thread fills a ring buffer and the audio thread decodes from it,
    so playback starts after a few KB instead of a whole file. A pre-fetch of the
    next track's first ~256 KB keeps the gap between songs short.
- **HTTPS.** VitaPlex's PS4 build already has to work around HTTPS (mpv can't do
  it; `utils/https_proxy.cpp` exists for this). v1 of the daemon uses plain HTTP
  to the server's LAN address. Remote/`plex.direct` HTTPS comes later, through
  libcurl + mbedTLS (as the app links) or `libSceSsl`.
- **Server bookkeeping.** Report progress to `/:/timeline` and mark played songs,
  as the Vita plugin does, and stop transcode sessions properly.
- **Output.** `sceAudioOutOpen` on the **BGM** port type, the port meant for
  background music (OpenOrbis numbers it 1, MAIN is 0). Fall back to MAIN if
  BGM is refused. Volume is the system's.

### 4c. Controls and now-playing (v1)
The PS4 has no Music row we can write to (see §2), and a daemon doesn't receive
the controller while a game has focus. v1 does:
- A **notification** on each new song ("Now playing — title · artist").
- **Remote control over the LAN.** The daemon already listens on a socket, so
  open it on the LAN too and give it a minimal **Plex Companion** player
  endpoint. The PS4 then appears as a player in Plex / Plexamp on your phone,
  with play/pause/next/seek. This is the most useful control surface there is
  while a game is running, and it's a documented Plex protocol.
- VitaPlex, once reopened, shows the daemon's state and takes the queue back.

## 5. What carries over vs. what's new
**Carries over:**
- All BGFT / AppInstUtil install code (`src/updater_ps4/main.cpp`), including
  the "load the PRX and resolve at runtime" lesson from builds 1552–1556.
- The protocol and hand-over record design (`src/handover_psv/`).
- Transcode URL building, timeline reporting and the played-song rules (the Vita
  plugin and `PlexClient` implement all of them).
- Notifications (`shell_integration_ps4.cpp`).
- Build plumbing: the VPLX00003 target and `cmake/bundle_updater_pkg.cmake`
  already build a second pkg and ship it inside VitaPlex.

**New:**
- The daemon's lifecycle: being installed as a daemon, started by VitaPlex,
  staying up.
- The streaming player (ring buffer + minimp3/dr_flac + sceAudioOut). It's a few
  hundred lines; both decoders are single-header and public domain.
- A small HTTP client for the daemon (plain sockets for v1, no borealis).
- The socket protocol (a port of QM v15).
- Optional: the Plex Companion endpoint.

## 6. Risks / unknowns (each needs a device test by you)
1. **Does a daemon keep running and keep its audio during a game?** This is
   the deciding question. If the system suspends, kills or mutes it, audio falls
   back to a GoldHEN game plugin (audio in games only) and the daemon keeps the
   update job.
2. **Games that turn background music off.** Some games block BGM (the way
   they mute Spotify). The BGM port may be silenced on purpose there. That's
   acceptable; MAIN might get around it, but that could clash with the game's
   own mix.
3. **Daemon install path.** Itemzflow-style daemons are installed as system
   apps. We need the method that works on GoldHEN without extra payloads, or
   accept that the daemon is a normal fpkg app launched in the background and
   check that it isn't closed when a game starts.
4. **Quick-menu controls.** Not attempted in v1 (SceShellUI is Mono). If a
   proven ShellUI mod surfaces later, it would be a separate phase.
5. **Memory.** A daemon gets far less than VitaPlex. A ring buffer plus minimp3
   is well under 4 MB, so this should be fine. A TLS stack is the part that
   could push it.

## 7. Phased plan
- **Phase 0: bring-up (de-risks §6.1).** Turn VPLX00003 into a daemon that VitaPlex
  starts. It logs a heartbeat to `/data/VitaPlex/daemon.log` and plays a 440 Hz
  tone on the BGM port. On the console: start it, start a game, and check that
  the log keeps ticking and the tone stays audible. *Everything else depends on
  this result.*
- **Phase 1: updates through the daemon.** Move the helper's install code in,
  add wait-for-install + relaunch + result file, and drop the separate helper
  flow from `ps4_install.cpp`. *Small; worth having even if the music part fails.*
- **Phase 2: streaming player.** Play one track from a URL given over the socket:
  ring buffer, minimp3, start offset, plain HTTP.
- **Phase 3: hand-over.** `PLAY_QUEUE` / `TAKE_BACK`, queue advance, prefetch,
  timeline, notifications. VitaPlex gets a PS4 "Music After Leaving" setting
  (the Vita has one).
- **Phase 4: controls.** Plex Companion player endpoint.
- **Phase 5 (optional):** HTTPS for remote servers; FLAC direct play.

**Effort:** like the Vita plugin, this is several builds, each gated on a test on
your console. Phases 0–1 are small and reuse proven code. Phase 2 is the bulk
of the new work. The architecture is sound, but the whole music half depends
on the Phase 0 result.

## 8. Prior art to read before Phase 0
- [LightningMods/Itemzflow](https://github.com/LightningMods/Itemzflow): its
  daemon (install, launch, IPC with the app).
- [GoldHEN/GoldHEN_Plugins](https://github.com/GoldHEN/GoldHEN_Plugins): the
  game-plugin fallback.
- LightningMods/PS4-Store: the BGFT code `updater_ps4` is already based on.
- [john-tornblom/tiny-ps4-shell](https://github.com/john-tornblom/tiny-ps4-shell):
  a minimal example of a background process.
