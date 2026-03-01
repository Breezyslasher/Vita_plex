/**
 * VitaPlex - Player Activity implementation
 */

#include "activity/player_activity.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_queue.hpp"
#include "player/mpv_player.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "view/video_view.hpp"
#include <fstream>
#include <sys/stat.h>

namespace vitaplex {

// Base temp file path for streaming audio (MPV's HTTP handling crashes on Vita)
// Extension will be added dynamically based on the actual file type
static const std::string TEMP_AUDIO_BASE = "ux0:data/vitaplex/temp_stream";

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
            toggleShuffle();
            return true;
        });

        this->registerAction("Repeat", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
            toggleRepeat();
            return true;
        });
    } else {
        // Standard seek for non-queue playback
        this->registerAction("Rewind", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            seek(-10);
            return true;
        });

        this->registerAction("Forward", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            seek(10);
            return true;
        });
    }

    // Start update timer
    m_updateTimer.setCallback([this]() {
        updateProgress();
    });
    m_updateTimer.start(1000); // Update every second
}

void PlayerActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);

    // Re-enable background thumbnail loading now that playback is ending
    ImageLoader::setPaused(false);

    // Mark as destroying to prevent timer and image loader callbacks
    m_destroying = true;
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
        if (position > 0) {
            int timeMs = (int)(position * 1000);

            if (m_isLocalFile) {
                // Save progress for downloaded media
                DownloadsManager::getInstance().updateProgress(m_mediaKey, timeMs);
                DownloadsManager::getInstance().saveState();
                brls::Logger::info("PlayerActivity: Saved local progress {}ms for {}", timeMs, m_mediaKey);
            } else if (!m_isQueueMode && !m_mediaKey.empty()) {
                // Save progress to Plex server (not for queue mode - tracks change)
                PlexClient::getInstance().updatePlayProgress(m_mediaKey, timeMs);
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

    // Update display
    if (titleLabel) {
        titleLabel->setText(track->title);
    }
    if (artistLabel) {
        artistLabel->setText(track->artist);
    }

    // Update queue info display
    updateQueueDisplay();

    // Load album art - temporarily unpause the image loader for this one load
    if (albumArt && !track->thumb.empty()) {
        PlexClient& client = PlexClient::getInstance();
        std::string thumbUrl = client.getThumbnailUrl(track->thumb, 300, 300);
        bool wasPaused = ImageLoader::isPaused();
        if (wasPaused) ImageLoader::setPaused(false);
        ImageLoader::loadAsync(thumbUrl, [](brls::Image* image) {
            // Art loaded
        }, albumArt, m_alive);
        if (wasPaused) ImageLoader::setPaused(true);
        albumArt->setVisibility(brls::Visibility::VISIBLE);
    }

    // Use the rating key to get transcode URL
    m_mediaKey = track->ratingKey;
    PlexClient& client = PlexClient::getInstance();
    std::string url;

    if (!client.getTranscodeUrl(track->ratingKey, url, 0)) {
        brls::Logger::error("Failed to get transcode URL for track: {}", track->ratingKey);
        m_loadingMedia = false;
        return;
    }

    // Pause image loading and free cache to reclaim memory for MPV.
    ImageLoader::setPaused(true);
    ImageLoader::cancelAll();
    ImageLoader::clearCache();

    MpvPlayer& player = MpvPlayer::getInstance();

    // Set audio-only mode BEFORE initializing
    player.setAudioOnly(true);

    // Download audio to local file (HTTP workaround for Vita)
    // This uses HttpClient (libcurl), not MPV, so it's safe during activity transition.
    std::string playUrl = url;

    if (url.find("http://") == 0) {
        brls::Logger::info("PlayerActivity: Downloading audio stream to local file...");

        // Show loading message
        if (titleLabel) {
            titleLabel->setText("Loading audio...");
        }

        // Extract file extension from URL
        std::string ext = ".mp3";
        size_t queryPos = url.find('?');
        std::string urlPath = (queryPos != std::string::npos) ? url.substr(0, queryPos) : url;
        size_t dotPos = urlPath.rfind('.');
        if (dotPos != std::string::npos) {
            ext = urlPath.substr(dotPos);
        }

        std::string tempPath = TEMP_AUDIO_BASE + ext;

        std::ofstream tempFile(tempPath, std::ios::binary);
        if (!tempFile.is_open()) {
            brls::Logger::error("Failed to create temp file: {}", tempPath);
            m_loadingMedia = false;
            return;
        }

        int64_t totalBytes = 0;
        int64_t downloadedBytes = 0;
        int lastProgressPercent = -1;

        HttpClient httpClient;
        bool downloadSuccess = httpClient.downloadFile(url,
            [&tempFile, &downloadedBytes, &totalBytes, &lastProgressPercent, this](const char* data, size_t size) -> bool {
                tempFile.write(data, size);
                downloadedBytes += size;

                if (totalBytes > 0) {
                    int percent = (int)((downloadedBytes * 100) / totalBytes);
                    if (percent != lastProgressPercent && titleLabel) {
                        lastProgressPercent = percent;
                        char progressText[64];
                        snprintf(progressText, sizeof(progressText), "Loading audio... %d%%", percent);
                        brls::sync([this, progressText]() {
                            if (titleLabel) titleLabel->setText(progressText);
                        });
                    }
                }

                return tempFile.good();
            },
            [&totalBytes](int64_t size) {
                totalBytes = size;
            }
        );

        tempFile.close();

        if (!downloadSuccess) {
            brls::Logger::error("Failed to download audio stream");
            m_loadingMedia = false;
            return;
        }

        // Restore title after download
        if (titleLabel) {
            titleLabel->setText(track->title);
        }

        brls::Logger::info("PlayerActivity: Audio downloaded ({} bytes)", downloadedBytes);
        playUrl = tempPath;
    }

    if (!player.isInitialized()) {
        // Defer MPV init + load to after activity transition completes
        m_pendingPlayUrl = playUrl;
        m_pendingPlayTitle = track->title;
        m_pendingIsAudio = true;
        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Player already initialized (track change) - load immediately
    if (!player.loadUrl(playUrl, track->title)) {
        brls::Logger::error("Failed to load URL: {}", playUrl);
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

        // Extract filename from path
        size_t lastSlash = m_directFilePath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos)
            ? m_directFilePath.substr(lastSlash + 1)
            : m_directFilePath;

        if (titleLabel) {
            titleLabel->setText(filename);
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
            m_pendingPlayTitle = "Test File";
            m_pendingIsAudio = isAudioFile;
            m_loadingMedia = false;
            return;
        }

        // Player already initialized - load immediately
        if (!player.loadUrl(m_directFilePath, "Test File")) {
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
        DownloadItem* download = downloads.getDownload(m_mediaKey);

        if (!download || download->state != DownloadState::COMPLETED) {
            brls::Logger::error("PlayerActivity: Downloaded media not found or incomplete");
            m_loadingMedia = false;
            return;
        }

        brls::Logger::info("PlayerActivity: Playing local file: {}", download->localPath);

        if (titleLabel) {
            std::string title = download->title;
            if (!download->parentTitle.empty()) {
                title = download->parentTitle + " - " + download->title;
            }
            titleLabel->setText(title);
        }

        // Pause image loading and free cache to reclaim memory for MPV
        ImageLoader::setPaused(true);
        ImageLoader::cancelAll();
        ImageLoader::clearCache();

        MpvPlayer& player = MpvPlayer::getInstance();

        // Resume from saved viewOffset
        if (download->viewOffset > 0) {
            m_pendingSeek = download->viewOffset / 1000.0;
        }

        if (!player.isInitialized()) {
            // Defer MPV init + load to after activity transition completes
            m_pendingPlayUrl = download->localPath;
            m_pendingPlayTitle = download->title;
            m_pendingIsAudio = false;
            m_loadingMedia = false;
            return;
        }

        // Player already initialized - load immediately
        if (!player.loadUrl(download->localPath, download->title)) {
            brls::Logger::error("Failed to load local file: {}", download->localPath);
            m_loadingMedia = false;
            return;
        }

        // Show video view
        if (videoView) {
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

        // Get transcode URL for video/audio (forces Plex to convert to Vita-compatible format)
        std::string url;
        if (client.getTranscodeUrl(m_mediaKey, url, item.viewOffset)) {
            // Pause image loading and free cache memory before initializing MPV.
            // This stops background thumbnail fetches from competing with media
            // streaming, and frees memory (Vita only has 256MB).
            ImageLoader::setPaused(true);
            ImageLoader::cancelAll();
            ImageLoader::clearCache();

            MpvPlayer& player = MpvPlayer::getInstance();

            // Set audio-only mode BEFORE initializing
            player.setAudioOnly(isAudioContent);

            // MPV's HTTP handling crashes on Vita when loading network URLs directly.
            // Workaround: Download audio to local file first, then play the local file.
            // This uses libcurl (via HttpClient) which handles HTTP correctly on Vita.
            std::string playUrl = url;

            if (isAudioContent && url.find("http://") == 0) {
                brls::Logger::info("PlayerActivity: Downloading audio stream to local file (HTTP workaround)...");

                // Show loading message in title
                if (titleLabel) {
                    titleLabel->setText("Loading audio...");
                }

                // Extract file extension from URL (e.g., .mp3, .m4a, .ogg, .flac)
                std::string ext = ".mp3";  // Default extension
                size_t queryPos = url.find('?');
                std::string urlPath = (queryPos != std::string::npos) ? url.substr(0, queryPos) : url;
                size_t dotPos = urlPath.rfind('.');
                if (dotPos != std::string::npos) {
                    ext = urlPath.substr(dotPos);
                }

                // Build temp file path with correct extension
                std::string tempPath = TEMP_AUDIO_BASE + ext;

                // Open temp file for writing
                std::ofstream tempFile(tempPath, std::ios::binary);
                if (!tempFile.is_open()) {
                    brls::Logger::error("Failed to create temp file: {}", tempPath);
                    if (titleLabel) titleLabel->setText("Error: Cannot create temp file");
                    m_loadingMedia = false;
                    return;
                }

                // Track download progress
                int64_t totalBytes = 0;
                int64_t downloadedBytes = 0;
                int lastProgressPercent = -1;

                // Download the stream with progress updates
                HttpClient httpClient;
                bool downloadSuccess = httpClient.downloadFile(url,
                    [&tempFile, &downloadedBytes, &totalBytes, &lastProgressPercent, this](const char* data, size_t size) -> bool {
                        tempFile.write(data, size);
                        downloadedBytes += size;

                        // Update progress display (only when percentage changes to reduce overhead)
                        if (totalBytes > 0) {
                            int percent = (int)((downloadedBytes * 100) / totalBytes);
                            if (percent != lastProgressPercent && titleLabel) {
                                lastProgressPercent = percent;
                                char progressText[64];
                                snprintf(progressText, sizeof(progressText), "Loading audio... %d%%", percent);
                                brls::sync([this, progressText]() {
                                    if (titleLabel) titleLabel->setText(progressText);
                                });
                            }
                        }

                        return tempFile.good();
                    },
                    [&totalBytes](int64_t size) {
                        totalBytes = size;
                    }
                );

                tempFile.close();

                if (!downloadSuccess) {
                    brls::Logger::error("Failed to download audio stream");
                    if (titleLabel) titleLabel->setText("Error: Download failed");
                    m_loadingMedia = false;
                    return;
                }

                // Restore title after download
                if (titleLabel) {
                    titleLabel->setText(item.title);
                }

                brls::Logger::info("PlayerActivity: Audio downloaded ({} bytes), playing local file", downloadedBytes);
                playUrl = tempPath;
            }

            if (!player.isInitialized()) {
                // Defer MPV init + load to after activity transition completes.
                // initRenderContext() creates GXM resources (framebuffer, render target)
                // and loadUrl() spawns decoder threads that use the shared GXM context
                // via hwdec=vita-copy. Both conflict with NanoVG drawing during the
                // borealis activity show phase, causing a consistent SIGSEGV.
                brls::Logger::info("PlayerActivity: Deferring MPV init to after activity transition");
                m_pendingPlayUrl = playUrl;
                m_pendingPlayTitle = item.title;
                m_pendingIsAudio = isAudioContent;
            } else {
                // Player already initialized (e.g., mode didn't change) - load immediately
                brls::Logger::debug("PlayerActivity: Calling player.loadUrl...");
                if (!player.loadUrl(playUrl, item.title)) {
                    brls::Logger::error("Failed to load URL: {}", playUrl);
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

#ifdef __vita__
            // Flush GXM pipeline before loadfile to ensure NanoVG's previous
            // frame is fully retired from the GPU before the decoder starts.
            MpvPlayer::flushGpu();
#endif

            if (player.loadUrl(url, title)) {
                if (videoView && !isAudio) {
                    videoView->setVisibility(brls::Visibility::VISIBLE);
                    videoView->setVideoVisible(true);
                    brls::Logger::debug("Video view enabled (deferred)");
                }
                m_isPlaying = true;
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

        if (m_isQueueMode) {
            // Notify queue that track ended - it will call onTrackEnded
            MusicQueue::getInstance().onTrackEnded();
        } else {
            PlexClient::getInstance().markAsWatched(m_mediaKey);
            brls::Application::popActivity();
        }
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

    // Show OSD feedback
    MpvPlayer::getInstance().showOSD(
        queue.isShuffleEnabled() ? "Shuffle: ON" : "Shuffle: OFF", 1.5);
}

void PlayerActivity::toggleRepeat() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    queue.cycleRepeatMode();

    updateQueueDisplay();

    // Show OSD feedback
    const char* modeStr = "Repeat: OFF";
    if (queue.getRepeatMode() == RepeatMode::ONE) {
        modeStr = "Repeat: ONE";
    } else if (queue.getRepeatMode() == RepeatMode::ALL) {
        modeStr = "Repeat: ALL";
    }
    MpvPlayer::getInstance().showOSD(modeStr, 1.5);
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
        brls::Logger::info("PlayerActivity: Queue ended, closing player");
        brls::sync([]() {
            brls::Application::popActivity();
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
}

} // namespace vitaplex
