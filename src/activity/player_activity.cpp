// VitaPlex - Player Activity implementation

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_queue.hpp"
#include "app/music_controller.hpp"
#include "utils/now_playing.hpp"
#include "app/plex_palette.hpp"
#include "app/synclounge_session.hpp"
#include "player/mpv_player.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "utils/media_keys.hpp"
#include "utils/pip.h"
#include "view/video_view.hpp"
#include "platform/platform.hpp"
#if defined(__APPLE__)
// TARGET_OS_IOS, for the Auto branch of useMobileLayout().
#include <TargetConditionals.h>
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <fstream>
#include <sys/stat.h>

#ifdef __vita__
#include <psp2/power.h>
#endif

namespace vitaplex {

// Base temp path for streamed audio (mpv's HTTP handling crashes on Vita); extension added from the real file type.


std::atomic<bool> PlayerActivity::s_active{false};

bool PlayerActivity::isActive() {
    return s_active.load();
}

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

PlayerActivity* PlayerActivity::createForStream(const std::string& streamUrl, const std::string& title,
                                                const std::string& liveSessionUuid) {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isDirectFile = true;  // Use direct file path for stream URLs too
    activity->m_directFilePath = streamUrl;
    activity->m_streamTitle = title;
    activity->m_liveSessionUuid = liveSessionUuid;
    brls::Logger::info("PlayerActivity created for stream: {} ({}){}",
                       title, streamUrl,
                       liveSessionUuid.empty() ? "" : " [live]");
    return activity;
}

PlayerActivity* PlayerActivity::createWithQueue(const std::vector<MediaItem>& tracks, int startIndex,
                                                bool userPickedTrack,
                                                const std::string& playlistId) {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isQueueMode = true;

    MusicQueue& queue = MusicQueue::getInstance();

    // Try a server-side play queue when online, from the parent ratingKey (album/season) or the first track's.
    bool serverOk = false;
    // Whether the server already shuffled this queue; shuffling again would override an authoritative order.
    bool serverShuffled = false;
    if (!tracks.empty() && !PlexClient::getInstance().getServerUrl().empty()) {
        PlexClient& client = PlexClient::getInstance();
        PlexClient::PlayQueueContainer pq;

        // Determine queue type
        std::string queueType = "audio";
        if (!tracks.empty() && (tracks[0].mediaType == MediaType::EPISODE ||
                                 tracks[0].mediaType == MediaType::MOVIE)) {
            queueType = "video";
        }

        // Build URI from parent ratingKey (album/season) if available, otherwise from first track
        std::string uri;
        if (!tracks[0].parentRatingKey.empty()) {
            uri = client.buildPlayQueueDirectoryURI(tracks[0].parentRatingKey);
        } else {
            // No container to name: right for a single track, a lie for several, which the check below catches.
            uri = client.buildPlayQueueURI(tracks[0].ratingKey);
        }

        std::string startKey = (startIndex >= 0 && startIndex < (int)tracks.size())
            ? tracks[startIndex].ratingKey : "";

        // An accepted but empty queue is not usable, and a playlist is named by playlistID, not a URI.
        bool created = false;
        if (!playlistId.empty()) {
            int pid = 0;
            try { pid = std::stoi(playlistId); } catch (...) { pid = 0; }
            if (pid > 0) {
                created = client.createPlayQueueFromPlaylist(pid, queueType, pq, 0, startKey)
                          && !pq.items.empty();
            }
        }
        if (!created && playlistId.empty()) {
            created = client.createPlayQueue(uri, queueType, pq, startKey) && !pq.items.empty();
        }

        // Reject a server queue holding less than was asked for; Plex sends a window, so count what arrived.
        const int serverCount = (int)pq.items.size();
        serverOk = created && serverCount >= (int)tracks.size();
        if (created && !serverOk) {
            brls::Logger::warning(
                "PlayerActivity: server play queue {} came back with {} item(s) for a "
                "{}-track request — keeping the client-side queue",
                pq.playQueueID, serverCount, tracks.size());
        }
        if (serverOk) {
            queue.setFromPlayQueue(pq, pq.playQueueShuffled);
            serverShuffled = pq.playQueueShuffled;
            brls::Logger::info("PlayerActivity: Server play queue {} created ({} items)",
                               pq.playQueueID, pq.playQueueTotalCount);
        }
    }

    if (!serverOk) {
        // Offline or server failed - use client-side queue
        queue.setQueue(tracks, startIndex);
    }

    // "Shuffle New Queues": shuffle a fresh music queue, pinning the current track only if the user picked one.
    if (!tracks.empty() && tracks[0].mediaType == MediaType::MUSIC_TRACK &&
        Application::getInstance().getSettings().musicShuffleDefault &&
        !serverShuffled) {
        if (userPickedTrack) {
            queue.shuffleKeepingCurrent();
            brls::Logger::info("PlayerActivity: shuffle on by default, keeping the picked track first");
        } else {
            queue.shuffleFromStart();
            brls::Logger::info("PlayerActivity: shuffle on by default, opening on a random track");
        }
    }

    // Hand transport to MusicController while this player lives; on leave it takes over headlessly.
    MusicController::getInstance().attachForeground({
        [activity]() { activity->playNext(); },
        [activity]() { activity->playPrevious(); },
        [activity](const QueueItem* nextTrack) { activity->onTrackEnded(nextTrack); },
        [activity](bool on) { activity->setShuffleFromOs(on); },
        [activity](RepeatMode m) { activity->setRepeatFromOs(m); },
        [activity](int index) { activity->playFromQueue(index); }
    });

    brls::Logger::info("PlayerActivity created with queue of {} tracks, starting at {} (server={})",
                      tracks.size(), startIndex, serverOk);
    return activity;
}

PlayerActivity* PlayerActivity::createResumeQueue() {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isQueueMode = true;
    activity->m_isResuming = true;  // Don't restart playback

    // Resume existing queue - don't reset it
    MusicQueue& queue = MusicQueue::getInstance();

    // Reclaim transport from the (possibly headless) MusicController.
    MusicController::getInstance().attachForeground({
        [activity]() { activity->playNext(); },
        [activity]() { activity->playPrevious(); },
        [activity](const QueueItem* nextTrack) { activity->onTrackEnded(nextTrack); },
        [activity](bool on) { activity->setShuffleFromOs(on); },
        [activity](RepeatMode m) { activity->setRepeatFromOs(m); },
        [activity](int index) { activity->playFromQueue(index); }
    });

    brls::Logger::info("PlayerActivity resumed existing queue at index {}", queue.getCurrentIndex());
    return activity;
}

// borealis fatal()s on a navigation route to a non-focusable view, so skip rather than die.
static void routeIfFocusable(brls::View* v, brls::FocusDirection dir, brls::View* target) {
    if (v && v->isFocusable()) v->setCustomNavigationRoute(dir, target);
}

bool PlayerActivity::useMobileLayout() const {
    // Video always takes the classic player; the mobile layout is a music-only Now Playing screen.
    if (!m_isQueueMode || !MusicQueue::getInstance().isMusicQueue()) return false;

    switch (Application::getInstance().getSettings().playerLayout) {
        case 1: return false;   // Classic, everywhere
        case 2: return true;    // Mobile, everywhere — including handheld and TV
        default: break;         // Auto
    }
    // Auto: big-art suits phone-shaped screens. Width, not platform, so tablets and resized windows fit too.
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS)
    // platform::viewport* is what the rest of the player sizes from, and it is defined on every port.
    const float vw = platform::viewportWidth();
    const float vh = platform::viewportHeight();
    if (vw <= 0.0f || vh <= 0.0f) return false;   // unknown: leave it classic
    if (vh > vw) return true;                     // portrait phone/tablet
    return vw < 600.0f;
#else
    return false;   // PSV / PS4 / Switch / desktop keep the classic player
#endif
}

// The landscape OSD over video. Separate from useMobileLayout(): wanting one design says nothing about the other.
bool PlayerActivity::useVideoOsd() const {
    // Music picks its own mobile layout above; everything else reaching this player is video.
    if (m_isQueueMode && MusicQueue::getInstance().isMusicQueue()) return false;

    switch (Application::getInstance().getSettings().videoPlayerLayout) {
        case 1: return false;   // Classic, everywhere
        case 2: return true;    // Mobile, everywhere -- including handheld and TV
        default: break;         // Auto
    }
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS)
    const float vw = platform::viewportWidth();
    const float vh = platform::viewportHeight();
    if (vw <= 0.0f || vh <= 0.0f) return false;   // unknown: leave it classic
    // Short edge, not orientation: a phone suits touch controls either way, a tablet has room for the classic ones.
    return std::min(vw, vh) < 600.0f;
#else
    return false;   // PSV / PS4 / Switch / desktop keep the classic controls
#endif
}

// Photos and music keep controls up, but m_isQueueMode also covers single videos, which get an OSD.
bool PlayerActivity::controlsCanHide() const {
    return !m_isPhoto && (!m_isQueueMode || m_videoOsd);
}

brls::View* PlayerActivity::createContentView() {
    // All three layouts declare the same view ids, so only geometry differs and this class stays layout-agnostic.
    m_mobileLayout = useMobileLayout();
    m_videoOsd     = !m_mobileLayout && useVideoOsd();
    const bool mobileXml = m_mobileLayout || m_videoOsd;
    brls::Logger::info("PlayerActivity: using the {} player layout (queue={} music={})",
                       m_mobileLayout ? "mobile music" : m_videoOsd ? "mobile video" : "classic",
                       m_isQueueMode, MusicQueue::getInstance().isMusicQueue());
    return brls::View::createFromXMLResource(
        mobileXml ? "activity/player_mobile.xml" : "activity/player.xml");
}

void PlayerActivity::onContentAvailable() {
    brls::Logger::debug("PlayerActivity content available");

    // CENTERED so D-pad up/down always move focus; the default NATURAL cannot reach a row that is off screen.
    if (queueScroll)
        queueScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    // On screen now, so suppress the SyncLounge auto-join prompt; the in-player auto-load follows content instead.
    s_active.store(true);

#ifdef __vita__
    // Boost CPU/GPU clocks to max for smooth media playback
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif

    // Free bandwidth for the stream; setPaused comes later, once music has its album art.
    ImageLoader::cancelAll();

    // Non-queue playback stops background music first, with a "stopped" timeline so the server clears the session.
    if (!m_isQueueMode) {
        MusicQueue& existingQueue = MusicQueue::getInstance();
        if (!existingQueue.isEmpty()) {
            brls::Logger::info("PlayerActivity: Stopping background music for video playback");

            // Report stopped timeline for the current music track
            const QueueItem* track = existingQueue.getCurrentTrack();
            if (track && !track->ratingKey.empty()) {
                std::string key = "/library/metadata/" + track->ratingKey;
                int pqItemID = track->playQueueItemID;
                PlexClient::getInstance().reportTimeline(
                    track->ratingKey, key, "stopped", 0, track->duration * 1000, pqItemID);
            }

            MpvPlayer::getInstance().stop();
            existingQueue.clear();
        }
    }

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
            // Watch party: only the host may scrub, since the 1s tick snaps a follower's thumb back.
            {
                auto& sl = SyncLoungeSession::instance();
                if (sl.isConnected() && !sl.isHost()) {
                    MpvPlayer::getInstance().showOSD("Only the host can seek", 1.5);
                    return;
                }
            }
            // Seek to position
            MpvPlayer& player = MpvPlayer::getInstance();
            double duration = 0.0;
            // Prefer Plex's duration over mpv's in queue mode; mpv may only know the demuxed portion.
            if (m_isQueueMode) {
                const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
                if (track && track->duration > 0)
                    duration = (double)track->duration;
            }
            if (duration <= 0)
                duration = player.getDuration();
            // Direct play, local files and music seek locally; a transcoded video takes the debounced path.
            if (m_isQueueMode && !m_isLocalFile) {
                // A streamed music transcode cannot seek in place, so MusicController restarts it at the target.
                double absDuration = m_transcodeBaseOffsetMs / 1000.0 + duration;
                MusicController::getInstance().seekToMs(
                    (long long)(std::max(0.0, absDuration * progress) * 1000.0));
            } else if (m_isLocalFile || m_isDirectFile || m_isQueueMode || m_directPlay) {
                double baseOffsetSec = m_transcodeBaseOffsetMs / 1000.0;
                double absDuration = baseOffsetSec + duration;
                player.seekTo(std::max(0.0, absDuration * progress - baseOffsetSec));
            } else {
                requestTranscodeSeek(progress * knownDurationMs());
            }
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
                    // Scaled: unscaled this is ~19dp on mobile, barely past the recogniser's slop, so a nudge skipped a track.
                    float threshold = ui(60.0f);
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

    // A/OK raises a hidden OSD; with it up the focused control answers first.
    this->registerAction("Play/Pause", brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        resetControlsIdleTimer();
        if (!m_controlsVisible && controlsCanHide()) {
            showControls();
            return true;
        }
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
        if (m_lyricsOverlayVisible) {
            hideLyricsOverlay();
            return true;
        }
        // OSD up: close it first. Photo and music are excluded — hideControls() is a no-op there, so Back would trap them.
        if (m_controlsVisible && controlsCanHide()) {
            hideControls();
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

    // Android TV's Menu key arrives as GUIDE, so re-dispatch it as START to open the OSD.
    this->registerAction("", brls::ControllerButton::BUTTON_GUIDE, [](brls::View*) {
        brls::View* v = brls::Application::getCurrentFocus();
        while (v) {
            for (auto& a : v->getActions()) {
                if (a->getType() == brls::ActionType::ACTION_GAMEPAD &&
                    a->getButton() == brls::ControllerButton::BUTTON_START &&
                    a->isAvailable()) {
                    if (a->getActionListener()(v)) return true;
                }
            }
            v = v->getParent();
        }
        return true;
    });

    // PiP on right-stick click and an OSD button, video only, and only where PiP is implemented.
    if (!m_isQueueMode && !m_isPhoto && pip::isAvailable()) {
        auto pipHandler = [this](brls::View* view) {
            auto& player = MpvPlayer::getInstance();
            int vw = player.getVideoWidth();
            int vh = player.getVideoHeight();
            if (vw <= 0 || vh <= 0) {
                vw = 16;
                vh = 9;
            }
            pip::toggle(vw, vh);
            return true;
        };
        this->registerAction("Picture-in-Picture", brls::ControllerButton::BUTTON_RSB, pipHandler);
        if (pipBtn) {
            pipBtn->setVisibility(brls::Visibility::VISIBLE);
            pipBtn->registerClickAction(pipHandler);
            pipBtn->addGestureRecognizer(new brls::TapGestureRecognizer(pipBtn));
        }
    }

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

        // D-pad seeks while the OSD is hidden; when visible, reset the idle timer and let navigation through.
        this->registerAction("Seek Back", brls::ControllerButton::BUTTON_LEFT, [this](brls::View* view) {
            if (!m_controlsVisible) {
                resetControlsIdleTimer();
                int interval = Application::getInstance().getSettings().seekInterval;
                seek(-interval);
                return true;
            }
            resetControlsIdleTimer();
            return false;  // Let D-pad navigation handle it
        });

        this->registerAction("Seek Forward", brls::ControllerButton::BUTTON_RIGHT, [this](brls::View* view) {
            if (!m_controlsVisible) {
                resetControlsIdleTimer();
                int interval = Application::getInstance().getSettings().seekInterval;
                seek(interval);
                return true;
            }
            resetControlsIdleTimer();
            return false;  // Let D-pad navigation handle it
        });

    }

    // D-pad up/down summons a hidden OSD, else resets the idle timer so focus can move without a timeout.
    this->registerAction("Show Controls", brls::ControllerButton::BUTTON_UP, [this](brls::View* view) {
        if (!m_controlsVisible) {
            showControls();
            return true;
        }
        resetControlsIdleTimer();
        return false;
    });

    this->registerAction("Show Controls", brls::ControllerButton::BUTTON_DOWN, [this](brls::View* view) {
        if (!m_controlsVisible) {
            showControls();
            return true;
        }
        resetControlsIdleTimer();
        return false;
    });

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

    // Scrim only, so taps on the sheet's rows and header buttons do not dismiss the overlay.
    if (queueScrim) {
        queueScrim->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    hideQueueOverlay();
                }
            }));
    }

    if (lyricsScrim) {
        lyricsScrim->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    hideLyricsOverlay();
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

        // Resize the cover for the current viewport and keep it right across rotations.
        applyMusicLayoutForViewport();
        std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
        platform::onOrientationChanged([this, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;
            applyMusicLayoutForViewport();
        });

        wireMobileSheet();


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

        // In music mode: disable focusability on ALL hidden buttons so focus navigation skips them entirely
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

        // Lyrics: the track picker always listed them, but music had no button to open it with.
        if (lyricsBtn) {
            lyricsBtn->setVisibility(brls::Visibility::VISIBLE);
            lyricsBtn->setFocusable(true);
            setIconRes(lyricsIcon, "icons/subtitles.png");
            lyricsBtn->registerClickAction([this](brls::View* view) {
                // One lyrics file needs no picker; the picker stays for the rare track carrying several.
                fetchPlexStreams();
                std::vector<const PlexStream*> found;
                for (const auto& ps : m_plexStreams)
                    if (ps.streamType == 4 && !ps.key.empty()) found.push_back(&ps);

                if (found.size() == 1)  loadAndShowLyrics(*found.front());
                else if (found.empty()) showLyricsMessage("This track has no lyrics.");
                else                    showTrackOverlay(TrackSelectMode::SUBTITLE);
                return true;
            });
            lyricsBtn->addGestureRecognizer(new brls::TapGestureRecognizer(lyricsBtn));
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

        // Queue side-sheet "Clear" control (wired once; lives in the hidden overlay)
        if (queueClearBtn) {
            queueClearBtn->registerClickAction([this](brls::View* view) {
                clearUpcoming();
                return true;
            });
            queueClearBtn->addGestureRecognizer(new brls::TapGestureRecognizer(queueClearBtn));
            // Route Clear's UP back to itself so focus cannot escape behind the overlay.
            queueClearBtn->setFocusable(true);
            queueClearBtn->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
        }

        // Music mode: controls never auto-hide, always visible Override the controls auto-hide for music
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
        setIconRes(rewindIcon, rewindRes);
        setIconRes(forwardIcon, fwdRes);

        // Audio track button - shows track selection overlay
        if (audioBtn) {
            audioBtn->setVisibility(brls::Visibility::VISIBLE);
            if (audioIcon) {
                setIconRes(audioIcon, "icons/translate.png");
            }
            audioBtn->registerClickAction([this](brls::View* view) {
                showTrackOverlay(TrackSelectMode::AUDIO);
                return true;
            });
            audioBtn->addGestureRecognizer(new brls::TapGestureRecognizer(audioBtn));
        }

        // Subtitle track button - shows track selection overlay Video has its own subtitle button for this picker.
        if (lyricsBtn) {
            lyricsBtn->setFocusable(false);
            lyricsBtn->setVisibility(brls::Visibility::GONE);
        }

        if (subBtn) {
            subBtn->setVisibility(brls::Visibility::VISIBLE);
            if (subtitleIcon) {
                setIconRes(subtitleIcon, "icons/subtitles.png");
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
                setIconRes(videoIcon, "icons/video-image.png");
            }
            videoBtn->registerClickAction([this](brls::View* view) {
                showTrackOverlay(TrackSelectMode::VIDEO);
                return true;
            });
            videoBtn->addGestureRecognizer(new brls::TapGestureRecognizer(videoBtn));
        }
    }

    // Runs after the mode wiring that makes the audio/subtitle/video buttons visible; no-op in the other layouts.
    wireVideoOsd();

    // Block upward D-pad escape from the transport row to off-screen absolutely-positioned overlays.
    if (!m_isQueueMode) {
        routeIfFocusable(playBtn, brls::FocusDirection::UP, playBtn);
        routeIfFocusable(rewindBtn, brls::FocusDirection::UP, rewindBtn);
        routeIfFocusable(forwardBtn, brls::FocusDirection::UP, forwardBtn);
    }

    // Same downward, and only on focusable buttons — audioBtn/subBtn/videoBtn are non-focusable in music mode.
    routeIfFocusable(lyricsBtn, brls::FocusDirection::DOWN, lyricsBtn);
    routeIfFocusable(queueBtn, brls::FocusDirection::DOWN, queueBtn);
    if (!m_isQueueMode) {
        routeIfFocusable(audioBtn, brls::FocusDirection::DOWN, audioBtn);
        routeIfFocusable(subBtn, brls::FocusDirection::DOWN, subBtn);
        routeIfFocusable(videoBtn, brls::FocusDirection::DOWN, videoBtn);
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
        // Keep the OS notification's play/pause honest (e.g. mpv paused by audio-focus loss). No-op unless it diverged.
        if (m_isQueueMode) MusicController::getInstance().syncSessionState();

        // Back from the background: the GL surface was torn down, so re-issue the cover and icon loads.
        reapplyIcons();

        // mpv only knows the frame rate once the container is parsed, so this waits for a reading.
        applyContentRefreshRate();

        bool fg = brls::Application::isWindowForeground();
        if (m_isQueueMode && fg && !m_wasForeground && albumArt && !m_destroying) {
            const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
            if (track && !track->ratingKey.empty()) {
                DownloadItem dl;
                if (DownloadsManager::getInstance().getDownloadCopy(track->ratingKey, dl) &&
                    dl.state == DownloadState::COMPLETED && !dl.thumbPath.empty()) {
                    if (ImageLoader::loadFromFile(dl.thumbPath, albumArt))
                        albumArt->setVisibility(brls::Visibility::VISIBLE);
                } else if (!track->thumb.empty()) {
                    std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(track->thumb, m_mobileLayout ? 900 : 300,
                                                 m_mobileLayout ? 900 : 300);
                    ImageLoader::setPaused(false);
                    ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                        img->setVisibility(brls::Visibility::VISIBLE);
                    }, albumArt, m_alive);
                    ImageLoader::setPaused(true);
                }
            }
        }
        m_wasForeground = fg;
    });
    m_updateTimer.start(1000); // Update every second

    // Pump mpv so auto-advance does not wait on the slow poll; m_endHandled keeps it to once per track.
    m_endWatchTimer.setCallback([this]() {
        MpvPlayer& p = MpvPlayer::getInstance();
        if (!p.isInitialized() || m_endHandled) return;
        p.update();
        if (p.hasEnded()) updateProgress();
    });
    m_endWatchTimer.start(250);

    // Debounced seek commit; `finished` is false when stopped early, so a cancelled debounce never commits.
    m_seekCommitTimer.setEndCallback([this](bool finished) {
        if (finished && !m_destroying) commitTranscodeSeek();
    });

    // Start with controls hidden if auto-hide is enabled
    int autoHide = Application::getInstance().getSettings().controlsAutoHideSeconds;
    if (autoHide > 0 && !m_isPhoto) {
        hideControls();
    }

    // Runs for both modes, and is where focus sync goes live: every call before it is a no-op.
    m_focusWiringDone = true;
    syncHiddenFocus();
    registerIcons();

    // Space and media keys go through raw key events: borealis actions are keyed on ControllerButton, which neither has.
    m_inputManager = brls::Application::getPlatform()
                         ? brls::Application::getPlatform()->getInputManager() : nullptr;
    if (m_inputManager) {
        m_kbSub = m_inputManager->getKeyboardKeyStateChanged()->subscribe(
            [this](brls::KeyState ks) {
                if (!ks.pressed || m_destroying) return;
                const int k = (int)ks.key;
                if (k != brls::BRLS_KBD_KEY_SPACE &&
                    k != mediakey::PLAY_PAUSE && k != mediakey::STOP &&
                    k != mediakey::NEXT && k != mediakey::PREV) return;
                // Only when this player is on top: the event is global, and Space typed into a search field must not reach playback.
                const auto stack = brls::Application::getActivitiesStack();
                if (stack.empty() || stack.back() != this) return;
                resetControlsIdleTimer();

                if (k == mediakey::NEXT || k == mediakey::PREV) {
                    // Next/previous skips tracks in a music queue; a video has nothing to skip to, so seek instead.
                    const bool fwd = (k == mediakey::NEXT);
                    if (m_isQueueMode) {
                        fwd ? playNext() : playPrevious();
                    } else {
                        int interval = Application::getInstance().getSettings().seekInterval;
                        seek(fwd ? interval : -interval);
                    }
                    return;
                }
                togglePlayPause();   // Space, Play/Pause, and Stop
            });
        m_kbSubscribed = true;
    }
}

void PlayerActivity::setBackgroundTransparent(bool transparent) {
#ifdef __ANDROID__
    // The opaque restore must match palette::bg so the GL clear leaves no stale grey once video ends.
    if (transparent) {
        brls::Theme::getDarkTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
        brls::Theme::getLightTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
    } else {
        brls::Theme::getDarkTheme().addColor("brls/clear", vitaplex::palette::bg);
        brls::Theme::getLightTheme().addColor("brls/clear", vitaplex::palette::bg);
    }
#else
    (void)transparent;
#endif
}

void PlayerActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);

    // The keyboard event is owned by the input manager and outlives us.
    if (m_inputManager && m_kbSubscribed) {
        m_inputManager->getKeyboardKeyStateChanged()->unsubscribe(m_kbSub);
        m_kbSubscribed = false;
    }

    // Cancel any pending debounced seek so its commit cannot fire after we leave.
    m_seekCommitTimer.stop();
    m_seekTargetMs = -1.0;

    // Left the player — re-enable the SyncLounge auto-join prompt.
    s_active.store(false);

    // Restore the opaque clear so the rest of the app renders on its normal dark background.
    setBackgroundTransparent(false);

    // Clear "video playing" so onUserLeaveHint stops auto-triggering PiP outside the player.
    pip::setVideoPlaybackState(false, 0, 0);

    // Tear down the video OS media session; music's is owned by MusicController, so this is video only.
    if (m_videoOsActive) {
        m_videoOsActive = false;
        nowplaying::clear();
        nowplaying::clearHandler();
    }

    m_lyricsTimer.stop();   // nothing left to follow once the player is gone
    // Both teardown paths pass here, so one stop covers the hand-off too; the controller's poll takes over.
    m_endWatchTimer.stop();

    // Hand the display back: a 24Hz mode left set would redraw the whole UI at 24Hz.
    if (m_refreshRateApplied) {
        m_refreshRateApplied = false;
        platform::setPreferredRefreshRate(0.0f);
    }

    // Re-enable background thumbnail loading now that playback is ending
    ImageLoader::setPaused(false);

    // Leaving the player, so stop outbound mediaUpdates carrying stale media.
    SyncLoungeSession::instance().clearLocalMedia();

#ifdef __vita__
    // Restore reduced clock speeds for browsing (saves battery)
    scePowerSetArmClockFrequency(333);
    scePowerSetBusClockFrequency(166);
    scePowerSetGpuClockFrequency(166);
    scePowerSetGpuXbarClockFrequency(111);
#endif

    // If background music is enabled and we're in queue mode, don't stop playback
    if (m_isQueueMode && Application::getInstance().getSettings().backgroundMusic && !m_destroying) {
        brls::Logger::info("PlayerActivity: Leaving with background music enabled, not stopping");
        m_updateTimer.stop();
        if (m_alive) m_alive->store(false);
        // Hand off to the persistent controller: it keeps the OS session live and owns end-of-track while mpv plays on.
        MusicController::getInstance().detachForeground();
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
    m_pendingDurationMs = 0;

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

        // Prefer Plex's duration over mpv's in queue mode; mpv may only know the demuxed portion.
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

    // Really tearing down, not backgrounding: detach so the controller drops its hooks and clears the OS session.
    if (m_isQueueMode) {
        MusicController::getInstance().detachForeground();
    }
}

void PlayerActivity::loadFromQueue() {
    // Prevent rapid re-entry
    if (m_loadingMedia) {
        brls::Logger::debug("PlayerActivity: Already loading media, skipping");
        return;
    }
    m_loadingMedia = true;

    // Clear the end-of-track guard, and stop() first when auto-advancing so the loop cannot re-fire it.
    if (MpvPlayer::getInstance().hasEnded()) {
        MpvPlayer::getInstance().stop();
    }
    m_endHandled = false;

    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* track = queue.getCurrentTrack();

    if (!track) {
        brls::Logger::error("PlayerActivity: No current track in queue");
        m_loadingMedia = false;
        return;
    }

    brls::Logger::info("PlayerActivity: Loading track from queue: {} - {}",
                      track->artist, track->title);

    // Queue sheet open: rebuild it for the new track on the next frame so it updates immediately.
    if (m_queueOverlayVisible) {
        brls::sync([this]() {
            if (m_queueOverlayVisible) populateQueueList();
        });
    }

    // New track: the stream restarts at the top, so drop any offset a seek left behind.
    MusicController::getInstance().resetStreamStartOffset();
    m_transcodeBaseOffsetMs = 0;

    // Reset streams cache for the new track
    m_streamsLoaded = false;
    m_plexStreams.clear();
    m_partId = 0;

    // Resuming with mpv already playing: update the UI without restarting the track.
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
            // Not on mobile: music_artist already shows this, and a second copy lands on the time row.
            artistLabel->setVisibility((m_mobileLayout || track->artist.empty())
                ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
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
                std::string thumbUrl = client.getThumbnailUrl(track->thumb, m_mobileLayout ? 900 : 300,
                                                 m_mobileLayout ? 900 : 300);
                ImageLoader::setPaused(false);
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                    img->setVisibility(brls::Visibility::VISIBLE);
                }, albumArt, m_alive);
                ImageLoader::setPaused(true);
            }
        }

        // Show music UI elements
        if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
        if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);
        syncHiddenFocus();
        if (videoView) videoView->setVisibility(brls::Visibility::GONE);
        if (photoImage) photoImage->setVisibility(brls::Visibility::GONE);

        updatePlayPauseLabel();
        m_loadingMedia = false;
        return;
    }

    // Past the resume shortcut, so this is a different track; music auto-advance never cleared the old lyrics.
    if (m_lyricsOverlayVisible) hideLyricsOverlay();
    m_lyrics.clear();
    m_lyricRows.clear();
    m_lyricsIndex = -1;

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
        // Artist label only when there is text, and never on mobile where music_artist already carries it.
        artistLabel->setVisibility((m_mobileLayout || track->artist.empty())
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    // Update queue info display
    updateQueueDisplay();

    // Use the rating key to get the playback URL
    m_mediaKey = track->ratingKey;
    std::string url;

    // Pause image loading and drop stale in-flight loads before queuing this track's.
    ImageLoader::setPaused(true);
    ImageLoader::cancelAll();

    // Check if this track is downloaded locally - play from local file if available
    DownloadsManager& downloads = DownloadsManager::getInstance();
    DownloadItem dlItem;
    bool useLocalFile = false;
    if (downloads.getDownloadCopy(track->ratingKey, dlItem) && dlItem.state == DownloadState::COMPLETED) {
        url = dlItem.localPath;
        useLocalFile = true;
        m_isLocalFile = true;  // Suppress timeline reports when offline
        brls::Logger::info("PlayerActivity: Using downloaded file for track: {}", url);

        // Load cover art from local file if available (preferred over server URL)
        if (albumArt && !dlItem.thumbPath.empty()) {
            if (ImageLoader::loadFromFile(dlItem.thumbPath, albumArt)) {
                albumArt->setVisibility(brls::Visibility::VISIBLE);
            }
        }
    } else {
        // Stream from server
        m_isLocalFile = false;  // Reset in case previous track was local
        PlexClient& client = PlexClient::getInstance();

        // Use a prefetch resolved during the previous track; see kPrefetchMaxAgeMs, an expired session is answered 400.
        const int64_t prefetchAgeMs =
            m_prefetchAtMs > 0 ? (brls::getCPUTimeUsec() / 1000) - m_prefetchAtMs : 0;
        const bool prefetchStale = m_prefetchAtMs > 0 && prefetchAgeMs > kPrefetchMaxAgeMs;
        if (prefetchStale && !m_prefetchUrl.empty()) {
            brls::Logger::info("PlayerActivity: prefetched URL for {} is {}s old — resolving again",
                               m_prefetchKey, prefetchAgeMs / 1000);
        }
        const bool prefetched = !m_prefetchUrl.empty() &&
                                m_prefetchKey == track->ratingKey &&
                                m_prefetchVersion == queue.getVersion() &&
                                !prefetchStale;
        if (prefetched) {
            url = m_prefetchUrl;
            // The speculative resolve left the live session alone; now this URL is playing, it becomes current.
            client.adoptTranscodeSession(m_prefetchSession);
            brls::Logger::info("PlayerActivity: using prefetched stream URL for {}",
                               track->ratingKey);
        }
        // Whatever happens next, this entry is spent.
        m_prefetchKey.clear();
        m_prefetchUrl.clear();
        m_prefetchSession.clear();
        m_prefetchAtMs = 0;

        if (!prefetched && !client.getTranscodeUrl(track->ratingKey, url, 0)) {
            brls::Logger::error("Failed to get transcode URL for track: {}", track->ratingKey);
            m_loadingMedia = false;
            return;
        }

        // Unpause briefly so loadAsync accepts the album-art request, then re-pause to block other page loads.
        if (albumArt && !track->thumb.empty()) {
            PlexClient& artClient = PlexClient::getInstance();
            std::string thumbUrl = artClient.getThumbnailUrl(track->thumb, m_mobileLayout ? 900 : 300,
                                                 m_mobileLayout ? 900 : 300);
            ImageLoader::setPaused(false);
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, albumArt, m_alive);
            ImageLoader::setPaused(true);
            albumArt->setVisibility(brls::Visibility::VISIBLE);
        }
    }

    MpvPlayer& player = MpvPlayer::getInstance();

    // Set audio-only mode BEFORE initializing
    player.setAudioOnly(true);
    setBackgroundTransparent(false);  // audio-only: keep opaque

    // Clear the image cache only on first MPV init; later it just forces covers to be re-downloaded.
    if (!player.isInitialized()) {
        ImageLoader::clearCache();
    }

    // Stream audio directly via MPV (transcode API returns mp3 stream or local file)
    if (!player.isInitialized()) {
        // Defer MPV init + load to after activity transition completes
        m_pendingPlayUrl = url;
        m_pendingPlayTitle = track->title;
        m_pendingDurationMs = (int64_t)track->duration * 1000;
        m_pendingIsAudio = true;
        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Player already initialized (track change) - load immediately
    if (!player.loadUrl(url, track->title, (int64_t)track->duration * 1000)) {
        brls::Logger::error("Failed to load URL: {}", redactTokensInUrl(url));
        m_loadingMedia = false;
        return;
    }

    m_isPlaying = true;
    m_loadingMedia = false;
    MusicController::getInstance().publishNowPlaying(1);
}

void PlayerActivity::prefetchNextTrack() {
    if (!m_isQueueMode || m_destroying || m_prefetchInFlight) return;

    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* next = queue.peekNextTrack();
    if (!next || next->ratingKey.empty()) return;

    const uint32_t version = queue.getVersion();
    // Already resolved or already failed for this track and queue state; without it the 1s caller re-resolves forever.
    if (m_prefetchKey == next->ratingKey && m_prefetchVersion == version) return;

    // A downloaded track plays from disk; there is no URL to resolve.
    DownloadItem dl;
    if (DownloadsManager::getInstance().getDownloadCopy(next->ratingKey, dl) &&
        dl.state == DownloadState::COMPLETED && !dl.localPath.empty()) {
        return;
    }

    const std::string key = next->ratingKey;
    m_prefetchInFlight = true;
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;

    asyncRun([this, key, version, aliveWeak]() {
        std::string url, session;
        const bool ok = PlexClient::getInstance().getTranscodeUrlSpeculative(key, url, session);
        brls::sync([this, key, version, url, session, ok, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            m_prefetchInFlight = false;
            // Recorded either way: on failure the empty URL is what stops this from being retried on every tick.
            m_prefetchKey     = key;
            m_prefetchUrl     = ok ? url : std::string();
            m_prefetchAtMs    = ok ? (brls::getCPUTimeUsec() / 1000) : 0;
            m_prefetchSession = ok ? session : std::string();
            m_prefetchVersion = version;
            if (ok) brls::Logger::debug("PlayerActivity: prefetched stream URL for {}", key);
        });
    });
}

void PlayerActivity::loadMedia() {
    // Prevent rapid re-entry
    if (m_loadingMedia) {
        brls::Logger::debug("PlayerActivity: Already loading media, skipping");
        return;
    }
    m_loadingMedia = true;

    // A content switch supersedes a pending seek; clear the cached duration so a stale value cannot bleed through.
    m_seekCommitTimer.stop();
    m_seekTargetMs = -1.0;
    m_mediaDurationMs = 0;
    m_syncRecoverAttempts = 0;
    m_directPlay = false;

    // Same reason: auto-play-next reloads into this activity, so the old poster must not survive into the next item.
    m_osArtUrl.clear();
    m_osArtist.clear();
    m_osAlbum.clear();
    m_refreshRateApplied = false;   // the next file gets its own rate

    // The previous track's lyrics do not belong to this one.
    if (m_lyricsOverlayVisible) hideLyricsOverlay();
    m_lyrics.clear();
    m_lyricRows.clear();
    m_lyricsIndex = -1;

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
        setBackgroundTransparent(!isAudioFile);

        if (!player.isInitialized()) {
            // Defer MPV init past the activity transition: GXM and NanoVG conflict during the show phase (see DESIGN_NOTES).
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
            syncHiddenFocus();
            if (videoView) videoView->setVisibility(brls::Visibility::GONE);
            if (photoImage) photoImage->setVisibility(brls::Visibility::GONE);
        } else {
            if (m_videoOsd) {
                setVideoOsdTitle(dlItem.parentTitle.empty() ? dlItem.title : dlItem.parentTitle,
                                 dlItem.parentTitle.empty() ? "" : dlItem.title);
            } else if (titleLabel) {
                std::string title = dlItem.title;
                if (!dlItem.parentTitle.empty()) {
                    title = dlItem.parentTitle + " - " + dlItem.title;
                }
                titleLabel->setText(title);
            }
            // Offline: the cover is already a file on disk, so the session can show it with no server to reach.
            m_osArtUrl = dlItem.thumbPath;
            m_osArtist = dlItem.parentTitle;
            m_osAlbum.clear();
        }

        // Pause image loading and free cache to reclaim memory for MPV
        ImageLoader::setPaused(true);
        ImageLoader::cancelAll();
        ImageLoader::clearCache();

        MpvPlayer& player = MpvPlayer::getInstance();

        // Set audio-only mode for music tracks (skip render context)
        player.setAudioOnly(isAudioTrack);
        setBackgroundTransparent(!isAudioTrack);

        // Resume from the saved viewOffset when enabled, but restart if 95% or more was already watched.
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
            m_grandparentRatingKey = item.grandparentRatingKey;
        }

        // Poster and show/season for the OS media session; the lock screen had a bare title on a grey card.
        {
            const std::string art = !item.thumb.empty() ? item.thumb : item.grandparentThumb;
            m_osArtUrl = art.empty() ? "" : client.getThumbnailUrl(art, 512, 512);
            if (item.mediaType == MediaType::EPISODE) {
                m_osArtist = item.grandparentTitle;
                m_osAlbum = "S" + std::to_string(item.parentIndex) +
                            " - E" + std::to_string(item.index);
            } else {
                m_osArtist = item.year > 0 ? std::to_string(item.year) : "";
                m_osAlbum = item.studio;
            }
        }

        // Store markers for intro/credits skip
        m_markers = item.markers;
        if (!m_markers.empty()) {
            brls::Logger::info("PlayerActivity: Loaded {} markers for {}", m_markers.size(), item.title);
        }
        if (titleLabel) {
            if (m_videoOsd) {
                // The OSD's top bar has two lines, so give it show over episode rather than one run-together string.
                if (item.mediaType == MediaType::EPISODE) {
                    setVideoOsdTitle(item.grandparentTitle.empty() ? item.title
                                                                   : item.grandparentTitle,
                                     "S" + std::to_string(item.parentIndex) +
                                     "E" + std::to_string(item.index) + "  " + item.title);
                } else {
                    setVideoOsdTitle(item.title, item.year > 0 ? std::to_string(item.year) : "");
                }
            } else {
                std::string title = item.title;
                if (item.mediaType == MediaType::EPISODE) {
                    title = item.grandparentTitle + " - " + item.title;
                }
                titleLabel->setText(title);
            }
        }

        // SyncLounge: report what we are playing so outbound mediaUpdates carry real media, and re-arm announce-once.
        SyncLoungeSession::instance().setLocalMedia(item.title, item.type, m_mediaKey,
                                                    item.grandparentTitle, item.parentTitle);
        m_syncLoungeAnnounced = false;

        // Handle photos differently - display image instead of playing
        if (item.mediaType == MediaType::PHOTO) {
            brls::Logger::info("Displaying photo: {}", item.title);
            m_isPhoto = true;
            m_loadingMedia = false;

            // A photo is not video, and nothing knows until metadata arrives; strip the OSD to its title bar, the rest is inert.
            if (m_videoOsd) {
                if (centerControls) centerControls->setVisibility(brls::Visibility::GONE);
                for (const char* id : {"player/osd_bottom", "player/osd_bottom_scrim",
                                       "player/speed_btn", "player/next_btn"}) {
                    if (brls::View* v = getView(id)) v->setVisibility(brls::Visibility::GONE);
                }
            }

            // Load the full-size photo
            if (!item.thumb.empty()) {
                const auto& photoIc = platform::getImageConstraints();
                std::string photoUrl = client.getThumbnailUrl(item.thumb, photoIc.photoRequestWidth, photoIc.photoRequestHeight);
                brls::Logger::debug("Photo URL: {}", redactTokensInUrl(photoUrl));

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
                int artSize = platform::getImageConstraints().squareRequestSize;
                std::string thumbUrl = client.getThumbnailUrl(artPath, artSize, artSize);
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* image) {
                    // Art loaded
                }, albumArt, m_alive);
                albumArt->setVisibility(brls::Visibility::VISIBLE);
            }
        }

        // Transcode URL for video/audio; resume from viewOffset only when enabled, and restart if 95% was watched.
        int resumeOffset = 0;
        if (Application::getInstance().getSettings().resumePlayback && item.viewOffset > 0) {
            bool nearEnd = (item.duration > 0 && item.viewOffset >= item.duration * 0.95);
            if (!nearEnd) {
                resumeOffset = item.viewOffset;
            }
        }

        // Watch party: open at the host's timecode so playback starts in sync, with no corrective seek.
        if (SyncLoungeSession::instance().isConnected()) {
            auto rs = SyncLoungeSession::instance().remoteState();
            if (rs.valid && (rs.state == "playing" || rs.state == "paused")) {
                auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - rs.at).count();
                if (ageMs >= 0 && ageMs < 60000) {
                    double hostMs = rs.timeMs;
                    if (rs.state == "playing") hostMs += static_cast<double>(ageMs);
                    if (hostMs > 0 && (item.duration <= 0 || hostMs < item.duration)) {
                        resumeOffset = static_cast<int>(hostMs);
                        // Suppress an immediate redundant correction in the follow loop now that we open in sync.
                        m_lastSyncSeek = std::chrono::steady_clock::now();
                        brls::Logger::info(
                            "PlayerActivity: SyncLounge follow — starting at host offset {}ms (age {}ms)",
                            resumeOffset, (long)ageMs);
                    }
                }
            }
        }
        m_transcodeBaseOffsetMs = resumeOffset;
        // Remember Plex's full length: mpv's duration reads stale after a transcode reload and let the bar run past the end.
        m_mediaDurationMs = (item.duration > 0) ? (int)item.duration : 0;
        std::string url;
        if (client.getTranscodeUrl(m_mediaKey, url, resumeOffset)) {
            // Direct play: the server returned the original file, so mpv owns the timeline and seeks are local, not restarts.
            m_directPlay = (url.find("/transcode/universal/start") == std::string::npos);
            if (m_directPlay) {
                brls::Logger::info("PlayerActivity: direct play (original file), resume {}ms",
                                   resumeOffset);
                m_transcodeBaseOffsetMs = 0;
                if (resumeOffset > 0) m_pendingSeek = resumeOffset / 1000.0;
            }
            // Pause image loading and free the cache before MPV init; the Vita has 256MB and thumbnails compete with the stream.
            ImageLoader::setPaused(true);
            ImageLoader::cancelAll();
            ImageLoader::clearCache();

            MpvPlayer& player = MpvPlayer::getInstance();

            // Set audio-only mode BEFORE initializing
            player.setAudioOnly(isAudioContent);
            setBackgroundTransparent(!isAudioContent);

            // Stream directly via MPV (transcode API returns mp4/mp3 stream)
            if (!player.isInitialized()) {
                // Defer MPV init past the activity transition; GXM and NanoVG conflict there (see DESIGN_NOTES).
                brls::Logger::info("PlayerActivity: Deferring MPV init to after activity transition");
                m_pendingPlayUrl = url;
                m_pendingPlayTitle = item.title;
                m_pendingIsAudio = isAudioContent;
            } else {
                // Player already initialized (e.g., mode didn't change) - load immediately
                brls::Logger::debug("PlayerActivity: Calling player.loadUrl...");
                if (!player.loadUrl(url, item.title)) {
                    brls::Logger::error("Failed to load URL: {}", redactTokensInUrl(url));
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

void PlayerActivity::syncHiddenFocus() {
    // Inert until onContentAvailable finishes wiring: clearing the button earlier fatal()s the process.
    if (!m_focusWiringDone) return;

    auto shown = [](brls::View* v) {
        return v && v->getVisibility() == brls::Visibility::VISIBLE;
    };
    // The music transport starts GONE and is only shown for audio, so in video its buttons would sit in the focus order unseen.
    const bool music = shown(musicTransport);
    if (musicPlayBtn) musicPlayBtn->setFocusable(music);
    if (musicPrevBtn) musicPrevBtn->setFocusable(music);
    if (musicNextBtn) musicNextBtn->setFocusable(music);
    if (shuffleBtn)   shuffleBtn->setFocusable(music);
    if (repeatBtn)    repeatBtn->setFocusable(music);

    const bool queue = shown(queueOverlay);
    if (queueClearBtn) queueClearBtn->setFocusable(queue);
}

void PlayerActivity::updateProgress() {
    // Don't update if destroying or showing photo
    if (m_destroying || m_isPhoto) return;

    // Mirror the OS media session for video so media keys drive it like music; refreshed each tick to stay current.
    if (!m_isQueueMode) {
        MpvPlayer& osp = MpvPlayer::getInstance();
        if (osp.isInitialized() && (osp.isPlaying() || osp.isPaused())) {
            if (!m_videoOsActive) setupVideoMediaSession();
            else                  publishVideoNowPlaying();
        }
    }

    // Tick the diagnostic overlay with progress; lazy-created, so it costs no layout passes while the toggle is off.
    updateMpvStatsOverlay();

    // Keeps the pills, mute glyph and speed honest after anything changes them. No-op in the other two layouts.
    updateVideoOsd();

    // Phase 1 of 2: create MPV and its render context; loadUrl waits for the next frame.
    if (!m_pendingPlayUrl.empty()) {
        std::string url = m_pendingPlayUrl;
        std::string title = m_pendingPlayTitle;
        bool isAudio = m_pendingIsAudio;
        m_pendingPlayUrl.clear();
        m_pendingPlayTitle.clear();

        brls::Logger::info("PlayerActivity: Performing deferred MPV init (phase 1: create context)...");

        MpvPlayer& player = MpvPlayer::getInstance();
        player.setAudioOnly(isAudio);
        setBackgroundTransparent(!isAudio);

        if (!player.isInitialized()) {
            if (!player.init()) {
                brls::Logger::error("PlayerActivity: Deferred MPV init failed");
                return;
            }
        }

        // Phase 2: loadUrl next iteration, so NanoVG draws a full frame of fresh GXM state before the decoder touches it.
        auto alive = m_alive;
        const int64_t durationMs = m_pendingDurationMs;
        brls::sync([this, url, title, isAudio, alive, durationMs]() {
            if (!alive->load() || m_destroying) return;

            brls::Logger::info("PlayerActivity: Deferred MPV load (phase 2: loadUrl)...");

            MpvPlayer& player = MpvPlayer::getInstance();

            if (player.loadUrl(url, title, durationMs)) {
                if (videoView && !isAudio) {
                    videoView->setVisibility(brls::Visibility::VISIBLE);
                    videoView->setVideoVisible(true);
                    brls::Logger::debug("Video view enabled (deferred)");
                }
                // Mark video playback so Android auto-enters PiP on Home; actual video only, never the music player.
                if (!isAudio && !m_isQueueMode) {
                    pip::setVideoPlaybackState(true, 16, 9);
                }
                m_isPlaying = true;
                updatePlayPauseLabel();
                if (m_isQueueMode) MusicController::getInstance().publishNowPlaying(1);
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

    // Prefer Plex's duration over mpv's in queue mode; mpv may only know the demuxed portion.
    if (m_isQueueMode) {
        const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
        if (track && track->duration > 0) {
            duration = (double)track->duration;
        }
    }
    if (duration <= 0)
        duration = player.getDuration();


    // A music seek can restart the transcode, after which mpv's clock runs from zero; mirror where the stream now starts.
    if (m_isQueueMode)
        m_transcodeBaseOffsetMs = (int)MusicController::getInstance().streamStartOffsetMs();

    // Resolve the next track's URL while this one plays, held off 5s so it does not compete with this track's buffering.
    if (m_isQueueMode && position > 5.0) prefetchNextTrack();

    // While a seek is pending, showSeekPreview owns the slider and labels — they point at the target, not the position.
    if (duration > 0 && m_seekTargetMs < 0.0) {
        // With a resume offset, mpv's position and duration are relative to the stream start; compute absolute for the UI.
        double baseOffsetSec = m_transcodeBaseOffsetMs / 1000.0;
        double absPosition = baseOffsetSec + position;
        // Prefer Plex's length: baseOffset plus mpv duration reads stale after a transcode reload and inflates the bar.
        double absDuration = (m_mediaDurationMs > 0) ? (m_mediaDurationMs / 1000.0)
                                                     : (baseOffsetSec + duration);
        if (absDuration > 0.0 && absPosition > absDuration) absPosition = absDuration;

        if (progressSlider && absDuration > 0.0) {
            m_updatingSlider = true;
            progressSlider->setProgress((float)(absPosition / absDuration));
            m_updatingSlider = false;
        }

        // Elapsed left, remaining right; H:MM:SS past an hour, otherwise M:SS.
        {
            int posTotal = (int)absPosition;
            int posHr  = posTotal / 3600;
            int posMin = (posTotal % 3600) / 60;
            int posSec = posTotal % 60;

            int remaining = std::max(0, (int)(absDuration - absPosition));
            int remHr  = remaining / 3600;
            int remMin = (remaining % 3600) / 60;
            int remSec = remaining % 60;

            int durTotal = (int)absDuration;
            int durHr  = durTotal / 3600;
            int durMin = (durTotal % 3600) / 60;
            int durSec = durTotal % 60;

            char elapsedStr[24];
            char remainStr[24];
            if (durHr > 0) {
                snprintf(elapsedStr, sizeof(elapsedStr), "%d:%02d:%02d", posHr, posMin, posSec);
                snprintf(remainStr, sizeof(remainStr), "-%d:%02d:%02d    ", remHr, remMin, remSec);
            } else {
                snprintf(elapsedStr, sizeof(elapsedStr), "%d:%02d", posMin, posSec);
                snprintf(remainStr, sizeof(remainStr), "-%d:%02d    ", remMin, remSec);
            }

            if (timeElapsedLabel) timeElapsedLabel->setText(elapsedStr);
            if (timeRemainingLabel) timeRemainingLabel->setText(remainStr);

            // Keep legacy time label updated for video mode
            if (timeLabel) {
                char timeStr[48];
                if (durHr > 0) {
                    snprintf(timeStr, sizeof(timeStr), "%d:%02d:%02d / %d:%02d:%02d",
                             posHr, posMin, posSec, durHr, durMin, durSec);
                } else {
                    snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d",
                             posMin, posSec, durMin, durSec);
                }
                timeLabel->setText(timeStr);
            }
        }
    }

    // ─── SyncLounge: the host broadcasts, followers mirror its content and transport ───
    if (duration > 0 && SyncLoungeSession::instance().isConnected()) {
        SyncLoungeSession& sl = SyncLoungeSession::instance();
        // Check against Plex's duration, not mpv's: a corrupt transcode spikes both mpv figures together.
        const double baseSec0   = m_transcodeBaseOffsetMs / 1000.0;
        const double absPosSec0  = baseSec0 + position;
        const double realDurSec = (m_mediaDurationMs > 0) ? (m_mediaDurationMs / 1000.0)
                                                          : (baseSec0 + duration);
        const bool localSane = position >= 0.0 && absPosSec0 <= realDurSec + 30.0;

        // Party pause: when enabled any member can pause the whole party, so apply the latest action once.
        {
            auto pp = sl.partyPauseState();
            if (pp.seq != m_lastPartyPauseSeq) {
                m_lastPartyPauseSeq = pp.seq;
                if (pp.isPause && player.isPlaying())      player.pause();
                else if (!pp.isPause && player.isPaused()) player.play();
            }
        }

        // Switch content before announcing, so a follower never briefly claims the wrong item.
        if (!sl.isHost()) {
            auto mr  = sl.match();
            auto rsm = sl.remoteState();
            bool remoteFresh = rsm.valid &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - rsm.at).count() < 60;
            if (mr.resolved && mr.exact && !mr.ratingKey.empty() && remoteFresh &&
                mr.ratingKey != m_mediaKey && mr.ratingKey != m_syncLoungeContentKey &&
                !m_loadingMedia && !m_isQueueMode && !m_isLocalFile && !m_isDirectFile) {
                brls::Logger::info("SyncLounge: switching to host content ratingKey={} \"{}\"",
                                   mr.ratingKey, mr.title);
                m_syncLoungeContentKey = mr.ratingKey;
                m_syncLoungeClaimHostOnAnnounce = false;  // following — don't steal host
                MpvPlayer::getInstance().stop();
                m_mediaKey       = mr.ratingKey;
                m_endHandled     = false;
                m_introSkipped   = false;
                m_creditsSkipped = false;
                m_markers.clear();
                m_activeMarkerType.clear();
                m_skipButtonVisible = false;
                if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
                loadMedia();   // re-fetches by m_mediaKey, opens at host position
                return;        // new content loading — skip the rest of this tick
            }
        }

        // Announce once per loaded item so the party shows the right title; only a user-initiated new video claims host.
        if (localSane && !m_syncLoungeAnnounced) {
            const char* ast = player.isPlaying() ? "playing"
                            : player.isPaused()  ? "paused" : nullptr;
            if (ast) {
                // Only claim host for genuinely new content — opening the host's own item is joining to follow, not taking over.
                bool claimHost = m_syncLoungeClaimHostOnAnnounce;
                if (claimHost) {
                    auto mr = sl.match();
                    if (mr.resolved && !mr.ratingKey.empty() && mr.ratingKey == m_mediaKey)
                        claimHost = false;
                }
                sl.announceLocalMedia(ast, m_transcodeBaseOffsetMs + position * 1000.0,
                                      duration * 1000.0, claimHost);
                m_syncLoungeAnnounced = true;
            }
        }

        if (sl.isHost()) {
            // Host: publish the absolute timecode, on the same basis as /:/timeline. Throttled inside reportLocalState.
            if (localSane) {
                const char* st = player.isPlaying() ? "playing"
                               : player.isPaused()  ? "paused"
                                                    : nullptr;
                if (st) {
                    double timeMs = m_transcodeBaseOffsetMs + position * 1000.0;
                    double durMs  = duration * 1000.0;
                    sl.reportLocalState(st, timeMs, durMs, 1.0);
                }
            }
        } else {
            auto rs = sl.remoteState();
            // Only follow a recent host state; one frozen by a dropped host would have us seeking back to it forever.
            bool hostFresh = rs.valid &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - rs.at).count() < 12000;
            if (hostFresh && (rs.state == "playing" || rs.state == "paused")) {
                if (!localSane) {
                    // Corrupt PTS: restart the transcode at the host offset, rate-limited so a dead stream cannot loop.
                    auto now = std::chrono::steady_clock::now();
                    if (now - m_lastSyncSeek > std::chrono::seconds(8) &&
                        m_syncRecoverAttempts < 3) {
                        m_lastSyncSeek = now;
                        m_syncRecoverAttempts++;
                        brls::Logger::warning(
                            "SyncLounge: local position insane (corrupt stream) — "
                            "restarting transcode at host {}ms (attempt {}/3)",
                            (long)rs.timeMs, m_syncRecoverAttempts);
                        player.showOSD("Re-syncing…", 1.5);
                        restartTranscodeAtMs((int)rs.timeMs);
                    }
                } else {
                    // Position reads sane — clear the recovery counter.
                    m_syncRecoverAttempts = 0;
                    const double baseOffsetSec = m_transcodeBaseOffsetMs / 1000.0;
                    const double localPosSec   = baseOffsetSec + position;
                    const double remotePosSec  = rs.timeMs / 1000.0;

                    // Match transport state (cheap + idempotent).
                    if (rs.state == "paused" && player.isPlaying()) {
                        player.pause();
                    } else if (rs.state == "playing" && player.isPaused()) {
                        player.play();
                    }

                    // Correct large drift once, then cool down 8s — seeking an HLS transcode restarts it.
                    double drift = localPosSec - remotePosSec;
                    if (drift < 0) drift = -drift;
                    if (drift > 10.0) {
                        auto now = std::chrono::steady_clock::now();
                        if (now - m_lastSyncSeek > std::chrono::seconds(8)) {
                            m_lastSyncSeek = now;
                            player.seekTo(std::max(0.0, remotePosSec - baseOffsetSec));
                            brls::Logger::info(
                                "SyncLounge: seek to host {}s (local {}s, drift {}s)",
                                (long)remotePosSec, (long)localPosSec, (long)drift);
                        }
                    }
                }
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

    // Live TV keep-alive: each timeline ping resets the 300s stop-grab timer, else the grab dies and mpv gets 404s.
    if (!m_liveSessionUuid.empty() && !m_destroying) {
        m_liveKeepaliveCounter++;
        if (m_liveKeepaliveCounter >= 5) {
            m_liveKeepaliveCounter = 0;
            const std::string state = player.isPaused() ? "paused" : "playing";
            int playbackMs = (int)(position * 1000);
            std::string sess = m_liveSessionUuid;
            asyncRun([sess, state, playbackMs]() {
                PlexClient::getInstance().reportLiveTimeline(sess, playbackMs, state);
            });
        }
    }

    // Report the timeline periodically and on state changes, with duration so Plex shows the full length.
    if (!m_mediaKey.empty() && !m_isLocalFile && !m_isDirectFile) {
        std::string currentState = player.isPlaying() ? "playing" :
                                   player.isPaused()  ? "paused"  : "stopped";

        bool stateChanged = (currentState != m_lastTimelineState);
        m_timelineCounter++;

        if (stateChanged || m_timelineCounter >= 10) {
            m_timelineCounter = 0;
            m_lastTimelineState = currentState;

            int timeMs = m_transcodeBaseOffsetMs + (int)(position * 1000);
            int durationMs = (m_mediaDurationMs > 0) ? m_mediaDurationMs : (int)(duration * 1000);

            // A corrupt transcode spikes the position; posting it 400s on Plex and would poison the saved resume point.
            bool posInsane = (m_mediaDurationMs > 0 && timeMs > m_mediaDurationMs + 30000);
            if (!posInsane) {
                std::string ratingKey = m_mediaKey;
                int pqItemID = 0;
                // In queue mode, use the current track's ratingKey and playQueueItemID
                if (m_isQueueMode) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    const QueueItem* track = queue.getCurrentTrack();
                    if (track) {
                        ratingKey = track->ratingKey;
                        pqItemID = track->playQueueItemID;
                    }
                }

                std::string key = "/library/metadata/" + ratingKey;
                PlexClient::getInstance().reportTimeline(
                    ratingKey, key, currentState, timeMs, durationMs, pqItemID);
            }
        }
    }

    // Check hasEnded() regardless of m_isPlaying, which may have synced false a frame before ENDED was set.
    if (player.hasEnded() && !m_endHandled) {
        m_endHandled = true;  // Prevent multiple triggers
        m_isPlaying = false;
        brls::Logger::info("PlayerActivity: Playback ended (mediaType={}, queueMode={})",
            (int)m_mediaType, m_isQueueMode);

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
                brls::Logger::info("PlayerActivity: Looking for next episode (parent={}, index={})",
                    m_parentRatingKey, m_episodeIndex);
                playNextEpisode();
                return;
            }

            // No auto-play - just exit
            brls::sync([this]() {
                brls::Application::popActivity();
            });
        }
    }

    // Keep play/pause label in sync with actual player state
    bool actuallyPlaying = player.isPlaying();
    if (actuallyPlaying != m_isPlaying) {
        m_isPlaying = actuallyPlaying;
        updatePlayPauseLabel();
    }
}

void PlayerActivity::playNextEpisode() {
    // Fetch sibling episodes in the same season
    std::string seasonKey = m_parentRatingKey;
    std::string showKey = m_grandparentRatingKey;
    int currentIndex = m_episodeIndex;

    brls::async([this, seasonKey, showKey, currentIndex]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> siblings;
        if (!client.fetchChildren(seasonKey, siblings)) {
            brls::Logger::error("PlayerActivity: Failed to fetch season children for auto-play-next");
            brls::sync([this]() { brls::Application::popActivity(); });
            return;
        }

        // Find next episode in same season (index = currentIndex + 1)
        std::string nextKey;
        for (const auto& ep : siblings) {
            if (ep.index == currentIndex + 1) {
                nextKey = ep.ratingKey;
                break;
            }
        }

        // If not found in same season, try next season (cross-season)
        if (nextKey.empty() && !showKey.empty()) {
            brls::Logger::info("PlayerActivity: Last episode of season, checking next season");

            // Fetch all seasons of the show
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(showKey, seasons)) {
                // Find the current season's parentIndex by matching seasonKey, then look for the next season.
                std::string nextSeasonKey;
                bool foundCurrent = false;
                for (const auto& season : seasons) {
                    if (foundCurrent && season.mediaType == MediaType::SEASON) {
                        nextSeasonKey = season.ratingKey;
                        break;
                    }
                    if (season.ratingKey == seasonKey) {
                        foundCurrent = true;
                    }
                }

                if (!nextSeasonKey.empty()) {
                    // Fetch episodes of next season and take the first one
                    std::vector<MediaItem> nextSeasonEps;
                    if (client.fetchChildren(nextSeasonKey, nextSeasonEps) && !nextSeasonEps.empty()) {
                        // Find episode with lowest index (usually 1)
                        int lowestIdx = INT_MAX;
                        for (const auto& ep : nextSeasonEps) {
                            if (ep.index < lowestIdx && ep.mediaType == MediaType::EPISODE) {
                                lowestIdx = ep.index;
                                nextKey = ep.ratingKey;
                            }
                        }
                        brls::Logger::info("PlayerActivity: Found first episode of next season: {}",
                            nextKey);
                    }
                }
            }
        }

        if (nextKey.empty()) {
            brls::Logger::info("PlayerActivity: No next episode found, exiting player");
            brls::sync([this]() { brls::Application::popActivity(); });
            return;
        }

        brls::Logger::info("PlayerActivity: Auto-playing next episode: {}", nextKey);

        brls::sync([this, nextKey]() {
            // Stop current playback
            MpvPlayer::getInstance().stop();

            // Reset state for new episode
            m_mediaKey = nextKey;
            m_endHandled = false;
            m_introSkipped = false;
            m_creditsSkipped = false;
            m_markers.clear();
            m_activeMarkerType.clear();
            m_skipButtonVisible = false;
            if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);

            // Load the new episode
            loadMedia();
        });
    });
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

    // Publish the intended state directly: MpvPlayer's own state lags the command we just issued.
    if (m_isQueueMode) MusicController::getInstance().publishNowPlaying(m_isPlaying ? 1 : 0);
    else if (m_videoOsActive) publishVideoNowPlaying();

    // A manual play/pause is a user action, so announce it, and claim host when auto-host is on.
    double absMs = m_transcodeBaseOffsetMs + player.getPosition() * 1000.0;
    syncLoungeReportUserAction(m_isPlaying ? "playing" : "paused", absMs);

    // With party-pausing on a manual pause drives the party; gated inside sendPartyPause, as the server drops unsolicited senders.
    SyncLoungeSession& slpp = SyncLoungeSession::instance();
    if (slpp.isConnected() && slpp.isPartyPauseEnabled())
        slpp.sendPartyPause(!m_isPlaying);
}

void PlayerActivity::setupVideoMediaSession() {
    // Video only — music drives the OS session through MusicController.
    if (m_videoOsActive || m_isQueueMode) return;
    m_videoOsActive = true;
    auto alive = m_alive;

    // Own the global transport handler for this video; alive-guarded, and music reclaims it on its next attach.
    nowplaying::setHandler(
        [this, alive](nowplaying::Transport t) {
            if (!alive->load() || m_destroying) return;
            using T = nowplaying::Transport;
            switch (t) {
                case T::Play:        if (!m_isPlaying) togglePlayPause(); break;
                case T::Pause:       if (m_isPlaying)  togglePlayPause(); break;
                case T::Toggle:      togglePlayPause(); break;
                case T::Next:        playNextEpisode(); break;
                case T::Previous: {
                    double pos = MpvPlayer::getInstance().getPosition();
                    seek(-(int)(pos + 1.0));   // restart current item
                    break;
                }
                case T::Stop:        if (m_isPlaying) togglePlayPause(); break;
                case T::FastForward: seek(30);  break;
                case T::Rewind:      seek(-10); break;
                case T::CycleRepeat:                  // video has no repeat / shuffle
                case T::ToggleShuffle: break;
            }
            publishVideoNowPlaying();
        },
        [this, alive](long long ms) {
            if (!alive->load() || m_destroying) return;
            double cur = MpvPlayer::getInstance().getPosition();
            seek((int)((double)ms / 1000.0 - cur));   // absolute -> relative
            publishVideoNowPlaying();
        });

    publishVideoNowPlaying();
}

void PlayerActivity::applyContentRefreshRate() {
    if (m_refreshRateApplied || m_isQueueMode || m_isPhoto) return;

    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isPlaying()) return;

    // container-fps is declared; estimated-vf-fps is measured and only settles after a few frames, so it is the fallback.
    double fps = 0.0;
    try {
        std::string v = p.getProperty("container-fps");
        if (v.empty()) v = p.getProperty("estimated-vf-fps");
        if (!v.empty()) fps = std::stod(v);
    } catch (const std::exception&) {
        return;   // unparseable: try again on the next tick
    }
    // Anything outside this is a still, a bad reading, or a variable-rate file there is no single right mode for.
    if (!std::isfinite(fps) || fps < 10.0 || fps > 121.0) return;

    m_refreshRateApplied = true;
    platform::setPreferredRefreshRate((float)fps);
}

void PlayerActivity::publishVideoNowPlaying() {
    if (m_isQueueMode) return;   // music has its own publish path
    MpvPlayer& p = MpvPlayer::getInstance();

    nowplaying::Info info;
    if (titleLabel) info.title = titleLabel->getFullText();
#ifdef __ANDROID__
    // Android only: MPRIS and SMTC read the same struct, and their video controls stay as they are.
    info.artUrl = m_osArtUrl;
    info.artist = m_osArtist;
    info.album  = m_osAlbum;
#endif
    info.playing    = m_isPlaying;
    info.positionMs = (long long)(p.getPosition() * 1000.0);
    info.durationMs = (long long)(p.getDuration() * 1000.0);
    info.hasNext    = !m_grandparentRatingKey.empty();  // episodes can skip ahead
    info.hasPrev    = false;
    nowplaying::update(info);
    m_lastVideoOsPlaying = m_isPlaying;
}

void PlayerActivity::syncLoungeReportUserAction(const std::string& state, double absTimeMs) {
    SyncLoungeSession& sl = SyncLoungeSession::instance();
    if (!sl.isConnected()) return;
    // mpv can read NaN/0 at a state change; never broadcast that — the periodic host broadcast covers the gap.
    double durMs = MpvPlayer::getInstance().getDuration() * 1000.0;
    if (!std::isfinite(absTimeMs) || absTimeMs < 0.0) return;
    if (!std::isfinite(durMs)    || durMs    <= 0.0) return;
    // Pause, play and seek announce state but never claim host; only starting a new video does.
    sl.announceLocalMedia(state, absTimeMs, durMs, /*claimHost=*/false);
}

// Park the icon when there is no GL surface; setImageFromRes would silently drop the upload.
void PlayerActivity::setIconRes(brls::Image* img, const std::string& res) {
    if (!img) return;
    m_iconRes[img] = res;   // remembered whether or not the upload lands
    if (brls::Application::canUploadTextures()) img->setImageFromRes(res);
}

// What the XML loads at inflation, recorded not re-set: these are the paths reapplyIcons() needs if the context is lost.
void PlayerActivity::registerIcons() {
    auto note = [this](brls::Image* img, const char* res) {
        // Only if nothing set one already; the video branch picks seek-interval icons, and those are the real resources.
        if (img && m_iconRes.find(img) == m_iconRes.end()) m_iconRes[img] = res;
    };
    note(shuffleIcon,     "icons/shuffle-disabled.png");
    note(musicPrevIcon,   "icons/skip-previous.png");
    note(musicPlayIcon,   "icons/pause.png");
    note(musicNextIcon,   "icons/skip-next.png");
    note(repeatIcon,      "icons/repeat-off.png");
    note(rewindIcon,      "icons/rewind-10.png");
    note(playPauseIcon,   "icons/pause.png");
    note(forwardIcon,     "icons/fast-forward-10.png");
    note(audioIcon,       "icons/translate.png");
    note(subtitleIcon,    "icons/subtitles.png");
    note(videoIcon,       "icons/video-image.png");
    note(pipIcon,         "icons/video-image.png");
    note(queueIcon,       "icons/format-list-group.png");
    note(lyricsIcon,      "icons/subtitles.png");
}

// Re-upload every icon: a swap dropped while hidden and a texture lost with the EGL context look the same from here.
void PlayerActivity::reapplyIcons() {
    const bool safe = brls::Application::canUploadTextures();
    if (safe && !m_uploadsWereSafe) {
        for (const auto& [img, res] : m_iconRes)
            if (img) img->setImageFromRes(res);
    }
    m_uploadsWereSafe = safe;
}

void PlayerActivity::updatePlayPauseLabel() {
    const char* res = m_isPlaying ? "icons/pause.png" : "icons/play.png";
    setIconRes(playPauseIcon, res);
    setIconRes(musicPlayIcon, res);   // music transport's own play button
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

// Split a message on newlines so a multi-line reason renders as rows rather than one clipped line.
static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == '\n') { out.push_back(current); current.clear(); }
        else            { current += c; }
    }
    out.push_back(current);
    return out;
}

// Open the sheet on a message rather than a song, as untimed rows, so it scrolls and dismisses exactly like lyrics.
void PlayerActivity::showLyricsMessage(const std::string& text) {
    m_lyrics.clear();
    for (const std::string& line : splitLines(text)) {
        LyricLine l;
        l.timeMs = -1;
        l.text = line;
        m_lyrics.push_back(std::move(l));
    }
    m_lyricsFailed = true;
    buildLyricsRows();
    showLyricsOverlay();
}

void PlayerActivity::loadAndShowLyrics(const PlexStream& stream) {
    if (m_lyricsLoading) return;
    m_lyricsLoading = true;
    m_lyrics.clear();
    m_lyricsIndex = -1;

    const PlexStream lyricsStream = stream;   // copied: m_plexStreams can be rebuilt
    const std::string ratingKey = m_mediaKey;
    const int partId = m_partId;
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
    asyncRun([this, lyricsStream, ratingKey, partId, aliveWeak]() {
        std::vector<LyricLine> lines;
        std::string status;
        const bool ok = PlexClient::getInstance().fetchLyrics(
            ratingKey, lyricsStream, partId, lines, status);
        brls::sync([this, lines, ok, status, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            m_lyricsLoading = false;

            if (!ok || lines.empty()) {
                // Open the sheet and say what went wrong there; a toast over the player is easy to miss.
                showLyricsMessage(status.empty() ? std::string("No lyrics for this track.")
                                                 : status);
                return;
            }
            m_lyrics = lines;
            m_lyricsFailed = false;
            buildLyricsRows();
            showLyricsOverlay();
        });
    });
}

void PlayerActivity::buildLyricsRows() {
    if (!lyricsList) return;

    // Focus first: destroying focused children while they hold focus is what the queue rebuild guards against too.
    if (!lyricsList->getChildren().empty() && lyricsOverlayTitle) {
        lyricsOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(lyricsOverlayTitle);
    }
    lyricsList->clearViews();
    m_lyricRows.clear();
    m_lyricRows.reserve(m_lyrics.size());

    for (const auto& line : m_lyrics) {
        auto* label = new brls::Label();
        // A timed blank is a rest; give it height so the scroll still tracks the music through an instrumental break.
        label->setText(line.text.empty() ? " " : line.text);
        label->setFontSize(ui(17));
        label->setTextColor(nvgRGB(0x8A, 0x8A, 0x90));
        label->setMarginBottom(ui(10));
        lyricsList->addView(label);
        m_lyricRows.push_back(label);
    }

    if (lyricsOverlayTitle) {
        const bool synced = !m_lyricsFailed && !m_lyrics.empty()
                         && m_lyrics.front().timeMs >= 0;
        lyricsOverlayTitle->setText(m_lyricsFailed ? "Lyrics unavailable"
                                  : synced         ? "Lyrics"
                                                   : "Lyrics (not timed)");
    }
}

void PlayerActivity::showLyricsOverlay() {
    if (!lyricsOverlay) return;
    m_lyricsOverlayVisible = true;
    lyricsOverlay->setVisibility(brls::Visibility::VISIBLE);
    syncHiddenFocus();

    // Only worth ticking while the sheet is up, and only for a file with timings — an untimed one never moves.
    if (!m_lyricsFailed && !m_lyrics.empty() && m_lyrics.front().timeMs >= 0) {
        m_lyricsTimer.setCallback([this]() { syncLyricsToPosition(); });
        m_lyricsTimer.start(250);
        syncLyricsToPosition();
    }

    if (lyricsOverlayTitle) {
        lyricsOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(lyricsOverlayTitle);
    }
}

void PlayerActivity::hideLyricsOverlay() {
    m_lyricsTimer.stop();
    m_lyricsOverlayVisible = false;
    if (lyricsOverlayTitle) lyricsOverlayTitle->setFocusable(false);
    if (lyricsOverlay) {
        lyricsOverlay->setVisibility(brls::Visibility::GONE);
        syncHiddenFocus();
    }
    if (lyricsBtn && lyricsBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(lyricsBtn);
    } else if (musicPlayBtn) {
        brls::Application::giveFocus(musicPlayBtn);
    }
}

void PlayerActivity::syncLyricsToPosition() {
    if (!m_lyricsOverlayVisible || m_lyrics.empty()) return;

    // Absolute, not mpv's clock: a seek can restart the transcode, and lyric stamps are against the whole song.
    const int posMs = m_transcodeBaseOffsetMs
                    + (int)(MpvPlayer::getInstance().getPosition() * 1000.0);

    // Last line whose stamp has passed, walked linearly from the current index; only a backward seek goes far.
    int idx = -1;
    for (size_t i = 0; i < m_lyrics.size(); i++) {
        if (m_lyrics[i].timeMs < 0) continue;
        if (m_lyrics[i].timeMs > posMs) break;
        idx = (int)i;
    }
    if (idx == m_lyricsIndex) return;

    if (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_lyricRows.size()) {
        m_lyricRows[(size_t)m_lyricsIndex]->setTextColor(nvgRGB(0x8A, 0x8A, 0x90));
        m_lyricRows[(size_t)m_lyricsIndex]->setFontSize(ui(17));
    }
    m_lyricsIndex = idx;
    if (idx < 0 || idx >= (int)m_lyricRows.size()) return;

    brls::Label* row = m_lyricRows[(size_t)idx];
    row->setTextColor(nvgRGB(0xE5, 0xA0, 0x0D));
    row->setFontSize(ui(19));
    // getY() is absolute, so subtract the content origin, then bias up so the next few lines stay visible below.
    if (lyricsScroll && lyricsList) {
        const float offset = (row->getY() - lyricsList->getY())
                           - lyricsScroll->getHeight() * 0.4f;
        lyricsScroll->setContentOffsetY(offset < 0.0f ? 0.0f : offset, true);
    }
}

const PlexStream* PlayerActivity::findSideloadableStream(int trackId) const {
    for (const auto& ps : m_plexStreams) {
        if (ps.id != trackId) continue;
        if (ps.streamType == 4 && !ps.key.empty()) return &ps;
        return nullptr;
    }
    return nullptr;
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

    // Collect Plex streams of the requested type For subtitles, also include streamType 4
    std::vector<const PlexStream*> plexTracksOfType;
    for (const auto& ps : m_plexStreams) {
        if (ps.streamType == plexStreamType ||
            (mode == TrackSelectMode::SUBTITLE && ps.streamType == 4)) {
            plexTracksOfType.push_back(&ps);
        }
    }

    // Plex streams are primary for audio and subtitles: HLS muxes only the selected track, so mpv sees just one.
    bool usePlexStreams = (mode == TrackSelectMode::AUDIO || mode == TrackSelectMode::SUBTITLE)
                         && !plexTracksOfType.empty();

    // For subtitles, add "Off" option first
    if (mode == TrackSelectMode::SUBTITLE) {
        brls::Box* item = new brls::Box();
        item->setAxis(brls::Axis::ROW);
        item->setJustifyContent(brls::JustifyContent::FLEX_START);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setPaddingTop(ui(10));
        item->setPaddingBottom(ui(10));
        item->setPaddingLeft(ui(12));
        item->setPaddingRight(ui(12));
        item->setCornerRadius(ui(4));
        item->setFocusable(true);

        brls::Label* label = new brls::Label();
        label->setText(m_isQueueMode ? "Off (No Lyrics)" : "Off (No Subtitles)");
        label->setFontSize(ui(16));
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
            item->setPaddingTop(ui(10));
            item->setPaddingBottom(ui(10));
            item->setPaddingLeft(ui(12));
            item->setPaddingRight(ui(12));
            item->setCornerRadius(ui(4));
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
            label->setFontSize(ui(16));
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
            item->setPaddingTop(ui(10));
            item->setPaddingBottom(ui(10));
            item->setPaddingLeft(ui(12));
            item->setPaddingRight(ui(12));
            item->setCornerRadius(ui(4));
            item->setFocusable(true);

            if (track.selected) {
                item->setBackgroundColor(nvgRGBA(80, 80, 200, 100));
                item->setBorderColor(nvgRGB(100, 130, 255));
                item->setBorderThickness(1);
            }

            std::string prefix = track.selected ? "> " : "  ";
            brls::Label* label = new brls::Label();
            label->setText(prefix + displayStr);
            label->setFontSize(ui(16));
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
            label->setFontSize(ui(16));
            label->setTextColor(nvgRGB(180, 180, 180));
            label->setMargins(ui(12), ui(12), ui(12), ui(12));
            trackList->addView(label);
        }
    }

    // For subtitles, add "Search for Subtitles" option at the bottom
    if (mode == TrackSelectMode::SUBTITLE && !m_mediaKey.empty() && !m_isQueueMode) {
        // Add separator
        brls::Box* sep = new brls::Box();
        sep->setWidthPercentage(100.0f);  // the list content box, either layout
        sep->setHeight(ui(1));
        sep->setBackgroundColor(nvgRGBA(255, 255, 255, 40));
        sep->setMarginTop(ui(6));
        sep->setMarginBottom(ui(6));
        trackList->addView(sep);

        brls::Box* searchItem = new brls::Box();
        searchItem->setAxis(brls::Axis::ROW);
        searchItem->setJustifyContent(brls::JustifyContent::FLEX_START);
        searchItem->setAlignItems(brls::AlignItems::CENTER);
        searchItem->setPaddingTop(ui(10));
        searchItem->setPaddingBottom(ui(10));
        searchItem->setPaddingLeft(ui(12));
        searchItem->setPaddingRight(ui(12));
        searchItem->setCornerRadius(ui(4));
        searchItem->setFocusable(true);
        searchItem->setBackgroundColor(nvgRGBA(60, 120, 60, 80));

        brls::Label* searchLabel = new brls::Label();
        searchLabel->setText("Search for Subtitles...");
        searchLabel->setFontSize(ui(16));
        searchLabel->setTextColor(nvgRGB(140, 230, 140));
        searchItem->addView(searchLabel);

        searchItem->registerClickAction([this](brls::View* view) {
            // Defer a frame so the clicked view is not destroyed while its own click handler is still on the stack.
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

    // Move focus to the overlay title before clearing the list, so the focused child can be destroyed safely.
    if (trackOverlayTitle) {
        trackOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(trackOverlayTitle);
    }

    trackList->clearViews();

    // Add a loading label
    brls::Label* loadingLabel = new brls::Label();
    loadingLabel->setText("Searching for subtitles...");
    loadingLabel->setFontSize(ui(16));
    loadingLabel->setTextColor(nvgRGB(180, 180, 180));
    loadingLabel->setMargins(ui(12), ui(12), ui(12), ui(12));
    trackList->addView(loadingLabel);

    // Search for subtitles from Plex (queries OpenSubtitles, etc.)
    PlexClient& client = PlexClient::getInstance();
    std::vector<PlexClient::SubtitleResult> results;

    if (!client.searchSubtitles(m_mediaKey, "en", results) || results.empty()) {
        trackList->clearViews();
        trackOverlayTitle->setText("Subtitle Search");

        brls::Label* noResults = new brls::Label();
        noResults->setText("No subtitles found");
        noResults->setFontSize(ui(16));
        noResults->setTextColor(nvgRGB(180, 180, 180));
        noResults->setMargins(ui(12), ui(12), ui(12), ui(12));
        trackList->addView(noResults);

        // Add back button
        brls::Box* backItem = new brls::Box();
        backItem->setAxis(brls::Axis::ROW);
        backItem->setJustifyContent(brls::JustifyContent::FLEX_START);
        backItem->setAlignItems(brls::AlignItems::CENTER);
        backItem->setPaddingTop(ui(10));
        backItem->setPaddingBottom(ui(10));
        backItem->setPaddingLeft(ui(12));
        backItem->setPaddingRight(ui(12));
        backItem->setCornerRadius(ui(4));
        backItem->setFocusable(true);

        brls::Label* backLabel = new brls::Label();
        backLabel->setText("< Back to Subtitles");
        backLabel->setFontSize(ui(16));
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

    // Move focus to the overlay title before clearing the list, so the focused child can be destroyed safely.
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
    backItem->setPaddingTop(ui(10));
    backItem->setPaddingBottom(ui(10));
    backItem->setPaddingLeft(ui(12));
    backItem->setPaddingRight(ui(12));
    backItem->setCornerRadius(ui(4));
    backItem->setFocusable(true);

    brls::Label* backLabel = new brls::Label();
    backLabel->setText("< Back to Subtitles");
    backLabel->setFontSize(ui(16));
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
    sep->setWidthPercentage(100.0f);  // the list content box, either layout
    sep->setHeight(ui(1));
    sep->setBackgroundColor(nvgRGBA(255, 255, 255, 40));
    sep->setMarginTop(ui(4));
    sep->setMarginBottom(ui(4));
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
        item->setPaddingTop(ui(10));
        item->setPaddingBottom(ui(10));
        item->setPaddingLeft(ui(12));
        item->setPaddingRight(ui(12));
        item->setCornerRadius(ui(4));
        item->setFocusable(true);

        brls::Label* label = new brls::Label();
        label->setText(displayStr);
        label->setFontSize(ui(14));
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
                // HLS carries only the selected audio, so restart the transcode at the current position to re-mux the new track.
                {
                    double currentPos = player.getPosition();
                    int offsetMs = m_transcodeBaseOffsetMs + static_cast<int>(currentPos * 1000);
                    PlexClient& client = PlexClient::getInstance();
                    // Stop existing transcode session so Plex doesn't keep serving old audio segments
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
            } else if (const PlexStream* lyrics = findSideloadableStream(trackId)) {
                // Lyrics are a separate file, not muxed, so load them straight into mpv; setStreamSelection would restart audio for nothing.
                for (auto& ps : m_plexStreams)
                    if (ps.streamType == 3 || ps.streamType == 4)
                        ps.selected = (ps.id == trackId);
                loadAndShowLyrics(*lyrics);
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

    // In a watch party only the host drives playback, so block a follower's skip rather than let it desync and snap back.
    {
        auto& sl = SyncLoungeSession::instance();
        if (sl.isConnected() && !sl.isHost()) {
            player.showOSD("Only the host can seek", 1.5);
            return;
        }
    }

    // Direct play, local file or music: mpv has the data, so seek locally and instantly.
    if (m_isLocalFile || m_isDirectFile || m_isQueueMode || m_directPlay) {
        double targetSec = std::max(0.0, player.getPosition() + seconds);
        player.seekRelative(seconds);
        // SyncLounge: a manual seek is a user action — announce where we're going.
        syncLoungeReportUserAction(player.isPaused() ? "paused" : "playing",
                                   m_transcodeBaseOffsetMs + targetSec * 1000.0);
        return;
    }

    // Transcoded HLS: fold the skip into the pending target and debounce, so a burst of presses commits once.
    double base = (m_seekTargetMs >= 0.0)
                      ? m_seekTargetMs
                      : (m_transcodeBaseOffsetMs + player.getPosition() * 1000.0);
    requestTranscodeSeek(base + seconds * 1000.0);
}

double PlayerActivity::knownDurationMs() const {
    if (m_mediaDurationMs > 0) return (double)m_mediaDurationMs;
    double d = MpvPlayer::getInstance().getDuration();
    return d > 0.0 ? (m_transcodeBaseOffsetMs + d * 1000.0) : 0.0;
}

bool PlayerActivity::restartTranscodeAtMs(int offsetMs) {
    // Restart the transcode at offsetMs: for far seeks, and to escape a corrupt stream an mpv-local seek cannot leave.
    double total = knownDurationMs();
    if (total > 5000.0 && offsetMs > (int)total - 5000) offsetMs = (int)total - 5000;
    if (offsetMs < 0) offsetMs = 0;
    PlexClient& client = PlexClient::getInstance();
    client.stopTranscode();
    std::string url;
    if (!client.getTranscodeUrl(m_mediaKey, url, offsetMs)) return false;
    brls::Logger::info("Player: restarting transcode at offset={}ms", offsetMs);
    m_transcodeBaseOffsetMs = offsetMs;
    MpvPlayer::getInstance().loadUrl(url, "");
    return true;
}

void PlayerActivity::requestTranscodeSeek(double absMs) {
    // Clamp to the real media length so a stale mpv duration (right after a reload) can't push the target past the end.
    double totalMs = knownDurationMs();
    if (absMs < 0.0) absMs = 0.0;
    if (totalMs > 0.0 && absMs > totalMs) absMs = totalMs;
    m_seekTargetMs = absMs;

    // Show where we're heading while the debounce settles (otherwise the ~350 ms wait looks like a frozen UI).
    showSeekPreview(absMs, totalMs);

    // (Re)arm the debounce; rewind resets the countdown without firing, so the commit happens once after the last press.
    if (m_seekCommitTimer.isRunning())
        m_seekCommitTimer.rewind();
    else
        m_seekCommitTimer.start(350);
}

void PlayerActivity::commitTranscodeSeek() {
    if (m_seekTargetMs < 0.0) return;
    const double target = m_seekTargetMs;
    m_seekTargetMs = -1.0;

    MpvPlayer& player = MpvPlayer::getInstance();
    const double baseMs = m_transcodeBaseOffsetMs;
    const double curAbs = baseMs + player.getPosition() * 1000.0;

    // The m3u8 covers [baseMs, total], so only a jump before its start or far past the play head is worth a restart.
    static constexpr double kLocalForwardMaxMs = 60000.0;  // 60 s; tune to taste
    bool mustRestart = (target < baseMs) ||
                       ((target - curAbs) > kLocalForwardMaxMs);

    if (!mustRestart) {
        player.seekTo(std::max(0.0, (target - baseMs) / 1000.0));
    } else {
        // Re-transcode from the target rather than make mpv crawl un-transcoded HLS; mirrors the audio reload path.
        player.showOSD("Seeking…", 1.5);  // "Seeking…"
        if (!restartTranscodeAtMs((int)target) && target >= baseMs) {
            // Restart failed — fall back to a best-effort local seek.
            player.seekTo(std::max(0.0, (target - baseMs) / 1000.0));
        }
    }

    syncLoungeReportUserAction(player.isPaused() ? "paused" : "playing", target);
}

void PlayerActivity::showSeekPreview(double absMs, double totalMs) {
    // Move the slider to the pending spot, guarded so it cannot re-trigger the seek subscription.
    if (progressSlider && totalMs > 0.0) {
        m_updatingSlider = true;
        progressSlider->setProgress((float)std::min(1.0, std::max(0.0, absMs / totalMs)));
        m_updatingSlider = false;
    }

    auto fmt = [](double ms) {
        int t = std::max(0, (int)(ms / 1000.0));
        int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
        char b[24];
        if (h > 0) snprintf(b, sizeof(b), "%d:%02d:%02d", h, m, s);
        else       snprintf(b, sizeof(b), "%d:%02d", m, s);
        return std::string(b);
    };

    if (timeElapsedLabel) timeElapsedLabel->setText(fmt(absMs));
    MpvPlayer::getInstance().showOSD("Seek " + fmt(absMs), 1.0);
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
    bool newShuffle = !queue.isShuffleEnabled();

    // If synced to server, use server-side shuffle/unshuffle
    if (queue.isServerSynced()) {
        PlexClient& client = PlexClient::getInstance();
        PlexClient::PlayQueueContainer pq;
        bool ok = newShuffle
            ? client.shufflePlayQueue(queue.getPlayQueueID(), pq)
            : client.unshufflePlayQueue(queue.getPlayQueueID(), pq);

        if (ok && !pq.items.empty()) {
            queue.setFromPlayQueue(pq, newShuffle);
        } else {
            // Server call failed - fall back to client-side
            queue.setShuffle(newShuffle);
        }
    } else {
        queue.setShuffle(newShuffle);
    }

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
    setIconRes(shuffleIcon, queue.isShuffleEnabled()
        ? "icons/shuffle-variant.png" : "icons/shuffle-disabled.png");
}

// OS media controls asked for a shuffle/repeat state; reuse the on-screen paths so sync, icons and OSD match a tap.
void PlayerActivity::setShuffleFromOs(bool on) {
    if (!m_isQueueMode) return;
    if (on != MusicQueue::getInstance().isShuffleEnabled()) toggleShuffle();
}

void PlayerActivity::setRepeatFromOs(RepeatMode mode) {
    if (!m_isQueueMode) return;
    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.getRepeatMode() == mode) return;
    queue.setRepeatMode(mode);
    updateQueueDisplay();
    updateRepeatIcon();
}


// Whether there is room for the collapsed queue sheet; drop it rather than shrink the cover to a stamp.
bool PlayerActivity::mobileSheetFits() const {
    const float vh = platform::viewportHeight();
    if (vh <= 0.f) return false;
    return (vh - kMobileChrome - kMobileSheetHeight) >= kMobileMinCover;
}

void PlayerActivity::applyMusicLayoutForViewport() {
    if (!albumArt) return;

    // Size the cover to fill the width without crowding the controls, capped so the rows below still fit.
    float vw = platform::viewportWidth();
    float vh = platform::viewportHeight();
    if (vw <= 0 || vh <= 0) return;

    // The mobile layout is built around a big cover, so it takes a much larger share of the width than the classic player.
    float byWidth  = vw * (m_mobileLayout ? 0.78f : 0.55f);
    float byHeight = vh * (m_mobileLayout ? 0.42f : 0.45f);
    float target   = std::min(byWidth, byHeight);

    // Also fit the cover to the height actually left: everything under it is fixed, so a fraction alone clips the play button.
    if (m_mobileLayout) {
        float chrome = kMobileChrome;
        // The collapsed sheet is absolutely positioned, so leave its height clear or the transport lands behind it.
        if (mobileSheetFits()) chrome += kMobileSheetHeight;
        target = std::min(target, vh - chrome);
    }

    // Never below the 220px design size, except on mobile where a short screen would put back the overflow this avoids.
    const float floorPx = m_mobileLayout ? 150.f : 220.f;
    if (target < floorPx) target = floorPx;
    // Cap at 480 except on mobile, where a dominating cover is the design and the cap made it far too small.
    if (!m_mobileLayout && target > 480.f) target = 480.f;

    albumArt->setWidth(target);
    albumArt->setHeight(target);
}

void PlayerActivity::updateRepeatIcon() {
    if (!repeatIcon) return;
    switch (MusicQueue::getInstance().getRepeatMode()) {
        case RepeatMode::OFF: setIconRes(repeatIcon, "icons/repeat-off.png");  break;
        case RepeatMode::ALL: setIconRes(repeatIcon, "icons/repeat.png");      break;
        case RepeatMode::ONE: setIconRes(repeatIcon, "icons/repeat-once.png"); break;
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

        // Show shuffle position when shuffled, otherwise raw queue index
        int displayPos = queue.isShuffleEnabled()
            ? queue.getShufflePosition() + 1
            : queue.getCurrentIndex() + 1;

        snprintf(queueInfo, sizeof(queueInfo), "Track %d of %d%s",
                displayPos,
                queue.getQueueSize(),
                status.c_str());

        queueLabel->setText(queueInfo);
        queueLabel->setVisibility(brls::Visibility::VISIBLE);
    }

    updateMobileSheet();

    // Rebuild the sheet on a version change or a track advance; playTrack/playNext do not bump the version.
    if (m_queueOverlayVisible && queueList && !m_queueBatchActive) {
        if (m_cachedQueueVersion != queue.getVersion() ||
            m_lastRenderedCurrentIndex != queue.getCurrentIndex()) {
            populateQueueList();
        }
    }
}


// The collapsed queue sheet: what is left and what is next. Its views exist only on mobile, so they are looked up, not bound.
void PlayerActivity::updateMobileSheet() {
    if (!m_mobileLayout || !m_isQueueMode) return;

    auto* sheet = dynamic_cast<brls::Box*>(getView("player/sheet"));
    if (!sheet) return;

    if (!mobileSheetFits()) {
        sheet->setVisibility(brls::Visibility::GONE);
        sheet->setFocusable(false);
        return;
    }
    sheet->setFocusable(true);

    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* next = queue.peekNextTrack();

    // Nothing after this track, so the sheet gets out of the way rather than showing an empty row.
    if (!next) {
        sheet->setVisibility(brls::Visibility::GONE);
        return;
    }
    sheet->setVisibility(brls::Visibility::VISIBLE);

    auto* label = dynamic_cast<brls::Label*>(getView("player/sheet_upnext"));
    if (label) {
        const int remaining = queue.getQueueSize() -
            (queue.isShuffleEnabled() ? queue.getShufflePosition() + 1
                                      : queue.getCurrentIndex() + 1);
        char buf[64];
        snprintf(buf, sizeof(buf), "UP NEXT \u00b7 %d TRACK%s",
                 remaining > 0 ? remaining : 0, remaining == 1 ? "" : "S");
        label->setText(buf);
    }

    if (auto* t = dynamic_cast<brls::Label*>(getView("player/sheet_next_title")))
        t->setText(next->title);
    if (auto* a = dynamic_cast<brls::Label*>(getView("player/sheet_next_artist")))
        a->setText(next->artist);
    if (auto* d = dynamic_cast<brls::Label*>(getView("player/sheet_next_duration"))) {
        if (next->duration > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d", next->duration / 60, next->duration % 60);
            d->setText(buf);
        } else {
            d->setText("");
        }
    }

    // Next track's cover, re-fetched only when it changes — this runs once a second.
    auto* thumb = dynamic_cast<brls::Image*>(getView("player/sheet_next_thumb"));
    if (thumb && next->ratingKey != m_sheetThumbKey) {
        m_sheetThumbKey = next->ratingKey;
        DownloadItem dl;
        if (DownloadsManager::getInstance().getDownloadCopy(next->ratingKey, dl) &&
            dl.state == DownloadState::COMPLETED && !dl.thumbPath.empty()) {
            if (ImageLoader::loadFromFile(dl.thumbPath, thumb))
                thumb->setVisibility(brls::Visibility::VISIBLE);
        } else if (!next->thumb.empty()) {
            std::string url = PlexClient::getInstance().getThumbnailUrl(next->thumb, 160, 160);
            ImageLoader::setPaused(false);
            ImageLoader::loadAsync(url, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, thumb, m_alive);
            ImageLoader::setPaused(true);
        } else {
            thumb->setVisibility(brls::Visibility::GONE);
        }
    }
}

void PlayerActivity::wireMobileSheet() {
    if (!m_mobileLayout) return;

    // Lyrics fill this layout, so the classic scrim has nothing to occupy; this is the visible way out for touch.
    if (auto* close = dynamic_cast<brls::Box*>(getView("player/lyrics_close"))) {
        close->registerClickAction([this](brls::View*) {
            hideLyricsOverlay();
            return true;
        });
        close->addGestureRecognizer(new brls::TapGestureRecognizer(close));
    }

    auto* sheet = dynamic_cast<brls::Box*>(getView("player/sheet"));
    if (!sheet) return;

    // Tap the sheet to open the full queue; the grab handle is decorative until drag gestures are plumbed in.
    sheet->registerClickAction([this](brls::View*) {
        showQueueOverlay();
        return true;
    });
    sheet->addGestureRecognizer(new brls::TapGestureRecognizer(sheet));
}

// ─── The video OSD: one file carries both mobile designs, so building one hides the other's views ───
void PlayerActivity::wireVideoOsd() {
    if (!m_videoOsd) return;

    auto box  = [this](const char* id) { return dynamic_cast<brls::Box*>(getView(id)); };
    auto tap  = [this](brls::Box* b, std::function<void()> fn) {
        if (!b) return;
        b->registerClickAction([this, fn](brls::View*) {
            resetControlsIdleTimer();
            fn();
            return true;
        });
        b->addGestureRecognizer(new brls::TapGestureRecognizer(b));
    };

    // Hide the music design's furniture; the spacers must go too, since visible boxes still swallow taps.
    for (const char* id : {"player/music_seek", "player/sheet",
                           "player/spacer_upper", "player/spacer_lower"}) {
        if (brls::View* v = getView(id)) v->setVisibility(brls::Visibility::GONE);
    }
    if (albumArtContainer) albumArtContainer->setVisibility(brls::Visibility::GONE);
    if (musicInfo)        musicInfo->setVisibility(brls::Visibility::GONE);
    if (musicTransport)   musicTransport->setVisibility(brls::Visibility::GONE);
    // The music header has no id of its own, so clear its three children to keep them out of the focus order.
    for (brls::View* v : {(brls::View*)queueBtn, (brls::View*)lyricsBtn, (brls::View*)queueLabel}) {
        if (!v) continue;
        v->setFocusable(false);
        v->setVisibility(brls::Visibility::GONE);
    }
    if (queueBtn && queueBtn->getParent())
        queueBtn->getParent()->setVisibility(brls::Visibility::GONE);

    // Android composites the mpv surface behind the frame, so an opaque root hides the picture entirely.
    if (playerContainer) playerContainer->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

    // Re-anchor the controls to the bottom edge. They sit in the column for music; over video they float on the scrim.
    if (controlsBox) {
        controlsBox->setPositionType(brls::PositionType::ABSOLUTE);
        controlsBox->setPositionBottom(0.0f);
        controlsBox->setPositionLeft(0.0f);
        controlsBox->setPositionRight(0.0f);
    }

    // Move the shared views into the video row: music stacks bar over times, the OSD runs them inline.
    auto* scrubRow = box("player/video_scrub_row");
    auto* musicRow = box("player/time_row");
    if (scrubRow && musicRow && progressSlider && timeElapsedLabel && timeRemainingLabel) {
        auto move = [&](brls::View* v, brls::Box* from) {
            if (!v || !from) return;
            from->removeView(v, /*free=*/false);
            scrubRow->addView(v);
        };
        move(timeElapsedLabel, musicRow);
        move(progressSlider, dynamic_cast<brls::Box*>(progressSlider->getParent()));
        move(timeRemainingLabel, musicRow);

        timeElapsedLabel->setFontSize(16);
        timeElapsedLabel->setTextColor(nvgRGB(0xE2, 0xE2, 0xE6));
        timeRemainingLabel->setFontSize(16);
        timeRemainingLabel->setTextColor(nvgRGB(0x8C, 0x8C, 0x93));
        // 46 tall gives the 32dp touch target; bar and knob centre in whatever height the box gets.
        progressSlider->setHeight(46.0f);
        progressSlider->setWidth(brls::View::AUTO);
        progressSlider->setGrow(1.0f);
        progressSlider->setShrink(1.0f);
        progressSlider->setMargins(0.0f, 20.0f, 0.0f, 20.0f);
        progressSlider->setPointerSize(20.0f);
    }

    // The subtitle line stays hidden until it has text, so a film gets a title rather than a title and a blank.
    if (brls::View* v = getView("player/osd_bottom")) v->setVisibility(brls::Visibility::VISIBLE);
    if (brls::View* v = getView("player/subs_pill")) v->setVisibility(brls::Visibility::VISIBLE);

    tap(box("player/back_btn"),   [] { brls::Application::popActivity(); });
    tap(box("player/speed_btn"),  [this] { cycleSpeed(); });
    tap(box("player/subs_pill"),  [this] { showTrackOverlay(TrackSelectMode::SUBTITLE); });
    tap(box("player/next_btn"),   [this] {
        if (m_isQueueMode) playNext();
        else               playNextEpisode();
    });
    // The OSD starts hidden here but m_controlsVisible starts true; put them in step so the first tap behaves.
    showControls();
    updateVideoOsd();
}

// How much bigger the OSD should be than the handoff drew it; sqrt of the aspect ratio (see DESIGN_NOTES).
float PlayerActivity::videoOsdScale() const {
    const float w = platform::viewportWidth();    // always 1280 under borealis
    const float h = platform::viewportHeight();
    if (w <= 0.0f || h <= 0.0f) return 1.0f;
    return std::clamp(std::sqrt(std::min(w, h) / kVideoOsdBaseHeight), 1.0f, 1.6f);
}

// Re-size the OSD for the viewport; a no-op unless the factor moved, so it can run off the per-second tick.
void PlayerActivity::applyVideoOsdForViewport() {
    if (!m_videoOsd) return;
    const float k = videoOsdScale();
    if (std::abs(k - m_videoOsdScale) < 0.01f) return;
    m_videoOsdScale = k;

    // Base values are player_mobile.xml's; passing 0 leaves a dimension alone, since the pills are sized by their text.
    auto box = [&](const char* id, float w, float h, float r) {
        brls::View* v = getView(id);
        if (!v) return;
        if (w > 0.0f) v->setWidth(w * k);
        if (h > 0.0f) v->setHeight(h * k);
        if (r > 0.0f) v->setCornerRadius(r * k);
    };
    auto font = [&](const char* id, float pt) {
        if (auto* l = dynamic_cast<brls::Label*>(getView(id))) l->setFontSize(pt * k);
    };
    auto margins = [&](const char* id, float t, float r, float b, float l) {
        if (brls::View* v = getView(id)) v->setMargins(t * k, r * k, b * k, l * k);
    };

    // Top bar
    box("player/back_btn", 62, 62, 31);
    box("player/back_icon", 34, 34, 0);
    margins("player/osd_title_block", 0, 21, 0, 21);
    font("player/title", 22);
    font("player/artist", 16);
    box("player/speed_btn", 0, 42, 21);
    margins("player/speed_btn", 0, 21, 0, 0);
    font("player/speed_label", 17);
    box("player/sub_btn", 62, 62, 31);
    box("player/sub_icon", 31, 31, 0);
    box("player/video_btn", 62, 62, 31);
    box("player/video_icon", 31, 31, 0);
    box("player/pip_btn", 62, 62, 31);
    box("player/pip_icon", 29, 29, 0);

    // Centre transport
    box("player/rewind_btn", 78, 78, 39);
    margins("player/rewind_btn", 0, 78, 0, 0);
    box("player/rewind_icon", 34, 34, 0);
    box("player/play_btn", 106, 106, 53);
    box("player/play_pause_icon", 39, 39, 0);
    box("player/forward_btn", 78, 78, 39);
    margins("player/forward_btn", 0, 0, 0, 78);
    box("player/forward_icon", 34, 34, 0);

    // Scrubber and pills
    margins("player/video_scrub_row", 0, 0, 14, 0);
    font("player/time_elapsed", 16);
    font("player/time_remaining", 16);
    if (progressSlider) {
        progressSlider->setHeight(46.0f * k);
        progressSlider->setMargins(0.0f, 20.0f * k, 0.0f, 20.0f * k);
        progressSlider->setPointerSize(20.0f * k);
    }
    for (const char* id : {"player/next_btn", "player/audio_btn", "player/subs_pill"})
        box(id, 0, 42, 21);
    margins("player/next_btn", 0, 14, 0, 0);
    margins("player/audio_btn", 0, 14, 0, 0);
    font("player/next_label", 17);
    font("player/audio_label", 17);
    font("player/subs_label", 17);

    brls::Logger::info("PlayerActivity: video OSD scaled {:.2f}x for a {}x{} viewport",
                       k, platform::viewportWidth(), platform::viewportHeight());
}

// Pill text and the two toggle glyphs, cheap enough for the per-second tick that keeps them honest.
void PlayerActivity::updateVideoOsd() {
    if (!m_videoOsd) return;
    // Catches a fold, rotation or resize without a signal each — unfolding an already-portrait device never flips isPortrait().
    applyVideoOsdForViewport();
    auto& player = MpvPlayer::getInstance();

    auto label = [this](const char* id) { return dynamic_cast<brls::Label*>(getView(id)); };

    if (auto* l = label("player/speed_label")) {
        const double s = player.getSpeed();
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", s);
        std::string txt = buf;
        // Trim to the shortest honest form: 1.00 reads "1", 1.50 reads "1.5", 1.25 stays.
        while (txt.size() > 1 && txt.back() == '0') txt.pop_back();
        if (!txt.empty() && txt.back() == '.') txt.pop_back();
        l->setText(txt + "x");
    }

    // "Audio · English", "Subs · Off" — the selected track, which is why these are pills and not glyphs.
    if (auto* l = label("player/audio_label"))
        l->setText("Audio  " + trackSummary(TrackSelectMode::AUDIO));
    if (auto* l = label("player/subs_label"))
        l->setText("Subs  " + trackSummary(TrackSelectMode::SUBTITLE));

    // Next needs something after this; pressing it with nothing next is a no-op, not a failure.
    if (brls::View* v = getView("player/next_btn")) {
        const bool next = m_isQueueMode
            ? MusicQueue::getInstance().hasNext()
            : (m_mediaType == MediaType::EPISODE && !m_parentRatingKey.empty() && !m_isLocalFile);
        v->setVisibility(next ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

// What the pills say after the control's name. Plex first, mpv as fallback; reads the cache, never fetches.
std::string PlayerActivity::trackSummary(TrackSelectMode mode) const {
    const bool audio  = (mode == TrackSelectMode::AUDIO);
    const int  wanted = audio ? 2 : 3;

    for (const auto& ps : m_plexStreams) {
        if (ps.streamType != wanted || !ps.selected) continue;
        if (!ps.language.empty())     return ps.language;
        if (!ps.title.empty())        return ps.title;
        if (!ps.displayTitle.empty()) return ps.displayTitle;
        return ps.codec.empty() ? "On" : ps.codec;
    }

    for (const auto& t : MpvPlayer::getInstance().getTrackList(audio ? "audio" : "sub")) {
        if (!t.selected) continue;
        if (!t.lang.empty())  return t.lang;
        if (!t.title.empty()) return t.title;
        return "On";
    }

    return audio ? "Default" : "Off";
}

// The speeds worth having on a phone, in the order people reach for them.
void PlayerActivity::cycleSpeed() {
    static constexpr double kSteps[] = {1.0, 1.25, 1.5, 2.0, 0.75};
    auto& player = MpvPlayer::getInstance();
    const double now = player.getSpeed();

    size_t next = 0;
    for (size_t i = 0; i < std::size(kSteps); i++) {
        if (std::abs(kSteps[i] - now) < 0.01) { next = (i + 1) % std::size(kSteps); break; }
    }
    player.setSpeed(kSteps[next]);
    updateVideoOsd();
}

// The OSD prints show over episode, falling back to one line so a film gets a title and year, not a blank.
void PlayerActivity::setVideoOsdTitle(const std::string& title, const std::string& subtitle) {
    if (titleLabel) titleLabel->setText(title);
    if (!artistLabel) return;
    artistLabel->setText(subtitle);
    artistLabel->setVisibility(subtitle.empty() ? brls::Visibility::GONE
                                                : brls::Visibility::VISIBLE);
}

// Queue list overlay methods

void PlayerActivity::showQueueOverlay() {
    if (m_queueOverlayVisible) {
        hideQueueOverlay();
        return;
    }

    m_queueOverlayVisible = true;

    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::VISIBLE);
        syncHiddenFocus();

        queueOverlay->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
            // While a track is grabbed, B drops it (keeps the queue open) rather than closing — press again to close.
            if (m_queueGrabActive) { setQueueGrab(false); return true; }
            hideQueueOverlay();
            return true;
        });
        // X = remove the focused up-next track
        queueOverlay->registerAction("Remove", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
            removeFocusedQueueTrack();
            return true;
        });
        // L / R = move the focused up-next track earlier / later
        queueOverlay->registerAction("Move Up", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            moveFocusedQueueTrack(-1);
            return true;
        });
        queueOverlay->registerAction("Move Down", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            moveFocusedQueueTrack(1);
            return true;
        });
        // START grabs/drops the focused track; Android TV's hold-centre arrives as GUIDE and is re-dispatched here.
        queueOverlay->registerAction("Move", brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
            toggleQueueGrab();
            return true;
        });
        // While grabbed, up/down move the held track; otherwise return false so borealis navigates normally.
        queueOverlay->registerAction("", brls::ControllerButton::BUTTON_NAV_UP, [this](brls::View* view) {
            if (!m_queueGrabActive) return false;
            moveFocusedQueueTrack(-1);
            return true;
        }, /*hidden*/ true, /*allowRepeating*/ true);
        queueOverlay->registerAction("", brls::ControllerButton::BUTTON_NAV_DOWN, [this](brls::View* view) {
            if (!m_queueGrabActive) return false;
            moveFocusedQueueTrack(1);
            return true;
        }, /*hidden*/ true, /*allowRepeating*/ true);
    }

    // Rebuild on a version change or a track advance, else reuse the cached rows for an instant reopen.
    MusicQueue& queue = MusicQueue::getInstance();
    bool needRebuild = !queueList || queueList->getChildren().empty()
        || m_cachedQueueVersion == 0
        || m_cachedQueueVersion != queue.getVersion()
        || m_lastRenderedCurrentIndex != queue.getCurrentIndex();

    if (needRebuild) {
        populateQueueList();   // gives focus to the first up-next row (or Clear)
    } else {
        updateNowPlayingBlock();
        if (!m_queueBatchActive) {
            if (queueList && !queueList->getChildren().empty()) {
                brls::Application::giveFocus(queueList->getChildren()[0]);
            } else if (queueClearBtn) {
                brls::Application::giveFocus(queueClearBtn);
            }
        }
    }
}

void PlayerActivity::hideQueueOverlay() {
    m_queueOverlayVisible = false;
    m_queueGrabActive = false;   // drop any held track when the sheet closes
    m_queueBatchActive = false;  // Cancel any in-progress batch
    m_focusedQueueRow = nullptr;
    m_grabLift.reset(0.0f);      // stop any in-flight pickup animation
    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::GONE);
        syncHiddenFocus();
    }
    // Restore focus to queue button (fall back to play button if unavailable)
    if (queueBtn && queueBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(queueBtn);
    } else if (m_isQueueMode && musicPlayBtn) {
        brls::Application::giveFocus(musicPlayBtn);
    } else if (playBtn) {
        brls::Application::giveFocus(playBtn);
    }
}

void PlayerActivity::createQueueRow(int displayIdx, int trackIdx, const QueueItem& track, bool isCurrent) {
    (void)displayIdx;
    (void)isCurrent;  // the playing track lives in the Now Playing block, never in this list

    // Row: [grip] [thumb] [title / artist] [duration] [remove x]
    brls::Box* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::FLEX_START);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(uiRow(kQueueRowH));
    row->setPaddingLeft(uiRow(10));
    row->setPaddingRight(uiRow(10));
    row->setCornerRadius(uiRow(9));
    row->setFocusable(true);
    row->setMarginBottom(uiRow(kQueueRowGap));
    row->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

    // Drag-handle glyph (3 stacked bars) - a visual affordance for reordering
    brls::Box* grip = new brls::Box();
    grip->setAxis(brls::Axis::COLUMN);
    grip->setJustifyContent(brls::JustifyContent::CENTER);
    grip->setAlignItems(brls::AlignItems::CENTER);
    grip->setWidth(uiRow(14));
    grip->setMarginRight(uiRow(8));
    for (int b = 0; b < 3; b++) {
        brls::Box* bar = new brls::Box();
        bar->setWidth(uiRow(12));
        bar->setHeight(uiRow(2));
        bar->setCornerRadius(uiRow(1));
        bar->setBackgroundColor(nvgRGB(138, 138, 144));
        if (b < 2) bar->setMarginBottom(uiRow(3));
        grip->addView(bar);
    }
    row->addView(grip);

    // Cover art thumbnail (38x38), loaded lazily when the row nears the viewport
    brls::Image* thumb = new brls::Image();
    thumb->setWidth(uiRow(38));
    thumb->setHeight(uiRow(38));
    thumb->setCornerRadius(uiRow(6));
    thumb->setScalingType(brls::ImageScalingType::FIT);
    thumb->setMarginRight(uiRow(11));
    m_deferredThumbs.push_back({thumb, track.thumb, track.ratingKey, false});
    row->addView(thumb);

    // Title (white) over artist (muted), each ellipsized to a single line
    brls::Box* meta = new brls::Box();
    meta->setAxis(brls::Axis::COLUMN);
    meta->setJustifyContent(brls::JustifyContent::CENTER);
    meta->setGrow(1.0f);

    brls::Label* titleLbl = new brls::Label();
    titleLbl->setText(track.title);
    titleLbl->setFontSize(ui(14));
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    meta->addView(titleLbl);

    if (!track.artist.empty()) {
        brls::Label* artistLbl = new brls::Label();
        artistLbl->setText(track.artist);
        artistLbl->setFontSize(ui(12));
        artistLbl->setTextColor(nvgRGB(180, 180, 186));
        artistLbl->setSingleLine(true);
        artistLbl->setMarginTop(uiRow(1));
        meta->addView(artistLbl);
    }
    row->addView(meta);

    // Duration (tabular m:ss)
    brls::Label* durLbl = new brls::Label();
    if (track.duration > 0) {
        char durBuf[16];
        snprintf(durBuf, sizeof(durBuf), "%d:%02d", track.duration / 60, track.duration % 60);
        durLbl->setText(durBuf);
    } else {
        durLbl->setText("");
    }
    durLbl->setFontSize(ui(12));
    durLbl->setTextColor(nvgRGB(138, 138, 144));
    durLbl->setMarginLeft(uiRow(8));
    row->addView(durLbl);

    // Remove (x) affordance - reserved space, revealed only while focused
    brls::Box* removeBtn = new brls::Box();
    removeBtn->setAxis(brls::Axis::ROW);
    removeBtn->setJustifyContent(brls::JustifyContent::CENTER);
    removeBtn->setAlignItems(brls::AlignItems::CENTER);
    removeBtn->setWidth(uiRow(24));
    removeBtn->setHeight(uiRow(24));
    removeBtn->setCornerRadius(uiRow(6));
    removeBtn->setMarginLeft(uiRow(6));
    removeBtn->setVisibility(brls::Visibility::INVISIBLE);
    brls::Image* removeIcon = new brls::Image();
    removeIcon->setWidth(uiRow(12));
    removeIcon->setHeight(uiRow(12));
    removeIcon->setScalingType(brls::ImageScalingType::FIT);
    removeIcon->setImageFromRes("icons/cross.png");
    removeBtn->addView(removeIcon);
    row->addView(removeBtn);

    // Row -> track mapping (looked up dynamically by the handlers below)
    m_queueRowData[row] = {trackIdx, track.title, removeBtn};

    // Swipe left to remove: the row reddens with the swipe and saturates past the commit distance.
    auto restoreRowTint = [this, row]() {
        if (row == m_focusedQueueRow) {
            row->setBackgroundColor(m_queueGrabActive ? nvgRGBA(229, 160, 13, 90)
                                                      : nvgRGB(58, 58, 70));
        } else {
            row->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        }
    };
    // Commit distance scales with the layout: a fixed 120 units is a flick on a phone and fires while scrolling.
    const float kCommitPx = ui(120.0f);
    // Saturate the tint well before the commit distance, so the colour warns rather than acting as a progress bar.
    const float kTintFullPx = kCommitPx * 0.4f;
    const float kRestPx     = ui(4.0f);
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row, restoreRowTint, kCommitPx, kTintFullPx, kRestPx](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            // STAY is every frame of a pan; UNSURE and START fire once each and both report a zero delta.
            if (status.state == brls::GestureState::UNSURE ||
                status.state == brls::GestureState::START ||
                status.state == brls::GestureState::STAY) {
                float deltaX = status.position.x - status.startPosition.x;
                if (deltaX > 0) { row->setTranslationX(0); restoreRowTint(); return; }
                row->setTranslationX(deltaX);
                // Slide plus colour, no alpha fade — view alpha multiplies through and weakens both cues.
                float d = std::abs(deltaX);
                if (d < kRestPx) {
                    restoreRowTint();
                } else if (d >= kCommitPx) {
                    // Armed — letting go here deletes, so say so plainly.
                    row->setBackgroundColor(nvgRGBA(205, 55, 55, 235));
                } else {
                    float t = std::min(1.0f, d / kTintFullPx);
                    row->setBackgroundColor(
                        nvgRGBA(200, 60, 60, (unsigned char)(190.0f * std::sqrt(t))));
                }
            } else if (status.state == brls::GestureState::END) {
                float deltaX = status.position.x - status.startPosition.x;
                if (deltaX < -kCommitPx) {
                    auto it = m_queueRowData.find(row);
                    if (it != m_queueRowData.end()) {
                        int tIdx = it->second.trackIdx;
                        brls::sync([this, tIdx]() { removeQueueTrackByIndex(tIdx); });
                        return;
                    }
                }
                row->setTranslationX(0);
                restoreRowTint();
            } else if (status.state == brls::GestureState::FAILED ||
                       status.state == brls::GestureState::INTERRUPTED) {
                // A swipe turning into a vertical drag is interrupted, not failed; without this the row keeps its offset and red.
                row->setTranslationX(0);
                restoreRowTint();
            }
        }, brls::PanAxis::HORIZONTAL));

    // Vertical pan: hold briefly to drag-reorder, otherwise scroll — this is how touch reorders without bumpers.
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row, restoreRowTint](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            const float rowH = queueRowPitch();
            float deltaY = status.position.y - status.startPosition.y;

            if (status.state == brls::GestureState::UNSURE) {
                if (!m_dragState.active && m_dragState.draggedRow != row) {
                    m_dragState.holdStart = std::chrono::steady_clock::now();
                    m_dragState.holdMet = false;
                    m_dragState.active = false;
                    m_dragState.draggedRow = row;
                    m_dragState.scrollPassthrough = true;
                    m_dragState.initialScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                    m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                    m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                    auto it = m_queueRowData.find(row);
                    m_dragState.draggedTrackIdx = (it != m_queueRowData.end()) ? it->second.trackIdx : -1;
                }
            } else if (status.state == brls::GestureState::START ||
                       status.state == brls::GestureState::STAY) {
                // Promote a still-held touch into a drag
                if (!m_dragState.holdMet && m_dragState.draggedRow == row) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_dragState.holdStart).count();
                    if (elapsed >= HOLD_THRESHOLD_MS && std::abs(deltaY) < rowH * 0.5f) {
                        m_dragState.holdMet = true;
                        m_dragState.active = true;
                        m_dragState.scrollPassthrough = false;
                        m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                        m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                        m_dragState.dragStartY = status.position.y;
                        m_dragState.dragStartScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                        row->setBackgroundColor(nvgRGBA(229, 160, 13, 60));  // lift cue
                    }
                }

                // Not yet a drag: forward the vertical motion to the scroll view
                if (m_dragState.scrollPassthrough && m_dragState.draggedRow == row) {
                    if (queueScroll && std::abs(deltaY) >= ui(10.0f)) {
                        float newOffset = m_dragState.initialScrollY - deltaY;
                        if (newOffset < 0) newOffset = 0;
                        float maxScroll = queueMaxScroll();
                        if (newOffset > maxScroll) newOffset = maxScroll;
                        queueScroll->setContentOffsetY(newOffset, false);
                    }
                    return;
                }
                if (!m_dragState.holdMet || m_dragState.draggedRow != row) return;

                // Drag mode: row follows the finger, displaced rows slide aside
                float scrollDelta = queueScroll
                    ? (queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY) : 0.0f;
                float eff = (status.position.y - m_dragState.dragStartY) + scrollDelta;
                row->setTranslationY(eff);

                // Auto-scroll near the edges so a drag can reach off-screen rows; STAY fires every frame, even when still.
                if (queueScroll) {
                    // Rate is in rows, not units, so the taller mobile rows scroll at the same pace as the classic ones.
                    const float EDGE = ui(44.0f);
                    // Ramped by depth into the zone: a creep at the boundary to place exactly, fast at the edge to cross a long queue.
                    const float SLOW = rowH * 0.03f;
                    const float FAST = rowH * 0.18f;
                    float viewH = queueScroll->getHeight();
                    float fingerInView = status.position.y - queueScroll->getY();
                    float scrollY = queueScroll->getContentOffsetY();
                    float maxScroll = queueMaxScroll();
                    float past = fingerInView - (viewH - EDGE);   // into the bottom zone
                    float above = EDGE - fingerInView;            // into the top zone
                    if (past > 0 && scrollY < maxScroll) {
                        float speed = SLOW + (FAST - SLOW) * std::min(1.0f, past / EDGE);
                        queueScroll->setContentOffsetY(std::min(maxScroll, scrollY + speed), false);
                    } else if (above > 0 && scrollY > 0) {
                        float speed = SLOW + (FAST - SLOW) * std::min(1.0f, above / EDGE);
                        queueScroll->setContentOffsetY(std::max(0.0f, scrollY - speed), false);
                    }
                    // Re-read the offset so the row stays under the finger and the target reflects the new scroll position.
                    scrollDelta = queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY;
                    eff = (status.position.y - m_dragState.dragStartY) + scrollDelta;
                    row->setTranslationY(eff);
                }

                int origIdx = m_dragState.originalDisplayIdx;
                // Rows only: queueList also holds the "+N more" label, whose slot has no row behind it.
                const int rowCount = (int)m_queueRowData.size();
                int newTarget = origIdx + (int)std::lround(eff / rowH);
                if (newTarget < 0) newTarget = 0;
                if (newTarget > rowCount - 1) newTarget = rowCount - 1;
                m_dragState.targetDisplayIdx = newTarget;

                if (queueList) {
                    auto& children = queueList->getChildren();
                    for (int i = 0; i < std::min((int)children.size(), rowCount); i++) {
                        if (i == origIdx) continue;
                        float shift = 0.0f;
                        if (newTarget > origIdx && i > origIdx && i <= newTarget) shift = -rowH;
                        else if (newTarget < origIdx && i >= newTarget && i < origIdx) shift = rowH;
                        children[i]->setTranslationY(shift);
                    }
                }
            } else if (status.state == brls::GestureState::END ||
                       status.state == brls::GestureState::FAILED ||
                       status.state == brls::GestureState::INTERRUPTED) {
                bool didDrag = m_dragState.holdMet && m_dragState.draggedRow == row;
                int origIdx = m_dragState.originalDisplayIdx;
                int targetIdx = m_dragState.targetDisplayIdx;
                int fromTrack = m_dragState.draggedTrackIdx;

                if (queueList) for (auto* c : queueList->getChildren()) c->setTranslationY(0);

                bool committed = false;
                if (didDrag && status.state == brls::GestureState::END &&
                    origIdx >= 0 && targetIdx >= 0 && origIdx != targetIdx && fromTrack >= 0) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    const bool shuffled = queue.isShuffleEnabled();

                    // Work in play order, as the bumper path does: while shuffling a row's slot and its track index differ.
                    const int playFrom = m_queueWindowStart + origIdx;
                    const int playTo   = m_queueWindowStart + targetIdx;

                    if (playTo >= 0 && playTo < queue.getQueueSize() && playTo != playFrom) {
                        // Anchor the move to the track that will precede it: the target when dragging down, the row above when dragging up.
                        if (queue.isServerSynced()) {
                            const auto& q  = queue.getQueue();
                            const auto& so = queue.getShuffleOrder();
                            auto absAt = [&](int playPos) -> int {
                                if (!shuffled) return playPos;
                                return (playPos >= 0 && playPos < (int)so.size()) ? so[playPos] : -1;
                            };
                            int pqItemID = (fromTrack < (int)q.size()) ? q[fromTrack].playQueueItemID : 0;
                            int anchorAbs = absAt((playTo > playFrom) ? playTo : playTo - 1);
                            int afterPQItemID = (anchorAbs >= 0 && anchorAbs < (int)q.size())
                                                    ? q[anchorAbs].playQueueItemID : 0;
                            if (pqItemID > 0)
                                PlexClient::getInstance().movePlayQueueItem(
                                    queue.getPlayQueueID(), pqItemID, afterPQItemID);
                        }
                        // moveInPlayOrder, not moveTrack: while shuffling, play order is the shuffle order and m_queue must not move.
                        queue.moveInPlayOrder(playFrom, playTo);
                        committed = true;
                        brls::sync([this, targetIdx]() {
                            populateQueueList();
                            m_dragState.justEnded = false;
                            if (queueList) {
                                auto& ch = queueList->getChildren();
                                if (!ch.empty()) {
                                    int t = std::min(std::max(targetIdx, 0), (int)ch.size() - 1);
                                    brls::Application::giveFocus(ch[t]);
                                }
                            }
                        });
                    }
                }
                // Suppress the click that fires right after a drag gesture
                m_dragState.justEnded = didDrag;
                if (!committed) restoreRowTint();

                m_dragState.active = false;
                m_dragState.holdMet = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.scrollPassthrough = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
            }
        }, brls::PanAxis::VERTICAL));

    // Tap to play, suppressed right after a drag; while a track is grabbed, A drops it instead.
    row->registerClickAction([this, row](brls::View* view) {
        if (m_dragState.justEnded) {
            m_dragState.justEnded = false;
            return true;
        }
        if (m_queueGrabActive) {
            setQueueGrab(false);
            return true;
        }
        auto it = m_queueRowData.find(row);
        if (it != m_queueRowData.end()) {
            int idx = it->second.trackIdx;
            brls::sync([this, idx]() { playFromQueue(idx); });
        }
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // On focus: reveal the remove affordance, tint the row, lazy-load nearby thumbnails, restore the previous row.
    row->getFocusEvent()->subscribe([this, row](brls::View*) {
        if (m_focusedQueueRow && m_focusedQueueRow != row) {
            m_focusedQueueRow->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
            // Seat any lifted row back if focus genuinely moves off it.
            m_focusedQueueRow->setTranslationX(0.0f);
            m_focusedQueueRow->setShadowVisibility(false);
            auto pit = m_queueRowData.find(m_focusedQueueRow);
            if (pit != m_queueRowData.end() && pit->second.removeBtn) {
                pit->second.removeBtn->setVisibility(brls::Visibility::INVISIBLE);
            }
        }
        m_focusedQueueRow = row;
        // Grabbed rows keep a gold lift, re-applied so it survives the rebuild and re-focus each move triggers.
        row->setBackgroundColor(m_queueGrabActive ? nvgRGBA(229, 160, 13, 90)
                                                  : nvgRGB(58, 58, 70));
        auto it = m_queueRowData.find(row);
        if (it != m_queueRowData.end() && it->second.removeBtn) {
            it->second.removeBtn->setVisibility(brls::Visibility::VISIBLE);
        }
        int actualIdx = findQueueRowDisplayIndex(row);
        if (actualIdx >= 0) loadQueueThumbsAroundIndex(actualIdx);
    });

    queueList->addView(row);
}

void PlayerActivity::populateQueueList() {
    if (!queueList) return;
    if (m_queuePopulating) return;  // Prevent re-entrant calls
    m_queuePopulating = true;

    m_queueBatchActive = false;  // Cancel any in-progress batched population

    // Park focus on Clear before tearing down rows, so the focused view is never destroyed under borealis.
    m_focusedQueueRow = nullptr;
    m_grabLift.reset(0.0f);  // stop any in-flight lift before rows are destroyed
    if (!queueList->getChildren().empty() && queueClearBtn) {
        brls::Application::giveFocus(queueClearBtn);
    }

    m_queueRowData.clear();
    queueList->clearViews();
    m_deferredThumbs.clear();

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    int count = (int)tracks.size();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();
    int currentIndex = queue.getCurrentIndex();

    m_cachedQueueVersion = queue.getVersion();
    m_lastRenderedCurrentIndex = currentIndex;
    m_queueTotalCount = count;

    // Refresh the "Now Playing" header from the current track
    updateNowPlayingBlock();

    // Position in play order (the shuffle position when shuffling); everything after it is "Up Next".
    int currentPlayPos = shuffled ? queue.getShufflePosition() : currentIndex;
    int firstUpNext = currentPlayPos + 1;
    int upcoming = (count > firstUpNext) ? (count - firstUpNext) : 0;

    if (queueUpNextLabel) {
        if (upcoming > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "UP NEXT \xC2\xB7 %d", upcoming);
            queueUpNextLabel->setText(buf);
        } else {
            queueUpNextLabel->setText("UP NEXT");
        }
    }

    // Window the render so a huge shuffle-all queue cannot spawn thousands of rows; it only grows downward.
    m_queueWindowStart = firstUpNext < 0 ? 0 : firstUpNext;
    m_queueWindowEnd = std::min(count, m_queueWindowStart + QUEUE_RENDER_LIMIT);
    int windowSize = std::max(0, m_queueWindowEnd - m_queueWindowStart);

    if (upcoming <= 0) {
        brls::Label* empty = new brls::Label();
        empty->setText("Nothing up next");
        empty->setFontSize(ui(13));
        empty->setTextColor(nvgRGB(124, 124, 132));
        empty->setMarginTop(10);
        empty->setMarginLeft(10);
        queueList->addView(empty);
        if (m_queueOverlayVisible && queueClearBtn) {
            brls::Application::giveFocus(queueClearBtn);
        }
        m_queueFocusTargetChild = -1;
        m_queuePopulating = false;
        return;
    }

    auto addMoreLabel = [this, count]() {
        if (m_queueWindowEnd < count) {
            brls::Label* more = new brls::Label();
            char mbuf[48];
            snprintf(mbuf, sizeof(mbuf), "+%d more", count - m_queueWindowEnd);
            more->setText(mbuf);
            more->setFontSize(ui(12));
            more->setTextColor(nvgRGB(124, 124, 132));
            more->setMarginTop(8);
            more->setMarginLeft(10);
            queueList->addView(more);
        }
    };

    if (windowSize <= QUEUE_BATCH_SIZE) {
        for (int pos = m_queueWindowStart; pos < m_queueWindowEnd; pos++) {
            int tIdx = (shuffled && pos < (int)shuffleOrder.size()) ? shuffleOrder[pos] : pos;
            if (tIdx < 0 || tIdx >= count) continue;
            createQueueRow(pos, tIdx, tracks[tIdx], false);
        }
        addMoreLabel();
        linkFirstRowToClear();
        loadQueueThumbsAroundIndex(0);
        if (m_queueOverlayVisible && queueList && !queueList->getChildren().empty()) {
            // After a reorder, land on the moved track's new slot; otherwise the first row.
            int fc = (m_queueFocusTargetChild >= 0) ? m_queueFocusTargetChild : 0;
            fc = std::min(std::max(fc, 0), (int)queueList->getChildren().size() - 1);
            brls::Application::giveFocus(queueList->getChildren()[fc]);
        }
        m_queueFocusTargetChild = -1;
        m_queuePopulating = false;
        return;
    }

    // Larger window: snapshot the data and create rows in batches across frames
    m_queueBatchTracks.assign(tracks.begin(), tracks.end());
    m_queueBatchShuffleOrder.assign(shuffleOrder.begin(), shuffleOrder.end());
    m_queueBatchCurrentIndex = currentIndex;
    m_queueBatchShuffled = shuffled;
    m_queueBatchNext = m_queueWindowStart;
    m_queueBatchTotal = m_queueWindowEnd;
    m_queueBatchActive = true;
    populateQueueBatch();

    m_queuePopulating = false;
}

void PlayerActivity::updateNowPlayingBlock() {
    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* cur = queue.getCurrentTrack();
    if (!cur) {
        if (queueNowPlaying) queueNowPlaying->setVisibility(brls::Visibility::GONE);
        if (queueNpLabel)    queueNpLabel->setVisibility(brls::Visibility::GONE);
        return;
    }
    if (queueNpLabel)    queueNpLabel->setVisibility(brls::Visibility::VISIBLE);
    if (queueNowPlaying) queueNowPlaying->setVisibility(brls::Visibility::VISIBLE);

    if (queueNpTitle) {
        queueNpTitle->setText(cur->title);
        queueNpTitle->setSingleLine(true);
    }
    if (queueNpArtist) {
        queueNpArtist->setText(cur->artist);
        queueNpArtist->setSingleLine(true);
        queueNpArtist->setVisibility(cur->artist.empty()
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (queueNpThumb) {
        bool loaded = false;
        if (!cur->ratingKey.empty()) {
            DownloadItem dlItem;
            if (DownloadsManager::getInstance().getDownloadCopy(cur->ratingKey, dlItem) &&
                dlItem.state == DownloadState::COMPLETED && !dlItem.thumbPath.empty()) {
                loaded = ImageLoader::loadFromFile(dlItem.thumbPath, queueNpThumb);
            }
        }
        if (!loaded) {
            if (!cur->thumb.empty()) {
                std::string url = PlexClient::getInstance().getThumbnailUrl(cur->thumb, 120, 120);
                ImageLoader::setPaused(false);
                ImageLoader::loadAsync(url, [](brls::Image*) {}, queueNpThumb, m_alive);
                ImageLoader::setPaused(true);
            } else {
                queueNpThumb->setImageFromRes("icons/music.png");
            }
        }
    }
}

void PlayerActivity::removeQueueTrackByIndex(int trackIdx) {
    MusicQueue& queue = MusicQueue::getInstance();
    if (trackIdx < 0 || trackIdx >= queue.getQueueSize()) return;
    if (trackIdx == queue.getCurrentIndex()) return;  // never drop the playing track here

    if (queue.isServerSynced() && trackIdx < (int)queue.getQueue().size()) {
        int pqItemID = queue.getQueue()[trackIdx].playQueueItemID;
        if (pqItemID > 0) {
            PlexClient::getInstance().removeFromPlayQueue(queue.getPlayQueueID(), pqItemID);
        }
    }
    queue.removeTrack(trackIdx);
    populateQueueList();
}

void PlayerActivity::linkFirstRowToClear() {
    // A ScrollingFrame traps UP at its first row, so route that row to the stable Clear button above.
    if (!queueList || !queueClearBtn) return;
    auto& children = queueList->getChildren();
    if (!children.empty()) {
        children[0]->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
    }
}

void PlayerActivity::removeFocusedQueueTrack() {
    if (!m_queueOverlayVisible) return;
    brls::View* focused = brls::Application::getCurrentFocus();
    auto it = m_queueRowData.find(focused);
    if (it == m_queueRowData.end()) return;
    removeQueueTrackByIndex(it->second.trackIdx);
}

void PlayerActivity::moveFocusedQueueTrack(int direction) {
    if (!m_queueOverlayVisible || !queueList) return;
    brls::View* focused = brls::Application::getCurrentFocus();

    auto& children = queueList->getChildren();
    int childIdx = -1;
    for (int i = 0; i < (int)children.size(); i++) {
        if (children[i] == focused) { childIdx = i; break; }
    }
    if (childIdx < 0) return;
    int targetChild = childIdx + direction;
    if (targetChild < 0 || targetChild >= (int)children.size()) return;

    auto itFrom = m_queueRowData.find(children[childIdx]);
    auto itTo   = m_queueRowData.find(children[targetChild]);
    if (itFrom == m_queueRowData.end() || itTo == m_queueRowData.end()) return;

    MusicQueue& queue = MusicQueue::getInstance();
    const bool shuffled = queue.isShuffleEnabled();

    // Work in play order: a row's slot is window start plus child index, while the track it shows is absolute.
    const int playFrom = m_queueWindowStart + childIdx;
    const int playTo   = m_queueWindowStart + targetChild;
    const int absFrom  = itFrom->second.trackIdx;

    // Move the item after its new play-order predecessor, resolved through the shuffle order when shuffling.
    if (queue.isServerSynced()) {
        const auto& q  = queue.getQueue();
        const auto& so = queue.getShuffleOrder();
        auto absAt = [&](int playPos) -> int {
            if (!shuffled) return playPos;
            return (playPos >= 0 && playPos < (int)so.size()) ? so[playPos] : -1;
        };
        int pqItemID = (absFrom >= 0 && absFrom < (int)q.size()) ? q[absFrom].playQueueItemID : 0;
        int anchorAbs = absAt((direction > 0) ? playTo : playTo - 1);
        int afterPQItemID = (anchorAbs >= 0 && anchorAbs < (int)q.size())
                                ? q[anchorAbs].playQueueItemID : 0;
        if (pqItemID > 0) {
            PlexClient::getInstance().movePlayQueueItem(queue.getPlayQueueID(), pqItemID, afterPQItemID);
        }
    }

    // Reorder play order: the shuffle order when shuffled (m_queue untouched), the queue itself otherwise.
    queue.moveInPlayOrder(playFrom, playTo);

    // Swap the two rows in place rather than rebuilding: no cover flicker, and focus survives.
    brls::View* rowFrom = children[childIdx];
    queueList->removeView(rowFrom, /*free=*/false);  // detach, don't destroy
    queueList->addView(rowFrom, targetChild);        // re-insert at new slot
    // Unshuffled, the two tracks' absolute indices swapped, so swap the rows' trackIdx; shuffled, m_queue is untouched.
    if (!shuffled) std::swap(itFrom->second.trackIdx, itTo->second.trackIdx);
    // Row order (and possibly the first row) changed — keep the UP->Clear escape routes correct around the swap.
    refixQueueUpRoutes(std::min(childIdx, targetChild));
    // The row never lost focus, so giveFocus will not scroll; scroll it in explicitly so hold-to-move follows it.
    brls::Application::giveFocus(rowFrom);
    scrollQueueToChild(targetChild);
    // moveInPlayOrder bumped the version, so sync the cache or the periodic refresh rebuilds what we just hand-edited.
    m_cachedQueueVersion = queue.getVersion();
    m_lastRenderedCurrentIndex = queue.getCurrentIndex();
}

void PlayerActivity::refixQueueUpRoutes(int lo) {
    if (!queueList || !queueClearBtn) return;
    auto& ch = queueList->getChildren();
    // Re-point only the rows whose upward neighbour changed; skip the non-focusable "+N more" label.
    for (int i = std::max(0, lo); i <= lo + 2 && i < (int)ch.size(); i++) {
        if (m_queueRowData.find(ch[i]) == m_queueRowData.end()) continue;
        if (i == 0) {
            ch[0]->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
        } else {
            ch[i]->setCustomNavigationRoute(brls::FocusDirection::UP, ch[i - 1]);
        }
    }
}

float PlayerActivity::queueMaxScroll() {
    if (!queueScroll || !queueList) return 0.0f;
    // Ask ScrollingFrame's own content height; a figure derived from row counts can only disagree with it.
    float contentH = queueList->getHeight();
    if (contentH <= 0.0f)   // not laid out yet: estimate from what we build
        contentH = (float)queueList->getChildren().size() * queueRowPitch();
    return std::max(0.0f, contentH - queueScroll->getHeight());
}

void PlayerActivity::scrollQueueToChild(int idx) {
    if (!queueScroll || !queueList || idx < 0) return;
    const float rowH = queueRowPitch();
    float viewH = queueScroll->getHeight();
    if (viewH <= 0.0f) return;
    float maxScroll = queueMaxScroll();
    // Centre the row, clamped at the ends, matching the list's CENTERED nav so a held row stays put as it scrolls.
    float newOffset = (idx * rowH + rowH / 2.0f) - viewH / 2.0f;
    newOffset = std::min(std::max(newOffset, 0.0f), maxScroll);
    if (std::abs(newOffset - queueScroll->getContentOffsetY()) > 0.5f)
        queueScroll->setContentOffsetY(newOffset, false);
}

void PlayerActivity::toggleQueueGrab() {
    if (!m_queueOverlayVisible) return;
    if (m_queueGrabActive) {
        setQueueGrab(false);
        return;
    }
    // Only pick up an actual up-next track row — not the Clear button, the "+N more" label, or the empty-state text.
    brls::View* focused = brls::Application::getCurrentFocus();
    if (!focused || m_queueRowData.find(focused) == m_queueRowData.end())
        return;
    setQueueGrab(true);
}

void PlayerActivity::setQueueGrab(bool on) {
    m_queueGrabActive = on;
    // Gold tint for the grabbed row, mirroring the sidebar editor's cue; the normal focus tint returns on drop.
    if (m_focusedQueueRow) {
        m_focusedQueueRow->setBackgroundColor(on ? nvgRGBA(229, 160, 13, 90)
                                                  : nvgRGB(58, 58, 70));
        // Pop on pickup, settle on drop, with a shadow while held so the lift reads physically rather than by colour alone.
        animateGrabLift(on);
    }
    if (on) {
        MpvPlayer::getInstance().showOSD("Moving track \xC2\xB7 Up/Down to move, OK to drop", 2.0);
    }
}

void PlayerActivity::animateGrabLift(bool lifted) {
    brls::Box* row = m_focusedQueueRow;
    if (!row) return;
    // Capture this row so a later focus change cannot tug a different one, and drop any stale end callback.
    m_grabLift.setEndCallback([](bool) {});
    m_grabLift.reset(m_grabLift.getValue());
    if (lifted) {
        row->setShadowType(brls::ShadowType::GENERIC);
        row->setShadowVisibility(true);
        // Overshoot past the seated-out position, then settle: a tactile "pop".
        m_grabLift.addStep(1.18f, 110, tweeny::easing::quadraticOut);
        m_grabLift.addStep(1.0f, 90, tweeny::easing::quadraticOut);
    } else {
        m_grabLift.addStep(0.0f, 140, tweeny::easing::quadraticOut);
        // Drop the shadow and zero the offset only once fully seated again.
        m_grabLift.setEndCallback([this, row](bool) {
            if (m_queueRowData.count(row)) {
                row->setTranslationX(0.0f);
                row->setShadowVisibility(false);
            }
        });
    }
    m_grabLift.setTickCallback([this, row] {
        if (m_queueRowData.count(row))
            row->setTranslationX(m_grabLift.getValue() * kGrabLiftPx);
    });
    m_grabLift.start();
}

void PlayerActivity::clearUpcoming() {
    MusicQueue& queue = MusicQueue::getInstance();
    int currentIndex = queue.getCurrentIndex();
    bool shuffled = queue.isShuffleEnabled();

    std::vector<int> toRemove;
    if (shuffled) {
        const auto& order = queue.getShuffleOrder();
        for (int p = queue.getShufflePosition() + 1; p < (int)order.size(); p++) {
            toRemove.push_back(order[p]);
        }
    } else {
        for (int i = currentIndex + 1; i < queue.getQueueSize(); i++) {
            toRemove.push_back(i);
        }
    }
    if (toRemove.empty()) return;

    // Remove from the highest track index downward so lower indices stay valid
    std::sort(toRemove.begin(), toRemove.end());
    for (int k = (int)toRemove.size() - 1; k >= 0; k--) {
        int idx = toRemove[k];
        if (queue.isServerSynced() && idx < (int)queue.getQueue().size()) {
            int pqItemID = queue.getQueue()[idx].playQueueItemID;
            if (pqItemID > 0) {
                PlexClient::getInstance().removeFromPlayQueue(queue.getPlayQueueID(), pqItemID);
            }
        }
        queue.removeTrack(idx);
    }
    populateQueueList();
    MpvPlayer::getInstance().showOSD("Cleared up next", 1.5);
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
        // Convert absolute display index to child index within rendered window
        int childFocusIdx = focusIdx - m_queueWindowStart;
        loadQueueThumbsAroundIndex(childFocusIdx);
        linkFirstRowToClear();

        // Give focus to the current track now that all rows exist — or, after a reorder, to the moved track's new slot.
        if (m_queueOverlayVisible && queueList && !queueList->getChildren().empty()) {
            if (m_queueFocusTargetChild >= 0) childFocusIdx = m_queueFocusTargetChild;
            childFocusIdx = std::min(childFocusIdx, (int)queueList->getChildren().size() - 1);
            if (childFocusIdx < 0) childFocusIdx = 0;
            brls::Application::giveFocus(queueList->getChildren()[childFocusIdx]);
            if (queueOverlayTitle) queueOverlayTitle->setFocusable(false);
        }
        m_queueFocusTargetChild = -1;
    } else {
        // Schedule next batch on the next frame via brls::sync
        brls::sync([this]() {
            populateQueueBatch();
        });
    }
}

void PlayerActivity::expandQueueWindow(int direction) {
    if (!queueList || m_queueBatchActive || m_destroying) return;

    if (direction > 0) {
        // Expand downward - kick off async batch creation
        MusicQueue& queue = MusicQueue::getInstance();
        int count = (int)queue.getQueue().size();
        if (m_queueWindowEnd >= count) return;  // Already at the end
        if (m_expandActive) return;  // Already expanding

        m_expandNext = m_queueWindowEnd;
        m_expandEnd = std::min(count, m_queueWindowEnd + QUEUE_EXPAND_CHUNK);
        m_expandActive = true;
        brls::Logger::debug("Queue: starting async expand {} -> {} (total={})",
            m_expandNext, m_expandEnd, count);
        // Create first batch immediately so content appears right away
        expandQueueBatch();
    }
}

void PlayerActivity::expandQueueBatch() {
    if (!m_expandActive || !queueList || m_destroying) return;

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    int count = (int)tracks.size();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();
    int currentIndex = queue.getCurrentIndex();

    int batchEnd = std::min(m_expandNext + QUEUE_EXPAND_BATCH, m_expandEnd);

    for (int i = m_expandNext; i < batchEnd; i++) {
        int trackIdx = (shuffled && i < (int)shuffleOrder.size())
                        ? shuffleOrder[i] : i;
        if (trackIdx < 0 || trackIdx >= count) continue;
        const QueueItem& track = tracks[trackIdx];
        bool isCurrent = (trackIdx == currentIndex);
        createQueueRow(i, trackIdx, track, isCurrent);
    }

    int oldWindowEnd = m_queueWindowEnd;
    m_queueWindowEnd = batchEnd;
    m_expandNext = batchEnd;

    // Load thumbnails for newly added rows
    int thumbStart = oldWindowEnd - m_queueWindowStart;
    loadQueueThumbsAroundIndex(std::max(0, thumbStart));

    if (m_expandNext >= m_expandEnd) {
        // Expansion complete
        m_expandActive = false;
        brls::Logger::debug("Queue: async expand complete, windowEnd={}", m_queueWindowEnd);
    } else {
        // Schedule next batch on the next frame
        brls::sync([this]() {
            expandQueueBatch();
        });
    }
}

void PlayerActivity::loadQueueThumbsAroundIndex(int displayIndex) {
    if (m_deferredThumbs.empty()) return;

    // Load thumbnails for a window around this row; the 320px scroll shows about five 62px rows.
    int start = std::max(0, displayIndex - QUEUE_THUMB_BUFFER);
    int end = std::min((int)m_deferredThumbs.size(), displayIndex + QUEUE_THUMB_BUFFER + 6);

    // Unpause so loadAsync accepts the requests, then re-pause; the workers no longer check the flag, so they finish.
    ImageLoader::setPaused(false);

    PlexClient& client = PlexClient::getInstance();

    for (int i = start; i < end; i++) {
        auto& dt = m_deferredThumbs[i];
        if (dt.loaded) continue;
        if (dt.thumbPath.empty() && dt.ratingKey.empty()) continue;

        dt.loaded = true;

        // Try local file first (works offline)
        if (!dt.ratingKey.empty()) {
            DownloadItem dlItem;
            if (DownloadsManager::getInstance().getDownloadCopy(dt.ratingKey, dlItem) &&
                dlItem.state == DownloadState::COMPLETED && !dlItem.thumbPath.empty()) {
                if (ImageLoader::loadFromFile(dlItem.thumbPath, dt.image)) {
                    continue;
                }
            }
        }

        // Fall back to server URL
        if (!dt.thumbPath.empty()) {
            std::string thumbUrl = client.getThumbnailUrl(dt.thumbPath, 100, 100);
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* image) {
                // Thumbnail loaded
            }, dt.image, m_alive);
        }
    }

    ImageLoader::setPaused(true);
}

int PlayerActivity::findQueueRowDisplayIndex(brls::View* row) {
    if (!queueList) return -1;
    auto& children = queueList->getChildren();
    for (int i = 0; i < (int)children.size(); i++) {
        if (children[i] == row) return i;
    }
    return -1;
}

void PlayerActivity::swapQueueRows(int displayIdxA, int displayIdxB, bool skipThumbReload) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    if (displayIdxA < 0 || displayIdxA >= (int)children.size()) return;
    if (displayIdxB < 0 || displayIdxB >= (int)children.size()) return;
    if (displayIdxA == displayIdxB) return;

    brls::Box* rowA = (brls::Box*)children[displayIdxA];
    brls::Box* rowB = (brls::Box*)children[displayIdxB];

    // Row is [thumb][textBox][durLbl?], and textBox is [titleLbl][artistLbl?].
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
    // Swap trackIdx back: moveTrack already rearranged the queue, so each slot's index must follow its own position.
    std::swap(itA->second.trackIdx, itB->second.trackIdx);

    // --- Swap thumbnail images ---
    brls::Image* thumbA = (brls::Image*)childrenA[0];
    brls::Image* thumbB = (brls::Image*)childrenB[0];
    if (displayIdxA < (int)m_deferredThumbs.size() &&
        displayIdxB < (int)m_deferredThumbs.size()) {
        auto& dtA = m_deferredThumbs[displayIdxA];
        auto& dtB = m_deferredThumbs[displayIdxB];
        // Swap deferred thumb entries (thumbPath, ratingKey, loaded state)
        std::swap(dtA.thumbPath, dtB.thumbPath);
        std::swap(dtA.ratingKey, dtB.ratingKey);
        std::swap(dtA.loaded, dtB.loaded);
        // Re-point image pointers to their current rows
        dtA.image = thumbA;
        dtB.image = thumbB;
        // Reload thumbnails for the swap, skipped during chained swaps since the caller reloads once at the end.
        if (!skipThumbReload) {
            PlexClient& swapClient = PlexClient::getInstance();
            if (dtA.loaded && !dtA.thumbPath.empty()) {
                std::string urlA = swapClient.getThumbnailUrl(dtA.thumbPath, 100, 100);
                ImageLoader::loadAsync(urlA, [](brls::Image*) {}, thumbA, m_alive);
            } else {
                thumbA->setImageFromRes("icons/music.png");
            }
            if (dtB.loaded && !dtB.thumbPath.empty()) {
                std::string urlB = swapClient.getThumbnailUrl(dtB.thumbPath, 100, 100);
                ImageLoader::loadAsync(urlB, [](brls::Image*) {}, thumbB, m_alive);
            } else {
                thumbB->setImageFromRes("icons/music.png");
            }
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
            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdxA + m_queueWindowStart + 1);
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
            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdxB + m_queueWindowStart + 1);
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
        rowA->setBackgroundColor(nvgRGBA(229, 160, 13, 150));
        rowA->setBorderColor(nvgRGBA(255, 196, 64, 200));
        rowA->setBorderThickness(1.5f);
    } else {
        rowA->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
        rowA->setBorderColor(nvgRGBA(0, 0, 0, 0));
        rowA->setBorderThickness(0);
    }
    if (isCurrB) {
        rowB->setBackgroundColor(nvgRGBA(229, 160, 13, 150));
        rowB->setBorderColor(nvgRGBA(255, 196, 64, 200));
        rowB->setBorderThickness(1.5f);
    } else {
        rowB->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
        rowB->setBorderColor(nvgRGBA(0, 0, 0, 0));
        rowB->setBorderThickness(0);
    }

    queueList->invalidate();
}

void PlayerActivity::reassignQueueRange(int origIdx, int targetIdx) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    int childCount = (int)children.size();
    if (origIdx < 0 || origIdx >= childCount) return;
    if (targetIdx < 0 || targetIdx >= childCount) return;
    if (origIdx == targetIdx) return;

    MusicQueue& queue = MusicQueue::getInstance();
    int currentTrackIdx = queue.getCurrentIndex();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    // Move the widget through borealis' own API so Yoga recalculates; it keeps its cover texture, so nothing re-fetches.
    brls::View* draggedView = children[origIdx];
    queueList->removeView(draggedView, false);  // detach without deleting
    // After removal everything above origIdx shifts down one, so insert at targetIdx either way.
    queueList->addView(draggedView, (size_t)targetIdx);

    // Rotate the deferred thumbnails to stay in sync with children order
    int rangeStart = std::min(origIdx, targetIdx);
    int rangeEnd = std::max(origIdx, targetIdx);
    if (rangeEnd < (int)m_deferredThumbs.size()) {
        if (origIdx < targetIdx) {
            std::rotate(m_deferredThumbs.begin() + origIdx,
                         m_deferredThumbs.begin() + origIdx + 1,
                         m_deferredThumbs.begin() + targetIdx + 1);
        } else {
            std::rotate(m_deferredThumbs.begin() + targetIdx,
                         m_deferredThumbs.begin() + origIdx,
                         m_deferredThumbs.begin() + origIdx + 1);
        }
    }

    // Only the metadata needs fixing — trackIdx and the current-track tint; the content moved with the widget.
    for (int di = rangeStart; di <= rangeEnd && di < (int)children.size(); di++) {
        brls::Box* rowBox = (brls::Box*)children[di];

        int queueDisplayIdx = di + m_queueWindowStart;
        int trackIdx = (shuffled && queueDisplayIdx < (int)shuffleOrder.size())
                        ? shuffleOrder[queueDisplayIdx] : queueDisplayIdx;

        auto it = m_queueRowData.find(rowBox);
        if (it != m_queueRowData.end()) {
            it->second.trackIdx = trackIdx;
        }

        bool isCurr = (trackIdx == currentTrackIdx);
        if (isCurr) {
            rowBox->setBackgroundColor(nvgRGBA(229, 160, 13, 150));
            rowBox->setBorderColor(nvgRGBA(255, 196, 64, 200));
            rowBox->setBorderThickness(1.5f);
        } else {
            rowBox->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
            rowBox->setBorderColor(nvgRGBA(0, 0, 0, 0));
            rowBox->setBorderThickness(0);
        }
    }

    brls::Logger::debug("Drag: moved row {} -> {} via removeView/addView (no re-fetch)", origIdx, targetIdx);
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

        // Use window-offset display number so rows show correct position
        int displayNum = i + m_queueWindowStart + 1;

        auto& rowChildren = ((brls::Box*)child)->getChildren();
        if (rowChildren.size() >= 2) {
            auto& textBoxChildren = ((brls::Box*)rowChildren[1])->getChildren();
            if (!textBoxChildren.empty()) {
                brls::Label* titleLbl = (brls::Label*)textBoxChildren[0];
                if (isCurr) {
                    titleLbl->setText("> " + trackTitle);
                } else {
                    char numBuf[16];
                    snprintf(numBuf, sizeof(numBuf), "%d. ", displayNum);
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

    // Update window tracking after removal
    if (m_queueWindowEnd > 0) m_queueWindowEnd--;
    m_queueTotalCount = MusicQueue::getInstance().getQueueSize();

    // removeTrack already shifted the queue's indices, so rebuild the row map from its current state.
    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    auto& remainingChildren = queueList->getChildren();
    for (int i = 0; i < (int)remainingChildren.size(); i++) {
        int queueIdx = i + m_queueWindowStart;
        int trackIdx = (shuffled && queueIdx < (int)shuffleOrder.size())
                        ? shuffleOrder[queueIdx] : queueIdx;
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
        int displayNum = i + m_queueWindowStart + 1;

        auto& rowChildren = ((brls::Box*)child)->getChildren();
        if (rowChildren.size() >= 2) {
            auto& textBoxChildren = ((brls::Box*)rowChildren[1])->getChildren();
            if (!textBoxChildren.empty()) {
                brls::Label* titleLbl = (brls::Label*)textBoxChildren[0];
                if (isCurr) {
                    titleLbl->setText("> " + trackTitle);
                } else {
                    char numBuf[16];
                    snprintf(numBuf, sizeof(numBuf), "%d. ", displayNum);
                    titleLbl->setText(numBuf + trackTitle);
                }
            }
        }
    }

    // Update title and sync cached version (rows were updated in-place)
    updateQueueTitle();
    m_cachedQueueVersion = queue.getVersion();
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
        // Hide the overlay first, and only when it is up: this also runs for a row picked in the OS up-next list.
        if (m_queueOverlayVisible) hideQueueOverlay();

        // Stop current playback
        MpvPlayer::getInstance().stop();
        m_isPlaying = false;

        // Load selected track
        loadFromQueue();
    }
}

// Controls visibility toggle (like Suwayomi reader settings show/hide)

void PlayerActivity::updateSkipButton(double positionMs) {
    // Followers do not skip on their own, so suppress auto-skip and hide the button rather than let them desync.
    {
        auto& sl = SyncLoungeSession::instance();
        if (sl.isConnected() && !sl.isHost()) {
            if (m_skipButtonVisible || !m_activeMarkerType.empty()) {
                m_skipButtonVisible = false;
                m_activeMarkerType.clear();
                if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
            }
            return;
        }
    }

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
            brls::Logger::info("PlayerActivity: Auto-skipped {} to {}ms", activeType, activeEnd);
            // Hide skip button
            if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
            m_skipButtonVisible = false;
            m_activeMarkerType.clear();

            // If credits auto-skip + auto-play-next, go straight to next episode
            if (!isIntro && settings.autoPlayNext
                && m_mediaType == MediaType::EPISODE
                && !m_parentRatingKey.empty()
                && !m_isLocalFile) {
                brls::Logger::info("PlayerActivity: Credits auto-skipped, starting next episode");
                PlexClient::getInstance().markAsWatched(m_mediaKey);
                m_endHandled = true;
                playNextEpisode();
            } else {
                double seekToSec = (activeEnd - m_transcodeBaseOffsetMs) / 1000.0;
                if (seekToSec > 0) {
                    MpvPlayer::getInstance().seekTo(seekToSec);
                }
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
    // Watch party: only the host skips; a follower follows the host's seek.
    {
        auto& sl = SyncLoungeSession::instance();
        if (sl.isConnected() && !sl.isHost()) {
            MpvPlayer::getInstance().showOSD("Only the host can seek", 1.5);
            return;
        }
    }
    if (m_activeMarkerEndMs <= 0) return;

    double seekToSec = (m_activeMarkerEndMs - m_transcodeBaseOffsetMs) / 1000.0;
    if (seekToSec > 0) {
        MpvPlayer::getInstance().seekTo(seekToSec);
        brls::Logger::info("PlayerActivity: Manually skipped {} to {}ms", m_activeMarkerType, m_activeMarkerEndMs);
        // Host: announce the skip so the party jumps at once instead of waiting for the periodic broadcast.
        syncLoungeReportUserAction(MpvPlayer::getInstance().isPaused() ? "paused" : "playing",
                                   m_activeMarkerEndMs);

        // Mark as skipped to prevent auto-skip re-trigger
        if (m_activeMarkerType == "intro") m_introSkipped = true;
        else if (m_activeMarkerType == "credits") m_creditsSkipped = true;
    }

    // Hide button
    m_skipButtonVisible = false;
    m_activeMarkerType.clear();
    if (skipBtn) skipBtn->setVisibility(brls::Visibility::GONE);
}

// An invisible focused button still answers A, so mark its action unavailable and let the press fall through.
static void setSubtreeClickAvailable(brls::View* v, bool available) {
    if (!v) return;
    for (const auto& a : v->getActions()) {
        if (a->getType() == brls::ActionType::ACTION_GAMEPAD &&
            a->getButton() == brls::ControllerButton::BUTTON_A) {
            v->setActionAvailable(brls::ControllerButton::BUTTON_A, available);
            break;
        }
    }
    if (auto* box = dynamic_cast<brls::Box*>(v)) {
        for (brls::View* child : box->getChildren())
            setSubtreeClickAvailable(child, available);
    }
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

// The OSD's top bar and bottom scrim are not children of the control boxes, so they must be raised and lowered too.
void PlayerActivity::setVideoOsdChromeVisible(bool visible) {
    if (!m_videoOsd) return;
    const auto vis = visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE;
    for (const char* id : {"player/osd_top", "player/osd_bottom_scrim"}) {
        if (brls::View* v = getView(id)) {
            v->setAlpha(visible ? 1.0f : 0.0f);
            v->setVisibility(vis);
        }
    }
    setSubtreeClickAvailable(getView("player/osd_top"), visible);
}

void PlayerActivity::showControls() {
    m_controlsVisible = true;
    resetControlsIdleTimer();
    setSubtreeClickAvailable(controlsBox, true);
    setSubtreeClickAvailable(centerControls, true);
    if (controlsBox) {
        controlsBox->setAlpha(1.0f);
        controlsBox->setVisibility(brls::Visibility::VISIBLE);
    }
    if (centerControls) {
        centerControls->setAlpha(1.0f);
        centerControls->setVisibility(brls::Visibility::VISIBLE);
    }
    setVideoOsdChromeVisible(true);
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
    if (!controlsCanHide()) return;

    m_controlsVisible = false;
    if (controlsBox) {
        controlsBox->setAlpha(0.0f);
        controlsBox->setVisibility(brls::Visibility::GONE);
    }
    if (centerControls) {
        centerControls->setAlpha(0.0f);
        centerControls->setVisibility(brls::Visibility::GONE);
    }
    setVideoOsdChromeVisible(false);
    // Focus stays where it was, so hand A back to the activity or the press opens an overlay over a hidden OSD.
    setSubtreeClickAvailable(controlsBox, false);
    setSubtreeClickAvailable(centerControls, false);
}

// ─── MPV stats overlay: once a second, enough to diagnose choppy playback without adb (see DESIGN_NOTES) ───
void PlayerActivity::updateMpvStatsOverlay() {
    bool wanted = Application::getInstance().getSettings().showMpvStats;

    // Tear down when the toggle is off so we don't pay layout / paint cost while the user isn't looking at stats.
    if (!wanted) {
        if (m_mpvStatsBox && playerContainer) {
            playerContainer->removeView(m_mpvStatsBox);
            m_mpvStatsBox   = nullptr;
            m_mpvStatsLabel = nullptr;
        }
        return;
    }

    if (!m_mpvStatsBox && playerContainer) {
        m_mpvStatsBox = new brls::Box();
        m_mpvStatsBox->setPositionType(brls::PositionType::ABSOLUTE);
        m_mpvStatsBox->setPositionLeft(16);
        m_mpvStatsBox->setPositionTop(16);
        m_mpvStatsBox->setPadding(10);
        m_mpvStatsBox->setCornerRadius(6);
        m_mpvStatsBox->setBackgroundColor(nvgRGBA(0, 0, 0, 180));
        m_mpvStatsLabel = new brls::Label();
        m_mpvStatsLabel->setFontSize(13);
        m_mpvStatsLabel->setTextColor(nvgRGB(220, 220, 220));
        m_mpvStatsBox->addView(m_mpvStatsLabel);
        playerContainer->addView(m_mpvStatsBox);
    }
    if (!m_mpvStatsLabel) return;

    MpvPlayer& p = MpvPlayer::getInstance();
    if (!p.isInitialized()) {
        m_mpvStatsLabel->setText("MPV not initialized");
        return;
    }

    // mpv returns "" for missing properties and "yes"/"no" for flags; both go straight into the label.
    auto get = [&p](const char* name) -> std::string {
        std::string v = p.getProperty(name);
        return v.empty() ? std::string("?") : v;
    };

    // Format cache-speed as switchfin does so units match the logs; snprintf, since fmt may not be linked here.
    auto fmtSpeed = [&]() -> std::string {
        std::string raw = p.getProperty("cache-speed");
        if (raw.empty()) return "?";
        long long bps = 0;
        try { bps = std::stoll(raw); } catch (...) { return raw; }
        char buf[32];
        if (bps >> 20 > 0) snprintf(buf, sizeof(buf), "%.1f MB/s", bps / 1048576.0);
        else if (bps >> 10 > 0) snprintf(buf, sizeof(buf), "%.1f KB/s", bps / 1024.0);
        else snprintf(buf, sizeof(buf), "%lld B/s", bps);
        return std::string(buf);
    };

    // mpv reports cache depth to microsecond precision; one decimal is plenty.
    auto fmtCacheSecs = [&]() -> std::string {
        std::string raw = p.getProperty("demuxer-cache-time");
        if (raw.empty()) return "?";
        try {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f", std::stod(raw));
            return std::string(buf);
        } catch (...) { return raw; }
    };

    std::string body;
    body.reserve(256);

    // Audio-only leaves the video properties unset, which read as a column of "?"; ask mpv, not m_isQueueMode.
    const bool hasVideo = !p.getProperty("video-codec").empty();

    if (hasVideo) {
        body += "Codec: " + get("video-codec")  + " | HW: " + get("hwdec-current") + "\n";
        body += "Source: " + get("width") + "x" + get("height")
              + " @ " + get("container-fps") + " fps"
              + " | Bitrate: " + get("video-bitrate") + "\n";
        body += "Render: " + get("estimated-vf-fps") + " fps"
              + " | Display: " + get("estimated-display-fps") + " fps\n";
        body += "Drops: " + get("decoder-frame-drop-count") + " decoder, "
                         + get("frame-drop-count") + " vo\n";
    } else {
        body += "Codec: " + get("audio-codec-name") + " | AO: " + get("current-ao") + "\n";
        body += "Source: " + get("audio-params/samplerate") + " Hz, "
              + get("audio-params/channels")
              + " | Bitrate: " + get("audio-bitrate") + "\n";
        body += "Output: " + get("audio-out-params/samplerate") + " Hz, "
              + get("audio-out-params/channels") + ", "
              + get("audio-out-params/format") + "\n";
    }

    body += "Cache: " + fmtCacheSecs() + " s / " + fmtSpeed()
          + " | Paused: " + get("paused-for-cache");
    m_mpvStatsLabel->setText(body);
}

} // namespace vitaplex
