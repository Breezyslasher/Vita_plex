/**
 * VitaPlex - Library Section Tab
 * Shows content for a single library section (for sidebar mode)
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

class LibrarySectionTab : public brls::Box {
public:
    LibrarySectionTab(const std::string& sectionKey, const std::string& title);

    void onFocusGained() override;

private:
    void loadContent();
    void onItemSelected(const MediaItem& item);

    std::string m_sectionKey;
    std::string m_title;

    brls::Label* m_titleLabel = nullptr;
    RecyclingGrid* m_contentGrid = nullptr;

    std::vector<MediaItem> m_items;
    bool m_loaded = false;
};

} // namespace vitaplex
