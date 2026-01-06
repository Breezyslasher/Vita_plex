/**
 * VitaPlex - Search Tab
 * Search for media content
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

class SearchTab : public brls::Box {
public:
    SearchTab();

    void onFocusGained() override;

private:
    void performSearch(const std::string& query);
    void onItemSelected(const MediaItem& item);

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_searchLabel = nullptr;
    brls::Label* m_resultsLabel = nullptr;
    RecyclingGrid* m_resultsGrid = nullptr;

    std::string m_searchQuery;
    std::vector<MediaItem> m_results;
};

} // namespace vitaplex
