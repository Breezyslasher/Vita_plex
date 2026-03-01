/**
 * VitaPlex - MPV Video Player Implementation
 * Based on switchfin's MPV implementation for PS Vita
 * Using software rendering with NanoVG display
 */

#include "player/mpv_player.hpp"
#include "app/application.hpp"
#include <borealis.hpp>

#ifdef __vita__
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/gxm.h>
#include <nanovg.h>
#include <nanovg_gxm.h>
#include <nanovg_gxm_utils.h>
#include <borealis/platforms/psv/psv_video.hpp>
#endif

#include <cstring>
#include <cstdlib>
#include <clocale>
#include <mutex>
#include <vector>

namespace vitaplex {

#ifdef __vita__
// Flush the GXM GPU pipeline to ensure it's idle before mpv uses the shared
// GXM context. GXM is NOT thread-safe, so mpv's decoder threads and the main
// thread's NanoVG rendering must not use the GPU concurrently.
static void flushGxmPipeline() {
    brls::PsvVideoContext* videoContext = dynamic_cast<brls::PsvVideoContext*>(
        brls::Application::getPlatform()->getVideoContext());
    if (videoContext) {
        NVGXMwindow* gxm = videoContext->getWindow();
        if (gxm && gxm->context) {
            sceGxmFinish(gxm->context);
        }
    }
}

void MpvPlayer::flushGpu() {
    flushGxmPipeline();
}
#endif

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
    mpv_set_option_string(m_mpv, "ytdl", "no");  // Disable youtube-dl (like switchfin)
    mpv_set_option_string(m_mpv, "reset-on-next-file", "speed,pause");  // Reset state between files

    // ========================================
    // Video output configuration
    // ========================================

    if (m_audioOnly) {
        // Audio-only mode: disable all video processing
        brls::Logger::info("MpvPlayer: Initializing in audio-only mode");
        mpv_set_option_string(m_mpv, "vo", "null");
        mpv_set_option_string(m_mpv, "vid", "no");
        mpv_set_option_string(m_mpv, "video", "no");
        mpv_set_option_string(m_mpv, "audio-display", "no");  // Don't try to show album art
        mpv_set_option_string(m_mpv, "hwdec", "no");
    } else {
        // Video mode: use libmpv for video output - we'll create a render context
        brls::Logger::info("MpvPlayer: Initializing in video mode");
        mpv_set_option_string(m_mpv, "vo", "libmpv");

#ifdef __vita__
        // Vita-specific settings from switchfin
        // Use 2 decoder threads for software decode. More threads require
        // more stack memory (each pthread gets 512KB via our wrapper).
        mpv_set_option_string(m_mpv, "vd-lavc-threads", "2");
        mpv_set_option_string(m_mpv, "vd-lavc-skiploopfilter", "all");
        mpv_set_option_string(m_mpv, "vd-lavc-fast", "yes");

        // Disable hardware decoding. vita-copy uses the shared GXM immediate
        // context from its decoder thread, which races with NanoVG on the main
        // thread and causes a deterministic crash at eboot+0x161b0. Software
        // decoding keeps all GXM usage on the main thread (via the render
        // callback + brls::sync), eliminating the threading conflict.
        mpv_set_option_string(m_mpv, "hwdec", "no");

        // GXM-specific settings from switchfin
        mpv_set_option_string(m_mpv, "fbo-format", "rgba8");
        mpv_set_option_string(m_mpv, "video-latency-hacks", "yes");
#else
        mpv_set_option_string(m_mpv, "hwdec", "no");
#endif
    }

    // ========================================
    // Audio output configuration
    // ========================================

    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    mpv_set_option_string(m_mpv, "volume", "100");
    mpv_set_option_string(m_mpv, "volume-max", "150");

#ifdef __vita__
    if (m_audioOnly) {
        // Larger audio buffer to absorb network jitter and CPU stalls.
        // 500ms was too small and caused choppy playback; 1.5s gives
        // enough runway to mask brief stalls on Vita's slow CPU.
        mpv_set_option_string(m_mpv, "audio-buffer", "1.5");

        // Read ahead aggressively so the demuxer always has data ready
        mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "15");
    }
#endif

    // ========================================
    // Cache and demuxer settings
    // ========================================

#ifdef __vita__
    if (m_audioOnly) {
        // Audio streaming: enable cache and buffer enough data before starting
        // playback so the audio pipeline doesn't starve mid-stream.
        mpv_set_option_string(m_mpv, "cache", "yes");
        mpv_set_option_string(m_mpv, "cache-secs", "10");       // buffer 10s before starting
        mpv_set_option_string(m_mpv, "demuxer-max-bytes", "2MiB");
        mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "512KiB");
    } else {
        // Video: HLS streaming needs cache to buffer .ts segments.
        // Without cache, the HLS demuxer can't properly download and
        // queue segments, leading to crashes on the first frame.
        mpv_set_option_string(m_mpv, "cache", "yes");
        mpv_set_option_string(m_mpv, "demuxer-max-bytes", "2MiB");
        mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "512KiB");
        // HLS-specific: give the demuxer more time to probe codec parameters.
        // Fixes "Could not find codec parameters" warnings with TS streams.
        // Keep probesize conservative (2MB) - the Vita only has 192MB heap total
        // and 10MB probesize was causing excessive memory pressure during HLS init.
        mpv_set_option_string(m_mpv, "demuxer-lavf-analyzeduration", "5");
        mpv_set_option_string(m_mpv, "demuxer-lavf-probesize", "2000000");
    }
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
    mpv_set_option_string(m_mpv, "user-agent", PLEX_CLIENT_NAME "/" PLEX_CLIENT_VERSION);

    // Per official Plex API (developer.plex.tv/pms), X-Plex-Client-Identifier
    // is a REQUIRED HTTP header (in=header). Set it here so MPV sends it
    // when streaming from Plex transcode endpoints.
    mpv_set_option_string(m_mpv, "http-header-fields",
        "X-Plex-Client-Identifier: VitaPlex,"
        "X-Plex-Product: VitaPlex,"
        "X-Plex-Version: 1.0.0,"
        "X-Plex-Platform: PlayStation Vita,"
        "X-Plex-Device: PS Vita,"
        "X-Plex-Client-Profile-Name: Generic,"
        "X-Plex-Device-Name: PS Vita");

    // Note: demuxer-lavf-probe-info and force-seekable caused crashes on Vita
    // Keep options minimal for compatibility

    // ========================================
    // Seek settings for faster seeking
    // ========================================

#ifdef __vita__
    // Use keyframe-based seeking for faster forward/rewind (especially for audio)
    // hr-seek=no means seek to nearest keyframe instead of exact position
    // This is much faster and prevents stuttering during seek operations
    mpv_set_option_string(m_mpv, "hr-seek", "no");

    // Don't wait for audio to resync after seeking - reduces seek delay
    mpv_set_option_string(m_mpv, "hr-seek-framedrop", "yes");
#endif

    // ========================================
    // Subtitle settings
    // ========================================

    mpv_set_option_string(m_mpv, "sub-auto", "fuzzy");
    mpv_set_option_string(m_mpv, "subs-fallback", "yes");

    // ========================================
    // Request log messages for debugging
    // ========================================

    mpv_request_log_messages(m_mpv, "warn");  // Use warn level to reduce log spam on Vita

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
    // Set up render context for video display (skip for audio-only)
    // ========================================

    if (!m_audioOnly) {
        if (!initRenderContext()) {
            brls::Logger::error("MpvPlayer: Failed to create render context, falling back to audio-only");
            // Don't fail - we can still play audio
        }
    } else {
        brls::Logger::info("MpvPlayer: Skipping render context for audio-only mode");
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

        m_stopping.store(true);

        // Clean up render context first (locks m_renderMutex to wait for in-flight renders)
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
        m_stopping.store(false);
    }
    m_state = MpvPlayerState::IDLE;
    m_commandPending = false;
}

void MpvPlayer::setAudioOnly(bool audioOnly) {
    if (m_audioOnly == audioOnly) return;

    brls::Logger::info("MpvPlayer: Setting audio-only mode: {}", audioOnly);

    // If player is already initialized, we need to reinitialize with new mode
    // because vo and hwdec settings can only be set before mpv_initialize
    if (m_mpv) {
        brls::Logger::info("MpvPlayer: Reinitializing to change mode...");
        shutdown();
    }

    m_audioOnly = audioOnly;
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

    std::string normalizedUrl = url;

    // Normalize URL scheme to lowercase for http/https (handles Http, HTTP, HtTp, etc.)
    if (normalizedUrl.length() > 7) {
        // Check for http:// or https:// (case insensitive)
        std::string prefix = normalizedUrl.substr(0, 8);
        for (auto& c : prefix) c = tolower(c);

        if (prefix.find("http://") == 0 || prefix.find("https://") == 0) {
            // Find the :// and lowercase everything before it
            size_t colonPos = normalizedUrl.find("://");
            if (colonPos != std::string::npos) {
                for (size_t i = 0; i < colonPos; i++) {
                    normalizedUrl[i] = tolower(normalizedUrl[i]);
                }
            }
        }
    }

    brls::Logger::info("MpvPlayer: Loading URL: {}", normalizedUrl);

    m_currentUrl = normalizedUrl;
    m_playbackInfo = MpvPlaybackInfo();
    m_playbackInfo.mediaTitle = title;

    // Mark command as pending
    m_commandPending = true;

#ifdef __vita__
    // Disable render callback during loading to prevent GXM context conflicts.
    // Will be re-enabled in FILE_LOADED event handler.
    m_renderReady.store(false);
#endif

    // Use simple loadfile command - options are already set globally during init()
    // Format: loadfile <url> [flags]
    // Note: Per-file options (5th arg) require different format and aren't well supported
    brls::Logger::debug("MpvPlayer: Sending loadfile command...");

#ifdef __vita__
    // Flush GPU pipeline before loadfile. When mpv processes the loadfile command,
    // it spawns decoder threads that use the shared GXM context. If NanoVG's GPU
    // operations are still in flight, the concurrent GXM access crashes Thread 6.
    if (m_mpvRenderCtx) {
        flushGxmPipeline();
    }
#endif

    const char* cmd[] = {"loadfile", normalizedUrl.c_str(), "replace", nullptr};
    int result = mpv_command_async(m_mpv, CMD_LOADFILE, cmd);
    if (result < 0) {
        m_errorMessage = std::string("Failed to queue load command: ") + mpv_error_string(result);
        brls::Logger::error("MpvPlayer: {}", m_errorMessage);
        m_commandPending = false;
        setState(MpvPlayerState::ERROR);
        return false;
    }
    brls::Logger::debug("MpvPlayer: loadfile command queued successfully (result={})", result);

    brls::Logger::debug("MpvPlayer: About to call setState(LOADING)...");
    setState(MpvPlayerState::LOADING);
    brls::Logger::debug("MpvPlayer: setState done, about to return true");
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

    // Don't seek if not actively playing/paused
    if (m_state != MpvPlayerState::PLAYING && m_state != MpvPlayerState::PAUSED) {
        brls::Logger::debug("MpvPlayer: Cannot seek in state {}", (int)m_state);
        return;
    }

    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%.2f", seconds);

    const char* cmd[] = {"seek", timeStr, "absolute", NULL};
    mpv_command_async(m_mpv, CMD_SEEK, cmd);
}

void MpvPlayer::seekRelative(double seconds) {
    if (!m_mpv || m_stopping) return;

    // Don't seek if not actively playing/paused
    if (m_state != MpvPlayerState::PLAYING && m_state != MpvPlayerState::PAUSED) return;

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
    brls::Logger::debug("MpvPlayer::setState entered with newState={}", (int)newState);
    if (m_state != newState) {
        brls::Logger::debug("MpvPlayer: State change: {} -> {}", (int)m_state, (int)newState);
        m_state = newState;
        brls::Logger::debug("MpvPlayer::setState assignment done");
    }
    brls::Logger::debug("MpvPlayer::setState exiting");
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
#ifdef __vita__
                // Now that the file is loaded and decoders are initialized,
                // enable the render callback so MPV can display frames.
                // This is deferred from init to avoid GXM context conflicts
                // between MPV's threads and NanoVG during the loading phase.
                if (m_mpvRenderCtx && !m_renderReady.load()) {
                    brls::Logger::info("MpvPlayer: Enabling render callback");
                    // Flush twice with a sync barrier to ensure all pending GXM
                    // operations from both NanoVG and mpv's decoder init are
                    // fully retired before we enable frame rendering.
                    flushGxmPipeline();
                    // Do an initial "dummy" render check so mpv's internal
                    // GXM state is fully initialized before the real callback.
                    {
                        std::lock_guard<std::mutex> lock(m_renderMutex);
                        uint64_t flags = mpv_render_context_update(m_mpvRenderCtx);
                        if (flags & MPV_RENDER_UPDATE_FRAME) {
                            flushGxmPipeline();
                            int result = mpv_render_context_render(m_mpvRenderCtx, m_mpvParams);
                            if (result < 0) {
                                brls::Logger::error("MpvPlayer: Initial render failed: {}", mpv_error_string(result));
                            }
                            mpv_render_context_report_swap(m_mpvRenderCtx);
                            flushGxmPipeline();
                        }
                    }
                    brls::Logger::info("MpvPlayer: Render callback enabled");
                    m_renderReady.store(true);
                }
#endif
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

#ifdef __vita__
// Callback for mpv render context when a new frame is ready
// Uses brls::sync() to render on main thread OUTSIDE of draw phase (like switchfin)
void MpvPlayer::onRenderUpdate(void* ctx) {
    MpvPlayer* player = static_cast<MpvPlayer*>(ctx);
    if (!player || player->m_stopping.load() || !player->m_renderReady.load()) {
        return;
    }

    // Schedule render on main thread - brls::sync executes between frames, not during draw
    brls::sync([player]() {
        // Lock the render mutex to prevent concurrent access to GXM resources
        // (cleanupRenderContext also locks this mutex before freeing resources)
        std::lock_guard<std::mutex> lock(player->m_renderMutex);

        // Double-check state inside sync+lock in case player was destroyed between
        // when brls::sync was called and when this lambda actually executes
        if (!player->m_renderReady.load() || !player->m_mpvRenderCtx ||
            !player->m_gxmFramebuffer || player->m_stopping.load()) {
            return;
        }

        // Flush GPU pipeline to ensure NanoVG's previous frame is complete
        // before mpv starts a new GXM scene on the shared context
        flushGxmPipeline();

        uint64_t flags = mpv_render_context_update(player->m_mpvRenderCtx);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            int result = mpv_render_context_render(player->m_mpvRenderCtx, player->m_mpvParams);
            if (result < 0) {
                brls::Logger::error("MpvPlayer: GXM render failed: {}", mpv_error_string(result));
            }
            mpv_render_context_report_swap(player->m_mpvRenderCtx);

            // Flush again after mpv render so its GXM operations complete
            // before NanoVG begins the next frame
            flushGxmPipeline();
        }
    });
}
#endif

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

    // Flush the GPU pipeline before sharing the GXM context with mpv.
    // mpv's decoder threads will use this context, and GXM is not thread-safe.
    flushGxmPipeline();

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

    // Create NanoVG image with zeroed (black) pixel data.
    // Passing nullptr would leave the GXM texture uninitialized, which can cause
    // a GPU fault if NanoVG's draw pipeline touches it before MPV renders a frame.
    size_t pixelDataSize = (size_t)m_videoWidth * m_videoHeight * 4;
    std::vector<unsigned char> blackPixels(pixelDataSize, 0);
    m_nvgImage = nvgCreateImageRGBA(vg, m_videoWidth, m_videoHeight, 0, blackPixels.data());
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

    // Set up render params array for use in callbacks (like switchfin)
    m_mpvParams[0] = {MPV_RENDER_PARAM_GXM_FBO, &m_mpvFbo};
    m_mpvParams[1] = {MPV_RENDER_PARAM_INVALID, nullptr};

    // Register the render update callback but keep m_renderReady=false.
    // The callback checks m_renderReady and will skip rendering until it's true.
    // We defer setting m_renderReady=true until FILE_LOADED event, so MPV's
    // render callback doesn't use the shared GXM context during the loading phase
    // (which would conflict with NanoVG on the main thread).
    mpv_render_context_set_update_callback(m_mpvRenderCtx, onRenderUpdate, this);

    // Flush the GXM pipeline after all resource creation to ensure:
    // 1. mpv_render_context_create's shader patcher ops are fully committed
    // 2. The framebuffer's render target and surfaces are finalized
    // 3. The NanoVG texture upload (black pixels) is complete
    // Without this flush, the next NanoVG draw frame can hit stale/conflicting
    // GXM state left behind by the render context and framebuffer creation.
    flushGxmPipeline();

    brls::Logger::info("MpvPlayer: GXM render context created successfully ({}x{})", m_videoWidth, m_videoHeight);
    return true;
#else
    // Non-Vita builds don't need render context
    return false;
#endif
}

void MpvPlayer::cleanupRenderContext() {
#ifdef __vita__
    // Signal that render is no longer ready FIRST (atomic, visible to mpv thread immediately)
    m_renderReady.store(false);

    // Flush GPU pipeline before freeing GXM resources
    flushGxmPipeline();

    {
        // Lock the render mutex to ensure no in-flight render callback is accessing
        // GXM resources while we free them
        std::lock_guard<std::mutex> lock(m_renderMutex);

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

        // Reset FBO and render params
        m_mpvFbo = {};
        m_mpvParams[0] = {MPV_RENDER_PARAM_INVALID, nullptr};
        m_mpvParams[1] = {MPV_RENDER_PARAM_INVALID, nullptr};
    }
#endif
}

void MpvPlayer::render() {
    // Rendering is handled in onRenderUpdate callback via brls::sync()
    // This function exists for API compatibility but does nothing
    // VideoView::draw() just displays the already-rendered NanoVG texture
}

} // namespace vitaplex
