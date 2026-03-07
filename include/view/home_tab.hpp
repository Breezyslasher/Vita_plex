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
    brls::HScrollingFrame* createMediaRow(brls::Box** contentOut);
    void populateRow(brls::Box* rowContent, const std::vector<MediaItem>& items);

    // Vertical scroll container
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    brls::Label* m_titleLabel = nullptr;

    // Continue Watching section
    brls::HScrollingFrame* m_continueWatchingRow = nullptr;
    brls::Box* m_continueWatchingContent = nullptr;

    // Recently Added Movies section
    brls::HScrollingFrame* m_moviesRow = nullptr;
    brls::Box* m_moviesContent = nullptr;

    // Recently Added TV Shows section
    brls::HScrollingFrame* m_showsRow = nullptr;
    brls::Box* m_showsContent = nullptr;

    // Recently Added Music section
    brls::HScrollingFrame* m_musicRow = nullptr;
    brls::Box* m_musicContent = nullptr;

    std::vector<MediaItem> m_continueWatching;
    std::vector<MediaItem> m_recentMovies;
    std::vector<MediaItem> m_recentShows;
    std::vector<MediaItem> m_recentMusic;
    bool m_loaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
