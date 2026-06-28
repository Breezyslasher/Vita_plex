/**
 * VitaPlex - Main Activity
 * Main navigation with tabs for Home, Library, Search, Settings
 */

#pragma once

#include <borealis.hpp>

namespace vitaplex {

class MainActivity : public brls::Activity {
public:
    MainActivity();
    ~MainActivity() override;

    brls::View* createContentView() override;

    void onContentAvailable() override;

    // The live MainActivity (sidebar host), or nullptr if none is on screen.
    // Lets the inline Sidebar editor rebuild the sidebar after committing.
    static MainActivity* getInstance() { return s_instance; }

    // Tear down and re-add every sidebar tab from the current settings
    // (order + hidden sets). Safe to call while the activity is on screen.
    void rebuildSidebar();

private:
    // Add Home, then the ordered + visible movable items (each library, then
    // Search / Live TV / Downloads), then Settings, honoring sidebarOrder +
    // hiddenLibraries + hiddenSidebarItems. Shared by the first build
    // (onContentAvailable) and rebuildSidebar().
    void buildSidebarTabs();
    // Re-apply the sidebar width / collapse decision based on the
    // current viewport's image constraints. Hoisted out of
    // onContentAvailable() so it can be called again on orientation
    // change (portrait constraints tighten the sidebar so the grid has
    // room).
    void applySidebarSizingForViewport();

    BRLS_BIND(brls::TabFrame, tabFrame, "main/tab_frame");

    static MainActivity* s_instance;
};

} // namespace vitaplex
