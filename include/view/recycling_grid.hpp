/**
 * VitaPlex - Recycling Grid
 * Memory-efficient grid view with infinite scroll pagination.
 * Automatically fetches the next page when scrolling near the bottom.
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"
#include <functional>

namespace vitaplex {

class RecyclingGrid : public brls::ScrollingFrame {
public:
    RecyclingGrid();

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
    int m_columns = 6;
    int m_visibleRows = 3;
    size_t m_renderedCount = 0;

    bool m_hasMore = false;
    bool m_loading = false;  // Prevents duplicate fetch requests
};

} // namespace vitaplex
