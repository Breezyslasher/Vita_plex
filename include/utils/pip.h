#pragma once

/**
 * VitaPlex - Picture-in-Picture helper
 *
 * Cross-platform PiP abstraction.
 *
 * Android: calls VitaPlexActivity.enterPiP(aspectNum, aspectDen) via JNI.
 *          Android then manages PiP lifecycle; there is no explicit "exit"
 *          — the user leaves PiP by tapping the window or returning to the
 *          app.
 *
 * Desktop (Linux/Mac/Windows via SDL2): shrinks the SDL window to a small
 *          bottom-right corner size with always-on-top enabled. A second
 *          call restores the previous window geometry and decorations.
 *
 * Other platforms (Vita, Switch, PS4): PiP is not available — the helpers
 *          return false.
 */

namespace vitaplex {
namespace pip {

// Is PiP available on the current platform?
bool isAvailable();

// Are we currently in PiP?
bool isActive();

// Enter PiP mode. Aspect ratio is used on Android to size the PiP window.
// Returns false if PiP is not supported or the request failed.
bool enter(int videoWidth, int videoHeight);

// Exit PiP (desktop only — on Android the user exits PiP via the system UI).
// Returns false if not currently in PiP or not supported.
bool leave();

// Convenience: toggle between PiP and normal mode.
bool toggle(int videoWidth, int videoHeight);

// Publish current video playback state to the platform. On Android this
// enables "auto PiP on Home" (onUserLeaveHint). No-op elsewhere.
void setVideoPlaybackState(bool playing, int videoWidth, int videoHeight);

} // namespace pip
} // namespace vitaplex
