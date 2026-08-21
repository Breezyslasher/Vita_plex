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

/// Launch the separate updater helper app (title VPLX00003), which installs
/// the downloaded pkg (/data/VitaPlex/update.pkg, by convention) while
/// VitaPlex is closed — the only way to replace the running title, since the
/// system won't install over it in place. VitaPlex must quit right after a
/// successful launch so the helper can take over.
///
/// Returns 0 if the helper was launched (VitaPlex should now quit); non-zero
/// if it could not be launched — most likely the helper pkg isn't installed —
/// with `err` set so the caller can fall back to a manual install.
int launchUpdater(std::string& err);

/// Install the updater helper app, whose pkg ships inside VitaPlex's own pkg
/// (so the user only ever installs one app). Registers with the system
/// installer exactly like any other package — the helper is a different title,
/// so this is not blocked by "already installed". Call this when
/// launchUpdater() reports the helper is missing; the install runs in the
/// background, so the user retries the update once it finishes.
///
/// Returns 0 once the install is registered and started.
int installUpdaterApp(std::string& err);

/// Whether the updater helper is installed and ready to launch. Poll this
/// while waiting for installUpdaterApp() to finish — do NOT probe by calling
/// launchUpdater(), because every launch of a not-yet-installed title pops a
/// system "Cannot start the application" (CE-40841-7) dialog on screen.
bool isUpdaterInstalled();

/// Remove the updater helper if it's installed. Call this from VitaPlex once an
/// update has completed — the helper is only needed while one installs, and
/// VitaPlex reinstalls it on demand. Cleaning up from here rather than letting
/// the helper uninstall itself matters: a title that removes its own running
/// self is killed mid-call and the system reports a crash (CE-36329-3).
void removeUpdaterApp();

}  // namespace ps4
#endif
