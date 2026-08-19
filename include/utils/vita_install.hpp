/*
    VitaPlex — PS Vita in-app self-update (ported from pleNx, thcolin/gamepad-media-center-aggregator)
*/

#pragma once

#ifdef __PSV__
#include <functional>
#include <string>

namespace vita {

/// Self-update from a VPK already downloaded to disk: extract it to a scratch
/// directory (fully validating the archive first), then overwrite the
/// installed files at ux0:app/<TITLE_ID>/ in place — the way VitaShell's own
/// self-updater works. ScePromoterUtility is deliberately NOT used: promoting
/// a title that is currently running fails with 0x80101114.
///
/// - `vpkPath`    : the downloaded `.vpk` (a plain ZIP) to install.
/// - `workDir`    : a writable scratch directory under `ux0:` used to extract
///                  the package before the overwrite; wiped and recreated by
///                  this call.
/// - `err`        : filled with a human-readable reason on failure.
/// - `onProgress` : called after each archive entry as (done, total) — covers
///                  extraction only, which is the long part; the copy that
///                  follows reports nothing. Runs on the caller's thread.
///
/// Returns 0 on success. The running executable keeps executing from RAM;
/// the caller restarts into the new version via
/// sceAppMgrLoadExec("app0:eboot.bin", ...).
int installVpk(const std::string& vpkPath, const std::string& workDir, std::string& err,
               std::function<void(int done, int total)> onProgress = {});

}  // namespace vita
#endif
