/**
 * VitaPlex - MusicController (implementation)
 * See include/app/music_controller.hpp.
 */

#include "app/music_controller.hpp"

#include "utils/now_playing.hpp"
#include "player/mpv_player.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "utils/async.hpp"

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
    MpvPlayer& p = MpvPlayer::getInstance();
    if (p.isInitialized() && (p.isPlaying() || p.isPaused())) {
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
    m_pollTimer.start(1000);
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

bool MusicController::loadCurrentHeadless() {
    const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
    if (!track) return false;

    std::string url;
    DownloadItem dl;
    PlexClient& client = PlexClient::getInstance();
    const bool prefetched = !m_prefetchUrl.empty() &&
                            m_prefetchKey == track->ratingKey &&
                            m_prefetchVersion == MusicQueue::getInstance().getVersion();
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
    if (!player.loadUrl(url, track->title)) {
        brls::Logger::error("MusicController: loadUrl failed for {}", url);
        return false;
    }
    m_endHandled = false;
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
            // Recorded either way: on failure the empty URL is what keeps this
            // from being retried on every tick.
            m_prefetchKey     = key;
            m_prefetchUrl     = ok ? url : std::string();
            m_prefetchSession = ok ? session : std::string();
            m_prefetchVersion = version;
        });
    });
}

void MusicController::publishNowPlaying(int playingOverride) {
    MusicQueue& q = MusicQueue::getInstance();
    const QueueItem* t = q.getCurrentTrack();
    if (!t) { stopSession(); return; }

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
    info.positionMs = (long long)(p.getPosition() * 1000.0);
    // MpvPlayer's state lags the play()/pause() command; trust the caller's intent
    // when it knows it (playingOverride), else fall back to the queried state.
    info.playing = (playingOverride >= 0) ? (playingOverride != 0) : p.isPlaying();
    info.hasNext = q.hasNext();
    info.hasPrev = q.hasPrevious();
    info.repeat = toBridgeRepeat(q.getRepeatMode());
    info.shuffle = q.isShuffleEnabled();
    info.userRating = t->userRating;
    info.showRepeat = true;    // music exposes repeat + shuffle (video doesn't)
    info.showShuffle = true;
    // Before update(), so the playback state it builds already carries the
    // skip-to-item action and the active row id.
    publishQueue();
    nowplaying::update(info);

    m_lastPublishedPlaying = info.playing;
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
    } else if (!m_lastPublishedPlaying && p.isPlaying()) {
        publishNowPlaying(1);        // mpv resumed / finally started
    }
}

void MusicController::togglePlayPause() {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    bool wasPaused = p.isPaused();   // settled state read before the toggle
    p.togglePause();
    publishNowPlaying(wasPaused ? 1 : 0);
}

void MusicController::playPause(bool play) {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    if (play) p.play(); else p.pause();
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
        // Pause rather than stop: the queue and position survive, so picking it
        // back up in the morning is one press.
        MpvPlayer& p = MpvPlayer::getInstance();
        if (p.isInitialized() && p.isPlaying()) playPause(false);
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
    p.seekTo((double)ms / 1000.0);
    publishNowPlaying();
}

void MusicController::seekRelativeMs(long long deltaMs) {
    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) return;
    long long target = (long long)(p.getPosition() * 1000.0) + deltaMs;
    if (target < 0) target = 0;
    p.seekTo((double)target / 1000.0);
    publishNowPlaying();
}

void MusicController::stopPlayback() {
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

} // namespace vitaplex
