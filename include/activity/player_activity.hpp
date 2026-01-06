/**
 * VitaPlex - Player Activity
 * Video playback screen with controls
 */

#pragma once

#include <borealis.hpp>
#include <borealis/core/timer.hpp>
#include <string>

namespace vitaplex {

class PlayerActivity : public brls::Activity {
public:
    PlayerActivity(const std::string& mediaKey);

    brls::View* createContentView() override;

    void onContentAvailable() override;

    void willDisappear(bool resetState) override;

private:
    void loadMedia();
    void updateProgress();
    void togglePlayPause();
    void seek(int seconds);

    std::string m_mediaKey;
    bool m_isPlaying = false;
    brls::RepeatingTimer m_updateTimer;

    BRLS_BIND(brls::Box, playerContainer, "player/container");
    BRLS_BIND(brls::Label, titleLabel, "player/title");
    BRLS_BIND(brls::Label, timeLabel, "player/time");
    BRLS_BIND(brls::Slider, progressSlider, "player/progress");
    BRLS_BIND(brls::Box, controlsBox, "player/controls");
};

} // namespace vitaplex
