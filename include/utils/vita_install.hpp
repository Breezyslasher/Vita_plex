/*
    VitaPlex — PS Vita in-app self-update (ported from pleNx, thcolin/gamepad-media-center-aggregator)
*/

#pragma once

#ifdef __PSV__
#include <functional>
#include <string>

namespace vita {

/// Install a homebrew VPK already downloaded to disk, in place, by forging its
/// fake-package `head.bin` and handing the extracted directory to
/// ScePromoterUtility. This is the technique every Vita homebrew installer uses
/// (VitaShell, VitaDB Downloader, …); it relies on the HENkaku-patched promoter
/// present on every hacked Vita.
///
/// - `vpkPath`    : the downloaded `.vpk` (a plain ZIP) to install.
/// - `workDir`    : a writable scratch directory under `ux0:` used to extract
///                  the package before promotion; wiped and recreated by this
///                  call.
/// - `err`        : filled with a human-readable reason on failure.
/// - `onProgress` : called after each archive entry as (done, total) — covers
///                  extraction only, which is the long part; the promotion
///                  that follows reports nothing. Runs on the caller's thread.
///
/// Returns 0 on success. The bubble is replaced in place, so the caller must
/// quit the app afterwards for the user to relaunch the new version.
int installVpk(const std::string& vpkPath, const std::string& workDir, std::string& err,
               std::function<void(int done, int total)> onProgress = {});

/// Launch an installed app by its title id (via a psgm:play URI). Used to hand
/// off to the updater stub, and by the stub to relaunch VitaPlex afterwards.
void launchTitle(const std::string& titleId);

}  // namespace vita
#endif
