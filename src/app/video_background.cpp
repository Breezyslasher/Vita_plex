/**
 * VitaPlex - Video in the background: app/video_background.hpp.
 */

#include "app/video_background.hpp"

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/downloads_manager.hpp"
#include "app/plex_client.hpp"
#include "app/video_queue.hpp"
#include "player/mpv_player.hpp"
#include "utils/async.hpp"

namespace vitaplex {

namespace {
constexpr int kTickMs = 250;
// The server told where the video is every this many ticks (10 s), as the
// player tells it.
constexpr int kReportTicks = 40;
}  // namespace

VideoBackground& VideoBackground::getInstance() {
    static VideoBackground instance;
    return instance;
}

int VideoBackground::positionMs() const {
    return m_session.baseOffsetMs + (int)(MpvPlayer::getInstance().getPosition() * 1000);
}

void VideoBackground::begin(const Session& session) {
    m_session = session;
    m_active = true;
    m_ticks = 0;
    // No picture to draw: the video is not decoded while nobody sees it.
    MpvPlayer::getInstance().setVideoEnabled(false);
    if (!m_timerSet) {
        m_timerSet = true;
        m_timer.setCallback([this]() { tick(); });
    }
    m_timer.start(kTickMs);
    brls::Logger::info("VideoBackground: {} ({}) plays on at {}ms with its player left",
                       m_session.ratingKey, m_session.title, positionMs());
}

void VideoBackground::tick() {
    if (!m_active) return;
    MpvPlayer& player = MpvPlayer::getInstance();
    player.update();
    if (player.hasEnded()) {
        finish();
        return;
    }
    if (player.hasError() || !player.isInitialized()) {
        brls::Logger::warning("VideoBackground: playback failed; stopped");
        stop();
        return;
    }
    if (++m_ticks % kReportTicks == 0) report(player.isPaused() ? "paused" : "playing");
}

// Where the video is, to the server; off the UI thread, as the player's.
void VideoBackground::report(const char* state) {
    if (m_session.isLocalFile || m_session.ratingKey.empty()) return;
    const std::string key = m_session.ratingKey;
    const std::string st = state;
    const int timeMs = positionMs();
    const int durationMs = m_session.durationMs;
    asyncRun([key, st, timeMs, durationMs]() {
        PlexClient::getInstance().reportTimeline(key, "/library/metadata/" + key, st, timeMs,
                                                 durationMs);
    });
}

void VideoBackground::stop() {
    if (!m_active) return;
    m_active = false;
    m_timer.stop();
    MpvPlayer& player = MpvPlayer::getInstance();
    const bool playing = player.isInitialized() && (player.isPlaying() || player.isPaused());
    const int timeMs = playing ? positionMs() : 0;
    brls::Logger::info("VideoBackground: {} stopped at {}ms", m_session.ratingKey, timeMs);
    if (timeMs > 0) {
        if (m_session.isLocalFile) {
            DownloadsManager::getInstance().updateProgress(m_session.ratingKey, timeMs);
            DownloadsManager::getInstance().saveState();
        } else if (!m_session.ratingKey.empty()) {
            const std::string key = m_session.ratingKey;
            const int durationMs = m_session.durationMs;
            asyncRun([key, timeMs, durationMs]() {
                PlexClient::getInstance().updatePlayProgress(key, timeMs);
                PlexClient::getInstance().reportTimeline(key, "/library/metadata/" + key,
                                                         "stopped", timeMs, durationMs);
            });
        }
    }
    if (player.isInitialized()) player.stop();
    player.setVideoEnabled(true);
}

// The video ended with its player left: watched, and on to the video
// queue's next, in the player.
void VideoBackground::finish() {
    m_active = false;
    m_timer.stop();
    const Session done = m_session;
    brls::Logger::info("VideoBackground: {} ended", done.ratingKey);
    MpvPlayer& player = MpvPlayer::getInstance();
    if (player.isInitialized()) player.stop();
    player.setVideoEnabled(true);
    if (!done.ratingKey.empty() && !done.isLocalFile) {
        const std::string key = done.ratingKey;
        const int durationMs = done.durationMs;
        asyncRun([key, durationMs]() {
            PlexClient::getInstance().markAsWatched(key);
            PlexClient::getInstance().reportTimeline(key, "/library/metadata/" + key, "stopped",
                                                     durationMs, durationMs);
        });
    }
    VideoQueueItem next;
    if (VideoQueue::getInstance().popNext(next)) {
        brls::Application::notify("Now playing: " + next.title);
        Application::getInstance().pushPlayerActivity(next.ratingKey);
    } else {
        brls::Application::notify("Finished: " + done.title);
    }
}

bool VideoBackground::reopen() {
    if (!m_active) return false;
    const Session session = m_session;
    MpvPlayer& player = MpvPlayer::getInstance();
    const bool playing = player.isInitialized() && (player.isPlaying() || player.isPaused());
    const int timeMs = playing ? positionMs() : 0;
    stop();
    // Where it was, whatever the resume setting and before the server has
    // the place stop() sends it.
    PlayerActivity::resumeNextAt(session.ratingKey, timeMs);
    brls::Application::pushActivity(new PlayerActivity(session.ratingKey, session.isLocalFile));
    return true;
}

}  // namespace vitaplex
