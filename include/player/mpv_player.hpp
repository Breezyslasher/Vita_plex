/**
 * VitaPlex - MPV Video Player
 * Hardware-accelerated video playback using libmpv with GXM rendering on Vita
 */

#pragma once

#include <string>

#ifdef __vita__
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gxm.h>
#else
// Stub for non-Vita builds
typedef struct mpv_handle mpv_handle;
typedef struct mpv_event mpv_event;
typedef struct mpv_event_property mpv_event_property;
typedef struct mpv_render_context mpv_render_context;
#endif

namespace vitaplex {

// Player states
enum class MpvPlayerState {
    IDLE,
    LOADING,
    PLAYING,
    PAUSED,
    BUFFERING,
    ENDED,
    ERROR
};

// Playback info
struct MpvPlaybackInfo {
    double position = 0.0;
    double duration = 0.0;
    int volume = 100;
    bool muted = false;
    std::string mediaTitle;
    std::string videoCodec;
    int videoWidth = 0;
    int videoHeight = 0;
    double fps = 0.0;
    std::string audioCodec;
    int audioChannels = 0;
    int sampleRate = 0;
    int subtitleTrack = 0;
    int audioTrack = 0;
    double cacheUsed = 0.0;
    bool seeking = false;
    bool buffering = false;
    double bufferingPercent = 0.0;
};

/**
 * MPV-based video player with GXM rendering support on Vita
 */
class MpvPlayer {
public:
    static MpvPlayer& getInstance();

    // Lifecycle
    bool init();
    void shutdown();
    bool isInitialized() const { return m_mpv != nullptr; }

    // Playback control
    bool loadUrl(const std::string& url, const std::string& title = "");
    bool loadFile(const std::string& path);
    void play();
    void pause();
    void togglePause();
    void stop();

    // Seeking
    void seekTo(double seconds);
    void seekRelative(double seconds);
    void seekPercent(double percent);
    void seekChapter(int delta);

    // Volume
    void setVolume(int percent);
    int getVolume() const;
    void adjustVolume(int delta);
    void setMute(bool muted);
    bool isMuted() const;
    void toggleMute();

    // Tracks
    void setSubtitleTrack(int track);
    void setAudioTrack(int track);
    void cycleSubtitle();
    void cycleAudio();
    void toggleSubtitles();
    void setSubtitleDelay(double seconds);
    void setAudioDelay(double seconds);

    // State
    MpvPlayerState getState() const { return m_state; }
    bool isPlaying() const { return m_state == MpvPlayerState::PLAYING; }
    bool isPaused() const { return m_state == MpvPlayerState::PAUSED; }
    bool isIdle() const { return m_state == MpvPlayerState::IDLE; }
    bool isLoading() const { return m_state == MpvPlayerState::LOADING || m_state == MpvPlayerState::BUFFERING; }
    bool hasEnded() const { return m_state == MpvPlayerState::ENDED; }
    bool hasError() const { return m_state == MpvPlayerState::ERROR; }

    // Info
    double getPosition() const;
    double getDuration() const;
    double getPercentPosition() const;
    const MpvPlaybackInfo& getPlaybackInfo() const { return m_playbackInfo; }
    std::string getErrorMessage() const { return m_errorMessage; }

    // OSD
    void showOSD(const std::string& text, double durationSec = 2.0);
    void toggleOSD();

    // Options and properties
    void setOption(const std::string& name, const std::string& value);
    std::string getProperty(const std::string& name) const;

    // Update (call in render loop)
    void update();
    void render();

    // Check if render context is available (video mode vs audio-only)
    bool hasRenderContext() const { return m_mpvRenderCtx != nullptr; }

    // Get NanoVG image handle for drawing video (returns 0 if not available)
    int getVideoImage() const {
#ifdef __vita__
        return m_nvgImage;
#else
        return 0;
#endif
    }

    // Get video dimensions
    int getVideoWidth() const { return 960; }
    int getVideoHeight() const { return 544; }

private:
    MpvPlayer() = default;
    ~MpvPlayer();
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    bool initRenderContext();
    void cleanupRenderContext();
    void eventMainLoop();
    void updatePlaybackInfo();
    void handleEvent(mpv_event* event);
    void handlePropertyChange(mpv_event_property* prop, uint64_t id);
    void setState(MpvPlayerState newState);

    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_mpvRenderCtx = nullptr;
    MpvPlayerState m_state = MpvPlayerState::IDLE;
    MpvPlaybackInfo m_playbackInfo;
    std::string m_errorMessage;
    std::string m_currentUrl;
    bool m_subtitlesVisible = true;
    bool m_stopping = false;        // Shutdown in progress
    bool m_commandPending = false;  // Async command pending

#ifdef __vita__
    // GXM render resources
    int m_nvgImage = 0;                 // NanoVG image handle for display
    void* m_gxmFramebuffer = nullptr;   // GXM framebuffer structure
    mpv_gxm_fbo m_mpvFbo = {};          // MPV GXM FBO parameters
    int m_videoWidth = 960;
    int m_videoHeight = 544;
    bool m_renderReady = false;         // Flag for when frame is ready
#endif
};

} // namespace vitaplex
