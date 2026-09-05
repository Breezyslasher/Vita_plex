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
#include <chrono>
#include <cstdint>
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
        std::function<void(int)> onPlayIndex;               // jump to a queue row (rich UI load)
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
    //
    // positionOverrideMs is the same idea for position: MpvPlayer::seekTo issues
    // an async mpv command, so getPosition() still reads the pre-seek value for
    // a while afterwards. Publishing that made the OS scrubber snap back to
    // where the drag started. -1 = query MpvPlayer.
    void publishNowPlaying(int playingOverride = -1, long long positionOverrideMs = -1);
    // Stop and clear the OS media session/notification.
    void stopSession();

    // Re-publish the session if MpvPlayer's *settled* play/pause state diverged
    // from what we last sent — catches changes we didn't trigger (audio-focus
    // pause, a stall, an optimistic state that didn't take). Also re-anchors the
    // position when it has drifted from what the OS must be showing, since the
    // OS extrapolates from the last publish and nothing corrects a divergence on
    // its own. Cheap; call it from the per-second timers that already run (the
    // headless poll + the foreground player's update timer). Ignores transient
    // LOADING/BUFFERING.
    void syncSessionState();

    // Transport entry points. These are also the targets of the OS media buttons
    // (wired through nowplaying::setHandler in install()).
    void togglePlayPause();
    void playPause(bool play);
    void next();
    void previous();
    // Absolute seek within the current track. Not simply an mpv seek: a Plex
    // music transcode is generated on the fly and served without range support,
    // so mpv can only move inside whatever it has already buffered — "Cannot
    // seek in this stream" for anything beyond that. When mpv reports the stream
    // unseekable this restarts the transcode at the target instead, which is
    // what Plex's offset= parameter exists for and what the video path already
    // does in restartTranscodeAtMs().
    void seekToMs(long long ms);
    void seekRelativeMs(long long deltaMs);   // fast-forward / rewind keys

    // Where the currently loaded stream begins, in ms into the track. Non-zero
    // once a seek has restarted the transcode part-way in: mpv then plays from 0
    // locally, so anything reporting an absolute position must add this back.
    long long streamStartOffsetMs() const { return m_streamStartOffsetMs; }
    // Called when a new track is loaded — the next stream starts at the top
    // again. PlayerActivity's own load path calls this too.
    void resetStreamStartOffset() { m_streamStartOffsetMs = 0; }
    void stopPlayback();                       // Stop key: halt mpv + clear session
    // Jump straight to a row of the published queue (absolute queue index), as picked in the OS up-next list.
    void playQueueIndex(int index);
    // "Like this track" from the OS controls: writes the Plex user rating of the
    // currently playing track (10 for liked, 0 to clear).
    void setCurrentTrackLiked(bool liked);

    // Sleep timer. Pauses playback after `minutes`; 0 cancels a running one.
    // Deliberately not persisted — it is a one-shot for tonight, not a setting
    // that should still be armed next week.
    void startSleepTimer(int minutes);
    int sleepTimerMinutes() const { return m_sleepMinutes; }
    // Whole minutes left, or 0 when nothing is armed. For the settings label.
    int sleepTimerRemaining() const;

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
    // Resolve the next track's stream URL while the current one still plays.
    // getTranscodeUrl() costs two blocking round-trips (/library/metadata, then
    // /decision) and runs on the main loop, so doing it at end-of-track put both
    // of them inside the silence between songs. Mirrors PlayerActivity's
    // prefetch; the pair (ratingKey, queue version) is the invalidation, and a
    // cached entry with an empty URL records a failed attempt so it is not
    // retried every tick.
    void prefetchNextTrack();
    void publishQueue();            // push the queue window to the OS, if changed
    void startPolling();
    void stopPolling();

    bool m_installed = false;
    bool m_hasForeground = false;
    bool m_polling = false;
    bool m_endHandled = false;
    bool m_sessionActive = false;        // a session/notification is currently up
    bool m_lastPublishedPlaying = false; // play flag of the most recent publish
    // Position anchor of the most recent publish, and when it was sent. The OS
    // runs its scrubber forward from this pair on its own, so these are what
    // syncSessionState() compares reality against.
    long long m_lastPublishedPositionMs = 0;
    std::chrono::steady_clock::time_point m_lastPublishAt{};
    // A seek we have told the OS about but mpv has not arrived at yet. Seeking
    // an HTTP stream is not instant — it re-buffers, and mpv keeps reporting the
    // old position meanwhile — so without this the drift check below would see a
    // huge gap one tick later and "correct" the scrubber straight back to where
    // the drag started. -1 = nothing in flight.
    long long m_pendingSeekMs = -1;
    std::chrono::steady_clock::time_point m_pendingSeekAt{};
    std::string m_lastPublishedRatingKey;  // to notice a track change
    // See streamStartOffsetMs(). Set when a seek restarts the transcode part-way
    // into the track, cleared whenever a track is loaded from its start.
    long long m_streamStartOffsetMs = 0;
    bool m_restartingTranscode = false;   // a restart is resolving; don't stack another
    // Restart the transcode so the stream itself begins at ms. Resolves the URL
    // off the UI thread — it costs two blocking round-trips — then reloads.
    void restartTranscodeAtMs(long long ms);
    // Fingerprint of the last queue window sent to the OS. publishNowPlaying()
    // runs every second; without this the whole list would cross JNI each time.
    uint64_t m_lastQueueSig = 0;
    // Next-track prefetch (see prefetchNextTrack).
    std::string m_prefetchKey;       // ratingKey the cached URL belongs to
    std::string m_prefetchUrl;       // empty = resolve was attempted and failed
    std::string m_prefetchSession;   // transcode session negotiated for that URL
    uint32_t m_prefetchVersion = 0;  // MusicQueue version the entry was built at
    int64_t  m_prefetchAtMs = 0;     // when the entry was resolved
    bool m_prefetchInFlight = false;
    ForegroundHooks m_fg;
    brls::RepeatingTimer m_pollTimer;  // headless end-of-track watcher
    // The poll runs four times a second so the gap between a track ending and
    // the next one loading stays short. syncSessionState() only needs the
    // original once-a-second cadence, so it fires on every fourth tick.
    int m_pollTick = 0;
    // Sleep timer. A repeating one-second tick rather than a single delayed
    // callback, so the remaining time can be shown and a cancel takes effect
    // immediately.
    brls::RepeatingTimer m_sleepTimer;
    int m_sleepMinutes = 0;            // what the user picked, 0 = off
    int m_sleepSecondsLeft = 0;
};

} // namespace vitaplex
