/**
 * VitaPlex - OS "Now Playing" media session bridge
 *
 * Publishes the currently playing music track to the operating system's media
 * controls (lock screen / notification shade on Android; a no-op elsewhere) and
 * routes the transport buttons the OS sends back (play/pause/next/previous/seek)
 * to a handler the app registers — see MusicController.
 *
 * The platform-specific implementation lives in the same .cpp, guarded by
 * __ANDROID__; every other target links the no-op stubs so callers don't need
 * their own #ifdefs.
 */

#pragma once

#include <string>
#include <functional>
#include <cstdint>

namespace vitaplex {
namespace nowplaying {

// Repeat mode mirrored to / from the OS controls (kept independent of the music
// queue's enum so the bridge stays generic — MusicController maps between them).
enum class RepeatMode { Off, All, One };

// Snapshot of what's playing, handed to the OS each time it changes.
struct Info {
    std::string title;
    std::string artist;
    std::string album;
    std::string artUrl;        // http(s) thumbnail URL, or a local file path for
                               // downloaded tracks; empty for none
    int64_t durationMs = 0;
    int64_t positionMs = 0;
    bool playing = false;      // true = playing, false = paused
    bool hasNext = false;      // gate the next/previous buttons
    bool hasPrev = false;
    RepeatMode repeat = RepeatMode::Off;  // current repeat state
    bool shuffle = false;                 // current shuffle state
    bool showRepeat = false;   // expose the repeat control (music only, not video)
    bool showShuffle = false;  // expose the shuffle control (music only, not video)
};

// Transport commands the OS controls can send back to us. CycleRepeat /
// ToggleShuffle are the "advance to next state" variants the Android custom
// actions use; SMTC/MPRIS instead hand us an explicit target via the setHandler
// repeat/shuffle callbacks below.
enum class Transport { Toggle, Play, Pause, Next, Previous, Stop, FastForward, Rewind,
                       CycleRepeat, ToggleShuffle };

// Publish / refresh the OS media session + notification. No-op off Android.
void update(const Info& info);

// Tear the session + notification down (playback stopped / queue emptied).
void clear();

// Register the handler that receives transport commands from the OS controls.
// onTransport handles the discrete buttons; onSeekMs carries an absolute seek
// position in milliseconds; onSetRepeat / onSetShuffle carry an explicit target
// repeat mode / shuffle flag (the SMTC + MPRIS controls request a specific state
// rather than a cycle). All are invoked on the UI (main) thread.
void setHandler(std::function<void(Transport)> onTransport,
                std::function<void(int64_t)> onSeekMs,
                std::function<void(RepeatMode)> onSetRepeat = nullptr,
                std::function<void(bool)> onSetShuffle = nullptr);
void clearHandler();

// Called by the platform layer (Android JNI / desktop backends) when the OS
// sends a command. Marshals onto the UI thread and invokes the registered
// handler.
void dispatchTransport(Transport t);
void dispatchSeek(int64_t positionMs);
void dispatchSetRepeat(RepeatMode mode);
void dispatchSetShuffle(bool on);

} // namespace nowplaying
} // namespace vitaplex
