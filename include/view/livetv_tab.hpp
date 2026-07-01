/**
 * VitaPlex - Live TV Tab
 * Browse live TV channels and program guide
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/plex_client.hpp"

namespace vitaplex {

// The custom-drawn EPG grid (file-local in livetv_tab.cpp) — the whole
// guide is ONE view that paints header, channel column, cells and the
// now-line itself instead of a ~2500-view borealis forest.
class EpgGridView;

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
    bool isDescendantOf(brls::View* view, brls::View* ancestor);

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

    // Hover-driven hero updates are debounced: focus events only record the
    // wanted channel/program here, and draw() applies it once focus has
    // rested. Applying immediately cost a dozen setText/setWidth calls (each
    // a synchronous full-tree relayout) plus a thumbnail HTTP fetch per
    // dpad press — the single biggest cost of navigating the guide on Vita.
    void queueHeroForChannel(const LiveTVChannel& channel);
    void queueHeroForProgram(const LiveTVChannel& channel, const GuideProgram& program);
    void applyPendingHero();

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

    // EPG Guide section. The guide is a single custom-drawn view — it
    // owns its own data, scroll offsets and virtual focus cursor, so the
    // tab keeps no per-row/per-cell state at all.
    brls::Label* m_guideLabel = nullptr;
    brls::Box* m_guideContainer = nullptr;      // Card that hosts the grid
    EpgGridView* m_grid = nullptr;

    // Per-frame cost accounting (logged on Vita every few hundred frames
    // so a hardware log pinpoints where guide frame time goes).
    int64_t m_perfLastFrameUs = 0;
    int64_t m_perfFrameUs = 0;
    int64_t m_perfDrawUs  = 0;
    int     m_perfFrames  = 0;

    // Data
    std::vector<LiveTVChannel> m_channels;
    std::vector<EPGChannel> m_epgChannels;
    std::vector<DVRRecording> m_recordings;
    int64_t m_guideStartTime = 0;  // Current time rounded to 30 min
    int m_hoursToShow = 12;        // Hours of programming to show (12 hours)
    bool m_loaded = false;
    int64_t m_lastFullLoadTime = 0;   // Timestamp of last full channel/EPG load
    int64_t m_lastRefreshTime = 0;    // Timestamp of last "now playing" refresh

    // Debounced hero update (see queueHeroFor* / applyPendingHero).
    LiveTVChannel m_pendingHeroChannel;
    GuideProgram  m_pendingHeroProgram;
    bool    m_pendingHeroHasProgram = false;
    bool    m_heroUpdatePending     = false;
    int64_t m_lastHoverUs           = 0;   // CPU time of the last hover event

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
