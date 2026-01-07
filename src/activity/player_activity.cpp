/**
 * VitaPlex - Player Activity implementation
 */

#include "activity/player_activity.hpp"
#include "app/plex_client.hpp"
#include "player/mpv_player.hpp"

namespace vitaplex {

PlayerActivity::PlayerActivity(const std::string& mediaKey)
    : m_mediaKey(mediaKey) {
    brls::Logger::debug("PlayerActivity created for media: {}", mediaKey);
}

brls::View* PlayerActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/player.xml");
}

void PlayerActivity::onContentAvailable() {
    brls::Logger::debug("PlayerActivity content available");

    // Load media details
    loadMedia();

    // Set up controls
    if (progressSlider) {
        progressSlider->setProgress(0.0f);
        progressSlider->getProgressEvent()->subscribe([this](float progress) {
            // Seek to position
            MpvPlayer& player = MpvPlayer::getInstance();
            double duration = player.getDuration();
            player.seekTo(duration * progress);
        });
    }

    // Register controller actions
    this->registerAction("Play/Pause", brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        togglePlayPause();
        return true;
    });

    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    this->registerAction("Rewind", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        seek(-10);
        return true;
    });

    this->registerAction("Forward", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        seek(10);
        return true;
    });

    // Start update timer
    m_updateTimer.setCallback([this]() {
        updateProgress();
    });
    m_updateTimer.start(1000); // Update every second
}

void PlayerActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);

    // Stop update timer
    m_updateTimer.stop();

    // Stop playback and save progress
    MpvPlayer& player = MpvPlayer::getInstance();

    if (player.isPlaying() || player.isPaused()) {
        double position = player.getPosition();
        int timeMs = (int)(position * 1000);

        // Save progress to Plex
        PlexClient::getInstance().updatePlayProgress(m_mediaKey, timeMs);

        player.stop();
    }

    m_isPlaying = false;
}

void PlayerActivity::loadMedia() {
    PlexClient& client = PlexClient::getInstance();
    MediaItem item;

    // Check if this is a direct file path (local playback test)
    bool isLocalFile = m_mediaKey.find("ux0:") == 0 || m_mediaKey.find("/") == 0;

    if (isLocalFile) {
        brls::Logger::info("PlayerActivity: Playing direct file: {}", m_mediaKey);

        // Set up title
        if (titleLabel) {
            size_t lastSlash = m_mediaKey.find_last_of("/\\");
            std::string filename = (lastSlash != std::string::npos) ?
                m_mediaKey.substr(lastSlash + 1) : m_mediaKey;
            titleLabel->setText(filename);
        }

        MpvPlayer& player = MpvPlayer::getInstance();

        // Determine if this is audio or video based on extension
        bool isAudio = (m_mediaKey.find(".mp3") != std::string::npos ||
                       m_mediaKey.find(".flac") != std::string::npos ||
                       m_mediaKey.find(".aac") != std::string::npos ||
                       m_mediaKey.find(".ogg") != std::string::npos ||
                       m_mediaKey.find(".wav") != std::string::npos);

        // For video files, we need to handle UI differently
        if (!isAudio) {
            brls::Logger::info("PlayerActivity: Video file detected, using video mode");
            player.setVideoEnabled(true);

            // Hide the borealis UI during video playback to avoid GPU conflicts
            // MPV's vita VO will render directly to the screen
            if (controlsBox) {
                controlsBox->setVisibility(brls::Visibility::GONE);
            }
        } else {
            brls::Logger::info("PlayerActivity: Audio file detected, using audio-only mode");
            player.setVideoEnabled(false);
        }

        if (!player.isInitialized()) {
            if (!player.init()) {
                brls::Logger::error("Failed to initialize MPV player");
                return;
            }
        }

        // Load the file directly
        if (player.loadUrl(m_mediaKey, m_mediaKey)) {
            player.play();
            m_isPlaying = true;
        } else {
            brls::Logger::error("Failed to load file: {}", m_mediaKey);
        }
        return;
    }

    // Normal Plex media playback
    if (client.fetchMediaDetails(m_mediaKey, item)) {
        if (titleLabel) {
            std::string title = item.title;
            if (item.mediaType == MediaType::EPISODE) {
                title = item.grandparentTitle + " - " + item.title;
            }
            titleLabel->setText(title);
        }

        // Store viewOffset for deferred seeking after file loads
        m_pendingSeekOffset = item.viewOffset;

        // Determine if this is audio content
        bool isAudio = (item.mediaType == MediaType::MUSIC_TRACK ||
                       item.type == "track");

        MpvPlayer& player = MpvPlayer::getInstance();

        // Configure player mode based on content type
        if (!isAudio) {
            brls::Logger::info("PlayerActivity: Video content, enabling video mode");
            player.setVideoEnabled(true);

            // Hide UI for video - MPV renders directly to screen
            if (controlsBox) {
                controlsBox->setVisibility(brls::Visibility::GONE);
            }
        } else {
            brls::Logger::info("PlayerActivity: Audio content, using audio-only mode");
            player.setVideoEnabled(false);
        }

        // Get playback URL
        std::string url;
        if (client.getPlaybackUrl(m_mediaKey, url)) {
            if (!player.isInitialized()) {
                if (!player.init()) {
                    brls::Logger::error("Failed to initialize MPV player");
                    return;
                }
            }

            // Load URL - seeking will happen in updateProgress() after file loads
            if (player.loadUrl(url, item.title)) {
                player.play();
                m_isPlaying = true;
            } else {
                brls::Logger::error("Failed to load media URL");
            }
        }
    }
}

void PlayerActivity::updateProgress() {
    MpvPlayer& player = MpvPlayer::getInstance();

    if (!player.isInitialized()) return;

    // Process mpv events to update state
    player.update();

    // Handle deferred seeking after file loads
    if (m_pendingSeekOffset > 0 && player.isPlaying()) {
        double seekSeconds = m_pendingSeekOffset / 1000.0;
        brls::Logger::debug("Performing deferred seek to {} seconds", seekSeconds);
        player.seekTo(seekSeconds);
        m_pendingSeekOffset = 0;  // Clear pending seek
    }

    // Don't update UI if player is in error state or not playing
    if (player.hasError()) {
        brls::Logger::error("Player error: {}", player.getErrorMessage());
        return;
    }

    double position = player.getPosition();
    double duration = player.getDuration();

    if (duration > 0) {
        if (progressSlider) {
            progressSlider->setProgress((float)(position / duration));
        }

        if (timeLabel) {
            int posMin = (int)position / 60;
            int posSec = (int)position % 60;
            int durMin = (int)duration / 60;
            int durSec = (int)duration % 60;

            char timeStr[32];
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d",
                     posMin, posSec, durMin, durSec);
            timeLabel->setText(timeStr);
        }
    }

    // Check if playback ended
    if (player.hasEnded()) {
        PlexClient::getInstance().markAsWatched(m_mediaKey);
        brls::Application::popActivity();
    }
}

void PlayerActivity::togglePlayPause() {
    MpvPlayer& player = MpvPlayer::getInstance();

    if (player.isPlaying()) {
        player.pause();
        m_isPlaying = false;
    } else if (player.isPaused()) {
        player.play();
        m_isPlaying = true;
    }
}

void PlayerActivity::seek(int seconds) {
    MpvPlayer& player = MpvPlayer::getInstance();
    player.seekRelative(seconds);
}

} // namespace vitaplex
