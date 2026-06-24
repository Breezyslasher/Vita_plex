/**
 * VitaPlex - Application implementation
 */

#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "app/plex_palette.hpp"
#include "activity/login_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/player_activity.hpp"
#include "app/synclounge_session.hpp"
#include "view/media_detail_view.hpp"

#include <borealis.hpp>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <memory>
#include <atomic>
#include "platform/paths.hpp"
#include "platform/platform.hpp"
#include "utils/image_loader.hpp"
#include "view/home_user_picker.hpp"

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

    // SyncLounge auto-join prompt: when the room host starts new content that
    // resolves to a confident local match and we're NOT already in a player,
    // offer to join via the same options popover the START menu uses. Fires
    // on the UI thread (the session marshals it).
    SyncLoungeSession::instance().setMatchPromptCallback(
        [](const std::string& ratingKey, const std::string& title) {
            if (PlayerActivity::isActive()) return;          // already watching — auto-load handles it
            if (SyncLoungeSession::instance().isHost()) return;  // we're driving the party
            std::vector<OptionRow> rows;
            rows.push_back({ "play.png", "Join the watch party", "", true, false,
                [ratingKey](brls::View*) {
                    Application::getInstance().pushPlayerActivity(ratingKey);
                    return true;
                }});
            rows.push_back({ "cross.png", "Not now", "", false, true,
                [](brls::View*) { return true; }});
            MediaDetailView::showCenteredChoice("Watch party", title, std::move(rows));
        });

    // Apply the saved "auto host" preference to the session (off by default, so
    // playing here never takes over the party unless the user opted in).
    SyncLoungeSession::instance().setAutoHost(m_settings.syncLoungeAutoHost);

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

    int selected = 0;
    if (!m_currentHomeUserUuid.empty()) {
        for (size_t i = 0; i < users.size(); i++) {
            if (users[i].uuid == m_currentHomeUserUuid) {
                selected = (int)i;
                break;
            }
        }
    }

    // The verbatim switch / token-store logic, lifted out of the old dropdown
    // handler and returning success so the picker can flash the PIN dots on a
    // wrong PIN. Plex Home /switch is a quick call; keep it synchronous as
    // before. Presentation lives in view/home_user_picker.hpp.
    auto trySwitch = [](const HomeUser& user, const std::string& pin) -> bool {
        Application& app = Application::getInstance();
        std::string newToken;
        if (!PlexClient::getInstance().switchHomeUser(
                app.getMasterAuthToken(), user.uuid, pin, newToken)) {
            return false;
        }
        app.setAuthToken(newToken);
        PlexClient::getInstance().setAuthToken(newToken);
        app.setCurrentHomeUserUuid(user.uuid);
        app.setCurrentHomeUserTitle(user.title);
        app.saveSettings();
        return true;
    };

    homepicker::show(users, selected, trySwitch, onComplete);
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
    namespace pal = vitaplex::palette;

    // The app ships ONE cohesive dark "all-Plex" palette, so force the dark
    // variant regardless of the (now-cosmetic) theme setting.
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

    // Repaint the borealis theme slots in place (no submodule fork). Applied
    // to BOTH tables so any stray light-theme read still lands on the warm
    // palette. Two rules keep focus and selection distinct from across a room:
    //   - Plex GOLD is the accent: brand / active / selected / primary FILL.
    //   - The highlight gradient is a warm cream HALO (gold -> #FFD46B), never
    //     a fill — so a focused control reads differently from a gold-filled
    //     selected one. (We deliberately move the old cyan glow to warm; the
    //     halo's brightness + the fact selection is a *fill* keep them apart.)
    const NVGcolor goldPulse   = pal::goldTint(0.15f);          // click ripple, gold low-alpha
    const NVGcolor spinnerGold = nvgRGBA(229, 160, 13, 90);     // global loading spinner
    for (brls::Theme* t : { &brls::Theme::getDarkTheme(),
                            &brls::Theme::getLightTheme() }) {
        // surfaces (warm charcoal)
        t->addColor("brls/clear",                     pal::bg);
        t->addColor("brls/background",                pal::bg);
        t->addColor("brls/sidebar/background",        pal::panel);
        t->addColor("brls/sidebar/separator",         pal::line);
        t->addColor("brls/applet_frame/separator",    pal::line);
        t->addColor("brls/header/border",             pal::line);
        // text
        t->addColor("brls/text",                      pal::text);
        t->addColor("brls/text_disabled",             pal::dim);
        t->addColor("brls/header/subtitle",           pal::muted);
        t->addColor("brls/header/rectangle",          pal::muted);
        // accent = gold (brand / active / selected)
        t->addColor("brls/accent",                    pal::gold);
        t->addColor("brls/sidebar/active_item",       pal::gold);
        t->addColor("brls/list/listItem_value_color", pal::gold);
        // primary button = gold FILL + ink text (the "picked" CTA)
        t->addColor("brls/button/primary_enabled_background",  pal::gold);
        t->addColor("brls/button/primary_enabled_text",        pal::goldInk);
        t->addColor("brls/button/primary_disabled_background", pal::surface3);
        t->addColor("brls/button/primary_disabled_text",       pal::dim);
        // default (secondary) button = surface-3 / white
        t->addColor("brls/button/default_enabled_background",  pal::surface3);
        t->addColor("brls/button/default_disabled_background", pal::surface2);
        t->addColor("brls/button/default_enabled_text",        pal::text);
        t->addColor("brls/button/default_disabled_text",       pal::dim);
        t->addColor("brls/button/enabled_border_color",        pal::line);
        t->addColor("brls/button/disabled_border_color",       pal::line);
        // "highlight" text-button variant
        t->addColor("brls/button/highlight_enabled_text",      pal::gold);
        t->addColor("brls/button/highlight_disabled_text",     pal::dim);
        // FOCUS HALO = warm gold-white (the borealis highlight gradient)
        t->addColor("brls/highlight/color1",     pal::gold);       // inner — ties to gold
        t->addColor("brls/highlight/color2",     pal::focusHalo);  // outer — bright cream
        t->addColor("brls/highlight/background", pal::surface2);   // warm fill behind focus
        t->addColor("brls/click_pulse",          goldPulse);
        // slider — gold filled line + gold scrubber knob (bright knob with a
        // deeper-gold rim so it stays distinct from the filled line)
        t->addColor("brls/slider/line_filled",          pal::gold);
        t->addColor("brls/slider/line_empty",           pal::surface3);
        t->addColor("brls/slider/pointer_color",        pal::goldBright);
        t->addColor("brls/slider/pointer_border_color", pal::goldDeep);
        // spinner → warm gold
        t->addColor("brls/spinner/bar_color",           spinnerGold);
    }

    brls::Logger::info("Applied all-Plex dark palette");
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
    {
        std::string sls = extractString("syncLoungeServer");
        if (!sls.empty()) m_settings.syncLoungeServer = sls;
        m_settings.syncLoungeRoom = extractString("syncLoungeRoom");
        m_settings.syncLoungeAutoHost = extractBool("syncLoungeAutoHost", false);
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
    json += "  \"syncLoungeServer\": \"" + esc(m_settings.syncLoungeServer) + "\",\n";
    json += "  \"syncLoungeRoom\": \"" + esc(m_settings.syncLoungeRoom) + "\",\n";
    json += "  \"syncLoungeAutoHost\": " + b(m_settings.syncLoungeAutoHost) + ",\n";
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
