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
};

// Transport commands the OS controls can send back to us.
enum class Transport { Toggle, Play, Pause, Next, Previous, Stop, FastForward, Rewind };

// Publish / refresh the OS media session + notification. No-op off Android.
void update(const Info& info);

// Tear the session + notification down (playback stopped / queue emptied).
void clear();

// Register the handler that receives transport commands from the OS controls.
// onTransport handles the discrete buttons; onSeekMs carries an absolute seek
// position in milliseconds. Both are invoked on the UI (main) thread.
void setHandler(std::function<void(Transport)> onTransport,
                std::function<void(int64_t)> onSeekMs);
void clearHandler();

// Called by the platform layer (Android JNI) when the OS sends a command.
// Marshals onto the UI thread and invokes the registered handler.
void dispatchTransport(Transport t);
void dispatchSeek(int64_t positionMs);

} // namespace nowplaying
} // namespace vitaplex
