/**
 * VitaPlex - MPV Video Player Implementation
 * Based on switchfin's MPV implementation for PS Vita
 * Using software rendering with NanoVG display
 */

#include "player/mpv_player.hpp"
#include <borealis.hpp>

#ifdef __vita__
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <nanovg.h>
#include <nanovg_gxm.h>
#include <nanovg_gxm_utils.h>
#include <borealis/platforms/psv/psv_video.hpp>
#endif

#include <cstring>
#include <cstdlib>
#include <clocale>

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

    // Set locale for consistent number formatting (important for mpv)
    setlocale(LC_NUMERIC, "C");

#ifdef __vita__
    // Ensure CPU is at max speed for video decoding
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif

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
    // Core configuration (matching switchfin)
    // ========================================

    mpv_set_option_string(m_mpv, "osd-level", "0");
    mpv_set_option_string(m_mpv, "video-timing-offset", "0");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");

    // ========================================
    // Video output configuration (matching switchfin for Vita)
    // ========================================

    // Use libmpv for video output - we'll create a render context
    mpv_set_option_string(m_mpv, "vo", "libmpv");

#ifdef __vita__
    // Vita-specific settings from switchfin
    mpv_set_option_string(m_mpv, "vd-lavc-threads", "4");
    mpv_set_option_string(m_mpv, "vd-lavc-skiploopfilter", "all");
    mpv_set_option_string(m_mpv, "vd-lavc-fast", "yes");

    // Use Vita hardware decoding (from switchfin)
    mpv_set_option_string(m_mpv, "hwdec", "vita-copy");

    // GXM-specific settings from switchfin
    mpv_set_option_string(m_mpv, "fbo-format", "rgba8");
    mpv_set_option_string(m_mpv, "video-latency-hacks", "yes");
#else
    mpv_set_option_string(m_mpv, "hwdec", "no");
#endif

    // ========================================
    // Audio output configuration
    // ========================================

    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    mpv_set_option_string(m_mpv, "volume", "100");
    mpv_set_option_string(m_mpv, "volume-max", "150");

    // ========================================
    // Cache and demuxer settings
    // ========================================

#ifdef __vita__
    // Enable cache for network streaming (required for HTTP)
    // Use smaller cache sizes for Vita's limited memory
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "2MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "512KiB");
    mpv_set_option_string(m_mpv, "cache-secs", "10");
#else
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "4MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "2MiB");
#endif

    // ========================================
    // Network settings for streaming
    // ========================================

    mpv_set_option_string(m_mpv, "network-timeout", "30");

    // User agent for Plex compatibility
    mpv_set_option_string(m_mpv, "user-agent", "VitaPlex/1.0");

    // ========================================
    // Subtitle settings
    // ========================================

    mpv_set_option_string(m_mpv, "sub-auto", "fuzzy");
    mpv_set_option_string(m_mpv, "subs-fallback", "yes");

    // ========================================
    // Request log messages (verbose for debugging crashes)
    // ========================================

    mpv_request_log_messages(m_mpv, "info");

    // ========================================
    // Initialize MPV
    // ========================================

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

    brls::Logger::debug("MpvPlayer: mpv_initialize succeeded");

    // ========================================
    // Set up render context for video display
    // ========================================

    if (!initRenderContext()) {
        brls::Logger::error("MpvPlayer: Failed to create render context, falling back to audio-only");
        // Don't fail - we can still play audio
    }

    // ========================================
    // Set up property observers (matching switchfin IDs)
    // ========================================

    mpv_observe_property(m_mpv, 1, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 2, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 3, "duration", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 4, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 5, "cache-speed", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 6, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 7, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 8, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 9, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 10, "volume", MPV_FORMAT_INT64);

    brls::Logger::info("MpvPlayer: Initialized successfully");
    m_state = MpvPlayerState::IDLE;
    m_commandPending = false;
    return true;
}

void MpvPlayer::shutdown() {
    if (m_mpv) {
        brls::Logger::debug("MpvPlayer: Shutting down");

        m_stopping = true;

        // Clean up render context first
        cleanupRenderContext();

        // Send quit command (like switchfin does in clean())
        const char* cmd[] = {"quit", NULL};
        mpv_command(m_mpv, cmd);

#ifdef __vita__
        // Give mpv time to cleanup
        sceKernelDelayThread(200000);  // 200ms
#endif

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
    if (normalizedUrl.length() > 5) {
        // Convert scheme to lowercase
        for (size_t i = 0; i < 6 && i < normalizedUrl.length(); i++) {
            if (normalizedUrl[i] == ':') break;
            normalizedUrl[i] = tolower(normalizedUrl[i]);
        }
    }

    brls::Logger::info("MpvPlayer: Loading URL: {}", normalizedUrl);

    m_currentUrl = normalizedUrl;
    m_playbackInfo = MpvPlaybackInfo();
    m_playbackInfo.mediaTitle = title;

    // Mark command as pending
    m_commandPending = true;

    // Use simple loadfile command
    const char* cmd[] = {"loadfile", normalizedUrl.c_str(), "replace", nullptr};
    int result = mpv_command_async(m_mpv, CMD_LOADFILE, cmd);
    if (result < 0) {
        m_errorMessage = std::string("Failed to queue load command: ") + mpv_error_string(result);
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
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

    const char* cmd[] = {"stop", NULL};
    mpv_command_async(m_mpv, CMD_STOP, cmd);

    m_currentUrl.clear();
    m_playbackInfo = MpvPlaybackInfo();
    setState(MpvPlayerState::IDLE);
}

void MpvPlayer::seekTo(double seconds) {
    if (!m_mpv || m_stopping) return;

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

    int64_t vol = (int64_t)percent;
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_INT64, &vol);
}

int MpvPlayer::getVolume() const {
    if (!m_mpv) return 100;

    int64_t vol = 100;
    mpv_get_property(m_mpv, "volume", MPV_FORMAT_INT64, &vol);
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
    mpv_get_property(m_mpv, "playback-time", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double MpvPlayer::getDuration() const {
    if (!m_mpv) return 0.0;

    int64_t dur = 0;
    mpv_get_property(m_mpv, "duration", MPV_FORMAT_INT64, &dur);
    return (double)dur;
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

    // Process events (matching switchfin's eventMainLoop)
    eventMainLoop();

    // Update playback info when playing
    if (m_state == MpvPlayerState::PLAYING || m_state == MpvPlayerState::PAUSED) {
        updatePlaybackInfo();
    }
}

void MpvPlayer::eventMainLoop() {
    if (!m_mpv) return;

    // Process all pending events (matching switchfin's approach)
    while (true) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);

        if (!event || event->event_id == MPV_EVENT_NONE) {
            return;
        }

        switch (event->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                if (event->data) {
                    mpv_event_log_message* msg = (mpv_event_log_message*)event->data;
                    if (msg->log_level <= MPV_LOG_LEVEL_ERROR) {
                        brls::Logger::error("mpv {}: {}", msg->prefix, msg->text);
                    } else if (msg->log_level <= MPV_LOG_LEVEL_WARN) {
                        brls::Logger::warning("mpv {}: {}", msg->prefix, msg->text);
                    } else if (msg->log_level <= MPV_LOG_LEVEL_INFO) {
                        brls::Logger::info("mpv {}: {}", msg->prefix, msg->text);
                    }
                }
                break;
            }

            case MPV_EVENT_SHUTDOWN:
                brls::Logger::debug("MpvPlayer: EVENT_SHUTDOWN");
                setState(MpvPlayerState::IDLE);
                return;

            case MPV_EVENT_START_FILE:
                brls::Logger::debug("MpvPlayer: EVENT_START_FILE");
                setState(MpvPlayerState::LOADING);
                break;

            case MPV_EVENT_FILE_LOADED:
                brls::Logger::info("MpvPlayer: EVENT_FILE_LOADED");
                m_commandPending = false;
                // Don't transition to PLAYING yet - wait for playback to actually start
                break;

            case MPV_EVENT_PLAYBACK_RESTART:
                brls::Logger::debug("MpvPlayer: EVENT_PLAYBACK_RESTART");
                m_commandPending = false;
                // Now safe to say we're playing
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
                if (event->data) {
                    mpv_event_end_file* end = (mpv_event_end_file*)event->data;
                    brls::Logger::debug("MpvPlayer: EVENT_END_FILE reason={} error={}",
                                       (int)end->reason, end->error);

                    m_commandPending = false;

                    // MPV_END_FILE_REASON_EOF = 0
                    // MPV_END_FILE_REASON_STOP = 2
                    // MPV_END_FILE_REASON_ERROR = 4
                    if (end->reason == 0) {
                        setState(MpvPlayerState::ENDED);
                    } else if (end->reason == 4 || end->error < 0) {
                        if (end->error < 0) {
                            m_errorMessage = std::string("Playback error: ") + mpv_error_string(end->error);
                        } else {
                            m_errorMessage = "Playback failed";
                        }
                        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
                        setState(MpvPlayerState::ERROR);
                    } else {
                        setState(MpvPlayerState::IDLE);
                    }
                }
                break;
            }

            case MPV_EVENT_IDLE:
                brls::Logger::debug("MpvPlayer: EVENT_IDLE");
                m_commandPending = false;
                if (m_state != MpvPlayerState::ERROR && m_state != MpvPlayerState::ENDED) {
                    setState(MpvPlayerState::IDLE);
                }
                break;

            case MPV_EVENT_COMMAND_REPLY:
                brls::Logger::debug("MpvPlayer: EVENT_COMMAND_REPLY id={} error={}",
                                   event->reply_userdata, event->error);
                if (event->reply_userdata == CMD_LOADFILE && event->error < 0) {
                    m_errorMessage = std::string("Load failed: ") + mpv_error_string(event->error);
                    brls::Logger::error("MpvPlayer: {}", m_errorMessage);
                    m_commandPending = false;
                    setState(MpvPlayerState::ERROR);
                }
                break;

            case MPV_EVENT_PROPERTY_CHANGE:
                if (event->data) {
                    handlePropertyChange((mpv_event_property*)event->data, event->reply_userdata);
                }
                break;

            default:
                break;
        }
    }
}

void MpvPlayer::handlePropertyChange(mpv_event_property* prop, uint64_t id) {
    if (!prop || !prop->name) return;

    // Handle property changes based on observer ID (matching switchfin)
    switch (id) {
        case 1: // core-idle
            if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                bool idle = *(int*)prop->data != 0;
                brls::Logger::debug("MpvPlayer: core-idle = {}", idle);
            }
            break;

        case 2: // pause
            if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                bool paused = *(int*)prop->data != 0;
                if (m_state == MpvPlayerState::PLAYING || m_state == MpvPlayerState::PAUSED) {
                    setState(paused ? MpvPlayerState::PAUSED : MpvPlayerState::PLAYING);
                }
            }
            break;

        case 3: // duration
            if (prop->format == MPV_FORMAT_INT64 && prop->data) {
                m_playbackInfo.duration = (double)(*(int64_t*)prop->data);
            }
            break;

        case 4: // playback-time
            if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                m_playbackInfo.position = *(double*)prop->data;
            }
            break;

        case 5: // cache-speed
            if (prop->format == MPV_FORMAT_INT64 && prop->data) {
                m_playbackInfo.cacheUsed = (double)(*(int64_t*)prop->data);
            }
            break;

        case 6: // paused-for-cache
            if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                bool buffering = *(int*)prop->data != 0;
                m_playbackInfo.buffering = buffering;
                if (buffering && m_state == MpvPlayerState::PLAYING) {
                    setState(MpvPlayerState::BUFFERING);
                } else if (!buffering && m_state == MpvPlayerState::BUFFERING) {
                    setState(MpvPlayerState::PLAYING);
                }
            }
            break;

        case 7: // eof-reached
            if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                bool eof = *(int*)prop->data != 0;
                if (eof) {
                    brls::Logger::debug("MpvPlayer: EOF reached");
                }
            }
            break;

        case 8: // seeking
            if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                m_playbackInfo.seeking = *(int*)prop->data != 0;
            }
            break;

        case 9: // speed
            if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                // Could store playback speed if needed
            }
            break;

        case 10: // volume
            if (prop->format == MPV_FORMAT_INT64 && prop->data) {
                m_playbackInfo.volume = (int)(*(int64_t*)prop->data);
            }
            break;
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
            brls::Logger::info("MpvPlayer: Video {}x{} @ {:.2f}fps codec={}",
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
            brls::Logger::info("MpvPlayer: Audio {}ch @ {}Hz codec={}",
                              m_playbackInfo.audioChannels, m_playbackInfo.sampleRate,
                              m_playbackInfo.audioCodec);
        }
    }
}

// Callback for mpv render context when a new frame is ready
static void on_mpv_render_update(void* ctx) {
    MpvPlayer* player = static_cast<MpvPlayer*>(ctx);
    // Signal that a new frame is available
    // The actual rendering will happen in render()
#ifdef __vita__
    if (player) {
        // Set flag that new frame is ready (thread-safe atomic would be better)
        // For now, we'll handle this in the render loop
    }
#endif
}

bool MpvPlayer::initRenderContext() {
#ifdef __vita__
    if (m_mpvRenderCtx) {
        brls::Logger::debug("MpvPlayer: Render context already exists");
        return true;
    }

    if (!m_mpv) {
        brls::Logger::error("MpvPlayer: Cannot create render context - mpv not initialized");
        return false;
    }

    brls::Logger::info("MpvPlayer: Creating GXM render context...");

    // Get the GXM window from borealis
    brls::PsvVideoContext* videoContext = dynamic_cast<brls::PsvVideoContext*>(
        brls::Application::getPlatform()->getVideoContext());
    if (!videoContext) {
        brls::Logger::error("MpvPlayer: Failed to get PSV video context - will play audio only");
        return false;
    }

    NVGXMwindow* gxm = videoContext->getWindow();
    if (!gxm) {
        brls::Logger::error("MpvPlayer: Failed to get GXM window - will play audio only");
        return false;
    }

    if (!gxm->context || !gxm->shader_patcher) {
        brls::Logger::error("MpvPlayer: GXM context or shader_patcher is null - will play audio only");
        return false;
    }

    NVGcontext* vg = brls::Application::getNVGContext();
    if (!vg) {
        brls::Logger::error("MpvPlayer: Failed to get NanoVG context - will play audio only");
        return false;
    }

    brls::Logger::info("MpvPlayer: GXM context acquired, setting up render params...");

    // Set up GXM init parameters (matching switchfin)
    mpv_gxm_init_params gxm_params = {
        .context = gxm->context,
        .shader_patcher = gxm->shader_patcher,
        .buffer_index = 0,
        .msaa = SCE_GXM_MULTISAMPLE_4X,  // Match switchfin
    };

    brls::Logger::info("MpvPlayer: Setting up MPV render params with API type: {}", MPV_RENDER_API_TYPE_GXM);

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_GXM)},
        {MPV_RENDER_PARAM_GXM_INIT_PARAMS, &gxm_params},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    // Create render context
    brls::Logger::info("MpvPlayer: Calling mpv_render_context_create...");
    int result = mpv_render_context_create(&m_mpvRenderCtx, m_mpv, params);
    if (result < 0) {
        brls::Logger::error("MpvPlayer: Failed to create GXM render context: {} (code {})",
                           mpv_error_string(result), result);
        brls::Logger::error("MpvPlayer: Will continue with audio-only playback");
        return false;
    }

    brls::Logger::info("MpvPlayer: GXM render context created successfully!");

    // Create NanoVG image and GXM framebuffer for video output
    m_videoWidth = DISPLAY_WIDTH;
    m_videoHeight = DISPLAY_HEIGHT;
    int texture_stride = ALIGN(m_videoWidth, 8);

    // Create NanoVG image
    m_nvgImage = nvgCreateImageRGBA(vg, m_videoWidth, m_videoHeight, 0, nullptr);
    if (m_nvgImage == 0) {
        brls::Logger::error("MpvPlayer: Failed to create NanoVG image");
        mpv_render_context_free(m_mpvRenderCtx);
        m_mpvRenderCtx = nullptr;
        return false;
    }

    // Get the texture from NanoVG image for GXM framebuffer
    NVGXMtexture* texture = nvgxmImageHandle(vg, m_nvgImage);
    if (!texture) {
        brls::Logger::error("MpvPlayer: Failed to get NanoVG texture handle");
        nvgDeleteImage(vg, m_nvgImage);
        m_nvgImage = 0;
        mpv_render_context_free(m_mpvRenderCtx);
        m_mpvRenderCtx = nullptr;
        return false;
    }

    // Create GXM framebuffer for MPV to render to
    NVGXMframebufferInitOptions framebufferOpts = {
        .display_buffer_count = 1,
        .scenesPerFrame = 1,
        .render_target = texture,
        .color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
        .color_surface_type = SCE_GXM_COLOR_SURFACE_LINEAR,
        .display_width = m_videoWidth,
        .display_height = m_videoHeight,
        .display_stride = texture_stride,
    };

    NVGXMframebuffer* fbo = gxmCreateFramebuffer(&framebufferOpts);
    if (!fbo) {
        brls::Logger::error("MpvPlayer: Failed to create GXM framebuffer");
        nvgDeleteImage(vg, m_nvgImage);
        m_nvgImage = 0;
        mpv_render_context_free(m_mpvRenderCtx);
        m_mpvRenderCtx = nullptr;
        return false;
    }

    m_gxmFramebuffer = fbo;

    // Set up MPV FBO parameters
    m_mpvFbo.render_target = fbo->gxm_render_target;
    m_mpvFbo.color_surface = &fbo->gxm_color_surfaces[0].surface;
    m_mpvFbo.depth_stencil_surface = &fbo->gxm_depth_stencil_surface;
    m_mpvFbo.w = m_videoWidth;
    m_mpvFbo.h = m_videoHeight;

    // Set up render update callback
    mpv_render_context_set_update_callback(m_mpvRenderCtx, on_mpv_render_update, this);

    m_renderReady = true;
    brls::Logger::info("MpvPlayer: GXM render context created successfully ({}x{})", m_videoWidth, m_videoHeight);
    return true;
#else
    // Non-Vita builds don't need render context
    return false;
#endif
}

void MpvPlayer::cleanupRenderContext() {
#ifdef __vita__
    m_renderReady = false;

    if (m_mpvRenderCtx) {
        brls::Logger::debug("MpvPlayer: Cleaning up GXM render context");
        mpv_render_context_free(m_mpvRenderCtx);
        m_mpvRenderCtx = nullptr;
    }

    // Clean up GXM framebuffer
    if (m_gxmFramebuffer) {
        gxmDeleteFramebuffer(static_cast<NVGXMframebuffer*>(m_gxmFramebuffer));
        m_gxmFramebuffer = nullptr;
    }

    // Clean up NanoVG image
    NVGcontext* vg = brls::Application::getNVGContext();
    if (m_nvgImage && vg) {
        nvgDeleteImage(vg, m_nvgImage);
        m_nvgImage = 0;
    }

    // Reset FBO
    m_mpvFbo = {};
#endif
}

void MpvPlayer::render() {
#ifdef __vita__
    if (!m_mpvRenderCtx || !m_renderReady || !m_gxmFramebuffer) {
        return;
    }

    // Check if a new frame is available
    uint64_t flags = mpv_render_context_update(m_mpvRenderCtx);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
        return;  // No new frame
    }

    // Set up GXM render parameters for this frame
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_GXM_FBO, &m_mpvFbo},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    // Render the frame using GXM directly (not deferred)
    int result = mpv_render_context_render(m_mpvRenderCtx, render_params);
    if (result < 0) {
        brls::Logger::error("MpvPlayer: GXM render failed: {}", mpv_error_string(result));
    }
    mpv_render_context_report_swap(m_mpvRenderCtx);
#endif
}

} // namespace vitaplex
