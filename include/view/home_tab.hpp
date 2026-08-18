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
    // Culls rails/headers scrolled out of the page before drawing —
    // borealis never culls nested Boxes, so below-the-fold rails (and
    // every card in them) were drawn every frame.
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    void loadContent();
    void loadRecentChannels();          // Live TV rails: Recent Channels + Shows On Now
    void onItemSelected(const MediaItem& item);

    // Section header: gold accent rect + title. Returns the row Box.
    brls::Box* makeSectionHeader(const std::string& title);

    // Helper to create a media row with horizontal scrolling
    HorizontalScrollRow* createMediaRow();
    // preferPoster renders show posters instead of the landscape stills
    // an episode-typed item would otherwise get — used by "Shows On Now".
    void populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items,
                     bool directPlay = false, bool preferPoster = false);
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
    // The provider's "On Now" rails, at the bottom of the page. All
    // hidden until the server actually returns items for them.
    brls::Box*           m_showsOnNowHeader = nullptr;
    HorizontalScrollRow* m_showsOnNowRow = nullptr;
    brls::Box*           m_moviesOnNowHeader = nullptr;
    HorizontalScrollRow* m_moviesOnNowRow = nullptr;
    brls::Box*           m_sportsOnNowHeader = nullptr;
    HorizontalScrollRow* m_sportsOnNowRow = nullptr;

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
    // Server-computed rails. When these are populated the corresponding
    // rail renders from them; m_recentChannels is only used for the
    // EPG-grid fallback on servers that don't advertise the hubs.
    std::vector<MediaItem> m_recentChannelItems;
    std::vector<MediaItem> m_showsOnNow;
    std::vector<MediaItem> m_moviesOnNow;
    std::vector<MediaItem> m_sportsOnNow;
    bool m_loaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    // ImageLoader needs an atomic flag; recycled per-build of the channel row so
    // in-flight loads bail when their target Image is freed.
    std::shared_ptr<std::atomic<bool>> m_channelImgAlive;
};

} // namespace vitaplex
