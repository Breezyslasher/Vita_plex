/**
 * VitaPlex - Recycling Grid
 * Efficient grid view for displaying media items
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
    void setOnItemSelected(std::function<void(const MediaItem&)> callback);
    void setOnItemStartAction(std::function<void(const MediaItem&)> callback);

    static brls::View* create();

private:
    void rebuildGrid();
    void appendPage();       // Append next page of items to the grid
    void onItemClicked(int index);
    void addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index);

    std::vector<MediaItem> m_items;
    std::function<void(const MediaItem&)> m_onItemSelected;
    std::function<void(const MediaItem&)> m_onItemStartAction;

    brls::Box* m_contentBox = nullptr;
    brls::Button* m_loadMoreBtn = nullptr;
    int m_columns = 4;
    int m_visibleRows = 3;
    size_t m_renderedCount = 0;  // How many items are currently rendered

    static constexpr size_t PAGE_SIZE = 60;  // Items per page (10 rows of 6)
};

} // namespace vitaplex
