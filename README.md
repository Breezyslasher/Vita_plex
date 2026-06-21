# VitaPlex

A native Plex client for PlayStation Vita and beyond. Stream movies, TV, music, and live TV from your Plex Media Server directly on your device with full controller and touch support.

> **Note:** App is still in beta — some features don't fully work on certain platforms yet. Check [Issue 124](../../issues/124) for known bugs.

## Features

**Home & Browsing**

- Continue Watching, On Deck, Recently Added, and per-library hubs
- Library tab with grid view, batch operations, and library-specific sections
- Music tab with artists, albums, playlists, and album hubs (Albums, Singles, EPs)
- Search across movies, TV, music, and Live TV
- Hidden libraries and custom sidebar order

**Player (mpv-based)**

- Hardware-accelerated decoding (where the platform supports it)
- Direct Play first, transcode fallback with quality + bitrate controls
- Subtitle search (OpenSubtitles), size presets, default language
- Auto-skip intro / credits using Plex's markers
- Auto-play next episode, resume playback, adjustable seek interval
- Configurable controls auto-hide
- Optional on-screen mpv stats overlay for diagnostics

**Music**

- Background music: keep audio playing when you leave the player
- Configurable default track action (queue, play, ask)
- Artist hubs grouped by Albums / Singles / EPs

**Live TV & DVR**

- Full EPG grid with on-now hero card, per-row horizontal scrolling, sticky channel column
- Configurable guide window (6 / 12 / 24 hours)
- Hover live-updates the hero with the show under focus
- One-press recording from the guide
- DVR settings: default target library, start / end recording padding, partial recording toggle, minimum recording quality

**Plex Home users**

- User picker at login and app boot
- Auto-login as last-used user (toggleable)
- Switch User from Settings without logging out
- PIN-protected user support

**Downloads & Offline**

- Download any movie, episode, or album for offline viewing
- Bidirectional progress sync (push local progress, pull server progress)
- Delete-after-watch option
- Full offline mode for when the server is unreachable

**Customization**

- Light / dark / system theme
- Hide libraries, reorder sidebar, collapse sidebar to icons
- Hide titles in grid, skip single-season seasons view
- Toggle Collections, Playlists, and Genres in library views
- Per-platform sensible defaults (quality, bitrate, controls auto-hide)

## Mobile

| Platform | Status | Issues |
|:--|:--:|:--:|
| Android | ✅ Supported | None |
| iOS / iPadOS | ✅ Supported | |
| PS Vita | ✅ Supported | |

## Consoles

| Platform | Status | Issues |
|:--|:--:|:--:|
| PS4 | ✅ Supported | |
| Android TV | ✅ Supported | |
| Nintendo Switch | ✅ Supported | |
| Apple TV (tvOS) | ✅ Supported | |

## Desktop

| Platform | Status | Issues |
|:--|:--:|:--:|
| Windows | ✅ Supported | None |
| macOS | ✅ Supported | |
| Linux | ✅ Supported | None |

> [!NOTE]
> Some console platforms require modded/homebrew-enabled systems.

## Requirements

- Plex Media Server running and accessible
- Plex account (sign-in via username/password or plex.tv/link PIN)
- Network connectivity between device and server

**PS Vita:** Requires HENkaku/Enso
**Nintendo Switch:** Requires Atmosphère or similar CFW
**PS4:** Requires homebrew-enabled console

## Installation

**PS Vita:**

1. Download the VPK file from [Releases](../../releases)
2. Transfer to Vita via USB or FTP
3. Install using VitaShell
4. On first launch, sign in via username/password or plex.tv/link

**Nintendo Switch:**

1. Download the NRO file from [Releases](../../releases)
2. Copy to `/switch/VitaPlex/` on your SD card
3. Launch from the hbmenu

**PS4:**

1. Download the PKG file from [Releases](../../releases)
2. Install via your favorite homebrew installer

**Android / Android TV:** Install the APK and sign in on first launch.

**iOS / iPadOS / tvOS:** Build and sideload using AltStore / Xcode (signed builds are published on Releases when available).

**Desktop:** Download the appropriate build for Windows, macOS, or Linux from [Releases](../../releases) and run.

## License

[Add license information here]
