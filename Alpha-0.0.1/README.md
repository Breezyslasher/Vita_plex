<div align="center">

<img src="new/new_icon.png" alt="VitaPlex" width="128" height="128" />

# Vita_plex

**A native Plex client for PlayStation Vita**

[![VPK Build](https://github.com/Breezyslasher/Vita_plex/actions/workflows/build.yml/badge.svg)](https://github.com/Breezyslasher/Vita_plex/actions)
[![Firmware](https://img.shields.io/badge/Vita-3.60%2B-orange)](https://henkaku.xyz/)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)
[![GitHub release](https://img.shields.io/github/v/release/Breezyslasher/Vita_plex?include_prereleases)](https://github.com/Breezyslasher/Vita_plex/releases)

Browse, stream, and download your Plex library directly on the PS Vita — native C++, no transcoding proxy, no web wrapper.

</div>

---

## 📖 Table of Contents

- [About](#-about)
- [Current Build](#-current-build)
- [Alpha 0.0.1 — The First Build](#-alpha-001--the-first-build)
- [Feature Comparison](#-feature-comparison)
- [Requirements](#-requirements)
- [Installation](#-installation)
- [Controls](#-controls)
- [Building from Source](#-building-from-source)
- [Roadmap](#-roadmap)
- [Credits](#-credits)
- [License](#-license)

---

## 🎬 About

Vita_plex is a homebrew Plex client written in native C++ for the PlayStation Vita. It talks directly to your Plex Media Server over HTTP, handles authentication via Plex PIN / credentials, and plays content using a Vita-tuned MPV build.

The project began as a lightweight vita2d menu (**alpha 0.0.1**) and has since been rewritten on top of the [Borealis](https://github.com/dragonflylee/borealis) UI framework — giving it a modern, controller-friendly experience, offline downloads, a Live TV program guide, and a proper music queue.

---

## 🚀 Current Build

The current build is a **full rewrite** on Borealis with hardware-accelerated NanoVG/GXM rendering, activity-based navigation, a sidebar-driven main menu, and offline download support. Audio playback works cleanly via MPV; video playback uses an HLS/MPEG-TS transcode pipeline and is actively being stabilized.

### Home

![Home](new/home_page_new.png)

Sidebar-driven main navigation (**Home / Movies / DVR TV Shows / TV Shows / CD Music / Search / Live TV / Downloads / Settings**). The Home tab surfaces Continue Watching and Recently Added Movies, pulled directly from your Plex server.

### Movies Library

![Movies](new/movies.png)

Poster-grid browse of your Movies library with **All** and **Categories** filters. Cover art is loaded from Plex and cached locally.

### Music Library

![CD Music](new/msuic_page.png)

Artist grid with **All / Categories / Playlists** tabs. Album art fallbacks walk track → album → artist so something always renders.

### Artist Page

![Artist — AmaLee](new/album_page.png)

Artist view shows the artist's photo, album grid, and music video grid. The `Start` overlay on a poster indicates the focused item.

### Album / Track List

![Track List — MY NINJA WAY](new/track_page.png)

Album view with numbered track list, durations, and per-track download indicators (`DL` badge).

### Artist Download View

![Artist Download](new/artis_downlaod.png)

Select an artist and queue the whole discography — **Play All** or **Add to Queue**. Each track's download status is shown inline (`Queued`, `Downloading...`, `Ready`, `Failed`).

### Music Player

![Music Player](new/palyer_ui.png)

Full-screen now-playing view with album art, shuffle / previous / play-pause / next / repeat controls, a scrub bar, and track position indicators.

### Music Queue

![Music Queue](new/music_queue.png)

Drag-and-drop reorder, swipe-to-remove, and **LB/RB** to move tracks. Shows total queue duration and the currently playing track highlighted at the top.

### Video Player

![Video Player](new/video_palyer_ui.png)

MPV-backed video player with a scrub bar, skip ±10 s, subtitle toggle, audio track picker, and aspect-ratio controls. Audio uses the `ao=vita` driver; video renders through a GXM callback.

### Movie Detail

![Movie Detail](new/movie_info_page.png)

Full metadata view: cover art, year, rating, runtime, **Play** / **Download** actions, and an Extras row for trailers and behind-the-scenes.

### Downloads Tab

![Downloads Tab](new/downlaods_tab.png)

Manage offline content with **Stop**, **Resume All**, **Sync**, and **Clear Done** actions. Per-item status shows how many items are ready, queued, downloading, or failed.

### Downloaded Season View

![Season Downloads](new/downaloded_season_inside.png)

Per-episode download status — red rows are failed downloads, green rows are actively downloading with live progress and byte counters.

### Live TV / Program Guide

![Live TV](new/live_tv.png)

Channel grid with a scrollable **Program Guide** (time slots across channels) and a **DVR Recordings** section. Requires Plex Pass and a configured DVR on your server.

### Search

![Search](new/search.png)

On-screen keyboard input, results grouped by type (Movies, Tracks, etc.) with poster art.

### Settings

Settings are grouped into **User Interface**, **Layout**, **Content Display**, **Playback**, **Music**, **Transcoding**, **Downloads**, and **Debug**.

<details>
<summary><b>Settings — User Interface & Layout</b></summary>

![Settings 1](new/setting_tab_1.png)

Logout, Theme, Debug Logging, Show Debug Tab, Libraries in Sidebar, Collapse Sidebar, Manage Hidden Libraries, and Sidebar Order.
</details>

<details>
<summary><b>Settings — Content Display</b></summary>

![Settings 2](new/setting_2.png)

Show Collections, Show Playlists, Show Categories, Hide Movie/Show Titles, Skip Season for Single-Season Shows.
</details>

<details>
<summary><b>Settings — Playback & Music</b></summary>

![Settings 3](new/setting_3.png)

Resume Playback, Show Subtitles, Subtitle Size, Seek Interval, Controls Auto-Hide, Auto-Skip Intro, Auto-Skip Credits, Default Track Action, Background Music.
</details>

<details>
<summary><b>Settings — Transcoding, Downloads & Debug</b></summary>

![Settings 4](new/setting_4.png)

Video Quality (up to 1080p @ 20 Mbps), Force Transcode, Try Direct Play First, Connection Timeout, Delete After Watching, Clear All Downloads, Network Test, Test Local Playback.
</details>

### Running on Real Hardware

![Running on PS Vita](new/live_scrren_ps_vita_new.png)

The `.vpk` installs on any PS Vita with custom firmware (HENkaku / Enso / h-encore) on 3.60+.

---

## 🕰️ Alpha 0.0.1 — The First Build

The original release was a lightweight, text-menu-driven client built on vita2d with a custom HTTP/Plex-API stack. It ran at a steady 60 FPS at 960×544 and established the core feature set: auth, library browse, search, Live TV, and basic SceAvPlayer-based playback.

**All original screenshots are preserved below for posterity.**

### Home Screen

![Alpha 0.0.1 Home](old/home_screen_plex_alpha_old.png)

Text-list main menu: **Libraries / Search / Continue Watching / Recently Added / Live TV / Settings / Logout**. The orange VitaPlex header and currently-connected server name (`openmediavault`) sit at the top.

### Libraries List

![Alpha 0.0.1 Libraries](old/libaryies_list_old.png)

Pre-grid library browse — each library is tagged with its type: `[M]` movie, `[T]` show, `[A]` artist. No cover art at this stage.

### Movie Detail

![Alpha 0.0.1 Movie Info](old/movie_info_old.png)

Basic detail page (*3:10 to Yuma*): year, rating, studio, runtime, resolution/codec, synopsis, and **Play** / **Mark Watched** actions.

### Search

![Alpha 0.0.1 Search](old/search_old.png)

Search with text-only results grouped by media type — shown here with 4 results for "star wars" spanning movies and a track.

### Player UI

![Alpha 0.0.1 Player](old/palyer_ui_old.png)

The original player — loading an audio track with an empty progress bar and footer controls: `L/R 10s`, `Left/Right 30s`, `Up/Down Vol`, `X Pause`, `O Stop`.

### Music Artist List

![Alpha 0.0.1 Music](old/msuic_old.png)

Artist listing with thumbnails — the first build where album/artist artwork was rendered.

### Album / Track Page

![Alpha 0.0.1 Tracks](old/tarck_page_old.png)

BlackGryph0n ›  IMmortal — numbered track list with durations and `[W]` watched markers.

### Browse / Continue Watching

![Alpha 0.0.1 History](old/history_tab_alpha_old.png)

Continue Watching / Browse — compact row with poster, year, type, and runtime.

### Live TV — Empty State

![Alpha 0.0.1 Live TV](old/live_tv_old.png)

Live TV empty state — surfaced clearly when the server has no DVR configured.

### Early Live Preview on Hardware

![Alpha 0.0.1 on Vita3K](old/live_screen_old.png)

Early preview running in Vita3K with the PLEX launcher LiveArea tile — the very first time the build booted.

---

## 📊 Feature Comparison

| Feature | Alpha 0.0.1 | Current Build |
|---|---|---|
| UI Framework | vita2d + custom text menus | Borealis (NanoVG + GXM) |
| Navigation | Linear list menus | Sidebar tabs + activity stack |
| Library Browse | Text list with `[M]/[T]/[A]` tags | Poster grid with All / Categories |
| Cover Art | Music only | Movies, TV, Music, Music Videos |
| Audio Playback | SceAvPlayer (basic) | MPV with `ao=vita` driver |
| Video Playback | SceAvPlayer (basic) | MPV HLS/MPEG-TS pipeline |
| Music Queue | ❌ | ✅ Drag / swipe / LB-RB reorder |
| Offline Downloads | ❌ | ✅ Movies, Shows, Music |
| Live TV | Empty-state only | Full program guide + DVR |
| Search | Text list | Typed, grouped results with art |
| Settings | Minimal | UI / Layout / Playback / Music / Transcoding / Downloads / Debug |
| Auth | Credentials | PIN + Credentials + multi-server auto-detect |
| Subtitles | ❌ | ✅ Toggle + size selection |
| Controller Navigation | Manual wiring | Automatic D-pad / stick focus |
| Touch Support | Limited | Full touchscreen |

---

## ✅ Requirements

- **PS Vita** with custom firmware — HENkaku / Enso / h-encore (firmware **3.60 or later**)
- **VitaShell** (or equivalent) to install `.vpk` files
- A running **Plex Media Server** reachable from the Vita's network
- Plex Pass is **not required** for general playback. It **is** required for Live TV / DVR features.

---

## 📦 Installation

1. Download the latest `VitaPlex.vpk` from the [Releases](https://github.com/Breezyslasher/Vita_plex/releases) page.
2. Transfer it to your Vita (FTP via VitaShell, USB, or memory card).
3. Open VitaShell → select `VitaPlex.vpk` → press **X** → install.
4. Launch **Vita Plex** from the LiveArea.
5. Sign in with the Plex PIN flow (visit `plex.tv/link` and enter the 4-character code) or your Plex username/password.
6. The app will auto-detect your servers and connect to the first available one.

Settings and auth tokens are saved to:

```
ux0:data/VitaPlex/settings.json
```

Debug logs are written to:

```
ux0:data/VitaPlex/vitaplex.log
```

---

## 🎮 Controls

| Button | Action |
|---|---|
| **D-Pad / Left Stick** | Navigate focus |
| **✕ (Cross)** | Select / Confirm |
| **◯ (Circle)** | Back |
| **△ (Triangle)** | Search |
| **□ (Square)** | Context action (varies by screen) |
| **L / R** | Seek ±10 s in player |
| **LB / RB** | Reorder in music queue |
| **Start** | Menu / Exit |
| **Touchscreen** | Tap to select, drag to scroll |

---

## 🔨 Building from Source

### Prerequisites

- [VitaSDK](https://vitasdk.org/) with `$VITASDK` exported
- CMake 3.14+
- Git (for submodules)

### Build

```bash
git clone --recursive https://github.com/Breezyslasher/Vita_plex.git
cd Vita_plex
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/cmake/vitasdk.cmake
make -j$(nproc)
```

The resulting `VitaPlex.vpk` will be in the `build/` directory.

### CI Builds

Every push to the `Fix-andriod` branch triggers a GitHub Actions workflow that builds the VPK inside the official `vitasdk/vitasdk` Docker container. Artifacts are attached to each Actions run and tagged commits publish a release automatically.

---

## 🗺️ Roadmap

- [x] Borealis UI rewrite
- [x] Plex PIN + credential login
- [x] Multi-server auto-detect
- [x] Music playback + queue
- [x] Offline downloads (movies, shows, music)
- [x] Live TV program guide
- [x] Subtitle support
- [ ] Stable HLS video playback on hardware
- [ ] Background download continuation
- [ ] Chromecast-style "Play On" another Plex client
- [ ] Android port (tracked on the `Fix-andriod` branch)

---

## 🙏 Credits

- **[Borealis](https://github.com/dragonflylee/borealis)** — UI framework (Nintendo Switch-inspired)
- **[MPV](https://mpv.io/)** — Media playback engine (Vita-patched from the Switchfin project)
- **[VitaSDK](https://vitasdk.org/)** — PS Vita toolchain
- **[Switchfin](https://github.com/dragonflylee/switchfin)** — Reference MPV + Borealis build for Vita
- **Plex** — for the Plex Media Server and public API
- All contributors and homebrew scene devs who made this possible

---

## 📄 License

Released under the **GPL-3.0** license. See [`LICENSE`](LICENSE) for the full text.

> ⚠️ This is an unofficial, community-made client. **Vita_plex is not affiliated with Plex, Inc. or Sony.** "Plex" and the Plex logo are trademarks of Plex, Inc.
