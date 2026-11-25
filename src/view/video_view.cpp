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

#ifdef __ANDROID__
    // Direct-surface playback (Stage 4): mpv renders straight to
    // MpvSurface beneath the SDL surface. VideoView is now a layout
    // placeholder — drawing anything here would paint over the
    // transparent clear and hide the video. The Android compositor
    // shows the video through this rect because borealis clears with
    // alpha=0 (see brls/clear override in main.cpp) and we add no
    // pixels to the SDL surface for this region.
    return;
#else
    if (!m_videoVisible) {
        return;
    }

    // Black behind the picture, wherever it does not reach: the player's
    // background is clear during video, so letterboxing (a film wider than
    // the screen) and the moments before the first frame showed the app's
    // grey instead.
    //
    // A full-screen video also goes to the screen's edges, past the safe
    // area the UI is kept in (Application::setWindowSize; only iOS and
    // Apple TV report one: 80 px at the sides and 60 at the top and bottom
    // on Apple TV), which otherwise framed the picture in the app's
    // background. The UI is drawn translated by the inset and scaled by
    // windowScale, so the whole window is, in the view's units, from
    // -inset / scale and window / scale wide.
    const float scale = brls::Application::windowScale;
    const bool insets = brls::Application::safeInsetLeft > 0 || brls::Application::safeInsetTop > 0 ||
                        brls::Application::safeInsetRight > 0 || brls::Application::safeInsetBottom > 0;
    if (insets && scale > 0 && width >= brls::Application::contentWidth - 1 &&
        height >= brls::Application::contentHeight - 1) {
        x = -brls::Application::safeInsetLeft / scale;
        y = -brls::Application::safeInsetTop / scale;
        width = (float)brls::Application::windowWidth / scale;
        height = (float)brls::Application::windowHeight / scale;
    }
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGB(0, 0, 0));
    nvgFill(vg);

    MpvPlayer& player = MpvPlayer::getInstance();

    // Get the video image handle (rendered by onRenderUpdate via brls::sync)
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
#endif // !__ANDROID__
}

brls::View* VideoView::create() {
    return new VideoView();
}

} // namespace vitaplex
