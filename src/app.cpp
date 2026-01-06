/**
 * VitaPlex - Plex Client for PlayStation Vita
 * Main application implementation with PIN auth, library browsing, search, and playback
 */

#include "app.h"
#include "utils/http_client.h"
#include "player/mpv_player.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/apputil.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <cstdarg>

namespace vitaplex {

// ============================================================================
// Debug Logging System
// ============================================================================

static SceUID s_logFile = -1;
static bool s_loggingEnabled = false;
static const char* LOG_PATH = "ux0:data/VitaPlex/debug.log";

void initDebugLog() {
    if (s_loggingEnabled && s_logFile < 0) {
        // Ensure directory exists
        sceIoMkdir("ux0:data/VitaPlex", 0777);
        
        // Open log file (append mode)
        s_logFile = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (s_logFile >= 0) {
            // Write header
            SceDateTime time;
            sceRtcGetCurrentClockLocalTime(&time);
            char header[256];
            snprintf(header, sizeof(header), 
                "\n\n========== VitaPlex Debug Log ==========\n"
                "Started: %04d-%02d-%02d %02d:%02d:%02d\n"
                "=========================================\n\n",
                time.year, time.month, time.day, 
                time.hour, time.minute, time.second);
            sceIoWrite(s_logFile, header, strlen(header));
            sceClibPrintf("Debug logging enabled: %s\n", LOG_PATH);
        } else {
            sceClibPrintf("Failed to open log file: %s (error: 0x%08X)\n", LOG_PATH, s_logFile);
        }
    }
}

void closeDebugLog() {
    if (s_logFile >= 0) {
        debugLog("=== Log closed ===\n");
        sceIoClose(s_logFile);
        s_logFile = -1;
    }
}

void setDebugLogEnabled(bool enabled) {
    s_loggingEnabled = enabled;
    if (enabled) {
        initDebugLog();
    } else {
        closeDebugLog();
    }
}

void debugLog(const char* format, ...) {
    char buffer[1024];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Always print to console
    sceClibPrintf("%s", buffer);

    if (s_loggingEnabled && s_logFile >= 0) {
        // Timestamp
        SceDateTime time;
        sceRtcGetCurrentClockLocalTime(&time);

        char timestamped[1280];
        snprintf(timestamped, sizeof(timestamped),
            "[%02d:%02d:%02d.%03d] %s",
            time.hour,
            time.minute,
            time.second,
            time.microsecond / 1000,
            buffer
        );

        // Write log
        sceIoWrite(s_logFile, timestamped, strlen(timestamped));

        // FORCE flush (Vita-safe way)
        sceIoClose(s_logFile);
        s_logFile = sceIoOpen(
            LOG_PATH,
            SCE_O_WRONLY | SCE_O_APPEND | SCE_O_CREAT,
            0777
        );
    }
}

// Colors
static const unsigned int COLOR_WHITE = RGBA8(255, 255, 255, 255);
static const unsigned int COLOR_BLACK = RGBA8(0, 0, 0, 255);
static const unsigned int COLOR_GRAY = RGBA8(128, 128, 128, 255);
static const unsigned int COLOR_DARK_GRAY = RGBA8(60, 60, 60, 255);
static const unsigned int COLOR_ORANGE = RGBA8(229, 160, 13, 255);
static const unsigned int COLOR_DARK_BG = RGBA8(30, 30, 30, 255);
static const unsigned int COLOR_CARD_BG = RGBA8(45, 45, 45, 255);
static const unsigned int COLOR_SELECTED = RGBA8(229, 160, 13, 100);
static const unsigned int COLOR_ERROR = RGBA8(255, 80, 80, 255);
static const unsigned int COLOR_SUCCESS = RGBA8(80, 255, 80, 255);

// IME Dialog state
static bool s_imeDialogRunning = false;
static SceUInt16 s_imeInputText[256];
static SceUInt16 s_imeTitleText[128];  // Static buffer for title (must persist)
static char s_imeResult[256];
static int s_imeTargetField = -1;  // Which field the IME is editing

// Login screen state
static int s_selectedField = 0;  // 0=server, 1=username, 2=password, 3=login btn, 4=pin btn
static char s_serverUrl[256] = "";
static char s_username[128] = "";
static char s_password[128] = "";

// PIN auth state
static int s_pinCheckCounter = 0;

// Search state
static char s_searchText[256] = "";

// Helper: Extract JSON value
static std::string extractJsonValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    
    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";
    
    size_t valueStart = json.find_first_not_of(" \t\n\r", colonPos + 1);
    if (valueStart == std::string::npos) return "";
    
    if (json[valueStart] == '"') {
        size_t valueEnd = json.find('"', valueStart + 1);
        if (valueEnd == std::string::npos) return "";
        return json.substr(valueStart + 1, valueEnd - valueStart - 1);
    } else if (json[valueStart] == 'n' && json.substr(valueStart, 4) == "null") {
        return "";
    } else {
        size_t valueEnd = json.find_first_of(",}]", valueStart);
        if (valueEnd == std::string::npos) return "";
        std::string value = json.substr(valueStart, valueEnd - valueStart);
        // Trim whitespace
        while (!value.empty() && (value.back() == ' ' || value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }
        return value;
    }
}

// Helper: Extract JSON int
static int extractJsonInt(const std::string& json, const std::string& key) {
    std::string value = extractJsonValue(json, key);
    if (value.empty()) return 0;
    return atoi(value.c_str());
}

// Helper: Extract JSON float
static float extractJsonFloat(const std::string& json, const std::string& key) {
    std::string value = extractJsonValue(json, key);
    if (value.empty()) return 0.0f;
    return (float)atof(value.c_str());
}

// Helper: Extract JSON bool
static bool extractJsonBool(const std::string& json, const std::string& key) {
    std::string value = extractJsonValue(json, key);
    return (value == "true" || value == "1");
}

// UTF-8 to UTF-16 conversion
static void utf8ToUtf16(const char* utf8, SceUInt16* utf16, size_t maxLen) {
    size_t i = 0, j = 0;
    while (utf8[i] && j < maxLen - 1) {
        unsigned char c = utf8[i];
        if (c < 0x80) {
            utf16[j++] = c;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            utf16[j++] = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            utf16[j++] = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F);
            i += 3;
        } else {
            i++;
        }
    }
    utf16[j] = 0;
}

// UTF-16 to UTF-8 conversion
static void utf16ToUtf8(const SceUInt16* utf16, char* utf8, size_t maxLen) {
    size_t i = 0, j = 0;
    while (utf16[i] && j < maxLen - 1) {
        SceUInt16 c = utf16[i++];
        if (c < 0x80) {
            utf8[j++] = (char)c;
        } else if (c < 0x800) {
            if (j + 2 > maxLen - 1) break;
            utf8[j++] = 0xC0 | (c >> 6);
            utf8[j++] = 0x80 | (c & 0x3F);
        } else {
            if (j + 3 > maxLen - 1) break;
            utf8[j++] = 0xE0 | (c >> 12);
            utf8[j++] = 0x80 | ((c >> 6) & 0x3F);
            utf8[j++] = 0x80 | (c & 0x3F);
        }
    }
    utf8[j] = 0;
}

// Initialize IME dialog
static void initImeDialog(const char* title, const char* initialText, int maxLen, bool isPassword = false) {
    if (s_imeDialogRunning) {
        debugLog("IME: Dialog already running\n");
        return;
    }
    
    debugLog("IME: Starting dialog for '%s'\n", title);
    
    // Clear buffers first
    memset(s_imeInputText, 0, sizeof(s_imeInputText));
    memset(s_imeTitleText, 0, sizeof(s_imeTitleText));
    memset(s_imeResult, 0, sizeof(s_imeResult));
    
    // Convert title to UTF-16 (use static buffer)
    utf8ToUtf16(title, s_imeTitleText, 128);
    
    // Convert initial text to UTF-16
    if (initialText && initialText[0]) {
        utf8ToUtf16(initialText, s_imeInputText, 256);
    }
    
    // Initialize IME parameters
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    
    // Basic settings
    param.supportedLanguages = 0;  // 0 = all languages
    param.languagesForced = SCE_FALSE;
    param.type = isPassword ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
    param.option = 0;
    if (isPassword) {
        param.option |= SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    }
    
    // Text settings - use static buffers
    param.title = s_imeTitleText;
    param.maxTextLength = maxLen;
    param.inputTextBuffer = s_imeInputText;
    param.initialText = s_imeInputText;
    
    // Dialog settings
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;
    param.inputMethod = 0;  // Default input method
    
    // Initialize the dialog
    int ret = sceImeDialogInit(&param);
    if (ret < 0) {
        debugLog("IME: sceImeDialogInit failed: 0x%08X\n", ret);
        
        // Common error codes:
        // 0x80100701 = SCE_COMMON_DIALOG_ERROR_NOT_RUNNING
        // 0x80100702 = SCE_COMMON_DIALOG_ERROR_ALREADY_RUNNING
        // 0x80100703 = SCE_COMMON_DIALOG_ERROR_PARAM
        // 0x80100704 = SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED
        // 0x80100705 = SCE_COMMON_DIALOG_ERROR_NOT_INIT
        
        if (ret == (int)0x80100705) {
            debugLog("IME: Common dialog not initialized, trying to reinitialize...\n");
            // Try to initialize common dialog
            SceCommonDialogConfigParam configParam;
            sceCommonDialogConfigParamInit(&configParam);
            sceCommonDialogSetConfigParam(&configParam);
            
            // Try again
            ret = sceImeDialogInit(&param);
            if (ret < 0) {
                debugLog("IME: Retry failed: 0x%08X\n", ret);
                return;
            }
        } else {
            return;
        }
    }
    
    s_imeDialogRunning = true;
    debugLog("IME: Dialog started successfully\n");
}

// Update IME dialog, returns: 0=running, 1=finished, -1=cancelled
static int updateImeDialog() {
    if (!s_imeDialogRunning) return -1;
    
    SceCommonDialogStatus status = sceImeDialogGetStatus();
    
    if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult result;
        memset(&result, 0, sizeof(result));
        sceImeDialogGetResult(&result);
        
        debugLog("IME: Dialog finished, button=%d\n", result.button);
        
        if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            // Convert result back to UTF-8
            utf16ToUtf8(s_imeInputText, s_imeResult, 256);
            debugLog("IME: Result text: %s\n", s_imeResult);
            sceImeDialogTerm();
            s_imeDialogRunning = false;
            return 1;
        } else {
            // Cancelled
            debugLog("IME: Dialog cancelled\n");
            sceImeDialogTerm();
            s_imeDialogRunning = false;
            return -1;
        }
    } else if (status == SCE_COMMON_DIALOG_STATUS_NONE) {
        // Dialog was terminated unexpectedly
        debugLog("IME: Dialog terminated unexpectedly\n");
        s_imeDialogRunning = false;
        return -1;
    }
    
    // Still running
    return 0;
}

// Singleton instance
App& App::getInstance() {
    static App instance;
    return instance;
}

bool App::init() {
    debugLog("VitaPlex App initializing...\n");
    
    // Create data directory
    sceIoMkdir("ux0:data/VitaPlex", 0777);
    
    // Load saved settings
    loadSettings();
    
    // Check if we have saved login
    if (hasSavedLogin()) {
        debugLog("Found saved login, attempting to restore...\n");
        if (restoreSavedLogin()) {
            debugLog("Restored login successfully!\n");
            // Go directly to home if restore successful
            m_state = AppState::HOME;
            fetchLibrarySections();
            fetchHubs();
        } else {
            debugLog("Failed to restore login, showing login screen\n");
            m_state = AppState::LOGIN;
        }
    } else {
        m_state = AppState::LOGIN;
    }
    
    m_running = true;
    return true;
}

void App::setState(AppState state) {
    m_state = state;
    // Reset UI state when changing screens
    m_selectedItem = 0;
    m_scrollOffset = 0;
}

void App::setError(const std::string& message) {
    m_lastError = message;
    debugLog("Error: %s\n", message.c_str());
}

std::string App::buildApiUrl(const std::string& endpoint) {
    // m_currentServer.address already contains the full URL with port
    // e.g., "http://192.168.1.28:32400"
    std::string url = m_currentServer.address;
    
    // Remove trailing slash if present
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    
    url += endpoint;
    
    // Add token
    if (!m_authToken.empty()) {
        if (endpoint.find('?') != std::string::npos) {
            url += "&X-Plex-Token=" + m_authToken;
        } else {
            url += "?X-Plex-Token=" + m_authToken;
        }
    }
    
    return url;
}

MediaType App::parseMediaType(const std::string& typeStr) {
    if (typeStr == "movie") return MediaType::MOVIE;
    if (typeStr == "show") return MediaType::SHOW;
    if (typeStr == "season") return MediaType::SEASON;
    if (typeStr == "episode") return MediaType::EPISODE;
    if (typeStr == "artist") return MediaType::MUSIC_ARTIST;
    if (typeStr == "album") return MediaType::MUSIC_ALBUM;
    if (typeStr == "track") return MediaType::MUSIC_TRACK;
    if (typeStr == "photo") return MediaType::PHOTO;
    return MediaType::UNKNOWN;
}

// ============================================================================
// Authentication
// ============================================================================

bool App::login(const std::string& username, const std::string& password) {
    debugLog("Attempting login for user: %s\n", username.c_str());
    
    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/users/signin";
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    req.headers["X-Plex-Platform"] = PLEX_PLATFORM;
    req.headers["X-Plex-Device"] = PLEX_DEVICE;
    
    req.body = "login=" + username + "&password=" + password;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 201 || resp.statusCode == 200) {
        m_authToken = extractJsonValue(resp.body, "authToken");
        if (!m_authToken.empty()) {
            debugLog("Login successful, token obtained\n");
            
            // Store user info
            m_settings.username = extractJsonValue(resp.body, "username");
            m_settings.email = extractJsonValue(resp.body, "email");
            if (m_settings.username.empty()) {
                m_settings.username = username;  // Fallback to input
            }
            
            debugLog("Logged in as: %s (%s)\n", m_settings.username.c_str(), m_settings.email.c_str());
            return true;
        }
    }
    
    std::string error = extractJsonValue(resp.body, "error");
    if (!error.empty()) {
        setError(error);
    } else {
        setError("Login failed: " + std::to_string(resp.statusCode));
    }
    return false;
}

bool App::requestPin() {
    debugLog("Requesting PIN for plex.tv/link authentication\n");
    
    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/pins";
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    req.headers["X-Plex-Platform"] = PLEX_PLATFORM;
    req.headers["X-Plex-Device"] = PLEX_DEVICE;
    
    req.body = "strong=false";  // Non-strong code for plex.tv/link
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 201 || resp.statusCode == 200) {
        m_pinAuth.id = extractJsonInt(resp.body, "id");
        m_pinAuth.code = extractJsonValue(resp.body, "code");
        m_pinAuth.expiresIn = extractJsonInt(resp.body, "expiresIn");
        m_pinAuth.authToken = "";
        m_pinAuth.expired = false;
        
        if (!m_pinAuth.code.empty()) {
            debugLog("PIN obtained: %s (id: %d)\n", m_pinAuth.code.c_str(), m_pinAuth.id);
            s_pinCheckCounter = 0;
            return true;
        }
    }
    
    setError("Failed to get PIN: " + std::to_string(resp.statusCode));
    return false;
}

bool App::checkPin() {
    if (m_pinAuth.id == 0) return false;
    
    char url[256];
    snprintf(url, sizeof(url), "https://plex.tv/api/v2/pins/%d", m_pinAuth.id);
    
    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        std::string authToken = extractJsonValue(resp.body, "authToken");
        if (!authToken.empty() && authToken != "null") {
            m_pinAuth.authToken = authToken;
            m_authToken = authToken;
            
            // Store user info if available
            m_settings.username = extractJsonValue(resp.body, "username");
            m_settings.email = extractJsonValue(resp.body, "email");
            
            debugLog("PIN authorized! Token obtained.\n");
            if (!m_settings.username.empty()) {
                debugLog("Logged in as: %s\n", m_settings.username.c_str());
            }
            return true;
        }
        
        // Check if expired
        int expiresIn = extractJsonInt(resp.body, "expiresIn");
        if (expiresIn <= 0) {
            m_pinAuth.expired = true;
        }
    }
    
    return false;
}

bool App::connectToServer(const std::string& url) {
    debugLog("Connecting to server: %s\n", url.c_str());
    
    // Store the URL as-is (should include port if needed)
    m_currentServer.address = url;
    
    // Remove trailing slash for consistency
    while (!m_currentServer.address.empty() && m_currentServer.address.back() == '/') {
        m_currentServer.address.pop_back();
    }
    
    // Test connection
    std::string apiUrl = buildApiUrl("/");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_currentServer.machineIdentifier = extractJsonValue(resp.body, "machineIdentifier");
        m_currentServer.name = extractJsonValue(resp.body, "friendlyName");
        if (m_currentServer.name.empty()) {
            m_currentServer.name = "Plex Server";
        }
        debugLog("Connected to: %s (%s)\n", 
                      m_currentServer.name.c_str(), 
                      m_currentServer.machineIdentifier.c_str());
        
        // Save credentials for auto-login
        if (m_settings.rememberLogin && !m_authToken.empty()) {
            m_settings.savedAuthToken = m_authToken;
            m_settings.savedServerUrl = m_currentServer.address;
            m_settings.savedServerName = m_currentServer.name;
            saveSettings();
            debugLog("Credentials saved for auto-login\n");
        }
        
        return true;
    }
    
    setError("Failed to connect: " + std::to_string(resp.statusCode));
    return false;
}

void App::logout() {
    // Clear thumbnails before clearing items
    clearThumbnails();
    
    // Clear saved credentials
    m_settings.savedAuthToken.clear();
    m_settings.savedServerUrl.clear();
    m_settings.savedServerName.clear();
    saveSettings();  // Save to remove stored credentials
    
    m_authToken.clear();
    m_currentServer = PlexServer();
    m_librarySections.clear();
    m_mediaItems.clear();
    m_searchResults.clear();
    m_continueWatching.clear();
    m_hubs.clear();
    m_navStack.clear();
    m_pinAuth = PinAuth();
    m_settings = AppSettings();
    s_serverUrl[0] = '\0';
    s_username[0] = '\0';
    s_password[0] = '\0';
    m_selectedItem = 0;
    m_scrollOffset = 0;
    setState(AppState::LOGIN);
}

// ============================================================================
// Settings Persistence
// ============================================================================

static const char* SETTINGS_PATH = "ux0:data/VitaPlex/settings.cfg";

bool App::saveSettings() {
    debugLog("Saving settings to %s\n", SETTINGS_PATH);
    
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        debugLog("Failed to open settings file for writing\n");
        return false;
    }
    
    // Write simple key=value format
    char buffer[2048];
    int len = snprintf(buffer, sizeof(buffer),
        "version=%d\n"
        "videoQuality=%d\n"
        "autoPlay=%d\n"
        "showSubtitles=%d\n"
        "enableFileLogging=%d\n"
        "rememberLogin=%d\n"
        "username=%s\n"
        "email=%s\n"
        "authToken=%s\n"
        "serverUrl=%s\n"
        "serverName=%s\n",
        VITA_PLEX_VERSION_NUM,
        (int)m_settings.videoQuality,
        m_settings.autoPlay ? 1 : 0,
        m_settings.showSubtitles ? 1 : 0,
        m_settings.enableFileLogging ? 1 : 0,
        m_settings.rememberLogin ? 1 : 0,
        m_settings.username.c_str(),
        m_settings.email.c_str(),
        m_settings.rememberLogin ? m_settings.savedAuthToken.c_str() : "",
        m_settings.rememberLogin ? m_settings.savedServerUrl.c_str() : "",
        m_settings.rememberLogin ? m_settings.savedServerName.c_str() : ""
    );
    
    sceIoWrite(fd, buffer, len);
    sceIoClose(fd);
    
    debugLog("Settings saved successfully\n");
    return true;
}

bool App::loadSettings() {
    debugLog("Loading settings from %s\n", SETTINGS_PATH);
    
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        debugLog("No settings file found, using defaults\n");
        return false;
    }
    
    char buffer[2048];
    int bytesRead = sceIoRead(fd, buffer, sizeof(buffer) - 1);
    sceIoClose(fd);
    
    if (bytesRead <= 0) {
        return false;
    }
    buffer[bytesRead] = '\0';
    
    // Parse key=value pairs
    std::string content(buffer);
    size_t pos = 0;
    while (pos < content.length()) {
        size_t lineEnd = content.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = content.length();
        
        std::string line = content.substr(pos, lineEnd - pos);
        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);
            
            if (key == "videoQuality") {
                m_settings.videoQuality = (VideoQuality)atoi(value.c_str());
            } else if (key == "autoPlay") {
                m_settings.autoPlay = (atoi(value.c_str()) != 0);
            } else if (key == "showSubtitles") {
                m_settings.showSubtitles = (atoi(value.c_str()) != 0);
            } else if (key == "enableFileLogging") {
                m_settings.enableFileLogging = (atoi(value.c_str()) != 0);
                // Enable debug logging if it was saved as enabled
                if (m_settings.enableFileLogging) {
                    setDebugLogEnabled(true);
                }
            } else if (key == "rememberLogin") {
                m_settings.rememberLogin = (atoi(value.c_str()) != 0);
            } else if (key == "username") {
                m_settings.username = value;
            } else if (key == "email") {
                m_settings.email = value;
            } else if (key == "authToken") {
                m_settings.savedAuthToken = value;
            } else if (key == "serverUrl") {
                m_settings.savedServerUrl = value;
            } else if (key == "serverName") {
                m_settings.savedServerName = value;
            }
        }
        pos = lineEnd + 1;
    }
    
    debugLog("Settings loaded: authToken=%s, serverUrl=%s\n", 
                  m_settings.savedAuthToken.empty() ? "(none)" : "(saved)",
                  m_settings.savedServerUrl.c_str());
    return true;
}

bool App::restoreSavedLogin() {
    if (m_settings.savedAuthToken.empty() || m_settings.savedServerUrl.empty()) {
        return false;
    }
    
    debugLog("Restoring saved login to %s\n", m_settings.savedServerUrl.c_str());
    
    // Restore auth token and server
    m_authToken = m_settings.savedAuthToken;
    m_currentServer.address = m_settings.savedServerUrl;
    m_currentServer.name = m_settings.savedServerName;
    
    // Test connection by fetching library sections
    if (!fetchLibrarySections()) {
        debugLog("Failed to verify saved login\n");
        m_authToken.clear();
        m_currentServer = PlexServer();
        return false;
    }
    
    // Pre-fill the server URL field for display
    strncpy(s_serverUrl, m_settings.savedServerUrl.c_str(), sizeof(s_serverUrl) - 1);
    
    return true;
}

// ============================================================================
// Library Operations
// ============================================================================

bool App::fetchLibrarySections() {
    debugLog("Fetching library sections...\n");
    
    std::string apiUrl = buildApiUrl("/library/sections");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_librarySections.clear();
        
        // Parse sections from JSON
        size_t pos = 0;
        while ((pos = resp.body.find("\"key\":", pos)) != std::string::npos) {
            // Find the object boundaries
            size_t objStart = resp.body.rfind('{', pos);
            size_t objEnd = resp.body.find('}', pos);
            if (objStart == std::string::npos || objEnd == std::string::npos) break;
            
            std::string objJson = resp.body.substr(objStart, objEnd - objStart + 1);
            
            LibrarySection section;
            section.key = extractJsonValue(objJson, "key");
            section.title = extractJsonValue(objJson, "title");
            section.type = extractJsonValue(objJson, "type");
            section.thumb = extractJsonValue(objJson, "thumb");
            section.art = extractJsonValue(objJson, "art");
            
            // Only add valid sections (with numeric keys)
            if (!section.key.empty() && !section.title.empty()) {
                m_librarySections.push_back(section);
                debugLog("  Found library: %s (%s)\n", section.title.c_str(), section.type.c_str());
            }
            
            pos = objEnd;
        }
        
        return !m_librarySections.empty();
    }
    
    setError("Failed to fetch libraries: " + std::to_string(resp.statusCode));
    return false;
}

bool App::fetchLibraryContent(const std::string& sectionKey) {
    debugLog("Fetching content for section: %s\n", sectionKey.c_str());
    
    m_currentSectionKey = sectionKey;
    std::string apiUrl = buildApiUrl("/library/sections/" + sectionKey + "/all");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_mediaItems.clear();
        
        // Parse media items
        size_t pos = 0;
        std::string searchFor = "\"ratingKey\":";
        while ((pos = resp.body.find(searchFor, pos)) != std::string::npos) {
            size_t objStart = resp.body.rfind('{', pos);
            size_t objEnd = pos;
            int braceCount = 0;
            bool foundEnd = false;
            
            // Find matching closing brace
            for (size_t i = objStart; i < resp.body.length() && !foundEnd; i++) {
                if (resp.body[i] == '{') braceCount++;
                else if (resp.body[i] == '}') {
                    braceCount--;
                    if (braceCount == 0) {
                        objEnd = i;
                        foundEnd = true;
                    }
                }
            }
            
            if (!foundEnd) break;
            
            std::string objJson = resp.body.substr(objStart, objEnd - objStart + 1);
            
            MediaItem item;
            item.ratingKey = extractJsonValue(objJson, "ratingKey");
            item.key = extractJsonValue(objJson, "key");
            item.title = extractJsonValue(objJson, "title");
            item.summary = extractJsonValue(objJson, "summary");
            item.thumb = extractJsonValue(objJson, "thumb");
            item.art = extractJsonValue(objJson, "art");
            item.type = extractJsonValue(objJson, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(objJson, "year");
            item.duration = extractJsonInt(objJson, "duration");
            item.viewOffset = extractJsonInt(objJson, "viewOffset");
            item.rating = extractJsonFloat(objJson, "rating");
            item.contentRating = extractJsonValue(objJson, "contentRating");
            item.studio = extractJsonValue(objJson, "studio");
            item.grandparentTitle = extractJsonValue(objJson, "grandparentTitle");
            item.seasonNumber = extractJsonInt(objJson, "parentIndex");
            item.episodeNumber = extractJsonInt(objJson, "index");
            
            // Check if watched
            std::string viewCount = extractJsonValue(objJson, "viewCount");
            item.watched = !viewCount.empty() && viewCount != "0";
            
            if (!item.ratingKey.empty() && !item.title.empty()) {
                m_mediaItems.push_back(item);
            }
            
            pos = objEnd;
        }
        
        debugLog("Found %d items\n", (int)m_mediaItems.size());
        return true;
    }
    
    setError("Failed to fetch content: " + std::to_string(resp.statusCode));
    return false;
}

bool App::fetchChildren(const std::string& ratingKey) {
    debugLog("Fetching children for: %s\n", ratingKey.c_str());
    
    std::string apiUrl = buildApiUrl("/library/metadata/" + ratingKey + "/children");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_mediaItems.clear();
        
        // Parse children items (seasons, episodes, albums, tracks)
        size_t pos = 0;
        std::string searchFor = "\"ratingKey\":";
        while ((pos = resp.body.find(searchFor, pos)) != std::string::npos) {
            size_t objStart = resp.body.rfind('{', pos);
            size_t objEnd = pos;
            int braceCount = 0;
            bool foundEnd = false;
            
            for (size_t i = objStart; i < resp.body.length() && !foundEnd; i++) {
                if (resp.body[i] == '{') braceCount++;
                else if (resp.body[i] == '}') {
                    braceCount--;
                    if (braceCount == 0) {
                        objEnd = i;
                        foundEnd = true;
                    }
                }
            }
            
            if (!foundEnd) break;
            
            std::string objJson = resp.body.substr(objStart, objEnd - objStart + 1);
            
            MediaItem item;
            item.ratingKey = extractJsonValue(objJson, "ratingKey");
            item.key = extractJsonValue(objJson, "key");
            item.title = extractJsonValue(objJson, "title");
            item.summary = extractJsonValue(objJson, "summary");
            item.thumb = extractJsonValue(objJson, "thumb");
            item.type = extractJsonValue(objJson, "type");
            item.mediaType = parseMediaType(item.type);
            item.index = extractJsonInt(objJson, "index");
            item.parentIndex = extractJsonInt(objJson, "parentIndex");
            item.leafCount = extractJsonInt(objJson, "leafCount");
            item.viewedLeafCount = extractJsonInt(objJson, "viewedLeafCount");
            item.duration = extractJsonInt(objJson, "duration");
            item.viewOffset = extractJsonInt(objJson, "viewOffset");
            item.year = extractJsonInt(objJson, "year");
            
            // Parent info
            item.grandparentTitle = extractJsonValue(objJson, "grandparentTitle");
            item.parentTitle = extractJsonValue(objJson, "parentTitle");
            
            // Watch status
            std::string viewCount = extractJsonValue(objJson, "viewCount");
            item.watched = !viewCount.empty() && viewCount != "0";
            
            if (!item.ratingKey.empty()) {
                m_mediaItems.push_back(item);
                debugLog("  Child: %s (type: %s, index: %d)\n", 
                              item.title.c_str(), item.type.c_str(), item.index);
            }
            
            pos = objEnd;
        }
        
        debugLog("Found %d children\n", (int)m_mediaItems.size());
        return !m_mediaItems.empty();
    }
    
    setError("Failed to fetch children: " + std::to_string(resp.statusCode));
    return false;
}

// Navigation stack management
void App::pushNavigation(const std::string& key, const std::string& title, MediaType type) {
    NavEntry entry;
    entry.key = key;
    entry.title = title;
    entry.type = type;
    entry.selectedItem = m_selectedItem;
    entry.scrollOffset = m_scrollOffset;
    m_navStack.push_back(entry);
    debugLog("Push nav: %s (%d items in stack)\n", title.c_str(), (int)m_navStack.size());
}

void App::popNavigation() {
    if (!m_navStack.empty()) {
        NavEntry entry = m_navStack.back();
        m_navStack.pop_back();
        m_selectedItem = entry.selectedItem;
        m_scrollOffset = entry.scrollOffset;
        debugLog("Pop nav: %s (%d items in stack)\n", entry.title.c_str(), (int)m_navStack.size());
    }
}

// Image loading
bool App::loadThumbnail(MediaItem& item, int width, int height) {
    if (item.thumb.empty() || item.thumbTexture != nullptr) {
        return item.thumbTexture != nullptr;
    }
    
    // Build thumbnail URL with transcoding for size
    std::string thumbUrl = m_currentServer.address + "/photo/:/transcode";
    thumbUrl += "?url=" + item.thumb;
    
    char sizeStr[64];
    snprintf(sizeStr, sizeof(sizeStr), "&width=%d&height=%d&minSize=1", width, height);
    thumbUrl += sizeStr;
    thumbUrl += "&X-Plex-Token=" + m_authToken;
    
    debugLog("Loading thumbnail: %s\n", item.title.c_str());
    
    HttpClient client;
    HttpRequest req;
    req.url = thumbUrl;
    req.method = "GET";
    req.headers["Accept"] = "image/jpeg, image/png";
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200 && !resp.body.empty()) {
        // Try to load as PNG first, then JPEG
        item.thumbTexture = vita2d_load_PNG_buffer((const unsigned char*)resp.body.data());
        if (!item.thumbTexture) {
            item.thumbTexture = vita2d_load_JPEG_buffer((const unsigned char*)resp.body.data(), resp.body.size());
        }
        
        if (item.thumbTexture) {
            debugLog("Loaded thumbnail for: %s\n", item.title.c_str());
            return true;
        }
    }
    
    return false;
}

void App::loadVisibleThumbnails() {
    // Load thumbnails for currently visible items (async would be better)
    int visibleItems = 5;
    int startIdx = m_scrollOffset;
    int endIdx = std::min((int)m_mediaItems.size(), startIdx + visibleItems);
    
    for (int i = startIdx; i < endIdx; i++) {
        if (m_mediaItems[i].thumbTexture == nullptr && !m_mediaItems[i].thumb.empty()) {
            loadThumbnail(m_mediaItems[i], 100, 150);
            break;  // Load one at a time to avoid blocking
        }
    }
}

void App::clearThumbnails() {
    debugLog("Clearing %d thumbnails\n", (int)m_mediaItems.size());
    
    // Wait for any pending drawing to complete
    vita2d_wait_rendering_done();
    
    for (auto& item : m_mediaItems) {
        if (item.thumbTexture != nullptr) {
            vita2d_free_texture(item.thumbTexture);
            item.thumbTexture = nullptr;
        }
    }
    
    // Also clear search results thumbnails
    for (auto& item : m_searchResults) {
        if (item.thumbTexture != nullptr) {
            vita2d_free_texture(item.thumbTexture);
            item.thumbTexture = nullptr;
        }
    }
    
    // Clear continue watching thumbnails
    for (auto& item : m_continueWatching) {
        if (item.thumbTexture != nullptr) {
            vita2d_free_texture(item.thumbTexture);
            item.thumbTexture = nullptr;
        }
    }
}

bool App::fetchMediaDetails(const std::string& ratingKey) {
    debugLog("Fetching details for: %s\n", ratingKey.c_str());
    
    std::string apiUrl = buildApiUrl("/library/metadata/" + ratingKey);
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        // Parse the first Metadata item
        size_t metaStart = resp.body.find("\"Metadata\"");
        if (metaStart != std::string::npos) {
            std::string json = resp.body.substr(metaStart);
            
            m_currentMedia.ratingKey = ratingKey;
            m_currentMedia.title = extractJsonValue(json, "title");
            m_currentMedia.summary = extractJsonValue(json, "summary");
            m_currentMedia.thumb = extractJsonValue(json, "thumb");
            m_currentMedia.art = extractJsonValue(json, "art");
            m_currentMedia.type = extractJsonValue(json, "type");
            m_currentMedia.mediaType = parseMediaType(m_currentMedia.type);
            m_currentMedia.year = extractJsonInt(json, "year");
            m_currentMedia.duration = extractJsonInt(json, "duration");
            m_currentMedia.viewOffset = extractJsonInt(json, "viewOffset");
            m_currentMedia.rating = extractJsonFloat(json, "rating");
            m_currentMedia.contentRating = extractJsonValue(json, "contentRating");
            m_currentMedia.studio = extractJsonValue(json, "studio");
            
            // Extract stream info from Media/Part
            size_t mediaPos = json.find("\"Media\"");
            if (mediaPos != std::string::npos) {
                std::string mediaJson = json.substr(mediaPos);
                m_currentMedia.videoCodec = extractJsonValue(mediaJson, "videoCodec");
                m_currentMedia.audioCodec = extractJsonValue(mediaJson, "audioCodec");
                m_currentMedia.videoWidth = extractJsonInt(mediaJson, "width");
                m_currentMedia.videoHeight = extractJsonInt(mediaJson, "height");
                
                // Get stream URL from Part
                size_t partPos = mediaJson.find("\"Part\"");
                if (partPos != std::string::npos) {
                    std::string partJson = mediaJson.substr(partPos);
                    std::string partKey = extractJsonValue(partJson, "key");
                    if (!partKey.empty()) {
                        m_currentMedia.streamUrl = buildApiUrl(partKey);
                    }
                }
            }
            
            return true;
        }
    }
    
    setError("Failed to fetch details: " + std::to_string(resp.statusCode));
    return false;
}

bool App::fetchHubs() {
    debugLog("Fetching home hubs...\n");
    
    std::string apiUrl = buildApiUrl("/hubs");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_hubs.clear();
        
        // Parse hubs
        size_t pos = 0;
        while ((pos = resp.body.find("\"hubIdentifier\":", pos)) != std::string::npos) {
            size_t objStart = resp.body.rfind('{', pos);
            
            // Find end of hub object (tricky with nested Metadata)
            size_t nextHub = resp.body.find("\"hubIdentifier\":", pos + 1);
            size_t objEnd = (nextHub != std::string::npos) ? nextHub : resp.body.length();
            
            std::string hubJson = resp.body.substr(objStart, objEnd - objStart);
            
            Hub hub;
            hub.title = extractJsonValue(hubJson, "title");
            hub.type = extractJsonValue(hubJson, "type");
            hub.hubIdentifier = extractJsonValue(hubJson, "hubIdentifier");
            hub.key = extractJsonValue(hubJson, "key");
            hub.more = extractJsonBool(hubJson, "more");
            
            // Parse items in this hub (limited to first few)
            size_t itemPos = 0;
            int itemCount = 0;
            while ((itemPos = hubJson.find("\"ratingKey\":", itemPos)) != std::string::npos && itemCount < 10) {
                size_t itemStart = hubJson.rfind('{', itemPos);
                size_t itemEnd = hubJson.find('}', itemPos);
                if (itemStart != std::string::npos && itemEnd != std::string::npos) {
                    std::string itemJson = hubJson.substr(itemStart, itemEnd - itemStart + 1);
                    
                    MediaItem item;
                    item.ratingKey = extractJsonValue(itemJson, "ratingKey");
                    item.title = extractJsonValue(itemJson, "title");
                    item.thumb = extractJsonValue(itemJson, "thumb");
                    item.type = extractJsonValue(itemJson, "type");
                    item.mediaType = parseMediaType(item.type);
                    item.year = extractJsonInt(itemJson, "year");
                    
                    if (!item.ratingKey.empty()) {
                        hub.items.push_back(item);
                        itemCount++;
                    }
                }
                itemPos = itemEnd;
            }
            
            if (!hub.hubIdentifier.empty() && !hub.items.empty()) {
                m_hubs.push_back(hub);
                debugLog("  Hub: %s (%d items)\n", hub.title.c_str(), (int)hub.items.size());
            }
            
            pos = objEnd;
        }
        
        return true;
    }
    
    return false;
}

bool App::fetchContinueWatching() {
    debugLog("Fetching continue watching (on deck)...\n");
    
    // Get on deck items - these are items in progress
    std::string apiUrl = buildApiUrl("/library/onDeck");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_continueWatching.clear();
        
        size_t pos = 0;
        while ((pos = resp.body.find("\"ratingKey\":", pos)) != std::string::npos) {
            size_t objStart = resp.body.rfind('{', pos);
            size_t objEnd = resp.body.find('}', pos);
            if (objStart == std::string::npos || objEnd == std::string::npos) break;
            
            std::string objJson = resp.body.substr(objStart, objEnd - objStart + 1);
            
            MediaItem item;
            item.ratingKey = extractJsonValue(objJson, "ratingKey");
            item.title = extractJsonValue(objJson, "title");
            item.thumb = extractJsonValue(objJson, "thumb");
            item.type = extractJsonValue(objJson, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(objJson, "year");
            item.duration = extractJsonInt(objJson, "duration");
            item.viewOffset = extractJsonInt(objJson, "viewOffset");
            item.grandparentTitle = extractJsonValue(objJson, "grandparentTitle");
            
            if (!item.ratingKey.empty()) {
                m_continueWatching.push_back(item);
                debugLog("  Found on deck: %s (offset: %d)\n", item.title.c_str(), item.viewOffset);
            }
            
            pos = objEnd;
        }
        
        debugLog("Found %d items on deck\n", (int)m_continueWatching.size());
        
        if (m_continueWatching.empty()) {
            setError("No items in Continue Watching");
            return false;
        }
        
        return true;
    }
    
    setError("Failed to fetch continue watching");
    return false;
}

bool App::fetchRecentlyAdded() {
    debugLog("Fetching recently added...\n");
    
    std::string apiUrl = buildApiUrl("/library/recentlyAdded");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        // Parse same as library content
        m_mediaItems.clear();
        
        size_t pos = 0;
        while ((pos = resp.body.find("\"ratingKey\":", pos)) != std::string::npos) {
            size_t objStart = resp.body.rfind('{', pos);
            size_t objEnd = resp.body.find('}', pos);
            if (objStart == std::string::npos || objEnd == std::string::npos) break;
            
            std::string objJson = resp.body.substr(objStart, objEnd - objStart + 1);
            
            MediaItem item;
            item.ratingKey = extractJsonValue(objJson, "ratingKey");
            item.title = extractJsonValue(objJson, "title");
            item.thumb = extractJsonValue(objJson, "thumb");
            item.type = extractJsonValue(objJson, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(objJson, "year");
            
            if (!item.ratingKey.empty()) {
                m_mediaItems.push_back(item);
            }
            
            pos = objEnd;
        }
        
        return true;
    }
    
    return false;
}

// ============================================================================
// Live TV
// ============================================================================

bool App::fetchLiveTVChannels() {
    debugLog("Fetching Live TV channels...\n");
    
    m_liveTVChannels.clear();
    m_hasLiveTV = false;
    
    HttpClient client;
    HttpRequest req;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    // Step 1: Get DVR devices from /livetv/dvrs
    std::string apiUrl = buildApiUrl("/livetv/dvrs");
    req.url = apiUrl;
    HttpResponse resp = client.request(req);
    
    debugLog("DVR response status: %d, body length: %d\n", resp.statusCode, (int)resp.body.size());
    
    std::vector<std::string> dvrIds;
    
    if (resp.statusCode == 200) {
        // Parse DVR IDs from response
        // Look for "key":"/livetv/dvrs/X" pattern or "id":"X"
        size_t pos = 0;
        while ((pos = resp.body.find("\"key\":\"/livetv/dvrs/", pos)) != std::string::npos) {
            pos += 20;  // Skip past "key":"/livetv/dvrs/
            size_t endPos = resp.body.find("\"", pos);
            if (endPos != std::string::npos) {
                std::string dvrId = resp.body.substr(pos, endPos - pos);
                // Remove any trailing path parts
                size_t slashPos = dvrId.find('/');
                if (slashPos != std::string::npos) {
                    dvrId = dvrId.substr(0, slashPos);
                }
                if (!dvrId.empty()) {
                    dvrIds.push_back(dvrId);
                    debugLog("Found DVR ID: %s\n", dvrId.c_str());
                }
            }
        }
        
        // Also try "identifier" field
        pos = 0;
        while ((pos = resp.body.find("\"identifier\":\"", pos)) != std::string::npos) {
            pos += 14;
            size_t endPos = resp.body.find("\"", pos);
            if (endPos != std::string::npos) {
                std::string dvrId = resp.body.substr(pos, endPos - pos);
                bool found = false;
                for (const auto& id : dvrIds) {
                    if (id == dvrId) found = true;
                }
                if (!found && !dvrId.empty()) {
                    dvrIds.push_back(dvrId);
                    debugLog("Found DVR identifier: %s\n", dvrId.c_str());
                }
            }
        }
        
        m_hasLiveTV = !dvrIds.empty();
    }
    
    // Step 2: For each DVR, get the channel lineup
    for (const auto& dvrId : dvrIds) {
        // Try lineup endpoint
        apiUrl = buildApiUrl("/livetv/dvrs/" + dvrId + "/lineup");
        req.url = apiUrl;
        resp = client.request(req);
        
        debugLog("Lineup for DVR %s: status %d\n", dvrId.c_str(), resp.statusCode);
        
        if (resp.statusCode == 200) {
            parseChannelsFromResponse(resp.body);
        }
        
        // Also try channels endpoint
        apiUrl = buildApiUrl("/livetv/dvrs/" + dvrId + "/channels");
        req.url = apiUrl;
        resp = client.request(req);
        
        if (resp.statusCode == 200) {
            parseChannelsFromResponse(resp.body);
        }
    }
    
    // Step 3: Try media/providers endpoint for cloud-based Live TV
    apiUrl = buildApiUrl("/media/providers");
    req.url = apiUrl;
    resp = client.request(req);
    
    if (resp.statusCode == 200 && resp.body.find("livetv") != std::string::npos) {
        debugLog("Found Live TV in media providers\n");
        m_hasLiveTV = true;
        
        // Try to get grid data from the provider
        size_t providerPos = resp.body.find("\"identifier\":\"");
        if (providerPos != std::string::npos) {
            providerPos += 14;
            size_t endPos = resp.body.find("\"", providerPos);
            if (endPos != std::string::npos) {
                std::string providerId = resp.body.substr(providerPos, endPos - providerPos);
                
                // Try to get grid from provider
                apiUrl = buildApiUrl("/media/providers/" + providerId + "/grid");
                req.url = apiUrl;
                resp = client.request(req);
                
                if (resp.statusCode == 200) {
                    parseChannelsFromResponse(resp.body);
                }
            }
        }
    }
    
    // Step 4: Try the tv.plex.providers.epg endpoint (for Plex Free TV)
    apiUrl = buildApiUrl("/tv.plex.providers.epg.cloud/hubs/discover");
    req.url = apiUrl;
    resp = client.request(req);
    
    if (resp.statusCode == 200) {
        debugLog("Found Plex Free TV channels\n");
        m_hasLiveTV = true;
        parseChannelsFromResponse(resp.body);
    }
    
    // Step 5: Check library sections for Live TV type
    for (const auto& section : m_librarySections) {
        if (section.type == "movie" || section.type == "show") continue;
        
        apiUrl = buildApiUrl("/library/sections/" + section.key + "/all");
        req.url = apiUrl;
        resp = client.request(req);
        
        if (resp.statusCode == 200) {
            // Check if this looks like live TV content
            if (resp.body.find("\"live\"") != std::string::npos || 
                resp.body.find("\"channel\"") != std::string::npos) {
                m_hasLiveTV = true;
                parseChannelsFromResponse(resp.body);
            }
        }
    }
    
    if (m_liveTVChannels.empty()) {
        debugLog("No Live TV channels found after all attempts\n");
        return false;
    }
    
    // Remove duplicates
    std::sort(m_liveTVChannels.begin(), m_liveTVChannels.end(),
              [](const LiveTVChannel& a, const LiveTVChannel& b) {
                  if (a.channelNumber != b.channelNumber) 
                      return a.channelNumber < b.channelNumber;
                  return a.ratingKey < b.ratingKey;
              });
    
    auto last = std::unique(m_liveTVChannels.begin(), m_liveTVChannels.end(),
                           [](const LiveTVChannel& a, const LiveTVChannel& b) {
                               return a.ratingKey == b.ratingKey;
                           });
    m_liveTVChannels.erase(last, m_liveTVChannels.end());
    
    debugLog("Total Live TV channels: %d\n", (int)m_liveTVChannels.size());
    return true;
}

void App::parseChannelsFromResponse(const std::string& body) {
    // Try multiple JSON patterns for channels
    
    // Pattern 1: "ratingKey" based channels
    size_t pos = 0;
    while ((pos = body.find("\"ratingKey\":", pos)) != std::string::npos) {
        // Find the object boundaries
        size_t objStart = body.rfind('{', pos);
        size_t objEnd = pos;
        int braceCount = 0;
        bool inString = false;
        
        for (size_t i = objStart; i < body.size(); i++) {
            if (body[i] == '"' && (i == 0 || body[i-1] != '\\')) {
                inString = !inString;
            }
            if (!inString) {
                if (body[i] == '{') braceCount++;
                else if (body[i] == '}') {
                    braceCount--;
                    if (braceCount == 0) {
                        objEnd = i + 1;
                        break;
                    }
                }
            }
        }
        
        if (objStart == std::string::npos || objEnd <= objStart) {
            pos++;
            continue;
        }
        
        std::string objJson = body.substr(objStart, objEnd - objStart);
        
        // Check if this looks like a channel/live TV item
        std::string type = extractJsonValue(objJson, "type");
        if (type != "channel" && type != "video" && type != "clip" && 
            objJson.find("\"live\"") == std::string::npos) {
            pos = objEnd;
            continue;
        }
        
        LiveTVChannel channel;
        channel.ratingKey = extractJsonValue(objJson, "ratingKey");
        channel.key = extractJsonValue(objJson, "key");
        channel.title = extractJsonValue(objJson, "title");
        if (channel.title.empty()) {
            channel.title = extractJsonValue(objJson, "name");
        }
        channel.thumb = extractJsonValue(objJson, "thumb");
        channel.callSign = extractJsonValue(objJson, "callSign");
        if (channel.callSign.empty()) {
            channel.callSign = extractJsonValue(objJson, "channelCallSign");
        }
        channel.channelNumber = extractJsonInt(objJson, "index");
        if (channel.channelNumber == 0) {
            channel.channelNumber = extractJsonInt(objJson, "channelNumber");
        }
        channel.currentProgram = extractJsonValue(objJson, "grandparentTitle");
        if (channel.currentProgram.empty()) {
            channel.currentProgram = extractJsonValue(objJson, "summary");
        }
        
        if (!channel.ratingKey.empty() && !channel.title.empty()) {
            m_liveTVChannels.push_back(channel);
            debugLog("Added channel: %s (#%d)\n", channel.title.c_str(), channel.channelNumber);
        }
        
        pos = objEnd;
    }
    
    // Pattern 2: "Channel" wrapper objects
    pos = 0;
    while ((pos = body.find("\"Channel\"", pos)) != std::string::npos) {
        size_t arrStart = body.find('[', pos);
        if (arrStart == std::string::npos || arrStart > pos + 20) {
            pos++;
            continue;
        }
        
        size_t arrEnd = body.find(']', arrStart);
        if (arrEnd == std::string::npos) {
            pos++;
            continue;
        }
        
        std::string arrJson = body.substr(arrStart, arrEnd - arrStart + 1);
        
        // Parse each channel in the array
        size_t chanPos = 0;
        while ((chanPos = arrJson.find('{', chanPos)) != std::string::npos) {
            size_t chanEnd = arrJson.find('}', chanPos);
            if (chanEnd == std::string::npos) break;
            
            std::string chanJson = arrJson.substr(chanPos, chanEnd - chanPos + 1);
            
            LiveTVChannel channel;
            channel.ratingKey = extractJsonValue(chanJson, "ratingKey");
            channel.key = extractJsonValue(chanJson, "key");
            channel.title = extractJsonValue(chanJson, "title");
            channel.thumb = extractJsonValue(chanJson, "thumb");
            channel.callSign = extractJsonValue(chanJson, "callSign");
            channel.channelNumber = extractJsonInt(chanJson, "index");
            
            if (!channel.ratingKey.empty() && !channel.title.empty()) {
                // Check if already exists
                bool exists = false;
                for (const auto& ch : m_liveTVChannels) {
                    if (ch.ratingKey == channel.ratingKey) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    m_liveTVChannels.push_back(channel);
                }
            }
            
            chanPos = chanEnd + 1;
        }
        
        pos = arrEnd;
    }
}

bool App::fetchLiveTVGuide(int hoursAhead) {
    debugLog("Fetching Live TV guide for next %d hours\n", hoursAhead);
    
    // Get current programs for each channel
    for (auto& channel : m_liveTVChannels) {
        std::string apiUrl = buildApiUrl("/livetv/epg?channelID=" + channel.ratingKey);
        
        HttpClient client;
        HttpRequest req;
        req.url = apiUrl;
        req.method = "GET";
        req.headers["Accept"] = "application/json";
        req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
        
        HttpResponse resp = client.request(req);
        
        if (resp.statusCode == 200) {
            // Parse current program
            channel.currentProgram = extractJsonValue(resp.body, "title");
            
            // Try to find next program
            size_t nextPos = resp.body.find("\"title\":", resp.body.find("\"title\":") + 10);
            if (nextPos != std::string::npos) {
                size_t valueStart = resp.body.find("\"", nextPos + 8) + 1;
                size_t valueEnd = resp.body.find("\"", valueStart);
                if (valueStart != std::string::npos && valueEnd != std::string::npos) {
                    channel.nextProgram = resp.body.substr(valueStart, valueEnd - valueStart);
                }
            }
        }
    }
    
    return true;
}

bool App::startLiveTVPlayback(const std::string& channelKey) {
    debugLog("Starting Live TV playback for channel: %s\n", channelKey.c_str());
    
    // Find the channel
    const LiveTVChannel* channel = nullptr;
    for (const auto& ch : m_liveTVChannels) {
        if (ch.key == channelKey || ch.ratingKey == channelKey) {
            channel = &ch;
            break;
        }
    }
    
    if (!channel) {
        setError("Channel not found");
        return false;
    }
    
    // Build transcode URL for live TV
    // URL-encode the path parameter (required by Plex API)
    std::string encodedPath = HttpClient::urlEncode(channel->key);
    
    std::string transcodeUrl = m_currentServer.address;
    transcodeUrl += "/video/:/transcode/universal/start.m3u8?";
    transcodeUrl += "path=" + encodedPath;
    transcodeUrl += "&mediaIndex=0&partIndex=0";
    transcodeUrl += "&protocol=hls";
    transcodeUrl += "&directPlay=0&directStream=1";
    
    // Video settings - H.264 baseline for Vita
    transcodeUrl += "&videoBitrate=4000";
    transcodeUrl += "&videoCodec=h264";
    transcodeUrl += "&maxWidth=1280&maxHeight=720";
    
    // Audio - AAC stereo
    transcodeUrl += "&audioCodec=aac&audioChannels=2";
    
    // Live TV specific
    transcodeUrl += "&live=1";
    
    // Add token and Plex client identification parameters (URL-encode values with spaces)
    transcodeUrl += "&X-Plex-Token=" + m_authToken;
    transcodeUrl += "&X-Plex-Client-Identifier=" PLEX_CLIENT_ID;
    transcodeUrl += "&X-Plex-Product=" PLEX_CLIENT_NAME;
    transcodeUrl += "&X-Plex-Version=" PLEX_CLIENT_VERSION;
    transcodeUrl += "&X-Plex-Platform=PlayStation%20Vita";  // URL-encoded space
    transcodeUrl += "&X-Plex-Device=PS%20Vita";  // URL-encoded space
    
    // Generate a session ID for this transcode request
    char sessionId[32];
    snprintf(sessionId, sizeof(sessionId), "&session=vitaltv%llu", (unsigned long long)sceKernelGetProcessTimeWide());
    transcodeUrl += sessionId;
    
    debugLog("Live TV URL: %s\n", transcodeUrl.c_str());
    
    // Create a media item for the channel
    m_currentMedia = MediaItem();
    m_currentMedia.ratingKey = channel->ratingKey;
    m_currentMedia.title = channel->title;
    if (!channel->currentProgram.empty()) {
        m_currentMedia.title += " - " + channel->currentProgram;
    }
    m_currentMedia.type = "livetv";
    m_currentMedia.mediaType = MediaType::LIVE_TV_CHANNEL;
    m_currentMedia.streamUrl = transcodeUrl;
    m_currentMedia.duration = 0;  // Live has no fixed duration
    
    m_isPlaying = true;
    setState(AppState::PLAYER);
    
    // Try to start mpv playback
    #ifdef USE_MPV_PLAYER
    MpvPlayer& player = MpvPlayer::getInstance();
    if (!player.isInitialized()) {
        debugLog("Attempting to initialize mpv for Live TV...\n");
        if (!player.init()) {
            debugLog("MPV init failed for Live TV, showing URL only\n");
            // Don't fail - just show player UI
        }
    }
    
    if (player.isInitialized()) {
        if (!player.loadUrl(transcodeUrl, m_currentMedia.title)) {
            debugLog("MPV loadUrl failed for Live TV\n");
            // Continue anyway
        }
    }
    #endif
    
    return true;
}

// ============================================================================
// Search
// ============================================================================

bool App::search(const std::string& query) {
    debugLog("Searching for: %s\n", query.c_str());
    
    m_searchQuery = query;
    
    // URL encode the query
    std::string encodedQuery;
    for (char c : query) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encodedQuery += c;
        } else if (c == ' ') {
            encodedQuery += "%20";
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encodedQuery += hex;
        }
    }
    
    std::string apiUrl = buildApiUrl("/hubs/search?query=" + encodedQuery);
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        m_searchResults.clear();
        
        // Parse results from hubs
        size_t pos = 0;
        while ((pos = resp.body.find("\"ratingKey\":", pos)) != std::string::npos) {
            size_t objStart = resp.body.rfind('{', pos);
            size_t objEnd = resp.body.find('}', pos);
            if (objStart == std::string::npos || objEnd == std::string::npos) break;
            
            std::string objJson = resp.body.substr(objStart, objEnd - objStart + 1);
            
            MediaItem item;
            item.ratingKey = extractJsonValue(objJson, "ratingKey");
            item.title = extractJsonValue(objJson, "title");
            item.thumb = extractJsonValue(objJson, "thumb");
            item.type = extractJsonValue(objJson, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(objJson, "year");
            
            if (!item.ratingKey.empty()) {
                m_searchResults.push_back(item);
            }
            
            pos = objEnd;
        }
        
        debugLog("Found %d results\n", (int)m_searchResults.size());
        return true;
    }
    
    setError("Search failed: " + std::to_string(resp.statusCode));
    return false;
}

// ============================================================================
// Playback
// ============================================================================

bool App::getPlaybackUrl(const std::string& ratingKey) {
    // The stream URL is already set when fetching media details
    return fetchMediaDetails(ratingKey);
}

bool App::updatePlayProgress(const std::string& ratingKey, int timeMs) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), 
             "/:/timeline?ratingKey=%s&key=/library/metadata/%s&state=playing&time=%d",
             ratingKey.c_str(), ratingKey.c_str(), timeMs);
    
    std::string apiUrl = buildApiUrl(endpoint);
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    return resp.statusCode == 200;
}

bool App::markAsWatched(const std::string& ratingKey) {
    std::string apiUrl = buildApiUrl("/:/scrobble?key=/library/metadata/" + ratingKey + "&identifier=com.plexapp.plugins.library");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    return resp.statusCode == 200;
}

bool App::markAsUnwatched(const std::string& ratingKey) {
    std::string apiUrl = buildApiUrl("/:/unscrobble?key=/library/metadata/" + ratingKey + "&identifier=com.plexapp.plugins.library");
    
    HttpClient client;
    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    HttpResponse resp = client.request(req);
    return resp.statusCode == 200;
}

// ============================================================================
// Drawing
// ============================================================================

void App::drawLoginScreen(vita2d_pgf* font) {
    // Title
    vita2d_pgf_draw_text(font, 380, 60, COLOR_ORANGE, 1.2f, "VitaPlex");
    vita2d_pgf_draw_text(font, 350, 90, COLOR_GRAY, 0.8f, "Plex Client for PS Vita");
    
    // Instructions
    vita2d_pgf_draw_text(font, 50, 140, COLOR_WHITE, 0.7f, 
                         "Enter your Plex server URL and credentials, or use PIN authentication.");
    
    int y = 180;
    int fieldHeight = 50;
    
    // Server URL field
    unsigned int serverColor = (s_selectedField == 0) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(50, y, 860, 40, serverColor);
    vita2d_pgf_draw_text(font, 60, y + 28, COLOR_GRAY, 0.7f, "Server URL:");
    vita2d_pgf_draw_text(font, 200, y + 28, COLOR_WHITE, 0.8f, 
                         s_serverUrl[0] ? s_serverUrl : "http://192.168.1.x:32400");
    y += fieldHeight;
    
    // Username field
    unsigned int userColor = (s_selectedField == 1) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(50, y, 860, 40, userColor);
    vita2d_pgf_draw_text(font, 60, y + 28, COLOR_GRAY, 0.7f, "Username:");
    vita2d_pgf_draw_text(font, 200, y + 28, COLOR_WHITE, 0.8f, 
                         s_username[0] ? s_username : "plex@example.com");
    y += fieldHeight;
    
    // Password field
    unsigned int passColor = (s_selectedField == 2) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(50, y, 860, 40, passColor);
    vita2d_pgf_draw_text(font, 60, y + 28, COLOR_GRAY, 0.7f, "Password:");
    if (s_password[0]) {
        std::string masked(strlen(s_password), '*');
        vita2d_pgf_draw_text(font, 200, y + 28, COLOR_WHITE, 0.8f, masked.c_str());
    } else {
        vita2d_pgf_draw_text(font, 200, y + 28, COLOR_GRAY, 0.8f, "********");
    }
    y += fieldHeight + 20;
    
    // Login button
    unsigned int loginBtnColor = (s_selectedField == 3) ? COLOR_ORANGE : COLOR_CARD_BG;
    vita2d_draw_rectangle(50, y, 400, 50, loginBtnColor);
    vita2d_pgf_draw_text(font, 180, y + 35, COLOR_WHITE, 1.0f, "Login with Credentials");
    
    // PIN button
    unsigned int pinBtnColor = (s_selectedField == 4) ? COLOR_ORANGE : COLOR_CARD_BG;
    vita2d_draw_rectangle(510, y, 400, 50, pinBtnColor);
    vita2d_pgf_draw_text(font, 620, y + 35, COLOR_WHITE, 1.0f, "Login with PIN Code");
    
    // Error message
    if (!m_lastError.empty()) {
        vita2d_pgf_draw_text(font, 50, 480, COLOR_ERROR, 0.8f, m_lastError.c_str());
    }
    
    // Controls help
    vita2d_pgf_draw_text(font, 50, 520, COLOR_GRAY, 0.6f, 
                         "D-Pad: Navigate  X: Select/Edit  O: Back  Start: Exit");
}

void App::drawPinAuthScreen(vita2d_pgf* font) {
    // Title
    vita2d_pgf_draw_text(font, 350, 80, COLOR_ORANGE, 1.2f, "PIN Authentication");
    
    // Instructions
    vita2d_pgf_draw_text(font, 200, 150, COLOR_WHITE, 0.9f, "Go to plex.tv/link and enter this code:");
    
    // Large PIN display
    vita2d_draw_rectangle(300, 200, 360, 120, COLOR_CARD_BG);
    
    if (!m_pinAuth.code.empty() && m_pinAuth.code.length() >= 4) {
        // Draw each character of the PIN with spacing
        int x = 340;
        for (size_t i = 0; i < m_pinAuth.code.length() && i < 8; i++) {
            char str[2] = {m_pinAuth.code[i], '\0'};
            vita2d_pgf_draw_text(font, x, 280, COLOR_ORANGE, 2.5f, str);
            x += 80;
        }
    } else {
        vita2d_pgf_draw_text(font, 400, 270, COLOR_GRAY, 1.5f, "----");
    }
    
    // Status
    if (m_pinAuth.expired) {
        vita2d_pgf_draw_text(font, 320, 360, COLOR_ERROR, 0.9f, "PIN expired. Press X to get a new one.");
    } else if (!m_authToken.empty()) {
        vita2d_pgf_draw_text(font, 320, 360, COLOR_SUCCESS, 0.9f, "Authorized! Press Triangle to connect.");
    } else {
        vita2d_pgf_draw_text(font, 320, 360, COLOR_GRAY, 0.8f, "Waiting for authorization...");
    }
    
    // Server URL entry
    vita2d_pgf_draw_text(font, 50, 420, COLOR_WHITE, 0.8f, "Server URL (required):");
    vita2d_draw_rectangle(50, 440, 860, 40, COLOR_CARD_BG);
    if (s_serverUrl[0]) {
        vita2d_pgf_draw_text(font, 60, 468, COLOR_WHITE, 0.8f, s_serverUrl);
    } else {
        vita2d_pgf_draw_text(font, 60, 468, COLOR_GRAY, 0.8f, "http://192.168.1.x:32400");
    }
    
    // Controls
    vita2d_pgf_draw_text(font, 50, 520, COLOR_GRAY, 0.6f, 
                         "X: Edit Server URL / Refresh PIN  O: Back  Triangle: Connect");
}

void App::drawHomeScreen(vita2d_pgf* font) {
    // Header
    vita2d_pgf_draw_text(font, 30, 40, COLOR_ORANGE, 1.0f, "VitaPlex");
    vita2d_pgf_draw_text(font, 150, 40, COLOR_GRAY, 0.7f, m_currentServer.name.c_str());
    
    // Menu options
    int y = 80;
    const char* menuItems[] = {"Libraries", "Search", "Continue Watching", "Recently Added", "Live TV", "Settings", "Logout"};
    int menuCount = 7;
    
    for (int i = 0; i < menuCount; i++) {
        unsigned int color = (m_selectedItem == i) ? COLOR_SELECTED : COLOR_CARD_BG;
        vita2d_draw_rectangle(30, y, 900, 45, color);
        
        // Add icons for certain items
        const char* icon = "";
        if (i == 4) icon = "[TV]";  // Live TV
        
        if (icon[0]) {
            vita2d_pgf_draw_text(font, 50, y + 32, COLOR_ORANGE, 0.8f, icon);
            vita2d_pgf_draw_text(font, 100, y + 32, COLOR_WHITE, 0.85f, menuItems[i]);
        } else {
            vita2d_pgf_draw_text(font, 50, y + 32, COLOR_WHITE, 0.85f, menuItems[i]);
        }
        y += 52;
    }
    
    // Hubs preview (if loaded)
    if (!m_hubs.empty() && m_hubIndex < (int)m_hubs.size()) {
        y = 460;
        vita2d_pgf_draw_text(font, 30, y, COLOR_ORANGE, 0.8f, m_hubs[m_hubIndex].title.c_str());
        
        // Show item count
        char countStr[32];
        snprintf(countStr, sizeof(countStr), "(%d items)", (int)m_hubs[m_hubIndex].items.size());
        vita2d_pgf_draw_text(font, 300, y, COLOR_GRAY, 0.6f, countStr);
    }
    
    // Controls
    vita2d_pgf_draw_text(font, 30, 520, COLOR_GRAY, 0.6f, 
                         "D-Pad: Navigate  X: Select  O: Back  Start: Exit");
}

void App::drawLibraryScreen(vita2d_pgf* font) {
    // Header
    vita2d_pgf_draw_text(font, 30, 40, COLOR_ORANGE, 1.0f, "Libraries");
    
    if (m_librarySections.empty()) {
        vita2d_pgf_draw_text(font, 30, 100, COLOR_GRAY, 0.8f, "Loading libraries...");
        return;
    }
    
    int y = 80;
    int visibleItems = 7;
    int startIdx = m_scrollOffset;
    int endIdx = std::min(startIdx + visibleItems, (int)m_librarySections.size());
    
    for (int i = startIdx; i < endIdx; i++) {
        const auto& section = m_librarySections[i];
        unsigned int color = (m_selectedItem == i) ? COLOR_SELECTED : COLOR_CARD_BG;
        vita2d_draw_rectangle(30, y, 900, 55, color);
        
        // Library icon based on type
        const char* icon = "[?]";
        if (section.type == "movie") icon = "[M]";
        else if (section.type == "show") icon = "[T]";
        else if (section.type == "artist") icon = "[A]";
        else if (section.type == "photo") icon = "[P]";
        
        vita2d_pgf_draw_text(font, 50, y + 38, COLOR_ORANGE, 0.9f, icon);
        vita2d_pgf_draw_text(font, 100, y + 38, COLOR_WHITE, 0.9f, section.title.c_str());
        vita2d_pgf_draw_text(font, 700, y + 38, COLOR_GRAY, 0.7f, section.type.c_str());
        
        y += 60;
    }
    
    // Scroll indicator
    if ((int)m_librarySections.size() > visibleItems) {
        char scrollStr[32];
        snprintf(scrollStr, sizeof(scrollStr), "%d / %d", m_selectedItem + 1, (int)m_librarySections.size());
        vita2d_pgf_draw_text(font, 850, 40, COLOR_GRAY, 0.7f, scrollStr);
    }
    
    // Controls
    vita2d_pgf_draw_text(font, 30, 520, COLOR_GRAY, 0.6f, 
                         "D-Pad: Navigate  X: Open Library  O: Back");
}

void App::drawBrowseScreen(vita2d_pgf* font) {
    // Header with breadcrumb navigation
    std::string header = "Browse";
    for (const auto& section : m_librarySections) {
        if (section.key == m_currentSectionKey) {
            header = section.title;
            break;
        }
    }
    
    // Add navigation breadcrumb
    if (!m_navStack.empty()) {
        header = "";
        for (size_t i = 0; i < m_navStack.size(); i++) {
            if (i > 0) header += " > ";
            // Truncate long titles
            std::string title = m_navStack[i].title;
            if (title.length() > 15) {
                title = title.substr(0, 12) + "...";
            }
            header += title;
        }
    }
    vita2d_pgf_draw_text(font, 30, 35, COLOR_ORANGE, 0.9f, header.c_str());
    
    if (m_mediaItems.empty()) {
        vita2d_pgf_draw_text(font, 30, 100, COLOR_GRAY, 0.8f, "No items found.");
        return;
    }
    
    int y = 55;
    int visibleItems = 5;
    int itemHeight = 95;  // Taller items for poster display
    int startIdx = m_scrollOffset;
    int endIdx = std::min(startIdx + visibleItems, (int)m_mediaItems.size());
    
    for (int i = startIdx; i < endIdx; i++) {
        const auto& item = m_mediaItems[i];
        unsigned int color = (m_selectedItem == i) ? COLOR_SELECTED : COLOR_CARD_BG;
        vita2d_draw_rectangle(30, y, 900, itemHeight - 5, color);
        
        // Draw thumbnail/poster if loaded
        int textX = 50;
        if (item.thumbTexture) {
            int thumbW = vita2d_texture_get_width(item.thumbTexture);
            int thumbH = vita2d_texture_get_height(item.thumbTexture);
            float scale = std::min(60.0f / thumbW, 80.0f / thumbH);
            vita2d_draw_texture_scale(item.thumbTexture, 40, y + 5, scale, scale);
            textX = 115;
        } else {
            // Placeholder for poster
            vita2d_draw_rectangle(40, y + 5, 60, 80, COLOR_DARK_GRAY);
            
            // Type indicator
            const char* typeIcon = "?";
            if (item.mediaType == MediaType::MOVIE) typeIcon = "M";
            else if (item.mediaType == MediaType::SHOW) typeIcon = "TV";
            else if (item.mediaType == MediaType::SEASON) typeIcon = "S";
            else if (item.mediaType == MediaType::EPISODE) typeIcon = "E";
            else if (item.mediaType == MediaType::MUSIC_ARTIST) typeIcon = "A";
            else if (item.mediaType == MediaType::MUSIC_ALBUM) typeIcon = "AL";
            else if (item.mediaType == MediaType::MUSIC_TRACK) typeIcon = "T";
            vita2d_pgf_draw_text(font, 55, y + 50, COLOR_GRAY, 0.7f, typeIcon);
            textX = 115;
        }
        
        // Title (potentially with episode number)
        std::string displayTitle = item.title;
        if (item.mediaType == MediaType::EPISODE && item.index > 0) {
            char epTitle[256];
            snprintf(epTitle, sizeof(epTitle), "%d. %s", item.index, item.title.c_str());
            displayTitle = epTitle;
        } else if (item.mediaType == MediaType::SEASON && item.index > 0) {
            char seasonTitle[64];
            snprintf(seasonTitle, sizeof(seasonTitle), "Season %d", item.index);
            displayTitle = seasonTitle;
        }
        
        // Truncate long titles
        if (displayTitle.length() > 45) {
            displayTitle = displayTitle.substr(0, 42) + "...";
        }
        vita2d_pgf_draw_text(font, textX, y + 28, COLOR_WHITE, 0.85f, displayTitle.c_str());
        
        // Info line 1: Year/Type or Parent info
        char infoStr[128];
        if (item.mediaType == MediaType::EPISODE) {
            if (!item.grandparentTitle.empty()) {
                snprintf(infoStr, sizeof(infoStr), "%s - S%02dE%02d", 
                         item.grandparentTitle.c_str(), item.parentIndex, item.index);
            } else {
                snprintf(infoStr, sizeof(infoStr), "S%02dE%02d", item.parentIndex, item.index);
            }
        } else if (item.mediaType == MediaType::SEASON) {
            snprintf(infoStr, sizeof(infoStr), "%d episodes", item.leafCount);
        } else if (item.mediaType == MediaType::SHOW) {
            snprintf(infoStr, sizeof(infoStr), "%d | TV Show", item.year);
        } else if (item.year > 0) {
            snprintf(infoStr, sizeof(infoStr), "%d | %s", item.year, item.type.c_str());
        } else {
            snprintf(infoStr, sizeof(infoStr), "%s", item.type.c_str());
        }
        vita2d_pgf_draw_text(font, textX, y + 52, COLOR_GRAY, 0.65f, infoStr);
        
        // Info line 2: Duration or track count
        if (item.duration > 0) {
            int mins = item.duration / 60000;
            char durStr[16];
            snprintf(durStr, sizeof(durStr), "%d min", mins);
            vita2d_pgf_draw_text(font, textX, y + 72, COLOR_GRAY, 0.6f, durStr);
        } else if (item.leafCount > 0 && item.mediaType == MediaType::MUSIC_ALBUM) {
            char trackStr[32];
            snprintf(trackStr, sizeof(trackStr), "%d tracks", item.leafCount);
            vita2d_pgf_draw_text(font, textX, y + 72, COLOR_GRAY, 0.6f, trackStr);
        }
        
        // Right side: Watch status or child indicator
        if (item.mediaType == MediaType::SHOW || item.mediaType == MediaType::SEASON || 
            item.mediaType == MediaType::MUSIC_ARTIST || item.mediaType == MediaType::MUSIC_ALBUM) {
            // Has children - show count
            vita2d_pgf_draw_text(font, 880, y + 45, COLOR_GRAY, 0.7f, ">");
            if (item.viewedLeafCount > 0 && item.leafCount > 0) {
                char progStr[32];
                snprintf(progStr, sizeof(progStr), "%d/%d", item.viewedLeafCount, item.leafCount);
                vita2d_pgf_draw_text(font, 820, y + 45, COLOR_GRAY, 0.6f, progStr);
            }
        } else {
            // Playable item - show watch status
            if (item.watched) {
                vita2d_pgf_draw_text(font, 870, y + 45, COLOR_SUCCESS, 0.7f, "[W]");
            } else if (item.viewOffset > 0) {
                vita2d_pgf_draw_text(font, 870, y + 45, COLOR_ORANGE, 0.7f, "[>]");
            }
        }
        
        y += itemHeight;
    }
    
    // Scroll indicator
    char scrollStr[32];
    snprintf(scrollStr, sizeof(scrollStr), "%d / %d", m_selectedItem + 1, (int)m_mediaItems.size());
    vita2d_pgf_draw_text(font, 850, 35, COLOR_GRAY, 0.7f, scrollStr);
    
    // Controls - context sensitive
    const char* selectAction = "View Details";
    if (!m_mediaItems.empty() && m_selectedItem < (int)m_mediaItems.size()) {
        MediaType type = m_mediaItems[m_selectedItem].mediaType;
        if (type == MediaType::SHOW || type == MediaType::MUSIC_ARTIST) {
            selectAction = "View Seasons/Albums";
        } else if (type == MediaType::SEASON || type == MediaType::MUSIC_ALBUM) {
            selectAction = "View Episodes/Tracks";
        }
    }
    
    char controlStr[128];
    snprintf(controlStr, sizeof(controlStr), "X: %s  Triangle: Search  O: Back", selectAction);
    vita2d_pgf_draw_text(font, 30, 520, COLOR_GRAY, 0.6f, controlStr);
}

void App::drawSearchScreen(vita2d_pgf* font) {
    // Header
    vita2d_pgf_draw_text(font, 30, 40, COLOR_ORANGE, 1.0f, "Search");
    
    // Search box
    vita2d_draw_rectangle(30, 60, 900, 45, COLOR_CARD_BG);
    vita2d_pgf_draw_text(font, 50, 92, COLOR_WHITE, 0.9f, 
                         s_searchText[0] ? s_searchText : "Press X to enter search term...");
    
    // Results
    if (m_searchResults.empty()) {
        if (!m_searchQuery.empty()) {
            vita2d_pgf_draw_text(font, 30, 140, COLOR_GRAY, 0.8f, "No results found.");
        }
    } else {
        int y = 120;
        int visibleItems = 6;
        int startIdx = m_scrollOffset;
        int endIdx = std::min(startIdx + visibleItems, (int)m_searchResults.size());
        
        for (int i = startIdx; i < endIdx; i++) {
            const auto& item = m_searchResults[i];
            unsigned int color = (m_selectedItem == i) ? COLOR_SELECTED : COLOR_CARD_BG;
            vita2d_draw_rectangle(30, y, 900, 55, color);
            
            vita2d_pgf_draw_text(font, 50, y + 25, COLOR_WHITE, 0.85f, item.title.c_str());
            
            char infoStr[64];
            snprintf(infoStr, sizeof(infoStr), "%s | %d", item.type.c_str(), item.year);
            vita2d_pgf_draw_text(font, 50, y + 45, COLOR_GRAY, 0.65f, infoStr);
            
            y += 60;
        }
        
        // Result count
        char countStr[32];
        snprintf(countStr, sizeof(countStr), "%d results", (int)m_searchResults.size());
        vita2d_pgf_draw_text(font, 800, 40, COLOR_GRAY, 0.7f, countStr);
    }
    
    // Controls
    vita2d_pgf_draw_text(font, 30, 520, COLOR_GRAY, 0.6f, 
                         "X: Search/Select  D-Pad: Navigate  O: Back");
}

void App::drawMediaDetailScreen(vita2d_pgf* font) {
    // Title
    vita2d_pgf_draw_text(font, 30, 40, COLOR_ORANGE, 1.0f, m_currentMedia.title.c_str());
    
    // Year and content rating
    int y = 70;
    char infoLine[128];
    snprintf(infoLine, sizeof(infoLine), "%d | %s | %s", 
             m_currentMedia.year, 
             m_currentMedia.contentRating.c_str(),
             m_currentMedia.studio.c_str());
    vita2d_pgf_draw_text(font, 30, y, COLOR_GRAY, 0.75f, infoLine);
    y += 30;
    
    // Duration and rating
    int mins = m_currentMedia.duration / 60000;
    snprintf(infoLine, sizeof(infoLine), "%d min | Rating: %.1f", mins, m_currentMedia.rating);
    vita2d_pgf_draw_text(font, 30, y, COLOR_GRAY, 0.75f, infoLine);
    y += 30;
    
    // Video info
    if (m_currentMedia.videoWidth > 0) {
        snprintf(infoLine, sizeof(infoLine), "%dx%d | %s | %s",
                 m_currentMedia.videoWidth, m_currentMedia.videoHeight,
                 m_currentMedia.videoCodec.c_str(), m_currentMedia.audioCodec.c_str());
        vita2d_pgf_draw_text(font, 30, y, COLOR_GRAY, 0.7f, infoLine);
        y += 25;
    }
    
    // Summary (wrapped)
    y += 10;
    vita2d_draw_rectangle(30, y, 900, 250, COLOR_CARD_BG);
    y += 20;
    
    std::string summary = m_currentMedia.summary;
    int lineWidth = 85;
    int lineY = y;
    while (!summary.empty() && lineY < y + 220) {
        std::string line = summary.substr(0, lineWidth);
        size_t lastSpace = line.rfind(' ');
        if (lastSpace != std::string::npos && summary.length() > (size_t)lineWidth) {
            line = summary.substr(0, lastSpace);
            summary = summary.substr(lastSpace + 1);
        } else {
            summary = summary.substr(std::min((size_t)lineWidth, summary.length()));
        }
        vita2d_pgf_draw_text(font, 40, lineY + 15, COLOR_WHITE, 0.7f, line.c_str());
        lineY += 22;
    }
    
    // Action buttons
    y = 420;
    
    // Play button
    unsigned int playColor = (m_selectedItem == 0) ? COLOR_ORANGE : COLOR_CARD_BG;
    vita2d_draw_rectangle(30, y, 200, 45, playColor);
    vita2d_pgf_draw_text(font, 90, y + 32, COLOR_WHITE, 0.9f, "Play");
    
    // Resume button (if applicable)
    if (m_currentMedia.viewOffset > 0) {
        unsigned int resumeColor = (m_selectedItem == 1) ? COLOR_ORANGE : COLOR_CARD_BG;
        vita2d_draw_rectangle(250, y, 200, 45, resumeColor);
        int resumeMins = m_currentMedia.viewOffset / 60000;
        char resumeStr[32];
        snprintf(resumeStr, sizeof(resumeStr), "Resume (%dm)", resumeMins);
        vita2d_pgf_draw_text(font, 280, y + 32, COLOR_WHITE, 0.8f, resumeStr);
    }
    
    // Mark watched/unwatched
    int watchBtnIdx = (m_currentMedia.viewOffset > 0) ? 2 : 1;
    unsigned int watchColor = (m_selectedItem == watchBtnIdx) ? COLOR_ORANGE : COLOR_CARD_BG;
    vita2d_draw_rectangle(470, y, 250, 45, watchColor);
    vita2d_pgf_draw_text(font, 500, y + 32, COLOR_WHITE, 0.8f, 
                         m_currentMedia.watched ? "Mark Unwatched" : "Mark Watched");
    
    // Controls
    vita2d_pgf_draw_text(font, 30, 520, COLOR_GRAY, 0.6f, 
                         "D-Pad: Navigate  X: Select  O: Back");
}

void App::drawSettingsScreen(vita2d_pgf* font) {
    // Header
    vita2d_pgf_draw_text(font, 30, 40, COLOR_ORANGE, 1.0f, "Settings");
    
    int y = 80;
    
    // Account section
    vita2d_pgf_draw_text(font, 30, y, COLOR_WHITE, 0.85f, "Account");
    y += 25;
    
    vita2d_draw_rectangle(30, y, 900, 80, COLOR_CARD_BG);
    
    // Username
    vita2d_pgf_draw_text(font, 50, y + 25, COLOR_GRAY, 0.7f, "User:");
    if (!m_settings.username.empty()) {
        vita2d_pgf_draw_text(font, 130, y + 25, COLOR_WHITE, 0.7f, m_settings.username.c_str());
    } else if (!m_settings.email.empty()) {
        vita2d_pgf_draw_text(font, 130, y + 25, COLOR_WHITE, 0.7f, m_settings.email.c_str());
    } else {
        vita2d_pgf_draw_text(font, 130, y + 25, COLOR_GRAY, 0.7f, "Logged in via PIN");
    }
    
    // Server
    vita2d_pgf_draw_text(font, 50, y + 55, COLOR_GRAY, 0.7f, "Server:");
    vita2d_pgf_draw_text(font, 130, y + 55, COLOR_WHITE, 0.7f, m_currentServer.name.c_str());
    vita2d_pgf_draw_text(font, 450, y + 55, COLOR_GRAY, 0.6f, m_currentServer.address.c_str());
    y += 95;
    
    // Video settings section
    y += 10;
    vita2d_pgf_draw_text(font, 30, y, COLOR_WHITE, 0.85f, "Video Playback");
    y += 25;
    
    // Video quality
    unsigned int qualityColor = (m_selectedItem == 0) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(30, y, 900, 50, qualityColor);
    vita2d_pgf_draw_text(font, 50, y + 32, COLOR_WHITE, 0.75f, "Transcode Quality");
    const char* qualityNames[] = {"Original (Direct)", "1080p (8 Mbps)", "720p (4 Mbps)", "480p (2 Mbps)", "360p (1 Mbps)"};
    int qualityIdx = static_cast<int>(m_settings.videoQuality);
    vita2d_pgf_draw_text(font, 350, y + 32, COLOR_ORANGE, 0.75f, qualityNames[qualityIdx]);
    vita2d_pgf_draw_text(font, 850, y + 32, COLOR_GRAY, 0.6f, "< >");
    y += 55;
    
    // Auto-play
    unsigned int autoplayColor = (m_selectedItem == 1) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(30, y, 900, 50, autoplayColor);
    vita2d_pgf_draw_text(font, 50, y + 32, COLOR_WHITE, 0.75f, "Auto-play next episode");
    vita2d_pgf_draw_text(font, 350, y + 32, m_settings.autoPlay ? COLOR_SUCCESS : COLOR_GRAY, 0.75f,
                         m_settings.autoPlay ? "ON" : "OFF");
    y += 55;
    
    // Subtitles
    unsigned int subsColor = (m_selectedItem == 2) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(30, y, 900, 50, subsColor);
    vita2d_pgf_draw_text(font, 50, y + 32, COLOR_WHITE, 0.75f, "Show subtitles");
    vita2d_pgf_draw_text(font, 350, y + 32, m_settings.showSubtitles ? COLOR_SUCCESS : COLOR_GRAY, 0.75f,
                         m_settings.showSubtitles ? "ON" : "OFF");
    y += 70;
    
    // Debug section
    vita2d_pgf_draw_text(font, 30, y, COLOR_WHITE, 0.85f, "Debug");
    y += 25;
    
    // Debug logging toggle
    unsigned int debugColor = (m_selectedItem == 3) ? COLOR_SELECTED : COLOR_CARD_BG;
    vita2d_draw_rectangle(30, y, 900, 50, debugColor);
    vita2d_pgf_draw_text(font, 50, y + 32, COLOR_WHITE, 0.75f, "Save debug log to file");
    vita2d_pgf_draw_text(font, 350, y + 32, m_settings.enableFileLogging ? COLOR_SUCCESS : COLOR_GRAY, 0.75f,
                         m_settings.enableFileLogging ? "ON" : "OFF");
    if (m_settings.enableFileLogging) {
        vita2d_pgf_draw_text(font, 450, y + 32, COLOR_GRAY, 0.6f, "(ux0:data/VitaPlex/debug.log)");
    }
    y += 70;
    
    // Logout button
    unsigned int logoutColor = (m_selectedItem == 4) ? COLOR_ORANGE : COLOR_CARD_BG;
    vita2d_draw_rectangle(30, y, 200, 50, logoutColor);
    vita2d_pgf_draw_text(font, 85, y + 35, COLOR_WHITE, 0.9f, "Logout");
    
    // Version info
    vita2d_pgf_draw_text(font, 750, y + 35, COLOR_GRAY, 0.6f, "VitaPlex v" VITA_PLEX_VERSION);
    
    // Controls
    vita2d_pgf_draw_text(font, 30, 520, COLOR_GRAY, 0.6f, 
                         "Up/Down: Navigate  Left/Right: Change  X: Toggle  O: Back");
}

void App::drawPlayerScreen(vita2d_pgf* font) {
    // Dark background for player
    vita2d_clear_screen();
    
    MpvPlayer& player = MpvPlayer::getInstance();
    const MpvPlaybackInfo& info = player.getPlaybackInfo();
    
    if (m_state == AppState::PLAYER) {
        // Title bar at top (semi-transparent)
        vita2d_draw_rectangle(0, 0, SCREEN_WIDTH, 60, RGBA8(0, 0, 0, 180));
        vita2d_pgf_draw_text(font, 30, 40, COLOR_WHITE, 0.9f, m_currentMedia.title.c_str());
        
        // Show media type
        bool isAudio = (m_currentMedia.mediaType == MediaType::MUSIC_TRACK || m_currentMedia.type == "track");
        const char* mediaTypeStr = isAudio ? "Audio" : "Video (Audio Only)";
        vita2d_pgf_draw_text(font, SCREEN_WIDTH - 200, 40, COLOR_ORANGE, 0.7f, mediaTypeStr);
        
        // Center status based on player state
        MpvPlayerState state = player.getState();
        int centerY = SCREEN_HEIGHT / 2;
        
        // Show album art placeholder or media info
        vita2d_draw_rectangle(SCREEN_WIDTH/2 - 100, centerY - 120, 200, 200, COLOR_CARD_BG);
        
        // Music note or video icon
        if (isAudio) {
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 20, centerY - 20, COLOR_ORANGE, 2.0f, "♪");
        } else {
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 30, centerY - 30, COLOR_ORANGE, 1.5f, "VIDEO");
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 60, centerY + 20, COLOR_GRAY, 0.6f, "(Audio Only Mode)");
        }
        
        // Status text below the icon
        if (state == MpvPlayerState::LOADING) {
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 50, centerY + 100, COLOR_WHITE, 1.0f, "Loading...");
        } else if (state == MpvPlayerState::BUFFERING) {
            char bufStr[64];
            snprintf(bufStr, sizeof(bufStr), "Buffering... %.0f%%", info.bufferingPercent);
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 80, centerY + 100, COLOR_WHITE, 1.0f, bufStr);
        } else if (state == MpvPlayerState::PAUSED) {
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 40, centerY + 100, COLOR_WHITE, 1.2f, "PAUSED");
        } else if (state == MpvPlayerState::PLAYING) {
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 40, centerY + 100, COLOR_SUCCESS, 1.0f, "Playing");
        } else if (state == MpvPlayerState::ERROR) {
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 30, centerY + 100, COLOR_ERROR, 1.0f, "Error");
            vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 150, centerY + 130, COLOR_GRAY, 0.7f, 
                                 player.getErrorMessage().c_str());
        }
        
        // Progress bar at bottom (semi-transparent)
        vita2d_draw_rectangle(0, SCREEN_HEIGHT - 80, SCREEN_WIDTH, 80, RGBA8(0, 0, 0, 180));
        
        // Progress bar background
        vita2d_draw_rectangle(30, SCREEN_HEIGHT - 50, SCREEN_WIDTH - 60, 12, COLOR_DARK_GRAY);
        
        // Progress bar fill
        double duration = info.duration > 0 ? info.duration : m_currentMedia.duration / 1000.0;
        double position = info.position;
        float progress = 0.0f;
        if (duration > 0) {
            progress = (float)(position / duration);
            if (progress > 1.0f) progress = 1.0f;
        }
        int fillWidth = (int)((SCREEN_WIDTH - 60) * progress);
        if (fillWidth > 0) {
            vita2d_draw_rectangle(30, SCREEN_HEIGHT - 50, fillWidth, 12, COLOR_ORANGE);
        }
        
        // Time display
        int posMin = (int)(position / 60);
        int posSec = (int)((int)position % 60);
        int durMin = (int)(duration / 60);
        int durSec = (int)((int)duration % 60);
        
        char timeStr[64];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d", posMin, posSec, durMin, durSec);
        vita2d_pgf_draw_text(font, SCREEN_WIDTH / 2 - 60, SCREEN_HEIGHT - 20, COLOR_WHITE, 0.8f, timeStr);
        
        // Volume indicator
        char volStr[32];
        if (info.muted) {
            snprintf(volStr, sizeof(volStr), "MUTE");
        } else {
            snprintf(volStr, sizeof(volStr), "Vol: %d%%", info.volume);
        }
        vita2d_pgf_draw_text(font, SCREEN_WIDTH - 130, SCREEN_HEIGHT - 20, COLOR_GRAY, 0.6f, volStr);
        
        // Controls hint
        vita2d_pgf_draw_text(font, 30, SCREEN_HEIGHT - 20, COLOR_GRAY, 0.5f, 
                             "L/R:10s  Left/Right:30s  Up/Down:Vol  X:Pause  O:Stop");
    }
}

void App::drawLiveTVScreen(vita2d_pgf* font) {
    // Background
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_DARK_BG);
    
    // Header
    vita2d_draw_rectangle(0, 0, SCREEN_WIDTH, 60, COLOR_CARD_BG);
    vita2d_pgf_draw_text(font, 30, 40, COLOR_ORANGE, 1.0f, "Live TV");
    vita2d_pgf_draw_text(font, SCREEN_WIDTH - 200, 40, COLOR_GRAY, 0.7f, m_currentServer.name.c_str());
    
    if (m_liveTVChannels.empty()) {
        vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 100, SCREEN_HEIGHT/2, COLOR_GRAY, 0.9f, 
                             "No Live TV channels found");
        vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 150, SCREEN_HEIGHT/2 + 40, COLOR_GRAY, 0.7f, 
                             "Live TV DVR is required on Plex Pass");
        
        // Controls
        vita2d_draw_rectangle(0, SCREEN_HEIGHT - 50, SCREEN_WIDTH, 50, COLOR_CARD_BG);
        vita2d_pgf_draw_text(font, 30, SCREEN_HEIGHT - 20, COLOR_GRAY, 0.6f, "O: Back    Triangle: Refresh");
        return;
    }
    
    // Channel list
    int itemHeight = 75;
    int visibleItems = 6;
    int startY = 70;
    
    for (int i = 0; i < visibleItems && (m_scrollOffset + i) < (int)m_liveTVChannels.size(); i++) {
        int idx = m_scrollOffset + i;
        const LiveTVChannel& channel = m_liveTVChannels[idx];
        int y = startY + i * itemHeight;
        
        // Selection highlight
        if (idx == m_selectedItem) {
            vita2d_draw_rectangle(10, y, SCREEN_WIDTH - 20, itemHeight - 5, COLOR_SELECTED);
        } else {
            vita2d_draw_rectangle(10, y, SCREEN_WIDTH - 20, itemHeight - 5, COLOR_CARD_BG);
        }
        
        // Channel number
        char chNumStr[16];
        snprintf(chNumStr, sizeof(chNumStr), "%d", channel.channelNumber);
        vita2d_pgf_draw_text(font, 30, y + 25, COLOR_ORANGE, 0.9f, chNumStr);
        
        // Channel name/callsign
        vita2d_pgf_draw_text(font, 100, y + 25, COLOR_WHITE, 0.8f, channel.title.c_str());
        if (!channel.callSign.empty()) {
            vita2d_pgf_draw_text(font, 100, y + 50, COLOR_GRAY, 0.6f, channel.callSign.c_str());
        }
        
        // Current program
        if (!channel.currentProgram.empty()) {
            vita2d_pgf_draw_text(font, 350, y + 25, COLOR_WHITE, 0.7f, channel.currentProgram.c_str());
        }
        
        // Next program
        if (!channel.nextProgram.empty()) {
            char nextStr[128];
            snprintf(nextStr, sizeof(nextStr), "Next: %s", channel.nextProgram.c_str());
            vita2d_pgf_draw_text(font, 350, y + 50, COLOR_GRAY, 0.6f, nextStr);
        }
    }
    
    // Scroll indicator
    if (m_liveTVChannels.size() > (size_t)visibleItems) {
        int totalHeight = SCREEN_HEIGHT - 130;
        int thumbHeight = (visibleItems * totalHeight) / m_liveTVChannels.size();
        if (thumbHeight < 20) thumbHeight = 20;
        int thumbY = 70 + (m_scrollOffset * totalHeight) / m_liveTVChannels.size();
        vita2d_draw_rectangle(SCREEN_WIDTH - 8, thumbY, 5, thumbHeight, COLOR_ORANGE);
    }
    
    // Controls
    vita2d_draw_rectangle(0, SCREEN_HEIGHT - 50, SCREEN_WIDTH, 50, COLOR_CARD_BG);
    vita2d_pgf_draw_text(font, 30, SCREEN_HEIGHT - 20, COLOR_GRAY, 0.6f, 
                         "X: Watch    O: Back    Triangle: Refresh");
}

bool App::startPlayback(bool resume) {
    debugLog("Starting playback for: %s (type: %s, resume=%d)\n", 
                  m_currentMedia.ratingKey.c_str(), m_currentMedia.type.c_str(), resume);
    
    // Check if this is a photo - don't try to play photos as video
    if (m_currentMedia.mediaType == MediaType::PHOTO || m_currentMedia.type == "photo") {
        debugLog("Photo detected, showing image instead of playing\n");
        return showPhoto();
    }
    
    // Check if this is music - use audio-specific endpoint
    bool isAudio = (m_currentMedia.mediaType == MediaType::MUSIC_TRACK || m_currentMedia.type == "track");
    if (isAudio) {
        debugLog("Audio track detected, using audio transcode endpoint\n");
    }
    
    // Build transcode URL for Vita-compatible playback
    uint64_t offset = resume ? m_currentMedia.viewOffset : 0;
    
    // Get quality setting
    int videoBitrate = 4000;  // Default 720p
    int maxWidth = 1280;
    int maxHeight = 720;
    
    switch (m_settings.videoQuality) {
        case VideoQuality::ORIGINAL:
            videoBitrate = 0;  // Try direct play
            maxWidth = 1920;
            maxHeight = 1080;
            break;
        case VideoQuality::QUALITY_1080P:
            videoBitrate = 8000;
            maxWidth = 1920;
            maxHeight = 1080;
            break;
        case VideoQuality::QUALITY_720P:
            videoBitrate = 4000;
            maxWidth = 1280;
            maxHeight = 720;
            break;
        case VideoQuality::QUALITY_480P:
            videoBitrate = 2000;
            maxWidth = 854;
            maxHeight = 480;
            break;
        case VideoQuality::QUALITY_360P:
            videoBitrate = 1000;
            maxWidth = 640;
            maxHeight = 360;
            break;
    }
    
    // URL-encode the path parameter (required by Plex API)
    std::string pathParam = "/library/metadata/" + m_currentMedia.ratingKey;
    std::string encodedPath = HttpClient::urlEncode(pathParam);
    
    // Build transcode URL
    std::string transcodeUrl = m_currentServer.address;
    
    if (isAudio) {
        // Audio-specific transcode endpoint
        transcodeUrl += "/music/:/transcode/universal/start.mp3?";
        transcodeUrl += "path=" + encodedPath;
        transcodeUrl += "&mediaIndex=0&partIndex=0";
        transcodeUrl += "&protocol=http";
        transcodeUrl += "&directPlay=0&directStream=1";
        // Audio codec - MP3 or AAC for Vita compatibility
        transcodeUrl += "&audioCodec=mp3&audioBitrate=320";
    } else {
        // Video transcode endpoint
        transcodeUrl += "/video/:/transcode/universal/start.mp4?";
        transcodeUrl += "path=" + encodedPath;
        transcodeUrl += "&mediaIndex=0&partIndex=0";
        transcodeUrl += "&protocol=http";
        transcodeUrl += "&fastSeek=1";
        transcodeUrl += "&directPlay=0&directStream=1";  // Allow direct stream if compatible
        
        // Video settings - H.264 baseline for Vita
        if (videoBitrate > 0) {
            char bitrateStr[32];
            snprintf(bitrateStr, sizeof(bitrateStr), "&videoBitrate=%d", videoBitrate);
            transcodeUrl += bitrateStr;
        }
        transcodeUrl += "&videoCodec=h264";
        
        char resStr[64];
        snprintf(resStr, sizeof(resStr), "&maxWidth=%d&maxHeight=%d", maxWidth, maxHeight);
        transcodeUrl += resStr;
        
        // Audio - AAC stereo
        transcodeUrl += "&audioCodec=aac&audioChannels=2";
    }
    
    // Offset for resume
    if (offset > 0) {
        char offsetStr[32];
        snprintf(offsetStr, sizeof(offsetStr), "&offset=%llu", (unsigned long long)offset);
        transcodeUrl += offsetStr;
    }
    
    // Add token and Plex client identification parameters (URL-encode values with spaces)
    transcodeUrl += "&X-Plex-Token=" + m_authToken;
    transcodeUrl += "&X-Plex-Client-Identifier=" PLEX_CLIENT_ID;
    transcodeUrl += "&X-Plex-Product=" PLEX_CLIENT_NAME;
    transcodeUrl += "&X-Plex-Version=" PLEX_CLIENT_VERSION;
    transcodeUrl += "&X-Plex-Platform=PlayStation%20Vita";  // URL-encoded space
    transcodeUrl += "&X-Plex-Device=PS%20Vita";  // URL-encoded space
    
    // Generate a session ID for this transcode request
    char sessionId[32];
    snprintf(sessionId, sizeof(sessionId), "&session=vita%llu", (unsigned long long)sceKernelGetProcessTimeWide());
    transcodeUrl += sessionId;
    
    debugLog("Transcode URL: %s\n", transcodeUrl.c_str());
    
    // Store URL for display
    m_currentMedia.streamUrl = transcodeUrl;
    
    // Debug: Log what we're about to play
    debugLog("Final playback URL: %s\n", transcodeUrl.c_str());
    
    // Set playback state
    m_isPlaying = true;
    m_playPosition = offset;
    setState(AppState::PLAYER);
    
    // Try to initialize mpv player
    // Note: mpv-vita may not be fully functional in all emulator/hw configurations
    // We'll set up the player state regardless for future compatibility
    MpvPlayer& player = MpvPlayer::getInstance();
    
    // Only attempt mpv if it's available
    #ifdef USE_MPV_PLAYER
    if (!player.isInitialized()) {
        debugLog("Attempting to initialize mpv player...\n");
        if (!player.init()) {
            debugLog("MPV init failed, falling back to display mode\n");
            // Don't fail - just show player UI without actual playback
            // User can still see the URL and try on real hardware
        }
    }
    
    if (player.isInitialized()) {
        // Load the transcode URL - NOT the direct URL
        debugLog("Loading into MPV: %s\n", transcodeUrl.c_str());
        if (!player.loadUrl(transcodeUrl, m_currentMedia.title)) {
            debugLog("MPV loadUrl failed\n");
            // Continue anyway to show player UI
        }
    }
    #else
    // MPV not available, just show player UI
    debugLog("MPV player not compiled in, showing URL only\n");
    (void)player;  // Suppress unused warning
    #endif
    
    debugLog("Player state set up successfully\n");
    return true;
}

bool App::showPhoto() {
    debugLog("Showing photo: %s\n", m_currentMedia.ratingKey.c_str());
    
    // Build photo URL
    std::string photoUrl = m_currentServer.address;
    photoUrl += "/photo/:/transcode?url=" + m_currentMedia.thumb;
    photoUrl += "&width=960&height=544&minSize=1";  // Vita screen size
    photoUrl += "&X-Plex-Token=" + m_authToken;
    
    // Store URL
    m_currentMedia.streamUrl = photoUrl;
    
    // Try to load the photo
    HttpClient client;
    HttpRequest req;
    req.url = photoUrl;
    req.method = "GET";
    req.headers["Accept"] = "image/jpeg, image/png";
    
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200 && !resp.body.empty()) {
        // Free any existing photo texture
        if (m_currentMedia.thumbTexture) {
            vita2d_wait_rendering_done();
            vita2d_free_texture(m_currentMedia.thumbTexture);
            m_currentMedia.thumbTexture = nullptr;
        }
        
        // Load the photo
        m_currentMedia.thumbTexture = vita2d_load_PNG_buffer((const unsigned char*)resp.body.data());
        if (!m_currentMedia.thumbTexture) {
            m_currentMedia.thumbTexture = vita2d_load_JPEG_buffer((const unsigned char*)resp.body.data(), resp.body.size());
        }
        
        if (m_currentMedia.thumbTexture) {
            debugLog("Photo loaded successfully\n");
            setState(AppState::PHOTO_VIEW);
            return true;
        }
    }
    
    setError("Failed to load photo");
    return false;
}

void App::stopPlayback() {
    debugLog("Stopping playback at position %llu\n", (unsigned long long)m_playPosition);
    
    #ifdef USE_MPV_PLAYER
    // Stop mpv player
    MpvPlayer& player = MpvPlayer::getInstance();
    if (player.isInitialized()) {
        // Get final position before stopping
        m_playPosition = (uint64_t)(player.getPosition() * 1000);  // Convert to ms
        player.stop();
    }
    #endif
    
    // Update progress on server
    if (m_playPosition > 0) {
        updatePlayProgress(m_currentMedia.ratingKey, (int)m_playPosition);
    }
    
    m_isPlaying = false;
    m_playPosition = 0;
    setState(AppState::MEDIA_DETAIL);
}

// ============================================================================
// Input Handling
// ============================================================================

void App::handleLoginInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    // Navigate fields
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        s_selectedField = (s_selectedField > 0) ? s_selectedField - 1 : 4;
        debugLog("Login: Selected field %d\n", s_selectedField);
    }
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        s_selectedField = (s_selectedField < 4) ? s_selectedField + 1 : 0;
        debugLog("Login: Selected field %d\n", s_selectedField);
    }
    if ((ctrl->buttons & SCE_CTRL_LEFT) && !(oldCtrl->buttons & SCE_CTRL_LEFT)) {
        if (s_selectedField >= 3) {
            s_selectedField = (s_selectedField == 3) ? 4 : 3;
        }
    }
    if ((ctrl->buttons & SCE_CTRL_RIGHT) && !(oldCtrl->buttons & SCE_CTRL_RIGHT)) {
        if (s_selectedField >= 3) {
            s_selectedField = (s_selectedField == 3) ? 4 : 3;
        }
    }
    
    // Select/Edit with X button
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        debugLog("Login: X pressed on field %d\n", s_selectedField);
        
        if (s_selectedField == 0) {
            debugLog("Login: Opening IME for Server URL\n");
            initImeDialog("Server URL", s_serverUrl, 255);
        } else if (s_selectedField == 1) {
            debugLog("Login: Opening IME for Username\n");
            initImeDialog("Username", s_username, 127);
        } else if (s_selectedField == 2) {
            debugLog("Login: Opening IME for Password\n");
            initImeDialog("Password", s_password, 127, true);
        } else if (s_selectedField == 3) {
            // Login with credentials
            if (s_serverUrl[0] && s_username[0] && s_password[0]) {
                m_lastError.clear();
                if (login(s_username, s_password)) {
                    if (connectToServer(s_serverUrl)) {
                        if (fetchLibrarySections()) {
                            fetchHubs();
                            setState(AppState::HOME);
                        }
                    }
                }
            } else {
                setError("Please fill in all fields");
            }
        } else if (s_selectedField == 4) {
            // Switch to PIN auth
            m_lastError.clear();
            if (requestPin()) {
                setState(AppState::PIN_AUTH);
            }
        }
    }
    
    // Exit
    if ((ctrl->buttons & SCE_CTRL_START) && !(oldCtrl->buttons & SCE_CTRL_START)) {
        m_running = false;
    }
}

void App::handlePinAuthInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    // Edit server URL
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        if (m_pinAuth.expired) {
            // Request new PIN
            requestPin();
        } else {
            initImeDialog("Server URL", s_serverUrl, 255);
        }
    }
    
    // Connect after PIN approved
    if ((ctrl->buttons & SCE_CTRL_TRIANGLE) && !(oldCtrl->buttons & SCE_CTRL_TRIANGLE)) {
        if (!m_authToken.empty() && s_serverUrl[0]) {
            if (connectToServer(s_serverUrl)) {
                if (fetchLibrarySections()) {
                    fetchHubs();
                    setState(AppState::HOME);
                }
            }
        } else if (m_authToken.empty()) {
            setError("PIN not yet authorized");
        } else {
            setError("Please enter server URL");
        }
    }
    
    // Back to login
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        m_pinAuth = PinAuth();
        setState(AppState::LOGIN);
    }
}

void App::handleHomeInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        m_selectedItem = (m_selectedItem > 0) ? m_selectedItem - 1 : 6;
    }
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        m_selectedItem = (m_selectedItem < 6) ? m_selectedItem + 1 : 0;
    }
    
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        switch (m_selectedItem) {
            case 0:  // Libraries
                m_selectedItem = 0;
                m_scrollOffset = 0;
                setState(AppState::LIBRARY);
                break;
            case 1:  // Search
                s_searchText[0] = '\0';
                m_searchResults.clear();
                m_selectedItem = 0;
                setState(AppState::SEARCH);
                break;
            case 2:  // Continue Watching
                if (fetchContinueWatching()) {
                    // Copy continue watching items to media items for browsing
                    m_mediaItems = m_continueWatching;
                    m_selectedItem = 0;
                    m_scrollOffset = 0;
                    m_currentSectionKey = "continue";
                    setState(AppState::BROWSE);
                }
                break;
            case 3:  // Recently Added
                fetchRecentlyAdded();
                m_selectedItem = 0;
                m_scrollOffset = 0;
                setState(AppState::BROWSE);
                break;
            case 4:  // Live TV
                fetchLiveTVChannels();
                m_selectedItem = 0;
                m_scrollOffset = 0;
                setState(AppState::LIVE_TV);
                break;
            case 5:  // Settings
                m_selectedItem = 0;
                setState(AppState::SETTINGS);
                break;
            case 6:  // Logout
                logout();
                break;
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        logout();
    }
    
    if ((ctrl->buttons & SCE_CTRL_START) && !(oldCtrl->buttons & SCE_CTRL_START)) {
        m_running = false;
    }
}

void App::handleLibraryInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    int listSize = (int)m_librarySections.size();
    int visibleItems = 7;
    
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        if (m_selectedItem > 0) {
            m_selectedItem--;
            if (m_selectedItem < m_scrollOffset) {
                m_scrollOffset = m_selectedItem;
            }
        }
    }
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        if (m_selectedItem < listSize - 1) {
            m_selectedItem++;
            if (m_selectedItem >= m_scrollOffset + visibleItems) {
                m_scrollOffset = m_selectedItem - visibleItems + 1;
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        if (m_selectedItem < listSize) {
            const auto& section = m_librarySections[m_selectedItem];
            if (fetchLibraryContent(section.key)) {
                m_selectedItem = 0;
                m_scrollOffset = 0;
                setState(AppState::BROWSE);
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        m_selectedItem = 0;
        setState(AppState::HOME);
    }
}

void App::handleBrowseInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    int listSize = (int)m_mediaItems.size();
    int visibleItems = 5;  // Reduced for poster display
    
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        if (m_selectedItem > 0) {
            m_selectedItem--;
            if (m_selectedItem < m_scrollOffset) {
                m_scrollOffset = m_selectedItem;
            }
        }
    }
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        if (m_selectedItem < listSize - 1) {
            m_selectedItem++;
            if (m_selectedItem >= m_scrollOffset + visibleItems) {
                m_scrollOffset = m_selectedItem - visibleItems + 1;
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        if (m_selectedItem < listSize) {
            const auto& item = m_mediaItems[m_selectedItem];
            
            // Check if this item has children (show -> seasons, season -> episodes, artist -> albums, album -> tracks)
            bool hasChildren = (item.mediaType == MediaType::SHOW || 
                               item.mediaType == MediaType::SEASON ||
                               item.mediaType == MediaType::MUSIC_ARTIST ||
                               item.mediaType == MediaType::MUSIC_ALBUM);
            
            if (hasChildren) {
                // Save current position and drill down
                pushNavigation(item.ratingKey, item.title, item.mediaType);
                clearThumbnails();
                
                if (fetchChildren(item.ratingKey)) {
                    m_selectedItem = 0;
                    m_scrollOffset = 0;
                } else {
                    popNavigation();
                }
            } else {
                // It's a playable item (movie, episode, track) - show details
                if (fetchMediaDetails(item.ratingKey)) {
                    m_selectedItem = 0;
                    setState(AppState::MEDIA_DETAIL);
                }
            }
        }
    }
    
    // Search shortcut
    if ((ctrl->buttons & SCE_CTRL_TRIANGLE) && !(oldCtrl->buttons & SCE_CTRL_TRIANGLE)) {
        s_searchText[0] = '\0';
        m_searchResults.clear();
        m_selectedItem = 0;
        setState(AppState::SEARCH);
    }
    
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        clearThumbnails();
        
        if (!m_navStack.empty()) {
            // Go back in navigation stack
            NavEntry prev = m_navStack.back();
            popNavigation();
            
            if (!m_navStack.empty()) {
                // Still have parent, fetch its children
                NavEntry parent = m_navStack.back();
                fetchChildren(parent.key);
            } else {
                // Back to library content
                fetchLibraryContent(m_currentSectionKey);
            }
        } else {
            // Back to library list
            m_selectedItem = 0;
            m_scrollOffset = 0;
            setState(AppState::LIBRARY);
        }
    }
}

void App::handleSearchInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    int listSize = (int)m_searchResults.size();
    int visibleItems = 6;
    
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        if (m_selectedItem > 0) {
            m_selectedItem--;
            if (m_selectedItem < m_scrollOffset) {
                m_scrollOffset = m_selectedItem;
            }
        }
    }
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        if (m_selectedItem < listSize - 1) {
            m_selectedItem++;
            if (m_selectedItem >= m_scrollOffset + visibleItems) {
                m_scrollOffset = m_selectedItem - visibleItems + 1;
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        if (m_searchResults.empty() || m_selectedItem < 0) {
            // Open keyboard for search
            initImeDialog("Search", s_searchText, 255);
        } else if (m_selectedItem < listSize) {
            const auto& item = m_searchResults[m_selectedItem];
            
            // Check if item has children
            bool hasChildren = (item.mediaType == MediaType::SHOW || 
                               item.mediaType == MediaType::SEASON ||
                               item.mediaType == MediaType::MUSIC_ARTIST ||
                               item.mediaType == MediaType::MUSIC_ALBUM);
            
            if (hasChildren) {
                // Clear and go to browse with navigation
                m_navStack.clear();
                pushNavigation(item.ratingKey, item.title, item.mediaType);
                
                if (fetchChildren(item.ratingKey)) {
                    m_selectedItem = 0;
                    m_scrollOffset = 0;
                    setState(AppState::BROWSE);
                } else {
                    popNavigation();
                }
            } else {
                // View selected result details
                if (fetchMediaDetails(item.ratingKey)) {
                    setState(AppState::MEDIA_DETAIL);
                }
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        m_selectedItem = 0;
        m_scrollOffset = 0;
        setState(AppState::HOME);
    }
}

void App::handleMediaDetailInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    int maxBtn = (m_currentMedia.viewOffset > 0) ? 2 : 1;
    
    if ((ctrl->buttons & SCE_CTRL_LEFT) && !(oldCtrl->buttons & SCE_CTRL_LEFT)) {
        if (m_selectedItem > 0) m_selectedItem--;
    }
    if ((ctrl->buttons & SCE_CTRL_RIGHT) && !(oldCtrl->buttons & SCE_CTRL_RIGHT)) {
        if (m_selectedItem < maxBtn) m_selectedItem++;
    }
    
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        int watchBtnIdx = (m_currentMedia.viewOffset > 0) ? 2 : 1;
        
        if (m_selectedItem == 0) {
            // Play from beginning
            debugLog("Play button pressed - media: %s\n", m_currentMedia.ratingKey.c_str());
            startPlayback(false);
        } else if (m_selectedItem == 1 && m_currentMedia.viewOffset > 0) {
            // Resume
            debugLog("Resume button pressed at %d ms - media: %s\n", m_currentMedia.viewOffset, m_currentMedia.ratingKey.c_str());
            startPlayback(true);
        } else if (m_selectedItem == watchBtnIdx) {
            // Toggle watched status
            if (m_currentMedia.watched) {
                if (markAsUnwatched(m_currentMedia.ratingKey)) {
                    m_currentMedia.watched = false;
                }
            } else {
                if (markAsWatched(m_currentMedia.ratingKey)) {
                    m_currentMedia.watched = true;
                }
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        m_selectedItem = 0;
        setState(AppState::BROWSE);
    }
}

void App::handleSettingsInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    // Navigate settings (now 5 items: 0-4)
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        if (m_selectedItem > 0) m_selectedItem--;
    }
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        if (m_selectedItem < 4) m_selectedItem++;
    }
    
    // Change values with left/right
    if ((ctrl->buttons & SCE_CTRL_LEFT) && !(oldCtrl->buttons & SCE_CTRL_LEFT)) {
        if (m_selectedItem == 0) {
            // Video quality - decrease
            int q = static_cast<int>(m_settings.videoQuality);
            if (q > 0) {
                m_settings.videoQuality = static_cast<VideoQuality>(q - 1);
            }
        }
    }
    if ((ctrl->buttons & SCE_CTRL_RIGHT) && !(oldCtrl->buttons & SCE_CTRL_RIGHT)) {
        if (m_selectedItem == 0) {
            // Video quality - increase
            int q = static_cast<int>(m_settings.videoQuality);
            if (q < 4) {
                m_settings.videoQuality = static_cast<VideoQuality>(q + 1);
            }
        }
    }
    
    // Toggle with X
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        switch (m_selectedItem) {
            case 1:
                m_settings.autoPlay = !m_settings.autoPlay;
                saveSettings();
                break;
            case 2:
                m_settings.showSubtitles = !m_settings.showSubtitles;
                saveSettings();
                break;
            case 3:
                // Toggle debug file logging
                m_settings.enableFileLogging = !m_settings.enableFileLogging;
                setDebugLogEnabled(m_settings.enableFileLogging);
                saveSettings();
                if (m_settings.enableFileLogging) {
                    debugLog("Debug file logging ENABLED by user\n");
                }
                break;
            case 4:
                logout();
                break;
        }
    }
    
    // Back
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        m_selectedItem = 0;
        setState(AppState::HOME);
    }
}

void App::handlePlayerInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    #ifdef USE_MPV_PLAYER
    MpvPlayer& player = MpvPlayer::getInstance();
    bool mpvReady = player.isInitialized();
    #else
    bool mpvReady = false;
    #endif
    
    // Play/Pause toggle
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) player.togglePause();
        #endif
        debugLog("Player: Toggle pause\n");
    }
    
    // Seek backward (L trigger - 10 seconds)
    if ((ctrl->buttons & SCE_CTRL_LTRIGGER) && !(oldCtrl->buttons & SCE_CTRL_LTRIGGER)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) player.seekRelative(-10.0);
        #endif
        debugLog("Player: Seek backward 10s\n");
    }
    
    // Seek forward (R trigger - 10 seconds)
    if ((ctrl->buttons & SCE_CTRL_RTRIGGER) && !(oldCtrl->buttons & SCE_CTRL_RTRIGGER)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) player.seekRelative(10.0);
        #endif
        debugLog("Player: Seek forward 10s\n");
    }
    
    // Fast seek backward (Left - 30 seconds)
    if ((ctrl->buttons & SCE_CTRL_LEFT) && !(oldCtrl->buttons & SCE_CTRL_LEFT)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) player.seekRelative(-30.0);
        #endif
        debugLog("Player: Seek backward 30s\n");
    }
    
    // Fast seek forward (Right - 30 seconds)
    if ((ctrl->buttons & SCE_CTRL_RIGHT) && !(oldCtrl->buttons & SCE_CTRL_RIGHT)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) player.seekRelative(30.0);
        #endif
        debugLog("Player: Seek forward 30s\n");
    }
    
    // Volume up
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) {
            player.adjustVolume(5);
            char msg[32];
            snprintf(msg, sizeof(msg), "Volume: %d%%", player.getVolume());
            player.showOSD(msg, 1.0);
        }
        #endif
    }
    
    // Volume down
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) {
            player.adjustVolume(-5);
            char msg[32];
            snprintf(msg, sizeof(msg), "Volume: %d%%", player.getVolume());
            player.showOSD(msg, 1.0);
        }
        #endif
    }
    
    // Toggle mute (Square)
    if ((ctrl->buttons & SCE_CTRL_SQUARE) && !(oldCtrl->buttons & SCE_CTRL_SQUARE)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) {
            player.toggleMute();
            player.showOSD(player.isMuted() ? "Muted" : "Unmuted", 1.0);
        }
        #endif
    }
    
    // Cycle subtitles (Triangle)
    if ((ctrl->buttons & SCE_CTRL_TRIANGLE) && !(oldCtrl->buttons & SCE_CTRL_TRIANGLE)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) {
            player.cycleSubtitle();
            player.showOSD("Cycling subtitles", 1.0);
        }
        #endif
    }
    
    // Cycle audio (Select)
    if ((ctrl->buttons & SCE_CTRL_SELECT) && !(oldCtrl->buttons & SCE_CTRL_SELECT)) {
        #ifdef USE_MPV_PLAYER
        if (mpvReady) {
            player.cycleAudio();
            player.showOSD("Cycling audio track", 1.0);
        }
        #endif
    }
    
    // Stop and go back
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        stopPlayback();
    }
    
    #ifdef USE_MPV_PLAYER
    if (mpvReady) {
        // Check if playback ended
        if (player.hasEnded()) {
            debugLog("Player: Playback ended\n");
            // Mark as watched if we finished
            markAsWatched(m_currentMedia.ratingKey);
            stopPlayback();
        }
        
        // Check for errors
        if (player.hasError()) {
            setError(player.getErrorMessage());
            stopPlayback();
        }
        
        // Update position tracking
        m_playPosition = (uint64_t)(player.getPosition() * 1000);
    }
    #endif
}

// ============================================================================
// Live TV
// ============================================================================

void App::handleLiveTVInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    int channelCount = (int)m_liveTVChannels.size();
    if (channelCount == 0) {
        // Back button
        if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
            setState(AppState::HOME);
        }
        return;
    }
    
    // Navigate channels
    if ((ctrl->buttons & SCE_CTRL_UP) && !(oldCtrl->buttons & SCE_CTRL_UP)) {
        if (m_selectedItem > 0) {
            m_selectedItem--;
            if (m_selectedItem < m_scrollOffset) {
                m_scrollOffset = m_selectedItem;
            }
        }
    }
    
    if ((ctrl->buttons & SCE_CTRL_DOWN) && !(oldCtrl->buttons & SCE_CTRL_DOWN)) {
        if (m_selectedItem < channelCount - 1) {
            m_selectedItem++;
            if (m_selectedItem >= m_scrollOffset + 6) {  // 6 visible channels
                m_scrollOffset = m_selectedItem - 5;
            }
        }
    }
    
    // Watch channel
    if ((ctrl->buttons & SCE_CTRL_CROSS) && !(oldCtrl->buttons & SCE_CTRL_CROSS)) {
        if (m_selectedItem < channelCount) {
            startLiveTVPlayback(m_liveTVChannels[m_selectedItem].key);
        }
    }
    
    // Back
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        setState(AppState::HOME);
    }
    
    // Refresh guide
    if ((ctrl->buttons & SCE_CTRL_TRIANGLE) && !(oldCtrl->buttons & SCE_CTRL_TRIANGLE)) {
        fetchLiveTVChannels();
    }
}

void App::handlePhotoViewInput(SceCtrlData* ctrl, SceCtrlData* oldCtrl) {
    // Go back to media detail
    if ((ctrl->buttons & SCE_CTRL_CIRCLE) && !(oldCtrl->buttons & SCE_CTRL_CIRCLE)) {
        // Free photo texture
        if (m_currentMedia.thumbTexture) {
            vita2d_wait_rendering_done();
            vita2d_free_texture(m_currentMedia.thumbTexture);
            m_currentMedia.thumbTexture = nullptr;
        }
        setState(AppState::MEDIA_DETAIL);
    }
    
    // Could add zoom/pan controls here in future
}

void App::drawPhotoViewScreen(vita2d_pgf* font) {
    // Black background
    vita2d_clear_screen();
    
    if (m_currentMedia.thumbTexture) {
        // Get texture dimensions
        int texWidth = vita2d_texture_get_width(m_currentMedia.thumbTexture);
        int texHeight = vita2d_texture_get_height(m_currentMedia.thumbTexture);
        
        // Calculate scaling to fit screen while maintaining aspect ratio
        float scaleX = (float)SCREEN_WIDTH / texWidth;
        float scaleY = (float)SCREEN_HEIGHT / texHeight;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        
        // Center the image
        int drawWidth = (int)(texWidth * scale);
        int drawHeight = (int)(texHeight * scale);
        int x = (SCREEN_WIDTH - drawWidth) / 2;
        int y = (SCREEN_HEIGHT - drawHeight) / 2;
        
        vita2d_draw_texture_scale(m_currentMedia.thumbTexture, x, y, scale, scale);
    } else {
        vita2d_pgf_draw_text(font, SCREEN_WIDTH/2 - 80, SCREEN_HEIGHT/2, COLOR_WHITE, 1.0f, "Loading photo...");
    }
    
    // Title at top
    vita2d_draw_rectangle(0, 0, SCREEN_WIDTH, 50, RGBA8(0, 0, 0, 180));
    vita2d_pgf_draw_text(font, 30, 35, COLOR_WHITE, 0.9f, m_currentMedia.title.c_str());
    
    // Controls at bottom
    vita2d_draw_rectangle(0, SCREEN_HEIGHT - 40, SCREEN_WIDTH, 40, RGBA8(0, 0, 0, 180));
    vita2d_pgf_draw_text(font, 30, SCREEN_HEIGHT - 12, COLOR_GRAY, 0.6f, "O: Back");
}

// ============================================================================
// DVR Operations
// ============================================================================

bool App::fetchDVRRecordings() {
    debugLog("Fetching DVR recordings...\n");
    
    HttpClient client;
    HttpRequest req;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    // Try to get DVR recordings from the scheduled recordings endpoint
    std::string apiUrl = buildApiUrl("/media/subscriptions");
    req.url = apiUrl;
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200) {
        debugLog("DVR subscriptions response received\n");
        // Parse recordings - these appear as scheduled items
        // In actual implementation, would parse and store in a recordings list
        return true;
    }
    
    // Try alternate endpoint
    apiUrl = buildApiUrl("/livetv/dvrs/recordings");
    req.url = apiUrl;
    resp = client.request(req);
    
    if (resp.statusCode == 200) {
        debugLog("DVR recordings response received\n");
        return true;
    }
    
    return false;
}

bool App::scheduleDVRRecording(const std::string& programKey) {
    debugLog("Scheduling DVR recording for: %s\n", programKey.c_str());
    
    HttpClient client;
    HttpRequest req;
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    // Schedule recording via Plex API
    std::string apiUrl = buildApiUrl("/media/subscriptions");
    apiUrl += "?type=1&targetLibrarySectionID=&targetSectionLocationID=";
    apiUrl += "&prefs[minVideoQuality]=0&prefs[replaceLowerQuality]=true";
    apiUrl += "&prefs[recordPartials]=false&prefs[startOffsetMinutes]=0";
    apiUrl += "&prefs[endOffsetMinutes]=0&prefs[lineupChannel]=";
    apiUrl += "&prefs[startTimeslot]=-1&prefs[comskipEnabled]=-1";
    apiUrl += "&prefs[comskipMethod]=-1&prefs[oneShot]=true";
    apiUrl += "&hints[ratingKey]=" + programKey;
    
    req.url = apiUrl;
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200 || resp.statusCode == 201) {
        debugLog("DVR recording scheduled successfully\n");
        return true;
    }
    
    debugLog("Failed to schedule DVR recording: %d\n", resp.statusCode);
    return false;
}

bool App::cancelDVRRecording(const std::string& recordingKey) {
    debugLog("Canceling DVR recording: %s\n", recordingKey.c_str());
    
    HttpClient client;
    HttpRequest req;
    req.method = "DELETE";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    
    std::string apiUrl = buildApiUrl("/media/subscriptions/" + recordingKey);
    req.url = apiUrl;
    HttpResponse resp = client.request(req);
    
    if (resp.statusCode == 200 || resp.statusCode == 204) {
        debugLog("DVR recording canceled successfully\n");
        return true;
    }
    
    debugLog("Failed to cancel DVR recording: %d\n", resp.statusCode);
    return false;
}

// ============================================================================
// Main Loop
// ============================================================================

void App::run() {
    initDebugLog();
    debugLog("VitaPlex running...\n");
    
    vita2d_pgf* font = vita2d_load_default_pgf();
    if (!font) {
        debugLog("Failed to load font!\n");
        return;
    }
    
    SceCtrlData ctrl, oldCtrl;
    memset(&ctrl, 0, sizeof(ctrl));
    memset(&oldCtrl, 0, sizeof(oldCtrl));
    
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    
    while (m_running) {
        oldCtrl = ctrl;
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        
        // Handle IME dialog if running
        if (s_imeDialogRunning) {
            // Start vita2d drawing frame
            vita2d_start_drawing();
            vita2d_clear_screen();
            
            // Draw a simple background while IME is active
            vita2d_draw_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_DARK_BG);
            vita2d_pgf_draw_text(font, 350, 250, COLOR_WHITE, 1.0f, "Entering text...");
            
            // End drawing before common dialog update
            vita2d_end_drawing();
            
            // Update common dialog - vita2d provides a helper for this
            vita2d_common_dialog_update();
            
            // Check IME dialog status
            int imeResult = updateImeDialog();
            if (imeResult == 1) {
                // IME finished - apply result based on current state
                debugLog("IME: Applying result to field %d in state %d\n", s_selectedField, (int)m_state);
                if (m_state == AppState::LOGIN) {
                    if (s_selectedField == 0) {
                        strncpy(s_serverUrl, s_imeResult, sizeof(s_serverUrl) - 1);
                        s_serverUrl[sizeof(s_serverUrl) - 1] = '\0';
                        debugLog("IME: Server URL set to: %s\n", s_serverUrl);
                    } else if (s_selectedField == 1) {
                        strncpy(s_username, s_imeResult, sizeof(s_username) - 1);
                        s_username[sizeof(s_username) - 1] = '\0';
                        debugLog("IME: Username set to: %s\n", s_username);
                    } else if (s_selectedField == 2) {
                        strncpy(s_password, s_imeResult, sizeof(s_password) - 1);
                        s_password[sizeof(s_password) - 1] = '\0';
                        debugLog("IME: Password set\n");
                    }
                } else if (m_state == AppState::PIN_AUTH) {
                    strncpy(s_serverUrl, s_imeResult, sizeof(s_serverUrl) - 1);
                    s_serverUrl[sizeof(s_serverUrl) - 1] = '\0';
                } else if (m_state == AppState::SEARCH) {
                    strncpy(s_searchText, s_imeResult, sizeof(s_searchText) - 1);
                    s_searchText[sizeof(s_searchText) - 1] = '\0';
                    if (s_searchText[0]) {
                        search(s_searchText);
                    }
                }
            } else if (imeResult == -1) {
                debugLog("IME: Dialog cancelled or closed\n");
            }
            
            vita2d_swap_buffers();
            sceDisplayWaitVblankStart();
            continue;  // Skip normal drawing when IME is active
        }
        
        // Start drawing
        vita2d_start_drawing();
        vita2d_clear_screen();
        
        // Handle input based on state
        switch (m_state) {
            case AppState::LOGIN:
                handleLoginInput(&ctrl, &oldCtrl);
                break;
            case AppState::PIN_AUTH:
                handlePinAuthInput(&ctrl, &oldCtrl);
                // Auto-check PIN every ~3 seconds (180 frames at 60fps)
                if (!m_pinAuth.expired && m_authToken.empty()) {
                    s_pinCheckCounter++;
                    if (s_pinCheckCounter >= 180) {
                        checkPin();
                        s_pinCheckCounter = 0;
                    }
                }
                break;
            case AppState::HOME:
                handleHomeInput(&ctrl, &oldCtrl);
                break;
            case AppState::LIBRARY:
                handleLibraryInput(&ctrl, &oldCtrl);
                break;
            case AppState::BROWSE:
                handleBrowseInput(&ctrl, &oldCtrl);
                break;
            case AppState::SEARCH:
                handleSearchInput(&ctrl, &oldCtrl);
                break;
            case AppState::MEDIA_DETAIL:
                handleMediaDetailInput(&ctrl, &oldCtrl);
                break;
            case AppState::SETTINGS:
                handleSettingsInput(&ctrl, &oldCtrl);
                break;
            case AppState::PLAYER:
                debugLog("Main loop: PLAYER state - handling input\n");
                handlePlayerInput(&ctrl, &oldCtrl);
                // Update mpv player
                #ifdef USE_MPV_PLAYER
                debugLog("Main loop: PLAYER state - checking MPV\n");
                if (MpvPlayer::getInstance().isInitialized()) {
                    debugLog("Main loop: PLAYER state - calling MPV update\n");
                    MpvPlayer::getInstance().update();
                    debugLog("Main loop: PLAYER state - MPV update done\n");
                }
                #endif
                debugLog("Main loop: PLAYER state - input handled\n");
                break;
            case AppState::LIVE_TV:
                handleLiveTVInput(&ctrl, &oldCtrl);
                break;
            case AppState::PHOTO_VIEW:
                handlePhotoViewInput(&ctrl, &oldCtrl);
                break;
            default:
                break;
        }
        
        // Draw current screen
        switch (m_state) {
            case AppState::LOGIN:
                drawLoginScreen(font);
                break;
            case AppState::PIN_AUTH:
                drawPinAuthScreen(font);
                break;
            case AppState::HOME:
                drawHomeScreen(font);
                break;
            case AppState::LIBRARY:
                drawLibraryScreen(font);
                break;
            case AppState::BROWSE:
                drawBrowseScreen(font);
                // Load thumbnails progressively
                loadVisibleThumbnails();
                break;
            case AppState::SEARCH:
                drawSearchScreen(font);
                break;
            case AppState::MEDIA_DETAIL:
                drawMediaDetailScreen(font);
                break;
            case AppState::SETTINGS:
                drawSettingsScreen(font);
                break;
            case AppState::PLAYER:
                debugLog("Main loop: Drawing player screen\n");
                drawPlayerScreen(font);
                debugLog("Main loop: Player screen drawn\n");
                break;
            case AppState::LIVE_TV:
                drawLiveTVScreen(font);
                break;
            case AppState::PHOTO_VIEW:
                drawPhotoViewScreen(font);
                break;
            default:
                break;
        }
        
        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }
    
    // Shutdown mpv player
    #ifdef USE_MPV_PLAYER
    MpvPlayer::getInstance().shutdown();
    #endif
    
    vita2d_free_pgf(font);
}

void App::shutdown() {
    debugLog("VitaPlex shutting down...\n");
    closeDebugLog();
    m_running = false;
}

} // namespace vitaplex
