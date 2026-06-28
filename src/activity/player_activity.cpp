/**
 * VitaPlex - Player Activity implementation
 */

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_queue.hpp"
#include "app/plex_palette.hpp"
#include "app/synclounge_session.hpp"
#include "player/mpv_player.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "utils/pip.h"
#include "view/video_view.hpp"
#include "platform/platform.hpp"
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

// Base temp file path for streaming audio (MPV's HTTP handling crashes on Vita)
// Extension will be added dynamically based on the actual file type


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

PlayerActivity* PlayerActivity::createWithQueue(const std::vector<MediaItem>& tracks, int startIndex) {
    PlayerActivity* activity = new PlayerActivity("", false);
    activity->m_isQueueMode = true;

    MusicQueue& queue = MusicQueue::getInstance();

    // Try to create a server-side play queue when online
    // Uses the parent ratingKey (album/season) or the first track's ratingKey
    bool serverOk = false;
    if (!tracks.empty() && !PlexClient::getInstance().getServerUrl().empty()) {
        PlexClient& client = PlexClient::getInstance();
        PlexClient::PlayQueueContainer pq;

        // Determine queue type
        std::string queueType = "audio";
        if (!tracks.empty() && (tracks[0].mediaType == MediaType::EPISODE ||
                                 tracks[0].mediaType == MediaType::MOVIE)) {
            queueType = "video";
        }

        // Build URI from parent ratingKey (album/season) if available,
        // otherwise from first track
        std::string uri;
        if (!tracks[0].parentRatingKey.empty()) {
            uri = client.buildPlayQueueDirectoryURI(tracks[0].parentRatingKey);
        } else if (tracks.size() == 1) {
            uri = client.buildPlayQueueURI(tracks[0].ratingKey);
        } else {
            // Multiple tracks without parent - use first track's URI
            uri = client.buildPlayQueueURI(tracks[0].ratingKey);
        }

        std::string startKey = (startIndex >= 0 && startIndex < (int)tracks.size())
            ? tracks[startIndex].ratingKey : "";

        serverOk = client.createPlayQueue(uri, queueType, pq, startKey);
        if (serverOk && !pq.items.empty()) {
            queue.setFromPlayQueue(pq, pq.playQueueShuffled);
            brls::Logger::info("PlayerActivity: Server play queue {} created ({} items)",
                               pq.playQueueID, pq.playQueueTotalCount);
        }
    }

    if (!serverOk) {
        // Offline or server failed - use client-side queue
        queue.setQueue(tracks, startIndex);
    }

    // Set up track ended callback
    queue.setTrackEndedCallback([activity](const QueueItem* nextTrack) {
        activity->onTrackEnded(nextTrack);
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

    // The up-next list scrolls CENTERED so D-pad Up/Down always change focus and
    // scroll immediately. The default NATURAL only moves focus once the next row
    // is already fully on screen, so UP/DOWN couldn't reach an off-screen track.
    // CENTERED clamps at the ends, so the first/last tracks still sit flush.
    if (queueScroll)
        queueScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    // We're on screen now — suppress the SyncLounge auto-join prompt (the
    // in-player auto-load follows content changes instead).
    s_active.store(true);

#ifdef __vita__
    // Boost CPU/GPU clocks to max for smooth media playback
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif

    // Cancel pending background thumbnail loads (HomeTab, MediaDetailView)
    // to free up network bandwidth for media streaming.
    // We don't setPaused(true) here yet because music queue mode needs to
    // load album art first. setPaused is called later in loadMedia/loadFromQueue
    // right before MPV starts streaming.
    ImageLoader::cancelAll();

    // If music is currently playing in the background and we're starting
    // a non-queue playback (video/episode), stop the music first.
    // Send a "stopped" timeline so the server clears the music session.
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
            // Watch party: only the host can scrub. Block a follower's drag (the
            // 1s progress tick snaps the thumb back to the synced position).
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
            // For music queue mode, prefer Plex API duration (full track length)
            // over MPV duration which may only reflect buffered/demuxed portion
            if (m_isQueueMode) {
                const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
                if (track && track->duration > 0)
                    duration = (double)track->duration;
            }
            if (duration <= 0)
                duration = player.getDuration();
            // Direct play / local / music seek locally & instantly (mpv has the
            // data). A transcoded video routes through the debounced, transcode-
            // aware path so a big scrub restarts the transcode at the target
            // instead of stalling mpv on un-transcoded segments. The slider maps
            // [0,1] onto the real media length so it can't scrub past the end.
            if (m_isLocalFile || m_isDirectFile || m_isQueueMode || m_directPlay) {
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

    // Android TV remote alias: GUIDE re-dispatches as START so the
    // Menu key (which surfaces as BUTTON_GUIDE) opens the OSD too,
    // not just the gamepad-only Start button. MainActivity has the
    // mirror handler at the tab-frame level for when this activity
    // isn't on top — but the activity stack means parent-walk doesn't
    // cross activities, so PlayerActivity needs its own.
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

    // Picture-in-Picture: toggle on right-stick click and via a touchable OSD
    // button (for phones with no gamepad). Only shown in video mode, and only
    // registered on platforms where PiP is implemented (Android + desktop
    // SDL2).
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

        // D-pad left/right seek when controls are hidden (for TV remotes
        // without shoulder buttons). When controls are visible, return false
        // so D-pad navigation between buttons works normally — but reset
        // the idle timer first so the OSD doesn't fade out from under
        // the user while they're navigating between controls (the
        // Android TV bug: "OSD closes while I'm still moving").
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

    // D-pad up/down: when controls are HIDDEN, summon them — this is
    // how Android TV remotes (which have no BUTTON_START) reopen the
    // OSD after it auto-hides. When controls are VISIBLE, reset the
    // idle timer and return false so up/down focus navigation between
    // transport rows / buttons keeps working without the OSD timing
    // out mid-press ("odd colors / closes while user is still
    // moving"). Registered for BOTH music and video modes.
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

    // Queue overlay dismiss on tap - attach to the scrim only so taps on the
    // sheet panel (rows, header buttons) don't close the overlay.
    if (queueScrim) {
        queueScrim->addGestureRecognizer(new brls::TapGestureRecognizer(
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

        // Rescale the album cover to fit the current viewport (e.g.
        // make it large on a portrait phone where the XML default of
        // 220x220 would float lost in the middle), and keep it sized
        // correctly across rotations. The captured weak ref guards
        // against the orientation callback firing after the activity
        // has been popped.
        applyMusicLayoutForViewport();
        std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
        platform::onOrientationChanged([this, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;
            applyMusicLayoutForViewport();
        });

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

        // Queue side-sheet "Clear" control (wired once; lives in the hidden overlay)
        if (queueClearBtn) {
            queueClearBtn->registerClickAction([this](brls::View* view) {
                clearUpcoming();
                return true;
            });
            queueClearBtn->addGestureRecognizer(new brls::TapGestureRecognizer(queueClearBtn));
            // Clear is the top of the sheet's focusable content; route UP back to
            // itself so it can't escape to the player's queue button behind the
            // overlay (which is what happens with the default upward traversal).
            queueClearBtn->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
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

    // Block upward D-pad navigation from center transport controls so focus
    // doesn't escape to off-screen elements (absolutely-positioned overlays)
    if (!m_isQueueMode) {
        if (playBtn) playBtn->setCustomNavigationRoute(brls::FocusDirection::UP, playBtn);
        if (rewindBtn) rewindBtn->setCustomNavigationRoute(brls::FocusDirection::UP, rewindBtn);
        if (forwardBtn) forwardBtn->setCustomNavigationRoute(brls::FocusDirection::UP, forwardBtn);
    }

    // Block downward D-pad navigation from the bottom button row so focus
    // doesn't escape to off-screen elements (absolutely-positioned overlays)
    // Only set on focusable buttons — in music mode audioBtn/subBtn/videoBtn are non-focusable
    if (queueBtn) queueBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, queueBtn);
    if (!m_isQueueMode) {
        if (audioBtn) audioBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, audioBtn);
        if (subBtn) subBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, subBtn);
        if (videoBtn) videoBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, videoBtn);
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

    // Debounced transcode-seek commit (see seek() / requestTranscodeSeek). The
    // `finished` flag is false when we stop() the timer early (teardown / content
    // switch), so a cancelled debounce never commits a stale seek.
    m_seekCommitTimer.setEndCallback([this](bool finished) {
        if (finished && !m_destroying) commitTranscodeSeek();
    });

    // Start with controls hidden if auto-hide is enabled
    int autoHide = Application::getInstance().getSettings().controlsAutoHideSeconds;
    if (autoHide > 0 && !m_isPhoto) {
        hideControls();
    }
}

void PlayerActivity::setBackgroundTransparent(bool transparent) {
#ifdef __ANDROID__
    // Toggled by the audio/video branches in loadMedia / loadFromQueue
    // and reset on willDisappear. The opaque restore value must match the
    // all-Plex background (palette::bg) that applyTheme() installs, so the
    // GL clear can't leave a stale cool-grey behind once video ends. Both
    // tables are updated for symmetry; only the active one is rendered.
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

    // Cancel any pending debounced seek so its commit can't fire after we leave
    // (stop() passes finished=false, which the end callback ignores).
    m_seekCommitTimer.stop();
    m_seekTargetMs = -1.0;

    // Left the player — re-enable the SyncLounge auto-join prompt.
    s_active.store(false);

    // Always restore the opaque clear when leaving the player so the
    // rest of the app (library, settings, etc.) renders with its normal
    // dark background instead of showing through to whatever sits
    // behind the SDL surface.
    setBackgroundTransparent(false);

    // Clear "video playing" state so onUserLeaveHint stops auto-triggering PiP
    // once the user is no longer in the player activity.
    pip::setVideoPlaybackState(false, 0, 0);

    // Re-enable background thumbnail loading now that playback is ending
    ImageLoader::setPaused(false);

    // SyncLounge: we're leaving the player, so we're no longer playing this
    // item — stop our outbound mediaUpdates from carrying stale media.
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

    // If the queue side sheet is open, rebuild it for the new current track
    // (Now Playing + Up Next) on the next frame so it updates immediately.
    if (m_queueOverlayVisible) {
        brls::sync([this]() {
            if (m_queueOverlayVisible) populateQueueList();
        });
    }

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
            artistLabel->setVisibility(track->artist.empty()
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
                std::string thumbUrl = client.getThumbnailUrl(track->thumb, 300, 300);
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
        // Only show the artist label if there's actually text to display
        artistLabel->setVisibility(track->artist.empty()
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    // Update queue info display
    updateQueueDisplay();

    // Use the rating key to get the playback URL
    m_mediaKey = track->ratingKey;
    std::string url;

    // Pause image loading and invalidate stale in-flight loads from previous
    // pages before queuing any new loads for this track.
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
        if (!client.getTranscodeUrl(track->ratingKey, url, 0)) {
            brls::Logger::error("Failed to get transcode URL for track: {}", track->ratingKey);
            m_loadingMedia = false;
            return;
        }

        // Load album art from server - temporarily unpause so loadAsync
        // accepts the request, then re-pause to block other page loads.
        // The async worker no longer checks pause, so the load will complete.
        if (albumArt && !track->thumb.empty()) {
            PlexClient& artClient = PlexClient::getInstance();
            std::string thumbUrl = artClient.getThumbnailUrl(track->thumb, 300, 300);
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

    // Only clear image cache on first MPV init to free memory for the player.
    // On subsequent track changes MPV is already allocated, and clearing the
    // cache forces covers/queue thumbnails to be re-downloaded from the server.
    if (!player.isInitialized()) {
        ImageLoader::clearCache();
    }

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

    // A content switch supersedes any pending debounced seek from the old item;
    // clear the cached duration so a stale value can't bleed into the next item
    // (the video path re-sets it from Plex metadata below).
    m_seekCommitTimer.stop();
    m_seekTargetMs = -1.0;
    m_mediaDurationMs = 0;
    m_syncRecoverAttempts = 0;
    m_directPlay = false;

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
        setBackgroundTransparent(!isAudioTrack);

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
            m_grandparentRatingKey = item.grandparentRatingKey;
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

        // SyncLounge: report what we're playing so our outbound mediaUpdates
        // carry real media (server/party show the title, with show+season for
        // episodes); and re-arm the announce-once for this item.
        SyncLoungeSession::instance().setLocalMedia(item.title, item.type, m_mediaKey,
                                                    item.grandparentTitle, item.parentTitle);
        m_syncLoungeAnnounced = false;

        // Handle photos differently - display image instead of playing
        if (item.mediaType == MediaType::PHOTO) {
            brls::Logger::info("Displaying photo: {}", item.title);
            m_isPhoto = true;
            m_loadingMedia = false;

            // Load the full-size photo
            if (!item.thumb.empty()) {
                const auto& photoIc = platform::getImageConstraints();
                std::string photoUrl = client.getThumbnailUrl(item.thumb, photoIc.photoRequestWidth, photoIc.photoRequestHeight);
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
                int artSize = platform::getImageConstraints().squareRequestSize;
                std::string thumbUrl = client.getThumbnailUrl(artPath, artSize, artSize);
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

        // SyncLounge: when following a watch party and we have a recent host
        // position, open at the host's timecode (baked into the transcode
        // offset) instead of our own resume point — so playback starts already
        // in sync and there's no big corrective seek after load. Extrapolate a
        // playing host forward by how long ago it reported. Same-content
        // assumed; ignored if the host state is stale (> 60s) or out of range.
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
                        // Suppress an immediate redundant correction in the
                        // follow loop now that we open in sync.
                        m_lastSyncSeek = std::chrono::steady_clock::now();
                        brls::Logger::info(
                            "PlayerActivity: SyncLounge follow — starting at host offset {}ms (age {}ms)",
                            resumeOffset, (long)ageMs);
                    }
                }
            }
        }
        m_transcodeBaseOffsetMs = resumeOffset;
        // Remember Plex's authoritative full length so the seek bar and seek
        // clamps don't rely on mpv's duration, which reads stale right after a
        // transcode reload and used to let the bar/offset run past the end.
        m_mediaDurationMs = (item.duration > 0) ? (int)item.duration : 0;
        std::string url;
        if (client.getTranscodeUrl(m_mediaKey, url, resumeOffset)) {
            // Direct play: the server returned the original file (not an HLS
            // transcode). mpv owns the timeline, so play from 0 and seek to the
            // resume point; seeks become local/instant instead of transcode
            // restarts. Detected from the URL (transcode URLs hit start.m3u8).
            m_directPlay = (url.find("/transcode/universal/start") == std::string::npos);
            if (m_directPlay) {
                brls::Logger::info("PlayerActivity: direct play (original file), resume {}ms",
                                   resumeOffset);
                m_transcodeBaseOffsetMs = 0;
                if (resumeOffset > 0) m_pendingSeek = resumeOffset / 1000.0;
            }
            // Pause image loading and free cache memory before initializing MPV.
            // This stops background thumbnail fetches from competing with media
            // streaming, and frees memory (Vita only has 256MB).
            ImageLoader::setPaused(true);
            ImageLoader::cancelAll();
            ImageLoader::clearCache();

            MpvPlayer& player = MpvPlayer::getInstance();

            // Set audio-only mode BEFORE initializing
            player.setAudioOnly(isAudioContent);
            setBackgroundTransparent(!isAudioContent);

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

    // Tick the diagnostic overlay every second alongside progress.
    // No-op when the toggle is off; lazy-creates the views the first
    // time it sees the toggle on so the panel doesn't sit in the view
    // tree consuming layout passes when the user isn't using it.
    updateMpvStatsOverlay();

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
        setBackgroundTransparent(!isAudio);

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
                // Mark video playback state so Android auto-enters PiP when
                // the user hits Home. Only for actual video (not music) and
                // not when running the queue-mode (music) player.
                if (!isAudio && !m_isQueueMode) {
                    pip::setVideoPlaybackState(true, 16, 9);
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

    // While a debounced seek is pending, showSeekPreview owns the slider + time
    // labels (they point at the target, not the live position) — don't stomp it.
    if (duration > 0 && m_seekTargetMs < 0.0) {
        // For transcoded streams with a resume offset, MPV position/duration are
        // relative to the stream start (offset point). Compute absolute values
        // for correct UI display.
        double baseOffsetSec = m_transcodeBaseOffsetMs / 1000.0;
        double absPosition = baseOffsetSec + position;
        // Prefer Plex's authoritative length; baseOffset + mpv duration reads
        // stale right after a transcode reload and would inflate the bar.
        double absDuration = (m_mediaDurationMs > 0) ? (m_mediaDurationMs / 1000.0)
                                                     : (baseOffsetSec + duration);
        if (absDuration > 0.0 && absPosition > absDuration) absPosition = absDuration;

        if (progressSlider && absDuration > 0.0) {
            m_updatingSlider = true;
            progressSlider->setProgress((float)(absPosition / absDuration));
            m_updatingSlider = false;
        }

        // Update time labels: elapsed on left, remaining on right
        // Format as H:MM:SS when duration >= 1 hour, otherwise M:SS
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

    // ── SyncLounge: watch-party sync ──────────────────────────────────────
    // When connected to a room: if we're host, broadcast our transport state
    // and announce our media so the party follows us; otherwise switch to the
    // host's matched content (if different), announce ours, and mirror the
    // host's play/pause/seek.
    if (duration > 0 && SyncLoungeSession::instance().isConnected()) {
        SyncLoungeSession& sl = SyncLoungeSession::instance();
        // Sanity-check the local position against Plex's REAL duration, not mpv's:
        // a corrupt transcode can spike mpv's PTS so both position and duration
        // read as a bogus huge value (~25h), which the old position<=duration
        // check happily passed — then the host-follow re-seeks forever. Compare
        // the absolute position to the authoritative length instead.
        const double baseSec0   = m_transcodeBaseOffsetMs / 1000.0;
        const double absPosSec0  = baseSec0 + position;
        const double realDurSec = (m_mediaDurationMs > 0) ? (m_mediaDurationMs / 1000.0)
                                                          : (baseSec0 + duration);
        const bool localSane = position >= 0.0 && absPosSec0 <= realDurSec + 30.0;

        // Party pause: when enabled, ANY member can pause/resume the whole
        // party. Apply the latest action once (independent of host/follower).
        {
            auto pp = sl.partyPauseState();
            if (pp.seq != m_lastPartyPauseSeq) {
                m_lastPartyPauseSeq = pp.seq;
                if (pp.isPause && player.isPlaying())      player.pause();
                else if (!pp.isPause && player.isPaused()) player.play();
            }
        }

        // Follower content-switch FIRST: if the host is on different media than
        // we are, switch to the matched local item BEFORE announcing — so
        // opening content that differs from the host doesn't briefly announce /
        // claim the wrong item. Exact match + fresh host + remote Plex only;
        // loadMedia() then opens it at the host's position (see #1 above).
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

        // Announce our media once per loaded item so the party/server shows
        // the right title even before any pause/play. Claims host only for a
        // user-initiated NEW video (auto-loaded follows pass claimHost=false).
        // `duration` is already > 0 here, so the values are valid.
        if (localSane && !m_syncLoungeAnnounced) {
            const char* ast = player.isPlaying() ? "playing"
                            : player.isPaused()  ? "paused" : nullptr;
            if (ast) {
                // Only claim host for genuinely NEW content. If we're opening the
                // same item the host is already playing — e.g. clicking "Join"
                // on the auto-join prompt — we're joining to follow, not taking
                // over, so never claim host (even with room auto-host on).
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
            // Host: publish our absolute timecode (matches the /:/timeline
            // basis: transcode base offset + mpv position). Throttled inside
            // reportLocalState.
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
            // Only follow a RECENT host state. If the host's updates stop (they
            // left or their connection dropped) the last state freezes; without
            // this we'd keep seeking back to that stale frozen position forever.
            bool hostFresh = rs.valid &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - rs.at).count() < 12000;
            if (hostFresh && (rs.state == "playing" || rs.state == "paused")) {
                if (!localSane) {
                    // mpv's position is garbage — a corrupt transcode PTS spiked
                    // it past the real end. An mpv-local seek can't escape it (it
                    // lands right back in the bad segments and the host-follow
                    // re-seeks forever), so restart the transcode at the host
                    // offset for fresh segments. Hard rate-limit + a small cap so
                    // a stream that won't recover doesn't loop indefinitely.
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

                    // Correct large drift, but rate-limit hard. Seeking an HLS
                    // transcode restarts it and the position takes several
                    // seconds to settle, so a per-second re-seek would thrash
                    // (the host also keeps advancing while we rebuffer). One
                    // seek, then an 8s cooldown so it can settle before we
                    // reconsider; a 10s threshold tolerates residual skew.
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

    // Live TV keep-alive: each /:/timeline ping resets the server's 300-sec
    // rolling-subscription stop-grab timer. Without it the grab dies after
    // ~5 min, the universal transcode session that's fed by it gets killed,
    // and mpv starts getting 404s on the next playlist refresh.
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
            int durationMs = (m_mediaDurationMs > 0) ? m_mediaDurationMs : (int)(duration * 1000);

            // A corrupt transcode can spike mpv's position past the real end.
            // Posting that 400s on Plex and would poison the saved resume point
            // (next open would try to resume hours in), so skip until it's sane.
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

    // Detect playback end: check hasEnded() regardless of m_isPlaying
    // to avoid missing the event if m_isPlaying was synced to false
    // in a previous frame before the ENDED state was set.
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
                // Find current season's parentIndex, then look for next season
                // Current season's parentIndex is stored in item.parentIndex during loadMedia
                // but we can find it by matching seasonKey
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

    // SyncLounge: a manual play/pause is a user action — announce it (and,
    // under auto-host, claim host so the party follows the Vita).
    double absMs = m_transcodeBaseOffsetMs + player.getPosition() * 1000.0;
    syncLoungeReportUserAction(m_isPlaying ? "playing" : "paused", absMs);

    // When party-pausing is enabled, a manual pause/resume drives the whole
    // party (any member can). Gated inside sendPartyPause (only sent when
    // enabled — the server disconnects an unsolicited sender).
    SyncLoungeSession& slpp = SyncLoungeSession::instance();
    if (slpp.isConnected() && slpp.isPartyPauseEnabled())
        slpp.sendPartyPause(!m_isPlaying);
}

void PlayerActivity::syncLoungeReportUserAction(const std::string& state, double absTimeMs) {
    SyncLoungeSession& sl = SyncLoungeSession::instance();
    if (!sl.isConnected()) return;
    // mpv's position/duration can read NaN/0 for a moment right at a state
    // change; never broadcast that (the server renders it as NaN:NaN). Skip
    // the announce until the readings are valid — the periodic host broadcast
    // (and the next action) cover the gap.
    double durMs = MpvPlayer::getInstance().getDuration() * 1000.0;
    if (!std::isfinite(absTimeMs) || absTimeMs < 0.0) return;
    if (!std::isfinite(durMs)    || durMs    <= 0.0) return;
    // Pause/play/seek announce state but never claim host — only starting a
    // new video does (the announce-on-load above).
    sl.announceLocalMedia(state, absTimeMs, durMs, /*claimHost=*/false);
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

    // In a watch party only the host drives playback — a follower's manual
    // fast-forward/rewind would just desync and get pulled back to the host, so
    // block it outright.
    {
        auto& sl = SyncLoungeSession::instance();
        if (sl.isConnected() && !sl.isHost()) {
            player.showOSD("Only the host can seek", 1.5);
            return;
        }
    }

    // Direct play, local file, or music (mp3, not HLS): mpv has the data, so
    // seek locally and instantly — these never stall the way a forward seek on
    // an HLS transcode does.
    if (m_isLocalFile || m_isDirectFile || m_isQueueMode || m_directPlay) {
        double targetSec = std::max(0.0, player.getPosition() + seconds);
        player.seekRelative(seconds);
        // SyncLounge: a manual seek is a user action — announce where we're going.
        syncLoungeReportUserAction(player.isPaused() ? "paused" : "playing",
                                   m_transcodeBaseOffsetMs + targetSec * 1000.0);
        return;
    }

    // Transcoded HLS video: fold this skip into the pending absolute target and
    // debounce, so a burst of presses commits ONCE. commitTranscodeSeek() then
    // decides local-seek vs transcode-restart. Accumulate from the pending
    // target if one is in flight, else from the current absolute position.
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
    // Stop the current transcode and start a fresh one at offsetMs so Plex
    // re-encodes from there. Used for far seeks and to escape a corrupt stream
    // (bad-PTS / un-transcoded segments) that an mpv-local seek can't get out of.
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
    // Clamp to the real media length so a stale mpv duration (right after a
    // reload) can't push the target past the end.
    double totalMs = knownDurationMs();
    if (absMs < 0.0) absMs = 0.0;
    if (totalMs > 0.0 && absMs > totalMs) absMs = totalMs;
    m_seekTargetMs = absMs;

    // Show where we're heading while the debounce settles (otherwise the ~350 ms
    // wait looks like a frozen UI).
    showSeekPreview(absMs, totalMs);

    // (Re)arm the debounce: rewind resets the countdown without firing, so the
    // commit happens once, ~350 ms after the last press/scrub.
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

    // The current transcode's m3u8 covers [baseMs, total], so anything already
    // behind the play head is on hand and cheap to seek locally. Only a jump
    // before the transcode's start, or one far ahead of the play head (beyond
    // what Plex has likely produced), is worth a restart.
    static constexpr double kLocalForwardMaxMs = 60000.0;  // 60 s; tune to taste
    bool mustRestart = (target < baseMs) ||
                       ((target - curAbs) > kLocalForwardMaxMs);

    if (!mustRestart) {
        player.seekTo(std::max(0.0, (target - baseMs) / 1000.0));
    } else {
        // Tell Plex to re-transcode from the target instead of making mpv crawl
        // across un-transcoded HLS. Mirrors the audio/subtitle reload path.
        player.showOSD("Seeking…", 1.5);  // "Seeking…"
        if (!restartTranscodeAtMs((int)target) && target >= baseMs) {
            // Restart failed — fall back to a best-effort local seek.
            player.seekTo(std::max(0.0, (target - baseMs) / 1000.0));
        }
    }

    syncLoungeReportUserAction(player.isPaused() ? "paused" : "playing", target);
}

void PlayerActivity::showSeekPreview(double absMs, double totalMs) {
    // Move the slider to the pending spot (guarded so it doesn't re-trigger the
    // seek subscription) and reflect the target time in the labels + a short OSD.
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
    shuffleIcon->setImageFromRes(queue.isShuffleEnabled()
        ? "icons/shuffle-variant.png" : "icons/shuffle-disabled.png");
}

void PlayerActivity::applyMusicLayoutForViewport() {
    if (!albumArt) return;

    // Compute a target cover size that fills the available width
    // without crowding the controls. The XML defaults to 220×220 which
    // is right for Vita / Switch landscape; on a portrait phone we want
    // something closer to ~55% of the viewport width so the cover
    // actually fills the space instead of floating tiny in the middle.
    // Cap the height to ~45% of the viewport so the music_info and
    // music_transport rows below it still have somewhere to land.
    float vw = platform::viewportWidth();
    float vh = platform::viewportHeight();
    if (vw <= 0 || vh <= 0) return;

    float byWidth  = vw * 0.55f;
    float byHeight = vh * 0.45f;
    float target   = std::min(byWidth, byHeight);

    // Don't shrink below the original 220px design size — every
    // platform has at least enough room for that on landscape.
    if (target < 220.f) target = 220.f;
    // And don't blow up beyond 480 in either direction; pushed any
    // bigger the cover starts dominating the layout on big tablets
    // and the controls feel orphaned at the bottom.
    if (target > 480.f) target = 480.f;

    albumArt->setWidth(target);
    albumArt->setHeight(target);
}

void PlayerActivity::updateRepeatIcon() {
    if (!repeatIcon) return;
    switch (MusicQueue::getInstance().getRepeatMode()) {
        case RepeatMode::OFF: repeatIcon->setImageFromRes("icons/repeat-off.png");  break;
        case RepeatMode::ALL: repeatIcon->setImageFromRes("icons/repeat.png");      break;
        case RepeatMode::ONE: repeatIcon->setImageFromRes("icons/repeat-once.png"); break;
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

    // Refresh the queue side sheet if it's open. Rebuild when the queue
    // version changed (size/order/shuffle) or when the song advanced -
    // playTrack/playNext don't bump the version, so the current index is
    // watched separately to keep Now Playing + Up Next in sync.
    if (m_queueOverlayVisible && queueList && !m_queueBatchActive) {
        if (m_cachedQueueVersion != queue.getVersion() ||
            m_lastRenderedCurrentIndex != queue.getCurrentIndex()) {
            populateQueueList();
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

    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::VISIBLE);

        queueOverlay->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
            // While a track is grabbed, B drops it (keeps the queue open) rather
            // than closing — press again to close.
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
        // START picks up / drops the focused track for reorder. On Android TV
        // hold-center fires GUIDE, which the player's GUIDE handler re-dispatches
        // to the focused view's START action — landing here — so bare D-pad
        // remotes (no L/R bumpers) can reorder too.
        queueOverlay->registerAction("Move", brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
            toggleQueueGrab();
            return true;
        });
        // While grabbed, Up/Down move the held track; otherwise return false so
        // borealis does its normal list navigation (it only navigates when the
        // button isn't consumed by an action).
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

    // Rebuild when the queue changed (version) or when the song advanced -
    // playTrack/playNext don't bump the version, so we also watch the current
    // index. Otherwise reuse the cached rows for an instant reopen.
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
    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::GONE);
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
    row->setHeight(52);
    row->setPaddingLeft(10);
    row->setPaddingRight(10);
    row->setCornerRadius(9);
    row->setFocusable(true);
    row->setMarginBottom(2);
    row->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

    // Drag-handle glyph (3 stacked bars) - a visual affordance for reordering
    brls::Box* grip = new brls::Box();
    grip->setAxis(brls::Axis::COLUMN);
    grip->setJustifyContent(brls::JustifyContent::CENTER);
    grip->setAlignItems(brls::AlignItems::CENTER);
    grip->setWidth(14);
    grip->setMarginRight(8);
    for (int b = 0; b < 3; b++) {
        brls::Box* bar = new brls::Box();
        bar->setWidth(12);
        bar->setHeight(2);
        bar->setCornerRadius(1);
        bar->setBackgroundColor(nvgRGB(138, 138, 144));
        if (b < 2) bar->setMarginBottom(3);
        grip->addView(bar);
    }
    row->addView(grip);

    // Cover art thumbnail (38x38), loaded lazily when the row nears the viewport
    brls::Image* thumb = new brls::Image();
    thumb->setWidth(38);
    thumb->setHeight(38);
    thumb->setCornerRadius(6);
    thumb->setScalingType(brls::ImageScalingType::FIT);
    thumb->setMarginRight(11);
    m_deferredThumbs.push_back({thumb, track.thumb, track.ratingKey, false});
    row->addView(thumb);

    // Title (white) over artist (muted), each ellipsized to a single line
    brls::Box* meta = new brls::Box();
    meta->setAxis(brls::Axis::COLUMN);
    meta->setJustifyContent(brls::JustifyContent::CENTER);
    meta->setGrow(1.0f);

    brls::Label* titleLbl = new brls::Label();
    titleLbl->setText(track.title);
    titleLbl->setFontSize(14);
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    meta->addView(titleLbl);

    if (!track.artist.empty()) {
        brls::Label* artistLbl = new brls::Label();
        artistLbl->setText(track.artist);
        artistLbl->setFontSize(12);
        artistLbl->setTextColor(nvgRGB(180, 180, 186));
        artistLbl->setSingleLine(true);
        artistLbl->setMarginTop(1);
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
    durLbl->setFontSize(12);
    durLbl->setTextColor(nvgRGB(138, 138, 144));
    durLbl->setMarginLeft(8);
    row->addView(durLbl);

    // Remove (x) affordance - reserved space, revealed only while focused
    brls::Box* removeBtn = new brls::Box();
    removeBtn->setAxis(brls::Axis::ROW);
    removeBtn->setJustifyContent(brls::JustifyContent::CENTER);
    removeBtn->setAlignItems(brls::AlignItems::CENTER);
    removeBtn->setWidth(24);
    removeBtn->setHeight(24);
    removeBtn->setCornerRadius(6);
    removeBtn->setMarginLeft(6);
    removeBtn->setVisibility(brls::Visibility::INVISIBLE);
    brls::Image* removeIcon = new brls::Image();
    removeIcon->setWidth(12);
    removeIcon->setHeight(12);
    removeIcon->setScalingType(brls::ImageScalingType::FIT);
    removeIcon->setImageFromRes("icons/cross.png");
    removeBtn->addView(removeIcon);
    row->addView(removeBtn);

    // Row -> track mapping (looked up dynamically by the handlers below)
    m_queueRowData[row] = {trackIdx, track.title, removeBtn};

    // Swipe left to remove this track from the queue
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            if (status.state == brls::GestureState::UNSURE || status.state == brls::GestureState::START) {
                float deltaX = status.position.x - status.startPosition.x;
                if (deltaX > 0) { row->setTranslationX(0); row->setAlpha(1.0f); return; }
                row->setTranslationX(deltaX);
                float alpha = 1.0f - std::min(1.0f, std::abs(deltaX) / 200.0f);
                row->setAlpha(std::max(0.2f, alpha));
            } else if (status.state == brls::GestureState::END) {
                float deltaX = status.position.x - status.startPosition.x;
                if (deltaX < -120.0f) {
                    auto it = m_queueRowData.find(row);
                    if (it != m_queueRowData.end()) {
                        int tIdx = it->second.trackIdx;
                        brls::sync([this, tIdx]() { removeQueueTrackByIndex(tIdx); });
                        return;
                    }
                }
                row->setTranslationX(0);
                row->setAlpha(1.0f);
            } else if (status.state == brls::GestureState::FAILED) {
                row->setTranslationX(0);
                row->setAlpha(1.0f);
            }
        }, brls::PanAxis::HORIZONTAL));

    // Vertical pan: hold briefly to drag-reorder, otherwise scroll the list.
    // Touch users get reordering back without an L/R controller; the dragged
    // row follows the finger while neighbours slide out of the way, and the
    // drop commits via moveTrack + rebuild.
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            constexpr float rowH = 54.0f;  // 52 height + 2 margin
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
                    if (queueScroll && std::abs(deltaY) >= 10.0f) {
                        float newOffset = m_dragState.initialScrollY - deltaY;
                        if (newOffset < 0) newOffset = 0;
                        float viewH = queueScroll->getHeight();
                        int n = queueList ? (int)queueList->getChildren().size() : 0;
                        float maxScroll = std::max(0.0f, n * rowH + 8.0f - viewH);
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

                // Auto-scroll while the finger sits near the top/bottom edge of
                // the viewport, so a drag can reach rows past the visible window.
                // STAY fires every frame the finger is held, so this keeps
                // scrolling even when the finger isn't moving.
                if (queueScroll) {
                    constexpr float EDGE = 44.0f;
                    constexpr float SPEED = 9.0f;
                    float viewH = queueScroll->getHeight();
                    float fingerInView = status.position.y - queueScroll->getY();
                    float scrollY = queueScroll->getContentOffsetY();
                    int n = queueList ? (int)queueList->getChildren().size() : 0;
                    float maxScroll = std::max(0.0f, n * rowH + 8.0f - viewH);
                    if (fingerInView > viewH - EDGE && scrollY < maxScroll) {
                        queueScroll->setContentOffsetY(std::min(maxScroll, scrollY + SPEED), false);
                    } else if (fingerInView < EDGE && scrollY > 0) {
                        queueScroll->setContentOffsetY(std::max(0.0f, scrollY - SPEED), false);
                    }
                    // Re-read the scroll offset so the dragged row stays under the
                    // finger and the target index reflects the new scroll position.
                    scrollDelta = queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY;
                    eff = (status.position.y - m_dragState.dragStartY) + scrollDelta;
                    row->setTranslationY(eff);
                }

                int origIdx = m_dragState.originalDisplayIdx;
                int childCount = queueList ? (int)queueList->getChildren().size() : 0;
                int newTarget = origIdx + (int)std::lround(eff / rowH);
                if (newTarget < 0) newTarget = 0;
                if (newTarget > childCount - 1) newTarget = childCount - 1;
                m_dragState.targetDisplayIdx = newTarget;

                if (queueList) {
                    auto& children = queueList->getChildren();
                    for (int i = 0; i < (int)children.size(); i++) {
                        if (i == origIdx) continue;
                        float shift = 0.0f;
                        if (newTarget > origIdx && i > origIdx && i <= newTarget) shift = -rowH;
                        else if (newTarget < origIdx && i >= newTarget && i < origIdx) shift = rowH;
                        children[i]->setTranslationY(shift);
                    }
                }
            } else if (status.state == brls::GestureState::END ||
                       status.state == brls::GestureState::FAILED) {
                bool didDrag = m_dragState.holdMet && m_dragState.draggedRow == row;
                int origIdx = m_dragState.originalDisplayIdx;
                int targetIdx = m_dragState.targetDisplayIdx;
                int fromTrack = m_dragState.draggedTrackIdx;

                if (queueList) for (auto* c : queueList->getChildren()) c->setTranslationY(0);

                bool committed = false;
                if (didDrag && status.state == brls::GestureState::END &&
                    origIdx >= 0 && targetIdx >= 0 && origIdx != targetIdx && fromTrack >= 0) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    bool shuffled = queue.isShuffleEnabled();
                    const auto& sOrder = queue.getShuffleOrder();
                    int targetPlayPos = targetIdx + m_queueWindowStart;
                    int toTrack = (shuffled && targetPlayPos < (int)sOrder.size())
                                    ? sOrder[targetPlayPos] : targetPlayPos;
                    if (toTrack >= 0 && toTrack < queue.getQueueSize() && toTrack != fromTrack) {
                        if (queue.isServerSynced()) {
                            const auto& q = queue.getQueue();
                            int pqItemID = (fromTrack < (int)q.size()) ? q[fromTrack].playQueueItemID : 0;
                            int afterPQItemID = (toTrack > 0 && toTrack - 1 < (int)q.size())
                                                    ? q[toTrack - 1].playQueueItemID : 0;
                            if (pqItemID > 0)
                                PlexClient::getInstance().movePlayQueueItem(
                                    queue.getPlayQueueID(), pqItemID, afterPQItemID);
                        }
                        queue.moveTrack(fromTrack, toTrack);
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
                if (!committed) {
                    row->setBackgroundColor(m_focusedQueueRow == row
                        ? nvgRGB(58, 58, 70) : nvgRGBA(0, 0, 0, 0));
                }

                m_dragState.active = false;
                m_dragState.holdMet = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.scrollPassthrough = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
            }
        }, brls::PanAxis::VERTICAL));

    // Tap / A to play this track (suppressed right after a drag-reorder). While
    // a track is grabbed for reorder, A/OK drops it instead of playing — so
    // click-to-play still works normally whenever nothing is grabbed.
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

    // On focus: reveal this row's remove affordance, tint its background, and
    // lazy-load nearby thumbnails. Restores the previously focused row.
    row->getFocusEvent()->subscribe([this, row](brls::View*) {
        if (m_focusedQueueRow && m_focusedQueueRow != row) {
            m_focusedQueueRow->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
            auto pit = m_queueRowData.find(m_focusedQueueRow);
            if (pit != m_queueRowData.end() && pit->second.removeBtn) {
                pit->second.removeBtn->setVisibility(brls::Visibility::INVISIBLE);
            }
        }
        m_focusedQueueRow = row;
        // Grabbed rows get a gold "lifted" tint (re-applied here so it survives
        // the rebuild + re-focus that each move triggers); otherwise the normal
        // focus tint.
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

    // Park focus on the Clear button before tearing down rows so we never
    // destroy the focused view out from under borealis.
    m_focusedQueueRow = nullptr;
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

    // Position of the current track in *play order* (shuffle position when
    // shuffling). Everything after it is "Up Next".
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

    // Window the render so a huge shuffle-all queue doesn't spawn thousands of
    // rows. Only ever expands toward the end (downward).
    m_queueWindowStart = firstUpNext < 0 ? 0 : firstUpNext;
    m_queueWindowEnd = std::min(count, m_queueWindowStart + QUEUE_RENDER_LIMIT);
    int windowSize = std::max(0, m_queueWindowEnd - m_queueWindowStart);

    if (upcoming <= 0) {
        brls::Label* empty = new brls::Label();
        empty->setText("Nothing up next");
        empty->setFontSize(13);
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
            more->setFontSize(12);
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
            // After a reorder, land on the moved track's new slot; otherwise the
            // first row.
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
                queueNpThumb->setImageFromRes("img/default_music.png");
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
    // The up-next list lives in a ScrollingFrame, which traps UP navigation at
    // its first row. Route UP off that row to the Clear button sitting above the
    // list so focus can escape upward. (Entering the list via DOWN from Clear is
    // not trapped, so it needs no route.) The route points from the ephemeral
    // row to the stable Clear button, so it can't dangle across rebuilds.
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

    // Work in PLAY order. A row's position in the rendered window is its play
    // position (window child index + window start); when shuffled that's a
    // shuffle-order index, otherwise an absolute queue index. The track each row
    // shows is its stored (absolute) trackIdx.
    const int playFrom = m_queueWindowStart + childIdx;
    const int playTo   = m_queueWindowStart + targetChild;
    const int absFrom  = itFrom->second.trackIdx;

    // Server sync: move the item after its new play-order predecessor (the track
    // that will sit just before it). Resolve neighbours through the shuffle order
    // when shuffled so the anchor is the right track, not an absolute neighbour.
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

    // Reorder play order: the shuffle order when shuffled (m_queue untouched),
    // the queue itself otherwise.
    queue.moveInPlayOrder(playFrom, playTo);

    // Swap the two adjacent row views in place — never a full rebuild. This
    // covers every adjacent reorder (mid-list, first row, shuffled), so moving a
    // track (including to the very top) doesn't tear down and rebuild the list:
    // no cover-reload flicker, and the row keeps focus since its view isn't
    // destroyed.
    brls::View* rowFrom = children[childIdx];
    queueList->removeView(rowFrom, /*free=*/false);  // detach, don't destroy
    queueList->addView(rowFrom, targetChild);        // re-insert at new slot
    // Non-shuffled: moveInPlayOrder swapped the two tracks' absolute indices, so
    // swap the rows' stored trackIdx to match. Shuffled: m_queue (and each row's
    // absolute trackIdx) is unchanged, so the rows keep their trackIdx as-is.
    if (!shuffled) std::swap(itFrom->second.trackIdx, itTo->second.trackIdx);
    // Row order (and possibly the first row) changed — keep the UP->Clear escape
    // routes correct around the swap.
    refixQueueUpRoutes(std::min(childIdx, targetChild));
    // The row never lost focus, so giveFocus is a no-op and won't scroll; scroll
    // the moved row into view explicitly so grab-mode hold-to-move follows it.
    brls::Application::giveFocus(rowFrom);
    scrollQueueToChild(targetChild);
    // moveInPlayOrder bumped the queue version; sync our cache so the periodic
    // refresh (which rebuilds on a version change) doesn't tear down and rebuild
    // the list we just hand-edited.
    m_cachedQueueVersion = queue.getVersion();
    m_lastRenderedCurrentIndex = queue.getCurrentIndex();
}

void PlayerActivity::refixQueueUpRoutes(int lo) {
    if (!queueList || !queueClearBtn) return;
    auto& ch = queueList->getChildren();
    // Re-point the rows whose correct upward neighbour could have changed: the
    // two swapped rows (lo, lo+1) and the one just below (lo+2), whose neighbour
    // is now a different object. Row 0 escapes to Clear; the rest point at the
    // row above. Rows outside this window keep their existing (still-correct)
    // routes. Only track rows are focusable / routable — skip the "+N more"
    // label.
    for (int i = std::max(0, lo); i <= lo + 2 && i < (int)ch.size(); i++) {
        if (m_queueRowData.find(ch[i]) == m_queueRowData.end()) continue;
        if (i == 0) {
            ch[0]->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
        } else {
            ch[i]->setCustomNavigationRoute(brls::FocusDirection::UP, ch[i - 1]);
        }
    }
}

void PlayerActivity::scrollQueueToChild(int idx) {
    if (!queueScroll || !queueList || idx < 0) return;
    constexpr float rowH = 54.0f;  // 52 height + 2 margin (matches the rows)
    float viewH = queueScroll->getHeight();
    if (viewH <= 0.0f) return;
    int n = (int)queueList->getChildren().size();
    float maxScroll = std::max(0.0f, n * rowH + 8.0f - viewH);
    // Center the row (clamped at the ends), matching the list's CENTERED nav so
    // a grab-mode move keeps the held row centered as the list scrolls under it.
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
    // Only pick up an actual up-next track row — not the Clear button, the
    // "+N more" label, or the empty-state text.
    brls::View* focused = brls::Application::getCurrentFocus();
    if (!focused || m_queueRowData.find(focused) == m_queueRowData.end())
        return;
    setQueueGrab(true);
}

void PlayerActivity::setQueueGrab(bool on) {
    m_queueGrabActive = on;
    // Lift the grabbed (focused) row with a gold tint, mirroring the sidebar
    // editor's grabbed-row cue; restore the normal focus tint on drop.
    if (m_focusedQueueRow) {
        m_focusedQueueRow->setBackgroundColor(on ? nvgRGBA(229, 160, 13, 90)
                                                  : nvgRGB(58, 58, 70));
    }
    if (on) {
        MpvPlayer::getInstance().showOSD("Moving track \xC2\xB7 Up/Down to move, OK to drop", 2.0);
    }
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

        // Give focus to the current track now that all rows exist — or, after a
        // reorder, to the moved track's new slot.
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

    // Load thumbnails for a window around the given display index
    // Queue scroll is 320px with ~62px rows = ~5 visible rows
    int start = std::max(0, displayIndex - QUEUE_THUMB_BUFFER);
    int end = std::min((int)m_deferredThumbs.size(), displayIndex + QUEUE_THUMB_BUFFER + 6);

    // Temporarily unpause so loadAsync accepts the requests, then re-pause.
    // The async workers no longer check the pause flag, so queued loads
    // will complete even after we re-pause here.
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
        // Swap deferred thumb entries (thumbPath, ratingKey, loaded state)
        std::swap(dtA.thumbPath, dtB.thumbPath);
        std::swap(dtA.ratingKey, dtB.ratingKey);
        std::swap(dtA.loaded, dtB.loaded);
        // Re-point image pointers to their current rows
        dtA.image = thumbA;
        dtB.image = thumbB;
        // Reload thumbnails to reflect the swap (skip during chained swaps
        // to avoid race conditions - caller will reload after all swaps)
        if (!skipThumbReload) {
            PlexClient& swapClient = PlexClient::getInstance();
            if (dtA.loaded && !dtA.thumbPath.empty()) {
                std::string urlA = swapClient.getThumbnailUrl(dtA.thumbPath, 100, 100);
                ImageLoader::loadAsync(urlA, [](brls::Image*) {}, thumbA, m_alive);
            } else {
                thumbA->setImageFromRes("img/default_music.png");
            }
            if (dtB.loaded && !dtB.thumbPath.empty()) {
                std::string urlB = swapClient.getThumbnailUrl(dtB.thumbPath, 100, 100);
                ImageLoader::loadAsync(urlB, [](brls::Image*) {}, thumbB, m_alive);
            } else {
                thumbB->setImageFromRes("img/default_music.png");
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

    // Move the dragged row widget using borealis's own API so the Yoga
    // layout engine properly recalculates positions. The widget keeps
    // its loaded cover texture - no re-fetch needed.
    brls::View* draggedView = children[origIdx];
    queueList->removeView(draggedView, false);  // detach without deleting
    // After removal, indices above origIdx shift down by 1.
    // To land at the correct final position we must adjust:
    // Moving down (orig < target): target was shifted down, so insert at target
    //   (the gap closes above, target-1 is now correct but addView inserts
    //    BEFORE the element at that index, so we still use target)
    // Moving up (orig > target): nothing above target shifted, insert at target
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

    // Update lightweight metadata for each row in the affected range:
    // QueueRowData trackIdx and current-track highlight colors.
    // The view content (cover, title text, artist, duration) moved with
    // the widget - we only fix the metadata mapping.
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
    // Watch party followers don't skip on their own — the host drives and the
    // follower seeks to match. Suppress auto-skip and hide the manual skip
    // button so a follower can't desync past the intro/credits independently.
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
        // Host: announce the skip so the party jumps with us right away instead
        // of waiting for the periodic state broadcast.
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

// ─── MPV stats overlay ─────────────────────────────────────────────────
//
// Fires once per second from updateProgress(). The whole point is to
// surface enough information that you can tell why playback is choppy
// without an adb cable:
//
//   - decoder-frame-drop-count   high → decode can't keep up (try
//                                 hwdec change, profile=fast,
//                                 vd-lavc-fast).
//   - frame-drop-count           high → vo/display dropping frames
//                                 (try video-sync change or vo=gpu).
//   - estimated-vf-fps vs        big gap → render path is the
//     container-fps               bottleneck.
//   - paused-for-cache true /    network can't sustain bitrate; lower
//     low cache-speed             quality or use the local network.
//
// The label is built lazily so toggling the setting off doesn't leave
// any view tree overhead behind.
void PlayerActivity::updateMpvStatsOverlay() {
    bool wanted = Application::getInstance().getSettings().showMpvStats;

    // Tear down when the toggle is off so we don't pay layout / paint
    // cost while the user isn't looking at stats.
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

    // Trim helper — mpv returns "" for missing properties, "yes"/"no"
    // for flags, and a number/string otherwise. We just feed them
    // straight into the label.
    auto get = [&p](const char* name) -> std::string {
        std::string v = p.getProperty(name);
        return v.empty() ? std::string("?") : v;
    };

    // Format cache-speed (bytes/sec) the same way switchfin does in
    // mpv_core.cpp so the units match what the user sees in logs.
    // snprintf instead of fmt::format because the rest of the project
    // only pulls fmt in via borealis::Logger and that symbol isn't
    // guaranteed to be linked here.
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

    std::string body;
    body.reserve(256);
    body += "Codec: " + get("video-codec")  + " | HW: " + get("hwdec-current") + "\n";
    body += "Source: " + get("width") + "x" + get("height")
          + " @ " + get("container-fps") + " fps"
          + " | Bitrate: " + get("video-bitrate") + "\n";
    body += "Render: " + get("estimated-vf-fps") + " fps"
          + " | Display: " + get("estimated-display-fps") + " fps\n";
    body += "Drops: " + get("decoder-frame-drop-count") + " decoder, "
                     + get("frame-drop-count") + " vo\n";
    body += "Cache: " + get("demuxer-cache-time") + " s / " + fmtSpeed()
          + " | Paused: " + get("paused-for-cache");
    m_mpvStatsLabel->setText(body);
}

} // namespace vitaplex
