/**
 * VitaPlex - Main Activity implementation
 */

#include "activity/main_activity.hpp"
#include "view/home_tab.hpp"
#include "view/library_tab.hpp"
#include "view/library_section_tab.hpp"
#include "view/search_tab.hpp"
#include "view/settings_tab.hpp"
#include "view/livetv_tab.hpp"
#include "view/downloads_tab.hpp"
#include "view/music_tab.hpp"
#include "app/downloads_manager.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "utils/async.hpp"

namespace vitaplex {

// Cached library sections for sidebar mode
static std::vector<LibrarySection> s_cachedSections;

MainActivity::MainActivity() {
    brls::Logger::debug("MainActivity created");
}

brls::View* MainActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/main.xml");
}

void MainActivity::onContentAvailable() {
    brls::Logger::debug("MainActivity content available");

    if (tabFrame) {
        AppSettings& settings = Application::getInstance().getSettings();

        // Apply collapse sidebar setting
        if (settings.collapseSidebar) {
            brls::View* sidebar = tabFrame->getView("brls/tab_frame/sidebar");
            if (sidebar) {
                sidebar->setWidth(150);
                brls::Logger::debug("MainActivity: Collapsed sidebar to 150px");
            }
        }

        bool hasLiveTV = PlexClient::getInstance().hasLiveTV();

        // If showing libraries in sidebar, only show actual library sections
        // Don't show premade tabs like "Library", "Music", "TV"
        if (settings.showLibrariesInSidebar) {
            // Home tab
            tabFrame->addTab("Home", []() { return new HomeTab(); });

            // Load actual library sections to sidebar
            loadLibrariesToSidebar();

            // Search
            tabFrame->addTab("Search", []() { return new SearchTab(); });

            // Live TV if available
            if (hasLiveTV) {
                tabFrame->addTab("Live TV", []() { return new LiveTVTab(); });
            }
        } else {
            // Standard mode with premade tabs
            // Add tabs based on sidebar order setting
            std::string sidebarOrder = settings.sidebarOrder;

            // Parse the order or use default
            std::vector<std::string> order;
            if (!sidebarOrder.empty()) {
                std::string orderStr = sidebarOrder;
                size_t pos = 0;
                while ((pos = orderStr.find(',')) != std::string::npos) {
                    order.push_back(orderStr.substr(0, pos));
                    orderStr.erase(0, pos + 1);
                }
                if (!orderStr.empty()) {
                    order.push_back(orderStr);
                }
            } else {
                // Default order
                order = {"home", "library", "music", "search", "livetv"};
            }

            // Add tabs in specified order
            for (const std::string& item : order) {
                if (item == "home") {
                    tabFrame->addTab("Home", []() { return new HomeTab(); });
                } else if (item == "library") {
                    tabFrame->addTab("Library", []() { return new LibraryTab(); });
                } else if (item == "music") {
                    tabFrame->addTab("Music", []() { return new MusicTab(); });
                } else if (item == "search") {
                    tabFrame->addTab("Search", []() { return new SearchTab(); });
                } else if (item == "livetv" && hasLiveTV) {
                    tabFrame->addTab("Live TV", []() { return new LiveTVTab(); });
                }
            }
        }

        // Downloads tab (always available)
        tabFrame->addTab("Downloads", []() { return new DownloadsTab(); });

        // Settings always at the bottom
        tabFrame->addSeparator();
        tabFrame->addTab("Settings", []() { return new SettingsTab(); });

        // Focus first tab
        tabFrame->focusTab(0);
    }
}

void MainActivity::loadLibrariesToSidebar() {
    brls::Logger::debug("MainActivity: Loading libraries to sidebar...");

    // Add separator before libraries
    tabFrame->addSeparator();

    // Fetch libraries synchronously to maintain correct sidebar order
    PlexClient& client = PlexClient::getInstance();
    std::vector<LibrarySection> sections;

    if (client.fetchLibrarySections(sections)) {
        brls::Logger::info("MainActivity: Got {} library sections", sections.size());

        // Get hidden libraries setting
        std::string hiddenLibraries = Application::getInstance().getSettings().hiddenLibraries;

        // Helper to check if hidden
        auto isHidden = [&hiddenLibraries](const std::string& key) -> bool {
            if (hiddenLibraries.empty()) return false;
            std::string hidden = hiddenLibraries;
            size_t pos = 0;
            while ((pos = hidden.find(',')) != std::string::npos) {
                if (hidden.substr(0, pos) == key) return true;
                hidden.erase(0, pos + 1);
            }
            return (hidden == key);
        };

        // Add library tabs
        for (const auto& section : sections) {
            if (isHidden(section.key)) {
                brls::Logger::debug("MainActivity: Hiding library: {}", section.title);
                continue;
            }

            std::string key = section.key;
            std::string title = section.title;

            tabFrame->addTab(title, [key, title]() {
                return new LibrarySectionTab(key, title);
            });

            brls::Logger::debug("MainActivity: Added sidebar tab for library: {}", title);
        }
    } else {
        brls::Logger::error("MainActivity: Failed to fetch library sections");
    }
}

} // namespace vitaplex
