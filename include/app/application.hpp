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

private:
    Application() = default;
    ~Application() = default;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool m_initialized = false;
    std::string m_authToken;
    std::string m_serverUrl;
    std::string m_username;
};

} // namespace vitaplex
