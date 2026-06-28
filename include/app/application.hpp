/**
 * VitaPlex - Plex Client for PlayStation Vita
 * Borealis-based Application
 */

#pragma once

#include <string>
#include <functional>

// Application version. Two flavours, both injected by the build system
// (CMakeLists.txt forwards them from -DAPP_VERSION / -DAPP_DISPLAY_VERSION):
//
//   VITA_PLEX_VERSION         numeric ("1.0.2" or "1.0.2.455") — embedded
//                             in Vita SFOs / PS4 pkgs / deb changelogs and
//                             sent as the X-Plex-Version header to Plex
//                             servers. Must look like a version number.
//
//   VITA_PLEX_DISPLAY_VERSION human-readable ("Beta 1.0.2") — shown in
//                             the Settings > Version cell. Can include
//                             labels and spaces.
//
// Fallbacks below apply only to ad-hoc builds that bypass CMake so the
// header still compiles standalone.
#ifndef VITA_PLEX_VERSION
#define VITA_PLEX_VERSION "2.0.0"
#endif
#ifndef VITA_PLEX_DISPLAY_VERSION
#define VITA_PLEX_DISPLAY_VERSION VITA_PLEX_VERSION
#endif
#define VITA_PLEX_VERSION_NUM 200

// Plex client identification
#define PLEX_CLIENT_ID "vita-plex-client-001"
#define PLEX_CLIENT_NAME "VitaPlex"
#define PLEX_CLIENT_VERSION VITA_PLEX_VERSION

// NOTE: per-platform Plex transcode identification and limits
// (previously PLEX_PLATFORM / PLEX_DEVICE / PLEX_MAX_VIDEO_* / PLEX_DEFAULT_*)
// now live in the platform abstraction layer — see
// include/platform/platform.hpp::VideoConstraints and the per-target
// implementations in src/platform/platform_<name>.cpp. Callers should read
// them via vitaplex::platform::getVideoConstraints() instead of using
// ifdef-guarded #defines.

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

    // Layout Settings
    bool collapseSidebar = false;         // Collapse sidebar to icons only
    std::string hiddenLibraries;          // Comma-separated list of library keys to hide
    std::string sidebarOrder;             // Custom sidebar order. Movable ids between Home and Settings,
                                          // comma-separated. Built-ins: search,livetv,downloads,library,music;
                                          // per-library ids are "lib:<sectionKey>".
    std::string hiddenSidebarItems;       // Comma-separated built-in sidebar ids hidden via the editor
                                          // (search,livetv,downloads,library,music). Libraries use hiddenLibraries.
    std::string librarySortPrefs;         // Per-section sort, encoded "key=param|label;key2=..."

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
    // ISO 639-1 / -2 code prefilled into the subtitle search dialog and
    // used as the default when the user opens "Search online for
    // subtitles…". Empty falls back to "en". User-editable from the
    // Settings tab.
    std::string defaultSubtitleLanguage = "en";

    // Transcode Settings — defaults are set by Application::init() from
    // platform::getVideoConstraints(). 0 means "use the platform default",
    // and downstream code treats videoQuality == ORIGINAL_UNSET the same.
    VideoQuality videoQuality = VideoQuality::QUALITY_1080P;
    int maxBitrate = 0;         // 0 = use platform default bitrate
    bool forceTranscode = false;

    // Network Settings
    int connectionTimeout = 180; // seconds (3 minutes for slow connections)
    bool directPlay = false;     // Try direct play first

    // SyncLounge (watch party) — remembered so the user doesn't retype them
    // each session. Server defaults to the public instance.
    std::string syncLoungeServer = "https://server.synclounge.tv";
    std::string syncLoungeRoom;

    // Download Settings
    bool deleteAfterWatch = false;     // Auto-delete after fully watched

    // Music Settings
    TrackDefaultAction trackDefaultAction = TrackDefaultAction::ASK_EACH_TIME;  // Default action for tracks
    bool backgroundMusic = true;       // Allow leaving player without stopping music

    // Live TV / DVR Settings
    // Library section the user wants new DVR recordings to land in. When
    // empty, scheduleRecording falls back to whatever the server-side
    // /media/subscriptions/template recommended. The title is cached
    // alongside the ID so the settings cell can render it without an
    // extra /library/sections fetch every time the tab opens.
    std::string defaultDvrSectionId;
    std::string defaultDvrSectionTitle;
    // Recording knobs forwarded as /media/subscriptions?prefs[...] on the
    // POST. Defaults mirror what scheduleRecording used to hardcode.
    int  dvrStartOffsetMinutes = 2;    // Pad recording start by this many minutes
    int  dvrEndOffsetMinutes   = 2;    // Pad recording end by this many minutes
    bool dvrRecordPartials     = true; // Keep recordings that didn't fully complete
    int  dvrMinVideoQuality    = 0;    // 0 = any; higher demands better source quality
    // EPG window the Live TV tab fetches and renders. Stays a multiple of
    // 6 so the time-header slots line up; LiveTVTab clamps it on read.
    int  liveTvGuideHours      = 12;

    // Plex Home users. When true, restoring a saved session goes straight
    // into the last-used user (current behaviour). When false, the boot
    // flow shows the user picker first so the user can switch accounts
    // without logging out.
    bool autoLoginAsLastUser = true;

    // HTTP response cache. Lifetime in minutes for the global on-disk
    // cache used by PlexClient (library sections, Live TV channels,
    // Home hubs). 0 disables caching entirely; non-zero values let
    // get() reuse a cached body up to that many minutes old.
    int cacheLifetimeMinutes = 60;

    // When true, PlayerActivity overlays a small mpv-stats panel at
    // the top-left of the video so the user can see codec, hwdec,
    // FPS, frame drops, and cache state in real time. Driven from the
    // Playback Tuning dialog; off by default.
    bool showMpvStats = false;
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
    void pushLiveTVPlayerActivity(const std::string& streamUrl, const std::string& channelTitle,
                                  const std::string& liveSessionUuid = "");

    // Authentication state
    bool isLoggedIn() const { return !m_authToken.empty(); }
    const std::string& getAuthToken() const { return m_authToken; }
    void setAuthToken(const std::string& token) { m_authToken = token; }
    const std::string& getServerUrl() const { return m_serverUrl; }
    void setServerUrl(const std::string& url) { m_serverUrl = url; }

    // Plex Home user state. The master token is the account-level token
    // returned by /users/signin or /pins/{id}; it's what fetchHomeUsers
    // and switchHomeUser need. m_authToken is the *effective* token,
    // either the master one (no user switched) or the per-user token
    // from switchHomeUser. When the user hasn't switched away from the
    // owner, master == authToken and the home-user fields stay empty.
    const std::string& getMasterAuthToken() const { return m_masterAuthToken; }
    void setMasterAuthToken(const std::string& token) { m_masterAuthToken = token; }
    const std::string& getCurrentHomeUserUuid() const { return m_currentHomeUserUuid; }
    void setCurrentHomeUserUuid(const std::string& uuid) { m_currentHomeUserUuid = uuid; }
    const std::string& getCurrentHomeUserTitle() const { return m_currentHomeUserTitle; }
    void setCurrentHomeUserTitle(const std::string& t) { m_currentHomeUserTitle = t; }

    // Settings persistence
    bool loadSettings();
    bool saveSettings();

    // Plex Home user picker. Fetches the Home users list using the
    // master token and presents a Dropdown of names. On pick, prompts
    // for a PIN if the user is protected, switches via /home/users/{uuid}/
    // switch, saves the new per-user token to settings, then invokes the
    // onComplete callback. If the account has no Plex Home, only the
    // owner, or the master token is missing, onComplete is invoked
    // immediately with no UI shown — caller proceeds normally.
    void showHomeUserPicker(std::function<void()> onComplete);

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
    std::string m_masterAuthToken;
    std::string m_currentHomeUserUuid;
    std::string m_currentHomeUserTitle;
    std::string m_serverUrl;
    std::string m_username;
    AppSettings m_settings;
};

} // namespace vitaplex
