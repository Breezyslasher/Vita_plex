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

    static brls::View* create();

private:
    void rebuildGrid();
    void onItemClicked(int index);

    std::vector<MediaItem> m_items;
    std::function<void(const MediaItem&)> m_onItemSelected;

    brls::Box* m_contentBox = nullptr;
    int m_columns = 4;
    int m_visibleRows = 3;
};

} // namespace vitaplex
