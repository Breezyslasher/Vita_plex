/**
 * VitaPlex - Application implementation
 */

#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "activity/login_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/player_activity.hpp"

#include <borealis.hpp>
#include <fstream>
#include <cstring>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

namespace vitaplex {

static const char* SETTINGS_PATH = "ux0:data/VitaPlex/settings.json";

Application& Application::getInstance() {
    static Application instance;
    return instance;
}

bool Application::init() {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    brls::Logger::info("VitaPlex {} initializing...", VITA_PLEX_VERSION);

#ifdef __vita__
    // Create data directory
    int ret = sceIoMkdir("ux0:data/VitaPlex", 0777);
    brls::Logger::debug("sceIoMkdir result: {:#x}", ret);
#endif

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

void Application::pushPlayerActivity(const std::string& mediaKey) {
    brls::Application::pushActivity(new PlayerActivity(mediaKey));
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
#ifdef __vita__
    brls::Logger::debug("loadSettings: Opening {}", SETTINGS_PATH);

    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        brls::Logger::debug("No settings file found (error: {:#x})", fd);
        return false;
    }

    // Get file size
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    brls::Logger::debug("loadSettings: File size = {}", size);

    if (size <= 0 || size > 16384) {
        brls::Logger::error("loadSettings: Invalid file size");
        sceIoClose(fd);
        return false;
    }

    std::string content;
    content.resize(size);
    sceIoRead(fd, &content[0], size);
    sceIoClose(fd);

    brls::Logger::debug("loadSettings: Read {} bytes", content.length());

    // Simple JSON parsing for strings (handles whitespace after colon)
    auto extractString = [&content](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        // Skip whitespace after colon
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
        // Expect opening quote
        if (pos >= content.length() || content[pos] != '"') return "";
        pos++; // Skip opening quote
        size_t end = content.find("\"", pos);
        if (end == std::string::npos) return "";
        return content.substr(pos, end - pos);
    };

    // Parse integers
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

    // Parse booleans
    auto extractBool = [&content](const std::string& key, bool defaultVal = false) -> bool {
        std::string search = "\"" + key + "\":";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return defaultVal;
        pos += search.length();
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
        return (content.substr(pos, 4) == "true");
    };

    // Load authentication
    m_authToken = extractString("authToken");
    m_serverUrl = extractString("serverUrl");
    m_username = extractString("username");

    brls::Logger::info("loadSettings: authToken={}, serverUrl={}, username={}",
                       m_authToken.empty() ? "(empty)" : "(set)",
                       m_serverUrl.empty() ? "(empty)" : m_serverUrl,
                       m_username.empty() ? "(empty)" : m_username);

    // Load UI settings
    m_settings.theme = static_cast<AppTheme>(extractInt("theme"));
    m_settings.showClock = extractBool("showClock", true);
    m_settings.animationsEnabled = extractBool("animationsEnabled", true);
    m_settings.debugLogging = extractBool("debugLogging", true);

    // Load layout settings
    m_settings.showLibrariesInSidebar = extractBool("showLibrariesInSidebar", false);
    m_settings.collapseSidebar = extractBool("collapseSidebar", false);
    m_settings.hiddenLibraries = extractString("hiddenLibraries");
    m_settings.sidebarOrder = extractString("sidebarOrder");

    // Load content display settings
    m_settings.showCollections = extractBool("showCollections", true);
    m_settings.showPlaylists = extractBool("showPlaylists", true);
    m_settings.showGenres = extractBool("showGenres", true);

    // Load playback settings
    m_settings.autoPlayNext = extractBool("autoPlayNext", true);
    m_settings.resumePlayback = extractBool("resumePlayback", true);
    m_settings.showSubtitles = extractBool("showSubtitles", true);
    m_settings.subtitleSize = static_cast<SubtitleSize>(extractInt("subtitleSize"));
    m_settings.seekInterval = extractInt("seekInterval");
    if (m_settings.seekInterval <= 0) m_settings.seekInterval = 10;

    // Load transcode settings
    m_settings.videoQuality = static_cast<VideoQuality>(extractInt("videoQuality"));
    m_settings.forceTranscode = extractBool("forceTranscode", false);
    m_settings.burnSubtitles = extractBool("burnSubtitles", true);
    m_settings.maxBitrate = extractInt("maxBitrate");
    if (m_settings.maxBitrate <= 0) m_settings.maxBitrate = 2000;

    // Load network settings
    m_settings.connectionTimeout = extractInt("connectionTimeout");
    if (m_settings.connectionTimeout <= 0) m_settings.connectionTimeout = 180; // 3 minutes default
    m_settings.directPlay = extractBool("directPlay", false);

    brls::Logger::info("Settings loaded successfully");
    return !m_authToken.empty();
#else
    return false;
#endif
}

bool Application::saveSettings() {
#ifdef __vita__
    brls::Logger::info("saveSettings: Saving to {}", SETTINGS_PATH);
    brls::Logger::debug("saveSettings: authToken={}, serverUrl={}, username={}",
                        m_authToken.empty() ? "(empty)" : "(set)",
                        m_serverUrl.empty() ? "(empty)" : m_serverUrl,
                        m_username.empty() ? "(empty)" : m_username);

    // Create JSON content
    std::string json = "{\n";

    // Authentication
    json += "  \"authToken\": \"" + m_authToken + "\",\n";
    json += "  \"serverUrl\": \"" + m_serverUrl + "\",\n";
    json += "  \"username\": \"" + m_username + "\",\n";

    // UI settings
    json += "  \"theme\": " + std::to_string(static_cast<int>(m_settings.theme)) + ",\n";
    json += "  \"showClock\": " + std::string(m_settings.showClock ? "true" : "false") + ",\n";
    json += "  \"animationsEnabled\": " + std::string(m_settings.animationsEnabled ? "true" : "false") + ",\n";
    json += "  \"debugLogging\": " + std::string(m_settings.debugLogging ? "true" : "false") + ",\n";

    // Layout settings
    json += "  \"showLibrariesInSidebar\": " + std::string(m_settings.showLibrariesInSidebar ? "true" : "false") + ",\n";
    json += "  \"collapseSidebar\": " + std::string(m_settings.collapseSidebar ? "true" : "false") + ",\n";
    json += "  \"hiddenLibraries\": \"" + m_settings.hiddenLibraries + "\",\n";
    json += "  \"sidebarOrder\": \"" + m_settings.sidebarOrder + "\",\n";

    // Content display settings
    json += "  \"showCollections\": " + std::string(m_settings.showCollections ? "true" : "false") + ",\n";
    json += "  \"showPlaylists\": " + std::string(m_settings.showPlaylists ? "true" : "false") + ",\n";
    json += "  \"showGenres\": " + std::string(m_settings.showGenres ? "true" : "false") + ",\n";

    // Playback settings
    json += "  \"autoPlayNext\": " + std::string(m_settings.autoPlayNext ? "true" : "false") + ",\n";
    json += "  \"resumePlayback\": " + std::string(m_settings.resumePlayback ? "true" : "false") + ",\n";
    json += "  \"showSubtitles\": " + std::string(m_settings.showSubtitles ? "true" : "false") + ",\n";
    json += "  \"subtitleSize\": " + std::to_string(static_cast<int>(m_settings.subtitleSize)) + ",\n";
    json += "  \"seekInterval\": " + std::to_string(m_settings.seekInterval) + ",\n";

    // Transcode settings
    json += "  \"videoQuality\": " + std::to_string(static_cast<int>(m_settings.videoQuality)) + ",\n";
    json += "  \"forceTranscode\": " + std::string(m_settings.forceTranscode ? "true" : "false") + ",\n";
    json += "  \"burnSubtitles\": " + std::string(m_settings.burnSubtitles ? "true" : "false") + ",\n";
    json += "  \"maxBitrate\": " + std::to_string(m_settings.maxBitrate) + ",\n";

    // Network settings
    json += "  \"connectionTimeout\": " + std::to_string(m_settings.connectionTimeout) + ",\n";
    json += "  \"directPlay\": " + std::string(m_settings.directPlay ? "true" : "false") + "\n";

    json += "}\n";

    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        brls::Logger::error("Failed to open settings file for writing: {:#x}", fd);
        return false;
    }

    int written = sceIoWrite(fd, json.c_str(), json.length());
    sceIoClose(fd);

    if (written == (int)json.length()) {
        brls::Logger::info("Settings saved successfully ({} bytes)", written);
        return true;
    } else {
        brls::Logger::error("Failed to write settings: only {} of {} bytes written", written, json.length());
        return false;
    }
#else
    return false;
#endif
}

} // namespace vitaplex
