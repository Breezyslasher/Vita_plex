/**
 * VitaPlex - Recycling Grid
 * Memory-efficient grid view with infinite scroll pagination.
 * Automatically fetches the next page when scrolling near the bottom.
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"
#include <atomic>
#include <functional>
#include <memory>
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

    // Override to (1) visibility-cull off-screen rows and (2) paint
    // covers via batched nvgImagePattern instead of per-cell brls::Image
    // children. Cover painting itself lives in MediaItemCell::draw() so
    // non-grid containers (the home tab's HorizontalScrollRow, the
    // detail view's HScrollingFrame) get covers too.
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    // Override to recompute (columns, cellWidth) the moment borealis
    // hands us a real laid-out width. The grid never guesses from
    // viewportWidth — if data arrives before the first layout pass,
    // setDataSource just stashes it and rebuildGrid() runs from here
    // once the size is known. After that, every subsequent layout
    // (window resize, orientation flip, parent reflow) flows through
    // the same path so there's exactly one source of truth.
    void onLayout() override;

    static brls::View* create();

private:
    void rebuildGrid();
    void onItemClicked(int index);
    void addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index);
    // Real laid-out content width (interior of m_contentBox). Falls back
    // to a viewport estimate before the first layout pass. Non-const
    // because borealis View::getWidth() isn't const.
    int availableContentWidth();

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

    // Column count and per-cell cover width are picked by onLayout()
    // once the grid has a real width. cellWidth==0 also serves as the
    // "not yet laid out" sentinel: setDataSource() defers building
    // until onLayout sets a positive value.
    int m_columns = 0;
    int m_cellWidth = 0;
    int m_visibleRows = 3;
    size_t m_renderedCount = 0;

    bool m_hasMore = false;
    bool m_loading = false;  // Prevents duplicate fetch requests

    // Lifetime guard for the orientation-change listener — the listener
    // is global and lives forever, but the grid can be destroyed while
    // the user navigates away. The listener checks this flag before
    // touching `this`.
    std::shared_ptr<std::atomic<bool>> m_alive
        = std::make_shared<std::atomic<bool>>(true);
};

} // namespace vitaplex
