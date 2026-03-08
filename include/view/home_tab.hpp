/**
 * VitaPlex - Home Tab
 * Shows continue watching, recently added movies, shows, and music
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"
#include "view/horizontal_scroll_row.hpp"

namespace vitaplex {

class HomeTab : public brls::Box {
public:
    HomeTab();
    ~HomeTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void loadContent();
    void onItemSelected(const MediaItem& item);

    // Helper to create a media row with horizontal scrolling
    HorizontalScrollRow* createMediaRow();
    void populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items, bool directPlay = false);

    // Vertical scroll container
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    brls::Label* m_titleLabel = nullptr;

    // Continue Watching section
    HorizontalScrollRow* m_continueWatchingRow = nullptr;

    // Recently Added Movies section
    HorizontalScrollRow* m_moviesRow = nullptr;

    // Recently Added TV Shows section
    HorizontalScrollRow* m_showsRow = nullptr;

    // Recently Added Music section
    HorizontalScrollRow* m_musicRow = nullptr;

    std::vector<MediaItem> m_continueWatching;
    std::vector<MediaItem> m_recentMovies;
    std::vector<MediaItem> m_recentShows;
    std::vector<MediaItem> m_recentMusic;
    bool m_loaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
