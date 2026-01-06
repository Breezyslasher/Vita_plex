/**
 * VitaPlex - Home Tab
 * Shows continue watching, recently added, and hubs
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

class HomeTab : public brls::Box {
public:
    HomeTab();

    void onFocusGained() override;

private:
    void loadContent();
    void onItemSelected(const MediaItem& item);

    brls::Label* m_titleLabel = nullptr;
    brls::Box* m_continueWatchingBox = nullptr;
    brls::Box* m_recentlyAddedBox = nullptr;

    std::vector<MediaItem> m_continueWatching;
    std::vector<MediaItem> m_recentlyAdded;
    bool m_loaded = false;
};

} // namespace vitaplex
