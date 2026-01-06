#pragma once

/**
 * VitaPlex - Plex Client for PlayStation Vita
 * Based on switchfin architecture (https://github.com/dragonflylee/switchfin)
 */

#include <string>
#include <vector>

#include <psp2/ctrl.h>
#include <vita2d.h>

// Application version
#define VITA_PLEX_VERSION "1.5.1"
#define VITA_PLEX_VERSION_NUM 151

// Screen dimensions (PS Vita)
#define SCREEN_WIDTH 960
#define SCREEN_HEIGHT 544

// Plex client identification
#define PLEX_CLIENT_ID "vita-plex-client-001"
#define PLEX_CLIENT_NAME "VitaPlex"
#define PLEX_CLIENT_VERSION VITA_PLEX_VERSION
#define PLEX_PLATFORM "PlayStation Vita"
#define PLEX_DEVICE "PS Vita"

namespace vitaplex {

// Application states
enum class AppState {
    INIT,
    LOGIN,
    PIN_AUTH,      // PIN/Link code authentication
    HOME,
    LIBRARY,
    BROWSE,        // Browsing library content
    SEARCH,        // Search screen
    MEDIA_DETAIL,
    PLAYER,
    PHOTO_VIEW,    // Photo viewing
    LIVE_TV,       // Live TV channels
    SETTINGS,
    ERROR,
    EXIT
};

// Login method
enum class LoginMethod {
    CREDENTIALS,   // Username/password
    PIN_CODE       // plex.tv/link PIN
};

// Media types
enum class MediaType {
    UNKNOWN,
    MOVIE,
    SHOW,
    SEASON,
    EPISODE,
    MUSIC_ARTIST,
    MUSIC_ALBUM,
    MUSIC_TRACK,
    PHOTO,
    LIVE_TV_CHANNEL,
    LIVE_TV_PROGRAM
};

// Live TV Channel info
struct LiveTVChannel {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string thumb;
    std::string callSign;
    int channelNumber = 0;
    std::string currentProgram;
    std::string nextProgram;
    int64_t programStart = 0;
    int64_t programEnd = 0;
};

// Navigation stack entry for hierarchical browsing
struct NavEntry {
    std::string key;
    std::string title;
    MediaType type;
    int selectedItem = 0;
    int scrollOffset = 0;
};

// Library section info
struct LibrarySection {
    std::string key;
    std::string title;
    std::string type;      // movie, show, artist, photo
    std::string art;
    std::string thumb;
    int count = 0;
};

// Media item info
struct MediaItem {
    std::string ratingKey;
    std::string key;           // For children navigation
    std::string title;
    std::string summary;
    std::string thumb;
    std::string art;
    std::string type;
    MediaType mediaType = MediaType::UNKNOWN;
    int year = 0;
    int duration = 0;      // in milliseconds
    int viewOffset = 0;    // resume position
    float rating = 0.0f;
    std::string contentRating;
    std::string studio;
    bool watched = false;
    
    // For episodes
    std::string grandparentTitle;  // Show name
    std::string parentTitle;       // Season name
    int parentIndex = 0;           // Season number
    int index = 0;                 // Episode/track number
    int seasonNumber = 0;
    int episodeNumber = 0;
    
    // For seasons/albums
    int leafCount = 0;             // Number of children (episodes/tracks)
    int viewedLeafCount = 0;       // Number watched
    
    // Stream info
    std::string streamUrl;
    std::string videoCodec;
    std::string audioCodec;
    int videoWidth = 0;
    int videoHeight = 0;
    
    // Cached texture (loaded async)
    vita2d_texture* thumbTexture = nullptr;
};

// Plex server info
struct PlexServer {
    std::string name;
    std::string address;
    int port = 32400;
    std::string machineIdentifier;
    std::string accessToken;
};

// PIN authentication info
struct PinAuth {
    int id = 0;
    std::string code;
    std::string authToken;
    bool expired = false;
    int expiresIn = 0;
    bool useJwt = false;  // Whether this PIN uses JWT authentication
};

// Hub (for home screen)
struct Hub {
    std::string title;
    std::string type;
    std::string hubIdentifier;
    std::string key;
    std::vector<MediaItem> items;
    bool more = false;
};

// Video quality setting
enum class VideoQuality {
    ORIGINAL,
    QUALITY_1080P,
    QUALITY_720P,
    QUALITY_480P,
    QUALITY_360P
};

// Application settings
struct AppSettings {
    // Video settings
    VideoQuality videoQuality = VideoQuality::QUALITY_720P;
    bool autoPlay = true;
    bool showSubtitles = true;
    
    // Debug settings
    bool enableFileLogging = false;  // Log to file for debugging
    
    // User info
    std::string username;
    std::string email;
    std::string avatarUrl;
    
    // Server settings  
    std::string lastServerUrl;
    bool rememberLogin = true;
    
    // Saved credentials (encrypted in future)
    std::string savedAuthToken;
    std::string savedServerUrl;
    std::string savedServerName;
};

// Debug logging functions
void initDebugLog();
void closeDebugLog();
void debugLog(const char* format, ...);
void setDebugLogEnabled(bool enabled);

/**
 * Main application class
 */
class App {
public:
    static App& getInstance();
    
    bool init();
    void run();
    void shutdown();
    
    // State management
    void setState(AppState state);
    AppState getState() const { return m_state; }
    
    // Authentication methods
    bool login(const std::string& username, const std::string& password);
    bool requestPin();                    // Request new PIN for plex.tv/link
    bool checkPin();                      // Check if PIN has been authorized
    bool connectToServer(const std::string& url);
    void logout();
    bool isLoggedIn() const { return !m_authToken.empty(); }
    std::string getAuthToken() const { return m_authToken; }
    const PlexServer& getCurrentServer() const { return m_currentServer; }
    const PinAuth& getPinAuth() const { return m_pinAuth; }
    
    // Library operations
    bool fetchLibrarySections();
    bool fetchLibraryContent(const std::string& sectionKey);
    bool fetchChildren(const std::string& ratingKey);  // Get seasons/episodes/albums/tracks
    bool fetchMediaDetails(const std::string& ratingKey);
    bool fetchHubs();                     // Get home screen hubs
    bool fetchContinueWatching();
    bool fetchRecentlyAdded();
    
    // Live TV operations
    bool fetchLiveTVChannels();
    bool fetchLiveTVGuide(int hoursAhead = 4);
    bool startLiveTVPlayback(const std::string& channelKey);
    const std::vector<LiveTVChannel>& getLiveTVChannels() const { return m_liveTVChannels; }
    bool hasLiveTV() const { return m_hasLiveTV; }
    void parseChannelsFromResponse(const std::string& body);
    
    // DVR operations
    bool fetchDVRRecordings();
    bool scheduleDVRRecording(const std::string& programKey);
    bool cancelDVRRecording(const std::string& recordingKey);
    
    // Navigation stack
    void pushNavigation(const std::string& key, const std::string& title, MediaType type);
    void popNavigation();
    bool canGoBack() const { return !m_navStack.empty(); }
    
    // Image loading
    bool loadThumbnail(MediaItem& item, int width = 150, int height = 225);
    void loadVisibleThumbnails();  // Load thumbs for visible items
    void clearThumbnails();        // Free all loaded textures
    
    // Search
    bool search(const std::string& query);
    
    // Playback
    bool getPlaybackUrl(const std::string& ratingKey);
    bool updatePlayProgress(const std::string& ratingKey, int timeMs);
    bool markAsWatched(const std::string& ratingKey);
    bool markAsUnwatched(const std::string& ratingKey);
    
    // Getters for UI
    const std::vector<LibrarySection>& getLibrarySections() const { return m_librarySections; }
    const std::vector<MediaItem>& getMediaItems() const { return m_mediaItems; }
    const std::vector<MediaItem>& getSearchResults() const { return m_searchResults; }
    const std::vector<Hub>& getHubs() const { return m_hubs; }
    const MediaItem& getCurrentMedia() const { return m_currentMedia; }
    const AppSettings& getSettings() const { return m_settings; }
    
    // Settings
    void setVideoQuality(VideoQuality quality) { m_settings.videoQuality = quality; }
    void setAutoPlay(bool enabled) { m_settings.autoPlay = enabled; }
    void setShowSubtitles(bool enabled) { m_settings.showSubtitles = enabled; }
    
    // Persistence - save/load settings and login
    bool saveSettings();
    bool loadSettings();
    bool hasSavedLogin() const { return !m_settings.savedAuthToken.empty(); }
    bool restoreSavedLogin();
    
    // Video playback
    bool startPlayback(bool resume = false);
    void stopPlayback();
    bool showPhoto();  // Photo viewing
    
    // Error handling
    void setError(const std::string& message);
    std::string getLastError() const { return m_lastError; }
    
private:
    App() = default;
    ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    
    // Input handling
    void handleLoginInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handlePinAuthInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleHomeInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleLibraryInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleBrowseInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleSearchInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleMediaDetailInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleSettingsInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handlePlayerInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handleLiveTVInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    void handlePhotoViewInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl);
    
    // Drawing
    void drawLoginScreen(vita2d_pgf* font);
    void drawPinAuthScreen(vita2d_pgf* font);
    void drawHomeScreen(vita2d_pgf* font);
    void drawLibraryScreen(vita2d_pgf* font);
    void drawBrowseScreen(vita2d_pgf* font);
    void drawSearchScreen(vita2d_pgf* font);
    void drawMediaDetailScreen(vita2d_pgf* font);
    void drawSettingsScreen(vita2d_pgf* font);
    void drawPlayerScreen(vita2d_pgf* font);
    void drawLiveTVScreen(vita2d_pgf* font);
    void drawPhotoViewScreen(vita2d_pgf* font);
    
    // Helper to build API URL
    std::string buildApiUrl(const std::string& endpoint);
    
    // JSON parsing helpers
    MediaType parseMediaType(const std::string& typeStr);
    MediaItem parseMediaItemFromJson(const std::string& json, size_t& pos);
    
    bool m_running = false;
    AppState m_state = AppState::INIT;
    LoginMethod m_loginMethod = LoginMethod::CREDENTIALS;
    std::string m_lastError;
    std::string m_authToken;
    PlexServer m_currentServer;
    PinAuth m_pinAuth;
    
    // Data
    std::vector<LibrarySection> m_librarySections;
    std::vector<MediaItem> m_mediaItems;
    std::vector<MediaItem> m_searchResults;
    std::vector<MediaItem> m_continueWatching;
    std::vector<Hub> m_hubs;
    std::vector<NavEntry> m_navStack;  // Navigation history
    std::vector<LiveTVChannel> m_liveTVChannels;
    MediaItem m_currentMedia;
    std::string m_currentSectionKey;
    std::string m_searchQuery;
    AppSettings m_settings;
    bool m_hasLiveTV = false;
    
    // Player state
    bool m_isPlaying = false;
    uint64_t m_playPosition = 0;
    
    // UI state
    int m_selectedLibrary = 0;
    int m_selectedItem = 0;
    int m_scrollOffset = 0;
    int m_hubIndex = 0;
    int m_hubItemIndex = 0;
};

} // namespace vitaplex
