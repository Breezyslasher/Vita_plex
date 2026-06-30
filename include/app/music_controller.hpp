/**
 * VitaPlex - MusicController
 *
 * A persistent (singleton) owner of music playback transport that outlives the
 * PlayerActivity. It exists so the OS media notification (and headless auto-
 * advance) keep working after the user leaves the player with background music
 * enabled — at which point PlayerActivity is destroyed but MpvPlayer + MusicQueue
 * keep going.
 *
 * Handoff model (keeps the rich foreground path untouched and low-risk):
 *   - While PlayerActivity is alive it attaches foreground hooks; the controller
 *     routes next/previous/track-ended through them so the on-screen player drives
 *     the load and stays perfectly in sync (existing behaviour).
 *   - When PlayerActivity detaches (closed / background music), the controller
 *     takes over: it owns the MusicQueue end-of-track callback, polls MpvPlayer
 *     for end-of-track, advances the queue and loads the next track itself, and
 *     keeps the OS media session updated.
 *
 * play/pause/seek always go straight to the MpvPlayer singleton, so they work in
 * either mode.
 */

#pragma once

#include <borealis.hpp>
#include <functional>

#include "app/music_queue.hpp"

namespace vitaplex {

class MusicController {
public:
    static MusicController& getInstance();

    // Hooks the live PlayerActivity registers so the controller delegates the
    // UI-affecting transport to it instead of loading headlessly.
    struct ForegroundHooks {
        std::function<void()> onNext;                       // play next (rich UI load)
        std::function<void()> onPrevious;                   // play previous
        std::function<void(const QueueItem*)> onTrackEnded; // auto-advance handler
        std::function<void(bool)> onSetShuffle;             // server-aware shuffle + icon refresh
        std::function<void(RepeatMode)> onSetRepeat;        // set repeat + icon refresh
    };

    // Called by PlayerActivity on create (attach) and on destroy / background
    // hand-off (detach). detach() starts headless driving if music is playing.
    void attachForeground(ForegroundHooks hooks);
    void detachForeground();
    bool hasForeground() const { return m_hasForeground; }

    // Push the current track + playback state to the OS media session. Call when
    // the track changes and when play/pause toggles. playingOverride forces the
    // play/pause flag (1 playing, 0 paused) instead of querying MpvPlayer, whose
    // state lags the play()/pause() command by an async event — without it the
    // notification needs a second press to catch up. -1 = query MpvPlayer.
    void publishNowPlaying(int playingOverride = -1);
    // Stop and clear the OS media session/notification.
    void stopSession();

    // Re-publish the session if MpvPlayer's *settled* play/pause state diverged
    // from what we last sent — catches changes we didn't trigger (audio-focus
    // pause, a stall, an optimistic state that didn't take). Cheap; call it from
    // the per-second timers that already run (the headless poll + the foreground
    // player's update timer). Ignores transient LOADING/BUFFERING.
    void syncSessionState();

    // Transport entry points. These are also the targets of the OS media buttons
    // (wired through nowplaying::setHandler in install()).
    void togglePlayPause();
    void playPause(bool play);
    void next();
    void previous();
    void seekToMs(long long ms);
    void seekRelativeMs(long long deltaMs);   // fast-forward / rewind keys
    void stopPlayback();                       // Stop key: halt mpv + clear session

    // Repeat / shuffle from the OS controls. set* take an explicit target (SMTC /
    // MPRIS); cycle/toggle advance from the current state (Android custom actions).
    // All update the queue, refresh the on-screen player if attached, and re-publish.
    void setShuffleMode(bool on);
    void toggleShuffleMode();
    void setRepeatMode(RepeatMode mode);
    void cycleRepeatMode();

private:
    MusicController() = default;
    MusicController(const MusicController&) = delete;
    MusicController& operator=(const MusicController&) = delete;

    void install();                 // one-time: queue callback + OS handler
    void registerOsHandler();       // (re)claim the nowplaying transport handler
    void handleTrackEnded(const QueueItem* nextTrack);
    bool loadCurrentHeadless();     // minimal URL resolve + mpv loadUrl (no UI)
    void startPolling();
    void stopPolling();

    bool m_installed = false;
    bool m_hasForeground = false;
    bool m_polling = false;
    bool m_endHandled = false;
    bool m_sessionActive = false;        // a session/notification is currently up
    bool m_lastPublishedPlaying = false; // play flag of the most recent publish
    ForegroundHooks m_fg;
    brls::RepeatingTimer m_pollTimer;  // headless end-of-track watcher
};

} // namespace vitaplex
