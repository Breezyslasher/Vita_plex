/**
 * VitaPlex - Recycling Grid implementation
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

void RecyclingGrid::setOnItemStartAction(std::function<void(const MediaItem&)> callback) {
    m_onItemStartAction = callback;
}

void RecyclingGrid::addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index) {
    if (itemsInRow == 0) {
        currentRow = new brls::Box();
        currentRow->setAxis(brls::Axis::ROW);
        currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
        currentRow->setMarginBottom(10);
        // Insert before "Load More" button if it exists, otherwise at end
        if (m_loadMoreBtn) {
            size_t btnIdx = 0;
            auto& children = m_contentBox->getChildren();
            for (size_t c = 0; c < children.size(); c++) {
                if (children[c] == m_loadMoreBtn) {
                    btnIdx = c;
                    break;
                }
            }
            m_contentBox->addView(currentRow, btnIdx);
        } else {
            m_contentBox->addView(currentRow);
        }
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

    currentRow->addView(cell);

    itemsInRow++;
    if (itemsInRow >= m_columns) {
        itemsInRow = 0;
    }
}

void RecyclingGrid::rebuildGrid() {
    m_contentBox->clearViews();
    m_loadMoreBtn = nullptr;
    m_renderedCount = 0;

    if (m_items.empty()) return;

    appendPage();
}

void RecyclingGrid::appendPage() {
    size_t end = std::min(m_renderedCount + PAGE_SIZE, m_items.size());

    // Remove existing "Load More" button before adding new rows
    if (m_loadMoreBtn) {
        m_contentBox->removeView(m_loadMoreBtn);
        m_loadMoreBtn = nullptr;
    }

    brls::Box* currentRow = nullptr;
    // If we have items already, check if the last row is incomplete
    int itemsInRow = (int)(m_renderedCount % m_columns);
    if (itemsInRow > 0 && m_contentBox->getChildren().size() > 0) {
        // Get the last row to continue filling it
        auto& children = m_contentBox->getChildren();
        currentRow = dynamic_cast<brls::Box*>(children.back());
        if (!currentRow) itemsInRow = 0;
    } else {
        itemsInRow = 0;
    }

    for (size_t i = m_renderedCount; i < end; i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }

    m_renderedCount = end;

    // Add "Load More" button if there are remaining items
    if (m_renderedCount < m_items.size()) {
        size_t remaining = m_items.size() - m_renderedCount;
        m_loadMoreBtn = new brls::Button();
        auto* label = new brls::Label();
        label->setText("Load More (" + std::to_string(remaining) + " remaining)");
        label->setFontSize(16);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_loadMoreBtn->addView(label);
        m_loadMoreBtn->setMarginTop(10);
        m_loadMoreBtn->setHeight(44);
        m_loadMoreBtn->registerClickAction([this](brls::View*) {
            appendPage();
            return true;
        });
        m_loadMoreBtn->addGestureRecognizer(new brls::TapGestureRecognizer(m_loadMoreBtn));
        m_contentBox->addView(m_loadMoreBtn);
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
