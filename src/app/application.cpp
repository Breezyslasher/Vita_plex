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
            // Push Main first regardless — if the user backs out of the
            // picker (or never has one, because no Plex Home), they land
            // on the app as the last-used user. Then overlay the picker
            // when Auto-login is off. showHomeUserPicker no-ops when the
            // account has no Plex Home or only the owner.
            pushMainActivity();
            if (!m_settings.autoLoginAsLastUser) {
                showHomeUserPicker(nullptr);
            }
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

void Application::showHomeUserPicker(std::function<void()> onComplete) {
    // No master token = nothing to switch with. Caller proceeds as-is.
    if (m_masterAuthToken.empty()) {
        if (onComplete) onComplete();
        return;
    }

    std::vector<HomeUser> users;
    if (!PlexClient::getInstance().fetchHomeUsers(m_masterAuthToken, users)) {
        brls::Logger::warning("showHomeUserPicker: fetchHomeUsers failed");
        if (onComplete) onComplete();
        return;
    }

    // 0 users = no Plex Home on this account. 1 user = just the owner;
    // /switch isn't needed because master token IS that user's token.
    if (users.size() < 2) {
        if (onComplete) onComplete();
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(users.size());
    for (const auto& u : users) {
        std::string label = u.title;
        if (u.hasPin) label += "  (PIN)";
        if (u.admin)  label += "  (admin)";
        labels.push_back(label);
    }

    int selected = 0;
    if (!m_currentHomeUserUuid.empty()) {
        for (size_t i = 0; i < users.size(); i++) {
            if (users[i].uuid == m_currentHomeUserUuid) {
                selected = (int)i;
                break;
            }
        }
    }

    auto* dropdown = new brls::Dropdown(
        "Choose Plex Home User", labels,
        [users, onComplete](int picked) {
            // Dropdown::didSelectRowAt fires this callback synchronously
            // and THEN calls popActivity to dismiss itself. If we push a
            // new activity (the PIN IME, or an error Dialog from a failed
            // switch) inline here, the dropdown's pop targets that new
            // activity instead of itself — the IME flashes open and shut
            // and the user sees nothing. Defer to next main-loop tick so
            // the dropdown pops itself first, then we own the top of the
            // activity stack.
            brls::sync([users, onComplete, picked]() {
                if (picked < 0 || picked >= (int)users.size()) {
                    if (onComplete) onComplete();
                    return;
                }
                const HomeUser& chosen = users[picked];

                auto doSwitch = [chosen, onComplete](const std::string& pin) {
                    Application& app = Application::getInstance();
                    std::string newToken;
                    if (!PlexClient::getInstance().switchHomeUser(
                            app.getMasterAuthToken(), chosen.uuid, pin, newToken)) {
                        brls::Dialog* d = new brls::Dialog(
                            "Failed to switch to " + chosen.title +
                            (pin.empty() ? "" : " — check the PIN."));
                        d->addButton("OK", [onComplete]() {
                            if (onComplete) onComplete();
                        });
                        d->open();
                        return;
                    }
                    app.setAuthToken(newToken);
                    PlexClient::getInstance().setAuthToken(newToken);
                    app.setCurrentHomeUserUuid(chosen.uuid);
                    app.setCurrentHomeUserTitle(chosen.title);
                    app.saveSettings();
                    if (onComplete) onComplete();
                };

                if (chosen.hasPin) {
                    // Plex Home PINs are numeric, 4 digits. Use the standard
                    // IME — keeps the UI consistent and works on Vita / Switch
                    // / desktop without a custom keypad.
                    brls::Application::getImeManager()->openForText(
                        [doSwitch](std::string pin) { doSwitch(pin); },
                        "Enter PIN for " + chosen.title,
                        "4-digit PIN", 8, "", 0);
                } else {
                    doSwitch("");
                }
            });
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void Application::pushMainActivity() {
    brls::Application::pushActivity(new MainActivity());
}

void Application::pushPlayerActivity(const std::string& mediaKey, bool isLocalFile) {
    brls::Application::pushActivity(new PlayerActivity(mediaKey, isLocalFile));
}

void Application::pushLiveTVPlayerActivity(const std::string& streamUrl, const std::string& channelTitle,
                                           const std::string& liveSessionUuid) {
    brls::Application::pushActivity(
        PlayerActivity::createForStream(streamUrl, channelTitle, liveSessionUuid));
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

    // Repaint the borealis built-in focus highlight in Plex yellow so
    // the cyan glow doesn't fight the app's accent. Theme::addColor
    // overrides the named slot for the rest of the process — no
    // borealis patch required. We apply on both Light + Dark theme
    // tables so the swap survives a runtime theme change.
    const NVGcolor plexYellow      = nvgRGB(229, 160, 13);     // #E5A00D
    const NVGcolor plexYellowLight = nvgRGB(255, 196, 64);     // brightened for the highlight inner glow
    const NVGcolor plexYellowSoft  = nvgRGBA(229, 160, 13, 38); // ~15% alpha for click pulse
    for (brls::Theme* theme : { &brls::Theme::getDarkTheme(),
                                 &brls::Theme::getLightTheme() }) {
        theme->addColor("brls/highlight/color1", plexYellow);
        theme->addColor("brls/highlight/color2", plexYellowLight);
        theme->addColor("brls/click_pulse",      plexYellowSoft);
        theme->addColor("brls/button/highlight_enabled_text",  plexYellow);
        theme->addColor("brls/button/highlight_disabled_text", plexYellow);
    }

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

    // Plex Home user state. masterAuthToken falls back to authToken when
    // the saved file predates the home-users feature, so the very first
    // load after upgrading still has a valid master token to list users
    // with — otherwise the picker would be empty.
    m_masterAuthToken      = extractString("masterAuthToken");
    if (m_masterAuthToken.empty()) m_masterAuthToken = m_authToken;
    m_currentHomeUserUuid  = extractString("currentHomeUserUuid");
    m_currentHomeUserTitle = extractString("currentHomeUserTitle");

    brls::Logger::info("loadSettings: authToken={}, serverUrl={}, username={}",
                       m_authToken.empty() ? "(empty)" : "(set)",
                       m_serverUrl.empty() ? "(empty)" : m_serverUrl,
                       m_username.empty()  ? "(empty)" : m_username);

    // UI settings
    m_settings.theme = static_cast<AppTheme>(extractInt("theme"));
    m_settings.debugLogging = extractBool("debugLogging", true);

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
    {
        std::string lang = extractString("defaultSubtitleLanguage");
        if (!lang.empty()) m_settings.defaultSubtitleLanguage = lang;
    }

    m_settings.showMpvStats = extractBool("showMpvStats", false);

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

    // Live TV / DVR settings
    m_settings.defaultDvrSectionId    = extractString("defaultDvrSectionId");
    m_settings.defaultDvrSectionTitle = extractString("defaultDvrSectionTitle");
    {
        int v = extractInt("dvrStartOffsetMinutes");
        if (v >= 0 && v <= 60) m_settings.dvrStartOffsetMinutes = v;
    }
    {
        int v = extractInt("dvrEndOffsetMinutes");
        if (v >= 0 && v <= 60) m_settings.dvrEndOffsetMinutes = v;
    }
    m_settings.dvrRecordPartials = extractBool("dvrRecordPartials", true);
    {
        int v = extractInt("dvrMinVideoQuality");
        if (v >= 0 && v <= 100) m_settings.dvrMinVideoQuality = v;
    }
    {
        int v = extractInt("liveTvGuideHours");
        if (v > 0 && v <= 48) m_settings.liveTvGuideHours = v;
    }
    m_settings.autoLoginAsLastUser = extractBool("autoLoginAsLastUser", true);
    {
        // 0 = disabled; cap at one week so a corrupt settings file
        // can't pin us to a stale response forever.
        int v = extractInt("cacheLifetimeMinutes");
        if (v >= 0 && v <= 10080) m_settings.cacheLifetimeMinutes = v;
    }

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

    // JSON-escape every user-controlled string before splicing it into the
    // settings blob. Without this, a server URL or username containing a "
    // corrupts the file on save (so the next load silently wipes all
    // settings), and a hostile Plex server could arrange for its username
    // to contain \",\"authToken\":\"<attacker>\" and rewrite our stored
    // token on the next save.
    auto esc = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += (char)c;
                    }
            }
        }
        return out;
    };

    std::string json = "{\n";
    json += "  \"authToken\": \"" + esc(m_authToken) + "\",\n";
    json += "  \"masterAuthToken\": \"" + esc(m_masterAuthToken) + "\",\n";
    json += "  \"currentHomeUserUuid\": \"" + esc(m_currentHomeUserUuid) + "\",\n";
    json += "  \"currentHomeUserTitle\": \"" + esc(m_currentHomeUserTitle) + "\",\n";
    json += "  \"serverUrl\": \"" + esc(m_serverUrl) + "\",\n";
    json += "  \"username\": \"" + esc(m_username) + "\",\n";
    json += "  \"theme\": " + std::to_string(static_cast<int>(m_settings.theme)) + ",\n";
    json += "  \"debugLogging\": " + b(m_settings.debugLogging) + ",\n";
    json += "  \"showLibrariesInSidebar\": " + b(m_settings.showLibrariesInSidebar) + ",\n";
    json += "  \"collapseSidebar\": " + b(m_settings.collapseSidebar) + ",\n";
    json += "  \"hiddenLibraries\": \"" + esc(m_settings.hiddenLibraries) + "\",\n";
    json += "  \"sidebarOrder\": \"" + esc(m_settings.sidebarOrder) + "\",\n";
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
    json += "  \"defaultSubtitleLanguage\": \"" + esc(m_settings.defaultSubtitleLanguage) + "\",\n";
    json += "  \"showMpvStats\": " + b(m_settings.showMpvStats) + ",\n";
    json += "  \"videoQuality\": " + std::to_string(static_cast<int>(m_settings.videoQuality)) + ",\n";
    json += "  \"forceTranscode\": " + b(m_settings.forceTranscode) + ",\n";
    json += "  \"maxBitrate\": " + std::to_string(m_settings.maxBitrate) + ",\n";
    json += "  \"connectionTimeout\": " + std::to_string(m_settings.connectionTimeout) + ",\n";
    json += "  \"directPlay\": " + b(m_settings.directPlay) + ",\n";
    json += "  \"deleteAfterWatch\": " + b(m_settings.deleteAfterWatch) + ",\n";
    json += "  \"trackDefaultAction\": " + std::to_string(static_cast<int>(m_settings.trackDefaultAction)) + ",\n";
    json += "  \"backgroundMusic\": " + b(m_settings.backgroundMusic) + ",\n";
    json += "  \"defaultDvrSectionId\": \"" + esc(m_settings.defaultDvrSectionId) + "\",\n";
    json += "  \"defaultDvrSectionTitle\": \"" + esc(m_settings.defaultDvrSectionTitle) + "\",\n";
    json += "  \"dvrStartOffsetMinutes\": " + std::to_string(m_settings.dvrStartOffsetMinutes) + ",\n";
    json += "  \"dvrEndOffsetMinutes\": " + std::to_string(m_settings.dvrEndOffsetMinutes) + ",\n";
    json += "  \"dvrRecordPartials\": " + b(m_settings.dvrRecordPartials) + ",\n";
    json += "  \"dvrMinVideoQuality\": " + std::to_string(m_settings.dvrMinVideoQuality) + ",\n";
    json += "  \"liveTvGuideHours\": " + std::to_string(m_settings.liveTvGuideHours) + ",\n";
    json += "  \"autoLoginAsLastUser\": " + b(m_settings.autoLoginAsLastUser) + ",\n";
    json += "  \"cacheLifetimeMinutes\": " + std::to_string(m_settings.cacheLifetimeMinutes) + "\n";
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
