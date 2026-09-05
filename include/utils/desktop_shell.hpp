/**
 * VitaPlex - Linux desktop shell integration
 *
 * Two things the shell offers that a plain window cannot do for itself:
 * a notification when something finishes while the app is in the background,
 * and the progress bar drawn over the launcher icon.
 *
 * Both are session-bus calls, so both are compiled only where MPRIS is —
 * that is, on Linux with dbus-1 present at configure time. Everywhere else the
 * inline no-ops below are what callers link against, so nothing needs an
 * #ifdef at the call site.
 */

#pragma once

#include <string>
#include <cstdint>

namespace vitaplex {
namespace desktopshell {

#if defined(VITAPLEX_MPRIS)

// Post a desktop notification (org.freedesktop.Notifications). Fire and forget:
// a missing notification daemon is not an error worth surfacing.
void notify(const std::string& summary, const std::string& body);

// Draw a progress bar over the launcher icon, 0.0-1.0. Implemented by KDE
// Plasma and the Ubuntu dock via com.canonical.Unity.LauncherEntry; shells that
// do not listen simply never see the signal.
void setLauncherProgress(double fraction, bool visible);

// Badge the launcher icon with a number — how many downloads are still running.
void setLauncherCount(int64_t count, bool visible);

#else

inline void notify(const std::string&, const std::string&) {}
inline void setLauncherProgress(double, bool) {}
inline void setLauncherCount(int64_t, bool) {}

#endif

} // namespace desktopshell
} // namespace vitaplex
