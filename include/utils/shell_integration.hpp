/**
 * VitaPlex - shell integration
 *
 * Two things the surrounding system can do that the app's own window cannot:
 * say something when a background job finishes while the user is elsewhere,
 * and show how far along that job is without being looked at.
 *
 * Both exist on Linux and on Android, by completely different means — a
 * session-bus call on one, a NotificationManager post on the other — so the
 * interface is the concept and each backend does it its own way:
 *
 *   Linux    org.freedesktop.Notifications, plus the launcher progress bar
 *            (com.canonical.Unity.LauncherEntry) that KDE Plasma and the
 *            Ubuntu dock draw over the app icon.
 *   Android  A notification on the "Downloads" channel, and an ongoing
 *            notification carrying a progress bar — which is where Android
 *            puts this, there being no launcher icon to draw on.
 *
 * Everywhere else the inline no-ops below are what callers link against, so
 * no call site needs an #ifdef.
 */

#pragma once

#include <string>

namespace vitaplex {
namespace shell {

#if defined(VITAPLEX_MPRIS) || defined(__ANDROID__)

// Tell the user something finished. Fire and forget: no notification daemon,
// or a revoked Android notification permission, is not an error worth
// surfacing — and never worth failing the job that triggered it.
void notify(const std::string& summary, const std::string& body);

// Report ongoing background work, 0.0-1.0. Call with visible=false to take it
// away; left behind, it sits at whatever fraction the work stopped on.
void setProgress(double fraction, bool visible);

#else

inline void notify(const std::string&, const std::string&) {}
inline void setProgress(double, bool) {}

#endif

} // namespace shell
} // namespace vitaplex
