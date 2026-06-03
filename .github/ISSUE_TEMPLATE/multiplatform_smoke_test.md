---
name: Cross-Platform Smoke Test
about: Use this checklist to verify a new build works on every supported platform
title: "[Smoke Test] <platform> — <build version>"
labels: testing, multiplatform
assignees: ''
---

# Cross-Platform Smoke Test Checklist

Use this checklist to confirm VitaPlex works on the platform under test.
Focus on basic functionality only — not full regression testing.

**Platform tested:** <!-- PS Vita / PS4 / Switch / Android / Android TV / Desktop -->
**Device:** <!-- e.g. PS Vita PCH-2000, Sony Bravia A1 OLED, Chromecast w/ Google TV, Pixel 6, Steam Deck -->
**Build commit / version:**
**Plex server version:**

---

## App Startup & Connection

* [ ] App launches without crashing
* [ ] Sign in to Plex account (plex.tv OAuth flow completes)
* [ ] Server picker shows available servers
* [ ] Connect to selected Plex server
* [ ] App does not freeze during startup
* [ ] Auth token + server URL persist after restart

---

## Home Screen

* [ ] Home tab loads without errors
* [ ] "Continue Watching" row populates with in-progress items
* [ ] "On Deck" row populates
* [ ] "Recently Added" rows populate (per library)
* [ ] Posters / thumbnails load on home rows
* [ ] Horizontal scroll within a row works (DPAD / touch)
* [ ] Selecting a continue-watching item resumes playback at the correct offset

---

## Library & Navigation

* [ ] Movies library loads
* [ ] TV Shows library loads
* [ ] Music library loads
* [ ] Photo library loads (if applicable)
* [ ] Movie posters display correctly
* [ ] Movie titles display correctly under posters
* [ ] "All" / "Categories" filter chips work
* [ ] Pagination loads the next page when scrolling to the bottom
* [ ] No blank strips while scrolling (visibility cull is correct)
* [ ] Sidebar navigation between tabs works
* [ ] No UI layout issues (cut-off text, oversized elements)

---

## Movie / TV Show Detail

* [ ] Open a movie's detail page
* [ ] Movie poster loads in the detail header
* [ ] Title, year, runtime, rating show correctly
* [ ] Summary / description shows after a brief load
* [ ] Audio track picker appears with the file's tracks
* [ ] Subtitle picker appears with available subtitles
* [ ] Extras row populates (trailers / featurettes) for movies
* [ ] Open a TV show's detail page
* [ ] Seasons load with correct artwork
* [ ] Selecting a season loads its episodes
* [ ] Episode stills load
* [ ] Episode S01E01 subtitle line shows
* [ ] "Mark Watched" / "Mark Unwatched" works and persists

---

## Music

* [ ] Open an artist page (categories, albums load)
* [ ] Open an album page
* [ ] Album cover loads
* [ ] Track list populates with titles + durations
* [ ] Tap a track to play it
* [ ] Background music continues after leaving the player (if setting enabled)
* [ ] Queue / next track behavior is correct

---

## Player

* [ ] Movie plays back smoothly (no excessive frame drops)
* [ ] Audio is in sync
* [ ] Subtitles render correctly
* [ ] Pause / play / seek with controller works
* [ ] Pause / play / seek with on-screen controls works
* [ ] Seek bar shows correct position
* [ ] Audio track switch at runtime works
* [ ] Subtitle track switch at runtime works
* [ ] Progress is reported to the server (scrobble shows up in Plex)
* [ ] Exit player returns to detail view in the correct state
* [ ] Resume from a partially-watched item starts at the saved offset
* [ ] Live TV channel plays (if applicable)
* [ ] DVR recording plays (if applicable)

---

## Search

* [ ] Search tab opens
* [ ] Typing a query returns results
* [ ] Results include movies, shows, episodes, music as appropriate
* [ ] Selecting a result opens its detail page

---

## Downloads

* [ ] Downloads tab opens without crashing
* [ ] Download a movie / episode from its detail page
* [ ] Download progress updates
* [ ] Downloaded item plays offline
* [ ] Auto-delete after watched works (if setting enabled)

---

## Settings

* [ ] Settings menu opens
* [ ] Changing video quality saves and applies on next playback
* [ ] Toggling "Hide Titles Under Posters" reflects in the library grid
* [ ] Toggling "MPV Stats Overlay" shows the panel during playback
* [ ] Default subtitle language saves and prefills the search dialog
* [ ] Changing a setting persists after app restart

---

## Platform-Specific Checks

* [ ] Primary input works (controller / touch / keyboard / remote)
* [ ] D-pad navigation moves focus correctly
* [ ] Back / Circle / Esc returns to the previous screen
* [ ] UI scaling looks correct for the platform's resolution
* [ ] No missing icons or fonts
* [ ] No crashes when switching tabs rapidly
* [ ] App works after a clean restart
* [ ] Picture-in-Picture works (Android / Android TV only)
* [ ] System back button doesn't kill mid-playback unexpectedly

---

## Super Quick Test (minimum)

If short on time, verify:

* [ ] App launches
* [ ] Sign in / connect to Plex server
* [ ] Open Movies library
* [ ] Open a movie's detail page
* [ ] Start playback
* [ ] Pause / seek / resume
* [ ] Exit player
* [ ] Settings open
* [ ] Restart app successfully

---

## Notes / Issues Found

<!-- Paste logs, screenshots, or describe any problems below. -->
