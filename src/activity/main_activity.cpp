/**
 * VitaPlex - Main Activity implementation
 */

#include "activity/main_activity.hpp"
#include "view/home_tab.hpp"
#include "view/library_tab.hpp"
#include "view/library_section_tab.hpp"
#include "view/search_tab.hpp"
#include "view/settings_tab.hpp"
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

        // Add tabs
        tabFrame->addTab("Home", []() { return new HomeTab(); });

        // Library handling based on settings
        if (settings.showLibrariesInSidebar) {
            // Load libraries and add them as individual sidebar tabs
            loadLibrariesToSidebar();
        } else {
            // Single Library tab showing all sections
            tabFrame->addTab("Library", []() { return new LibraryTab(); });
        }

        tabFrame->addTab("Search", []() { return new SearchTab(); });
        tabFrame->addTab("Settings", []() { return new SettingsTab(); });

        // Focus first tab
        tabFrame->focusTab(0);
    }
}

void MainActivity::loadLibrariesToSidebar() {
    brls::Logger::debug("MainActivity: Loading libraries to sidebar...");

    // Add separator before libraries
    tabFrame->addSeparator();

    // Fetch libraries asynchronously and add them as tabs
    asyncRun([this]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<LibrarySection> sections;

        if (client.fetchLibrarySections(sections)) {
            brls::Logger::info("MainActivity: Got {} library sections", sections.size());

            // Get hidden libraries setting
            std::string hiddenLibraries = Application::getInstance().getSettings().hiddenLibraries;

            // Filter out hidden sections
            std::vector<LibrarySection> visibleSections;
            for (const auto& section : sections) {
                // Check if hidden
                bool hidden = false;
                if (!hiddenLibraries.empty()) {
                    std::string checkHidden = hiddenLibraries;
                    size_t pos = 0;
                    while ((pos = checkHidden.find(',')) != std::string::npos) {
                        if (checkHidden.substr(0, pos) == section.key) {
                            hidden = true;
                            break;
                        }
                        checkHidden.erase(0, pos + 1);
                    }
                    if (!hidden && checkHidden == section.key) {
                        hidden = true;
                    }
                }

                if (!hidden) {
                    visibleSections.push_back(section);
                } else {
                    brls::Logger::debug("MainActivity: Hiding library: {}", section.title);
                }
            }

            s_cachedSections = visibleSections;

            // Add library tabs on main thread
            brls::sync([this, visibleSections]() {
                for (const auto& section : visibleSections) {
                    std::string key = section.key;
                    std::string title = section.title;

                    tabFrame->addTab(title, [key, title]() {
                        return new LibrarySectionTab(key, title);
                    });

                    brls::Logger::debug("MainActivity: Added sidebar tab for library: {}", title);
                }
            });
        } else {
            brls::Logger::error("MainActivity: Failed to fetch library sections");
        }
    });
}

} // namespace vitaplex
