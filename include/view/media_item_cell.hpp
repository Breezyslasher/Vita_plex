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

    // True if this cell's media type would have shown the start-button
    // hint overlay when focused. RecyclingGrid::draw() reads this so it
    // can paint a single NVG image for the focused cell instead of every
    // cell maintaining its own brls::Image overlay.
    bool wantsStartHint() const;

    // Absolute screen coordinates of the cover area inside the cell —
    // i.e. the placeholder box that used to hold the brls::Image
    // thumbnail. The grid's batched draw uses this both for painting
    // covers and for anchoring the start-button hint.
    void getCoverBounds(float& cx, float& cy, float& cw, float& ch) const;

    // Raw NVG handle + source dimensions for the cover. Returned to the
    // grid's draw() pass which paints all visible covers in a single
    // batched nvgImagePattern loop. 0 means "no cover loaded yet" — the
    // grid paints the placeholder color in that case.
    int getCoverImage()  const { return m_nvgCover; }
    int getCoverWidth()  const { return m_coverW; }
    int getCoverHeight() const { return m_coverH; }

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
    // brls::Image, so titles/progress bar stay positioned correctly,
    // but the box itself draws nothing — RecyclingGrid paints the
    // actual cover at its bounds in a batched pass.
    brls::Box*  m_coverSlot = nullptr;
    int         m_nvgCover  = 0;   // NVG image handle, 0 = not loaded
    int         m_coverW    = 0;   // Source image dimensions, used to
    int         m_coverH    = 0;   // letterbox via nvgImagePattern.

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_subtitleLabel = nullptr;
    brls::Label* m_descriptionLabel = nullptr;  // Shows on focus for episodes
    brls::Rectangle* m_progressBar = nullptr;
};

} // namespace vitaplex
