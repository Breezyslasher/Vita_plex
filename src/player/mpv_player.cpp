/**
 * VitaPlex - MPV Video Player Implementation
 * Hardware-accelerated video playback using libmpv with FFmpeg-vita
 * Based on switchfin's MPV implementation for PS Vita
 */

#include "player/mpv_player.hpp"
#include <borealis.hpp>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <cstring>
#include <cstdlib>

#ifdef __vita__
#include <mpv/render.h>
#endif

namespace vitaplex {

MpvPlayer& MpvPlayer::getInstance() {
    static MpvPlayer instance;
    return instance;
}

MpvPlayer::~MpvPlayer() {
    shutdown();
}

bool MpvPlayer::init() {
    // Check if we want video or audio-only mode
    return m_videoEnabled ? initWithVideo() : initAudioOnly();
}

bool MpvPlayer::initAudioOnly() {
    if (m_mpv) {
        brls::Logger::debug("MpvPlayer: Already initialized");
        return true;
    }

    brls::Logger::info("MpvPlayer: Initializing in AUDIO-ONLY mode...");

    // Ensure CPU is at max speed for decoding
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Create mpv instance
    m_mpv = mpv_create();
    if (!m_mpv) {
        m_errorMessage = "Failed to create mpv instance";
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
        m_state = MpvPlayerState::ERROR;
        return false;
    }

    brls::Logger::debug("MpvPlayer: mpv context created");

    // ========================================
    // AUDIO-ONLY: Disable video completely
    // ========================================
    mpv_set_option_string(m_mpv, "vo", "null");
    mpv_set_option_string(m_mpv, "video", "no");
    mpv_set_option_string(m_mpv, "vid", "no");
    mpv_set_option_string(m_mpv, "hwdec", "no");
    mpv_set_option_string(m_mpv, "vd", "");

    brls::Logger::debug("MpvPlayer: Video disabled for audio-only mode");

    // Audio output configuration
    mpv_set_option_string(m_mpv, "ao", "vita,null");
    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    mpv_set_option_string(m_mpv, "volume", "100");
    mpv_set_option_string(m_mpv, "volume-max", "100");

    // Cache and demuxer settings
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "cache-secs", "10");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "8MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "4MiB");
    mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "5");

    // Network settings
    mpv_set_option_string(m_mpv, "network-timeout", "30");
    mpv_set_option_string(m_mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");
    mpv_set_option_string(m_mpv, "user-agent", "VitaPlex/1.0 (PlayStation Vita)");

    // Plex headers
    mpv_set_option_string(m_mpv, "http-header-fields",
        "Accept: */*,"
        "X-Plex-Client-Identifier: vita-plex-client-001,"
        "X-Plex-Product: VitaPlex,"
        "X-Plex-Version: 1.5.1,"
        "X-Plex-Platform: PlayStation Vita,"
        "X-Plex-Device: PS Vita");

    // Playback behavior
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "force-window", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=info");

    // Subtitles disabled
    mpv_set_option_string(m_mpv, "sub-auto", "no");
    mpv_set_option_string(m_mpv, "sub-visibility", "no");

    // Logging
    mpv_request_log_messages(m_mpv, "info");

    // Initialize MPV
    brls::Logger::debug("MpvPlayer: Calling mpv_initialize...");
    int result = mpv_initialize(m_mpv);
    if (result < 0) {
        m_errorMessage = std::string("Failed to initialize mpv: ") + mpv_error_string(result);
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        m_state = MpvPlayerState::ERROR;
        return false;
    }

    brls::Logger::info("MpvPlayer: Audio-only mode initialized successfully");
    setupPropertyObservers();
    m_state = MpvPlayerState::IDLE;
    return true;
}

bool MpvPlayer::initWithVideo() {
    if (m_mpv) {
        brls::Logger::debug("MpvPlayer: Already initialized");
        return true;
    }

    brls::Logger::info("MpvPlayer: Initializing with VIDEO support...");

    // Max CPU/GPU speed for video decoding
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Create mpv instance
    m_mpv = mpv_create();
    if (!m_mpv) {
        m_errorMessage = "Failed to create mpv instance";
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
        m_state = MpvPlayerState::ERROR;
        return false;
    }

    brls::Logger::debug("MpvPlayer: mpv context created");

    // ========================================
    // VIDEO MODE: Use libmpv video output
    // ========================================
    // Use libmpv as video output - we'll handle rendering ourselves
    mpv_set_option_string(m_mpv, "vo", "libmpv");

    // Enable hardware decoding for Vita
    mpv_set_option_string(m_mpv, "hwdec", "auto");

    // Video settings for Vita's 960x544 screen
    mpv_set_option_string(m_mpv, "video-sync", "audio");
    mpv_set_option_string(m_mpv, "framedrop", "vo");  // Drop frames if decoding is slow

    brls::Logger::debug("MpvPlayer: Video output configured (vo=libmpv, hwdec=auto)");

    // Audio output configuration
    mpv_set_option_string(m_mpv, "ao", "vita,null");
    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    mpv_set_option_string(m_mpv, "volume", "100");
    mpv_set_option_string(m_mpv, "volume-max", "100");

    // Cache settings - reduced for video to save memory
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "cache-secs", "5");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "16MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "4MiB");
    mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "3");

    // Network settings
    mpv_set_option_string(m_mpv, "network-timeout", "30");
    mpv_set_option_string(m_mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");
    mpv_set_option_string(m_mpv, "user-agent", "VitaPlex/1.0 (PlayStation Vita)");

    // Plex headers
    mpv_set_option_string(m_mpv, "http-header-fields",
        "Accept: */*,"
        "X-Plex-Client-Identifier: vita-plex-client-001,"
        "X-Plex-Product: VitaPlex,"
        "X-Plex-Version: 1.5.1,"
        "X-Plex-Platform: PlayStation Vita,"
        "X-Plex-Device: PS Vita");

    // Playback behavior
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "force-window", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=info");

    // Subtitles
    mpv_set_option_string(m_mpv, "sub-auto", "fuzzy");
    mpv_set_option_string(m_mpv, "sub-visibility", "yes");

    // Logging
    mpv_request_log_messages(m_mpv, "info");

    // Initialize MPV
    brls::Logger::debug("MpvPlayer: Calling mpv_initialize...");
    int result = mpv_initialize(m_mpv);
    if (result < 0) {
        m_errorMessage = std::string("Failed to initialize mpv: ") + mpv_error_string(result);
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        m_state = MpvPlayerState::ERROR;
        return false;
    }

    brls::Logger::info("MpvPlayer: mpv_initialize succeeded, creating render context...");

#ifdef __vita__
    // Create MPV render context for video output
    if (!createRenderContext()) {
        brls::Logger::error("MpvPlayer: Failed to create render context, falling back to audio-only");
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        m_videoEnabled = false;
        return initAudioOnly();
    }
#endif

    brls::Logger::info("MpvPlayer: Video mode initialized successfully");
    setupPropertyObservers();
    m_state = MpvPlayerState::IDLE;
    return true;
}

#ifdef __vita__
bool MpvPlayer::createRenderContext() {
    brls::Logger::info("MpvPlayer: Creating render context...");

    // Get the GXM context from borealis
    auto platform = brls::Application::getPlatform();
    if (!platform) {
        brls::Logger::error("MpvPlayer: No platform available");
        return false;
    }

    auto videoContext = platform->getVideoContext();
    if (!videoContext) {
        brls::Logger::error("MpvPlayer: No video context available");
        return false;
    }

    // For now, we'll use a simple software rendering approach
    // The MPV render API requires specific GXM setup that needs to be
    // coordinated with borealis. Until that's implemented, we'll use
    // the vita video output driver directly.

    brls::Logger::info("MpvPlayer: Using vita VO driver for video output");

    // Switch to vita VO instead of libmpv to avoid context conflicts
    mpv_set_property_string(m_mpv, "vo", "vita");

    m_hasRenderContext = true;
    return true;
}

void MpvPlayer::destroyRenderContext() {
    if (m_renderContext) {
        mpv_render_context_free(m_renderContext);
        m_renderContext = nullptr;
    }
    m_hasRenderContext = false;
}
#endif

void MpvPlayer::setupPropertyObservers() {
    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "cache-buffering-state", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "track-list/count", MPV_FORMAT_INT64);
}

void MpvPlayer::shutdown() {
#ifdef __vita__
    destroyRenderContext();
#endif

    if (m_mpv) {
        brls::Logger::debug("MpvPlayer: Shutting down");
        stop();

        // Give mpv time to cleanup
        sceKernelDelayThread(100000);  // 100ms

        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    m_state = MpvPlayerState::IDLE;
}

void MpvPlayer::setVideoEnabled(bool enabled) {
    if (m_mpv && m_videoEnabled != enabled) {
        // Need to reinitialize with different mode
        shutdown();
    }
    m_videoEnabled = enabled;
}

bool MpvPlayer::loadUrl(const std::string& url, const std::string& title) {
    if (!m_mpv) {
        if (!init()) {
            return false;
        }
    }

    // Prevent loading while already loading to avoid "command already pending" issues
    if (m_state == MpvPlayerState::LOADING) {
        brls::Logger::debug("MpvPlayer: Already loading, ignoring duplicate load request");
        return false;
    }

    brls::Logger::debug("MpvPlayer: Loading URL: {}", url);

    // Reset state before loading
    m_currentUrl = url;
    m_playbackInfo = MpvPlaybackInfo();
    m_playbackInfo.mediaTitle = title;
    m_commandId = 1;  // Reset command ID counter

    // Stop any current playback first (synchronous)
    const char* stopCmd[] = {"stop", NULL};
    mpv_command(m_mpv, stopCmd);

    // Process any pending events from the stop command
    processEvents();

    // Small delay to ensure stop completes
    sceKernelDelayThread(50000);  // 50ms

    // Set loading state BEFORE sending the command
    setState(MpvPlayerState::LOADING);

    // Load the file using async command to track completion
    const char* cmd[] = {"loadfile", url.c_str(), "replace", NULL};
    int result = mpv_command_async(m_mpv, m_commandId++, cmd);

    if (result < 0) {
        m_errorMessage = std::string("Failed to load URL: ") + mpv_error_string(result);
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
        setState(MpvPlayerState::ERROR);
        return false;
    }

    brls::Logger::debug("MpvPlayer: Load command sent (async id={})", m_commandId - 1);
    return true;
}

bool MpvPlayer::loadFile(const std::string& path) {
    return loadUrl(path, "");
}

void MpvPlayer::play() {
    if (!m_mpv) return;
    
    int paused = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
}

void MpvPlayer::pause() {
    if (!m_mpv) return;
    
    int paused = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
}

void MpvPlayer::togglePause() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"cycle", "pause", NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::stop() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"stop", NULL};
    mpv_command(m_mpv, cmd);
    
    m_currentUrl.clear();
    m_playbackInfo = MpvPlaybackInfo();
    setState(MpvPlayerState::IDLE);
}

void MpvPlayer::seekTo(double seconds) {
    if (!m_mpv) return;
    
    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%.2f", seconds);
    
    const char* cmd[] = {"seek", timeStr, "absolute", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::seekRelative(double seconds) {
    if (!m_mpv) return;
    
    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%+.2f", seconds);
    
    const char* cmd[] = {"seek", timeStr, "relative", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::seekPercent(double percent) {
    if (!m_mpv) return;
    
    char percentStr[32];
    snprintf(percentStr, sizeof(percentStr), "%.2f", percent);
    
    const char* cmd[] = {"seek", percentStr, "absolute-percent", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::seekChapter(int delta) {
    if (!m_mpv) return;
    
    char deltaStr[16];
    snprintf(deltaStr, sizeof(deltaStr), "%d", delta);
    
    const char* cmd[] = {"add", "chapter", deltaStr, NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::setVolume(int percent) {
    if (!m_mpv) return;
    
    if (percent < 0) percent = 0;
    if (percent > 150) percent = 150;
    
    double vol = (double)percent;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

int MpvPlayer::getVolume() const {
    if (!m_mpv) return 100;
    
    double vol = 100.0;
    mpv_get_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    return (int)vol;
}

void MpvPlayer::adjustVolume(int delta) {
    setVolume(getVolume() + delta);
}

void MpvPlayer::setMute(bool muted) {
    if (!m_mpv) return;
    
    int val = muted ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &val);
}

bool MpvPlayer::isMuted() const {
    if (!m_mpv) return false;
    
    int val = 0;
    mpv_get_property(m_mpv, "mute", MPV_FORMAT_FLAG, &val);
    return val != 0;
}

void MpvPlayer::toggleMute() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"cycle", "mute", NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::setSubtitleTrack(int track) {
    if (!m_mpv) return;
    
    int64_t sid = track;
    mpv_set_property(m_mpv, "sid", MPV_FORMAT_INT64, &sid);
}

void MpvPlayer::setAudioTrack(int track) {
    if (!m_mpv) return;
    
    int64_t aid = track;
    mpv_set_property(m_mpv, "aid", MPV_FORMAT_INT64, &aid);
}

void MpvPlayer::cycleSubtitle() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"cycle", "sid", NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::cycleAudio() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"cycle", "aid", NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::setSubtitleDelay(double seconds) {
    if (!m_mpv) return;
    
    mpv_set_property(m_mpv, "sub-delay", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvPlayer::setAudioDelay(double seconds) {
    if (!m_mpv) return;
    
    mpv_set_property(m_mpv, "audio-delay", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvPlayer::toggleSubtitles() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"cycle", "sub-visibility", NULL};
    mpv_command(m_mpv, cmd);
    m_subtitlesVisible = !m_subtitlesVisible;
}

double MpvPlayer::getPosition() const {
    if (!m_mpv) return 0.0;
    
    double pos = 0.0;
    mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double MpvPlayer::getDuration() const {
    if (!m_mpv) return 0.0;
    
    double dur = 0.0;
    mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

double MpvPlayer::getPercentPosition() const {
    double duration = getDuration();
    if (duration <= 0.0) return 0.0;
    return (getPosition() / duration) * 100.0;
}

void MpvPlayer::showOSD(const std::string& text, double durationSec) {
    if (!m_mpv) return;
    
    char durStr[16];
    snprintf(durStr, sizeof(durStr), "%d", (int)(durationSec * 1000));
    
    const char* cmd[] = {"show-text", text.c_str(), durStr, NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::toggleOSD() {
    if (!m_mpv) return;
    
    const char* cmd[] = {"cycle-values", "osd-level", "3", "1", NULL};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::setOption(const std::string& name, const std::string& value) {
    if (!m_mpv) return;
    mpv_set_option_string(m_mpv, name.c_str(), value.c_str());
}

std::string MpvPlayer::getProperty(const std::string& name) const {
    if (!m_mpv) return "";
    
    char* val = mpv_get_property_string(m_mpv, name.c_str());
    if (!val) return "";
    
    std::string result(val);
    mpv_free(val);
    return result;
}

void MpvPlayer::setState(MpvPlayerState newState) {
    if (m_state != newState) {
        brls::Logger::debug("MpvPlayer: State change: {} -> {}", (int)m_state, (int)newState);
        m_state = newState;
    }
}

void MpvPlayer::update() {
    if (!m_mpv) {
        return;
    }

    processEvents();
    updatePlaybackInfo();
}

void MpvPlayer::processEvents() {
    if (!m_mpv) return;

    while (m_mpv) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            break;
        }
        handleEvent(event);
    }
}

void MpvPlayer::handleEvent(mpv_event* event) {
    switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            brls::Logger::debug("MpvPlayer: Event START_FILE");
            setState(MpvPlayerState::LOADING);
            break;

        case MPV_EVENT_FILE_LOADED:
            brls::Logger::debug("MpvPlayer: Event FILE_LOADED");
            setState(MpvPlayerState::PLAYING);
            break;

        case MPV_EVENT_PLAYBACK_RESTART:
            brls::Logger::debug("MpvPlayer: Event PLAYBACK_RESTART");
            if (m_state == MpvPlayerState::LOADING || m_state == MpvPlayerState::BUFFERING) {
                int paused = 0;
                mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
                setState(paused ? MpvPlayerState::PAUSED : MpvPlayerState::PLAYING);
            }
            break;
            
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file* end = (mpv_event_end_file*)event->data;
            brls::Logger::debug("MpvPlayer: Event END_FILE (reason: {}, error: {})", (int)end->reason, end->error);

            // MPV end file reasons:
            // 0 = EOF (finished normally)
            // 2 = STOP (stopped by user)
            // 3 = QUIT
            // 4 = ERROR (playback failed) - in newer mpv versions
            // 5 = REDIRECT

            if (end->reason == 0) {  // MPV_END_FILE_REASON_EOF
                brls::Logger::debug("MpvPlayer: Playback finished (EOF)");
                setState(MpvPlayerState::ENDED);
            } else if (end->reason == 4 || end->reason == 3) {  // ERROR (4 in new mpv, 3 in old)
                if (end->error < 0) {
                    m_errorMessage = std::string("Playback error: ") + mpv_error_string(end->error);
                } else {
                    m_errorMessage = "Playback failed - file may be incompatible";
                }
                brls::Logger::debug("MpvPlayer: Error - {}", m_errorMessage);
                setState(MpvPlayerState::ERROR);
            } else if (end->reason == 2) {  // STOP
                brls::Logger::debug("MpvPlayer: Playback stopped");
                setState(MpvPlayerState::IDLE);
            } else {
                brls::Logger::debug("MpvPlayer: Unknown end reason {}", (int)end->reason);
                setState(MpvPlayerState::IDLE);
            }
            break;
        }

        case MPV_EVENT_IDLE:
            brls::Logger::debug("MpvPlayer: Event IDLE");
            if (m_state != MpvPlayerState::ERROR && m_state != MpvPlayerState::ENDED) {
                setState(MpvPlayerState::IDLE);
            }
            break;
            
        case MPV_EVENT_PROPERTY_CHANGE:
            handlePropertyChange((mpv_event_property*)event->data);
            break;
            
        case MPV_EVENT_LOG_MESSAGE: {
            mpv_event_log_message* msg = (mpv_event_log_message*)event->data;
            // Log mpv messages at appropriate level
            if (msg->log_level <= MPV_LOG_LEVEL_ERROR) {
                brls::Logger::error("mpv {}: {}", msg->prefix, msg->text);
            } else if (msg->log_level <= MPV_LOG_LEVEL_WARN) {
                brls::Logger::warning("mpv {}: {}", msg->prefix, msg->text);
            } else if (msg->log_level <= MPV_LOG_LEVEL_INFO) {
                brls::Logger::info("mpv {}: {}", msg->prefix, msg->text);
            }
            break;
        }

        case MPV_EVENT_COMMAND_REPLY: {
            brls::Logger::debug("MpvPlayer: EVENT_COMMAND_REPLY id={} error={}",
                               event->reply_userdata, event->error);
            if (event->error < 0) {
                brls::Logger::error("MpvPlayer: Command {} failed: {}",
                                   event->reply_userdata, mpv_error_string(event->error));
            }
            break;
        }

        default:
            break;
    }
}

void MpvPlayer::handlePropertyChange(mpv_event_property* prop) {
    if (!prop || !prop->name) return;
    
    if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
        int paused = *(int*)prop->data;
        if (m_state == MpvPlayerState::PLAYING || m_state == MpvPlayerState::PAUSED) {
            setState(paused ? MpvPlayerState::PAUSED : MpvPlayerState::PLAYING);
        }
    }
    else if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        m_playbackInfo.position = *(double*)prop->data;
    }
    else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        m_playbackInfo.duration = *(double*)prop->data;
    }
    else if (strcmp(prop->name, "volume") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        m_playbackInfo.volume = (int)(*(double*)prop->data);
    }
    else if (strcmp(prop->name, "mute") == 0 && prop->format == MPV_FORMAT_FLAG) {
        m_playbackInfo.muted = *(int*)prop->data != 0;
    }
    else if (strcmp(prop->name, "paused-for-cache") == 0 && prop->format == MPV_FORMAT_FLAG) {
        if (*(int*)prop->data) {
            m_playbackInfo.buffering = true;
            if (m_state == MpvPlayerState::PLAYING) {
                setState(MpvPlayerState::BUFFERING);
            }
        } else {
            m_playbackInfo.buffering = false;
            if (m_state == MpvPlayerState::BUFFERING) {
                setState(MpvPlayerState::PLAYING);
            }
        }
    }
    else if (strcmp(prop->name, "cache-buffering-state") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        m_playbackInfo.bufferingPercent = *(double*)prop->data;
    }
    else if (strcmp(prop->name, "seeking") == 0 && prop->format == MPV_FORMAT_FLAG) {
        m_playbackInfo.seeking = *(int*)prop->data != 0;
    }
}

void MpvPlayer::updatePlaybackInfo() {
    if (!m_mpv || m_state == MpvPlayerState::IDLE) return;
    
    // Get video codec info if not yet fetched
    if (m_playbackInfo.videoCodec.empty() && m_state == MpvPlayerState::PLAYING) {
        char* val = mpv_get_property_string(m_mpv, "video-codec");
        if (val) {
            m_playbackInfo.videoCodec = val;
            mpv_free(val);
        }
        
        int64_t w = 0, h = 0;
        mpv_get_property(m_mpv, "width", MPV_FORMAT_INT64, &w);
        mpv_get_property(m_mpv, "height", MPV_FORMAT_INT64, &h);
        m_playbackInfo.videoWidth = (int)w;
        m_playbackInfo.videoHeight = (int)h;
        
        double fps = 0.0;
        mpv_get_property(m_mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps);
        m_playbackInfo.fps = fps;
        
        brls::Logger::debug("MpvPlayer: Video info - {}x{} @ {:.2f} fps, codec: {}",
                      m_playbackInfo.videoWidth, m_playbackInfo.videoHeight,
                      m_playbackInfo.fps, m_playbackInfo.videoCodec);
    }
    
    if (m_playbackInfo.audioCodec.empty() && m_state == MpvPlayerState::PLAYING) {
        char* val = mpv_get_property_string(m_mpv, "audio-codec");
        if (val) {
            m_playbackInfo.audioCodec = val;
            mpv_free(val);
        }
        
        int64_t ch = 0, sr = 0;
        mpv_get_property(m_mpv, "audio-params/channel-count", MPV_FORMAT_INT64, &ch);
        mpv_get_property(m_mpv, "audio-params/samplerate", MPV_FORMAT_INT64, &sr);
        m_playbackInfo.audioChannels = (int)ch;
        m_playbackInfo.sampleRate = (int)sr;
        
        brls::Logger::debug("MpvPlayer: Audio info - {} channels @ {} Hz, codec: {}",
                      m_playbackInfo.audioChannels, m_playbackInfo.sampleRate,
                      m_playbackInfo.audioCodec);
    }
}

// Helper function to render the video frame
// MPV with vita2d output handles rendering internally
void MpvPlayer::render() {
    if (!m_mpv) return;
    
    // With vita2d vo, rendering is handled by mpv internally
    // Just need to call update() to process events
}

} // namespace vitaplex
