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
 *   PS Vita  — downloads the .vpk. The Vita installer refuses to replace
 *              a title while it is the running app (0x80101114 = in use),
 *              so a promote is attempted (utils/vita_install) but normally
 *              can't complete for the running app; the .vpk is kept and
 *              the user finishes the install from VitaShell.
 *   Android  — downloads the .apk and hands it to the system package
 *              installer (content:// via ApkProvider — no browser
 *              involved, so Android TV works too).
 *   PS4      — downloads the .pkg and registers it with the system
 *              installer (BGFT, utils/ps4_install); the user exits and
 *              the console finishes the install with its own progress.
 *   Desktop  — opens the release asset in the browser.
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
