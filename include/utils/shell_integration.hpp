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

#if defined(VITAPLEX_MPRIS) || defined(__ANDROID__) || defined(_WIN32)

// Called once at startup, before any window exists. Only Windows does anything
// with it: it declares the process's AppUserModelID, which is the identity the
// shell groups the taskbar button under and sends toasts as.
void init();

// Tell the user something finished. Fire and forget: no notification daemon,
// or a revoked Android notification permission, is not an error worth
// surfacing — and never worth failing the job that triggered it.
void notify(const std::string& summary, const std::string& body);

// Report ongoing background work, 0.0-1.0. Negative means "working, size not
// known yet" where the backend can draw that. Call with visible=false to take
// it away; left behind, it sits at whatever fraction the work stopped on.
//
// title and detail are for backends with somewhere to put text — Android's
// notification uses them as its two lines. The launcher bar on Linux and the
// taskbar bar on Windows draw a fraction and nothing else, so they ignore both;
// pass them anyway rather than making the caller ask which backend it has.
void setProgress(double fraction, const std::string& title,
                 const std::string& detail, bool visible);

#else

inline void init() {}
inline void notify(const std::string&, const std::string&) {}
inline void setProgress(double, const std::string&, const std::string&, bool) {}

#endif

// Windows only, and only meaningful there: whether the app may write the Start
// Menu shortcut that toast notifications are keyed on. A portable install can
// turn it off and leave nothing behind on the machine, at the cost of a
// flashing taskbar button instead of a toast. No-op elsewhere.
#if defined(_WIN32)
void setShortcutAllowed(bool allowed);
#else
inline void setShortcutAllowed(bool) {}
#endif

} // namespace shell
} // namespace vitaplex
