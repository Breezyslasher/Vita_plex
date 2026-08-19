/*
    VitaPlex — PS4 in-app self-update
*/

#pragma once

#ifdef __PS4__
#include <string>

namespace ps4 {

/// Hand a downloaded PKG to the system's background-file-transfer service
/// (BGFT) — the flatz Remote Package Installer technique that GoldHEN's
/// Direct PKG installer is built on. Registration is asynchronous: on
/// success the system takes over, shows its own progress notification, and
/// the app should exit so the installer can replace it.
///
/// - `pkgPath`     : the downloaded `.pkg` on /data (content id is read
///                   from its header).
/// - `contentName` : display name for the system's install notification.
/// - `err`         : filled with a human-readable reason on failure.
///
/// Returns 0 once the install task is registered and started.
int installPkg(const std::string& pkgPath, const std::string& contentName, std::string& err);

}  // namespace ps4
#endif
