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
#include <vector>
#include <functional>
#include <cstdint>

namespace vitaplex {
namespace nowplaying {

// Repeat mode mirrored to / from the OS controls (kept independent of the music
// queue's enum so the bridge stays generic — MusicController maps between them).
enum class RepeatMode { Off, All, One };

// Snapshot of what's playing, handed to the OS each time it changes.
struct Info {
    std::string mediaId;       // browse-tree id of this track ("track/<key>"),
                               // so the OS can offer it again after a reboot;
                               // empty for anything not in the music library
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

// One row of the play queue as the OS media controls list it. `id` is the
// caller's own handle for the row — VitaPlex passes the absolute queue index —
// and comes straight back through the skip-to-item handler.
struct QueueEntry {
    int64_t id = 0;
    std::string mediaId;   // browse-tree id, so a client that plays a queue row
                           // by media id instead of by queue id still works
    std::string title;
    std::string artist;
    std::string artUrl;
};

// Publish / refresh the OS media session + notification. No-op off Android.
void update(const Info& info);

// Publish the play queue (in play order, so shuffled means shuffled) and which
// row is playing. Android only — Auto, Assistant and Wear render it as the
// up-next list; a no-op elsewhere. An empty vector drops the queue.
//
// The list crosses a Binder transaction, so callers should send a window around
// the current track rather than a thousand-track library, and should only call
// this when the queue actually changed.
void setQueue(const std::vector<QueueEntry>& items, int64_t activeId);

// Tear the session + notification down (playback stopped / queue emptied).
void clear();

// Register the handler that receives transport commands from the OS controls.
// onTransport handles the discrete buttons; onSeekMs carries an absolute seek
// position in milliseconds; onSetRepeat / onSetShuffle carry an explicit target
// repeat mode / shuffle flag (the SMTC + MPRIS controls request a specific state
// rather than a cycle). All are invoked on the UI (main) thread.
// onSkipToQueueItem carries the `id` of a QueueEntry the user picked out of the
// published queue; onSetRating carries Android's "like this track" heart. Both
// are music-only, so a video session leaves them null and the OS request is
// simply dropped.
void setHandler(std::function<void(Transport)> onTransport,
                std::function<void(int64_t)> onSeekMs,
                std::function<void(RepeatMode)> onSetRepeat = nullptr,
                std::function<void(bool)> onSetShuffle = nullptr,
                std::function<void(int64_t)> onSkipToQueueItem = nullptr,
                std::function<void(bool)> onSetRating = nullptr);
void clearHandler();

// Called by the platform layer (Android JNI / desktop backends) when the OS
// sends a command. Marshals onto the UI thread and invokes the registered
// handler.
void dispatchTransport(Transport t);
void dispatchSeek(int64_t positionMs);
void dispatchSetRepeat(RepeatMode mode);
void dispatchSetShuffle(bool on);
void dispatchSkipToQueueItem(int64_t id);
void dispatchSetRating(bool liked);

} // namespace nowplaying
} // namespace vitaplex
