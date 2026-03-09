/**
 * VitaPlex - Settings Tab implementation
 */

#include "view/settings_tab.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/downloads_manager.hpp"
#include "player/mpv_player.hpp"
#include "activity/player_activity.hpp"
#include "utils/http_client.hpp"
#include <set>
#include <chrono>
#include <thread>

#ifdef __vita__
#include <psp2/net/netctl.h>
#include <psp2/net/net.h>
#endif

namespace vitaplex {

SettingsTab::SettingsTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Create scrolling container
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::COLUMN);
    m_contentBox->setPadding(20);
    m_contentBox->setGrow(1.0f);

    // Create all sections
    createAccountSection();
    createUISection();
    createLayoutSection();
    createContentDisplaySection();
    createPlaybackSection();
    createTranscodeSection();
    createDownloadsSection();
    createDebugSection();
    createAboutSection();

    m_scrollView->setContentView(m_contentBox);
    this->addView(m_scrollView);
}

void SettingsTab::createAccountSection() {
    Application& app = Application::getInstance();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("Account");
    m_contentBox->addView(header);

    // User info cell
    m_userLabel = new brls::Label();
    m_userLabel->setText("User: " + (app.getUsername().empty() ? "Not logged in" : app.getUsername()));
    m_userLabel->setFontSize(18);
    m_userLabel->setMarginLeft(16);
    m_userLabel->setMarginBottom(8);
    m_contentBox->addView(m_userLabel);

    // Server info cell
    m_serverLabel = new brls::Label();
    m_serverLabel->setText("Server: " + (app.getServerUrl().empty() ? "Not connected" : app.getServerUrl()));
    m_serverLabel->setFontSize(18);
    m_serverLabel->setMarginLeft(16);
    m_serverLabel->setMarginBottom(16);
    m_contentBox->addView(m_serverLabel);

    // Logout button
    auto* logoutCell = new brls::DetailCell();
    logoutCell->setText("Logout");
    logoutCell->setDetailText("Sign out from current account");
    logoutCell->registerClickAction([this](brls::View* view) {
        onLogout();
        return true;
    });
    m_contentBox->addView(logoutCell);
}

void SettingsTab::createUISection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("User Interface");
    m_contentBox->addView(header);

    // Theme selector
    m_themeSelector = new brls::SelectorCell();
    m_themeSelector->init("Theme", {"System", "Light", "Dark"}, static_cast<int>(settings.theme),
        [this](int index) {
            onThemeChanged(index);
        });
    m_contentBox->addView(m_themeSelector);

    // Debug logging toggle
    m_debugLogToggle = new brls::BooleanCell();
    m_debugLogToggle->init("Debug Logging", settings.debugLogging, [&settings](bool value) {
        settings.debugLogging = value;
        Application::getInstance().applyLogLevel();
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_debugLogToggle);

    // Show debug tab toggle
    m_showDebugTabToggle = new brls::BooleanCell();
    m_showDebugTabToggle->init("Show Debug Tab", settings.showDebugTab, [&settings](bool value) {
        settings.showDebugTab = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_showDebugTabToggle);

    // Info label for debug tab setting
    auto* debugInfoLabel = new brls::Label();
    debugInfoLabel->setText("Debug tab change requires app restart");
    debugInfoLabel->setFontSize(14);
    debugInfoLabel->setMarginLeft(16);
    debugInfoLabel->setMarginTop(8);
    m_contentBox->addView(debugInfoLabel);
}

void SettingsTab::createLayoutSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("Layout");
    m_contentBox->addView(header);

    // Show libraries in sidebar toggle
    m_sidebarLibrariesToggle = new brls::BooleanCell();
    m_sidebarLibrariesToggle->init("Libraries in Sidebar", settings.showLibrariesInSidebar, [&settings](bool value) {
        settings.showLibrariesInSidebar = value;
        Application::getInstance().saveSettings();
        // Note: Requires app restart to take effect
    });
    m_contentBox->addView(m_sidebarLibrariesToggle);

    // Collapse sidebar toggle
    m_collapseSidebarToggle = new brls::BooleanCell();
    m_collapseSidebarToggle->init("Collapse Sidebar", settings.collapseSidebar, [&settings](bool value) {
        settings.collapseSidebar = value;
        Application::getInstance().saveSettings();
        // Note: Requires app restart to take effect
    });
    m_contentBox->addView(m_collapseSidebarToggle);

    // Manage hidden libraries
    m_hiddenLibrariesCell = new brls::DetailCell();
    m_hiddenLibrariesCell->setText("Manage Hidden Libraries");
    int hiddenCount = 0;
    if (!settings.hiddenLibraries.empty()) {
        // Count comma-separated items
        hiddenCount = 1;
        for (char c : settings.hiddenLibraries) {
            if (c == ',') hiddenCount++;
        }
    }
    m_hiddenLibrariesCell->setDetailText(hiddenCount > 0 ? std::to_string(hiddenCount) + " hidden" : "None hidden");
    m_hiddenLibrariesCell->registerClickAction([this](brls::View* view) {
        onManageHiddenLibraries();
        return true;
    });
    m_contentBox->addView(m_hiddenLibrariesCell);

    // Manage sidebar order
    m_sidebarOrderCell = new brls::DetailCell();
    m_sidebarOrderCell->setText("Sidebar Order");
    m_sidebarOrderCell->setDetailText(settings.sidebarOrder.empty() ? "Default" : "Custom");
    m_sidebarOrderCell->registerClickAction([this](brls::View* view) {
        onManageSidebarOrder();
        return true;
    });
    m_contentBox->addView(m_sidebarOrderCell);

    // Info label
    auto* infoLabel = new brls::Label();
    infoLabel->setText("Layout changes require app restart");
    infoLabel->setFontSize(14);
    infoLabel->setMarginLeft(16);
    infoLabel->setMarginTop(8);
    m_contentBox->addView(infoLabel);
}

void SettingsTab::createContentDisplaySection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("Content Display");
    m_contentBox->addView(header);

    // Show collections toggle
    m_collectionsToggle = new brls::BooleanCell();
    m_collectionsToggle->init("Show Collections", settings.showCollections, [&settings](bool value) {
        settings.showCollections = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_collectionsToggle);

    // Show playlists toggle
    m_playlistsToggle = new brls::BooleanCell();
    m_playlistsToggle->init("Show Playlists", settings.showPlaylists, [&settings](bool value) {
        settings.showPlaylists = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_playlistsToggle);

    // Show genres/categories toggle
    m_genresToggle = new brls::BooleanCell();
    m_genresToggle->init("Show Categories", settings.showGenres, [&settings](bool value) {
        settings.showGenres = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_genresToggle);

    // Info label
    auto* contentInfoLabel = new brls::Label();
    contentInfoLabel->setText("Hides empty sections automatically");
    contentInfoLabel->setFontSize(14);
    contentInfoLabel->setMarginLeft(16);
    contentInfoLabel->setMarginTop(8);
    m_contentBox->addView(contentInfoLabel);
}

void SettingsTab::createPlaybackSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("Playback");
    m_contentBox->addView(header);

    // Auto-play next toggle
    m_autoPlayToggle = new brls::BooleanCell();
    m_autoPlayToggle->init("Auto-Play Next Episode", settings.autoPlayNext, [&settings](bool value) {
        settings.autoPlayNext = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_autoPlayToggle);

    // Resume playback toggle
    m_resumeToggle = new brls::BooleanCell();
    m_resumeToggle->init("Resume Playback", settings.resumePlayback, [&settings](bool value) {
        settings.resumePlayback = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_resumeToggle);

    // Show subtitles toggle
    m_subtitlesToggle = new brls::BooleanCell();
    m_subtitlesToggle->init("Show Subtitles", settings.showSubtitles, [&settings](bool value) {
        settings.showSubtitles = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_subtitlesToggle);

    // Subtitle size selector
    m_subtitleSizeSelector = new brls::SelectorCell();
    m_subtitleSizeSelector->init("Subtitle Size", {"Small", "Medium", "Large"},
        static_cast<int>(settings.subtitleSize),
        [this](int index) {
            onSubtitleSizeChanged(index);
        });
    m_contentBox->addView(m_subtitleSizeSelector);

    // Seek interval selector
    m_seekIntervalSelector = new brls::SelectorCell();
    m_seekIntervalSelector->init("Seek Interval",
        {"5 seconds", "10 seconds", "15 seconds", "30 seconds", "60 seconds"},
        settings.seekInterval == 5 ? 0 :
        settings.seekInterval == 10 ? 1 :
        settings.seekInterval == 15 ? 2 :
        settings.seekInterval == 30 ? 3 : 4,
        [this](int index) {
            onSeekIntervalChanged(index);
        });
    m_contentBox->addView(m_seekIntervalSelector);

    // Controls auto-hide selector
    m_controlsAutoHideSelector = new brls::SelectorCell();
    m_controlsAutoHideSelector->init("Controls Auto-Hide",
        {"Never", "3 seconds", "5 seconds", "10 seconds", "15 seconds"},
        settings.controlsAutoHideSeconds == 0 ? 0 :
        settings.controlsAutoHideSeconds == 3 ? 1 :
        settings.controlsAutoHideSeconds == 5 ? 2 :
        settings.controlsAutoHideSeconds == 10 ? 3 : 4,
        [this](int index) {
            onControlsAutoHideChanged(index);
        });
    m_contentBox->addView(m_controlsAutoHideSelector);

    // Auto-skip intro toggle
    m_autoSkipIntroToggle = new brls::BooleanCell();
    m_autoSkipIntroToggle->init("Auto-Skip Intro", settings.autoSkipIntro, [&settings](bool value) {
        settings.autoSkipIntro = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_autoSkipIntroToggle);

    // Auto-skip credits toggle
    m_autoSkipCreditsToggle = new brls::BooleanCell();
    m_autoSkipCreditsToggle->init("Auto-Skip Credits", settings.autoSkipCredits, [&settings](bool value) {
        settings.autoSkipCredits = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_autoSkipCreditsToggle);

    // Info label for skip settings
    auto* skipInfoLabel = new brls::Label();
    skipInfoLabel->setText("When off, a skip button appears briefly");
    skipInfoLabel->setFontSize(14);
    skipInfoLabel->setMarginLeft(16);
    skipInfoLabel->setMarginTop(8);
    m_contentBox->addView(skipInfoLabel);

    // Music section
    auto* musicHeader = new brls::Header();
    musicHeader->setTitle("Music");
    m_contentBox->addView(musicHeader);

    // Default track action selector
    m_trackActionSelector = new brls::SelectorCell();
    m_trackActionSelector->init("Default Track Action",
        {"Play Next", "Play Now (Replace Current)", "Add to Bottom of Queue", "Play Now (Clear Queue)", "Ask Each Time"},
        static_cast<int>(settings.trackDefaultAction),
        [](int index) {
            Application& app = Application::getInstance();
            app.getSettings().trackDefaultAction = static_cast<TrackDefaultAction>(index);
            app.saveSettings();
        });
    m_contentBox->addView(m_trackActionSelector);

    // Background music toggle
    m_backgroundMusicToggle = new brls::BooleanCell();
    m_backgroundMusicToggle->init("Background Music", settings.backgroundMusic, [&settings](bool value) {
        settings.backgroundMusic = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_backgroundMusicToggle);

    // Info label for music settings
    auto* musicInfoLabel = new brls::Label();
    musicInfoLabel->setText("Background music lets you leave player to add more songs");
    musicInfoLabel->setFontSize(14);
    musicInfoLabel->setMarginLeft(16);
    musicInfoLabel->setMarginTop(8);
    m_contentBox->addView(musicInfoLabel);
}

void SettingsTab::createTranscodeSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("Transcoding");
    m_contentBox->addView(header);

    // Video quality selector
    m_qualitySelector = new brls::SelectorCell();
    m_qualitySelector->init("Video Quality",
        {"Original (Direct Play)", "1080p (20 Mbps)", "720p (4 Mbps)", "480p (2 Mbps)", "360p (1 Mbps)", "240p (500 Kbps)"},
        static_cast<int>(settings.videoQuality),
        [this](int index) {
            onQualityChanged(index);
        });
    m_contentBox->addView(m_qualitySelector);

    // Force transcode toggle
    m_forceTranscodeToggle = new brls::BooleanCell();
    m_forceTranscodeToggle->init("Force Transcode", settings.forceTranscode, [&settings](bool value) {
        settings.forceTranscode = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_forceTranscodeToggle);

    // Direct play toggle
    m_directPlayToggle = new brls::BooleanCell();
    m_directPlayToggle->init("Try Direct Play First", settings.directPlay, [&settings](bool value) {
        settings.directPlay = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_directPlayToggle);

    // Connection timeout selector
    m_connectionTimeoutSelector = new brls::SelectorCell();
    m_connectionTimeoutSelector->init("Connection Timeout",
        {"30 seconds", "60 seconds", "120 seconds", "180 seconds", "300 seconds"},
        settings.connectionTimeout == 30 ? 0 :
        settings.connectionTimeout == 60 ? 1 :
        settings.connectionTimeout == 120 ? 2 :
        settings.connectionTimeout == 300 ? 4 : 3,
        [this](int index) {
            onConnectionTimeoutChanged(index);
        });
    m_contentBox->addView(m_connectionTimeoutSelector);
}

void SettingsTab::createDownloadsSection() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Section header
    auto* header = new brls::Header();
    header->setTitle("Downloads");
    m_contentBox->addView(header);

    // Delete after watch toggle
    m_deleteAfterWatchToggle = new brls::BooleanCell();
    m_deleteAfterWatchToggle->init("Delete After Watching", settings.deleteAfterWatch, [&settings](bool value) {
        settings.deleteAfterWatch = value;
        Application::getInstance().saveSettings();
    });
    m_contentBox->addView(m_deleteAfterWatchToggle);

    // Clear all downloads
    m_clearDownloadsCell = new brls::DetailCell();
    m_clearDownloadsCell->setText("Clear All Downloads");
    auto downloads = DownloadsManager::getInstance().getDownloads();
    m_clearDownloadsCell->setDetailText(std::to_string(downloads.size()) + " items");
    m_clearDownloadsCell->registerClickAction([this](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog("Delete all downloaded content?");

        dialog->addButton("Cancel", []() {});

        dialog->addButton("Delete All", [this]() {
            auto downloads = DownloadsManager::getInstance().getDownloads();
            for (const auto& item : downloads) {
                DownloadsManager::getInstance().deleteDownload(item.ratingKey);
            }
            if (m_clearDownloadsCell) {
                m_clearDownloadsCell->setDetailText("0 items");
            }
            brls::Application::notify("All downloads deleted");
        });

        dialog->open();
        return true;
    });
    m_contentBox->addView(m_clearDownloadsCell);

    // Downloads storage path info
    auto* pathLabel = new brls::Label();
    pathLabel->setText("Storage: " + DownloadsManager::getInstance().getDownloadsPath());
    pathLabel->setFontSize(14);
    pathLabel->setMarginLeft(16);
    pathLabel->setMarginTop(8);
    m_contentBox->addView(pathLabel);
}

void SettingsTab::createDebugSection() {
    // Section header
    auto* header = new brls::Header();
    header->setTitle("Debug");
    m_contentBox->addView(header);

    // Network test button
    auto* networkTestCell = new brls::DetailCell();
    networkTestCell->setText("Network Test");
    networkTestCell->setDetailText("View network info and test Plex connection");
    networkTestCell->registerClickAction([this](brls::View* view) {
        onNetworkTest();
        return true;
    });
    m_contentBox->addView(networkTestCell);

    // Test local playback button
    auto* testLocalCell = new brls::DetailCell();
    testLocalCell->setText("Test Local Playback");
    testLocalCell->setDetailText("ux0:data/VitaPlex/test.mp3");
    testLocalCell->registerClickAction([this](brls::View* view) {
        onTestLocalPlayback();
        return true;
    });
    m_contentBox->addView(testLocalCell);

    // Info label
    auto* infoLabel = new brls::Label();
    infoLabel->setText("Place test.mp3 or test.mp4 in ux0:data/VitaPlex/");
    infoLabel->setFontSize(14);
    infoLabel->setMarginLeft(16);
    infoLabel->setMarginTop(8);
    infoLabel->setMarginBottom(16);
    m_contentBox->addView(infoLabel);
}

void SettingsTab::createAboutSection() {
    // Section header
    auto* header = new brls::Header();
    header->setTitle("About");
    m_contentBox->addView(header);

    // Version info
    auto* versionCell = new brls::DetailCell();
    versionCell->setText("Version");
    versionCell->setDetailText(VITA_PLEX_VERSION);
    m_contentBox->addView(versionCell);

    // App description
    auto* descLabel = new brls::Label();
    descLabel->setText("VitaPlex - Plex Client for PlayStation Vita");
    descLabel->setFontSize(16);
    descLabel->setMarginLeft(16);
    descLabel->setMarginTop(8);
    m_contentBox->addView(descLabel);

    // Credit
    auto* creditLabel = new brls::Label();
    creditLabel->setText("UI powered by Borealis");
    creditLabel->setFontSize(14);
    creditLabel->setMarginLeft(16);
    creditLabel->setMarginTop(4);
    creditLabel->setMarginBottom(20);
    m_contentBox->addView(creditLabel);
}

void SettingsTab::onLogout() {
    brls::Dialog* dialog = new brls::Dialog("Are you sure you want to logout?");

    dialog->addButton("Cancel", []() {});

    dialog->addButton("Logout", [this]() {
        // Clear credentials
        PlexClient::getInstance().logout();
        Application::getInstance().setAuthToken("");
        Application::getInstance().setServerUrl("");
        Application::getInstance().setUsername("");
        Application::getInstance().saveSettings();

        // Go back to login
        Application::getInstance().pushLoginActivity();
    });

    dialog->open();
}

void SettingsTab::onThemeChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    settings.theme = static_cast<AppTheme>(index);
    app.applyTheme();
    app.saveSettings();
}

void SettingsTab::onQualityChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    settings.videoQuality = static_cast<VideoQuality>(index);

    // Update bitrate based on quality
    switch (settings.videoQuality) {
        case VideoQuality::ORIGINAL:
            settings.maxBitrate = 0;  // No limit
            break;
        case VideoQuality::QUALITY_1080P:
            settings.maxBitrate = 20000;
            break;
        case VideoQuality::QUALITY_720P:
            settings.maxBitrate = 4000;
            break;
        case VideoQuality::QUALITY_480P:
            settings.maxBitrate = 2000;
            break;
        case VideoQuality::QUALITY_360P:
            settings.maxBitrate = 1000;
            break;
        case VideoQuality::QUALITY_240P:
            settings.maxBitrate = 500;
            break;
    }

    app.saveSettings();
}

void SettingsTab::onSubtitleSizeChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    settings.subtitleSize = static_cast<SubtitleSize>(index);
    app.saveSettings();
}

void SettingsTab::onConnectionTimeoutChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    switch (index) {
        case 0: settings.connectionTimeout = 30; break;
        case 1: settings.connectionTimeout = 60; break;
        case 2: settings.connectionTimeout = 120; break;
        case 3: settings.connectionTimeout = 180; break;
        case 4: settings.connectionTimeout = 300; break;
    }

    app.saveSettings();
}

void SettingsTab::onSeekIntervalChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    switch (index) {
        case 0: settings.seekInterval = 5; break;
        case 1: settings.seekInterval = 10; break;
        case 2: settings.seekInterval = 15; break;
        case 3: settings.seekInterval = 30; break;
        case 4: settings.seekInterval = 60; break;
    }

    app.saveSettings();
}

void SettingsTab::onControlsAutoHideChanged(int index) {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    switch (index) {
        case 0: settings.controlsAutoHideSeconds = 0; break;
        case 1: settings.controlsAutoHideSeconds = 3; break;
        case 2: settings.controlsAutoHideSeconds = 5; break;
        case 3: settings.controlsAutoHideSeconds = 10; break;
        case 4: settings.controlsAutoHideSeconds = 15; break;
    }

    app.saveSettings();
}

void SettingsTab::onManageHiddenLibraries() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Fetch library sections
    std::vector<LibrarySection> sections;
    PlexClient::getInstance().fetchLibrarySections(sections);

    if (sections.empty()) {
        brls::Dialog* dialog = new brls::Dialog("No libraries found");
        dialog->addButton("OK", []() {});
        dialog->open();
        return;
    }

    // Parse currently hidden libraries
    std::set<std::string> hiddenKeys;
    std::string hidden = settings.hiddenLibraries;
    size_t pos = 0;
    while ((pos = hidden.find(',')) != std::string::npos) {
        std::string key = hidden.substr(0, pos);
        if (!key.empty()) hiddenKeys.insert(key);
        hidden.erase(0, pos + 1);
    }
    if (!hidden.empty()) hiddenKeys.insert(hidden);

    // Create scrollable dialog content for many libraries
    brls::Box* outerBox = new brls::Box();
    outerBox->setAxis(brls::Axis::COLUMN);
    outerBox->setWidth(400);
    outerBox->setHeight(350);  // Fixed height for scrolling

    auto* title = new brls::Label();
    title->setText("Select libraries to hide:");
    title->setFontSize(20);
    title->setMarginBottom(15);
    title->setMarginLeft(20);
    title->setMarginTop(20);
    outerBox->addView(title);

    // Scrolling frame for checkboxes
    brls::ScrollingFrame* scrollFrame = new brls::ScrollingFrame();
    scrollFrame->setGrow(1.0f);

    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPaddingLeft(20);
    content->setPaddingRight(20);

    std::vector<std::pair<std::string, brls::BooleanCell*>> checkboxes;

    for (const auto& section : sections) {
        auto* checkbox = new brls::BooleanCell();
        bool isHidden = (hiddenKeys.find(section.key) != hiddenKeys.end());
        checkbox->init(section.title, isHidden, [](bool value) {});
        content->addView(checkbox);
        checkboxes.push_back({section.key, checkbox});
    }

    scrollFrame->setContentView(content);
    outerBox->addView(scrollFrame);

    brls::Dialog* dialog = new brls::Dialog(outerBox);

    dialog->addButton("Cancel", []() {});

    dialog->addButton("Save", [checkboxes, this]() {
        Application& app = Application::getInstance();
        AppSettings& settings = app.getSettings();

        std::string newHidden;
        for (const auto& pair : checkboxes) {
            if (pair.second->isOn()) {
                if (!newHidden.empty()) newHidden += ",";
                newHidden += pair.first;
            }
        }

        settings.hiddenLibraries = newHidden;
        app.saveSettings();

        // Update the cell text
        int count = 0;
        if (!newHidden.empty()) {
            count = 1;
            for (char c : newHidden) {
                if (c == ',') count++;
            }
        }
        if (m_hiddenLibrariesCell) {
            m_hiddenLibrariesCell->setDetailText(count > 0 ? std::to_string(count) + " hidden" : "None hidden");
        }
    });

    dialog->open();
}

void SettingsTab::onManageSidebarOrder() {
    Application& app = Application::getInstance();
    AppSettings& settings = app.getSettings();

    // Default sidebar items (Settings is always last and not movable)
    std::vector<std::pair<std::string, std::string>> defaultItems = {
        {"home", "Home"},
        {"library", "Library"},
        {"music", "Music"},
        {"search", "Search"},
        {"livetv", "Live TV"}
    };

    // Parse current order or use default
    std::vector<std::pair<std::string, std::string>> currentOrder;
    if (!settings.sidebarOrder.empty()) {
        std::string order = settings.sidebarOrder;
        size_t pos = 0;
        while ((pos = order.find(',')) != std::string::npos) {
            std::string key = order.substr(0, pos);
            for (const auto& item : defaultItems) {
                if (item.first == key) {
                    currentOrder.push_back(item);
                    break;
                }
            }
            order.erase(0, pos + 1);
        }
        if (!order.empty()) {
            for (const auto& item : defaultItems) {
                if (item.first == order) {
                    currentOrder.push_back(item);
                    break;
                }
            }
        }
        // Add any missing items at the end
        for (const auto& item : defaultItems) {
            bool found = false;
            for (const auto& cur : currentOrder) {
                if (cur.first == item.first) {
                    found = true;
                    break;
                }
            }
            if (!found) currentOrder.push_back(item);
        }
    } else {
        currentOrder = defaultItems;
    }

    // Create dialog content
    brls::Box* outerBox = new brls::Box();
    outerBox->setAxis(brls::Axis::COLUMN);
    outerBox->setWidth(450);
    outerBox->setHeight(380);

    auto* title = new brls::Label();
    title->setText("Reorder sidebar items:");
    title->setFontSize(20);
    title->setMarginBottom(15);
    title->setMarginLeft(20);
    title->setMarginTop(20);
    outerBox->addView(title);

    // Use shared state
    auto orderCopy = std::make_shared<std::vector<std::pair<std::string, std::string>>>(currentOrder);
    auto labels = std::make_shared<std::vector<brls::Label*>>();

    // Scrolling frame for items
    brls::ScrollingFrame* scrollFrame = new brls::ScrollingFrame();
    scrollFrame->setGrow(1.0f);

    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPaddingLeft(20);
    content->setPaddingRight(20);

    // Helper to update all labels
    auto updateLabels = [orderCopy, labels]() {
        for (size_t i = 0; i < labels->size() && i < orderCopy->size(); i++) {
            (*labels)[i]->setText(std::to_string(i + 1) + ". " + (*orderCopy)[i].second);
        }
    };

    // Create rows for each item
    for (size_t i = 0; i < orderCopy->size(); i++) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(50);
        row->setMarginBottom(8);

        auto* label = new brls::Label();
        label->setText(std::to_string(i + 1) + ". " + (*orderCopy)[i].second);
        label->setFontSize(18);
        label->setGrow(1.0f);
        row->addView(label);
        labels->push_back(label);

        auto* btnBox = new brls::Box();
        btnBox->setAxis(brls::Axis::ROW);

        // Up button (move item up)
        auto* upBtn = new brls::Button();
        upBtn->setText(i > 0 ? "^" : " ");
        upBtn->setWidth(45);
        upBtn->setHeight(35);
        upBtn->setMarginRight(8);
        if (i > 0) {
            size_t idx = i;
            upBtn->registerClickAction([orderCopy, labels, idx, updateLabels](brls::View* view) {
                if (idx > 0 && idx < orderCopy->size()) {
                    std::swap((*orderCopy)[idx], (*orderCopy)[idx - 1]);
                    updateLabels();
                }
                return true;
            });
        }
        btnBox->addView(upBtn);

        // Down button (move item down)
        auto* downBtn = new brls::Button();
        downBtn->setText(i < orderCopy->size() - 1 ? "v" : " ");
        downBtn->setWidth(45);
        downBtn->setHeight(35);
        if (i < orderCopy->size() - 1) {
            size_t idx = i;
            downBtn->registerClickAction([orderCopy, labels, idx, updateLabels](brls::View* view) {
                if (idx < orderCopy->size() - 1) {
                    std::swap((*orderCopy)[idx], (*orderCopy)[idx + 1]);
                    updateLabels();
                }
                return true;
            });
        }
        btnBox->addView(downBtn);

        row->addView(btnBox);
        content->addView(row);
    }

    scrollFrame->setContentView(content);
    outerBox->addView(scrollFrame);

    // Note about Settings
    auto* noteLabel = new brls::Label();
    noteLabel->setText("Settings always appears last. Restart required.");
    noteLabel->setFontSize(14);
    noteLabel->setMarginLeft(20);
    noteLabel->setMarginBottom(10);
    outerBox->addView(noteLabel);

    brls::Dialog* dialog = new brls::Dialog(outerBox);

    dialog->addButton("Cancel", []() {});

    dialog->addButton("Reset", [orderCopy, defaultItems, this]() {
        Application& app = Application::getInstance();
        AppSettings& settings = app.getSettings();
        settings.sidebarOrder = "";
        app.saveSettings();
        if (m_sidebarOrderCell) {
            m_sidebarOrderCell->setDetailText("Default");
        }
    });

    dialog->addButton("Save", [orderCopy, this]() {
        Application& app = Application::getInstance();
        AppSettings& settings = app.getSettings();

        std::string newOrder;
        for (const auto& item : *orderCopy) {
            if (!newOrder.empty()) newOrder += ",";
            newOrder += item.first;
        }

        settings.sidebarOrder = newOrder;
        app.saveSettings();

        if (m_sidebarOrderCell) {
            m_sidebarOrderCell->setDetailText("Custom");
        }
    });

    dialog->open();
}

void SettingsTab::onNetworkTest() {
    // Show a toast while tests run
    brls::Application::notify("Running network test...");

    // Run the network tests on a detached thread to avoid blocking the UI
    std::thread([this]() {
        // ── 1. WiFi Check ──
        std::string ipAddress = "-";
        std::string dnsInfo = "-";
        std::string signalStr = "-";
        std::string ssid = "-";
        bool wifiConnected = false;

#ifdef __vita__
        SceNetCtlInfo info;

        int ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
        if (ret >= 0) {
            ipAddress = std::string(info.ip_address);
            wifiConnected = true;
        }

        ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &info);
        if (ret >= 0) {
            ssid = std::string(info.ssid);
        }

        ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info);
        if (ret >= 0) {
            signalStr = std::to_string(info.rssi_percentage) + "%";
        }

        ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_PRIMARY_DNS, &info);
        if (ret >= 0) {
            dnsInfo = std::string(info.primary_dns);
            ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SECONDARY_DNS, &info);
            if (ret >= 0) {
                dnsInfo += " / " + std::string(info.secondary_dns);
            }
        }
#else
        ipAddress = "127.0.0.1";
        dnsInfo = "8.8.8.8";
        signalStr = "100%";
        ssid = "Desktop";
        wifiConnected = true;
#endif

        // ── 2. Internet Check (latency) ──
        std::string internetStatus = "Skipped (no WiFi)";
        if (wifiConnected) {
            HttpClient netClient;
            netClient.setTimeout(10);

            auto start = std::chrono::steady_clock::now();
            std::string response;
            bool ok = netClient.get("http://connectivitycheck.gstatic.com/generate_204", response);
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            if (ok) {
                internetStatus = "Reachable (" + std::to_string(ms) + "ms)";
            } else {
                internetStatus = "Unreachable (" + std::to_string(ms) + "ms)";
            }
        }

        // ── 3. Plex Server Check (latency) ──
        Application& app = Application::getInstance();
        std::string serverUrl = app.getServerUrl();
        std::string plexStatus;
        std::string plexLatency = "-";

        if (serverUrl.empty()) {
            plexStatus = "Not configured";
        } else if (!wifiConnected) {
            plexStatus = "Skipped (no WiFi)";
        } else {
            HttpClient plexClient;
            plexClient.setTimeout(10);
            plexClient.setDefaultHeader("X-Plex-Token", app.getAuthToken());
            plexClient.setDefaultHeader("Accept", "application/json");

            auto start = std::chrono::steady_clock::now();
            std::string response;
            bool ok = plexClient.get(serverUrl + "/identity", response);
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            plexLatency = std::to_string(ms) + "ms";
            if (ok) {
                plexStatus = "Connected (" + std::to_string(ms) + "ms)";
            } else {
                plexStatus = "Failed (" + std::to_string(ms) + "ms)";
            }
        }

        // ── Build dialog on main thread ──
        // Capture results by value for the lambda
        brls::sync([=]() {
            brls::Box* content = new brls::Box();
            content->setAxis(brls::Axis::COLUMN);
            content->setWidth(700);
            content->setHeight(420);
            content->setPadding(25);

            auto* titleLabel = new brls::Label();
            titleLabel->setText("Network Test Results");
            titleLabel->setFontSize(22);
            titleLabel->setMarginBottom(15);
            content->addView(titleLabel);

            // Helper to create info rows (item #11 style)
            auto addRow = [&content](const std::string& label, const std::string& value) {
                auto* row = new brls::Box();
                row->setAxis(brls::Axis::ROW);
                row->setMarginBottom(8);
                auto* lblA = new brls::Label();
                lblA->setText(label);
                lblA->setFontSize(16);
                lblA->setWidth(220);
                row->addView(lblA);
                auto* lblB = new brls::Label();
                lblB->setText(value);
                lblB->setFontSize(16);
                row->addView(lblB);
                content->addView(row);
            };

            // Helper for section headers
            auto addHeader = [&content](const std::string& text) {
                auto* lbl = new brls::Label();
                lbl->setText(text);
                lbl->setFontSize(16);
                lbl->setMarginBottom(6);
                lbl->setMarginTop(4);
                content->addView(lbl);
            };

            // WiFi section
            addHeader("-- WiFi --");
            addRow("Status:", wifiConnected ? "Connected" : "Not Connected");
            addRow("Network:", ssid);
            addRow("IP Address:", ipAddress);
            addRow("DNS:", dnsInfo);
            addRow("Signal:", signalStr);

            // Internet section
            addHeader("-- Internet --");
            addRow("Connectivity:", internetStatus);

            // Plex server section
            addHeader("-- Plex Server --");
            addRow("Server:", serverUrl.empty() ? "Not configured" : serverUrl);
            addRow("Connection:", plexStatus);

            auto* dialog = new brls::Dialog(content);
            dialog->addButton("Close", []() {});
            dialog->open();
        });
    }).detach();
}

void SettingsTab::onTestLocalPlayback() {
    brls::Logger::info("SettingsTab: Testing local playback...");

    // Check for test files
    const std::string basePath = "ux0:data/VitaPlex/";
    std::string testFile;

    // Try mp4 first (to test video), then audio files
    std::vector<std::string> testFiles = {
        basePath + "test.mp4",
        basePath + "test.mp3",
        basePath + "test.ogg",
        basePath + "test.wav"
    };

    for (const auto& file : testFiles) {
        FILE* f = fopen(file.c_str(), "rb");
        if (f) {
            fclose(f);
            testFile = file;
            brls::Logger::info("SettingsTab: Found test file: {}", testFile);
            break;
        }
    }

    if (testFile.empty()) {
        brls::Application::notify("No test file found in ux0:data/VitaPlex/");
        brls::Logger::error("SettingsTab: No test file found");
        return;
    }

    // Push player activity with the test file (this shows the video view properly)
    brls::Logger::info("SettingsTab: Pushing player activity for: {}", testFile);
    PlayerActivity* activity = PlayerActivity::createForDirectFile(testFile);
    brls::Application::pushActivity(activity);
}

} // namespace vitaplex
