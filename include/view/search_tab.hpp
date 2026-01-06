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
    void populateRow(brls::Box* rowContent, const std::vector<MediaItem>& items);

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_searchLabel = nullptr;
    brls::Label* m_resultsLabel = nullptr;

    // Scrollable content for organized results
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    // Category rows
    brls::HScrollingFrame* m_moviesRow = nullptr;
    brls::Box* m_moviesContent = nullptr;
    brls::HScrollingFrame* m_showsRow = nullptr;
    brls::Box* m_showsContent = nullptr;
    brls::HScrollingFrame* m_episodesRow = nullptr;
    brls::Box* m_episodesContent = nullptr;
    brls::HScrollingFrame* m_musicRow = nullptr;
    brls::Box* m_musicContent = nullptr;

    std::string m_searchQuery;
    std::vector<MediaItem> m_results;
    std::vector<MediaItem> m_movies;
    std::vector<MediaItem> m_shows;
    std::vector<MediaItem> m_episodes;
    std::vector<MediaItem> m_music;
};

} // namespace vitaplex
