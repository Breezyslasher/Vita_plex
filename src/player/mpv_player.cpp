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

namespace vitaplex {

// Command IDs for async operations
static const uint64_t CMD_LOADFILE = 1;
static const uint64_t CMD_STOP = 2;
static const uint64_t CMD_SEEK = 3;

MpvPlayer& MpvPlayer::getInstance() {
    static MpvPlayer instance;
    return instance;
}

MpvPlayer::~MpvPlayer() {
    shutdown();
}

bool MpvPlayer::init() {
    if (m_mpv) {
        brls::Logger::debug("MpvPlayer: Already initialized");
        return true;
    }

    brls::Logger::debug("MpvPlayer: Initializing libmpv...");

    // Ensure CPU is at max speed for video decoding
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Create mpv instance
    m_mpv = mpv_create();
    if (!m_mpv) {
        m_errorMessage = "Failed to create mpv instance";
        brls::Logger::debug("MpvPlayer: {}", m_errorMessage);
        m_state = MpvPlayerState::ERROR;
        return false;
    }

    brls::Logger::debug("MpvPlayer: mpv context created");

    // ========================================
    // CRITICAL: Video output configuration
    // ========================================
    // We MUST use null video output because our app uses vita2d for UI
    // and vita2d can't be shared between app and mpv simultaneously.

    mpv_set_option_string(m_mpv, "vo", "null");

    // ========================================
    // Audio output configuration for Vita
    // ========================================

    mpv_set_option_string(m_mpv, "ao", "vita,null");
    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    mpv_set_option_string(m_mpv, "volume", "100");
    mpv_set_option_string(m_mpv, "volume-max", "100");

    // ========================================
    // Cache and demuxer settings
    // ========================================

    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "cache-secs", "10");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "8MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "4MiB");
    mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "5");

    // ========================================
    // Network settings for streaming
    // ========================================

    mpv_set_option_string(m_mpv, "network-timeout", "30");
    mpv_set_option_string(m_mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");

    mpv_set_option_string(m_mpv, "user-agent", "VitaPlex/1.0 (PlayStation Vita)");

    mpv_set_option_string(m_mpv, "http-header-fields",
        "Accept: */*,"
        "X-Plex-Client-Identifier: vita-plex-client-001,"
        "X-Plex-Product: VitaPlex,"
        "X-Plex-Version: 1.5.1,"
        "X-Plex-Platform: PlayStation Vita,"
        "X-Plex-Device: PS Vita");

    // ========================================
    // Playback behavior
    // ========================================

    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "force-window", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=warn");

    // ========================================
    // Subtitle settings
    // ========================================

    mpv_set_option_string(m_mpv, "sub-auto", "no");
    mpv_set_option_string(m_mpv, "sub-visibility", "no");

    // ========================================
    // Logging
    // ========================================

    mpv_request_log_messages(m_mpv, "warn");

    // ========================================
    // Initialize MPV
    // ========================================

    brls::Logger::debug("MpvPlayer: Calling mpv_initialize...");

    int result = mpv_initialize(m_mpv);
    if (result < 0) {
        m_errorMessage = std::string("Failed to initialize mpv: ") + mpv_error_string(result);
        brls::Logger::debug("MpvPlayer: {}", m_errorMessage);
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        m_state = MpvPlayerState::ERROR;
        return false;
    }

    brls::Logger::debug("MpvPlayer: mpv_initialize succeeded");

    // ========================================
    // Set up property observers
    // ========================================

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "cache-buffering-state", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "core-idle", MPV_FORMAT_FLAG);

    brls::Logger::debug("MpvPlayer: Initialized successfully");
    m_state = MpvPlayerState::IDLE;
    m_commandPending = false;
    return true;
}

void MpvPlayer::shutdown() {
    if (m_mpv) {
        brls::Logger::debug("MpvPlayer: Shutting down");

        // Set stopping flag to prevent further operations
        m_stopping = true;

        // Stop playback synchronously during shutdown
        const char* stopCmd[] = {"stop", NULL};
        mpv_command(m_mpv, stopCmd);

        // Give mpv time to cleanup
        sceKernelDelayThread(200000);  // 200ms

        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        m_stopping = false;
    }
    m_state = MpvPlayerState::IDLE;
    m_commandPending = false;
}

bool MpvPlayer::loadUrl(const std::string& url, const std::string& title) {
    if (!m_mpv) {
        if (!init()) {
            return false;
        }
    }

    // Prevent concurrent load operations
    if (m_commandPending) {
        brls::Logger::debug("MpvPlayer: Command already pending, ignoring load request");
        return false;
    }

    // Normalize URL scheme to lowercase
    std::string normalizedUrl = url;
    if (normalizedUrl.substr(0, 5) == "Http:") {
        normalizedUrl = "http:" + normalizedUrl.substr(5);
    } else if (normalizedUrl.substr(0, 6) == "Https:") {
        normalizedUrl = "https:" + normalizedUrl.substr(6);
    }

    brls::Logger::debug("MpvPlayer: Loading URL: {}", normalizedUrl);

    m_currentUrl = normalizedUrl;
    m_playbackInfo = MpvPlaybackInfo();
    m_playbackInfo.mediaTitle = title;

    // Mark command as pending
    m_commandPending = true;

    // Use ASYNC loadfile command - this is critical for Vita stability
    // The "replace" mode stops current playback and loads the new file
    const char* cmd[] = {"loadfile", normalizedUrl.c_str(), "replace", NULL};
    int result = mpv_command_async(m_mpv, CMD_LOADFILE, cmd);

    if (result < 0) {
        m_errorMessage = std::string("Failed to queue load command: ") + mpv_error_string(result);
        brls::Logger::debug("MpvPlayer: {}", m_errorMessage);
        m_commandPending = false;
        setState(MpvPlayerState::ERROR);
        return false;
    }

    setState(MpvPlayerState::LOADING);
    return true;
}

bool MpvPlayer::loadFile(const std::string& path) {
    return loadUrl(path, "");
}

void MpvPlayer::play() {
    if (!m_mpv || m_stopping) return;

    // Use async property set for safety
    int paused = 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &paused);
}

void MpvPlayer::pause() {
    if (!m_mpv || m_stopping) return;

    int paused = 1;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &paused);
}

void MpvPlayer::togglePause() {
    if (!m_mpv || m_stopping) return;

    const char* cmd[] = {"cycle", "pause", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::stop() {
    if (!m_mpv || m_stopping) return;

    brls::Logger::debug("MpvPlayer: Stopping playback");

    // Use async stop command
    const char* cmd[] = {"stop", NULL};
    mpv_command_async(m_mpv, CMD_STOP, cmd);

    m_currentUrl.clear();
    m_playbackInfo = MpvPlaybackInfo();
    setState(MpvPlayerState::IDLE);
}

void MpvPlayer::seekTo(double seconds) {
    if (!m_mpv || m_stopping) return;

    // Don't seek while loading
    if (m_state == MpvPlayerState::LOADING) {
        brls::Logger::debug("MpvPlayer: Deferring seek (still loading)");
        return;
    }

    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%.2f", seconds);

    const char* cmd[] = {"seek", timeStr, "absolute", NULL};
    mpv_command_async(m_mpv, CMD_SEEK, cmd);
}

void MpvPlayer::seekRelative(double seconds) {
    if (!m_mpv || m_stopping) return;

    if (m_state == MpvPlayerState::LOADING) return;

    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%+.2f", seconds);

    const char* cmd[] = {"seek", timeStr, "relative", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::seekPercent(double percent) {
    if (!m_mpv || m_stopping) return;

    if (m_state == MpvPlayerState::LOADING) return;

    char percentStr[32];
    snprintf(percentStr, sizeof(percentStr), "%.2f", percent);

    const char* cmd[] = {"seek", percentStr, "absolute-percent", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::seekChapter(int delta) {
    if (!m_mpv || m_stopping) return;

    char deltaStr[16];
    snprintf(deltaStr, sizeof(deltaStr), "%d", delta);

    const char* cmd[] = {"add", "chapter", deltaStr, NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::setVolume(int percent) {
    if (!m_mpv || m_stopping) return;

    if (percent < 0) percent = 0;
    if (percent > 150) percent = 150;

    double vol = (double)percent;
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &vol);
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
    if (!m_mpv || m_stopping) return;

    int val = muted ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &val);
}

bool MpvPlayer::isMuted() const {
    if (!m_mpv) return false;

    int val = 0;
    mpv_get_property(m_mpv, "mute", MPV_FORMAT_FLAG, &val);
    return val != 0;
}

void MpvPlayer::toggleMute() {
    if (!m_mpv || m_stopping) return;

    const char* cmd[] = {"cycle", "mute", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::setSubtitleTrack(int track) {
    if (!m_mpv || m_stopping) return;

    int64_t sid = track;
    mpv_set_property_async(m_mpv, 0, "sid", MPV_FORMAT_INT64, &sid);
}

void MpvPlayer::setAudioTrack(int track) {
    if (!m_mpv || m_stopping) return;

    int64_t aid = track;
    mpv_set_property_async(m_mpv, 0, "aid", MPV_FORMAT_INT64, &aid);
}

void MpvPlayer::cycleSubtitle() {
    if (!m_mpv || m_stopping) return;

    const char* cmd[] = {"cycle", "sid", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::cycleAudio() {
    if (!m_mpv || m_stopping) return;

    const char* cmd[] = {"cycle", "aid", NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::setSubtitleDelay(double seconds) {
    if (!m_mpv || m_stopping) return;

    mpv_set_property_async(m_mpv, 0, "sub-delay", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvPlayer::setAudioDelay(double seconds) {
    if (!m_mpv || m_stopping) return;

    mpv_set_property_async(m_mpv, 0, "audio-delay", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvPlayer::toggleSubtitles() {
    if (!m_mpv || m_stopping) return;

    const char* cmd[] = {"cycle", "sub-visibility", NULL};
    mpv_command_async(m_mpv, 0, cmd);
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
    if (!m_mpv || m_stopping) return;

    char durStr[16];
    snprintf(durStr, sizeof(durStr), "%d", (int)(durationSec * 1000));

    const char* cmd[] = {"show-text", text.c_str(), durStr, NULL};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvPlayer::toggleOSD() {
    if (!m_mpv || m_stopping) return;

    const char* cmd[] = {"cycle-values", "osd-level", "3", "1", NULL};
    mpv_command_async(m_mpv, 0, cmd);
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
    if (!m_mpv || m_stopping) return;

    processEvents();

    // Only update playback info when actually playing
    if (m_state == MpvPlayerState::PLAYING || m_state == MpvPlayerState::PAUSED) {
        updatePlaybackInfo();
    }
}

void MpvPlayer::processEvents() {
    if (!m_mpv) return;

    // Process all pending events with zero timeout (non-blocking)
    while (m_mpv && !m_stopping) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            break;
        }
        handleEvent(event);
    }
}

void MpvPlayer::handleEvent(mpv_event* event) {
    if (!event) return;

    switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            brls::Logger::debug("MpvPlayer: Event START_FILE");
            setState(MpvPlayerState::LOADING);
            break;

        case MPV_EVENT_FILE_LOADED:
            brls::Logger::debug("MpvPlayer: Event FILE_LOADED");
            m_commandPending = false;  // Load command completed
            // Don't change state yet - wait for PLAYBACK_RESTART
            break;

        case MPV_EVENT_PLAYBACK_RESTART:
            brls::Logger::debug("MpvPlayer: Event PLAYBACK_RESTART");
            m_commandPending = false;
            // Now safe to transition to playing
            if (m_state == MpvPlayerState::LOADING || m_state == MpvPlayerState::BUFFERING) {
                int paused = 0;
                if (mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused) >= 0) {
                    setState(paused ? MpvPlayerState::PAUSED : MpvPlayerState::PLAYING);
                } else {
                    setState(MpvPlayerState::PLAYING);
                }
            }
            break;

        case MPV_EVENT_END_FILE: {
            if (!event->data) break;

            mpv_event_end_file* end = (mpv_event_end_file*)event->data;
            brls::Logger::debug("MpvPlayer: Event END_FILE (reason: {}, error: {})", (int)end->reason, end->error);

            m_commandPending = false;

            if (end->reason == 0) {  // MPV_END_FILE_REASON_EOF
                brls::Logger::debug("MpvPlayer: Playback finished (EOF)");
                setState(MpvPlayerState::ENDED);
            } else if (end->reason == 4 || end->error < 0) {  // ERROR
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
                brls::Logger::debug("MpvPlayer: End reason {}", (int)end->reason);
                setState(MpvPlayerState::IDLE);
            }
            break;
        }

        case MPV_EVENT_IDLE:
            brls::Logger::debug("MpvPlayer: Event IDLE");
            m_commandPending = false;
            if (m_state != MpvPlayerState::ERROR && m_state != MpvPlayerState::ENDED) {
                setState(MpvPlayerState::IDLE);
            }
            break;

        case MPV_EVENT_COMMAND_REPLY: {
            // Handle async command completion
            brls::Logger::debug("MpvPlayer: Command reply (id: {}, error: {})",
                              event->reply_userdata, event->error);

            if (event->reply_userdata == CMD_LOADFILE) {
                if (event->error < 0) {
                    m_errorMessage = std::string("Load failed: ") + mpv_error_string(event->error);
                    brls::Logger::debug("MpvPlayer: {}", m_errorMessage);
                    m_commandPending = false;
                    setState(MpvPlayerState::ERROR);
                }
                // Success will be signaled by FILE_LOADED event
            }
            break;
        }

        case MPV_EVENT_PROPERTY_CHANGE:
            if (event->data) {
                handlePropertyChange((mpv_event_property*)event->data);
            }
            break;

        case MPV_EVENT_LOG_MESSAGE: {
            if (!event->data) break;

            mpv_event_log_message* msg = (mpv_event_log_message*)event->data;
            if (msg && msg->log_level <= MPV_LOG_LEVEL_WARN && msg->prefix && msg->text) {
                brls::Logger::debug("mpv [{}]: {}", msg->prefix, msg->text);
            }
            break;
        }

        default:
            break;
    }
}

void MpvPlayer::handlePropertyChange(mpv_event_property* prop) {
    if (!prop || !prop->name || !prop->data) return;

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
        int pausedForCache = *(int*)prop->data;
        if (pausedForCache) {
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
    else if (strcmp(prop->name, "core-idle") == 0 && prop->format == MPV_FORMAT_FLAG) {
        // Core idle indicates mpv is ready for next command
        int idle = *(int*)prop->data;
        if (idle && m_state == MpvPlayerState::LOADING) {
            // If we're idle while loading, something went wrong
            brls::Logger::debug("MpvPlayer: Core idle during loading state");
        }
    }
}

void MpvPlayer::updatePlaybackInfo() {
    if (!m_mpv || m_state == MpvPlayerState::IDLE || m_state == MpvPlayerState::LOADING) return;

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

        if (m_playbackInfo.videoWidth > 0) {
            brls::Logger::debug("MpvPlayer: Video info - {}x{} @ {:.2f} fps, codec: {}",
                          m_playbackInfo.videoWidth, m_playbackInfo.videoHeight,
                          m_playbackInfo.fps, m_playbackInfo.videoCodec);
        }
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

        if (m_playbackInfo.audioChannels > 0) {
            brls::Logger::debug("MpvPlayer: Audio info - {} channels @ {} Hz, codec: {}",
                          m_playbackInfo.audioChannels, m_playbackInfo.sampleRate,
                          m_playbackInfo.audioCodec);
        }
    }
}

void MpvPlayer::render() {
    // With null vo, nothing to render
}

} // namespace vitaplex
