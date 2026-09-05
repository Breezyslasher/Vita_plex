/**
 * VitaPlex - shell integration
 *
 * Two things the surrounding system can do that the app's own window cannot:
 * say something when a background job finishes while the user is elsewhere,
 * and show how far along that job is without being looked at.
 *
 * Every platform that has this at all does it by completely different means,
 * so the interface is the concept and each backend does it its own way:
 *
 *   Linux    org.freedesktop.Notifications, plus the launcher progress bar
 *            (com.canonical.Unity.LauncherEntry) that KDE Plasma and the
 *            Ubuntu dock draw over the app icon.
 *   Windows  A toast keyed on the process AppUserModelID, with a <progress>
 *            element updated in place, plus the taskbar button's own bar.
 *   Android  A notification on the "Downloads" channel, and an ongoing
 *            notification carrying a progress bar — which is where Android
 *            puts this, there being no launcher icon to draw on.
 *   Vita     SceNotificationUtil, including its BGDL progress form, so a
 *            download looks like any other download on the console.
 *   PS4      The SceShellUI popup via sceKernelSendNotificationRequest.
 *            Text only — the system download list is not reachable from a
 *            homebrew app, so there is nowhere to put progress.
 *
 * Switch has no equivalent: libnx's notif service schedules alarms and
 * cannot show a message from a running application, so it falls through to
 * the inline no-ops below — as does every other platform, so that no call
 * site needs an #ifdef.
 */

#pragma once

#include <string>

namespace vitaplex {
namespace shell {

#if defined(VITAPLEX_MPRIS) || defined(__ANDROID__) || defined(_WIN32) || \
    defined(__vita__) || defined(__PS4__)

// Called once at startup, before any window exists. Windows declares the
// process's AppUserModelID here — the identity the shell groups the taskbar
// button under and sends toasts as — and the Vita loads the notification
// sysmodule. Everywhere else it does nothing.
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
