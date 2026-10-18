/**
 * VitaPlex - MusicController (implementation)
 * See include/app/music_controller.hpp.
 */

#include "app/music_controller.hpp"

#include <algorithm>

#include "utils/now_playing.hpp"
#include "player/mpv_player.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"  // redactTokensInUrl

namespace vitaplex {

namespace {
// Map between the queue's repeat enum and the OS-bridge enum (kept separate so
// the bridge doesn't depend on MusicQueue).
nowplaying::RepeatMode toBridgeRepeat(RepeatMode m) {
    switch (m) {
        case RepeatMode::ALL: return nowplaying::RepeatMode::All;
        case RepeatMode::ONE: return nowplaying::RepeatMode::One;
        case RepeatMode::OFF: default: return nowplaying::RepeatMode::Off;
    }
}
// FNV-1a over the queue window we're about to publish. publishNowPlaying() runs
// once a second; without a fingerprint the whole list would cross JNI (and a
// Binder transaction) every tick for a queue that hasn't changed.
void hashMix(uint64_t& h, const std::string& s) {
    for (char c : s) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
    h ^= 0xffu; h *= 1099511628211ULL;   // separator, so "ab"+"c" != "a"+"bc"
}
void hashMix(uint64_t& h, long long v) {
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)((v >> (i * 8)) & 0xff);
        h *= 1099511628211ULL;
    }
}

RepeatMode fromBridgeRepeat(nowplaying::RepeatMode m) {
    switch (m) {
        case nowplaying::RepeatMode::All: return RepeatMode::ALL;
        case nowplaying::RepeatMode::One: return RepeatMode::ONE;
        case nowplaying::RepeatMode::Off: default: return RepeatMode::OFF;
    }
}
} // namespace

MusicController& MusicController::getInstance() {
    static MusicController instance;
    return instance;
}

void MusicController::install() {
    if (m_installed) return;
    m_installed = true;

    // Own the queue's end-of-track callback for the whole app lifetime. When a
    // foreground player is attached we forward to it; otherwise we advance + load
    // headlessly. (Replaces PlayerActivity wiring the callback to a raw pointer
    // that dangled once it was destroyed under background music.)
    MusicQueue::getInstance().setTrackEndedCallback([](const QueueItem* nextTrack) {
        MusicController::getInstance().handleTrackEnded(nextTrack);
    });

    // Receive the OS media buttons (lock-screen / notification / media keys).
    registerOsHandler();

    // Headless end-of-track watcher (MpvPlayer has no end callback). Runs only
    // while we're driving headlessly.
    //
    // update() is not optional here: ENDED is only ever set from inside mpv's
    // event loop, and nothing else pumps it on this path — without the pump
    // hasEnded() would stay false and headless auto-advance would never fire.
    // Four ticks a second rather than one, so a finished track is noticed
    // promptly instead of up to a second late; syncSessionState() keeps its
    // original per-second cadence.
    m_pollTimer.setCallback([this]() {
        MpvPlayer& p = MpvPlayer::getInstance();
        if (p.isInitialized()) p.update();
        if (p.hasEnded()) {
            if (!m_endHandled) {
                m_endHandled = true;
                // Advances the queue (respecting repeat/shuffle) and fires the
                // end-of-track callback -> handleTrackEnded() -> load next.
                MusicQueue::getInstance().onTrackEnded();
            }
        } else if (p.isPlaying() || p.isPaused()) {
            m_endHandled = false;
        } else if (p.hasError()) {
            // A stream that broke or a load that failed. Not an end of track:
            // the queue must not move past a song that never finished.
            onPlaybackFailed();
        }
        if (++m_pollTick >= 4) {
            m_pollTick = 0;
            syncSessionState();   // keep the notification honest about play/pause
            // Resolve the next track's URL while this one is still playing. Held
            // off for the first few seconds so the extra round-trips don't
            // compete with this track's own buffering; a no-op on every tick
            // after the first success.
            if (p.isPlaying() && p.getPosition() > 5.0) prefetchNextTrack();
        }
    });

    m_recoverTimer.setCallback([this]() { recoveryTick(); });
}

void MusicController::registerOsHandler() {
    // The transport handler is global (one per process). A video PlayerActivity
    // installs its own while it's on screen; this reclaims it for music whenever
    // music (re)attaches, so the two never fight over it.
    nowplaying::setHandler(
        [](nowplaying::Transport t) {
            auto& self = MusicController::getInstance();
            switch (t) {
                case nowplaying::Transport::Play:          self.playPause(true);       break;
                case nowplaying::Transport::Pause:         self.playPause(false);      break;
                case nowplaying::Transport::Toggle:        self.togglePlayPause();     break;
                case nowplaying::Transport::Next:          self.next();                break;
                case nowplaying::Transport::Previous:      self.previous();            break;
                case nowplaying::Transport::Stop:          self.stopPlayback();        break;
                case nowplaying::Transport::FastForward:   self.seekRelativeMs(10000); break;
                case nowplaying::Transport::Rewind:        self.seekRelativeMs(-10000);break;
                case nowplaying::Transport::CycleRepeat:   self.cycleRepeatMode();     break;
                case nowplaying::Transport::ToggleShuffle: self.toggleShuffleMode();   break;
            }
        },
        [](long long ms) { MusicController::getInstance().seekToMs(ms); },
        [](nowplaying::RepeatMode m) { MusicController::getInstance().setRepeatMode(fromBridgeRepeat(m)); },
        [](bool on) { MusicController::getInstance().setShuffleMode(on); },
        [](long long id) { MusicController::getInstance().playQueueIndex((int)id); },
        [](bool liked) { MusicController::getInstance().setCurrentTrackLiked(liked); });
}

void MusicController::attachForeground(ForegroundHooks hooks) {
    install();
    registerOsHandler();   // reclaim from any video session that had it
    m_fg = std::move(hooks);
    m_hasForeground = true;
    // The activity does its own prefetching from here on; drop ours so a stale
    // entry can't be adopted later against a queue that has moved on.
    m_prefetchKey.clear();
    m_prefetchUrl.clear();
    m_prefetchSession.clear();
    stopPolling();  // the live player polls + drives the queue itself
}

void MusicController::detachForeground() {
    m_hasForeground = false;
    m_fg = ForegroundHooks{};
    // Music still going (background music)? Take over headless driving.
    //
    // isLoading() belongs in this test, and leaving it out was a real bug. A
    // track that has not finished opening yet is the case that needs the pump
    // most: ENDED is only ever set from inside mpv's event loop, and once the
    // player view is gone this poll is the only thing that runs it. Landing in
    // the else branch there stopped the one remaining pump, so the state
    // machine sat at LOADING while mpv played the whole track on its own
    // thread, and the queue never advanced.
    //
    // It needed the handoff to happen during LOADING, which is why it only
    // ever hit the first track of a session — the deferred mpv init adds about
    // a second before PLAYING, and leaving the player inside that window was
    // enough. Observed as exactly that: handoff at 00:03:15.052 with mpv still
    // LOADING, then not one line logged for the next four and a half minutes
    // while a 223-second track played out, and the queue advancing only when
    // the player was reopened and its own timers took over again.
    //
    // A recovery in progress counts too: nothing is playing while it waits for
    // the server, but the track it reloads needs this poll to be watched.
    MpvPlayer& p = MpvPlayer::getInstance();
    if (p.isInitialized() && (p.isPlaying() || p.isPaused() || p.isLoading() ||
                              m_recover.active)) {
        m_endHandled = p.hasEnded();
        startPolling();
        publishNowPlaying();
    } else {
        stopPolling();
    }
}

void MusicController::startPolling() {
    if (m_polling) return;
    m_polling = true;
    // 250ms, not 1000: the callback pumps mpv every tick so a finished track is
    // noticed promptly, and gates syncSessionState() to every fourth tick to
    // keep that at its original once-a-second cadence. Left at 1000 the gate
    // would have stretched the session sync to once every four seconds.
    m_pollTimer.start(250);
}

void MusicController::stopPolling() {
    if (!m_polling) return;
    m_polling = false;
    m_pollTimer.stop();
}

void MusicController::handleTrackEnded(const QueueItem* nextTrack) {
    if (m_hasForeground && m_fg.onTrackEnded) {
        m_fg.onTrackEnded(nextTrack);
        return;
    }
    if (nextTrack) {
        loadCurrentHeadless();
        publishNowPlaying(1);   // auto-advanced into a playing track
    } else {
        stopSession();  // queue finished
    }
}

// A prefetched URL carries the transcode session that /decision opened for it,
// and Plex reaps a session nobody has started streaming. A device log caught the
// consequence: a decision taken at 02:04:36 was still being handed to mpv at
// 02:07:07 — two and a half minutes later — and the server answered 400, so the
// track after a long one simply would not play. Past this age, resolve again.
static constexpr int64_t kPrefetchMaxAgeMs = 60000;

static int64_t nowMs() { return brls::getCPUTimeUsec() / 1000; }

bool MusicController::loadCurrentHeadless() {
    const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
    if (!track) return false;
    cancelRecovery(false);   // this load decides what plays now

    std::string url;
    DownloadItem dl;
    PlexClient& client = PlexClient::getInstance();
    const int64_t prefetchAgeMs = m_prefetchAtMs > 0 ? nowMs() - m_prefetchAtMs : 0;
    const bool prefetchStale = m_prefetchAtMs > 0 && prefetchAgeMs > kPrefetchMaxAgeMs;
    const bool prefetched = !m_prefetchUrl.empty() &&
                            m_prefetchKey == track->ratingKey &&
                            m_prefetchVersion == MusicQueue::getInstance().getVersion() &&
                            !prefetchStale;
    if (prefetchStale && !m_prefetchUrl.empty()) {
        brls::Logger::info("MusicController: prefetched URL for {} is {}s old — resolving again",
                           m_prefetchKey, prefetchAgeMs / 1000);
        m_prefetchUrl.clear();
        m_prefetchSession.clear();
    }
    if (DownloadsManager::getInstance().getDownloadCopy(track->ratingKey, dl) &&
        dl.state == DownloadState::COMPLETED && !dl.localPath.empty()) {
        url = dl.localPath;
    } else if (prefetched) {
        // Resolved while the previous track was still playing — the whole point
        // is to keep those two blocking round-trips out of the gap between songs.
        url = m_prefetchUrl;
        client.adoptTranscodeSession(m_prefetchSession);
        brls::Logger::info("MusicController: using prefetched stream URL for {}", track->ratingKey);
    } else if (!client.getTranscodeUrl(track->ratingKey, url, 0)) {
        brls::Logger::error("MusicController: failed to resolve URL for {}", track->ratingKey);
        m_prefetchKey.clear();
        m_prefetchUrl.clear();
        m_prefetchSession.clear();
        onTrackLoadFailed();
        return false;
    }
    // Spent, whichever branch ran.
    m_prefetchKey.clear();
    m_prefetchUrl.clear();
    m_prefetchSession.clear();

    MpvPlayer& player = MpvPlayer::getInstance();
    if (!player.isInitialized()) {
        // Headless advance only happens while a track is already playing, so mpv
        // is already up; we never spin it up from nothing here.
        brls::Logger::warning("MusicController: mpv not initialized; skipping headless load");
        return false;
    }
    player.setAudioOnly(true);
    if (player.hasEnded()) player.stop();  // clear ENDED so the new load isn't re-ended
    if (!player.loadUrl(url, track->title, (int64_t)track->duration * 1000)) {
        brls::Logger::error("MusicController: loadUrl failed for {}", redactTokensInUrl(url));
        return false;
    }
    m_endHandled = false;
    m_streamStartOffsetMs = 0;   // fresh track, stream starts at its beginning
    return true;
}

void MusicController::prefetchNextTrack() {
    // The foreground player runs its own prefetch; two would just duplicate the
    // request and fight over which session ends up adopted.
    if (m_hasForeground || m_prefetchInFlight) return;

    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* next = queue.peekNextTrack();
    if (!next || next->ratingKey.empty()) return;

    const uint32_t version = queue.getVersion();
    if (m_prefetchKey == next->ratingKey && m_prefetchVersion == version) return;

    DownloadItem dl;
    if (DownloadsManager::getInstance().getDownloadCopy(next->ratingKey, dl) &&
        dl.state == DownloadState::COMPLETED && !dl.localPath.empty()) {
        return;   // plays from disk; nothing to resolve
    }

    const std::string key = next->ratingKey;
    m_prefetchInFlight = true;

    // Capturing `this` is safe here in a way it would not be in an activity:
    // MusicController is a singleton that outlives the whole session.
    asyncRun([this, key, version]() {
        std::string url, session;
        const bool ok = PlexClient::getInstance().getTranscodeUrlSpeculative(key, url, session);
        brls::sync([this, key, version, url, session, ok]() {
            m_prefetchInFlight = false;
            // Recorded either way: on failure the empty URL is what keeps this from being retried on every tick.
            m_prefetchKey     = key;
            m_prefetchUrl     = ok ? url : std::string();
            m_prefetchSession = ok ? session : std::string();
            m_prefetchVersion = version;
            m_prefetchAtMs    = ok ? nowMs() : 0;
        });
    });
}

void MusicController::publishNowPlaying(int playingOverride, long long positionOverrideMs) {
    MusicQueue& q = MusicQueue::getInstance();
    const QueueItem* t = q.getCurrentTrack();
    if (!t) { stopSession(); return; }

    // A different track than we last published means any seek still in flight
    // belonged to the old one — drop it, or it would suppress the position
    // re-anchor here and then report a seek that "never took".
    if (m_lastPublishedRatingKey != t->ratingKey) {
        m_lastPublishedRatingKey = t->ratingKey;
        m_pendingSeekMs = -1;
    }

    MpvPlayer& p = MpvPlayer::getInstance();
    nowplaying::Info info;
    info.mediaId = "track/" + t->ratingKey;   // what media resumption replays
    info.title = t->title;
    info.artist = t->artist;
    info.album = t->album;

    DownloadItem dl;
    if (DownloadsManager::getInstance().getDownloadCopy(t->ratingKey, dl) &&
        dl.state == DownloadState::COMPLETED && !dl.thumbPath.empty()) {
        info.artUrl = dl.thumbPath;                    // local cover for offline tracks
    } else if (!t->thumb.empty()) {
        info.artUrl = PlexClient::getInstance().getThumbnailUrl(t->thumb, 512, 512);
    }

    info.durationMs = (long long)t->duration * 1000;   // QueueItem.duration is seconds
    // Same reasoning as playingOverride below: seekTo() is an async mpv command,
    // so right after one getPosition() still reads where we were, not where the
    // user asked to go.
    info.positionMs = (positionOverrideMs >= 0)
        ? positionOverrideMs
        : m_streamStartOffsetMs + (long long)(p.getPosition() * 1000.0);
    // MpvPlayer's state lags the play()/pause() command; trust the caller's intent
    // when it knows it (playingOverride), else fall back to the queried state.
    // A recovery reads as playing: the music is meant to be on. On Android that
    // is also what keeps the CPU and Wi-Fi awake for its checks of the server.
    info.playing = (playingOverride >= 0) ? (playingOverride != 0)
                                          : (p.isPlaying() || m_recover.active);
    info.hasNext = q.hasNext();
    info.hasPrev = q.hasPrevious();
    info.repeat = toBridgeRepeat(q.getRepeatMode());
    info.shuffle = q.isShuffleEnabled();
    info.userRating = t->userRating;
    info.showRepeat = true;    // music exposes repeat + shuffle (video doesn't)
    info.showShuffle = true;
    // Before update(), so the playback state it builds already carries the skip-to-item action and the active row id.
    publishQueue();
    nowplaying::update(info);

    m_lastPublishedPlaying = info.playing;
    m_lastPublishedPositionMs = info.positionMs;
    m_lastPublishAt = std::chrono::steady_clock::now();
    m_sessionActive = true;
}

void MusicController::publishQueue() {
    MusicQueue& q = MusicQueue::getInstance();
    const std::vector<QueueItem>& tracks = q.getQueue();
    if (tracks.empty()) {
        if (m_lastQueueSig != 0) {
            nowplaying::setQueue({}, -1);
            m_lastQueueSig = 0;
        }
        return;
    }

    // Publish in play order, so a shuffled queue reads shuffled in the OS list.
    // Entries carry the ABSOLUTE queue index as their id, which is what comes
    // back on skip-to-item — the shuffle mapping stays our problem, not theirs.
    std::vector<int> order;
    int pos;
    const std::vector<int>& shuffled = q.getShuffleOrder();
    if (q.isShuffleEnabled() && shuffled.size() == tracks.size()) {
        order = shuffled;
        pos = q.getShufflePosition();
    } else {
        order.resize(tracks.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        pos = q.getCurrentIndex();
    }
    if (pos < 0 || pos >= (int)order.size()) pos = 0;

    // The list crosses a Binder transaction, so a thousand-track queue can't go
    // over whole. Send a window around the current track: enough to scroll
    // through in Android Auto, small enough to fit.
    constexpr int kWindow = 200, kBefore = 25;
    int first = pos > kBefore ? pos - kBefore : 0;
    int last = first + kWindow;
    if (last > (int)order.size()) {
        last = (int)order.size();
        first = last > kWindow ? last - kWindow : 0;
    }

    // Fingerprint first, build second: this runs on the per-second timers, and
    // hashing a few hundred rating keys is far cheaper than composing the same
    // number of thumbnail URLs for a queue that hasn't moved.
    const int64_t activeId = order[pos];
    uint64_t sig = 14695981039346656037ULL;
    for (int i = first; i < last; i++) hashMix(sig, tracks[(size_t)order[i]].ratingKey);
    hashMix(sig, (long long)activeId);
    if (sig == 0) sig = 1;   // 0 means "nothing published"
    if (sig == m_lastQueueSig) return;
    m_lastQueueSig = sig;

    PlexClient& plex = PlexClient::getInstance();
    std::vector<nowplaying::QueueEntry> entries;
    entries.reserve((size_t)(last - first));
    for (int i = first; i < last; i++) {
        const QueueItem& t = tracks[(size_t)order[i]];
        nowplaying::QueueEntry e;
        e.id = order[i];
        e.mediaId = "track/" + t.ratingKey;   // matches the browse tree's ids
        e.title = t.title;
        e.artist = t.artist;
        if (!t.thumb.empty()) e.artUrl = plex.getThumbnailUrl(t.thumb, 256, 256);
        entries.push_back(std::move(e));
    }
    nowplaying::setQueue(entries, activeId);
}

void MusicController::stopSession() {
    stopPolling();
    nowplaying::clear();
    m_lastQueueSig = 0;   // clear() drops the session's queue with everything else
    m_sessionActive = false;
}

void MusicController::syncSessionState() {
    if (!m_sessionActive) return;
    // Catch queue edits (reorder, add, remove) that don't go through a publish:
    // cheap, since it fingerprints the window and returns when nothing moved.
    publishQueue();
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    // Only react to a settled play/pause that disagrees with the last publish;
    // LOADING/BUFFERING is neither isPlaying() nor isPaused(), so a buffer stall
    // can't wrongly flip the notification to paused.
    if (m_lastPublishedPlaying && p.isPaused()) {
        publishNowPlaying(0);        // mpv paused on its own (e.g. audio-focus loss)
        return;
    }
    if (!m_lastPublishedPlaying && p.isPlaying()) {
        publishNowPlaying(1);        // mpv resumed / finally started
        return;
    }

    if (!p.isPlaying() && !p.isPaused()) return;   // LOADING/BUFFERING: no answer yet
    const long long realMs = m_streamStartOffsetMs + (long long)(p.getPosition() * 1000.0);
    const auto now = std::chrono::steady_clock::now();

    // A seek we've announced but mpv hasn't reached yet. Seeking an HTTP stream
    // means re-opening and re-buffering it, which takes well over one tick, and
    // mpv reports the *old* position the whole time. Correcting to that is what
    // yanked the notification scrubber back to where the drag started — so hold
    // the announced position until mpv arrives, or until it's clear it won't.
    if (m_pendingSeekMs >= 0) {
        const long long miss = (realMs > m_pendingSeekMs) ? (realMs - m_pendingSeekMs)
                                                          : (m_pendingSeekMs - realMs);
        if (miss <= 2500) {
            m_pendingSeekMs = -1;      // arrived; resume normal drift checking
        } else {
            const long long waited = std::chrono::duration_cast<std::chrono::seconds>(
                                         now - m_pendingSeekAt).count();
            if (waited < 8) return;    // still on its way — leave the OS alone
            // Long enough that the seek is not coming. Say so plainly, then let
            // the re-anchor below tell the truth about where playback actually is.
            brls::Logger::warning(
                "MusicController: seek to {}ms did not take after {}s (still at {}ms, "
                "mpv seekable={}) — the stream may not support seeking",
                m_pendingSeekMs, waited, realMs, p.isSeekable());
            m_pendingSeekMs = -1;
        }
    }

    // Position. The OS runs its own scrubber forward from the last publish, so a
    // divergence never corrects itself: a buffering stall, a track that ran
    // shorter than its metadata said, a seek that never landed. Work out what the
    // OS must be showing by now and re-anchor if reality has moved away from it.
    // On a threshold rather than every tick — each publish rebuilds the
    // notification, and a second of ordinary rounding drift is not worth one.
    long long shownMs = m_lastPublishedPositionMs;
    if (m_lastPublishedPlaying) {
        // Rate is 1.0 while playing, so the OS has advanced its own clock.
        shownMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - m_lastPublishAt).count();
    }
    const long long drift = (realMs > shownMs) ? (realMs - shownMs) : (shownMs - realMs);
    if (drift > 1500) publishNowPlaying(-1, realMs);
}

// Nothing is loaded: a track that failed, or a recovery that was called off or
// gave up. Neither playing nor paused, so play/pause have nothing to act on.
static bool nothingLoaded(MpvPlayer& p) {
    return !p.isPlaying() && !p.isPaused() && !p.hasEnded() && !p.isLoading();
}

void MusicController::togglePlayPause() {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    // A recovery reads as playing, so the toggle is a pause: call it off.
    if (m_recover.active) {
        cancelRecovery();
        p.pause();
        publishNowPlaying(0);
        return;
    }
    // With nothing loaded, Play means try the track again. Before this it
    // flipped mpv's pause flag on an empty player and nothing happened.
    if (nothingLoaded(p) && retryNow()) return;
    bool wasPaused = p.isPaused();   // settled state read before the toggle
    p.togglePause();
    publishNowPlaying(wasPaused ? 1 : 0);
}

void MusicController::playPause(bool play) {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    if (play) {
        if ((m_recover.active || nothingLoaded(p)) && retryNow()) return;
        p.play();
    } else {
        cancelRecovery();
        p.pause();
    }
    publishNowPlaying(play ? 1 : 0);
}

void MusicController::next() {
    if (m_hasForeground && m_fg.onNext) { m_fg.onNext(); return; }
    if (MusicQueue::getInstance().playNext()) {
        loadCurrentHeadless();
        publishNowPlaying(1);   // a freshly loaded track is playing
    }
}

void MusicController::previous() {
    if (m_hasForeground && m_fg.onPrevious) { m_fg.onPrevious(); return; }
    if (MusicQueue::getInstance().playPrevious()) {
        loadCurrentHeadless();
        publishNowPlaying(1);
    }
}

void MusicController::playQueueIndex(int index) {
    MusicQueue& q = MusicQueue::getInstance();
    if (index < 0 || index >= q.getQueueSize()) return;
    // Picking the row that's already playing means "resume it", not "reload it".
    if (index == q.getCurrentIndex()) { playPause(true); return; }
    if (m_hasForeground && m_fg.onPlayIndex) { m_fg.onPlayIndex(index); return; }
    if (!q.playTrack(index)) return;
    loadCurrentHeadless();
    publishNowPlaying(1);
}

void MusicController::setCurrentTrackLiked(bool liked) {
    MusicQueue& q = MusicQueue::getInstance();
    const QueueItem* t = q.getCurrentTrack();
    if (!t || t->ratingKey.empty()) return;
    const std::string key = t->ratingKey;
    // Update our copy immediately: the OS heart flips on the next publish
    // rather than after a round trip that might not come back.
    q.setCurrentTrackRating(liked ? 10.0f : 0.0f);
    publishNowPlaying();
    // The PUT is network I/O; nothing on screen depends on the answer.
    asyncRun([key, liked]() {
        const bool ok = PlexClient::getInstance().rateItem(key, liked ? 10.0f : 0.0f);
        brls::Logger::info("MusicController: rate {} -> {} ({})",
                           key, liked ? "liked" : "cleared", ok ? "ok" : "failed");
    });
}

void MusicController::startSleepTimer(int minutes) {
    m_sleepTimer.stop();
    m_sleepMinutes = minutes > 0 ? minutes : 0;
    m_sleepSecondsLeft = m_sleepMinutes * 60;
    if (m_sleepMinutes <= 0) {
        brls::Logger::info("MusicController: sleep timer cancelled");
        return;
    }

    m_sleepTimer.setCallback([this]() {
        if (m_sleepSecondsLeft > 0) m_sleepSecondsLeft--;
        if (m_sleepSecondsLeft > 0) return;

        m_sleepTimer.stop();
        m_sleepMinutes = 0;
        // Pause rather than stop: the queue and position survive, so picking it back up in the morning is one press.
        // A track waiting on a lost connection counts as playing: left alone it would start again after the timer.
        MpvPlayer& p = MpvPlayer::getInstance();
        if (p.isInitialized() && (p.isPlaying() || m_recover.active)) playPause(false);
        brls::Application::notify("Sleep timer - playback paused");
    });
    m_sleepTimer.start(1000);
    brls::Logger::info("MusicController: sleep timer armed for {} min", m_sleepMinutes);
}

int MusicController::sleepTimerRemaining() const {
    if (m_sleepMinutes <= 0) return 0;
    return (m_sleepSecondsLeft + 59) / 60;   // round up, so 1s left still reads "1 min"
}

void MusicController::seekToMs(long long ms) {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    if (ms < 0) ms = 0;

    // Waiting for the server to come back: there is nothing loaded to seek in,
    // and a restart of our own would race the recovery's reload. Move where
    // the recovery will resume instead, and try at once.
    if (m_recover.active) {
        m_recover.positionMs = ms;
        if (m_recover.reloading) {   // the reload under way is for the old point
            m_recover.reloading = false;
            if (p.isLoading()) p.stop();
        }
        retryNow();
        publishNowPlaying(-1, ms);
        return;
    }
    const long long nowMs = m_streamStartOffsetMs + (long long)(p.getPosition() * 1000.0);
    brls::Logger::info("MusicController: seek to {}ms (from {}ms, mpv seekable={})",
                       ms, nowMs, p.isSeekable());

    // A downloaded file is a real file and seeks fine. A live Plex transcode is
    // not: it arrives without range support, so mpv can only move inside what it
    // has already buffered and refuses anything past that. Restarting the
    // transcode at the target is the only way to get there.
    if (!p.isSeekable()) {
        restartTranscodeAtMs(ms);
        m_pendingSeekMs = ms;
        m_pendingSeekAt = std::chrono::steady_clock::now();
        publishNowPlaying(-1, ms);
        return;
    }

    // Seekable: an ordinary mpv seek, but mpv's timeline starts at whatever
    // offset the stream was opened with, so take that off the absolute target.
    p.seekTo((double)(ms - m_streamStartOffsetMs) / 1000.0);
    // Publish where the user asked to go, not where getPosition() still says we
    // are — seekTo is async, so reading it back here returns the pre-seek value,
    // and publishing that snapped the notification scrubber straight back.
    m_pendingSeekMs = ms;
    m_pendingSeekAt = std::chrono::steady_clock::now();
    publishNowPlaying(-1, ms);
}

void MusicController::seekRelativeMs(long long deltaMs) {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    long long target = m_streamStartOffsetMs + (long long)(p.getPosition() * 1000.0) + deltaMs;
    if (target < 0) target = 0;
    seekToMs(target);   // shares the unseekable-stream handling
}

void MusicController::restartTranscodeAtMs(long long ms) {
    if (m_restartingTranscode) return;   // one in flight is enough

    const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
    if (!track || track->ratingKey.empty()) return;

    // Don't run off the end: Plex will happily start a transcode past the last
    // sample and hand back a stream that ends immediately.
    const long long durationMs = (long long)track->duration * 1000;
    if (durationMs > 5000 && ms > durationMs - 5000) ms = durationMs - 5000;
    if (ms < 0) ms = 0;

    const std::string key = track->ratingKey;
    const std::string title = track->title;
    m_restartingTranscode = true;
    brls::Logger::info("MusicController: stream is not seekable — restarting the "
                       "transcode at {}ms for {}", ms, key);

    // Two blocking round-trips (/library/metadata, then /decision), so not on
    // the UI thread. Capturing `this` is safe: the controller is a singleton.
    asyncRun([this, key, title, ms]() {
        std::string url;
        const bool ok = PlexClient::getInstance().getTranscodeUrl(key, url, (int)ms);
        brls::sync([this, key, title, url, ms, ok]() {
            m_restartingTranscode = false;
            if (!ok || url.empty()) {
                brls::Logger::error("MusicController: could not restart the transcode at {}ms", ms);
                m_pendingSeekMs = -1;   // let the position re-anchor to reality
                return;
            }
            // The queue may have moved on while the request was in flight.
            const QueueItem* cur = MusicQueue::getInstance().getCurrentTrack();
            if (!cur || cur->ratingKey != key) {
                brls::Logger::info("MusicController: track changed mid-restart; dropping the seek");
                m_pendingSeekMs = -1;
                return;
            }
            MpvPlayer& p = MpvPlayer::getInstance();
            if (!p.isInitialized()) return;
            // The new stream starts at ms, so mpv's clock restarts at zero and
            // everything reporting an absolute position adds this back.
            m_streamStartOffsetMs = ms;
            if (p.hasEnded()) p.stop();
            if (!p.loadUrl(url, title)) {
                brls::Logger::error("MusicController: loadUrl failed restarting at {}ms", ms);
                m_streamStartOffsetMs = 0;
                m_pendingSeekMs = -1;
                return;
            }
            publishNowPlaying(1, ms);
        });
    });
}

void MusicController::stopPlayback() {
    cancelRecovery();
    MpvPlayer& p = MpvPlayer::getInstance();
    if (p.isInitialized()) p.stop();
    stopSession();
}

void MusicController::setShuffleMode(bool on) {
    MusicQueue& q = MusicQueue::getInstance();
    if (m_hasForeground && m_fg.onSetShuffle) {
        m_fg.onSetShuffle(on);              // rich, server-aware path + on-screen icon
    } else if (on != q.isShuffleEnabled()) {
        q.setShuffle(on);                   // headless: client-side shuffle
    }
    publishNowPlaying();                     // reflect the new state back to the OS
}

void MusicController::toggleShuffleMode() {
    setShuffleMode(!MusicQueue::getInstance().isShuffleEnabled());
}

void MusicController::setRepeatMode(RepeatMode mode) {
    MusicQueue& q = MusicQueue::getInstance();
    if (m_hasForeground && m_fg.onSetRepeat) {
        m_fg.onSetRepeat(mode);             // set + on-screen icon refresh
    } else {
        q.setRepeatMode(mode);
    }
    publishNowPlaying();
}

void MusicController::cycleRepeatMode() {
    // Match the in-app order: OFF -> ALL -> ONE -> OFF.
    RepeatMode next;
    switch (MusicQueue::getInstance().getRepeatMode()) {
        case RepeatMode::OFF: next = RepeatMode::ALL; break;
        case RepeatMode::ALL: next = RepeatMode::ONE; break;
        case RepeatMode::ONE: default: next = RepeatMode::OFF; break;
    }
    setRepeatMode(next);
}

// ---- Lost-connection recovery (see the header) ----

// Seconds to wait before each check of the server. Quick at first, since a
// dropped stream is often a short interruption, then every fifteen seconds:
// often enough to resume soon after the connection is back, rare enough not to
// matter while it stays away.
static const int kRecoverDelaysSec[] = {2, 3, 5, 10, 15};
static constexpr int kRecoverDelayCount = sizeof(kRecoverDelaysSec) / sizeof(kRecoverDelaysSec[0]);
// Give up after this long without the server, and only after this many checks.
// The count matters because the clock can keep running while a Vita sleeps: it
// should get a few tries after waking, not give up the moment it wakes.
static constexpr auto kRecoverGiveUpAfter = std::chrono::minutes(15);
static constexpr int kRecoverMinChecks = 10;
// The server answering and the track still not playing is not a lost
// connection, so a few tries and no more.
static constexpr int kRecoverMaxReloadFailures = 3;
// A reload that has not started playing by then has failed.
static constexpr auto kRecoverLoadTimeout = std::chrono::seconds(45);

void MusicController::onPlaybackFailed() {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized() || !p.hasError()) return;
    const uint32_t serial = p.errorSerial();
    if (serial == m_handledErrorSerial) return;
    m_handledErrorSerial = serial;
    // A reload the recovery issued itself; its tick sees the failure and counts it.
    if (m_recover.active) return;

    long long pos = m_streamStartOffsetMs + (long long)(p.getPosition() * 1000.0);
    // Two seconds back, so it does not pick up in the middle of a word.
    pos = pos > 2000 ? pos - 2000 : 0;
    beginRecovery(pos, p.getErrorMessage());
}

void MusicController::onTrackLoadFailed() {
    if (m_recover.active) return;
    beginRecovery(0, "could not resolve its stream");
}

void MusicController::beginRecovery(long long positionMs, const std::string& why) {
    install();   // the recovery timer's callback is set there
    const QueueItem* t = MusicQueue::getInstance().getCurrentTrack();
    if (!t || t->ratingKey.empty()) return;

    // A downloaded track plays from disk. Waiting for the server will not fix it.
    DownloadItem dl;
    if (DownloadsManager::getInstance().getDownloadCopy(t->ratingKey, dl) &&
        dl.state == DownloadState::COMPLETED && !dl.localPath.empty()) {
        brls::Logger::warning("MusicController: {} failed playing from disk ({}); not retrying",
                              t->ratingKey, why);
        return;
    }

    // Not past the end: a restart there hands back a stream that ends at once.
    const long long durationMs = (long long)t->duration * 1000;
    if (durationMs > 5000 && positionMs > durationMs - 5000) positionMs = durationMs - 5000;
    if (positionMs < 0) positionMs = 0;

    m_recoverGen++;
    m_recover = Recovery{};
    m_recover.active = true;
    m_recover.ratingKey = t->ratingKey;
    m_recover.positionMs = positionMs;
    m_recover.startedAt = std::chrono::steady_clock::now();
    m_recover.nextAt = m_recover.startedAt + std::chrono::seconds(kRecoverDelaysSec[0]);
    brls::Logger::warning("MusicController: lost {} at {}ms ({}); will reload it when the "
                          "server answers", t->ratingKey, positionMs, why);

    m_recoverTimer.start(500);
    publishNowPlaying(1, positionMs);
    // In the background this poll is what pumps mpv and watches the reload.
    if (!m_hasForeground) startPolling();
}

bool MusicController::retryNow() {
    const QueueItem* t = MusicQueue::getInstance().getCurrentTrack();
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!t || t->ratingKey.empty() || !p.isInitialized()) return false;

    if (!m_recover.active) {
        // Resume where a recovery of this same track left off, if one was
        // called off or gave up; otherwise from wherever the player stopped.
        long long pos = m_streamStartOffsetMs + (long long)(p.getPosition() * 1000.0);
        if (m_recover.ratingKey == t->ratingKey) pos = m_recover.positionMs;
        beginRecovery(pos, "Play pressed");
        if (!m_recover.active) return false;
    }
    if (!m_recover.reloading) m_recover.nextAt = std::chrono::steady_clock::now();
    m_recover.reloadFailures = 0;   // a fresh press gets a fresh set of tries
    return true;
}

void MusicController::cancelRecovery(bool keepResumePoint) {
    if (m_recover.active) {
        brls::Logger::info("MusicController: recovery of {} called off", m_recover.ratingKey);
        // A reload already under way would start the music after this.
        MpvPlayer& p = MpvPlayer::getInstance();
        if (m_recover.reloading && p.isInitialized() && p.isLoading()) p.stop();
    }
    endRecovery();
    if (!keepResumePoint) {
        m_recover.ratingKey.clear();
        m_recover.positionMs = 0;
    }
}

void MusicController::endRecovery() {
    m_recover.active = false;
    m_recover.busy = false;
    m_recover.reloading = false;
    m_recoverGen++;   // drops any check or resolve still in flight
    m_recoverTimer.stop();
}

void MusicController::recoveryAttemptFailed(bool serverAnswered) {
    const auto now = std::chrono::steady_clock::now();
    if (serverAnswered) {
        if (++m_recover.reloadFailures >= kRecoverMaxReloadFailures) {
            brls::Logger::error("MusicController: {} would not play after {} reloads with the "
                                "server answering; giving up", m_recover.ratingKey,
                                m_recover.reloadFailures);
            endRecovery();
            brls::Application::notify("Couldn't play this track. Press Play to try again.");
            publishNowPlaying(0);
            return;
        }
    } else {
        // Gone again, so earlier failures are as likely the connection as the
        // track; only failures since the server last answered count.
        m_recover.reloadFailures = 0;
        if (!m_recover.toldUser) {
            m_recover.toldUser = true;
            brls::Application::notify("Lost connection to the server. Music will resume when it's back.");
        }
    }
    const int idx = std::min(m_recover.probes, kRecoverDelayCount - 1);
    m_recover.nextAt = now + std::chrono::seconds(kRecoverDelaysSec[idx]);
    brls::Logger::info("MusicController: {} (check {}); next try in {}s",
                       serverAnswered ? "server answered but the track did not load"
                                      : "server unreachable",
                       m_recover.probes, kRecoverDelaysSec[idx]);
}

void MusicController::recoveryTick() {
    if (!m_recover.active) {
        m_recoverTimer.stop();
        return;
    }

    // The queue moved on underneath: a track picked, the queue replaced. Whatever
    // loads for that decides what plays now.
    const QueueItem* t = MusicQueue::getInstance().getCurrentTrack();
    if (!t || t->ratingKey != m_recover.ratingKey) {
        brls::Logger::info("MusicController: the track changed; recovery of {} dropped",
                           m_recover.ratingKey);
        endRecovery();
        return;
    }

    MpvPlayer& p = MpvPlayer::getInstance();
    const auto now = std::chrono::steady_clock::now();

    if (m_recover.reloading) {
        if (p.isPlaying() || p.isPaused()) {
            const long long secs = std::chrono::duration_cast<std::chrono::seconds>(
                                       now - m_recover.startedAt).count();
            brls::Logger::info("MusicController: {} is playing again from {}ms, after {}s "
                               "and {} check(s) of the server", m_recover.ratingKey,
                               m_recover.positionMs, secs, m_recover.probes);
            const bool told = m_recover.toldUser;
            const long long resumedAt = m_recover.positionMs;
            endRecovery();
            m_recover.ratingKey.clear();   // done; nothing left to resume
            m_recover.positionMs = 0;
            if (told) brls::Application::notify("Connection is back. Playing again.");
            if (m_hasForeground && m_fg.onRecovered) m_fg.onRecovered();
            publishNowPlaying(-1, resumedAt);
            return;
        }
        // Still opening: a remote server can take a while to start a stream.
        if (p.isLoading() && now - m_recover.reloadAt < kRecoverLoadTimeout) return;
        brls::Logger::warning("MusicController: reload of {} did not play (state {}{}{})",
                              m_recover.ratingKey, (int)p.getState(),
                              p.hasError() ? ", " : "",
                              p.hasError() ? p.getErrorMessage() : std::string());
        m_handledErrorSerial = p.errorSerial();   // this failure is counted here
        m_recover.reloading = false;
        if (p.isLoading()) p.stop();
        recoveryAttemptFailed(true);
        return;
    }

    if (m_recover.busy || now < m_recover.nextAt) return;

    if (now - m_recover.startedAt > kRecoverGiveUpAfter &&
        m_recover.probes >= kRecoverMinChecks) {
        brls::Logger::error("MusicController: no server for {} min; giving up on {}",
                            std::chrono::duration_cast<std::chrono::minutes>(
                                now - m_recover.startedAt).count(),
                            m_recover.ratingKey);
        endRecovery();
        brls::Application::notify("Still can't reach the server. Press Play to try again.");
        publishNowPlaying(0);
        return;
    }

    m_recover.busy = true;
    m_recover.probes++;
    const uint32_t gen = m_recoverGen;
    // Capturing `this` is safe: the controller is a singleton.
    asyncRun([this, gen]() {
        const bool up = PlexClient::getInstance().isServerReachable(8);
        brls::sync([this, gen, up]() {
            if (gen != m_recoverGen || !m_recover.active) return;
            m_recover.busy = false;
            if (up) recoveryReload();
            else    recoveryAttemptFailed(false);
        });
    });
}

void MusicController::recoveryReload() {
    const QueueItem* t = MusicQueue::getInstance().getCurrentTrack();
    if (!t || t->ratingKey != m_recover.ratingKey) {
        endRecovery();
        return;
    }
    const std::string key = t->ratingKey;
    const std::string title = t->title;
    const long long durationMs = (long long)t->duration * 1000;
    const long long posMs = m_recover.positionMs;

    m_recover.busy = true;
    const uint32_t gen = m_recoverGen;
    // Two blocking round-trips (/library/metadata, then /decision): off the UI thread.
    asyncRun([this, gen, key, title, durationMs, posMs]() {
        std::string url;
        const bool ok = PlexClient::getInstance().getTranscodeUrl(key, url, (int)posMs);
        brls::sync([this, gen, key, title, durationMs, posMs, url, ok]() {
            if (gen != m_recoverGen || !m_recover.active) return;
            m_recover.busy = false;
            if (posMs != m_recover.positionMs) {
                // Seeked while this was resolving; resolve the new point instead.
                m_recover.nextAt = std::chrono::steady_clock::now();
                return;
            }
            if (!ok || url.empty()) {
                brls::Logger::warning("MusicController: {} did not resolve", key);
                recoveryAttemptFailed(true);
                return;
            }
            MpvPlayer& p = MpvPlayer::getInstance();
            if (!p.isInitialized()) {   // torn down meanwhile; nothing to reload into
                endRecovery();
                return;
            }
            // A transcode is restarted by the server at the offset, so mpv's
            // clock starts at zero there. A direct-play original carries no
            // offset; mpv opens it and seeks there itself.
            const bool transcode = url.find("/transcode/universal/start") != std::string::npos;
            m_streamStartOffsetMs = transcode ? posMs : 0;
            const int64_t expectedMs = durationMs > 0 ? durationMs - m_streamStartOffsetMs : 0;
            p.setAudioOnly(true);
            if (!p.loadUrl(url, title, expectedMs, transcode ? 0.0 : posMs / 1000.0)) {
                m_streamStartOffsetMs = 0;
                recoveryAttemptFailed(true);
                return;
            }
            brls::Logger::info("MusicController: reloading {} at {}ms ({})", key, posMs,
                               transcode ? "transcode restarted there" : "seeking the original");
            m_endHandled = false;
            m_recover.reloading = true;
            m_recover.reloadAt = std::chrono::steady_clock::now();
        });
    });
}

} // namespace vitaplex
