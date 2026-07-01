# Plex background/during-game audio on Switch — "Plex-Tune" companion module
### Design & feasibility scope

## 1. Goal
Keep Plex music playing on the Nintendo Switch **in the background and while a game is running**, configured from VitaPlex.

**In scope:** Switch only. A companion **sysmodule + Ultrahand overlay** shipped from the VitaPlex repo, set up from the app.
**Out of scope:** Vita (use MusicPremium + a local player), PS4 (no equivalent exists), video, gapless.

## 2. Why it can't live inside the VitaPlex binary
On the Switch the foreground **application** process (VitaPlex) is torn down the moment a game boots. The only thing that survives is a **sysmodule** — a separate, persistent, headless system process. So background/during-game audio *must* be a second binary. This is a companion to VitaPlex, not a feature inside it. (Same reason streamfin is a sysmodule, not an app.)

## 3. Architecture (mirrors streamfin)
```
  VitaPlex (app)  --writes-->  sdmc:/config/plex-tune/config.ini   (server URL + token)
                                        |
                                        v
  Ultrahand overlay  <--- IPC --->  sysmodule (fork of sys-tune)
   - reads config                    - HTTP GET the mp3 stream URL
   - Plex auth (token)               - dr_mp3 decode
   - browse artists/albums/tracks    - audout output  (survives game launches)
   - transport (play/pause/next)     - reports status back over IPC
```
Three pieces, same split streamfin uses:
- **sysmodule** — forked from [sys-tune](https://github.com/HookedBehemoth/sys-tune): audio decode (dr_libs) + `audout`, plus an HTTP streaming loop. Persistent, tiny memory budget.
- **overlay** — [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay)/Tesla UI, summonable over any game: browse the Plex library, pick a track/album/radio, transport controls.
- **IPC** — overlay ↔ sysmodule (play URL, pause, skip, query status).

## 4. What carries over vs. what's new
**Carries over from streamfin/sys-tune (the de-risked part):**
- Audio decode pipeline (dr_mp3 / dr_flac / dr_wav) and `audout` output.
- The HTTP streaming loop (fetch a URL, feed the decoder).
- IPC scaffolding and the Ultrahand overlay framework.
- The whole "runs during games" behaviour — this is proven.

**New / Plex-specific (the actual work):**
- **Auth** — Plex `X-Plex-Token`. Cleanest path: VitaPlex already has the token + server address, so it writes them to `config.ini` and the overlay just reads them ("set up once in the app"). Fallback: implement the plex.tv PIN flow in the overlay.
- **Browse** — Plex endpoints (music library `type=8`, `/library/metadata/{id}/children` for artist→album→track, `/library/sections/{id}/all`). VitaPlex already implements every one of these.
- **Stream URL** — this is the win: call Plex's `/music/:/transcode/universal/decision` with `protocol=http, container=mp3, audioCodec=mp3` (exactly what VitaPlex's `getTranscodeUrl` already does) → Plex returns a **plain-HTTP progressive MP3** for *any* source format.

**Note:** VitaPlex's `PlexClient` is the perfect *reference* — it does all of the above — but the code can't be lifted wholesale. It depends on borealis + curl; a sysmodule needs a stripped-down reimplementation that fits the memory budget.

## 5. Why Plex is actually easier here than Jellyfin (streamfin)
- **No TLS needed.** Plex's transcode endpoint serves **plain HTTP** progressive MP3. streamfin/Jellyfin can force HTTPS; a TLS stack inside a sysmodule's tiny pool is the nastiest part of this class of project. Plex lets us skip it (LAN or even remote via plain-HTTP transcode).
- **No format matching.** dr_mp3 alone is enough — Plex transcodes FLAC/ALAC/AAC/OGG/whatever → MP3 server-side. streamfin is limited to source files already in FLAC/WAV/MP3.
- Trade-off: transcoding adds **Plex server CPU load**, and we should call the `decision`/`stop` session endpoints politely (VitaPlex already does).

## 6. Real risks / unknowns
- **Sysmodule memory.** The Switch gives background sysmodules a small shared pool; streamfin already warns playback can fail with too many overlays/sysmodules loaded. Our streaming buffer + MP3 decode must fit. (No TLS helps a lot here.)
- **Headless auth.** No browser in a daemon → rely on the token handoff from VitaPlex (recommended) rather than an in-overlay login.
- **Plex "direct" HTTPS for remote servers.** If someone's only reachable over `plex.direct` HTTPS with no plain-HTTP option, v1 won't cover them (LAN / plain-HTTP transcode only). Remote-HTTPS is a later, harder phase.
- **A whole second codebase**, Switch-only, to build and maintain.
- **Every phase needs Switch hardware testing by you** (I can't run it here).

## 7. Install / UX
1. User installs Ultrahand + the Plex-Tune sysmodule + overlay (to `/atmosphere/contents/...` and `/switch/.overlays/...`), like any Ultrahand overlay.
2. Configure Plex **once in VitaPlex** → it writes `sdmc:/config/plex-tune/config.ini`.
3. In a game: summon the Ultrahand overlay → Plex-Tune → browse & play. Audio persists across game launches.

## 8. Phased plan & effort
- **Phase 0 — Bring-up.** Fork streamfin, build it unchanged, confirm the toolchain + overlay loads + sysmodule runs on your Switch. *(setup / de-risk)*
- **Phase 1 — Plex MVP.** Swap the server layer: read token+URL from config, browse one library, get the `protocol=http` MP3 URL, play a chosen track in the background. *(core work)*
- **Phase 2 — VitaPlex handoff.** VitaPlex writes `config.ini`; optional "send to background player" affordance. *(small)*
- **Phase 3 — Usability.** Album/artist/radio browsing, queue, now-playing/status in the overlay, session cleanup. *(medium)*
- **Phase 4 (optional, hard).** Remote servers over HTTPS. *(defer)*

**Honest effort:** this is a **separate Switch homebrew**, multi-iteration (days, not hours), each round gated on your hardware testing. Not a small in-app feature. The architecture is de-risked by streamfin and the Plex API is fully understood (VitaPlex implements it), so the *risk* is low-to-moderate; the *cost* is real.

## 9. Recommendation
Feasible and, thanks to Plex's plain-HTTP MP3 transcode, **notably cleaner than a Jellyfin/streamfin build**. If pursued, target **LAN + transcode-to-MP3, token handoff from VitaPlex, defer remote HTTPS**. If that's more than you want to take on, the **downloads + plain sys-tune** route gives during-game audio for *downloaded* tracks today with zero new code.
