/**
 * VitaPlex - Plex Client for PlayStation Vita
 * Borealis-based Application
 */

#pragma once

#include <string>
#include <functional>

// Application version
#define VITA_PLEX_VERSION "2.0.0"
#define VITA_PLEX_VERSION_NUM 200

// Plex client identification - Client ID must be a proper UUID format
#define PLEX_CLIENT_ID "a3f5c8d2-7b9e-4f1a-8c6d-2e5f9b4a1c3d"
#define PLEX_CLIENT_NAME "VitaPlex"
#define PLEX_CLIENT_VERSION VITA_PLEX_VERSION
#define PLEX_PLATFORM "PlayStation Vita"
#define PLEX_DEVICE "PS Vita"
#define PLEX_DEVICE_NAME "VitaPlex"

namespace vitaplex {

// Theme options
enum class AppTheme {
    SYSTEM = 0,  // Follow system setting
    LIGHT = 1,
    DARK = 2
};

// Video quality options for transcoding
enum class VideoQuality {
    ORIGINAL = 0,      // Direct play/stream
    QUALITY_1080P = 1, // 1080p 20Mbps
    QUALITY_720P = 2,  // 720p 4Mbps
    QUALITY_480P = 3,  // 480p 2Mbps (recommended for Vita)
    QUALITY_360P = 4,  // 360p 1Mbps
    QUALITY_240P = 5   // 240p 500kbps
};

// Subtitle size options
enum class SubtitleSize {
    SMALL = 0,
    MEDIUM = 1,
    LARGE = 2
};

// Application settings structure
struct AppSettings {
    // UI Settings
    AppTheme theme = AppTheme::DARK;
    bool showClock = true;
    bool animationsEnabled = true;
    bool debugLogging = true;  // Enable debug logging

    // Playback Settings
    bool autoPlayNext = true;
    bool resumePlayback = true;
    bool showSubtitles = true;
    SubtitleSize subtitleSize = SubtitleSize::MEDIUM;
    int seekInterval = 10;  // seconds

    // Transcode Settings
    VideoQuality videoQuality = VideoQuality::QUALITY_480P;
    bool forceTranscode = false;
    bool burnSubtitles = true;  // Burn subtitles into video for Vita compatibility
    int maxBitrate = 2000;      // kbps

    // Network Settings
    int connectionTimeout = 30; // seconds
    bool directPlay = false;    // Try direct play first
};

/**
 * Application singleton - manages app lifecycle and global state
 */
class Application {
public:
    static Application& getInstance();

    // Initialize and run the application
    bool init();
    void run();
    void shutdown();

    // Navigation
    void pushLoginActivity();
    void pushMainActivity();
    void pushPlayerActivity(const std::string& mediaKey);

    // Authentication state
    bool isLoggedIn() const { return !m_authToken.empty(); }
    const std::string& getAuthToken() const { return m_authToken; }
    void setAuthToken(const std::string& token) { m_authToken = token; }
    const std::string& getServerUrl() const { return m_serverUrl; }
    void setServerUrl(const std::string& url) { m_serverUrl = url; }

    // Settings persistence
    bool loadSettings();
    bool saveSettings();

    // User info
    const std::string& getUsername() const { return m_username; }
    void setUsername(const std::string& name) { m_username = name; }

    // Application settings access
    AppSettings& getSettings() { return m_settings; }
    const AppSettings& getSettings() const { return m_settings; }

    // Apply theme
    void applyTheme();

    // Apply log level based on settings
    void applyLogLevel();

    // Get quality string for display
    static std::string getQualityString(VideoQuality quality);
    static std::string getThemeString(AppTheme theme);
    static std::string getSubtitleSizeString(SubtitleSize size);

private:
    Application() = default;
    ~Application() = default;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool m_initialized = false;
    std::string m_authToken;
    std::string m_serverUrl;
    std::string m_username;
    AppSettings m_settings;
};

} // namespace vitaplex
