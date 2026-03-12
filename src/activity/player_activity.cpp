/**
 * VitaPlex - Player Activity implementation
 */

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_queue.hpp"
#include "player/mpv_player.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "view/video_view.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sys/stat.h>

namespace vitaplex {

// Base temp file path for streaming audio (MPV's HTTP handling crashes on Vita)
// Extension will be added dynamically based on the actual file type


PlayerActivity::PlayerActivity(const std::string& mediaKey)
    : m_mediaKey(mediaKey), m_isLocalFile(false) {
    brls::Logger::debug("PlayerActivity created for media: {}", mediaKey);
}

PlayerActivity::PlayerActivity(const std::string& mediaKey, bool isLocalFile)
    : m_mediaKey(mediaKey), m_isLocalFile(isLocalFile) {
    brls::Logger::debug("PlayerActivity created for {} media: {}",
                       isLocalFile ? "local" : "remote", mediaKey);
}

PlayerActivity* PlayerActivity::createForDirectFile(const std::string& filePath) {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isDirectFile = true;
    activity->m_directFilePath = filePath;
    brls::Logger::debug("PlayerActivity created for direct file: {}", filePath);
    return activity;
}

PlayerActivity* PlayerActivity::createForStream(const std::string& streamUrl, const std::string& title) {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isDirectFile = true;  // Use direct file path for stream URLs too
    activity->m_directFilePath = streamUrl;
    activity->m_streamTitle = title;
    brls::Logger::info("PlayerActivity created for stream: {} ({})", title, streamUrl);
    return activity;
}

PlayerActivity* PlayerActivity::createWithQueue(const std::vector<MediaItem>& tracks, int startIndex) {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isQueueMode = true;

    // Set up the queue
    MusicQueue& queue = MusicQueue::getInstance();
    queue.setQueue(tracks, startIndex);

    // Set up track ended callback
    queue.setTrackEndedCallback([activity](const QueueItem* nextTrack) {
        activity->onTrackEnded(nextTrack);
    });

    brls::Logger::info("PlayerActivity created with queue of {} tracks, starting at {}",
                      tracks.size(), startIndex);
    return activity;
}

PlayerActivity* PlayerActivity::createResumeQueue() {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isQueueMode = true;
    activity->m_isResuming = true;  // Don't restart playback

    // Resume existing queue - don't reset it
    MusicQueue& queue = MusicQueue::getInstance();

    // Set up track ended callback for the new activity
    queue.setTrackEndedCallback([activity](const QueueItem* nextTrack) {
        activity->onTrackEnded(nextTrack);
    });

    brls::Logger::info("PlayerActivity resumed existing queue at index {}", queue.getCurrentIndex());
    return activity;
}

brls::View* PlayerActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/player.xml");
}

void PlayerActivity::onContentAvailable() {
    brls::Logger::debug("PlayerActivity content available");

    // Cancel pending background thumbnail loads (HomeTab, MediaDetailView)
    // to free up network bandwidth for media streaming.
    // We don't setPaused(true) here yet because music queue mode needs to
    // load album art first. setPaused is called later in loadMedia/loadFromQueue
    // right before MPV starts streaming.
    ImageLoader::cancelAll();

    // Load media details
    if (m_isQueueMode) {
        loadFromQueue();
    } else {
        loadMedia();
    }

    // Set up controls
    if (progressSlider) {
        progressSlider->setProgress(0.0f);
        progressSlider->getProgressEvent()->subscribe([this](float progress) {
            // Skip if this is a programmatic update (not user interaction)
            if (m_updatingSlider) return;
            resetControlsIdleTimer();
            // Seek to position
            MpvPlayer& player = MpvPlayer::getInstance();
            double duration = 0.0;
            // For music queue mode, prefer Plex API duration (full track length)
            // over MPV duration which may only reflect buffered/demuxed portion
            if (m_isQueueMode) {
                const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
                if (track && track->duration > 0)
                    duration = (double)track->duration;
            }
            if (duration <= 0)
                duration = player.getDuration();
            player.seekTo(duration * progress);
        });
    }

    // Register tap gesture on container to toggle controls (like Suwayomi reader)
    if (playerContainer) {
        playerContainer->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    resetControlsIdleTimer();
                    toggleControls();
                }
            }));
    }

    // Add horizontal swipe gesture on album art area for prev/next track (music mode)
    if (albumArtContainer) {
        albumArtContainer->addGestureRecognizer(new brls::PanGestureRecognizer(
            [this](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
                if (!m_isQueueMode) return;
                if (status.state == brls::GestureState::END) {
                    float deltaX = status.position.x - status.startPosition.x;
                    float threshold = 60.0f; // Minimum swipe distance
                    if (deltaX > threshold) {
                        // Swipe right = previous track
                        playPrevious();
                    } else if (deltaX < -threshold) {
                        // Swipe left = next track
                        playNext();
                    }
                }
            }, brls::PanAxis::HORIZONTAL));
    }

    // Register controller actions
    this->registerAction("Play/Pause", brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        resetControlsIdleTimer();
        togglePlayPause();
        return true;
    });

    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        resetControlsIdleTimer();
        // If track overlay is showing, dismiss it instead of leaving player
        if (m_trackSelectMode != TrackSelectMode::NONE) {
            hideTrackOverlay();
            return true;
        }
        // If queue overlay is showing, dismiss it instead of leaving player
        if (m_queueOverlayVisible) {
            hideQueueOverlay();
            return true;
        }
        // In music mode with background music enabled, leave without stopping
        if (m_isQueueMode && Application::getInstance().getSettings().backgroundMusic) {
            m_destroying = false;  // Don't mark as destroying - music continues
            brls::Application::popActivity();
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    // Toggle controls with Y and Start (like Suwayomi reader)
    this->registerAction("Toggle Controls", brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
        toggleControls();
        return true;
    });

    // Queue controls for music (LB/RB for previous/next, triggers for shuffle/repeat)
    if (m_isQueueMode) {
        this->registerAction("Previous", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            playPrevious();
            return true;
        });

        this->registerAction("Next", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            playNext();
            return true;
        });

        this->registerAction("Shuffle", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
            if (!m_controlsVisible) {
                togglePlayPause();
            } else {
                toggleShuffle();
            }
            return true;
        });

        this->registerAction("Repeat", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
            toggleRepeat();
            return true;
        });
    } else {
        // Standard seek for non-queue playback
        this->registerAction("Rewind", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            resetControlsIdleTimer();
            int interval = Application::getInstance().getSettings().seekInterval;
            seek(-interval);
            return true;
        });

        this->registerAction("Forward", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            resetControlsIdleTimer();
            int interval = Application::getInstance().getSettings().seekInterval;
            seek(interval);
            return true;
        });

        // X = cycle audio track (when controls visible), pause/unpause (when hidden)
        this->registerAction("Audio Track", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
            if (!m_controlsVisible) {
                togglePlayPause();
            } else {
                resetControlsIdleTimer();
                cycleAudioTrack();
            }
            return true;
        });

        this->registerAction("Subtitle", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
            resetControlsIdleTimer();
            cycleSubtitleTrack();
            return true;
        });
    }

    // Wire up touch buttons with tap gesture recognizers
    if (playBtn) {
        playBtn->registerClickAction([this](brls::View* view) {
            togglePlayPause();
            return true;
        });
        playBtn->addGestureRecognizer(new brls::TapGestureRecognizer(playBtn));
    }

    if (rewindBtn) {
        rewindBtn->registerClickAction([this](brls::View* view) {
            if (m_isQueueMode) {
                playPrevious();
            } else {
                int interval = Application::getInstance().getSettings().seekInterval;
                seek(-interval);
            }
            return true;
        });
        rewindBtn->addGestureRecognizer(new brls::TapGestureRecognizer(rewindBtn));
    }

    if (forwardBtn) {
        forwardBtn->registerClickAction([this](brls::View* view) {
            if (m_isQueueMode) {
                playNext();
            } else {
                int interval = Application::getInstance().getSettings().seekInterval;
                seek(interval);
            }
            return true;
        });
        forwardBtn->addGestureRecognizer(new brls::TapGestureRecognizer(forwardBtn));
    }

    // Track overlay dismiss on tap or B button
    if (trackOverlay) {
        trackOverlay->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    hideTrackOverlay();
                }
            }));
    }

    // Queue overlay dismiss on tap
    if (queueOverlay) {
        queueOverlay->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    hideQueueOverlay();
                }
            }));
    }

    // Show mode-specific icons and wire touch
    if (m_isQueueMode) {
        // Music mode: hide center video controls, show music transport + info
        if (centerControls) centerControls->setVisibility(brls::Visibility::GONE);

        // Show music-specific UI elements
        if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
        if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);

        // Wire music transport buttons
        if (musicPlayBtn) {
            musicPlayBtn->registerClickAction([this](brls::View* view) {
                togglePlayPause();
                return true;
            });
            musicPlayBtn->addGestureRecognizer(new brls::TapGestureRecognizer(musicPlayBtn));
        }
        if (musicPrevBtn) {
            musicPrevBtn->registerClickAction([this](brls::View* view) {
                playPrevious();
                return true;
            });
            musicPrevBtn->addGestureRecognizer(new brls::TapGestureRecognizer(musicPrevBtn));
        }
        if (musicNextBtn) {
            musicNextBtn->registerClickAction([this](brls::View* view) {
                playNext();
                return true;
            });
            musicNextBtn->addGestureRecognizer(new brls::TapGestureRecognizer(musicNextBtn));
        }

        // In music mode: disable focusability on ALL hidden buttons
        // so focus navigation skips them entirely
        if (audioBtn) { audioBtn->setFocusable(false); audioBtn->setVisibility(brls::Visibility::GONE); }
        if (subBtn) { subBtn->setFocusable(false); subBtn->setVisibility(brls::Visibility::GONE); }
        if (videoBtn) { videoBtn->setFocusable(false); videoBtn->setVisibility(brls::Visibility::GONE); }
        // Also disable center video controls buttons (hidden parent but still focusable)
        if (playBtn) playBtn->setFocusable(false);
        if (rewindBtn) rewindBtn->setFocusable(false);
        if (forwardBtn) forwardBtn->setFocusable(false);

        // Shuffle toggle button
        if (shuffleBtn) {
            shuffleBtn->registerClickAction([this](brls::View* view) {
                toggleShuffle();
                return true;
            });
            shuffleBtn->addGestureRecognizer(new brls::TapGestureRecognizer(shuffleBtn));
            // Set initial icon based on current shuffle state
            updateShuffleIcon();
        }

        // Repeat toggle button
        if (repeatBtn) {
            repeatBtn->registerClickAction([this](brls::View* view) {
                toggleRepeat();
                return true;
            });
            repeatBtn->addGestureRecognizer(new brls::TapGestureRecognizer(repeatBtn));
            // Set initial icon based on current repeat state
            updateRepeatIcon();
        }

        if (queueBtn) {
            queueBtn->setVisibility(brls::Visibility::VISIBLE);
            queueBtn->registerClickAction([this](brls::View* view) {
                if (m_queueOverlayVisible) {
                    hideQueueOverlay();
                } else {
                    showQueueOverlay();
                }
                return true;
            });
            queueBtn->addGestureRecognizer(new brls::TapGestureRecognizer(queueBtn));
        }

        // Music mode: controls never auto-hide, always visible
        // Override the controls auto-hide for music
        if (controlsBox) {
            controlsBox->setVisibility(brls::Visibility::VISIBLE);
            controlsBox->setAlpha(1.0f);
        }

        // Hide title/artist from bottom controls (shown in musicInfo instead)
        if (titleLabel) titleLabel->setVisibility(brls::Visibility::GONE);
        if (artistLabel) artistLabel->setVisibility(brls::Visibility::GONE);
    } else {
        // Video mode: seek icons matching the configured interval
        int seekSec = Application::getInstance().getSettings().seekInterval;
        std::string rewindRes = "icons/rewind-" + std::to_string(seekSec) + ".png";
        std::string fwdRes = "icons/fast-forward-" + std::to_string(seekSec) + ".png";
        if (rewindIcon) rewindIcon->setImageFromRes(rewindRes);
        if (forwardIcon) forwardIcon->setImageFromRes(fwdRes);

        // Audio track button - shows track selection overlay
        if (audioBtn) {
            audioBtn->setVisibility(brls::Visibility::VISIBLE);
            if (audioIcon) {
                audioIcon->setImageFromRes("icons/translate.png");
            }
            audioBtn->registerClickAction([this](brls::View* view) {
                showTrackOverlay(TrackSelectMode::AUDIO);
                return true;
            });
            audioBtn->addGestureRecognizer(new brls::TapGestureRecognizer(audioBtn));
        }

        // Subtitle track button - shows track selection overlay
        if (subBtn) {
            subBtn->setVisibility(brls::Visibility::VISIBLE);
            if (subtitleIcon) {
                subtitleIcon->setImageFromRes("icons/subtitles.png");
            }
            subBtn->registerClickAction([this](brls::View* view) {
                showTrackOverlay(TrackSelectMode::SUBTITLE);
                return true;
            });
            subBtn->addGestureRecognizer(new brls::TapGestureRecognizer(subBtn));
        }

        // Video track button - shows track selection overlay
        if (videoBtn) {
            videoBtn->setVisibility(brls::Visibility::VISIBLE);
            if (videoIcon) {
                videoIcon->setImageFromRes("icons/video-image.png");
            }
            videoBtn->registerClickAction([this](brls::View* view) {
                showTrackOverlay(TrackSelectMode::VIDEO);
                return true;
            });
            videoBtn->addGestureRecognizer(new brls::TapGestureRecognizer(videoBtn));
        }
    }

    // Wire up skip button for intro/credits
    if (skipBtn) {
        skipBtn->registerClickAction([this](brls::View* view) {
            skipToMarkerEnd();
            return true;
        });
        skipBtn->addGestureRecognizer(new brls::TapGestureRecognizer(skipBtn));
    }

    // Start update timer
    m_updateTimer.setCallback([this]() {
        updateProgress();
    });
    m_updateTimer.start(1000); // Update every second

    // Start with controls hidden if auto-hide is enabled
    int autoHide = Application::getInstance().getSettings().controlsAutoHideSeconds;
    if (autoHide > 0 && !m_isPhoto) {
        hideControls();
    }
}

void PlayerActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);

    // Re-enable background thumbnail loading now that playback is ending
    ImageLoader::setPaused(false);

    // If background music is enabled and we're in queue mode, don't stop playback
    if (m_isQueueMode && Application::getInstance().getSettings().backgroundMusic && !m_destroying) {
        brls::Logger::info("PlayerActivity: Leaving with background music enabled, not stopping");
        m_updateTimer.stop();
        if (m_alive) m_alive->store(false);
        return;
    }

    // Mark as destroying to prevent timer, image loader, and batch callbacks
    m_destroying = true;
    m_queueBatchActive = false;
    if (m_alive) {
        m_alive->store(false);
    }

    // Stop update timer first
    m_updateTimer.stop();

    // Clear any pending deferred init (user backed out before timer fired)
    m_pendingPlayUrl.clear();
    m_pendingPlayTitle.clear();

    // Hide video view
    if (videoView) {
        videoView->setVideoVisible(false);
    }

    // For photos, nothing to stop
    if (m_isPhoto) {
        return;
    }

    // Stop playback and save progress
    MpvPlayer& player = MpvPlayer::getInstance();

    // Only try to save progress if player is in a valid state
    if (player.isInitialized() && (player.isPlaying() || player.isPaused())) {
        double position = player.getPosition();
        double duration = 0.0;

        // For music queue mode, prefer Plex API duration (full track length)
        // over MPV duration which may only reflect buffered/demuxed portion
        if (m_isQueueMode) {
            const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
            if (track && track->duration > 0) {
                duration = (double)track->duration;
            }
        }
        if (duration <= 0)
            duration = player.getDuration();

        if (position > 0 || m_transcodeBaseOffsetMs > 0) {
            int timeMs = m_transcodeBaseOffsetMs + (int)(position * 1000);

            if (m_isLocalFile) {
                // Save progress for downloaded media
                DownloadsManager::getInstance().updateProgress(m_mediaKey, timeMs);
                DownloadsManager::getInstance().saveState();
                brls::Logger::info("PlayerActivity: Saved local progress {}ms for {}", timeMs, m_mediaKey);
            } else if (!m_mediaKey.empty()) {
                if (!m_isQueueMode) {
                    PlexClient::getInstance().updatePlayProgress(m_mediaKey, timeMs);
                }
                // Report stopped timeline so Plex knows playback ended with full duration
                std::string ratingKey = m_mediaKey;
                if (m_isQueueMode) {
                    const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
                    if (track) ratingKey = track->ratingKey;
                }
                std::string key = "/library/metadata/" + ratingKey;
                PlexClient::getInstance().reportTimeline(
                    ratingKey, key, "stopped", timeMs, (int)(duration * 1000));
            }
        }
    }

    // Save queue state
    if (m_isQueueMode) {
        MusicQueue::getInstance().saveState();
    }

    // Stop playback (safe to call even if not playing)
    if (player.isInitialized()) {
        player.stop();
    }

    m_isPlaying = false;
}

void PlayerActivity::loadFromQueue() {
    // Prevent rapid re-entry
    if (m_loadingMedia) {
        brls::Logger::debug("PlayerActivity: Already loading media, skipping");
        return;
    }
    m_loadingMedia = true;

    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* track = queue.getCurrentTrack();

    if (!track) {
        brls::Logger::error("PlayerActivity: No current track in queue");
        m_loadingMedia = false;
        return;
    }

    brls::Logger::info("PlayerActivity: Loading track from queue: {} - {}",
                      track->artist, track->title);

    // Reset streams cache for the new track
    m_streamsLoaded = false;
    m_plexStreams.clear();
    m_partId = 0;

    // If resuming and MPV is already playing/paused, just update the UI
    // without restarting the track (user pressed circle to return to player)
    MpvPlayer& resumePlayer = MpvPlayer::getInstance();
    if (m_isResuming && resumePlayer.isInitialized() &&
        (resumePlayer.isPlaying() || resumePlayer.isPaused())) {
        brls::Logger::info("PlayerActivity: Resuming existing playback, skipping reload");
        m_isPlaying = resumePlayer.isPlaying();
        m_mediaKey = track->ratingKey;
        m_isResuming = false;

        // Update display labels and album art, then return without reloading
        if (musicTitleLabel) musicTitleLabel->setText(track->title);
        if (musicArtistLabel) musicArtistLabel->setText(track->artist);
        if (titleLabel) titleLabel->setText(track->title);
        if (artistLabel) {
            artistLabel->setText(track->artist);
            artistLabel->setVisibility(brls::Visibility::VISIBLE);
        }
        updateQueueDisplay();

        // Load album art - prefer local file for downloaded tracks
        if (albumArt && !track->ratingKey.empty()) {
            DownloadItem resumeDlItem;
            if (DownloadsManager::getInstance().getDownloadCopy(track->ratingKey, resumeDlItem) &&
                resumeDlItem.state == DownloadState::COMPLETED && !resumeDlItem.thumbPath.empty()) {
                if (ImageLoader::loadFromFile(resumeDlItem.thumbPath, albumArt)) {
                    albumArt->setVisibility(brls::Visibility::VISIBLE);
                }
            } else if (!track->thumb.empty()) {
                PlexClient& client = PlexClient::getInstance();
                std::string thumbUrl = client.getThumbnailUrl(track->thumb, 400, 400);
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                    img->setVisibility(brls::Visibility::VISIBLE);
                }, albumArt, m_alive);
            }
        }

        // Show music UI elements
        if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
        if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);
        if (videoView) videoView->setVisibility(brls::Visibility::GONE);
        if (photoImage) photoImage->setVisibility(brls::Visibility::GONE);

        updatePlayPauseLabel();
        m_loadingMedia = false;
        return;
    }

    // Update display - use music info labels (between cover and play controls)
    if (musicTitleLabel) {
        musicTitleLabel->setText(track->title);
    }
    if (musicArtistLabel) {
        musicArtistLabel->setText(track->artist);
    }
    // Also update bottom controls title for non-music fallback
    if (titleLabel) {
        titleLabel->setText(track->title);
    }
    if (artistLabel) {
        artistLabel->setText(track->artist);
        artistLabel->setVisibility(brls::Visibility::VISIBLE);
    }

    // Update queue info display
    updateQueueDisplay();

    // Use the rating key to get the playback URL
    m_mediaKey = track->ratingKey;
    std::string url;

    // Check if this track is downloaded locally - play from local file if available
    DownloadsManager& downloads = DownloadsManager::getInstance();
    DownloadItem dlItem;
    bool useLocalFile = false;
    if (downloads.getDownloadCopy(track->ratingKey, dlItem) && dlItem.state == DownloadState::COMPLETED) {
        url = dlItem.localPath;
        useLocalFile = true;
        brls::Logger::info("PlayerActivity: Using downloaded file for track: {}", url);

        // Load cover art from local file if available (preferred over server URL)
        if (albumArt && !dlItem.thumbPath.empty()) {
            if (ImageLoader::loadFromFile(dlItem.thumbPath, albumArt)) {
                albumArt->setVisibility(brls::Visibility::VISIBLE);
            }
        }
    } else {
        // Stream from server
        PlexClient& client = PlexClient::getInstance();
        if (!client.getTranscodeUrl(track->ratingKey, url, 0)) {
            brls::Logger::error("Failed to get transcode URL for track: {}", track->ratingKey);
            m_loadingMedia = false;
            return;
        }

        // Load album art from server (only when streaming, not offline)
        if (albumArt && !track->thumb.empty()) {
            PlexClient& artClient = PlexClient::getInstance();
            std::string thumbUrl = artClient.getThumbnailUrl(track->thumb, 400, 400);
            bool wasPaused = ImageLoader::isPaused();
            if (wasPaused) ImageLoader::setPaused(false);
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, albumArt, m_alive);
            if (wasPaused) ImageLoader::setPaused(true);
            albumArt->setVisibility(brls::Visibility::VISIBLE);
        }
    }

    // Pause image loading and free cache to reclaim memory for MPV.
    ImageLoader::setPaused(true);
    ImageLoader::cancelAll();
    ImageLoader::clearCache();

    MpvPlayer& player = MpvPlayer::getInstance();

    // Set audio-only mode BEFORE initializing
    player.setAudioOnly(true);

    // Stream audio directly via MPV (transcode API returns mp3 stream or local file)
    if (!player.isInitialized()) {
        // Defer MPV init + load to after activity transition completes
        m_pendingPlayUrl = url;
        m_pendingPlayTitle = track->title;
        m_pendingIsAudio = true;
        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Player already initialized (track change) - load immediately
    if (!player.loadUrl(url, track->title)) {
        brls::Logger::error("Failed to load URL: {}", url);
        m_loadingMedia = false;
        return;
    }

    m_isPlaying = true;
    m_loadingMedia = false;
}

void PlayerActivity::loadMedia() {
    // Prevent rapid re-entry
    if (m_loadingMedia) {
        brls::Logger::debug("PlayerActivity: Already loading media, skipping");
        return;
    }
    m_loadingMedia = true;

    // Handle direct file playback (debug/testing)
    if (m_isDirectFile) {
        brls::Logger::info("PlayerActivity: Playing direct file: {}", m_directFilePath);

        // Use stream title if set, otherwise extract filename from path
        std::string displayTitle;
        if (!m_streamTitle.empty()) {
            displayTitle = m_streamTitle;
        } else {
            size_t lastSlash = m_directFilePath.find_last_of("/\\");
            displayTitle = (lastSlash != std::string::npos)
                ? m_directFilePath.substr(lastSlash + 1)
                : m_directFilePath;
        }

        if (titleLabel) {
            titleLabel->setText(displayTitle);
        }

        // Detect if this is an audio file
        std::string lowerPath = m_directFilePath;
        for (auto& c : lowerPath) c = tolower(c);
        bool isAudioFile = (lowerPath.find(".mp3") != std::string::npos ||
                           lowerPath.find(".m4a") != std::string::npos ||
                           lowerPath.find(".aac") != std::string::npos ||
                           lowerPath.find(".flac") != std::string::npos ||
                           lowerPath.find(".ogg") != std::string::npos ||
                           lowerPath.find(".wav") != std::string::npos ||
                           lowerPath.find(".wma") != std::string::npos);

        brls::Logger::info("PlayerActivity: File type detection - audio: {}", isAudioFile);

        // Pause image loading and free cache to reclaim memory for MPV
        ImageLoader::setPaused(true);
        ImageLoader::cancelAll();
        ImageLoader::clearCache();

        MpvPlayer& player = MpvPlayer::getInstance();

        // Set audio-only mode BEFORE initializing (to skip render context)
        player.setAudioOnly(isAudioFile);

        if (!player.isInitialized()) {
            // Defer MPV init + load to after activity transition completes.
            // initRenderContext() creates GXM resources and loadUrl() spawns
            // decoder threads that use the shared GXM context - both conflict
            // with NanoVG drawing during the borealis show phase.
            m_pendingPlayUrl = m_directFilePath;
            m_pendingPlayTitle = m_streamTitle.empty() ? "Test File" : m_streamTitle;
            m_pendingIsAudio = isAudioFile;
            m_loadingMedia = false;
            return;
        }

        // Player already initialized - load immediately
        std::string loadTitle = m_streamTitle.empty() ? "Test File" : m_streamTitle;
        if (!player.loadUrl(m_directFilePath, loadTitle)) {
            brls::Logger::error("Failed to load direct file: {}", m_directFilePath);
            m_loadingMedia = false;
            return;
        }

        // Show video view only for video files
        if (videoView && !isAudioFile) {
            videoView->setVisibility(brls::Visibility::VISIBLE);
            videoView->setVideoVisible(true);
        }

        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Handle local file playback (downloaded media)
    if (m_isLocalFile) {
        DownloadsManager& downloads = DownloadsManager::getInstance();
        DownloadItem dlItem;

        if (!downloads.getDownloadCopy(m_mediaKey, dlItem) || dlItem.state != DownloadState::COMPLETED) {
            brls::Logger::error("PlayerActivity: Downloaded media not found or incomplete");
            m_loadingMedia = false;
            return;
        }

        brls::Logger::info("PlayerActivity: Playing local file: {}", dlItem.localPath);

        // Detect if this is a music track
        bool isAudioTrack = (dlItem.mediaType == "track");

        if (isAudioTrack) {
            // Set up music UI labels
            if (musicTitleLabel) musicTitleLabel->setText(dlItem.title);
            if (musicArtistLabel) musicArtistLabel->setText(dlItem.parentTitle);
            if (titleLabel) titleLabel->setText(dlItem.title);
            if (artistLabel) {
                artistLabel->setText(dlItem.parentTitle);
                artistLabel->setVisibility(brls::Visibility::VISIBLE);
            }

            // Load cover art from downloaded file if available
            if (albumArt && !dlItem.thumbPath.empty()) {
                // Load local cover art image directly
                ImageLoader::loadFromFile(dlItem.thumbPath, albumArt);
                albumArt->setVisibility(brls::Visibility::VISIBLE);
            }

            // Show music UI, hide video view
            if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
            if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);
            if (videoView) videoView->setVisibility(brls::Visibility::GONE);
            if (photoImage) photoImage->setVisibility(brls::Visibility::GONE);
        } else {
            if (titleLabel) {
                std::string title = dlItem.title;
                if (!dlItem.parentTitle.empty()) {
                    title = dlItem.parentTitle + " - " + dlItem.title;
                }
                titleLabel->setText(title);
            }
        }

        // Pause image loading and free cache to reclaim memory for MPV
        ImageLoader::setPaused(true);
        ImageLoader::cancelAll();
        ImageLoader::clearCache();

        MpvPlayer& player = MpvPlayer::getInstance();

        // Set audio-only mode for music tracks (skip render context)
        player.setAudioOnly(isAudioTrack);

        // Resume from saved viewOffset if resumePlayback is enabled
        // If near the end (>= 95% watched), start from beginning instead
        if (Application::getInstance().getSettings().resumePlayback && dlItem.viewOffset > 0) {
            bool nearEnd = (dlItem.duration > 0 && dlItem.viewOffset >= dlItem.duration * 0.95);
            if (!nearEnd) {
                m_pendingSeek = dlItem.viewOffset / 1000.0;
            }
        }

        if (!player.isInitialized()) {
            // Defer MPV init + load to after activity transition completes
            m_pendingPlayUrl = dlItem.localPath;
            m_pendingPlayTitle = dlItem.title;
            m_pendingIsAudio = isAudioTrack;
            m_loadingMedia = false;
            return;
        }

        // Player already initialized - load immediately
        if (!player.loadUrl(dlItem.localPath, dlItem.title)) {
            brls::Logger::error("Failed to load local file: {}", dlItem.localPath);
            m_loadingMedia = false;
            return;
        }

        // Show video view only for non-audio content
        if (!isAudioTrack && videoView) {
            videoView->setVisibility(brls::Visibility::VISIBLE);
            videoView->setVideoVisible(true);
        }

        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Remote playback from Plex server
    PlexClient& client = PlexClient::getInstance();
    MediaItem item;

    if (client.fetchMediaDetails(m_mediaKey, item)) {
        // Store media type and episode info for auto-play-next
        m_mediaType = item.mediaType;
        if (item.mediaType == MediaType::EPISODE) {
            m_episodeIndex = item.index;
            m_parentRatingKey = item.parentRatingKey;
        }

        // Store markers for intro/credits skip
        m_markers = item.markers;
        if (!m_markers.empty()) {
            brls::Logger::info("PlayerActivity: Loaded {} markers for {}", m_markers.size(), item.title);
        }
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
            m_loadingMedia = false;

            // Load the full-size photo
            if (!item.thumb.empty()) {
                std::string photoUrl = client.getThumbnailUrl(item.thumb, 960, 544);
                brls::Logger::debug("Photo URL: {}", photoUrl);

                // Load photo into the view (photoImage is defined in player.xml)
                if (photoImage) {
                    photoImage->setVisibility(brls::Visibility::VISIBLE);
                    ImageLoader::loadAsync(photoUrl, [](brls::Image* image) {
                        // Photo loaded
                    }, photoImage, m_alive);
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

        // Detect if this is audio content
        bool isAudioContent = (item.mediaType == MediaType::MUSIC_TRACK);
        brls::Logger::info("PlayerActivity: Media type detection - audio: {}, type: {}",
                          isAudioContent, (int)item.mediaType);

        // Show album art for audio content (before we pause the image loader)
        if (isAudioContent && albumArt) {
            // Try track thumb, then album (parent) thumb, then artist (grandparent) thumb
            std::string artPath = item.thumb;
            if (artPath.empty()) artPath = item.parentThumb;
            if (artPath.empty()) artPath = item.grandparentThumb;

            if (!artPath.empty()) {
                std::string thumbUrl = client.getThumbnailUrl(artPath, 300, 300);
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* image) {
                    // Art loaded
                }, albumArt, m_alive);
                albumArt->setVisibility(brls::Visibility::VISIBLE);
            }
        }

        // Get transcode URL for video/audio (forces Plex to convert to Vita-compatible format)
        // Only resume from viewOffset if resumePlayback is enabled
        // If near the end (>= 95% watched), start from beginning instead
        int resumeOffset = 0;
        if (Application::getInstance().getSettings().resumePlayback && item.viewOffset > 0) {
            bool nearEnd = (item.duration > 0 && item.viewOffset >= item.duration * 0.95);
            if (!nearEnd) {
                resumeOffset = item.viewOffset;
            }
        }
        m_transcodeBaseOffsetMs = resumeOffset;
        std::string url;
        if (client.getTranscodeUrl(m_mediaKey, url, resumeOffset)) {
            // Pause image loading and free cache memory before initializing MPV.
            // This stops background thumbnail fetches from competing with media
            // streaming, and frees memory (Vita only has 256MB).
            ImageLoader::setPaused(true);
            ImageLoader::cancelAll();
            ImageLoader::clearCache();

            MpvPlayer& player = MpvPlayer::getInstance();

            // Set audio-only mode BEFORE initializing
            player.setAudioOnly(isAudioContent);

            // Stream directly via MPV (transcode API returns mp4/mp3 stream)
            if (!player.isInitialized()) {
                // Defer MPV init + load to after activity transition completes.
                // initRenderContext() creates GXM resources (framebuffer, render target)
                // and loadUrl() spawns decoder threads that use the shared GXM context
                // via hwdec=vita-copy. Both conflict with NanoVG drawing during the
                // borealis activity show phase, causing a consistent SIGSEGV.
                brls::Logger::info("PlayerActivity: Deferring MPV init to after activity transition");
                m_pendingPlayUrl = url;
                m_pendingPlayTitle = item.title;
                m_pendingIsAudio = isAudioContent;
            } else {
                // Player already initialized (e.g., mode didn't change) - load immediately
                brls::Logger::debug("PlayerActivity: Calling player.loadUrl...");
                if (!player.loadUrl(url, item.title)) {
                    brls::Logger::error("Failed to load URL: {}", url);
                    m_loadingMedia = false;
                    return;
                }

                // Show video view only for video content
                if (videoView && !isAudioContent) {
                    videoView->setVisibility(brls::Visibility::VISIBLE);
                    videoView->setVideoVisible(true);
                    brls::Logger::debug("Video view enabled");
                }

                m_isPlaying = true;
                brls::Logger::debug("PlayerActivity: loadMedia completed successfully for Plex stream");
            }
        } else {
            brls::Logger::error("Failed to get transcode URL for: {}", m_mediaKey);
        }
    }

    brls::Logger::debug("PlayerActivity: loadMedia exiting");
    m_loadingMedia = false;
}

void PlayerActivity::updateProgress() {
    // Don't update if destroying or showing photo
    if (m_destroying || m_isPhoto) return;

    // Deferred MPV initialization (Phase 1 of 2):
    // Create MPV and its GXM render context, but do NOT call loadUrl yet.
    // loadUrl spawns decoder threads that use the shared GXM context via
    // hwdec=vita-copy. If the decoder thread starts before NanoVG has drawn
    // at least one clean frame after initRenderContext(), the concurrent GXM
    // access crashes. So we schedule loadUrl via brls::sync for the NEXT frame.
    if (!m_pendingPlayUrl.empty()) {
        std::string url = m_pendingPlayUrl;
        std::string title = m_pendingPlayTitle;
        bool isAudio = m_pendingIsAudio;
        m_pendingPlayUrl.clear();
        m_pendingPlayTitle.clear();

        brls::Logger::info("PlayerActivity: Performing deferred MPV init (phase 1: create context)...");

        MpvPlayer& player = MpvPlayer::getInstance();
        player.setAudioOnly(isAudio);

        if (!player.isInitialized()) {
            if (!player.init()) {
                brls::Logger::error("PlayerActivity: Deferred MPV init failed");
                return;
            }
        }

        // Phase 2: schedule loadUrl for the NEXT main-loop iteration.
        // brls::sync callbacks execute between frames, so NanoVG will draw one
        // complete frame with the freshly-created GXM state before the decoder
        // thread gets a chance to touch the shared GXM context.
        auto alive = m_alive;
        brls::sync([this, url, title, isAudio, alive]() {
            if (!alive->load() || m_destroying) return;

            brls::Logger::info("PlayerActivity: Deferred MPV load (phase 2: loadUrl)...");

            MpvPlayer& player = MpvPlayer::getInstance();

            if (player.loadUrl(url, title)) {
                if (videoView && !isAudio) {
                    videoView->setVisibility(brls::Visibility::VISIBLE);
                    videoView->setVideoVisible(true);
                    brls::Logger::debug("Video view enabled (deferred)");
                }
                m_isPlaying = true;
                updatePlayPauseLabel();
                brls::Logger::info("PlayerActivity: Deferred load started successfully");
            } else {
                brls::Logger::error("PlayerActivity: Deferred loadUrl failed");
            }
        });
        return;
    }

    MpvPlayer& player = MpvPlayer::getInstance();

    if (!player.isInitialized()) {
        return;
    }

    // Always process MPV events to handle state transitions
    player.update();

    // Skip UI updates while MPV is still loading - be gentle on Vita's limited hardware
    if (player.isLoading()) {
        return;
    }

    // Handle pending seek when playback becomes ready
    if (m_pendingSeek > 0.0 && player.isPlaying()) {
        player.seekTo(m_pendingSeek);
        m_pendingSeek = 0.0;
    }

    double position = player.getPosition();
    double duration = 0.0;

    // For music queue mode, prefer Plex API duration (full track length)
    // over MPV duration which may only reflect buffered/demuxed portion
    if (m_isQueueMode) {
        const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
        if (track && track->duration > 0) {
            duration = (double)track->duration;
        }
    }
    if (duration <= 0)
        duration = player.getDuration();

    if (duration > 0) {
        if (progressSlider) {
            m_updatingSlider = true;
            progressSlider->setProgress((float)(position / duration));
            m_updatingSlider = false;
        }

        // Update time labels: elapsed on left, remaining on right
        {
            int posMin = (int)position / 60;
            int posSec = (int)position % 60;
            int remaining = std::max(0, (int)(duration - position));
            int remMin = remaining / 60;
            int remSec = remaining % 60;

            char elapsedStr[16];
            snprintf(elapsedStr, sizeof(elapsedStr), "%d:%02d", posMin, posSec);
            char remainStr[16];
            snprintf(remainStr, sizeof(remainStr), "-%d:%02d", remMin, remSec);

            if (timeElapsedLabel) timeElapsedLabel->setText(elapsedStr);
            if (timeRemainingLabel) timeRemainingLabel->setText(remainStr);

            // Keep legacy time label updated for video mode
            if (timeLabel) {
                int durMin = (int)duration / 60;
                int durSec = (int)duration % 60;
                char timeStr[32];
                snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d",
                         posMin, posSec, durMin, durSec);
                timeLabel->setText(timeStr);
            }
        }
    }

    // Update skip intro/credits button
    if (!m_markers.empty() && duration > 0) {
        double posMs = (m_transcodeBaseOffsetMs + position * 1000.0);
        updateSkipButton(posMs);
    }

    // Auto-hide controls after inactivity
    int autoHide = Application::getInstance().getSettings().controlsAutoHideSeconds;
    if (autoHide > 0 && m_controlsVisible && !m_isPhoto) {
        m_controlsIdleSeconds++;
        if (m_controlsIdleSeconds >= autoHide) {
            hideControls();
        }
    }

    // Report timeline to Plex server periodically and on state changes
    // This sends duration so Plex shows the full track/video length
    if (!m_mediaKey.empty() && !m_isLocalFile && !m_isDirectFile) {
        std::string currentState = player.isPlaying() ? "playing" :
                                   player.isPaused()  ? "paused"  : "stopped";

        bool stateChanged = (currentState != m_lastTimelineState);
        m_timelineCounter++;

        if (stateChanged || m_timelineCounter >= 10) {
            m_timelineCounter = 0;
            m_lastTimelineState = currentState;

            int timeMs = m_transcodeBaseOffsetMs + (int)(position * 1000);
            int durationMs = (int)(duration * 1000);

            std::string ratingKey = m_mediaKey;
            // In queue mode, use the current track's ratingKey
            if (m_isQueueMode) {
                const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
                if (track) {
                    ratingKey = track->ratingKey;
                }
            }

            std::string key = "/library/metadata/" + ratingKey;
            PlexClient::getInstance().reportTimeline(
                ratingKey, key, currentState, timeMs, durationMs);
        }
    }

    // Check if playback ended BEFORE syncing play/pause state,
    // because hasEnded() means isPlaying() is already false - if we sync
    // m_isPlaying first, we'd clear the flag and never detect the end.
    if (m_isPlaying && player.hasEnded()) {
        m_isPlaying = false;  // Prevent multiple triggers

        if (m_isQueueMode) {
            // Notify queue that track ended - it will call onTrackEnded
            MusicQueue::getInstance().onTrackEnded();
        } else {
            PlexClient::getInstance().markAsWatched(m_mediaKey);

            // Delete downloaded file after watching if setting is enabled
            if (m_isLocalFile && Application::getInstance().getSettings().deleteAfterWatch) {
                DownloadsManager::getInstance().deleteDownload(m_mediaKey);
                brls::Logger::info("PlayerActivity: Auto-deleted download after watch: {}", m_mediaKey);
            }

            // Auto-play next episode if enabled and this is an episode
            if (Application::getInstance().getSettings().autoPlayNext
                && m_mediaType == MediaType::EPISODE
                && !m_parentRatingKey.empty()
                && !m_isLocalFile) {
                // Fetch siblings (episodes in the same season)
                std::vector<MediaItem> episodes;
                if (PlexClient::getInstance().fetchChildren(m_parentRatingKey, episodes)) {
                    // Find next episode by index
                    for (const auto& ep : episodes) {
                        if (ep.index == m_episodeIndex + 1) {
                            brls::Logger::info("PlayerActivity: Auto-playing next episode: {}", ep.title);
                            brls::Application::popActivity();
                            Application::getInstance().pushPlayerActivity(ep.ratingKey);
                            return;
                        }
                    }
                }
            }

            brls::Application::popActivity();
        }
    }

    // Keep play/pause label in sync with actual player state
    // (must come after the hasEnded check above)
    bool actuallyPlaying = player.isPlaying();
    if (actuallyPlaying != m_isPlaying) {
        m_isPlaying = actuallyPlaying;
        updatePlayPauseLabel();
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
    updatePlayPauseLabel();
}

void PlayerActivity::updatePlayPauseLabel() {
    if (playPauseIcon) {
        playPauseIcon->setImageFromRes(m_isPlaying ? "icons/pause.png" : "icons/play.png");
    }
    // Also update music transport play icon
    if (musicPlayIcon) {
        musicPlayIcon->setImageFromRes(m_isPlaying ? "icons/pause.png" : "icons/play.png");
    }
}

void PlayerActivity::cycleAudioTrack() {
    showTrackOverlay(TrackSelectMode::AUDIO);
}

void PlayerActivity::cycleSubtitleTrack() {
    showTrackOverlay(TrackSelectMode::SUBTITLE);
}

void PlayerActivity::fetchPlexStreams() {
    if (m_streamsLoaded || m_mediaKey.empty()) return;

    PlexClient& client = PlexClient::getInstance();
    if (client.fetchStreams(m_mediaKey, m_plexStreams, m_partId)) {
        m_streamsLoaded = true;
        brls::Logger::info("fetchPlexStreams: Loaded {} streams, partId={}", m_plexStreams.size(), m_partId);
    }
}

void PlayerActivity::showTrackOverlay(TrackSelectMode mode) {
    if (m_trackSelectMode != TrackSelectMode::NONE) {
        hideTrackOverlay();
        return;
    }

    m_trackSelectMode = mode;
    populateTrackList(mode);

    if (trackOverlay) {
        trackOverlay->setVisibility(brls::Visibility::VISIBLE);
        // Register B button to dismiss overlay
        trackOverlay->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
            hideTrackOverlay();
            return true;
        });

        // Give focus to the currently selected track item for controller navigation
        if (trackList && !trackList->getChildren().empty()) {
            int idx = std::min(m_selectedTrackIndex, (int)trackList->getChildren().size() - 1);
            if (idx < 0) idx = 0;
            brls::Application::giveFocus(trackList->getChildren()[idx]);
        }
        // Reset overlay title focusable state (was set temporarily during list rebuild)
        if (trackOverlayTitle) {
            trackOverlayTitle->setFocusable(false);
        }
    }
}

void PlayerActivity::hideTrackOverlay() {
    TrackSelectMode prevMode = m_trackSelectMode;
    m_trackSelectMode = TrackSelectMode::NONE;
    // Restore the overlay title's focusable state (may have been set temporarily)
    if (trackOverlayTitle) {
        trackOverlayTitle->setFocusable(false);
    }
    if (trackOverlay) {
        trackOverlay->setVisibility(brls::Visibility::GONE);
    }
    // Restore focus to the appropriate button (only if visible and focusable)
    if (prevMode == TrackSelectMode::SUBTITLE && subBtn &&
        subBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(subBtn);
    } else if (prevMode == TrackSelectMode::VIDEO && videoBtn &&
               videoBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(videoBtn);
    } else if (audioBtn && audioBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(audioBtn);
    } else if (m_isQueueMode && musicPlayBtn) {
        brls::Application::giveFocus(musicPlayBtn);
    } else if (playBtn) {
        brls::Application::giveFocus(playBtn);
    }
}

void PlayerActivity::populateTrackList(TrackSelectMode mode) {
    if (!trackList || !trackOverlayTitle) return;

    // Transfer focus away before clearing, so destroying focused children is safe
    if (!trackList->getChildren().empty() && trackOverlayTitle) {
        trackOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(trackOverlayTitle);
    }

    // Clear existing items
    trackList->clearViews();
    m_selectedTrackIndex = 0;

    // Set title
    switch (mode) {
        case TrackSelectMode::AUDIO:
            trackOverlayTitle->setText("Audio Tracks");
            break;
        case TrackSelectMode::SUBTITLE:
            trackOverlayTitle->setText(m_isQueueMode ? "Lyrics" : "Subtitles");
            break;
        case TrackSelectMode::VIDEO:
            trackOverlayTitle->setText("Video Tracks");
            break;
        default:
            return;
    }

    // Fetch Plex streams (if not already cached) - these have all tracks
    fetchPlexStreams();

    int plexStreamType = (mode == TrackSelectMode::VIDEO) ? 1 :
                         (mode == TrackSelectMode::AUDIO) ? 2 : 3;

    // Collect Plex streams of the requested type
    // For subtitles, also include streamType 4
    std::vector<const PlexStream*> plexTracksOfType;
    for (const auto& ps : m_plexStreams) {
        if (ps.streamType == plexStreamType ||
            (mode == TrackSelectMode::SUBTITLE && ps.streamType == 4)) {
            plexTracksOfType.push_back(&ps);
        }
    }

    // For audio and subtitle modes, use Plex streams as the primary source
    // because HLS transcoding only muxes the selected track into the stream,
    // so MPV only sees 1 audio track. Plex metadata has all available tracks.
    bool usePlexStreams = (mode == TrackSelectMode::AUDIO || mode == TrackSelectMode::SUBTITLE)
                         && !plexTracksOfType.empty();

    // For subtitles, add "Off" option first
    if (mode == TrackSelectMode::SUBTITLE) {
        brls::Box* item = new brls::Box();
        item->setAxis(brls::Axis::ROW);
        item->setJustifyContent(brls::JustifyContent::FLEX_START);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setPaddingTop(10);
        item->setPaddingBottom(10);
        item->setPaddingLeft(12);
        item->setPaddingRight(12);
        item->setCornerRadius(4);
        item->setFocusable(true);

        brls::Label* label = new brls::Label();
        label->setText(m_isQueueMode ? "Off (No Lyrics)" : "Off (No Subtitles)");
        label->setFontSize(16);
        label->setTextColor(nvgRGB(220, 220, 220));
        item->addView(label);

        item->registerClickAction([this](brls::View* view) {
            selectTrack(TrackSelectMode::SUBTITLE, -1);
            return true;
        });
        item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
        trackList->addView(item);
    }

    if (usePlexStreams) {
        // Build track items from Plex streams (shows ALL available tracks)
        for (size_t i = 0; i < plexTracksOfType.size(); i++) {
            const auto& ps = *plexTracksOfType[i];

            std::string displayStr = ps.displayTitle;
            if (displayStr.empty()) {
                displayStr = ps.language;
                if (displayStr.empty()) displayStr = "Track " + std::to_string(i + 1);
                if (!ps.codec.empty()) displayStr += " [" + ps.codec + "]";
            }

            brls::Box* item = new brls::Box();
            item->setAxis(brls::Axis::ROW);
            item->setJustifyContent(brls::JustifyContent::FLEX_START);
            item->setAlignItems(brls::AlignItems::CENTER);
            item->setPaddingTop(10);
            item->setPaddingBottom(10);
            item->setPaddingLeft(12);
            item->setPaddingRight(12);
            item->setCornerRadius(4);
            item->setFocusable(true);

            if (ps.selected) {
                item->setBackgroundColor(nvgRGBA(80, 80, 200, 100));
                item->setBorderColor(nvgRGB(100, 130, 255));
                item->setBorderThickness(1);
                // Track index for focus when overlay opens
                m_selectedTrackIndex = static_cast<int>(trackList->getChildren().size());
            }

            std::string prefix = ps.selected ? "> " : "  ";
            brls::Label* label = new brls::Label();
            label->setText(prefix + displayStr);
            label->setFontSize(16);
            label->setTextColor(ps.selected ? nvgRGB(150, 200, 255) : nvgRGB(220, 220, 220));
            item->addView(label);

            int plexStreamId = ps.id;
            item->registerClickAction([this, mode, plexStreamId](brls::View* view) {
                selectTrack(mode, plexStreamId);
                return true;
            });
            item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
            trackList->addView(item);
        }
    } else {
        // Fallback: use MPV track list (for video tracks, or when no Plex data)
        MpvPlayer& player = MpvPlayer::getInstance();
        std::string mpvType;
        if (mode == TrackSelectMode::AUDIO) mpvType = "audio";
        else if (mode == TrackSelectMode::SUBTITLE) mpvType = "sub";
        else if (mode == TrackSelectMode::VIDEO) mpvType = "video";

        auto mpvTracks = player.getTrackList(mpvType);

        for (size_t i = 0; i < mpvTracks.size(); i++) {
            const auto& track = mpvTracks[i];

            std::string displayStr;
            if (!track.lang.empty()) {
                displayStr = track.lang;
            } else {
                displayStr = "Track " + std::to_string(track.id);
            }
            if (!track.title.empty()) {
                displayStr += " - " + track.title;
            }
            if (!track.codec.empty()) {
                displayStr += " [" + track.codec + "]";
            }

            brls::Box* item = new brls::Box();
            item->setAxis(brls::Axis::ROW);
            item->setJustifyContent(brls::JustifyContent::FLEX_START);
            item->setAlignItems(brls::AlignItems::CENTER);
            item->setPaddingTop(10);
            item->setPaddingBottom(10);
            item->setPaddingLeft(12);
            item->setPaddingRight(12);
            item->setCornerRadius(4);
            item->setFocusable(true);

            if (track.selected) {
                item->setBackgroundColor(nvgRGBA(80, 80, 200, 100));
                item->setBorderColor(nvgRGB(100, 130, 255));
                item->setBorderThickness(1);
            }

            std::string prefix = track.selected ? "> " : "  ";
            brls::Label* label = new brls::Label();
            label->setText(prefix + displayStr);
            label->setFontSize(16);
            label->setTextColor(track.selected ? nvgRGB(150, 200, 255) : nvgRGB(220, 220, 220));
            item->addView(label);

            int trackId = track.id;
            item->registerClickAction([this, mode, trackId](brls::View* view) {
                selectTrack(mode, trackId);
                return true;
            });
            item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
            trackList->addView(item);
        }

        if (mpvTracks.empty() && mode != TrackSelectMode::SUBTITLE) {
            brls::Label* label = new brls::Label();
            label->setText("No tracks available");
            label->setFontSize(16);
            label->setTextColor(nvgRGB(180, 180, 180));
            label->setMargins(12, 12, 12, 12);
            trackList->addView(label);
        }
    }

    // For subtitles, add "Search for Subtitles" option at the bottom
    if (mode == TrackSelectMode::SUBTITLE && !m_mediaKey.empty() && !m_isQueueMode) {
        // Add separator
        brls::Box* sep = new brls::Box();
        sep->setWidth(376);  // track list width (400) minus padding (24)
        sep->setHeight(1);
        sep->setBackgroundColor(nvgRGBA(255, 255, 255, 40));
        sep->setMarginTop(6);
        sep->setMarginBottom(6);
        trackList->addView(sep);

        brls::Box* searchItem = new brls::Box();
        searchItem->setAxis(brls::Axis::ROW);
        searchItem->setJustifyContent(brls::JustifyContent::FLEX_START);
        searchItem->setAlignItems(brls::AlignItems::CENTER);
        searchItem->setPaddingTop(10);
        searchItem->setPaddingBottom(10);
        searchItem->setPaddingLeft(12);
        searchItem->setPaddingRight(12);
        searchItem->setCornerRadius(4);
        searchItem->setFocusable(true);
        searchItem->setBackgroundColor(nvgRGBA(60, 120, 60, 80));

        brls::Label* searchLabel = new brls::Label();
        searchLabel->setText("Search for Subtitles...");
        searchLabel->setFontSize(16);
        searchLabel->setTextColor(nvgRGB(140, 230, 140));
        searchItem->addView(searchLabel);

        searchItem->registerClickAction([this](brls::View* view) {
            // Defer to next frame to avoid destroying the clicked view
            // while its click handler is still on the call stack
            brls::sync([this]() {
                populateSubtitleSearchResults();
            });
            return true;
        });
        searchItem->addGestureRecognizer(new brls::TapGestureRecognizer(searchItem));
        trackList->addView(searchItem);
    }
}

void PlayerActivity::populateSubtitleSearchResults() {
    if (!trackList || !trackOverlayTitle) return;

    trackOverlayTitle->setText("Searching Subtitles...");

    // Transfer focus to the overlay title before clearing the track list,
    // so the previously focused child can be safely destroyed
    if (trackOverlayTitle) {
        trackOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(trackOverlayTitle);
    }

    trackList->clearViews();

    // Add a loading label
    brls::Label* loadingLabel = new brls::Label();
    loadingLabel->setText("Searching for subtitles...");
    loadingLabel->setFontSize(16);
    loadingLabel->setTextColor(nvgRGB(180, 180, 180));
    loadingLabel->setMargins(12, 12, 12, 12);
    trackList->addView(loadingLabel);

    // Search for subtitles from Plex (queries OpenSubtitles, etc.)
    PlexClient& client = PlexClient::getInstance();
    std::vector<PlexClient::SubtitleResult> results;

    if (!client.searchSubtitles(m_mediaKey, "en", results) || results.empty()) {
        trackList->clearViews();
        trackOverlayTitle->setText("Subtitle Search");

        brls::Label* noResults = new brls::Label();
        noResults->setText("No subtitles found");
        noResults->setFontSize(16);
        noResults->setTextColor(nvgRGB(180, 180, 180));
        noResults->setMargins(12, 12, 12, 12);
        trackList->addView(noResults);

        // Add back button
        brls::Box* backItem = new brls::Box();
        backItem->setAxis(brls::Axis::ROW);
        backItem->setJustifyContent(brls::JustifyContent::FLEX_START);
        backItem->setAlignItems(brls::AlignItems::CENTER);
        backItem->setPaddingTop(10);
        backItem->setPaddingBottom(10);
        backItem->setPaddingLeft(12);
        backItem->setPaddingRight(12);
        backItem->setCornerRadius(4);
        backItem->setFocusable(true);

        brls::Label* backLabel = new brls::Label();
        backLabel->setText("< Back to Subtitles");
        backLabel->setFontSize(16);
        backLabel->setTextColor(nvgRGB(150, 200, 255));
        backItem->addView(backLabel);

        backItem->registerClickAction([this](brls::View* view) {
            brls::sync([this]() {
                populateTrackList(TrackSelectMode::SUBTITLE);
            });
            return true;
        });
        backItem->addGestureRecognizer(new brls::TapGestureRecognizer(backItem));
        trackList->addView(backItem);
        return;
    }

    // Store results for selection
    m_subtitleSearchResults = results;

    // Transfer focus to the overlay title before clearing the track list,
    // so the previously focused child can be safely destroyed
    if (trackOverlayTitle) {
        trackOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(trackOverlayTitle);
    }

    trackList->clearViews();
    trackOverlayTitle->setText("Subtitle Search Results");

    // Back button at top
    brls::Box* backItem = new brls::Box();
    backItem->setAxis(brls::Axis::ROW);
    backItem->setJustifyContent(brls::JustifyContent::FLEX_START);
    backItem->setAlignItems(brls::AlignItems::CENTER);
    backItem->setPaddingTop(10);
    backItem->setPaddingBottom(10);
    backItem->setPaddingLeft(12);
    backItem->setPaddingRight(12);
    backItem->setCornerRadius(4);
    backItem->setFocusable(true);

    brls::Label* backLabel = new brls::Label();
    backLabel->setText("< Back to Subtitles");
    backLabel->setFontSize(16);
    backLabel->setTextColor(nvgRGB(150, 200, 255));
    backItem->addView(backLabel);

    backItem->registerClickAction([this](brls::View* view) {
        brls::sync([this]() {
            populateTrackList(TrackSelectMode::SUBTITLE);
        });
        return true;
    });
    backItem->addGestureRecognizer(new brls::TapGestureRecognizer(backItem));
    trackList->addView(backItem);

    // Add separator
    brls::Box* sep = new brls::Box();
    sep->setWidth(376);  // track list width (400) minus padding (24)
    sep->setHeight(1);
    sep->setBackgroundColor(nvgRGBA(255, 255, 255, 40));
    sep->setMarginTop(4);
    sep->setMarginBottom(4);
    trackList->addView(sep);

    // Show up to 15 results to avoid overflow on Vita's small screen
    size_t maxResults = std::min(results.size(), (size_t)15);
    for (size_t i = 0; i < maxResults; i++) {
        const auto& sub = results[i];

        std::string displayStr = sub.displayTitle;
        if (displayStr.empty()) {
            displayStr = sub.language;
            if (!sub.codec.empty()) displayStr += " [" + sub.codec + "]";
        }
        if (!sub.provider.empty()) {
            displayStr += " (" + sub.provider + ")";
        }

        brls::Box* item = new brls::Box();
        item->setAxis(brls::Axis::ROW);
        item->setJustifyContent(brls::JustifyContent::FLEX_START);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setPaddingTop(10);
        item->setPaddingBottom(10);
        item->setPaddingLeft(12);
        item->setPaddingRight(12);
        item->setCornerRadius(4);
        item->setFocusable(true);

        brls::Label* label = new brls::Label();
        label->setText(displayStr);
        label->setFontSize(14);
        label->setTextColor(nvgRGB(220, 220, 220));
        item->addView(label);

        // Capture key and title by value to avoid referencing vector after potential invalidation
        std::string subKey = sub.key;
        std::string subTitle = sub.displayTitle;
        item->registerClickAction([this, subKey, subTitle](brls::View* view) {
            PlexClient& client = PlexClient::getInstance();
            brls::Logger::debug("Subtitle click: key={}", subKey);
            if (client.selectSearchedSubtitle(m_mediaKey, m_partId, subKey)) {
                MpvPlayer::getInstance().showOSD("Subtitle: " + subTitle, 2.0);
                m_streamsLoaded = false;
            } else {
                MpvPlayer::getInstance().showOSD("Failed to select subtitle", 2.0);
            }
            hideTrackOverlay();
            return true;
        });
        item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
        trackList->addView(item);
    }

    // Give focus to first item in the results list
    if (!trackList->getChildren().empty()) {
        brls::Application::giveFocus(trackList->getChildren()[0]);
    }
    // Reset overlay title focusable state (was set temporarily during list rebuild)
    if (trackOverlayTitle) {
        trackOverlayTitle->setFocusable(false);
    }
}

void PlayerActivity::selectTrack(TrackSelectMode mode, int trackId) {
    MpvPlayer& player = MpvPlayer::getInstance();

    // Check if we have Plex streams - if so, trackId is a Plex stream ID
    bool hasPlexStreams = !m_plexStreams.empty();

    switch (mode) {
        case TrackSelectMode::AUDIO:
            if (hasPlexStreams && m_partId > 0) {
                // trackId is a Plex stream ID - tell Plex server to switch audio
                std::string displayTitle = "Audio track " + std::to_string(trackId);
                for (const auto& ps : m_plexStreams) {
                    if (ps.id == trackId) {
                        displayTitle = ps.displayTitle;
                        break;
                    }
                }
                PlexClient::getInstance().setStreamSelection(m_partId, trackId, -1);
                // Mark the newly selected stream in our cached data
                for (auto& ps : m_plexStreams) {
                    if (ps.streamType == 2) {
                        ps.selected = (ps.id == trackId);
                    }
                }
                // Stop the current transcode and start a new one at the same
                // position so Plex re-muxes with the newly selected audio track.
                // HLS only contains the selected audio, so a reload is needed,
                // but we seek to the current position to avoid restarting.
                {
                    double currentPos = player.getPosition();
                    int offsetMs = m_transcodeBaseOffsetMs + static_cast<int>(currentPos * 1000);
                    PlexClient& client = PlexClient::getInstance();
                    // Stop existing transcode session so Plex doesn't keep
                    // serving old audio segments
                    client.stopTranscode();
                    std::string newUrl;
                    if (client.getTranscodeUrl(m_mediaKey, newUrl, offsetMs)) {
                        brls::Logger::info("selectTrack: Reloading audio at offset={}ms", offsetMs);
                        player.showOSD("Switching: " + displayTitle, 2.0);
                        m_transcodeBaseOffsetMs = offsetMs;
                        player.loadUrl(newUrl, "");
                    }
                }
            } else {
                // Fallback: trackId is an MPV track ID
                player.setAudioTrack(trackId);
                player.showOSD("Audio track " + std::to_string(trackId), 1.5);
            }
            break;

        case TrackSelectMode::SUBTITLE:
            if (trackId < 0) {
                // Disable subtitles
                if (m_partId > 0) {
                    PlexClient::getInstance().setStreamSelection(m_partId, -1, 0);
                }
                // Clear selection in cache
                for (auto& ps : m_plexStreams) {
                    if (ps.streamType == 3 || ps.streamType == 4) ps.selected = false;
                }
                // Reload transcode so Plex stops sending subtitles
                {
                    double currentPos = player.getPosition();
                    int offsetMs = m_transcodeBaseOffsetMs + static_cast<int>(currentPos * 1000);
                    PlexClient& client = PlexClient::getInstance();
                    client.stopTranscode();
                    std::string newUrl;
                    if (client.getTranscodeUrl(m_mediaKey, newUrl, offsetMs)) {
                        brls::Logger::info("selectTrack: Reloading subs off at offset={}ms", offsetMs);
                        player.showOSD("Subtitles off", 2.0);
                        m_transcodeBaseOffsetMs = offsetMs;
                        player.loadUrl(newUrl, "");
                    }
                }
            } else if (hasPlexStreams && m_partId > 0) {
                // trackId is a Plex stream ID - tell Plex server to switch subtitle
                std::string displayTitle = "Subtitle " + std::to_string(trackId);
                for (const auto& ps : m_plexStreams) {
                    if (ps.id == trackId) {
                        displayTitle = ps.displayTitle;
                        break;
                    }
                }
                PlexClient::getInstance().setStreamSelection(m_partId, -1, trackId);
                for (auto& ps : m_plexStreams) {
                    if (ps.streamType == 3 || ps.streamType == 4) {
                        ps.selected = (ps.id == trackId);
                    }
                }
                // Reload transcode so Plex serves the new subtitle stream
                {
                    double currentPos = player.getPosition();
                    int offsetMs = m_transcodeBaseOffsetMs + static_cast<int>(currentPos * 1000);
                    PlexClient& client = PlexClient::getInstance();
                    client.stopTranscode();
                    std::string newUrl;
                    if (client.getTranscodeUrl(m_mediaKey, newUrl, offsetMs)) {
                        brls::Logger::info("selectTrack: Reloading subs at offset={}ms", offsetMs);
                        player.showOSD("Switching: " + displayTitle, 2.0);
                        m_transcodeBaseOffsetMs = offsetMs;
                        player.loadUrl(newUrl, "");
                    }
                }
            } else {
                // Fallback: trackId is an MPV track ID
                player.setSubtitleTrack(trackId);
                player.showOSD("Subtitle track " + std::to_string(trackId), 1.5);
            }
            break;

        case TrackSelectMode::VIDEO:
            player.setVideoTrack(trackId);
            player.showOSD("Video track " + std::to_string(trackId), 1.5);
            break;

        default:
            break;
    }

    hideTrackOverlay();
}

void PlayerActivity::seek(int seconds) {
    MpvPlayer& player = MpvPlayer::getInstance();
    player.seekRelative(seconds);
}

// Queue control methods

void PlayerActivity::playNext() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.playNext()) {
        // Stop current playback
        MpvPlayer::getInstance().stop();
        m_isPlaying = false;

        // Load next track
        loadFromQueue();
    } else {
        brls::Logger::info("PlayerActivity: No next track");
    }
}

void PlayerActivity::playPrevious() {
    if (!m_isQueueMode) return;

    MpvPlayer& player = MpvPlayer::getInstance();

    // If we're more than 3 seconds in, restart current track
    if (player.getPosition() > 3.0) {
        player.seekTo(0);
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.playPrevious()) {
        // Stop current playback
        player.stop();
        m_isPlaying = false;

        // Load previous track
        loadFromQueue();
    } else {
        // Just restart current track
        player.seekTo(0);
    }
}

void PlayerActivity::toggleShuffle() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    queue.setShuffle(!queue.isShuffleEnabled());

    updateQueueDisplay();
    updateShuffleIcon();

    // Show OSD feedback
    MpvPlayer::getInstance().showOSD(
        queue.isShuffleEnabled() ? "Shuffle: ON" : "Shuffle: OFF", 1.5);
}

void PlayerActivity::toggleRepeat() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    queue.cycleRepeatMode();

    updateQueueDisplay();
    updateRepeatIcon();

    // Show OSD feedback
    const char* modeStr = "Repeat: OFF";
    if (queue.getRepeatMode() == RepeatMode::ONE) {
        modeStr = "Repeat: ONE";
    } else if (queue.getRepeatMode() == RepeatMode::ALL) {
        modeStr = "Repeat: ALL";
    }
    MpvPlayer::getInstance().showOSD(modeStr, 1.5);
}

void PlayerActivity::updateShuffleIcon() {
    if (!shuffleIcon) return;
    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.isShuffleEnabled()) {
        shuffleIcon->setImageFromRes("icons/shuffle-variant.png");
    } else {
        shuffleIcon->setImageFromRes("icons/shuffle-disabled.png");
    }
}

void PlayerActivity::updateRepeatIcon() {
    if (!repeatIcon) return;
    MusicQueue& queue = MusicQueue::getInstance();
    switch (queue.getRepeatMode()) {
        case RepeatMode::OFF:
            repeatIcon->setImageFromRes("icons/repeat-off.png");
            break;
        case RepeatMode::ALL:
            repeatIcon->setImageFromRes("icons/repeat.png");
            break;
        case RepeatMode::ONE:
            repeatIcon->setImageFromRes("icons/repeat-once.png");
            break;
    }
}

void PlayerActivity::onTrackEnded(const QueueItem* nextTrack) {
    if (m_destroying) return;

    if (nextTrack) {
        brls::Logger::info("PlayerActivity: Auto-advancing to next track: {}", nextTrack->title);

        // Load the next track
        brls::sync([this]() {
            loadFromQueue();
        });
    } else {
        brls::Logger::info("PlayerActivity: Queue ended, stopping playback");
        brls::sync([this]() {
            // Stop playback but keep player open so user can queue more songs
            m_isPlaying = false;
            updatePlayPauseLabel();
            MpvPlayer::getInstance().showOSD("Queue ended", 2.0);
        });
    }
}

void PlayerActivity::updateQueueDisplay() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();

    if (queueLabel) {
        char queueInfo[64];

        // Build status string
        std::string status;
        if (queue.isShuffleEnabled()) {
            status += " [Shuffle]";
        }
        if (queue.getRepeatMode() == RepeatMode::ONE) {
            status += " [Repeat 1]";
        } else if (queue.getRepeatMode() == RepeatMode::ALL) {
            status += " [Repeat]";
        }

        snprintf(queueInfo, sizeof(queueInfo), "Track %d of %d%s",
                queue.getCurrentIndex() + 1,
                queue.getQueueSize(),
                status.c_str());

        queueLabel->setText(queueInfo);
        queueLabel->setVisibility(brls::Visibility::VISIBLE);
    }

    // Refresh queue list overlay if visible - use lightweight update when
    // only the current track changed (avoids full rebuild for large queues)
    if (m_queueOverlayVisible && queueList) {
        int rowCount = (int)queueList->getChildren().size();
        if (rowCount != queue.getQueueSize()) {
            // Queue size changed - need full rebuild
            populateQueueList();
        } else {
            // Just update highlight on current track rows
            int currentIdx = queue.getCurrentIndex();
            for (auto& pair : m_queueRowData) {
                brls::Box* row = static_cast<brls::Box*>(pair.first);
                bool isCurrent = (pair.second.trackIdx == currentIdx);
                if (isCurrent) {
                    row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
                    row->setBorderColor(nvgRGBA(120, 160, 255, 200));
                    row->setBorderThickness(1.5f);
                } else {
                    row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                    row->setBorderColor(nvgRGBA(0, 0, 0, 0));
                    row->setBorderThickness(0);
                }
            }
            updateQueueTitle();
        }
    }
}

// Queue list overlay methods

void PlayerActivity::showQueueOverlay() {
    if (m_queueOverlayVisible) {
        hideQueueOverlay();
        return;
    }

    m_queueOverlayVisible = true;
    populateQueueList();

    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::VISIBLE);
        queueOverlay->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
            hideQueueOverlay();
            return true;
        });

        // L button = move focused song up in queue (swap + live renumber)
        queueOverlay->registerAction("Move Up", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            if (!queueList) return true;
            auto& children = queueList->getChildren();
            brls::View* focused = brls::Application::getCurrentFocus();
            for (int i = 0; i < (int)children.size(); i++) {
                if (children[i] == focused) {
                    if (i > 0) {
                        MusicQueue& queue = MusicQueue::getInstance();
                        bool shuffled = queue.isShuffleEnabled();
                        const auto& shuffleOrder = queue.getShuffleOrder();
                        int fromIdx = (shuffled && i < (int)shuffleOrder.size()) ? shuffleOrder[i] : i;
                        int toIdx = (shuffled && (i-1) < (int)shuffleOrder.size()) ? shuffleOrder[i-1] : (i-1);
                        queue.moveTrack(fromIdx, toIdx);
                        swapQueueRows(i, i - 1);
                        renumberQueueRows();
                        // Give focus to the moved row at its new position
                        if (i - 1 >= 0 && i - 1 < (int)queueList->getChildren().size()) {
                            brls::Application::giveFocus(queueList->getChildren()[i - 1]);
                        }
                    }
                    break;
                }
            }
            return true;
        });

        // R button = move focused song down in queue (swap + live renumber)
        queueOverlay->registerAction("Move Down", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            if (!queueList) return true;
            auto& children = queueList->getChildren();
            brls::View* focused = brls::Application::getCurrentFocus();
            for (int i = 0; i < (int)children.size(); i++) {
                if (children[i] == focused) {
                    if (i < (int)children.size() - 1) {
                        MusicQueue& queue = MusicQueue::getInstance();
                        bool shuffled = queue.isShuffleEnabled();
                        const auto& shuffleOrder = queue.getShuffleOrder();
                        int fromIdx = (shuffled && i < (int)shuffleOrder.size()) ? shuffleOrder[i] : i;
                        int toIdx = (shuffled && (i+1) < (int)shuffleOrder.size()) ? shuffleOrder[i+1] : (i+1);
                        queue.moveTrack(fromIdx, toIdx);
                        swapQueueRows(i, i + 1);
                        renumberQueueRows();
                        // Give focus to the moved row at its new position
                        if (i + 1 < (int)queueList->getChildren().size()) {
                            brls::Application::giveFocus(queueList->getChildren()[i + 1]);
                        }
                    }
                    break;
                }
            }
            return true;
        });

        // Give focus to the currently playing track in the list
        // When batching is active, focus is deferred until the final batch completes
        if (!m_queueBatchActive) {
            MusicQueue& queue = MusicQueue::getInstance();
            int focusIdx = 0;
            if (queue.isShuffleEnabled()) {
                focusIdx = queue.getShufflePosition();
            } else {
                focusIdx = queue.getCurrentIndex();
            }
            if (queueList && !queueList->getChildren().empty()) {
                focusIdx = std::min(focusIdx, (int)queueList->getChildren().size() - 1);
                if (focusIdx < 0) focusIdx = 0;
                brls::Application::giveFocus(queueList->getChildren()[focusIdx]);
            }
            // Reset overlay title focusable state (was set temporarily during list rebuild)
            if (queueOverlayTitle) {
                queueOverlayTitle->setFocusable(false);
            }
        }
    }
}

void PlayerActivity::hideQueueOverlay() {
    m_queueOverlayVisible = false;
    m_queueBatchActive = false;  // Cancel any in-progress batch
    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::GONE);
    }
    // Restore focus to queue button (fall back to play button if queue button unavailable)
    if (queueBtn && queueBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(queueBtn);
    } else if (m_isQueueMode && musicPlayBtn) {
        brls::Application::giveFocus(musicPlayBtn);
    } else if (playBtn) {
        brls::Application::giveFocus(playBtn);
    }
}

void PlayerActivity::createQueueRow(int displayIdx, int trackIdx, const QueueItem& track, bool isCurrent) {
    PlexClient& client = PlexClient::getInstance();

    // Row container: [cover art] [title + artist] [duration]
    brls::Box* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::FLEX_START);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPaddingTop(7);
    row->setPaddingBottom(7);
    row->setPaddingLeft(12);
    row->setPaddingRight(12);
    row->setCornerRadius(10);
    row->setFocusable(true);
    row->setMarginBottom(3);

    if (isCurrent) {
        row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
        row->setBorderColor(nvgRGBA(120, 160, 255, 200));
        row->setBorderThickness(1.5f);
    } else {
        row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
    }

    // Cover art thumbnail (48x48)
    brls::Image* thumb = new brls::Image();
    thumb->setWidth(48);
    thumb->setHeight(48);
    thumb->setCornerRadius(8);
    thumb->setScalingType(brls::ImageScalingType::FIT);
    thumb->setMarginRight(14);

    // Defer thumbnail loading - will be loaded lazily when row becomes visible
    if (!track.thumb.empty()) {
        std::string thumbUrl = client.getThumbnailUrl(track.thumb, 100, 100);
        m_deferredThumbs.push_back({thumb, thumbUrl, false});
    } else {
        m_deferredThumbs.push_back({thumb, "", true});  // No thumb to load
    }
    row->addView(thumb);

    // Text container: title on top, artist below
    brls::Box* textBox = new brls::Box();
    textBox->setAxis(brls::Axis::COLUMN);
    textBox->setJustifyContent(brls::JustifyContent::CENTER);
    textBox->setGrow(1.0f);

    // Track number + title
    brls::Label* titleLbl = new brls::Label();
    std::string titleStr;
    if (isCurrent) {
        titleStr = "> " + track.title;
    } else {
        char numBuf[8];
        snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdx + 1);
        titleStr = numBuf + track.title;
    }
    titleLbl->setText(titleStr);
    titleLbl->setFontSize(15);
    titleLbl->setTextColor(isCurrent ? nvgRGB(170, 210, 255) : nvgRGB(240, 240, 240));
    textBox->addView(titleLbl);

    // Artist name
    if (!track.artist.empty()) {
        brls::Label* artistLbl = new brls::Label();
        artistLbl->setText(track.artist);
        artistLbl->setFontSize(12);
        artistLbl->setTextColor(isCurrent ? nvgRGBA(170, 210, 255, 180) : nvgRGB(170, 170, 170));
        artistLbl->setMarginTop(2);
        textBox->addView(artistLbl);
    }

    row->addView(textBox);

    // Duration label on the right side
    if (track.duration > 0) {
        brls::Label* durLbl = new brls::Label();
        int durMin = track.duration / 60;
        int durSec = track.duration % 60;
        char durBuf[16];
        snprintf(durBuf, sizeof(durBuf), "%d:%02d", durMin, durSec);
        durLbl->setText(durBuf);
        durLbl->setFontSize(12);
        durLbl->setTextColor(nvgRGB(140, 140, 140));
        durLbl->setMarginLeft(8);
        row->addView(durLbl);
    }

    // Store the track data mapping for this row
    m_queueRowData[row] = {trackIdx, track.title};

    // Swipe left to remove track from queue
    // Handlers look up position dynamically so they stay valid after reordering
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            if (status.state == brls::GestureState::UNSURE || status.state == brls::GestureState::START) {
                float deltaX = status.position.x - status.startPosition.x;
                row->setTranslationX(deltaX);
                float alpha = 1.0f - std::min(1.0f, std::abs(deltaX) / 200.0f);
                row->setAlpha(std::max(0.2f, alpha));
            } else if (status.state == brls::GestureState::END) {
                float deltaX = status.position.x - status.startPosition.x;
                float threshold = 120.0f;
                if (deltaX < -threshold) {
                    // Look up current track index dynamically
                    auto it = m_queueRowData.find(row);
                    if (it != m_queueRowData.end()) {
                        int tIdx = it->second.trackIdx;
                        MusicQueue& queue = MusicQueue::getInstance();
                        if (tIdx != queue.getCurrentIndex()) {
                            int dIdx = findQueueRowDisplayIndex(row);
                            queue.removeTrack(tIdx);
                            if (dIdx >= 0) {
                                brls::sync([this, dIdx]() {
                                    removeQueueRow(dIdx);
                                });
                            }
                        } else {
                            row->setTranslationX(0);
                            row->setAlpha(1.0f);
                        }
                    }
                } else {
                    row->setTranslationX(0);
                    row->setAlpha(1.0f);
                }
            } else if (status.state == brls::GestureState::FAILED) {
                row->setTranslationX(0);
                row->setAlpha(1.0f);
            }
        }, brls::PanAxis::HORIZONTAL));

    // Vertical pan: scroll passthrough OR hold-to-drag reorder
    // When the user touches a row and moves vertically, this gesture fires
    // (which blocks the ScrollingFrame's own scroll). To fix scrolling:
    //  - If hold threshold NOT met: programmatically scroll the ScrollingFrame
    //  - If hold threshold met (finger held still): switch to drag-reorder mode
    // The STAY state is handled so the dragged row follows the finger in real time.
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            float deltaY = status.position.y - status.startPosition.y;

            if (status.state == brls::GestureState::UNSURE) {
                // First touch - record hold start time and initial scroll position
                if (!m_dragState.active && m_dragState.draggedRow != row) {
                    m_dragState.holdStart = std::chrono::steady_clock::now();
                    m_dragState.holdMet = false;
                    m_dragState.draggedRow = row;
                    m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                    m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                    m_dragState.scrollPassthrough = true;
                    m_dragState.initialScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                    m_dragState.dragStartY = 0.0f;
                    auto it = m_queueRowData.find(row);
                    m_dragState.draggedTrackIdx = (it != m_queueRowData.end()) ? it->second.trackIdx : -1;
                }
            } else if (status.state == brls::GestureState::START ||
                       status.state == brls::GestureState::STAY) {
                // Check if we should transition from scroll mode to drag mode
                if (!m_dragState.holdMet) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_dragState.holdStart).count();
                    if (elapsed >= HOLD_THRESHOLD_MS && std::abs(deltaY) < ROW_HEIGHT_PX * 0.5f) {
                        m_dragState.holdMet = true;
                        m_dragState.active = true;
                        m_dragState.scrollPassthrough = false;
                        m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                        m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                        m_dragState.dragStartY = status.position.y;
                        m_dragState.dragStartScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                        // Compute scroll view's absolute screen Y from the row's
                        // known content position and its absolute screen position
                        float rowContentY = m_dragState.originalDisplayIdx * ROW_HEIGHT_PX + 4.0f;
                        m_dragState.scrollViewTop = row->getY() - rowContentY + m_dragState.dragStartScrollY;
                        // Visual feedback: elevate the dragged row
                        row->setBackgroundColor(nvgRGBA(90, 110, 220, 160));
                    }
                }

                if (m_dragState.scrollPassthrough) {
                    if (queueScroll) {
                        // Dead zone: ignore small movements so the list doesn't
                        // jump the instant the finger twitches
                        constexpr float SCROLL_DEAD_ZONE = 12.0f;
                        if (std::abs(deltaY) < SCROLL_DEAD_ZONE)
                            return;

                        // Dampen so scrolling feels more natural on the small screen
                        constexpr float SCROLL_DAMPING = 0.55f;
                        float adjusted = (deltaY > 0)
                            ? (deltaY - SCROLL_DEAD_ZONE) * SCROLL_DAMPING
                            : (deltaY + SCROLL_DEAD_ZONE) * SCROLL_DAMPING;

                        float newOffset = m_dragState.initialScrollY - adjusted;

                        // Clamp to valid range
                        if (newOffset < 0) newOffset = 0;
                        float scrollViewHeight = queueScroll->getHeight();
                        int numRows = queueList ? (int)queueList->getChildren().size() : 0;
                        float contentHeight = numRows * ROW_HEIGHT_PX + 8.0f; // +8 for padding
                        float maxScroll = contentHeight - scrollViewHeight;
                        if (maxScroll < 0) maxScroll = 0;
                        if (newOffset > maxScroll) newOffset = maxScroll;

                        queueScroll->setContentOffsetY(newOffset, false);
                    }
                    return;
                }

                if (!m_dragState.holdMet) return;

                // -- Drag mode: dragged row follows finger --
                float dragDelta = status.position.y - m_dragState.dragStartY;
                float scrollDelta = queueScroll
                    ? (queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY) : 0.0f;

                row->setTranslationY(dragDelta + scrollDelta);
                float effectiveDelta = dragDelta + scrollDelta;

                // Calculate which display position the finger is over
                int origIdx = m_dragState.originalDisplayIdx;
                if (origIdx < 0 || m_dragState.draggedTrackIdx < 0) return;

                MusicQueue& queue = MusicQueue::getInstance();
                int queueSize = queue.getQueueSize();

                // Determine target position based on how many rows the finger crossed
                int rowsOffset = 0;
                if (effectiveDelta > ROW_HEIGHT_PX * 0.5f) {
                    rowsOffset = (int)((effectiveDelta + ROW_HEIGHT_PX * 0.5f) / ROW_HEIGHT_PX);
                } else if (effectiveDelta < -ROW_HEIGHT_PX * 0.5f) {
                    rowsOffset = -(int)((-effectiveDelta + ROW_HEIGHT_PX * 0.5f) / ROW_HEIGHT_PX);
                }
                int newTarget = origIdx + rowsOffset;
                if (newTarget < 0) newTarget = 0;
                if (newTarget >= queueSize) newTarget = queueSize - 1;
                m_dragState.targetDisplayIdx = newTarget;

                // Auto-scroll when the finger is near the top/bottom
                // edge of the scroll view.
                constexpr float AUTO_SCROLL_EDGE = 40.0f;
                constexpr float AUTO_SCROLL_SPEED = 7.0f;
                if (queueScroll && queueList) {
                    float scrollY = queueScroll->getContentOffsetY();
                    float scrollViewHeight = queueScroll->getHeight();
                    float contentHeight = queueSize * ROW_HEIGHT_PX + 8.0f;
                    float maxScroll = contentHeight - scrollViewHeight;
                    if (maxScroll < 0) maxScroll = 0;

                    float fingerInView = status.position.y - m_dragState.scrollViewTop;

                    // DEBUG: log all auto-scroll values
                    brls::Logger::debug("DRAG-AUTO: fingerY={:.0f} svTop={:.0f} "
                        "fingerInView={:.0f} scrollViewH={:.0f} edge={:.0f} "
                        "scrollY={:.0f} origIdx={} target={} queueSize={}",
                        status.position.y, m_dragState.scrollViewTop,
                        fingerInView, scrollViewHeight, AUTO_SCROLL_EDGE,
                        scrollY, origIdx, newTarget, queueSize);

                    if (fingerInView > scrollViewHeight - AUTO_SCROLL_EDGE
                        && scrollY < maxScroll) {
                        // Finger near bottom edge - scroll down
                        float newScroll = scrollY + AUTO_SCROLL_SPEED;
                        if (newScroll > maxScroll) newScroll = maxScroll;
                        brls::Logger::debug("DRAG-AUTO: SCROLLING DOWN newScroll={:.0f} maxScroll={:.0f}", newScroll, maxScroll);
                        queueScroll->setContentOffsetY(newScroll, false);
                    } else if (fingerInView < AUTO_SCROLL_EDGE
                               && scrollY > 0) {
                        // Finger near top edge - scroll up
                        float newScroll = scrollY - AUTO_SCROLL_SPEED;
                        if (newScroll < 0) newScroll = 0;
                        brls::Logger::debug("DRAG-AUTO: SCROLLING UP newScroll={:.0f}", newScroll);
                        queueScroll->setContentOffsetY(newScroll, false);
                    }

                    // Re-read scroll delta after possible auto-scroll
                    scrollDelta = queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY;
                    row->setTranslationY(dragDelta + scrollDelta);
                    effectiveDelta = dragDelta + scrollDelta;
                }

                // Shift displaced rows visually (no data changes yet)
                if (!queueList) return;
                auto& children = queueList->getChildren();
                for (int i = 0; i < (int)children.size(); i++) {
                    if (i == origIdx) continue; // dragged row handled above
                    float shift = 0.0f;
                    if (newTarget > origIdx) {
                        // Dragging down: rows between (origIdx, newTarget] shift up
                        if (i > origIdx && i <= newTarget) {
                            shift = -ROW_HEIGHT_PX;
                        }
                    } else if (newTarget < origIdx) {
                        // Dragging up: rows between [newTarget, origIdx) shift down
                        if (i >= newTarget && i < origIdx) {
                            shift = ROW_HEIGHT_PX;
                        }
                    }
                    children[i]->setTranslationY(shift);
                }
            } else if (status.state == brls::GestureState::END) {
                // Suppress tap/click that fires right after drag ends
                if (m_dragState.holdMet) {
                    m_dragState.justEnded = true;
                }

                // Reset all visual translations
                if (queueList) {
                    auto& children = queueList->getChildren();
                    for (auto* child : children) {
                        child->setTranslationY(0);
                    }
                }

                // Perform the actual queue reorder now
                int origIdx = m_dragState.originalDisplayIdx;
                int targetIdx = m_dragState.targetDisplayIdx;
                if (m_dragState.holdMet && origIdx >= 0 && targetIdx >= 0 &&
                    origIdx != targetIdx && m_dragState.draggedTrackIdx >= 0) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    bool isShuffled = queue.isShuffleEnabled();
                    const auto& sOrder = queue.getShuffleOrder();

                    int toTrackIdx = (isShuffled && targetIdx < (int)sOrder.size())
                                ? sOrder[targetIdx] : targetIdx;
                    queue.moveTrack(m_dragState.draggedTrackIdx, toTrackIdx);

                    // Move the visual row data to match: swap the dragged row
                    // step by step from origIdx to targetIdx
                    int step = (targetIdx > origIdx) ? 1 : -1;
                    for (int i = origIdx; i != targetIdx; i += step) {
                        swapQueueRows(i, i + step);
                    }
                    renumberQueueRows();
                }

                // Restore proper background for dragged row
                auto it = m_queueRowData.find(row);
                if (it != m_queueRowData.end()) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (it->second.trackIdx == queue.getCurrentIndex()) {
                        row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
                    } else {
                        row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                    }
                } else {
                    row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                }

                if (queueList) queueList->invalidate();

                // Reset drag state
                m_dragState.active = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.holdMet = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
                m_dragState.scrollPassthrough = false;
            } else if (status.state == brls::GestureState::FAILED) {
                // Suppress tap/click that fires right after drag ends
                if (m_dragState.holdMet) {
                    m_dragState.justEnded = true;
                }

                // Reset all visual translations
                if (queueList) {
                    auto& children = queueList->getChildren();
                    for (auto* child : children) {
                        child->setTranslationY(0);
                    }
                }

                // Restore background color
                auto it = m_queueRowData.find(row);
                if (it != m_queueRowData.end()) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (it->second.trackIdx == queue.getCurrentIndex()) {
                        row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
                    } else {
                        row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                    }
                } else {
                    row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                }
                m_dragState.active = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.holdMet = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
                m_dragState.scrollPassthrough = false;
            }
        }, brls::PanAxis::VERTICAL));

    // Click handler to play this track - defer to next frame to avoid
    // crash from modifying focus/views while gesture processing is active.
    // Suppress click if a drag just ended (prevents queue from closing after reorder).
    row->registerClickAction([this, row](brls::View* view) {
        if (m_dragState.justEnded) {
            m_dragState.justEnded = false;
            return true;
        }
        auto it = m_queueRowData.find(row);
        if (it != m_queueRowData.end()) {
            int idx = it->second.trackIdx;
            brls::sync([this, idx]() {
                playFromQueue(idx);
            });
        }
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // Lazy-load nearby thumbnails when this row gains focus
    // Use dynamic lookup instead of captured index, since drag reordering
    // changes row positions and would make a captured index stale
    row->getFocusEvent()->subscribe([this, row](brls::View*) {
        int actualIdx = findQueueRowDisplayIndex(row);
        if (actualIdx >= 0) {
            loadQueueThumbsAroundIndex(actualIdx);
        }
    });

    queueList->addView(row);
}

void PlayerActivity::populateQueueList() {
    if (!queueList || !queueOverlayTitle) return;
    if (m_queuePopulating) return;  // Prevent re-entrant calls
    m_queuePopulating = true;

    // Cancel any in-progress batched population
    m_queueBatchActive = false;

    // Transfer focus away from queue items before clearing to avoid destroying focused view
    if (!queueList->getChildren().empty() && queueOverlayTitle) {
        queueOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(queueOverlayTitle);
    }

    // Clear existing items
    m_queueRowData.clear();
    queueList->clearViews();

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    int currentIndex = queue.getCurrentIndex();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    // Calculate total queue duration
    int totalDuration = 0;
    for (const auto& t : tracks) totalDuration += t.duration;
    int totalMin = totalDuration / 60;
    int totalHrs = totalMin / 60;
    totalMin %= 60;

    // Set title with track count and total duration
    char titleBuf[96];
    if (totalHrs > 0) {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%dh %dm)%s",
                 (int)tracks.size(), totalHrs, totalMin, shuffled ? " - Shuffled" : "");
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%d min)%s",
                 (int)tracks.size(), totalMin, shuffled ? " - Shuffled" : "");
    }
    queueOverlayTitle->setText(std::string(titleBuf) + "\nHold & drag to reorder | Swipe left to remove | LB/RB to move");

    // Prepare deferred thumbnail loading - only load covers for visible rows
    m_deferredThumbs.clear();
    m_deferredThumbs.reserve(tracks.size());

    int count = (int)tracks.size();

    // For small queues, create all rows immediately (no batching needed)
    if (count <= QUEUE_BATCH_SIZE) {
        for (int i = 0; i < count; i++) {
            int trackIdx = (shuffled && i < (int)shuffleOrder.size())
                            ? shuffleOrder[i] : i;
            if (trackIdx < 0 || trackIdx >= (int)tracks.size()) continue;
            const QueueItem& track = tracks[trackIdx];
            bool isCurrent = (trackIdx == currentIndex);
            createQueueRow(i, trackIdx, track, isCurrent);
        }

        // Load thumbnails for the initially visible window
        int focusIdx = shuffled ? queue.getShufflePosition() : currentIndex;
        loadQueueThumbsAroundIndex(focusIdx);
        m_queuePopulating = false;
        return;
    }

    // For large queues, snapshot the data and create rows in batches across frames
    m_queueBatchTracks.assign(tracks.begin(), tracks.end());
    m_queueBatchShuffleOrder.assign(shuffleOrder.begin(), shuffleOrder.end());
    m_queueBatchCurrentIndex = currentIndex;
    m_queueBatchShuffled = shuffled;
    m_queueBatchNext = 0;
    m_queueBatchTotal = count;
    m_queueBatchActive = true;

    // Create first batch immediately so the UI isn't empty
    populateQueueBatch();

    m_queuePopulating = false;
}

void PlayerActivity::populateQueueBatch() {
    if (!m_queueBatchActive || !queueList || m_destroying) return;

    int end = std::min(m_queueBatchNext + QUEUE_BATCH_SIZE, m_queueBatchTotal);

    for (int i = m_queueBatchNext; i < end; i++) {
        int trackIdx = (m_queueBatchShuffled && i < (int)m_queueBatchShuffleOrder.size())
                        ? m_queueBatchShuffleOrder[i] : i;
        if (trackIdx < 0 || trackIdx >= (int)m_queueBatchTracks.size()) continue;
        const QueueItem& track = m_queueBatchTracks[trackIdx];
        bool isCurrent = (trackIdx == m_queueBatchCurrentIndex);
        createQueueRow(i, trackIdx, track, isCurrent);
    }

    m_queueBatchNext = end;

    if (m_queueBatchNext >= m_queueBatchTotal) {
        // All rows created - finalize
        m_queueBatchActive = false;
        m_queueBatchTracks.clear();
        m_queueBatchShuffleOrder.clear();

        // Load thumbnails for the initially visible window
        MusicQueue& queue = MusicQueue::getInstance();
        int focusIdx = queue.isShuffleEnabled() ? queue.getShufflePosition() : queue.getCurrentIndex();
        loadQueueThumbsAroundIndex(focusIdx);

        // Give focus to the current track now that all rows exist
        if (m_queueOverlayVisible && queueList && !queueList->getChildren().empty()) {
            focusIdx = std::min(focusIdx, (int)queueList->getChildren().size() - 1);
            if (focusIdx < 0) focusIdx = 0;
            brls::Application::giveFocus(queueList->getChildren()[focusIdx]);
            if (queueOverlayTitle) queueOverlayTitle->setFocusable(false);
        }
    } else {
        // Schedule next batch on the next frame via brls::sync
        brls::sync([this]() {
            populateQueueBatch();
        });
    }
}

void PlayerActivity::loadQueueThumbsAroundIndex(int displayIndex) {
    if (m_deferredThumbs.empty()) return;

    bool wasPaused = ImageLoader::isPaused();
    if (wasPaused) ImageLoader::setPaused(false);

    // Load thumbnails for a window around the given display index
    // Queue scroll is 320px with ~62px rows = ~5 visible rows
    int start = std::max(0, displayIndex - QUEUE_THUMB_BUFFER);
    int end = std::min((int)m_deferredThumbs.size(), displayIndex + QUEUE_THUMB_BUFFER + 6);

    for (int i = start; i < end; i++) {
        auto& dt = m_deferredThumbs[i];
        if (!dt.loaded && !dt.url.empty()) {
            dt.loaded = true;
            ImageLoader::loadAsync(dt.url, [](brls::Image* image) {
                // Thumbnail loaded
            }, dt.image, m_alive);
        }
    }

    if (wasPaused) ImageLoader::setPaused(true);
}

int PlayerActivity::findQueueRowDisplayIndex(brls::View* row) {
    if (!queueList) return -1;
    auto& children = queueList->getChildren();
    for (int i = 0; i < (int)children.size(); i++) {
        if (children[i] == row) return i;
    }
    return -1;
}

void PlayerActivity::swapQueueRows(int displayIdxA, int displayIdxB) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    if (displayIdxA < 0 || displayIdxA >= (int)children.size()) return;
    if (displayIdxB < 0 || displayIdxB >= (int)children.size()) return;
    if (displayIdxA == displayIdxB) return;

    brls::Box* rowA = (brls::Box*)children[displayIdxA];
    brls::Box* rowB = (brls::Box*)children[displayIdxB];

    // Row structure: [thumb(Image)] [textBox(Box)] [durLbl(Label, optional)]
    // textBox children: [titleLbl(Label)] [artistLbl(Label, optional)]
    auto& childrenA = rowA->getChildren();
    auto& childrenB = rowB->getChildren();
    if (childrenA.size() < 2 || childrenB.size() < 2) return;

    MusicQueue& queue = MusicQueue::getInstance();

    // --- Swap QueueRowData between the two rows ---
    auto itA = m_queueRowData.find(rowA);
    auto itB = m_queueRowData.find(rowB);
    if (itA == m_queueRowData.end() || itB == m_queueRowData.end()) return;

    QueueRowData dataA = itA->second;
    QueueRowData dataB = itB->second;
    itA->second = dataB;
    itB->second = dataA;
    // Swap trackIdx back - moveTrack already rearranged the queue array
    // so each display position's trackIdx should stay pointing to its
    // corresponding queue slot (the items swapped in the queue too)
    std::swap(itA->second.trackIdx, itB->second.trackIdx);

    // --- Swap thumbnail images ---
    brls::Image* thumbA = (brls::Image*)childrenA[0];
    brls::Image* thumbB = (brls::Image*)childrenB[0];
    if (displayIdxA < (int)m_deferredThumbs.size() &&
        displayIdxB < (int)m_deferredThumbs.size()) {
        auto& dtA = m_deferredThumbs[displayIdxA];
        auto& dtB = m_deferredThumbs[displayIdxB];
        // Swap deferred thumb entries (url, loaded state)
        std::swap(dtA.url, dtB.url);
        std::swap(dtA.loaded, dtB.loaded);
        // Re-point image pointers to their current rows
        dtA.image = thumbA;
        dtB.image = thumbB;
        // Reload thumbnails to reflect the swap
        if (dtA.loaded && !dtA.url.empty()) {
            ImageLoader::loadAsync(dtA.url, [](brls::Image*) {}, thumbA, m_alive);
        } else {
            thumbA->setImageFromRes("img/default_music.png");
        }
        if (dtB.loaded && !dtB.url.empty()) {
            ImageLoader::loadAsync(dtB.url, [](brls::Image*) {}, thumbB, m_alive);
        } else {
            thumbB->setImageFromRes("img/default_music.png");
        }
    }

    // --- Swap title and artist labels ---
    brls::Box* textBoxA = (brls::Box*)childrenA[1];
    brls::Box* textBoxB = (brls::Box*)childrenB[1];
    auto& textChildrenA = textBoxA->getChildren();
    auto& textChildrenB = textBoxB->getChildren();

    // Determine current-track status after the data swap
    bool isCurrA = (itA->second.trackIdx == queue.getCurrentIndex());
    bool isCurrB = (itB->second.trackIdx == queue.getCurrentIndex());

    // Update title label for row A (now has dataB's content)
    if (!textChildrenA.empty()) {
        brls::Label* titleLblA = (brls::Label*)textChildrenA[0];
        if (isCurrA) {
            titleLblA->setText("> " + itA->second.title);
            titleLblA->setTextColor(nvgRGB(170, 210, 255));
        } else {
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdxA + 1);
            titleLblA->setText(numBuf + itA->second.title);
            titleLblA->setTextColor(nvgRGB(240, 240, 240));
        }
    }
    // Update artist label for row A
    if (textChildrenA.size() >= 2) {
        brls::Label* artistLblA = (brls::Label*)textChildrenA[1];
        // Get the artist from the queue data
        int tIdxA = itA->second.trackIdx;
        if (tIdxA >= 0 && tIdxA < queue.getQueueSize()) {
            const QueueItem& trackA = queue.getQueue()[tIdxA];
            artistLblA->setText(trackA.artist);
            artistLblA->setTextColor(isCurrA ? nvgRGBA(170, 210, 255, 180) : nvgRGB(170, 170, 170));
        }
    }

    // Update title label for row B (now has dataA's content)
    if (!textChildrenB.empty()) {
        brls::Label* titleLblB = (brls::Label*)textChildrenB[0];
        if (isCurrB) {
            titleLblB->setText("> " + itB->second.title);
            titleLblB->setTextColor(nvgRGB(170, 210, 255));
        } else {
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdxB + 1);
            titleLblB->setText(numBuf + itB->second.title);
            titleLblB->setTextColor(nvgRGB(240, 240, 240));
        }
    }
    // Update artist label for row B
    if (textChildrenB.size() >= 2) {
        brls::Label* artistLblB = (brls::Label*)textChildrenB[1];
        int tIdxB = itB->second.trackIdx;
        if (tIdxB >= 0 && tIdxB < queue.getQueueSize()) {
            const QueueItem& trackB = queue.getQueue()[tIdxB];
            artistLblB->setText(trackB.artist);
            artistLblB->setTextColor(isCurrB ? nvgRGBA(170, 210, 255, 180) : nvgRGB(170, 170, 170));
        }
    }

    // --- Swap duration labels using queue data ---
    bool hasDurA = (childrenA.size() >= 3);
    bool hasDurB = (childrenB.size() >= 3);
    if (hasDurA && hasDurB) {
        brls::Label* durA = (brls::Label*)childrenA[2];
        brls::Label* durB = (brls::Label*)childrenB[2];
        // Get durations from queue data (itA/itB already swapped above)
        int tA = itA->second.trackIdx;
        int tB = itB->second.trackIdx;
        auto& tracks = queue.getQueue();
        if (tA >= 0 && tA < (int)tracks.size() && tracks[tA].duration > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d", tracks[tA].duration / 60, tracks[tA].duration % 60);
            durA->setText(buf);
        }
        if (tB >= 0 && tB < (int)tracks.size() && tracks[tB].duration > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d", tracks[tB].duration / 60, tracks[tB].duration % 60);
            durB->setText(buf);
        }
    }

    // --- Swap background/border colors (current track highlighting) ---
    if (isCurrA) {
        rowA->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
        rowA->setBorderColor(nvgRGBA(120, 160, 255, 200));
        rowA->setBorderThickness(1.5f);
    } else {
        rowA->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
        rowA->setBorderColor(nvgRGBA(0, 0, 0, 0));
        rowA->setBorderThickness(0);
    }
    if (isCurrB) {
        rowB->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
        rowB->setBorderColor(nvgRGBA(120, 160, 255, 200));
        rowB->setBorderThickness(1.5f);
    } else {
        rowB->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
        rowB->setBorderColor(nvgRGBA(0, 0, 0, 0));
        rowB->setBorderThickness(0);
    }

    queueList->invalidate();
}

void PlayerActivity::renumberQueueRows() {
    if (!queueList) return;
    MusicQueue& queue = MusicQueue::getInstance();
    auto& children = queueList->getChildren();

    for (int i = 0; i < (int)children.size(); i++) {
        brls::View* child = children[i];
        auto it = m_queueRowData.find(child);
        if (it == m_queueRowData.end()) continue;

        bool isCurr = (it->second.trackIdx == queue.getCurrentIndex());
        const std::string& trackTitle = it->second.title;

        auto& rowChildren = ((brls::Box*)child)->getChildren();
        if (rowChildren.size() >= 2) {
            auto& textBoxChildren = ((brls::Box*)rowChildren[1])->getChildren();
            if (!textBoxChildren.empty()) {
                brls::Label* titleLbl = (brls::Label*)textBoxChildren[0];
                if (isCurr) {
                    titleLbl->setText("> " + trackTitle);
                } else {
                    char numBuf[8];
                    snprintf(numBuf, sizeof(numBuf), "%d. ", i + 1);
                    titleLbl->setText(numBuf + trackTitle);
                }
            }
        }
    }
}

void PlayerActivity::removeQueueRow(int displayIdx) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    if (displayIdx < 0 || displayIdx >= (int)children.size()) return;

    // Remove from track index map
    brls::View* rowToRemove = children[displayIdx];
    m_queueRowData.erase(rowToRemove);

    // If the removed row has focus, transfer focus to a neighbor first
    if (brls::Application::getCurrentFocus() == rowToRemove) {
        if (displayIdx + 1 < (int)children.size()) {
            brls::Application::giveFocus(children[displayIdx + 1]);
        } else if (displayIdx - 1 >= 0) {
            brls::Application::giveFocus(children[displayIdx - 1]);
        } else if (queueOverlayTitle) {
            queueOverlayTitle->setFocusable(true);
            brls::Application::giveFocus(queueOverlayTitle);
        }
    }

    // Remove from deferred thumbnails list
    if (displayIdx < (int)m_deferredThumbs.size()) {
        m_deferredThumbs.erase(m_deferredThumbs.begin() + displayIdx);
    }

    // Remove the view from the list
    queueList->removeView(rowToRemove);

    // Update track index mappings - indices in MusicQueue shifted after removeTrack
    // We need to update any entries that had track indices > the removed one
    // The removeTrack already happened, so indices have been adjusted in the queue
    // Rebuild the map from the queue's current state
    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    auto& remainingChildren = queueList->getChildren();
    for (int i = 0; i < (int)remainingChildren.size(); i++) {
        int trackIdx = (shuffled && i < (int)shuffleOrder.size())
                        ? shuffleOrder[i] : i;
        if (trackIdx >= 0 && trackIdx < (int)tracks.size()) {
            m_queueRowData[remainingChildren[i]] = {trackIdx, tracks[trackIdx].title};
        }
    }

    // Update number labels on remaining rows using stored titles
    for (int i = displayIdx; i < (int)remainingChildren.size(); i++) {
        brls::View* child = remainingChildren[i];
        auto it = m_queueRowData.find(child);
        if (it == m_queueRowData.end()) continue;

        bool isCurr = (it->second.trackIdx == queue.getCurrentIndex());
        const std::string& trackTitle = it->second.title;

        auto& rowChildren = ((brls::Box*)child)->getChildren();
        if (rowChildren.size() >= 2) {
            auto& textBoxChildren = ((brls::Box*)rowChildren[1])->getChildren();
            if (!textBoxChildren.empty()) {
                brls::Label* titleLbl = (brls::Label*)textBoxChildren[0];
                if (isCurr) {
                    titleLbl->setText("> " + trackTitle);
                } else {
                    char numBuf[8];
                    snprintf(numBuf, sizeof(numBuf), "%d. ", i + 1);
                    titleLbl->setText(numBuf + trackTitle);
                }
            }
        }
    }

    // Update title
    updateQueueTitle();
    queueList->invalidate();
}

void PlayerActivity::updateQueueTitle() {
    if (!queueOverlayTitle) return;

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    bool shuffled = queue.isShuffleEnabled();

    int totalDuration = 0;
    for (const auto& t : tracks) totalDuration += t.duration;
    int totalMin = totalDuration / 60;
    int totalHrs = totalMin / 60;
    totalMin %= 60;

    char titleBuf[96];
    if (totalHrs > 0) {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%dh %dm)%s",
                 (int)tracks.size(), totalHrs, totalMin, shuffled ? " - Shuffled" : "");
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%d min)%s",
                 (int)tracks.size(), totalMin, shuffled ? " - Shuffled" : "");
    }
    queueOverlayTitle->setText(std::string(titleBuf) + "\nHold & drag to reorder | Swipe left to remove | LB/RB to move");
}

void PlayerActivity::playFromQueue(int index) {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.playTrack(index)) {
        // Hide queue overlay first (safe - just changes visibility)
        hideQueueOverlay();

        // Stop current playback
        MpvPlayer::getInstance().stop();
        m_isPlaying = false;

        // Load selected track
        loadFromQueue();
    }
}

// Controls visibility toggle (like Suwayomi reader settings show/hide)

void PlayerActivity::updateSkipButton(double positionMs) {
    AppSettings& settings = Application::getInstance().getSettings();

    // Check if we're inside any marker region
    std::string activeType;
    int activeEnd = 0;
    for (const auto& marker : m_markers) {
        if (positionMs >= marker.startTimeMs && positionMs < marker.endTimeMs) {
            activeType = marker.type;
            activeEnd = marker.endTimeMs;
            break;
        }
    }

    if (!activeType.empty()) {
        // We're inside a marker region
        bool isIntro = (activeType == "intro");
        bool autoSkip = isIntro ? settings.autoSkipIntro : settings.autoSkipCredits;
        bool& alreadySkipped = isIntro ? m_introSkipped : m_creditsSkipped;

        if (autoSkip && !alreadySkipped) {
            // Auto-skip: seek to end of marker
            alreadySkipped = true;
            double seekToSec = (activeEnd - m_transcodeBaseOffsetMs) / 1000.0;
            if (seekToSec > 0) {
                MpvPlayer::getInstance().seekTo(seekToSec);
                brls::Logger::info("PlayerActivity: Auto-skipped {} to {}ms", activeType, activeEnd);
                // Hide skip button
                if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
                m_skipButtonVisible = false;
                m_activeMarkerType.clear();
            }
            return;
        }

        // Manual skip mode: show button
        if (m_activeMarkerType != activeType) {
            // Entering a new marker region
            m_activeMarkerType = activeType;
            m_activeMarkerEndMs = activeEnd;
            m_skipButtonShowSeconds = 0;
            m_skipButtonVisible = true;

            if (skipLabel) {
                skipLabel->setText(isIntro ? "Skip Intro" : "Skip Credits");
            }
            if (skipBtn) {
                skipBtn->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            // Still in same marker region
            m_skipButtonShowSeconds++;

            // Auto-hide after 5 seconds if controls are not visible
            if (m_skipButtonShowSeconds >= 5 && !m_controlsVisible && m_skipButtonVisible) {
                m_skipButtonVisible = false;
                if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
            }
        }
    } else {
        // Not in any marker region - hide button
        if (m_skipButtonVisible || !m_activeMarkerType.empty()) {
            m_skipButtonVisible = false;
            m_activeMarkerType.clear();
            if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
        }
    }
}

void PlayerActivity::skipToMarkerEnd() {
    if (m_activeMarkerEndMs <= 0) return;

    double seekToSec = (m_activeMarkerEndMs - m_transcodeBaseOffsetMs) / 1000.0;
    if (seekToSec > 0) {
        MpvPlayer::getInstance().seekTo(seekToSec);
        brls::Logger::info("PlayerActivity: Manually skipped {} to {}ms", m_activeMarkerType, m_activeMarkerEndMs);

        // Mark as skipped to prevent auto-skip re-trigger
        if (m_activeMarkerType == "intro") m_introSkipped = true;
        else if (m_activeMarkerType == "credits") m_creditsSkipped = true;
    }

    // Hide button
    m_skipButtonVisible = false;
    m_activeMarkerType.clear();
    if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
}

void PlayerActivity::toggleControls() {
    if (m_controlsVisible) {
        hideControls();
    } else {
        showControls();
    }
}

void PlayerActivity::resetControlsIdleTimer() {
    m_controlsIdleSeconds = 0;
}

void PlayerActivity::showControls() {
    m_controlsVisible = true;
    resetControlsIdleTimer();
    if (controlsBox) {
        controlsBox->setAlpha(1.0f);
        controlsBox->setVisibility(brls::Visibility::VISIBLE);
    }
    if (centerControls) {
        centerControls->setAlpha(1.0f);
        centerControls->setVisibility(brls::Visibility::VISIBLE);
    }
    if (titleLabel) {
        titleLabel->setVisibility(brls::Visibility::VISIBLE);
    }
    // Re-show skip button if we're still in a marker region
    if (!m_activeMarkerType.empty() && skipBtn) {
        m_skipButtonVisible = true;
        m_skipButtonShowSeconds = 0;  // Reset auto-hide timer
        skipBtn->setVisibility(brls::Visibility::VISIBLE);
    }
}

void PlayerActivity::hideControls() {
    // Don't hide controls for photos or music mode
    if (m_isPhoto || m_isQueueMode) return;

    m_controlsVisible = false;
    if (controlsBox) {
        controlsBox->setAlpha(0.0f);
        controlsBox->setVisibility(brls::Visibility::GONE);
    }
    if (centerControls) {
        centerControls->setAlpha(0.0f);
        centerControls->setVisibility(brls::Visibility::GONE);
    }
}

} // namespace vitaplex
