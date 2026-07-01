/**
 * VitaPlex - Media Item Cell
 * A cell for displaying media items in a grid
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include "app/plex_client.hpp"

namespace vitaplex {

class MediaItemCell : public brls::Box {
public:
    MediaItemCell();
    ~MediaItemCell() override;

    void setItem(const MediaItem& item);
    const MediaItem& getItem() const { return m_item; }

    // True if this cell's media type shows the start-button hint overlay
    // when focused (movies, shows, seasons, albums, artists, playlists).
    // Episodes / tracks / clips intentionally don't get the hint.
    bool wantsStartHint() const;

    // Absolute screen coordinates of the cover slot. Used internally by
    // draw() to position the cover paint and the focused start-button
    // hint.
    void getCoverBounds(float& cx, float& cy, float& cw, float& ch) const;

    void onFocusGained() override;
    void onFocusLost() override;
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    static brls::View* create();

private:
    void loadThumbnail();
    void updateFocusInfo(bool focused);

    bool m_pressed = false;  // Touch press feedback overlay

    MediaItem m_item;
    std::string m_originalTitle;  // Store original truncated title

    // Alive flag - set to false in destructor to prevent use-after-free
    // in async image loader callbacks
    std::shared_ptr<std::atomic<bool>> m_alive;

    // Transparent placeholder Box that reserves layout space for the
    // cover. Borealis lays this out the same way it did the prior
    // brls::Image, so titles/progress bar stay positioned correctly;
    // draw() paints the actual cover at its bounds.
    brls::Box*  m_coverSlot = nullptr;
    int         m_nvgCover  = 0;   // NVG image handle, 0 = not loaded
    int         m_coverW    = 0;   // Source image dimensions, used to
    int         m_coverH    = 0;   // letterbox via nvgImagePattern.

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_subtitleLabel = nullptr;
    brls::Label* m_descriptionLabel = nullptr;  // Shows on focus for episodes
    brls::Rectangle* m_progressBar = nullptr;

    // Cached nvgTextBounds() results for the cover badges. The strings are
    // static per item, but draw() ran the measurement (fontstash shaping)
    // every frame for every visible cell. Reset in setItem().
    float m_ratingTextW = -1.0f;
    float m_charTextW   = -1.0f;
};

} // namespace vitaplex
