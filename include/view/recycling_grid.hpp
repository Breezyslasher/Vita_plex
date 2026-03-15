/**
 * VitaPlex - Recycling Grid
 * Memory-efficient grid view that only renders visible items + a small buffer.
 * Off-screen cells are destroyed and recreated on demand to keep RAM usage low.
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
    void onItemClicked(int index);
    void addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index);

    // Render only items in range [startIdx, endIdx) into the content box.
    // Called on initial build and when the visible window changes.
    void renderWindow(size_t startIdx, size_t endIdx);

    std::vector<MediaItem> m_items;
    std::function<void(const MediaItem&)> m_onItemSelected;
    std::function<void(const MediaItem&)> m_onItemStartAction;

    brls::Box* m_contentBox = nullptr;
    int m_columns = 6;
    int m_visibleRows = 3;

    // Current rendered window [m_windowStart, m_windowEnd)
    size_t m_windowStart = 0;
    size_t m_windowEnd = 0;

    // How many items to render at once (visible + buffer)
    // 3 visible rows * 6 columns = 18 visible, add 2 buffer rows = 30 total
    static constexpr size_t WINDOW_SIZE = 30;
};

} // namespace vitaplex
