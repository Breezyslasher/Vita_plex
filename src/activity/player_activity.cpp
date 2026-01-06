/**
 * VitaPlex - Player Activity implementation
 */

#include "activity/player_activity.hpp"
#include "app/plex_client.hpp"
#include "player/mpv_player.hpp"
#include "utils/image_loader.hpp"

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

    // Mark as destroying to prevent timer callbacks
    m_destroying = true;

    // Stop update timer
    m_updateTimer.stop();

    // For photos, nothing to stop
    if (m_isPhoto) {
        return;
    }

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

    if (client.fetchMediaDetails(m_mediaKey, item)) {
        if (titleLabel) {
            std::string title = item.title;
            if (item.mediaType == MediaType::EPISODE) {
                title = item.grandparentTitle + " - " + item.title;
            }
            titleLabel->setText(title);
        }

        // Handle photos differently - display image instead of playing
        if (item.mediaType == MediaType::PHOTO) {
            brls::Logger::info("Displaying photo: {}", item.title);
            m_isPhoto = true;

            // Load the full-size photo
            if (!item.thumb.empty()) {
                std::string photoUrl = client.getThumbnailUrl(item.thumb, 960, 544);
                brls::Logger::debug("Photo URL: {}", photoUrl);

                // Load photo into the view (photoImage is defined in player.xml)
                if (photoImage) {
                    photoImage->setVisibility(brls::Visibility::VISIBLE);
                    ImageLoader::loadAsync(photoUrl, [](brls::Image* image) {
                        // Photo loaded
                    }, photoImage);
                }

                // Hide player controls for photos
                if (progressSlider) {
                    progressSlider->setVisibility(brls::Visibility::GONE);
                }
                if (timeLabel) {
                    timeLabel->setVisibility(brls::Visibility::GONE);
                }
            }
            return;
        }

        // Get playback URL for video/audio
        std::string url;
        if (client.getPlaybackUrl(m_mediaKey, url)) {
            MpvPlayer& player = MpvPlayer::getInstance();

            if (!player.isInitialized()) {
                if (!player.init()) {
                    brls::Logger::error("Failed to initialize MPV player");
                    return;
                }
            }

            // Load the URL
            if (!player.loadUrl(url, item.title)) {
                brls::Logger::error("Failed to load URL: {}", url);
                return;
            }

            // Resume from viewOffset if available (seekTo is async, safe to call)
            if (item.viewOffset > 0) {
                player.seekTo(item.viewOffset / 1000.0);
            }

            player.play();
            m_isPlaying = true;
        } else {
            brls::Logger::error("Failed to get playback URL for: {}", m_mediaKey);
        }
    }
}

void PlayerActivity::updateProgress() {
    // Don't update if destroying or showing photo
    if (m_destroying || m_isPhoto) return;

    MpvPlayer& player = MpvPlayer::getInstance();

    if (!player.isInitialized()) return;

    // Process MPV events (state changes, property updates, etc.)
    player.update();

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

    // Check if playback ended (only if we were actually playing)
    if (m_isPlaying && player.hasEnded()) {
        m_isPlaying = false;  // Prevent multiple triggers
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
