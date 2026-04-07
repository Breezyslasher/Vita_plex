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

// Platform-specific identification and transcode limits
#if defined(__vita__)
#define PLEX_PLATFORM "PlayStation Vita"
#define PLEX_DEVICE "PS Vita"
#define PLEX_MAX_VIDEO_WIDTH 960
#define PLEX_MAX_VIDEO_HEIGHT 544
#define PLEX_MAX_VIDEO_LEVEL 40
#define PLEX_DEFAULT_BITRATE 2000
#define PLEX_DEFAULT_RESOLUTION "960x544"
#elif defined(__ANDROID__)
#define PLEX_PLATFORM "Android"
#define PLEX_DEVICE "Android TV"
#define PLEX_MAX_VIDEO_WIDTH 1920
#define PLEX_MAX_VIDEO_HEIGHT 1080
#define PLEX_MAX_VIDEO_LEVEL 51
#define PLEX_DEFAULT_BITRATE 8000
#define PLEX_DEFAULT_RESOLUTION "1920x1080"
#elif defined(__SWITCH__)
#define PLEX_PLATFORM "Nintendo Switch"
#define PLEX_DEVICE "Switch"
#define PLEX_MAX_VIDEO_WIDTH 1920
#define PLEX_MAX_VIDEO_HEIGHT 1080
#define PLEX_MAX_VIDEO_LEVEL 42
#define PLEX_DEFAULT_BITRATE 4000
#define PLEX_DEFAULT_RESOLUTION "1280x720"
#elif defined(__PS4__)
#define PLEX_PLATFORM "PlayStation 4"
#define PLEX_DEVICE "PS4"
#define PLEX_MAX_VIDEO_WIDTH 1920
#define PLEX_MAX_VIDEO_HEIGHT 1080
#define PLEX_MAX_VIDEO_LEVEL 51
#define PLEX_DEFAULT_BITRATE 10000
#define PLEX_DEFAULT_RESOLUTION "1920x1080"
#else
// Desktop (Windows, macOS, Linux)
#define PLEX_PLATFORM "Desktop"
#define PLEX_DEVICE "Desktop"
#define PLEX_MAX_VIDEO_WIDTH 1920
#define PLEX_MAX_VIDEO_HEIGHT 1080
#define PLEX_MAX_VIDEO_LEVEL 51
#define PLEX_DEFAULT_BITRATE 10000
#define PLEX_DEFAULT_RESOLUTION "1920x1080"
#endif

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

// Default action when selecting a track in album view
enum class TrackDefaultAction {
    PLAY_NEXT = 0,           // Add after current track
    PLAY_NOW_REPLACE = 1,    // Replace current and play next
    ADD_TO_BOTTOM = 2,       // Add to end of queue
    PLAY_NOW_CLEAR = 3,      // Clear queue and play
    ASK_EACH_TIME = 4        // Show dialog each time
};

// Application settings structure
struct AppSettings {
    // UI Settings
    AppTheme theme = AppTheme::DARK;
    bool debugLogging = true;  // Enable debug logging
    bool showDebugTab = false;  // Show debug tab in sidebar

    // Layout Settings
    bool showLibrariesInSidebar = false;  // Show libraries in sidebar instead of Library tab
    bool collapseSidebar = false;         // Collapse sidebar to icons only
    std::string hiddenLibraries;          // Comma-separated list of library keys to hide
    std::string sidebarOrder;             // Custom sidebar order (comma-separated: home,library,search,livetv,settings)

    // Content Display Settings
    bool showCollections = true;          // Show collections in library sections
    bool showPlaylists = true;            // Show playlists
    bool showGenres = true;               // Show genre categories
    bool hideTitlesInGrid = false;        // Hide titles under movie/show posters in grid
    bool skipSingleSeason = false;        // Skip season view for single-season shows

    // Playback Settings
    bool autoPlayNext = true;
    bool resumePlayback = true;
    bool showSubtitles = true;
    SubtitleSize subtitleSize = SubtitleSize::MEDIUM;
    int seekInterval = 10;  // seconds
    int controlsAutoHideSeconds = 5;  // Auto-hide player controls after inactivity (0 = never)
    bool autoSkipIntro = false;       // Automatically skip intro markers
    bool autoSkipCredits = false;     // Automatically skip credits markers

    // Transcode Settings (defaults are platform-specific)
#if defined(__vita__)
    VideoQuality videoQuality = VideoQuality::QUALITY_480P;
    int maxBitrate = 2000;      // kbps
#elif defined(__SWITCH__)
    VideoQuality videoQuality = VideoQuality::QUALITY_720P;
    int maxBitrate = 4000;      // kbps
#else
    // Android, PS4, Desktop
    VideoQuality videoQuality = VideoQuality::QUALITY_1080P;
    int maxBitrate = 8000;      // kbps
#endif
    bool forceTranscode = false;

    // Network Settings
    int connectionTimeout = 180; // seconds (3 minutes for slow connections)
    bool directPlay = false;     // Try direct play first

    // Download Settings
    bool deleteAfterWatch = false;     // Auto-delete after fully watched

    // Music Settings
    TrackDefaultAction trackDefaultAction = TrackDefaultAction::ASK_EACH_TIME;  // Default action for tracks
    bool backgroundMusic = true;       // Allow leaving player without stopping music
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
    void pushPlayerActivity(const std::string& mediaKey, bool isLocalFile = false);
    void pushLiveTVPlayerActivity(const std::string& streamUrl, const std::string& channelTitle);

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

    // Offline mode
    bool isOfflineMode() const { return m_offlineMode; }
    void setOfflineMode(bool offline) { m_offlineMode = offline; }

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
    bool m_offlineMode = false;
    std::string m_authToken;
    std::string m_serverUrl;
    std::string m_username;
    AppSettings m_settings;
};

} // namespace vitaplex
