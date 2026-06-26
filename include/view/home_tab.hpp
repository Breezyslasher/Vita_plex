/**
 * VitaPlex - Home Tab
 * A "Home" title over horizontal rails: Continue Watching, Recent Channels
 * (Live TV), and Recently Added movies / shows / music.
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include <vector>
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
    void loadRecentChannels();          // Live TV "Recent Channels" rail
    void onItemSelected(const MediaItem& item);

    // Section header: gold accent rect + title. Returns the row Box.
    brls::Box* makeSectionHeader(const std::string& title);

    // Helper to create a media row with horizontal scrolling
    HorizontalScrollRow* createMediaRow();
    void populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items, bool directPlay = false);
    void populateChannelRow();          // build channel cells into m_recentChannelsRow
    void tuneChannel(const LiveTVChannel& channel);  // same tune path as the Live TV tab

    // Vertical scroll container
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    brls::Label* m_titleLabel = nullptr;

    // Continue Watching section
    HorizontalScrollRow* m_continueWatchingRow = nullptr;

    // Recent Channels (Live TV) section — header + row hidden until channels load.
    brls::Box*           m_recentChannelsHeader = nullptr;
    HorizontalScrollRow* m_recentChannelsRow = nullptr;

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
    std::vector<LiveTVChannel> m_recentChannels;
    bool m_loaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    // ImageLoader needs an atomic flag; recycled per-build of the channel row so
    // in-flight loads bail when their target Image is freed.
    std::shared_ptr<std::atomic<bool>> m_channelImgAlive;
};

} // namespace vitaplex
