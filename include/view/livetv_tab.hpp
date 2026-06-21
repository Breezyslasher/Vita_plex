/**
 * VitaPlex - Live TV Tab
 * Browse live TV channels and program guide
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/plex_client.hpp"

namespace vitaplex {

// Program guide item
struct GuideProgram {
    std::string title;
    std::string summary;
    int64_t startTime = 0;
    int64_t endTime = 0;
    std::string ratingKey;
    std::string metadataKey;  // EPG metadata path for transcode/recording
    std::string thumb;        // Show poster / episode still for the hero card
    bool isRecording = false;
    int durationMinutes = 30;  // Duration in minutes
};

// Channel with programs for EPG grid
struct EPGChannel {
    LiveTVChannel channel;
    std::vector<GuideProgram> programs;
};

// DVR Recording
struct DVRRecording {
    std::string ratingKey;
    std::string title;
    std::string summary;
    int64_t scheduledTime = 0;
    std::string status;  // scheduled, recording, completed
    std::string channelTitle;
    std::string mediaSubscriptionId;  // For cancellation
};

class LiveTVTab : public brls::Box {
public:
    LiveTVTab();
    ~LiveTVTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

private:
    brls::View* findFirstFocusableInBox(brls::Box* box);
    brls::View* findLastFocusableInBox(brls::Box* box);
    bool isDescendantOf(brls::View* view, brls::View* ancestor);

    // Hide rows/cards that have scrolled out of their viewport so borealis
    // doesn't draw the whole off-screen subtree every frame (see draw()).
    void cullToViewport(brls::Box* content, brls::View* viewport, bool vertical);
    void loadChannels();
    void refreshCurrentPrograms();  // Lightweight refresh: only update "now playing" info
    void loadGuide();
    void loadRecordings();
    void buildEPGGrid();
    void onChannelSelected(const LiveTVChannel& channel);
    void onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel);
    void scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel);
    void cancelRecording(const DVRRecording& recording);
    std::string formatTime(int64_t timestamp);

    // New layout helpers
    void buildHero();                                // build the empty hero shell
    void updateHeroForChannel(const LiveTVChannel& channel);  // populate hero with current program
    void updateHeroForProgram(const LiveTVChannel& channel,   // populate hero with a specific
                              const GuideProgram& program);    // program (hover-driven)
    void resizeHeroThumbToImage(brls::Image* img);   // resize the hero thumb box to the loaded
                                                     // image's natural aspect (no letterbox)
    void updateCurrentTimeLine();    // reposition the cyan "now" rule each second

    // UI Components
    brls::Label* m_titleLabel = nullptr;
    brls::Box* m_scrollContent = nullptr;       // Direct child of the tab — no outer page scroll

    // On-Now hero
    brls::Box*   m_heroBox          = nullptr;
    brls::Image* m_heroThumb        = nullptr;
    brls::Box*   m_heroThumbHolder  = nullptr;  // fixed-size holder so the image keeps its slot while loading
    brls::Box*   m_heroLiveBadge    = nullptr;
    brls::Label* m_heroChannelName  = nullptr;
    brls::Label* m_heroChannelId    = nullptr;
    brls::Label* m_heroTitleLabel   = nullptr;
    brls::Label* m_heroSummaryLabel = nullptr;
    brls::Label* m_heroStartLabel   = nullptr;
    brls::Label* m_heroEndLabel     = nullptr;
    brls::Label* m_heroPctLabel     = nullptr;
    brls::Box*   m_heroProgressTrack = nullptr;
    brls::Box*   m_heroProgressFill  = nullptr;
    brls::Box*   m_heroWatchBtn     = nullptr;
    brls::Box*   m_heroRecordBtn    = nullptr;
    LiveTVChannel m_heroChannel;
    GuideProgram  m_heroProgram;
    bool          m_heroProgramValid = false;
    std::shared_ptr<std::atomic<bool>> m_heroThumbAlive;  // ImageLoader cancel handle

    // EPG Guide section
    brls::Label* m_guideLabel = nullptr;
    brls::Box* m_guideContainer = nullptr;      // Contains time header + grid
    brls::HScrollingFrame* m_timeHeaderScroll = nullptr;
    brls::Box* m_timeHeaderBox = nullptr;       // Horizontal time slots
    brls::ScrollingFrame* m_guideScrollV = nullptr;  // Vertical scroll inside the guide block
    brls::Box* m_guideBox = nullptr;            // Contains channel rows; scrolls inside m_guideScrollV
    brls::Box* m_currentTimeLine = nullptr;     // Absolute-positioned cyan rule over the program area
    // Per-row HScrollingFrame for the program cells. The channel column
    // sits *outside* this scroll on the left so it stays put when the
    // programs scroll horizontally. draw() reads the focused row's
    // offset and applies it to every other row + the time header so
    // they all move together.
    std::vector<brls::HScrollingFrame*> m_rowProgramScrolls;

    // Batch text rendering for the EPG cells. The patched nanovg lets
    // us flush every visible cell's title (and separately, every
    // subtitle) as a single render call instead of one per Label —
    // ~100 cells per build x ~2 labels each = ~200 draw calls otherwise.
    // Cells themselves are intentionally label-less; draw() walks this
    // vector after the standard Box::draw to paint the text on top.
    struct EpgCellInfo {
        brls::Box* cell = nullptr;     // owns the rect / focus / background
        brls::HScrollingFrame* scroll = nullptr;  // viewport the cell lives in
        std::string title;
        std::string subtitle;          // start-end + " · on now" if currently airing
    };
    std::vector<EpgCellInfo> m_epgCells;

    // Data
    std::vector<LiveTVChannel> m_channels;
    std::vector<EPGChannel> m_epgChannels;
    std::vector<DVRRecording> m_recordings;
    int64_t m_guideStartTime = 0;  // Current time rounded to 30 min
    int m_hoursToShow = 12;        // Hours of programming to show (12 hours)
    bool m_loaded = false;
    int64_t m_lastFullLoadTime = 0;   // Timestamp of last full channel/EPG load
    int64_t m_lastRefreshTime = 0;    // Timestamp of last "now playing" refresh

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
