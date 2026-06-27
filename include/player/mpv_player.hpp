/**
 * VitaPlex - MPV Video Player
 * Hardware-accelerated video playback using libmpv with GXM rendering on Vita
 */

#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>

#include "app/application.hpp"

#if defined(__vita__)
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gxm.h>
#elif defined(__ANDROID__)
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#else
#include <mpv/client.h>
#include <mpv/render.h>
#if defined(__SWITCH__) && defined(BOREALIS_USE_OPENGL)
// Switch OpenGL build (Mesa/nouveau + GLFW): use mpv's OpenGL render API so
// mpv renders straight into a GPU texture instead of a CPU framebuffer.
#include <mpv/render_gl.h>
#endif
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

    // Audio-only mode (disables video decoding for music playback)
    void setAudioOnly(bool audioOnly);
    bool isAudioOnly() const { return m_audioOnly; }

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

    // Track info (from MPV's track-list)
    struct TrackInfo {
        int id = 0;              // MPV track ID
        std::string type;        // "video", "audio", "sub"
        std::string title;       // Track title (if any)
        std::string lang;        // Language code (e.g., "eng")
        std::string codec;       // Codec name (e.g., "h264", "aac")
        bool selected = false;   // Currently selected
        bool isDefault = false;
    };

    // Tracks
    void setSubtitleTrack(int track);
    void setAudioTrack(int track);
    void setVideoTrack(int track);
    void cycleSubtitle();
    void cycleAudio();
    void toggleSubtitles();
    void setSubtitleDelay(double seconds);
    void setAudioDelay(double seconds);
    void disableSubtitles();
    void loadSubtitleUrl(const std::string& url);   // Load external subtitle (e.g. lyrics) by URL
    void removeExternalSubtitles();                   // Remove all external subtitle tracks

    // Get available tracks by type
    std::vector<TrackInfo> getTrackList(const std::string& type = "") const;

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

    // Flush GPU pipeline to serialize GXM access between MPV and NanoVG
    static void flushGpu();

    // Check if render context is available (video mode vs audio-only)
    bool hasRenderContext() const { return m_mpvRenderCtx != nullptr; }

    // Get NanoVG image handle for drawing video (returns 0 if not available)
    int getVideoImage() const { return m_nvgImage; }

    // Get video dimensions
    int getVideoWidth() const { return m_videoWidth; }
    int getVideoHeight() const { return m_videoHeight; }

#ifdef __ANDROID__
    // Direct-surface playback (see docs/android-direct-surface-playback.md).
    // mpv renders straight to a dedicated Android SurfaceView via vo=gpu
    // instead of the FBO/NanoVG composite path. The JNI layer
    // (src/platform/android_mpv_surface.cpp) owns the jobject global ref
    // and passes its address here as the mpv "wid".
    //
    // wid is reinterpret_cast<intptr_t> of a JNI global ref to the Java
    // Surface. Passing 0 detaches. Stage 1 only wires these — nothing
    // calls them until the SurfaceView lands (Stage 2) and the VO path
    // flips (Stage 4).
    void attachAndroidSurface(int64_t wid);
    void detachAndroidSurface();
    // Forwarded from the SurfaceView's surfaceChanged callback.
    void setAndroidSurfaceSize(int width, int height);
#endif

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
    std::atomic<bool> m_stopping{false};        // Shutdown in progress (accessed from mpv thread)
    bool m_commandPending = false;  // Async command pending
    bool m_audioOnly = false;       // Audio-only mode (no video decoding)

    // Static callback for render updates (called from MPV thread)
    static void onRenderUpdate(void* ctx);

#ifdef __vita__
    // GXM render resources
    void* m_gxmFramebuffer = nullptr;   // GXM framebuffer structure
    mpv_gxm_fbo m_mpvFbo = {};          // MPV GXM FBO parameters
    int m_flipY = 1;                    // Flip Y for correct orientation (matching switchfin)
    mpv_render_param m_mpvParams[3] = {};  // Render params: FLIP_Y + GXM_FBO + INVALID
#endif

#ifdef __ANDROID__
    // OpenGL render resources (Android TV: zero-copy GPU rendering).
    // mpv renders into m_glFbo, which has a NanoVG-managed GL texture as its
    // color attachment. NanoVG draws from that texture the next frame — no
    // CPU copy in the pipeline. Orientation is handled by NVG_IMAGE_FLIPY on
    // the NanoVG image; mpv renders natively (no MPV_RENDER_PARAM_FLIP_Y).
    unsigned int m_glFbo = 0;
    mpv_opengl_fbo m_mpvOpenGLFbo = {};
    mpv_render_param m_mpvParams[2] = {};
#endif

#if defined(__SWITCH__) && defined(BOREALIS_USE_OPENGL)
    // Switch OpenGL (Mesa/nouveau + GLFW) zero-copy GPU render path, mirroring
    // the Android one above. mpv renders into m_glFbo, whose color attachment
    // is a NanoVG-managed GL texture, so there's no per-frame CPU upload — the
    // big difference from the software path and the main cure for choppy Switch
    // video. switchfin uses deko3d for the same effect, but VitaPlex's borealis
    // has no deko3d/NanoVG interop helper, whereas the OpenGL build exposes
    // nvglImageHandleGL3 and fits the existing offscreen-composite model.
    //
    // If the prebuilt switch-libmpv was built without GL render support,
    // initRenderContext() falls back to the software path (m_videoBuffer, still
    // compiled in below) and leaves m_useGlRender false. onRenderUpdate() and
    // cleanupRenderContext() branch on m_useGlRender to pick the active path.
    unsigned int m_glFbo = 0;
    mpv_opengl_fbo m_mpvOpenGLFbo = {};
    bool m_useGlRender = false;
#endif

    int m_nvgImage = 0;
    // Initial FBO dimensions are set from platform::getVideoConstraints() in
    // setupNonVitaRender(); start at 0 so we don't depend on compile-time macros.
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    std::atomic<bool> m_renderReady{false};
    std::mutex m_renderMutex;
#if !defined(__vita__) && !defined(__ANDROID__)
    std::vector<unsigned char> m_videoBuffer;
#endif
};

} // namespace vitaplex
