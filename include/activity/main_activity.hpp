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

    brls::View* createContentView() override;

    void onContentAvailable() override;

private:
    void loadLibrariesToSidebar();
    // Re-apply the sidebar width / collapse decision based on the
    // current viewport's image constraints. Hoisted out of
    // onContentAvailable() so it can be called again on orientation
    // change (portrait constraints tighten the sidebar so the grid has
    // room).
    void applySidebarSizingForViewport();

    BRLS_BIND(brls::TabFrame, tabFrame, "main/tab_frame");
};

} // namespace vitaplex
