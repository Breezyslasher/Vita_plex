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
    sceIoMkdir("ux0:data/VitaPlex", 0777);
#endif

    // Load saved settings
    loadSettings();

    m_initialized = true;
    return true;
}

void Application::run() {
    // Check if we have saved login credentials
    if (isLoggedIn() && !m_serverUrl.empty()) {
        // Verify connection and go to main
        PlexClient::getInstance().setAuthToken(m_authToken);
        PlexClient::getInstance().setServerUrl(m_serverUrl);
        pushMainActivity();
    } else {
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

bool Application::loadSettings() {
#ifdef __vita__
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        brls::Logger::debug("No settings file found");
        return false;
    }

    // Get file size
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    if (size <= 0 || size > 8192) {
        sceIoClose(fd);
        return false;
    }

    std::string content;
    content.resize(size);
    sceIoRead(fd, &content[0], size);
    sceIoClose(fd);

    // Simple JSON parsing
    auto extractValue = [&content](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":\"";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        size_t end = content.find("\"", pos);
        if (end == std::string::npos) return "";
        return content.substr(pos, end - pos);
    };

    m_authToken = extractValue("authToken");
    m_serverUrl = extractValue("serverUrl");
    m_username = extractValue("username");

    brls::Logger::info("Loaded settings: user={}, server={}", m_username, m_serverUrl);
    return !m_authToken.empty();
#else
    return false;
#endif
}

bool Application::saveSettings() {
#ifdef __vita__
    // Create JSON content
    std::string json = "{\n";
    json += "  \"authToken\": \"" + m_authToken + "\",\n";
    json += "  \"serverUrl\": \"" + m_serverUrl + "\",\n";
    json += "  \"username\": \"" + m_username + "\"\n";
    json += "}\n";

    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        brls::Logger::error("Failed to save settings");
        return false;
    }

    sceIoWrite(fd, json.c_str(), json.length());
    sceIoClose(fd);

    brls::Logger::info("Settings saved");
    return true;
#else
    return false;
#endif
}

} // namespace vitaplex
