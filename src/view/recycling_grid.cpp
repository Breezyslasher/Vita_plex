/**
 * VitaPlex - Recycling Grid implementation
 * Infinite scroll: automatically fetches next page when user navigates
 * to the last row. Server-side pagination keeps memory low.
 */

#include "view/recycling_grid.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "view/long_press_gesture.hpp"
#include "platform/platform.hpp"

namespace vitaplex {

RecyclingGrid::RecyclingGrid() {
    this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    // Content box to hold all items
    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::COLUMN);
    m_contentBox->setPadding(10);
    this->setContentView(m_contentBox);

    // Grid layout — column count comes from the platform layer so PSV's
    // 960x544 screen gets 6 columns while a 1080p TV gets 7.
    const auto& ic = platform::getImageConstraints();
    m_columns = ic.gridColumns;
    m_visibleRows = 3;
}

RecyclingGrid::~RecyclingGrid() = default;

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
    const int spacing = platform::getImageConstraints().gridCellSpacing;
    if (itemsInRow == 0) {
        currentRow = new brls::Box();
        currentRow->setAxis(brls::Axis::ROW);
        currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
        currentRow->setMarginBottom(spacing);
        m_contentBox->addView(currentRow);
        m_rows.push_back(currentRow);
    }

    auto* cell = new MediaItemCell();
    cell->setItem(m_items[index]);
    cell->setMarginRight(spacing);
    m_cells.push_back(cell);

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

    // Long press on touch = same as START button options
    MediaItem capturedItem = m_items[index];
    cell->addGestureRecognizer(new LongPressGestureRecognizer(
        cell, [this, capturedItem](LongPressGestureStatus status) {
            if (status.state != brls::GestureState::START) {
                return;
            }

            if (capturedItem.mediaType == MediaType::MUSIC_ALBUM && m_onItemStartAction) {
                m_onItemStartAction(capturedItem);
            } else if (capturedItem.mediaType == MediaType::MOVIE) {
                MediaDetailView::showMovieContextMenuStatic(capturedItem);
            } else if (capturedItem.mediaType == MediaType::SHOW) {
                MediaDetailView::showShowContextMenuStatic(capturedItem);
            } else if (capturedItem.mediaType == MediaType::SEASON) {
                MediaDetailView::showSeasonContextMenuStatic(capturedItem);
            } else if (capturedItem.mediaType == MediaType::MUSIC_ARTIST) {
                MediaDetailView::showArtistContextMenuStatic(capturedItem);
            } else if (capturedItem.type == "playlist" && m_onItemStartAction) {
                m_onItemStartAction(capturedItem);
            }
        }));

    currentRow->addView(cell);

    itemsInRow++;
    if (itemsInRow >= m_columns) {
        itemsInRow = 0;
    }
}

void RecyclingGrid::rebuildGrid() {
    // If anything inside this grid currently has focus, clearViews() is
    // about to delete it out from under the focus stack — the very next
    // input event would then walk freed memory and crash. This is the
    // "hover transfer" crash when opening a category: the category cell
    // the user just clicked is still focused at the moment
    // setDataSource() repopulates the grid with the filtered items.
    // Walk up from current focus; if we find ourselves in the chain,
    // re-anchor focus on the grid itself (we're a ScrollingFrame, so
    // we're focusable). Once the new cells exist and lay out, borealis
    // will move focus into one of them naturally on the next input.
    brls::View* focused = brls::Application::getCurrentFocus();
    if (focused) {
        for (brls::View* p = focused; p != nullptr; p = p->getParent()) {
            if (p == this) {
                brls::Application::giveFocus(this);
                break;
            }
        }
    }

    m_contentBox->clearViews();
    m_rows.clear();
    m_cells.clear();
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

void RecyclingGrid::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    // Visibility-cull off-screen rows. ScrollingFrame::draw() walks every
    // row in m_contentBox and issues a full View::frame() pass on each —
    // even ones nowhere near the viewport. Flipping off-screen rows to
    // INVISIBLE preserves their layout space (so the scroll height stays
    // right) but lets borealis skip the per-cell draw entirely. On Vita
    // this is the difference between "smooth scroll" and "5 FPS".
    //
    // The prior implementation used a single sampled row pitch
    // (m_rows[0]->getHeight()) which ignored row marginBottom, so as the
    // user scrolled the pitch-based math drifted and culled rows that
    // were actually still inside the viewport — the symptom was blank
    // strips at the top of the grid where posters used to be. Asking
    // each row for its real screen position is O(rowCount) per frame
    // but rowCount is small (<100 typically) and every getY() walk is
    // just float arithmetic, so the cost is negligible compared to the
    // per-cell draw we save by culling correctly.
    //
    // BUFFER_PX preloads a strip just outside the viewport so a row
    // starting to scroll in doesn't pop — same idea as the prior -1/+2
    // index buffer, just measured in pixels.
    if (!m_rows.empty()) {
        constexpr float BUFFER_PX = 64.0f;
        float vpTop    = this->getY() - BUFFER_PX;
        float vpBottom = this->getY() + this->getHeight() + BUFFER_PX;
        for (brls::Box* row : m_rows) {
            if (!row) continue;
            float ry = row->getY();
            float rh = row->getHeight();
            // Rows have height 0 before the first layout pass; assume
            // visible in that case so the user doesn't see a flash of
            // blank cells on the first frame.
            bool visible = (rh <= 0.0f)
                ? true
                : (ry + rh > vpTop && ry < vpBottom);
            brls::Visibility desired = visible
                ? brls::Visibility::VISIBLE
                : brls::Visibility::INVISIBLE;
            if (row->getVisibility() != desired) {
                row->setVisibility(desired);
            }
        }
    }

    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
}

brls::View* RecyclingGrid::create() {
    return new RecyclingGrid();
}

} // namespace vitaplex
