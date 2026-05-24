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

    // Absolute screen coordinates of the cover area inside the cell. Used
    // by the grid pass to anchor the start-button hint without having to
    // know the per-media-type cover dimensions itself.
    void getThumbnailBounds(float& tx, float& ty, float& tw, float& th) const;

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

    brls::Image* m_thumbnailImage = nullptr;
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_subtitleLabel = nullptr;
    brls::Label* m_descriptionLabel = nullptr;  // Shows on focus for episodes
    brls::Rectangle* m_progressBar = nullptr;
};

} // namespace vitaplex
