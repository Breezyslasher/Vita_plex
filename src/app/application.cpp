/**
 * VitaPlex - Application implementation
 */

#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "activity/login_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/player_activity.hpp"

#include <borealis.hpp>
#include <fstream>
#include <cstring>
#include <filesystem>
#include "platform/paths.hpp"
#include "platform/platform.hpp"

namespace vitaplex {

Application& Application::getInstance() {
    static Application instance;
    return instance;
}

bool Application::init() {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    brls::Logger::info("VitaPlex {} initializing...", VITA_PLEX_VERSION);

    // Ensure the platform data root exists (settings/downloads/keys). This
    // used to be a sceIoMkdir on Vita and a std::filesystem::create_directories
    // everywhere else — both end up in the same newlib file descriptor layer
    // on Vita, so the std::filesystem path is portable across every target.
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(platformPath("settings.json")).parent_path(), ec);
    }

    // Seed platform-specific defaults BEFORE loading the settings file, so
    // the loader only overrides them if the user has previously saved values.
    const auto& vc = platform::getVideoConstraints();
    m_settings.maxBitrate = vc.defaultBitrate;
    m_settings.videoQuality = static_cast<VideoQuality>(vc.defaultVideoQualityIndex);

    // Load saved settings
    brls::Logger::info("Loading saved settings...");
    bool loaded = loadSettings();
    brls::Logger::info("Settings load result: {}", loaded ? "success" : "failed/not found");

    // Apply settings
    applyTheme();
    applyLogLevel();

    m_initialized = true;
    return true;
}

void Application::run() {
    brls::Logger::info("Application::run - isLoggedIn={}, serverUrl={}",
                       isLoggedIn(), m_serverUrl.empty() ? "(empty)" : m_serverUrl);

    // Check if we have saved login credentials
    if (isLoggedIn() && !m_serverUrl.empty()) {
        brls::Logger::info("Restoring saved session...");
        // Verify connection and go to main
        PlexClient::getInstance().setAuthToken(m_authToken);
        // Use connectToServer to properly initialize (including Live TV check)
        if (PlexClient::getInstance().connectToServer(m_serverUrl)) {
            brls::Logger::info("Restored session and connected to server");
            // Bidirectional sync: push local offline progress, pull server progress
            DownloadsManager::getInstance().init();
            DownloadsManager::getInstance().syncProgressBidirectional();
            pushMainActivity();
        } else {
            brls::Logger::error("Failed to connect to saved server, showing login");
            pushLoginActivity();
        }
    } else {
        brls::Logger::info("No saved session, showing login screen");
        // Show login screen
        pushLoginActivity();
    }

    // Main loop handled by Borealis
    while (brls::Application::mainLoop()) {
        // Application keeps running
    }
}

void Application::shutdown() {
    saveSettings();
    m_initialized = false;
    brls::Logger::info("VitaPlex shutting down");
}

void Application::pushLoginActivity() {
    brls::Application::pushActivity(new LoginActivity());
}

void Application::pushMainActivity() {
    brls::Application::pushActivity(new MainActivity());
}

void Application::pushPlayerActivity(const std::string& mediaKey, bool isLocalFile) {
    brls::Application::pushActivity(new PlayerActivity(mediaKey, isLocalFile));
}

void Application::pushLiveTVPlayerActivity(const std::string& streamUrl, const std::string& channelTitle) {
    brls::Application::pushActivity(PlayerActivity::createForStream(streamUrl, channelTitle));
}

void Application::applyTheme() {
    brls::ThemeVariant variant;

    switch (m_settings.theme) {
        case AppTheme::LIGHT:
            variant = brls::ThemeVariant::LIGHT;
            break;
        case AppTheme::DARK:
            variant = brls::ThemeVariant::DARK;
            break;
        case AppTheme::SYSTEM:
        default:
            // Default to dark for Vita
            variant = brls::ThemeVariant::DARK;
            break;
    }

    brls::Application::getPlatform()->setThemeVariant(variant);
    brls::Logger::info("Applied theme: {}", getThemeString(m_settings.theme));
}

void Application::applyLogLevel() {
    if (m_settings.debugLogging) {
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        brls::Logger::info("Debug logging enabled");
    } else {
        brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
        brls::Logger::info("Debug logging disabled");
    }
}

std::string Application::getQualityString(VideoQuality quality) {
    switch (quality) {
        case VideoQuality::ORIGINAL: return "Original (Direct Play)";
        case VideoQuality::QUALITY_1080P: return "1080p (20 Mbps)";
        case VideoQuality::QUALITY_720P: return "720p (4 Mbps)";
        case VideoQuality::QUALITY_480P: return "480p (2 Mbps)";
        case VideoQuality::QUALITY_360P: return "360p (1 Mbps)";
        case VideoQuality::QUALITY_240P: return "240p (500 Kbps)";
        default: return "Unknown";
    }
}

std::string Application::getThemeString(AppTheme theme) {
    switch (theme) {
        case AppTheme::SYSTEM: return "System";
        case AppTheme::LIGHT: return "Light";
        case AppTheme::DARK: return "Dark";
        default: return "Unknown";
    }
}

std::string Application::getSubtitleSizeString(SubtitleSize size) {
    switch (size) {
        case SubtitleSize::SMALL: return "Small";
        case SubtitleSize::MEDIUM: return "Medium";
        case SubtitleSize::LARGE: return "Large";
        default: return "Unknown";
    }
}

bool Application::loadSettings() {
    // Single unified path: std::ifstream works on every target (on Vita it
    // goes through newlib's POSIX shim into sceIoOpen). This replaces the
    // old #ifdef __vita__ duplicate of the loader that called sceIoOpen
    // directly and reached for a hard-coded "ux0:data/VitaPlex/settings.json".
    const std::string settingsPath = platformPath("settings.json");
    brls::Logger::debug("loadSettings: Opening {}", settingsPath);

    std::ifstream ifs(settingsPath);
    if (!ifs.is_open()) {
        brls::Logger::debug("No settings file found");
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();
    if (content.empty()) {
        brls::Logger::debug("Settings file is empty");
        return false;
    }

    // Simple JSON parsing for strings (handles whitespace after colon)
    auto extractString = [&content](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
        if (pos >= content.length() || content[pos] != '"') return "";
        pos++;
        size_t end = content.find("\"", pos);
        if (end == std::string::npos) return "";
        return content.substr(pos, end - pos);
    };

    auto extractInt = [&content](const std::string& key) -> int {
        std::string search = "\"" + key + "\":";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return 0;
        pos += search.length();
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
        size_t end = content.find_first_of(",}\n", pos);
        if (end == std::string::npos) return 0;
        return atoi(content.substr(pos, end - pos).c_str());
    };

    auto extractBool = [&content](const std::string& key, bool defaultVal = false) -> bool {
        std::string search = "\"" + key + "\":";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return defaultVal;
        pos += search.length();
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
        return (content.substr(pos, 4) == "true");
    };

    // Authentication
    m_authToken = extractString("authToken");
    m_serverUrl = extractString("serverUrl");
    m_username  = extractString("username");

    brls::Logger::info("loadSettings: authToken={}, serverUrl={}, username={}",
                       m_authToken.empty() ? "(empty)" : "(set)",
                       m_serverUrl.empty() ? "(empty)" : m_serverUrl,
                       m_username.empty()  ? "(empty)" : m_username);

    // UI settings
    m_settings.theme = static_cast<AppTheme>(extractInt("theme"));
    m_settings.debugLogging = extractBool("debugLogging", true);
    m_settings.showDebugTab = extractBool("showDebugTab", false);

    // Layout settings
    m_settings.showLibrariesInSidebar = extractBool("showLibrariesInSidebar", false);
    m_settings.collapseSidebar = extractBool("collapseSidebar", false);
    m_settings.hiddenLibraries = extractString("hiddenLibraries");
    m_settings.sidebarOrder    = extractString("sidebarOrder");

    // Content display settings
    m_settings.showCollections  = extractBool("showCollections", true);
    m_settings.showPlaylists    = extractBool("showPlaylists", true);
    m_settings.showGenres       = extractBool("showGenres", true);
    m_settings.hideTitlesInGrid = extractBool("hideTitlesInGrid", false);
    m_settings.skipSingleSeason = extractBool("skipSingleSeason", false);

    // Playback settings
    m_settings.autoPlayNext    = extractBool("autoPlayNext", true);
    m_settings.resumePlayback  = extractBool("resumePlayback", true);
    m_settings.showSubtitles   = extractBool("showSubtitles", true);
    m_settings.subtitleSize    = static_cast<SubtitleSize>(extractInt("subtitleSize"));
    m_settings.seekInterval    = extractInt("seekInterval");
    if (m_settings.seekInterval <= 0) m_settings.seekInterval = 10;
    m_settings.controlsAutoHideSeconds = extractInt("controlsAutoHideSeconds");
    if (m_settings.controlsAutoHideSeconds < 0) m_settings.controlsAutoHideSeconds = 5;
    m_settings.autoSkipIntro   = extractBool("autoSkipIntro", false);
    m_settings.autoSkipCredits = extractBool("autoSkipCredits", false);

    // Transcode settings. If the setting isn't present in the JSON, keep
    // the platform defaults Application::init() seeded earlier.
    {
        const auto& vc = platform::getVideoConstraints();
        int vq = extractInt("videoQuality");
        if (vq > 0) {
            m_settings.videoQuality = static_cast<VideoQuality>(vq);
        }
        m_settings.forceTranscode = extractBool("forceTranscode", false);
        int mb = extractInt("maxBitrate");
        if (mb > 0) {
            m_settings.maxBitrate = mb;
        } else {
            m_settings.maxBitrate = vc.defaultBitrate;
        }
    }

    // Network settings
    m_settings.connectionTimeout = extractInt("connectionTimeout");
    if (m_settings.connectionTimeout <= 0) m_settings.connectionTimeout = 180;
    m_settings.directPlay = extractBool("directPlay", false);

    // Download settings
    m_settings.deleteAfterWatch = extractBool("deleteAfterWatch", false);

    // Music settings
    int trackAction = extractInt("trackDefaultAction");
    if (trackAction >= 0 && trackAction <= 4) {
        m_settings.trackDefaultAction = static_cast<TrackDefaultAction>(trackAction);
    }
    m_settings.backgroundMusic = extractBool("backgroundMusic", true);

    brls::Logger::info("Settings loaded successfully from {}", settingsPath);
    return !m_authToken.empty();
}

bool Application::saveSettings() {
    // Single unified path: std::ofstream works on every target. Vita's
    // newlib shim forwards to sceIoOpen under the hood, so this removes
    // the old #ifdef __vita__ sceIoWrite duplicate.
    const std::string settingsPath = platformPath("settings.json");
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(settingsPath).parent_path(), ec);

    brls::Logger::debug("saveSettings: authToken={}, serverUrl={}, username={}",
                        m_authToken.empty() ? "(empty)" : "(set)",
                        m_serverUrl.empty() ? "(empty)" : m_serverUrl,
                        m_username.empty()  ? "(empty)" : m_username);

    auto b = [](bool v) { return std::string(v ? "true" : "false"); };

    std::string json = "{\n";
    json += "  \"authToken\": \"" + m_authToken + "\",\n";
    json += "  \"serverUrl\": \"" + m_serverUrl + "\",\n";
    json += "  \"username\": \"" + m_username + "\",\n";
    json += "  \"theme\": " + std::to_string(static_cast<int>(m_settings.theme)) + ",\n";
    json += "  \"debugLogging\": " + b(m_settings.debugLogging) + ",\n";
    json += "  \"showDebugTab\": " + b(m_settings.showDebugTab) + ",\n";
    json += "  \"showLibrariesInSidebar\": " + b(m_settings.showLibrariesInSidebar) + ",\n";
    json += "  \"collapseSidebar\": " + b(m_settings.collapseSidebar) + ",\n";
    json += "  \"hiddenLibraries\": \"" + m_settings.hiddenLibraries + "\",\n";
    json += "  \"sidebarOrder\": \"" + m_settings.sidebarOrder + "\",\n";
    json += "  \"showCollections\": " + b(m_settings.showCollections) + ",\n";
    json += "  \"showPlaylists\": " + b(m_settings.showPlaylists) + ",\n";
    json += "  \"showGenres\": " + b(m_settings.showGenres) + ",\n";
    json += "  \"hideTitlesInGrid\": " + b(m_settings.hideTitlesInGrid) + ",\n";
    json += "  \"skipSingleSeason\": " + b(m_settings.skipSingleSeason) + ",\n";
    json += "  \"autoPlayNext\": " + b(m_settings.autoPlayNext) + ",\n";
    json += "  \"resumePlayback\": " + b(m_settings.resumePlayback) + ",\n";
    json += "  \"showSubtitles\": " + b(m_settings.showSubtitles) + ",\n";
    json += "  \"subtitleSize\": " + std::to_string(static_cast<int>(m_settings.subtitleSize)) + ",\n";
    json += "  \"seekInterval\": " + std::to_string(m_settings.seekInterval) + ",\n";
    json += "  \"controlsAutoHideSeconds\": " + std::to_string(m_settings.controlsAutoHideSeconds) + ",\n";
    json += "  \"autoSkipIntro\": " + b(m_settings.autoSkipIntro) + ",\n";
    json += "  \"autoSkipCredits\": " + b(m_settings.autoSkipCredits) + ",\n";
    json += "  \"videoQuality\": " + std::to_string(static_cast<int>(m_settings.videoQuality)) + ",\n";
    json += "  \"forceTranscode\": " + b(m_settings.forceTranscode) + ",\n";
    json += "  \"maxBitrate\": " + std::to_string(m_settings.maxBitrate) + ",\n";
    json += "  \"connectionTimeout\": " + std::to_string(m_settings.connectionTimeout) + ",\n";
    json += "  \"directPlay\": " + b(m_settings.directPlay) + ",\n";
    json += "  \"deleteAfterWatch\": " + b(m_settings.deleteAfterWatch) + ",\n";
    json += "  \"trackDefaultAction\": " + std::to_string(static_cast<int>(m_settings.trackDefaultAction)) + ",\n";
    json += "  \"backgroundMusic\": " + b(m_settings.backgroundMusic) + "\n";
    json += "}\n";

    std::ofstream ofs(settingsPath, std::ios::trunc);
    if (!ofs.is_open()) {
        brls::Logger::error("Failed to open settings for write: {}", settingsPath);
        return false;
    }
    ofs << json;
    ofs.close();
    brls::Logger::info("Settings saved to {} ({} bytes)", settingsPath, json.length());
    return true;
}

} // namespace vitaplex
