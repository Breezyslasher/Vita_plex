/**
 * VitaPlex - Video View Implementation
 * Renders video frames from MPV player
 */

#include "view/video_view.hpp"
#include "player/mpv_player.hpp"

namespace vitaplex {

VideoView::VideoView() {
    // Set up as full-screen by default
    this->setGrow(1.0f);
}

void VideoView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    // Draw parent first
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    if (!m_videoVisible) {
        return;
    }

    MpvPlayer& player = MpvPlayer::getInstance();

    // Render new frame if available
    player.render();

    // Get the video image handle
    int videoImage = player.getVideoImage();
    if (videoImage == 0) {
        return;
    }

    // Calculate video dimensions while maintaining aspect ratio
    float videoWidth = (float)player.getVideoWidth();
    float videoHeight = (float)player.getVideoHeight();
    float aspectRatio = videoWidth / videoHeight;

    float drawWidth = width;
    float drawHeight = height;
    float drawX = x;
    float drawY = y;

    // Fit video to view while maintaining aspect ratio
    if (width / height > aspectRatio) {
        // View is wider than video
        drawWidth = height * aspectRatio;
        drawX = x + (width - drawWidth) / 2.0f;
    } else {
        // View is taller than video
        drawHeight = width / aspectRatio;
        drawY = y + (height - drawHeight) / 2.0f;
    }

    // Create image pattern for video frame
    NVGpaint imgPaint = nvgImagePattern(vg, drawX, drawY, drawWidth, drawHeight, 0.0f, videoImage, 1.0f);

    // Draw video frame
    nvgBeginPath(vg);
    nvgRect(vg, drawX, drawY, drawWidth, drawHeight);
    nvgFillPaint(vg, imgPaint);
    nvgFill(vg);
}

brls::View* VideoView::create() {
    return new VideoView();
}

} // namespace vitaplex
