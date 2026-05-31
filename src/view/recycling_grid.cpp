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
#include <algorithm>
#include <cmath>

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

RecyclingGrid::~RecyclingGrid() {
    // Release the lazily-allocated GPU texture for the start-button hint.
    // Without this every grid teardown would leak one image handle.
    if (m_startHintNvg != 0) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) nvgDeleteImage(vg, m_startHintNvg);
        m_startHintNvg = 0;
    }
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

    // Invalidate the visibility cull cache so newly-appended rows get
    // hidden if they land off-screen instead of paying the per-frame
    // draw cost while waiting for the user to scroll down to them.
    m_cachedFirstVisible = -1;
    m_cachedLastVisible  = -1;

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
    m_contentBox->clearViews();
    m_rows.clear();
    m_cells.clear();
    // Force visibility-cull and row-pitch to recompute against the new
    // layout. Stale values would leave half the new rows hidden.
    m_cachedFirstVisible = -1;
    m_cachedLastVisible  = -1;
    m_cachedRowPitch     = 0.0f;
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
    // ── Step 1: visibility-cull off-screen rows ─────────────────────────
    // ScrollingFrame::draw() walks every row in m_contentBox and issues a
    // full View::frame() pass on each — even ones nowhere near the
    // viewport. Flipping off-screen rows to INVISIBLE preserves their
    // layout space (so the scroll height stays right) but lets borealis
    // skip the per-cell draw entirely. On Vita this is the difference
    // between "smooth scroll" and "5 FPS".
    if (!m_rows.empty()) {
        // Sample the actual laid-out row pitch once we have one. Rows
        // have height 0 before the first layout pass, so we keep
        // retrying until a real value appears.
        if (m_cachedRowPitch <= 0.0f && m_rows[0]) {
            float h = m_rows[0]->getHeight();
            if (h > 0.0f) m_cachedRowPitch = h;
        }
        if (m_cachedRowPitch > 0.0f) {
            float scrollY = this->getContentOffsetY();
            float viewH   = this->getHeight();
            int rowCount  = (int)m_rows.size();

            int firstVisible = std::max(0, (int)(scrollY / m_cachedRowPitch) - 1);
            int lastVisible  = std::min(rowCount,
                                        (int)((scrollY + viewH) / m_cachedRowPitch) + 2);

            if (firstVisible != m_cachedFirstVisible ||
                lastVisible  != m_cachedLastVisible) {
                if (m_cachedFirstVisible < 0) {
                    // First pass: set every row in one sweep.
                    for (int i = 0; i < rowCount; i++) {
                        brls::Visibility desired = (i >= firstVisible && i < lastVisible)
                            ? brls::Visibility::VISIBLE
                            : brls::Visibility::INVISIBLE;
                        m_rows[i]->setVisibility(desired);
                    }
                } else {
                    // Subsequent passes: only touch the boundary rows
                    // that actually changed state.
                    for (int i = m_cachedFirstVisible;
                         i < firstVisible && i < rowCount; i++) {
                        if (i >= 0) m_rows[i]->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    for (int i = std::max(0, lastVisible);
                         i < m_cachedLastVisible && i < rowCount; i++) {
                        m_rows[i]->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    for (int i = firstVisible; i < lastVisible; i++) {
                        if (i >= 0) m_rows[i]->setVisibility(brls::Visibility::VISIBLE);
                    }
                }
                m_cachedFirstVisible = firstVisible;
                m_cachedLastVisible  = lastVisible;
            }
        }
    }

    // ── Step 2: let borealis paint everything as normal ─────────────────
    // After this call the cell backgrounds, focus highlights, titles,
    // subtitles, and progress bars have all painted. The cover slots
    // intentionally render nothing — they exist only to reserve layout
    // space for the batched cover pass below.
    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);

    // ── Step 3: batched cover paint ─────────────────────────────────────
    // One nvgSave + nvgIntersectScissor wraps the entire loop, then each
    // visible cell contributes either an nvgImagePattern fill (cover
    // loaded) or a flat-color rounded rect (placeholder). Previously
    // every cell carried a brls::Image child, so borealis walked View::
    // frame() / drawBackground() / drawShadow() for each one even though
    // only the cover itself was visible. With ~24 visible cells at 60fps
    // that was ~5,800 per-view paths/sec; the loop below is one path
    // per cell with no per-view overhead.
    if (m_cachedFirstVisible >= 0 && !m_cells.empty()) {
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);

        int startIdx = m_cachedFirstVisible * m_columns;
        int endIdx   = std::min(m_cachedLastVisible * m_columns,
                                (int)m_cells.size());

        for (int i = startIdx; i < endIdx; i++) {
            MediaItemCell* cell = m_cells[i];
            if (!cell) continue;

            float cx, cy, cw, ch;
            cell->getCoverBounds(cx, cy, cw, ch);
            if (cw <= 0.0f || ch <= 0.0f) continue;

            int nvgImg = cell->getCoverImage();
            if (nvgImg != 0) {
                // Letterbox: scale the source image to FIT inside the
                // slot without cropping (matches the prior brls::Image
                // setScalingType(FIT) behaviour exactly).
                float imgW = (float)cell->getCoverWidth();
                float imgH = (float)cell->getCoverHeight();
                if (imgW > 0.0f && imgH > 0.0f) {
                    float scale = std::min(cw / imgW, ch / imgH);
                    float sw = imgW * scale;
                    float sh = imgH * scale;
                    float ox = cx + (cw - sw) * 0.5f;
                    float oy = cy + (ch - sh) * 0.5f;
                    NVGpaint paint = nvgImagePattern(
                        vg, ox, oy, sw, sh, 0, nvgImg, 1.0f);
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, ox, oy, sw, sh, 4.0f);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);
                }
            } else {
                // Cover hasn't loaded yet — fill the slot with the same
                // gray the prior cell-level placeholder used.
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx, cy, cw, ch, 4.0f);
                nvgFillColor(vg, nvgRGB(40, 40, 48));
                nvgFill(vg);
            }
        }

        nvgRestore(vg);

        // ── Step 4: start-button hint on the focused cell ────────────
        // Painted after the cover loop so the hint always sits on top
        // of the cover, never the other way around. The hint image is
        // lazily uploaded once and reused for the lifetime of the grid.
        MediaItemCell* focusedCell = nullptr;
        for (int i = startIdx; i < endIdx; i++) {
            MediaItemCell* c = m_cells[i];
            if (c && c->isFocused() && c->wantsStartHint()) {
                focusedCell = c;
                break;
            }
        }
        if (focusedCell) {
            if (m_startHintNvg == 0) {
                m_startHintNvg = nvgCreateImage(
                    vg, RESOURCE_PREFIX "images/start_button.png", 0);
                if (m_startHintNvg != 0) {
                    nvgImageSize(vg, m_startHintNvg, &m_startHintW, &m_startHintH);
                }
            }
            if (m_startHintNvg != 0 && m_startHintW > 0 && m_startHintH > 0) {
                float fcx, fcy, fcw, fch;
                focusedCell->getCoverBounds(fcx, fcy, fcw, fch);
                if (fcw > 0.0f && fch > 0.0f) {
                    float hintW = (float)m_startHintW;
                    float hintH = (float)m_startHintH;
                    // Top-right corner of the cover, with a small inset
                    // matching the prior brls::Box overlay position.
                    float hx = fcx + fcw - hintW - 7.0f;
                    float hy = fcy + 7.0f;
                    nvgSave(vg);
                    NVGpaint paint = nvgImagePattern(
                        vg, hx, hy, hintW, hintH, 0, m_startHintNvg, 1.0f);
                    nvgBeginPath(vg);
                    nvgRect(vg, hx, hy, hintW, hintH);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);
                    nvgRestore(vg);
                }
            }
        }
    }
}

brls::View* RecyclingGrid::create() {
    return new RecyclingGrid();
}

} // namespace vitaplex
