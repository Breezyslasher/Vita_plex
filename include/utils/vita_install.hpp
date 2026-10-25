/*
    VitaPlex — PS Vita in-app self-update (ported from pleNx, thcolin/gamepad-media-center-aggregator)
*/

#pragma once

#ifdef __PSV__
#include <functional>
#include <string>

namespace vita {

// The stage stageVpk is in, reported to its progress callback so a UI can
// draw a step checklist: the archive unpacked, then the head.bin forge.
enum InstallPhase {
    PHASE_EXTRACT = 0,   // done/total = archive entries unpacked
    PHASE_VERIFY  = 1,   // done/total = 0/1 then 1/1 around the head.bin forge
};

/// Ready a homebrew VPK already downloaded to disk for the system's installer:
/// extract it to `stageDir` and forge its fake-package `head.bin`, the
/// technique every Vita homebrew installer uses (VitaShell, VitaDB
/// Downloader, …), which relies on the HENkaku-patched promoter present on
/// every hacked Vita. VitaPlex cannot hand the folder to the installer
/// itself, which refuses the title running: the shell companion plugin does,
/// in SceShell, once VitaPlex has quit (SC_CMD_UPDATE_INSTALL).
///
/// - `vpkPath`    : the downloaded `.vpk` (a plain ZIP).
/// - `stageDir`   : a writable directory under `ux0:`, wiped and recreated by
///                  this call; on success it holds the package to install.
/// - `err`        : filled with a human-readable reason on failure.
/// - `onProgress` : called as (phase, done, total) — see InstallPhase. Fires
///                  per archive entry during extract, and once each side of
///                  the head.bin forge. Runs on the caller's thread.
///
/// Returns 0 on success; on failure nothing is left in `stageDir`.
int stageVpk(const std::string& vpkPath, const std::string& stageDir, std::string& err,
             std::function<void(int phase, int done, int total)> onProgress = {});

/// Delete a directory tree, best effort (a stage left from an update).
void removeTree(const std::string& path);

/// Uninstall the updater stub (title VPLXUPD01) if it's installed, via
/// ScePromoterUtil. Builds up to 2055 installed it fresh from a bundled
/// updater.vpk for each update, which it then installed, and left it on the
/// LiveArea for VitaPlex to remove on its next start — the PS4
/// removeUpdaterApp() counterpart. An update into this build is still made
/// that way, by the build before. Safe because it's a different title (a
/// title can't uninstall its own running self); a no-op when the stub isn't
/// installed.
void removeUpdaterStub();

}  // namespace vita
#endif
