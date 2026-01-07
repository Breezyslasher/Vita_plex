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

// Plex client identification
#define PLEX_CLIENT_ID "vita-plex-client-001"
#define PLEX_CLIENT_NAME "VitaPlex"
#define PLEX_CLIENT_VERSION VITA_PLEX_VERSION
#define PLEX_PLATFORM "PlayStation Vita"
#define PLEX_DEVICE "PS Vita"

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

    // Layout Settings
    bool showLibrariesInSidebar = false;  // Show libraries in sidebar instead of Library tab
    bool collapseSidebar = false;         // Collapse sidebar to icons only
    std::string hiddenLibraries;          // Comma-separated list of library keys to hide
    std::string sidebarOrder;             // Custom sidebar order (comma-separated: home,library,search,livetv,settings)

    // Content Display Settings
    bool showCollections = true;          // Show collections in library sections
    bool showPlaylists = true;            // Show playlists
    bool showGenres = true;               // Show genre categories

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
    int connectionTimeout = 180; // seconds (3 minutes for slow connections)
    bool directPlay = false;     // Try direct play first

    // Download Settings
    bool autoStartDownloads = true;    // Start downloads automatically after queueing
    bool downloadOverWifiOnly = false; // Only download when on WiFi
    int maxConcurrentDownloads = 1;    // Max concurrent downloads
    bool deleteAfterWatch = false;     // Auto-delete after fully watched
    bool syncProgressOnConnect = true; // Sync offline progress when connected
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
