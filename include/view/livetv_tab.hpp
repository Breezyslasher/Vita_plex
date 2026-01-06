/**
 * VitaPlex - Live TV Tab
 * Browse live TV channels and program guide
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"

namespace vitaplex {

// Program guide item
struct GuideProgram {
    std::string title;
    std::string summary;
    int64_t startTime = 0;
    int64_t endTime = 0;
    std::string ratingKey;
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
};

class LiveTVTab : public brls::Box {
public:
    LiveTVTab();

    void onFocusGained() override;

private:
    void loadChannels();
    void loadGuide();
    void loadRecordings();
    void buildEPGGrid();
    void onChannelSelected(const LiveTVChannel& channel);
    void onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel);
    void scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel);
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
    int m_hoursToShow = 4;         // Hours of programming to show
    bool m_loaded = false;
};

} // namespace vitaplex
