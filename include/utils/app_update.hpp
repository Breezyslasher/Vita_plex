/**
 * VitaPlex - In-app updates
 *
 * Checks the GitHub releases feed and, where the platform allows it,
 * installs the update in place. Modelled on pleNx's updater
 * (thcolin/gamepad-media-center-aggregator, app/src/utils/version.cpp):
 * the same check → offer dialog → per-platform install flow, adapted to
 * VitaPlex's HttpClient and release layout. The offer names the new and
 * current versions only — release notes stay on the GitHub page.
 *
 *   Switch   — downloads the matching .nro and replaces the running one
 *              (path captured from argv[0]); relaunch applies it.
 *   PS Vita  — downloads the .vpk. A title can't promote over itself
 *              (0x80101114 = in use), so VitaPlex installs and launches a
 *              tiny bundled updater stub (VPLXUPD01) and quits; with
 *              VitaPlex closed the stub promotes the .vpk and relaunches
 *              VitaPlex (the AutoPlugin2 technique, utils/vita_install +
 *              src/updater_stub). VitaPlex removes the leftover stub on its
 *              next start (like PS4's helper). Falls back to a VitaShell
 *              install if the stub can't be staged.
 *   Android  — downloads the .apk and hands it to the system package
 *              installer (content:// via ApkProvider — no browser
 *              involved, so Android TV works too).
 *   PS4      — downloads the .pkg and registers it with the system
 *              installer (BGFT, utils/ps4_install); the user exits and
 *              the console finishes the install with its own progress.
 *   Linux    — AppImage replaces itself and relaunches; a .deb / Arch
 *              package is handed to the system installer (xdg-open).
 *              Flatpak and unrecognised installs stay browser-only.
 *   Windows  — downloads the portable .zip and hands a detached, windowless
 *              cmd a .bat that waits for VitaPlex to exit, unpacks the zip
 *              over the install folder (tar / Expand-Archive) and relaunches.
 *   macOS    — downloads the .dmg and, from a detached shell helper, mounts
 *              it and swaps the running VitaPlex.app bundle, then reopens it.
 *              A loose (non-.app) binary stays browser-only.
 *   Other / iOS / tvOS — opens the release asset in the browser.
 *
 * VitaPlex publishes pre-releases, which GitHub's /releases/latest never
 * returns — the check reads /releases and takes the newest non-draft
 * entry instead.
 */

#pragma once

#include <string>

namespace vitaplex {
namespace app_update {

// Remember where our own executable lives (Switch: argv[0] from hbloader,
// the file the updater must replace). Call once from main() before brls
// starts; a no-op elsewhere.
void setSelfPath(const char* argv0);

// Check GitHub for a newer release. `manual` distinguishes the settings
// cell from the startup check: manual reports "up to date" and re-offers
// a release the user previously skipped; the startup check stays silent
// in both cases. Network and parsing run on a worker thread.
void checkForUpdates(bool manual);

}  // namespace app_update
}  // namespace vitaplex
