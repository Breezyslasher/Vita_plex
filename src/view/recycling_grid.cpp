/**
 * VitaPlex - Recycling Grid implementation
 * Infinite scroll: automatically fetches next page when user navigates
 * to the last row. Server-side pagination keeps memory low.
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
    m_loading = false;
    rebuildGrid();
}

void RecyclingGrid::appendItems(const std::vector<MediaItem>& newItems) {
    if (newItems.empty()) {
        m_loading = false;
        return;
    }

    size_t oldSize = m_items.size();
    m_items.insert(m_items.end(), newItems.begin(), newItems.end());

    // Render the new items continuing from where we left off
    brls::Box* currentRow = nullptr;
    int itemsInRow = (int)(oldSize % m_columns);
    if (itemsInRow > 0 && m_contentBox->getChildren().size() > 0) {
        auto& children = m_contentBox->getChildren();
        currentRow = dynamic_cast<brls::Box*>(children.back());
        if (!currentRow) itemsInRow = 0;
    } else {
        itemsInRow = 0;
    }

    for (size_t i = oldSize; i < m_items.size(); i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }

    m_renderedCount = m_items.size();
    m_loading = false;
}

void RecyclingGrid::setOnItemSelected(std::function<void(const MediaItem&)> callback) {
    m_onItemSelected = callback;
}

void RecyclingGrid::setOnItemStartAction(std::function<void(const MediaItem&)> callback) {
    m_onItemStartAction = callback;
}

void RecyclingGrid::setOnLoadMore(std::function<void()> callback) {
    m_onLoadMore = callback;
}

void RecyclingGrid::setHasMore(bool hasMore) {
    m_hasMore = hasMore;
    m_loading = false;
}

brls::View* RecyclingGrid::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    brls::View* next = brls::ScrollingFrame::getNextFocus(direction, currentView);

    // If navigating DOWN and there's nowhere to go, we're at the bottom.
    // Trigger loading the next page if more items are available.
    if (direction == brls::FocusDirection::DOWN && next == nullptr &&
        m_hasMore && !m_loading && m_onLoadMore) {
        m_loading = true;
        // Use brls::sync to defer the fetch so focus resolution completes first
        brls::sync([this]() {
            if (m_onLoadMore) m_onLoadMore();
        });
    }

    return next;
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

void RecyclingGrid::rebuildGrid() {
    m_contentBox->clearViews();
    m_renderedCount = 0;

    if (m_items.empty()) return;

    brls::Box* currentRow = nullptr;
    int itemsInRow = 0;

    for (size_t i = 0; i < m_items.size(); i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }

    m_renderedCount = m_items.size();
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
