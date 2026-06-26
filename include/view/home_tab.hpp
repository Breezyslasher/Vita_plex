/**
 * VitaPlex - Home Tab
 * Direction A: a cinematic featured hero on top of the existing horizontal
 * rails (Continue Watching, Recent Channels [Live TV], Recently Added …).
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
    void loadRecentChannels();          // NEW: Live TV "Recent Channels" rail
    void onItemSelected(const MediaItem& item);

    // Featured hero (Direction A). buildHeader() picks hero vs plain title from
    // the "Show featured banner" setting; buildHero() paints the cinematic
    // header; updateHeroFromData() chooses the hero item from already-loaded
    // data and rebuilds; rebuildHeader() rebuilds while preserving focus.
    void buildHeader();
    void buildHero(brls::Box* parent);
    void rebuildHeader();
    void updateHeroFromData();

    // Section header: gold accent rect + title. Returns the row Box.
    brls::Box* makeSectionHeader(const std::string& title);

    // Helper to create a media row with horizontal scrolling
    HorizontalScrollRow* createMediaRow();
    void populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items, bool directPlay = false);
    void populateChannelRow();          // NEW: build channel cells into m_recentChannelsRow
    void tuneChannel(const LiveTVChannel& channel);  // NEW: same tune path as the Live TV tab

    // Vertical scroll container
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    // Header region (cinematic hero OR plain "Home" title), rebuilt on demand.
    brls::Box*   m_heroContainer = nullptr;
    brls::Button* m_heroPlayButton = nullptr;   // default focus target when the hero is on
    // Rails live in their own side-padded container so the hero can bleed full width.
    brls::Box*   m_railsContainer = nullptr;

    brls::Label* m_titleLabel = nullptr;        // plain-title path only

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

    // Featured hero state, chosen from already-loaded data (no new fetch).
    MediaItem m_heroItem;
    bool m_heroResolved = false;        // a hero item has been chosen
    bool m_heroFromContinue = false;    // Continue Watching wins over Recently Added
    bool m_lastShowFeaturedBanner = true;   // detect the setting toggle in onFocusGained

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    // ImageLoader needs an atomic flag; kept separate from m_alive (a bool) and
    // recycled per-build so in-flight loads bail when their target is freed.
    std::shared_ptr<std::atomic<bool>> m_heroImgAlive;
    std::shared_ptr<std::atomic<bool>> m_channelImgAlive;
    // Orientation callback lives for the tab's whole lifetime (m_alive is
    // recycled on focus, which would orphan the callback after one cycle).
    std::shared_ptr<bool> m_orientationAlive = std::make_shared<bool>(true);
};

} // namespace vitaplex
