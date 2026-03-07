/**
 * VitaPlex - Search Tab
 * Search for media content
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"
#include "view/horizontal_scroll_row.hpp"

namespace vitaplex {

class SearchTab : public brls::Box {
public:
    SearchTab();
    ~SearchTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void performSearch(const std::string& query);
    void onItemSelected(const MediaItem& item);
    void populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items);
    void setupNavigationRoutes();

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_searchLabel = nullptr;
    brls::Label* m_resultsLabel = nullptr;

    // Scrollable content for organized results
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    // Category labels and rows
    brls::Label* m_moviesLabel = nullptr;
    HorizontalScrollRow* m_moviesRow = nullptr;
    brls::Label* m_showsLabel = nullptr;
    HorizontalScrollRow* m_showsRow = nullptr;
    brls::Label* m_episodesLabel = nullptr;
    HorizontalScrollRow* m_episodesRow = nullptr;
    brls::Label* m_musicLabel = nullptr;
    HorizontalScrollRow* m_musicRow = nullptr;

    std::string m_searchQuery;
    std::vector<MediaItem> m_results;
    std::vector<MediaItem> m_movies;
    std::vector<MediaItem> m_shows;
    std::vector<MediaItem> m_episodes;
    std::vector<MediaItem> m_music;

    // Alive flag + generation counter for crash prevention
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    int m_loadGeneration = 0;
};

} // namespace vitaplex
