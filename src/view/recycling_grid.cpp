/**
 * VitaPlex - Recycling Grid implementation
 */

#include "view/recycling_grid.hpp"
#include "view/media_item_cell.hpp"

namespace vitaplex {

RecyclingGrid::RecyclingGrid() {
    this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    // Content box to hold all items
    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::COLUMN);
    m_contentBox->setPadding(10);
    this->setContentView(m_contentBox);

    // PS Vita screen: 960x544, so 4 columns of ~120px items works well
    m_columns = 6;
    m_visibleRows = 3;
}

void RecyclingGrid::setDataSource(const std::vector<MediaItem>& items) {
    m_items = items;
    rebuildGrid();
}

void RecyclingGrid::setOnItemSelected(std::function<void(const MediaItem&)> callback) {
    m_onItemSelected = callback;
}

void RecyclingGrid::rebuildGrid() {
    m_contentBox->clearViews();

    if (m_items.empty()) return;

    // Create rows
    brls::Box* currentRow = nullptr;
    int itemsInRow = 0;

    for (size_t i = 0; i < m_items.size(); i++) {
        if (itemsInRow == 0) {
            currentRow = new brls::Box();
            currentRow->setAxis(brls::Axis::ROW);
            currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
            currentRow->setMarginBottom(10);
            m_contentBox->addView(currentRow);
        }

        auto* cell = new MediaItemCell();
        cell->setItem(m_items[i]);
        cell->setWidth(140);
        cell->setHeight(200);
        cell->setMarginRight(10);

        int index = (int)i;
        cell->registerClickAction([this, index](brls::View* view) {
            onItemClicked(index);
            return true;
        });
        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

        currentRow->addView(cell);

        itemsInRow++;
        if (itemsInRow >= m_columns) {
            itemsInRow = 0;
        }
    }
}

void RecyclingGrid::onItemClicked(int index) {
    if (index >= 0 && index < (int)m_items.size() && m_onItemSelected) {
        m_onItemSelected(m_items[index]);
    }
}

brls::View* RecyclingGrid::create() {
    return new RecyclingGrid();
}

} // namespace vitaplex
