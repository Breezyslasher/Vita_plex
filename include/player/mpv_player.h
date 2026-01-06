/**
 * VitaPlex - MPV Video Player
 * Hardware-accelerated video playback using libmpv with FFmpeg-vita
 */

#pragma once

#include <string>
#include <mpv/client.h>

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

// Playback info structure
struct MpvPlaybackInfo {
    double position = 0.0;      // Current position in seconds
    double duration = 0.0;      // Total duration in seconds
    int volume = 100;           // Volume 0-100
    bool muted = false;
    std::string mediaTitle;
    
    // Video info
    std::string videoCodec;
    int videoWidth = 0;
    int videoHeight = 0;
    double fps = 0.0;
    int videoBitrate = 0;
    
    // Audio info
    std::string audioCodec;
    int audioChannels = 0;
    int sampleRate = 0;
    int audioBitrate = 0;
    
    // Subtitle
    int subtitleTrack = 0;
    int audioTrack = 0;
    
    // Buffer
    double cacheUsed = 0.0;     // Cache used in seconds
    bool seeking = false;
    bool buffering = false;
    double bufferingPercent = 0.0;
};

/**
 * MPV-based video player for PS Vita
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
    void seekChapter(int delta);  // +1 next, -1 previous
    
    // Volume control
    void setVolume(int percent);
    int getVolume() const;
    void adjustVolume(int delta);
    void setMute(bool muted);
    bool isMuted() const;
    void toggleMute();
    
    // Track selection
    void setSubtitleTrack(int track);
    void setAudioTrack(int track);
    void cycleSubtitle();
    void cycleAudio();
    void setSubtitleDelay(double seconds);
    void setAudioDelay(double seconds);
    void toggleSubtitles();
    
    // State queries
    MpvPlayerState getState() const { return m_state; }
    bool isPlaying() const { return m_state == MpvPlayerState::PLAYING; }
    bool isPaused() const { return m_state == MpvPlayerState::PAUSED; }
    bool isIdle() const { return m_state == MpvPlayerState::IDLE; }
    bool isLoading() const { return m_state == MpvPlayerState::LOADING || m_state == MpvPlayerState::BUFFERING; }
    bool hasEnded() const { return m_state == MpvPlayerState::ENDED; }
    bool hasError() const { return m_state == MpvPlayerState::ERROR; }
    
    // Playback info
    double getPosition() const;
    double getDuration() const;
    double getPercentPosition() const;
    const MpvPlaybackInfo& getPlaybackInfo() const { return m_playbackInfo; }
    std::string getErrorMessage() const { return m_errorMessage; }
    
    // OSD
    void showOSD(const std::string& text, double durationSec = 2.0);
    void toggleOSD();
    
    // Render frame (call this in your render loop)
    void update();
    
    // Render video frame (with vita2d vo, this is handled internally)
    void render();
    
    // Properties
    void setOption(const std::string& name, const std::string& value);
    std::string getProperty(const std::string& name) const;
    
private:
    MpvPlayer() = default;
    ~MpvPlayer();
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;
    
    void processEvents();
    void updatePlaybackInfo();
    void handleEvent(mpv_event* event);
    void handlePropertyChange(mpv_event_property* prop);
    void setState(MpvPlayerState newState);
    
    // MPV handle
    mpv_handle* m_mpv = nullptr;
    
    // State
    MpvPlayerState m_state = MpvPlayerState::IDLE;
    MpvPlaybackInfo m_playbackInfo;
    std::string m_errorMessage;
    std::string m_currentUrl;
    bool m_subtitlesVisible = true;
};

} // namespace vitaplex
