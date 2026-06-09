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
    // 960x544 screen gets 6 columns while a 1080p TV gets 7. On
    // platforms whose user can rotate the device (Android portrait /
    // a vertical desktop monitor), getImageConstraints() returns a
    // different table with fewer columns; we re-query and rebuild the
    // grid when the orientation flips so existing pages reflow.
    const auto& ic = platform::getImageConstraints();
    m_columns = ic.gridColumns;
    m_visibleRows = 3;

    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
    platform::onOrientationChanged([this, aliveWeak]() {
        auto alive = aliveWeak.lock();
        if (!alive || !alive->load()) return;
        int newCols = platform::getImageConstraints().gridColumns;
        if (newCols == m_columns) return;
        m_columns = newCols;
        if (!m_items.empty()) rebuildGrid();
    });
}

RecyclingGrid::~RecyclingGrid() {
    if (m_alive) m_alive->store(false);
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

    // Register START button action for episodes
    if (m_items[index].mediaType == MediaType::EPISODE) {
        MediaItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [capturedItem](brls::View* view) {
                MediaDetailView::showEpisodeContextMenu(capturedItem);
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
            } else if (capturedItem.mediaType == MediaType::EPISODE) {
                MediaDetailView::showEpisodeContextMenu(capturedItem);
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
    // Crash fix for "open category": setDataSource() runs while one of
    // our cells (the category cell the user just clicked) still owns
    // the focus. The old implementation called m_contentBox->clearViews()
    // first, which deleted the focused cell mid-event-dispatch — the
    // next input then walked into freed memory. Calling
    // Application::giveFocus(this) before clearing wasn't enough either:
    // ScrollingFrame::getDefaultFocus() resolves back into our own
    // content view, so focus just moved to a *different* doomed cell.
    //
    // Strategy: keep the old cells around until the new ones exist,
    // hand focus to a new cell, *then* delete the old ones. The view
    // tree gets briefly larger than the on-screen item set (old + new
    // rows live in m_contentBox simultaneously for the duration of
    // this function), but borealis won't draw the old rows in that
    // window because we never run a layout pass between the add and
    // the remove.

    // Step 1: snapshot the old cells/rows. m_rows and m_cells are then
    // emptied so addCellForItem() can append the new entries without
    // having to walk past stale pointers.
    std::vector<brls::Box*> oldRows = m_rows;
    std::vector<MediaItemCell*> oldCells = m_cells;
    m_rows.clear();
    m_cells.clear();
    m_renderedCount = 0;

    // Step 2: figure out whether the current focus lives inside any of
    // the cells we're about to retire. If it does, we need to relocate
    // focus before the delete in Step 5.
    brls::View* focused = brls::Application::getCurrentFocus();
    bool focusInOld = false;
    if (focused) {
        for (auto* oldCell : oldCells) {
            for (brls::View* p = focused; p != nullptr; p = p->getParent()) {
                if (p == oldCell) { focusInOld = true; break; }
            }
            if (focusInOld) break;
        }
    }

    // Step 3: build the new rows/cells. addCellForItem() addView()s into
    // m_contentBox, so the old rows are still in the tree above them.
    brls::Box* currentRow = nullptr;
    int itemsInRow = 0;
    for (size_t i = 0; i < m_items.size(); i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }
    m_renderedCount = m_items.size();

    // Step 4: move focus onto a freshly-created cell so deleting the
    // old ones in Step 5 can't pull the focused view out from under the
    // event dispatcher. If we have no new cells (empty data source) and
    // focus was in the old set, fall back to giving focus to the grid's
    // parent — borealis will resolve to a sibling (a view-mode button
    // etc.) rather than back into our about-to-be-empty content box.
    if (focusInOld) {
        if (!m_cells.empty()) {
            brls::Application::giveFocus(m_cells[0]);
        } else if (brls::View* parent = this->getParent()) {
            brls::Application::giveFocus(parent);
        }
    }

    // Step 5: now safe — the only refs to the old cells are in oldRows
    // and the focus stack no longer points at any of them.
    //
    // Reset m_contentBox's lastFocusedView cache before deleting any
    // row that it might point at. Box::onChildFocusGained sets this
    // pointer to whichever row last contained the focused view; it's
    // only cleared by Box::clearViews(), NOT by removeView(). Without
    // this reset, Box::getDefaultFocus() — which the next
    // giveFocus(m_contentGrid) walk lands in — dereferences the
    // dangling row pointer and either returns nullptr (so focus stays
    // wherever it was, the "stays on Back" symptom when leaving a
    // playlist) or returns a stale pointer that crashes on the next
    // input.
    m_contentBox->setLastFocusedView(nullptr);
    for (auto* row : oldRows) {
        if (row) m_contentBox->removeView(row, true);
    }
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
