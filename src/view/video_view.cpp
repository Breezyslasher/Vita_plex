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

    // mpv draws each frame into a render target of a fixed size
    // (getVideoWidth x getVideoHeight, 1920x1080 on a PC), the film fitted
    // into it with black bars of its own. Fitting that whole target into the
    // view added the target's bars to the view's: a 2.41:1 film in a window
    // wider than 16:9 had black down both sides as well as above and below.
    // So the part of the target the film covers is what is fitted, by the
    // film's own shape (its display size, the pixel aspect applied).
    const float texW = (float)player.getVideoWidth();
    const float texH = (float)player.getVideoHeight();
    if (texW <= 0 || texH <= 0) return;
    const auto& info = player.getPlaybackInfo();
    float aspectRatio = texW / texH;
    if (info.displayWidth > 0 && info.displayHeight > 0)
        aspectRatio = (float)info.displayWidth / (float)info.displayHeight;

    // The film's rectangle inside the target, as mpv centres it.
    float srcX = 0, srcY = 0, srcW = texW, srcH = texH;
    if (aspectRatio > texW / texH) {
        srcH = texW / aspectRatio;
        srcY = (texH - srcH) / 2.0f;
    } else {
        srcW = texH * aspectRatio;
        srcX = (texW - srcW) / 2.0f;
    }

    float drawWidth = width;
    float drawHeight = height;
    float drawX = x;
    float drawY = y;

    // Fit the film to the view, keeping its shape
    if (width / height > aspectRatio) {
        // View is wider than video
        drawWidth = height * aspectRatio;
        drawX = x + (width - drawWidth) / 2.0f;
    } else {
        // View is taller than video
        drawHeight = width / aspectRatio;
        drawY = y + (height - drawHeight) / 2.0f;
    }

    // The whole target placed so that the film's part lands on the draw rect.
    const float sx = drawWidth / srcW;
    const float sy = drawHeight / srcH;
    NVGpaint imgPaint = nvgImagePattern(vg, drawX - srcX * sx, drawY - srcY * sy, texW * sx,
                                        texH * sy, 0.0f, videoImage, 1.0f);

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
