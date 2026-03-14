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
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

private:
    brls::View* findFirstFocusableInBox(brls::Box* box);
    bool isDescendantOf(brls::View* view, brls::View* ancestor);
    void loadChannels();
    void refreshCurrentPrograms();  // Lightweight refresh: only update "now playing" info
    void loadGuide();
    void loadRecordings();
    void buildEPGGrid();
    void updateQuickAccessPrograms();  // Update current program labels without rebuilding
    void onChannelSelected(const LiveTVChannel& channel);
    void onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel);
    void scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel);
    void cancelRecording(const DVRRecording& recording);
    std::string formatTime(int64_t timestamp);

    // UI Components
    brls::Label* m_titleLabel = nullptr;
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    // Channels quick access section
    brls::Label* m_channelsLabel = nullptr;
    brls::HScrollingFrame* m_channelsRow = nullptr;
    brls::Box* m_channelsContent = nullptr;

    // EPG Guide section
    brls::Label* m_guideLabel = nullptr;
    brls::Box* m_guideContainer = nullptr;      // Contains time header + grid
    brls::HScrollingFrame* m_timeHeaderScroll = nullptr;
    brls::Box* m_timeHeaderBox = nullptr;       // Horizontal time slots
    brls::ScrollingFrame* m_guideScrollV = nullptr;  // Vertical scroll for channels
    brls::Box* m_guideBox = nullptr;            // Contains channel rows

    // DVR section
    brls::Label* m_dvrLabel = nullptr;
    brls::HScrollingFrame* m_dvrRow = nullptr;
    brls::Box* m_dvrContent = nullptr;

    // Data
    std::vector<LiveTVChannel> m_channels;
    std::vector<EPGChannel> m_epgChannels;
    std::vector<DVRRecording> m_recordings;
    int64_t m_guideStartTime = 0;  // Current time rounded to 30 min
    int m_hoursToShow = 12;        // Hours of programming to show (12 hours)
    bool m_loaded = false;
    int64_t m_lastFullLoadTime = 0;   // Timestamp of last full channel/EPG load
    int64_t m_lastRefreshTime = 0;    // Timestamp of last "now playing" refresh

    // Quick access program labels (for lightweight refresh)
    std::vector<brls::Label*> m_quickAccessProgLabels;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
