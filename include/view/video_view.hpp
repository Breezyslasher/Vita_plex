/**
 * VitaPlex - Video View
 * Custom view for rendering video frames from MPV player
 */

#pragma once

#include <borealis.hpp>

namespace vitaplex {

/**
 * VideoView - renders video frames from MPV player using NanoVG
 */
class VideoView : public brls::Box {
public:
    VideoView();
    ~VideoView() override = default;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;

    // Set visibility of video display
    void setVideoVisible(bool visible) { m_videoVisible = visible; }
    bool isVideoVisible() const { return m_videoVisible; }

    static brls::View* create();

private:
    bool m_videoVisible = false;
};

} // namespace vitaplex
