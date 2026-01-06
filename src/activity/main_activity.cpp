/**
 * VitaPlex - Main Activity implementation
 */

#include "activity/main_activity.hpp"
#include "view/home_tab.hpp"
#include "view/library_tab.hpp"
#include "view/search_tab.hpp"
#include "view/settings_tab.hpp"

namespace vitaplex {

MainActivity::MainActivity() {
    brls::Logger::debug("MainActivity created");
}

brls::View* MainActivity::createContentView() {
    return brls::View::createFromXMLResource("xml/activity/main.xml");
}

void MainActivity::onContentAvailable() {
    brls::Logger::debug("MainActivity content available");

    if (tabFrame) {
        // Add tabs
        tabFrame->addTab("Home", new HomeTab());
        tabFrame->addTab("Library", new LibraryTab());
        tabFrame->addTab("Search", new SearchTab());
        tabFrame->addTab("Settings", new SettingsTab());

        // Focus first tab
        tabFrame->focusTab(0);
    }
}

} // namespace vitaplex
