/**
 * VitaPlex - Recycling Grid
 * Memory-efficient grid view with infinite scroll pagination.
 * Automatically fetches the next page when scrolling near the bottom.
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"
#include <functional>
#include <vector>

namespace vitaplex {

class MediaItemCell;

class RecyclingGrid : public brls::ScrollingFrame {
public:
    RecyclingGrid();
    ~RecyclingGrid() override;

    void setDataSource(const std::vector<MediaItem>& items);
    // Append additional items (called when next page arrives from server)
    void appendItems(const std::vector<MediaItem>& newItems);
    void setOnItemSelected(std::function<void(const MediaItem&)> callback);
    void setOnItemStartAction(std::function<void(const MediaItem&)> callback);

    // Called automatically when user scrolls to the bottom.
    // Owner should fetch the next page and call appendItems().
    void setOnLoadMore(std::function<void()> callback);

    // Tell the grid whether more items are available on the server
    void setHasMore(bool hasMore);

    // Override to detect when user tries to scroll past the bottom
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

    // Visibility-cull off-screen rows and paint the focused cell's
    // start-button hint as a single batched NVG draw rather than as a
    // per-cell brls::Box overlay.
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    static brls::View* create();

private:
    void rebuildGrid();
    void onItemClicked(int index);
    void addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index);

    std::vector<MediaItem> m_items;
    std::function<void(const MediaItem&)> m_onItemSelected;
    std::function<void(const MediaItem&)> m_onItemStartAction;
    std::function<void()> m_onLoadMore;

    brls::Box* m_contentBox = nullptr;
    // Row containers in display order, populated alongside the cells.
    // Lets draw() flip whole rows to INVISIBLE in one call when they
    // scroll off-screen, instead of paying the per-view frame() cost.
    std::vector<brls::Box*> m_rows;
    // Direct cell pointers so we can scan the visible range without
    // traversing every row->getChildren() each frame.
    std::vector<MediaItemCell*> m_cells;

    int m_columns = 6;
    int m_visibleRows = 3;
    size_t m_renderedCount = 0;

    bool m_hasMore = false;
    bool m_loading = false;  // Prevents duplicate fetch requests

    // Cached visible row range. -1 means "not yet computed". When the
    // range stays put between frames we skip the per-row visibility
    // updates entirely.
    int m_cachedFirstVisible = -1;
    int m_cachedLastVisible  = -1;
    // First-laid-out row pitch (height + bottom margin). Sampled lazily
    // from the actual layout so the math works across poster / square /
    // landscape cells without per-media-type code.
    float m_cachedRowPitch = 0.0f;

    // Lazily-created NVG handle for the start-button overlay icon. One
    // handle for the whole grid replaces N brls::Image children, and
    // nvgDeleteImage in the dtor releases the GPU texture.
    int m_startHintNvg = 0;
    int m_startHintW   = 0;
    int m_startHintH   = 0;
};

} // namespace vitaplex
