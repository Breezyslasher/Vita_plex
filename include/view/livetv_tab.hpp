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
    void onChannelSelected(const LiveTVChannel& channel);
    void onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel);
    void scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel);

    // UI Components
    brls::Label* m_titleLabel = nullptr;
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    // Channels section
    brls::HScrollingFrame* m_channelsRow = nullptr;
    brls::Box* m_channelsContent = nullptr;

    // Guide section
    brls::Box* m_guideBox = nullptr;

    // DVR section
    brls::Label* m_dvrLabel = nullptr;
    brls::HScrollingFrame* m_dvrRow = nullptr;
    brls::Box* m_dvrContent = nullptr;

    // Data
    std::vector<LiveTVChannel> m_channels;
    std::vector<DVRRecording> m_recordings;
    bool m_loaded = false;
};

} // namespace vitaplex
