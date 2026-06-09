/**
 * VitaPlex - Main Activity implementation
 */

#include "activity/main_activity.hpp"
#include "view/home_tab.hpp"
#include "view/library_tab.hpp"
#include "view/library_section_tab.hpp"
#include "view/search_tab.hpp"
#include "view/settings_tab.hpp"
#include "view/debug_tab.hpp"
#include "view/livetv_tab.hpp"
#include "view/downloads_tab.hpp"
#include "view/music_tab.hpp"
#include "app/downloads_manager.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/async.hpp"
#include "platform/platform.hpp"

#include <algorithm>

namespace vitaplex {

// Cached library sections for sidebar mode
static std::vector<LibrarySection> s_cachedSections;

// Helper to calculate text width (approximate based on character count).
// Average character width scales with the platform's title font size, so
// desktop builds with larger UI text reserve more pixels per character
// than the Vita build does.
static int calculateTextWidth(const std::string& text) {
    const auto& ic = platform::getImageConstraints();
    // Rough glyph width ≈ 0.6 × font size (serviceable for ASCII sidebar labels).
    int charWidth = std::max(8, (ic.titleFontSize * 3) / 4);
    int padding   = 50;
    return static_cast<int>(text.length()) * charWidth + padding;
}

MainActivity::MainActivity() {
    brls::Logger::debug("MainActivity created");
}

brls::View* MainActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/main.xml");
}

void MainActivity::applySidebarSizingForViewport() {
    if (!tabFrame) return;
    AppSettings& settings = Application::getInstance().getSettings();
    const auto& ic = platform::getImageConstraints();

    // Calculate dynamic sidebar width based on content. min/max are
    // platform-tuned so desktop builds get a wider sidebar (260-450px)
    // while Vita stays at 200-350px — AND in portrait the same
    // platform tightens these to leave room for the grid.
    int sidebarWidth = ic.sidebarMinWidth;

    std::vector<std::string> standardTabs =
        {"Home", "Library", "Music", "Search", "Live TV", "Downloads", "Settings"};
    for (const auto& tab : standardTabs) {
        sidebarWidth = std::max(sidebarWidth, calculateTextWidth(tab));
    }

    if (settings.showLibrariesInSidebar && !Application::getInstance().isOfflineMode()) {
        // Reuse the section cache populated on the first pass — no point
        // refetching from Plex just because the user rotated.
        for (const auto& section : s_cachedSections) {
            sidebarWidth = std::max(sidebarWidth, calculateTextWidth(section.title));
        }
    }

    sidebarWidth = std::min(sidebarWidth, ic.sidebarMaxWidth);
    brls::View* sidebar = tabFrame->getView("brls/tab_frame/sidebar");
    if (sidebar) {
        if (settings.collapseSidebar) {
            int collapsedWidth = std::max(120, (ic.sidebarMinWidth * 4) / 5);
            sidebar->setWidth(collapsedWidth);
            brls::Logger::debug("MainActivity: Collapsed sidebar to {}px", collapsedWidth);
        } else {
            sidebar->setWidth(sidebarWidth);
            brls::Logger::debug("MainActivity: Dynamic sidebar width: {}px", sidebarWidth);
        }
    }
}

void MainActivity::onContentAvailable() {
    brls::Logger::debug("MainActivity content available");

    if (tabFrame) {
        AppSettings& settings = Application::getInstance().getSettings();

        // First pass populates s_cachedSections from Plex; afterwards
        // applySidebarSizingForViewport() reuses the cache.
        if (settings.showLibrariesInSidebar && !Application::getInstance().isOfflineMode()) {
            PlexClient& client = PlexClient::getInstance();
            std::vector<LibrarySection> sections;
            if (client.fetchLibrarySections(sections)) {
                s_cachedSections = sections;
            }
        }
        applySidebarSizingForViewport();

        // Re-apply sidebar width when the user rotates — portrait
        // constraints squeeze the bar smaller so the grid has room.
        platform::onOrientationChanged([this]() {
            applySidebarSizingForViewport();
        });

        bool isOffline = Application::getInstance().isOfflineMode();
        bool hasLiveTV = !isOffline && PlexClient::getInstance().hasLiveTV();

        // In offline mode, skip all server-dependent tabs (Home, Search, Library, etc.)
        // Only Downloads and Settings will be shown
        if (!isOffline) {
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
        }

        // Downloads tab (always available)
        tabFrame->addTab("Downloads", []() { return new DownloadsTab(); });

        // Debug and Settings always at the bottom
        tabFrame->addSeparator();
        if (settings.showDebugTab) {
            tabFrame->addTab("Debug", []() { return new DebugTab(); });
        }
        tabFrame->addTab("Settings", []() { return new SettingsTab(); });

        // Focus first tab
        tabFrame->focusTab(0);

        // Register BUTTON_B on the root content view (parent of tabFrame) so it
        // intercepts back/circle regardless of which child has focus. When a
        // dialog closes, borealis may restore focus to the root Box instead of
        // a child inside tabFrame, which would bypass a handler registered only
        // on tabFrame and let AppletFrame show the "exit this app" dialog.
        brls::View* rootBox = tabFrame->getParent();
        if (rootBox) {
            rootBox->registerAction("", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
                MusicQueue& queue = MusicQueue::getInstance();
                if (!queue.isEmpty() && queue.getCurrentIndex() >= 0) {
                    auto* playerActivity = PlayerActivity::createResumeQueue();
                    brls::Application::pushActivity(playerActivity);
                }
                // Always return true to prevent the exit confirmation dialog
                return true;
            });
        }
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
            std::string type = section.type;

            tabFrame->addTab(title, [key, title, type]() {
                return new LibrarySectionTab(key, title, type);
            });

            brls::Logger::debug("MainActivity: Added sidebar tab for library: {}", title);
        }
    } else {
        brls::Logger::error("MainActivity: Failed to fetch library sections");
    }
}

} // namespace vitaplex
