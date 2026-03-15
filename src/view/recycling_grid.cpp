/**
 * VitaPlex - Recycling Grid implementation
 * Only renders a window of items around the current scroll position.
 * This drastically reduces memory usage for large libraries.
 */

#include "view/recycling_grid.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"

namespace vitaplex {

RecyclingGrid::RecyclingGrid() {
    this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    // Content box to hold all items
    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::COLUMN);
    m_contentBox->setPadding(10);
    this->setContentView(m_contentBox);

    // PS Vita screen: 960x544, so 6 columns of ~120px items
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

void RecyclingGrid::setOnItemStartAction(std::function<void(const MediaItem&)> callback) {
    m_onItemStartAction = callback;
}

void RecyclingGrid::addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index) {
    if (itemsInRow == 0) {
        currentRow = new brls::Box();
        currentRow->setAxis(brls::Axis::ROW);
        currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
        currentRow->setMarginBottom(10);
        m_contentBox->addView(currentRow);
    }

    auto* cell = new MediaItemCell();
    cell->setItem(m_items[index]);
    cell->setMarginRight(10);

    int idx = (int)index;
    cell->registerClickAction([this, idx](brls::View* view) {
        onItemClicked(idx);
        return true;
    });
    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

    // Register START button action for album items
    if (m_items[index].mediaType == MediaType::MUSIC_ALBUM && m_onItemStartAction) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [this, capturedItem](brls::View* view) {
                if (m_onItemStartAction) {
                    m_onItemStartAction(capturedItem);
                }
                return true;
            });
    }

    // Register START button action for movies
    if (m_items[index].mediaType == MediaType::MOVIE) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [capturedItem](brls::View* view) {
                MediaDetailView::showMovieContextMenuStatic(capturedItem);
                return true;
            });
    }

    // Register START button action for TV shows
    if (m_items[index].mediaType == MediaType::SHOW) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [capturedItem](brls::View* view) {
                MediaDetailView::showShowContextMenuStatic(capturedItem);
                return true;
            });
    }

    // Register START button action for seasons
    if (m_items[index].mediaType == MediaType::SEASON) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [capturedItem](brls::View* view) {
                MediaDetailView::showSeasonContextMenuStatic(capturedItem);
                return true;
            });
    }

    // Register START button action for artists
    if (m_items[index].mediaType == MediaType::MUSIC_ARTIST) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [capturedItem](brls::View* view) {
                MediaDetailView::showArtistContextMenuStatic(capturedItem);
                return true;
            });
    }

    // Register START button action for playlists
    if (m_items[index].type == "playlist" && m_onItemStartAction) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [this, capturedItem](brls::View* view) {
                if (m_onItemStartAction) {
                    m_onItemStartAction(capturedItem);
                }
                return true;
            });
    }

    currentRow->addView(cell);

    itemsInRow++;
    if (itemsInRow >= m_columns) {
        itemsInRow = 0;
    }
}

void RecyclingGrid::renderWindow(size_t startIdx, size_t endIdx) {
    m_contentBox->clearViews();

    if (startIdx >= endIdx || startIdx >= m_items.size()) return;

    // Clamp end to items size
    if (endIdx > m_items.size()) endIdx = m_items.size();

    brls::Box* currentRow = nullptr;
    int itemsInRow = 0;

    for (size_t i = startIdx; i < endIdx; i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }

    m_windowStart = startIdx;
    m_windowEnd = endIdx;
}

void RecyclingGrid::rebuildGrid() {
    m_contentBox->clearViews();
    m_windowStart = 0;
    m_windowEnd = 0;

    if (m_items.empty()) return;

    // Render only the first window of items
    size_t end = std::min(m_items.size(), WINDOW_SIZE);
    renderWindow(0, end);
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
