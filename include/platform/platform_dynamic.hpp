#pragma once

/**
 * Shared formula that derives an `ImageConstraints` struct from a live
 * screen resolution. Used by platform_desktop.cpp and platform_android.cpp
 * so the UI adapts to the actual monitor / phone / tablet / foldable
 * display the program is currently running on.
 *
 * Fixed-resolution consoles (Vita, PS4, Switch) do NOT use this header —
 * they return hand-tuned constants directly from their own
 * platform_<name>.cpp files to preserve their tight memory budgets and
 * hardware-specific hardware quirks. Only the variable-resolution
 * platforms call this.
 *
 * The formula picks sizes by a linear scale factor against a 1080p
 * reference (so a 1920×1080 monitor is scale = 1.0, a 2560×1440 monitor
 * is scale ≈ 1.33, a 3840×2160 4K monitor is scale = 2.0, and a 1280×720
 * small laptop is scale ≈ 0.67). The grid column count is then derived
 * from the actual pixel width so an ultrawide 3440×1440 panel ends up
 * with more columns instead of just bigger posters. Everything else
 * (cache slots, library page size, text truncation budgets, thumbnail
 * request resolutions) is tiered by total pixel count.
 */

#include "platform/platform.hpp"

#include <algorithm>
#include <cmath>

namespace vitaplex {
namespace platform {

inline ImageConstraints computeDynamicImageConstraints(int widthPx, int heightPx) {
    // Clamp pathological inputs (0×0 before window creation, ridiculous
    // 8K+ displays that would overflow the cache sizing below).
    if (widthPx  < 640)  widthPx  = 640;
    if (heightPx < 360)  heightPx = 360;
    if (widthPx  > 7680) widthPx  = 7680;
    if (heightPx > 4320) heightPx = 4320;

    // Linear scale factor keyed on horizontal resolution (1920 = 1.0).
    // Horizontal is the right axis because grid UIs are width-dominated:
    // a portrait foldable cover screen (904×2316) needs SMALL posters so
    // multiple columns still fit across its narrow width, while an ultra-
    // wide 3440×1440 monitor needs MORE columns at the same poster size.
    // Basing scale on height would make the cover screen use huge 340px
    // posters that overflow the 904-pixel width.
    float scale = static_cast<float>(widthPx) / 1920.0f;
    if (scale < 0.60f) scale = 0.60f;   // phone / small-laptop floor
    if (scale > 2.00f) scale = 2.00f;   // 4K / 5K ceiling

    auto scl = [scale](int base) {
        int v = static_cast<int>(std::lround(base * scale));
        return v < 1 ? 1 : v;
    };

    ImageConstraints c{};

    // --- Cell geometry -------------------------------------------------
    c.posterWidth        = scl(170);
    c.posterHeight       = scl(255);
    c.squareCoverSize    = scl(170);
    c.landscapeWidth     = scl(240);
    c.landscapeHeight    = scl(135);
    c.gridCellSpacing    = scl(16);

    // Grid columns: pack as many posters as fit across the real screen
    // width, after reserving space for the sidebar + outer padding. This
    // is what makes ultrawides use their extra horizontal real estate.
    int sidebar     = scl(260);
    int outerPad    = scl(64);
    int cellTotal   = c.posterWidth + c.gridCellSpacing * 2;
    int avail       = widthPx - sidebar - outerPad;
    int cols        = (cellTotal > 0) ? (avail / cellTotal) : 5;
    if (cols < 3)  cols = 3;
    if (cols > 12) cols = 12;
    c.gridColumns = cols;

    // --- Typography ----------------------------------------------------
    c.titleFontSize       = scl(16);
    c.subtitleFontSize    = scl(13);
    c.descriptionFontSize = scl(11);
    c.homeTitleFontSize   = scl(30);
    c.homeSectionFontSize = scl(22);

    // --- Row heights (must clear cell + label + margins) --------------
    c.homeRowHeight      = c.posterHeight    + scl(55);
    c.landscapeRowHeight = c.landscapeHeight + scl(60);
    c.squareRowHeight    = c.squareCoverSize + scl(55);
    c.listRowHeight      = scl(64);

    // --- LiveTV layout -------------------------------------------------
    c.livetvChannelCardWidth = scl(180);
    c.livetvChannelRowHeight = scl(140);
    c.livetvGuideHeight      = scl(480);

    // --- Text truncation budgets (rough: ~7-8 px per glyph) ----------
    c.maxCellTitleChars     = std::max(10, c.posterWidth / 7);
    c.maxListTitleChars     = std::max(40, widthPx / 18);
    c.maxLiveTVProgramChars = std::max(12, c.livetvChannelCardWidth / 7);
    c.maxLiveTVChannelChars = std::max(10, c.livetvChannelCardWidth / 8);

    // --- Sidebar & dialog ---------------------------------------------
    c.sidebarMinWidth = scl(260);
    c.sidebarMaxWidth = scl(450);
    c.dialogWidth     = scl(560);

    // --- Memory tier: image cache, library page, music carousel ------
    // Scales with total pixel budget as a proxy for how much decoded
    // image data the device can comfortably keep resident.
    long pixels = static_cast<long>(widthPx) * static_cast<long>(heightPx);
    if (pixels >= 7'000'000L) {          // ≈ 4K / 5K
        c.imageCacheSize       = 200;
        c.libraryPageSize      = 750;
        c.playlistTrackPageSize= 300;
        c.musicCarouselLimit   = 200;
    } else if (pixels >= 3'500'000L) {    // ≈ 1440p / ultrawide
        c.imageCacheSize       = 160;
        c.libraryPageSize      = 600;
        c.playlistTrackPageSize= 250;
        c.musicCarouselLimit   = 175;
    } else if (pixels >= 1'800'000L) {    // ≈ 1080p / PS4 / desktop
        c.imageCacheSize       = 120;
        c.libraryPageSize      = 500;
        c.playlistTrackPageSize= 200;
        c.musicCarouselLimit   = 150;
    } else if (pixels >=   800'000L) {    // ≈ 720p / Switch / small tablet
        c.imageCacheSize       =  80;
        c.libraryPageSize      = 250;
        c.playlistTrackPageSize= 125;
        c.musicCarouselLimit   = 100;
    } else {                              // ≈ phone / Vita-class
        c.imageCacheSize       =  40;
        c.libraryPageSize      = 100;
        c.playlistTrackPageSize=  60;
        c.musicCarouselLimit   =  60;
    }

    // --- Thumbnail request resolutions (Plex photo/:/transcode) ------
    // Ask Plex for ~2x the display size for retina sharpness, capped at
    // the actual screen resolution to avoid wasting bandwidth on pixels
    // the GPU will never draw.
    c.posterRequestWidth     = std::min(widthPx,  c.posterWidth     * 2);
    c.posterRequestHeight    = std::min(heightPx, c.posterHeight    * 2);
    c.squareRequestSize      = std::min(widthPx,  c.squareCoverSize * 2);
    c.landscapeRequestWidth  = std::min(widthPx,  c.landscapeWidth  * 2);
    c.landscapeRequestHeight = std::min(heightPx, c.landscapeHeight * 2);
    c.detailPosterRequestWidth  = std::min(widthPx,  scl(600));
    c.detailPosterRequestHeight = std::min(heightPx, scl(900));
    c.photoRequestWidth  = widthPx;   // native screen for full-screen photos
    c.photoRequestHeight = heightPx;

    return c;
}

/**
 * Cache helper: returns a reference to a static ImageConstraints that is
 * lazily recomputed whenever the current screen size differs from the
 * cached size. Safe to call from any thread (the cache is not thread-safe
 * for concurrent writes, but reads are idempotent and the recompute is
 * pure with respect to (widthPx, heightPx)).
 *
 * Desktop and Android platform files delegate their getImageConstraints()
 * implementation to this helper.
 */
inline const ImageConstraints& getDynamicImageConstraintsCached() {
    static ImageConstraints cached{};
    static int lastW = 0;
    static int lastH = 0;
    ScreenSize s = getScreenSize();
    if (s.widthPx != lastW || s.heightPx != lastH) {
        cached = computeDynamicImageConstraints(s.widthPx, s.heightPx);
        lastW  = s.widthPx;
        lastH  = s.heightPx;
    }
    return cached;
}

}  // namespace platform
}  // namespace vitaplex
