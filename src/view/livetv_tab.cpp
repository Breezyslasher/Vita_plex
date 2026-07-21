/**
 * VitaPlex - Live TV Tab implementation
 *
 * Layout: On-Now hero (featured channel + current program) → full-width
 * Program Guide (the dominant block) → DVR strip. Built directly on
 * borealis boxes; no XML. Sizes derive from platform::getImageConstraints()
 * so the same code scales from Vita's 960×544 logical viewport up to a
 * 1080p TV.
 */

#include "view/livetv_tab.hpp"
#include "app/application.hpp"
#include "app/plex_palette.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "platform/platform.hpp"
#include <algorithm>
#include <atomic>
#include <ctime>

// Forward declarations for the patched nanovg batch text API. The
// patches/nanovg.c version adds these but doesn't expose them in
// nanovg.h, so declare them here. nvgTextBatchBegin captures the
// current fill / paint / font state and accumulates every subsequent
// nvgText call into a single render flush, dropped by nvgTextBatchEnd.
extern "C" {
    void nvgTextBatchBegin(NVGcontext* ctx);
    void nvgTextBatchEnd(NVGcontext* ctx);
}

namespace vitaplex {

// ============================================================================
// GuideBox
// ============================================================================
// borealis' default Box::getNextFocus for a COLUMN box just walks to the
// next sibling and calls getDefaultFocus on it. For our EPG rows that
// always lands on the row's first focusable view — the channel-logo
// column — instead of the program cell that aligns with the one the
// user came from.
//
// Match by the *start* of the source's box: walk the adjacent row for a
// focusable view whose horizontal range contains source.getX(). That
// way DOWN from a 1-hour show onto two 30-minute cells picks the *first*
// cell (because the 1-hour cell starts at the same X as the first
// 30-min cell), instead of bouncing onto the second when the source's
// centre happens to land on the cell boundary. Fall back to matching
// by the source's *end* X so a source that begins inside a gap still
// finds a target whose end aligns. Walks further rows on visibility
// gaps so the navigation doesn't dead-end on culled or empty rows.
class GuideBox : public brls::Box {
public:
    brls::View* getNextFocus(brls::FocusDirection direction,
                             brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN ||
            direction == brls::FocusDirection::UP) {
            brls::View* row = currentView;
            while (row && row->getParent() != this) row = row->getParent();
            if (row) {
                auto& kids = getChildren();
                int idx = -1;
                for (int i = 0; i < (int)kids.size(); i++) {
                    if (kids[i] == row) { idx = i; break; }
                }
                int step = (direction == brls::FocusDirection::DOWN) ? 1 : -1;
                int targetIdx = idx + step;
                // `currentView` is whatever bubbled up the focus chain
                // (a rowBox or programsBox), not the actually-focused
                // leaf cell. Probe by the real focused view so the X
                // range matches the cell the user *sees* highlighted.
                brls::View* focused = brls::Application::getCurrentFocus();
                if (!focused) focused = currentView;
                // Use a small epsilon-back from the source's right edge so a
                // box ending at exactly 440 looks at 439 (which falls inside
                // a target cell of [320, 440)) instead of 440 (which is the
                // start of the next cell).
                const float sourceStart = focused->getX();
                const float sourceEnd   = sourceStart + focused->getWidth() - 0.5f;
                while (idx >= 0 && targetIdx >= 0 && targetIdx < (int)kids.size()) {
                    brls::View* targetRow = kids[targetIdx];
                    if (targetRow->getVisibility() == brls::Visibility::VISIBLE) {
                        if (brls::View* hit = findFocusableContainingX(targetRow, sourceStart)) return hit;
                        if (brls::View* hit = findFocusableContainingX(targetRow, sourceEnd))   return hit;
                    }
                    targetIdx += step;
                }
            }
        }
        return brls::Box::getNextFocus(direction, currentView);
    }

private:
    // Return the first focusable descendant of `root` whose horizontal
    // range [X, X+W) contains the probe X.
    static brls::View* findFocusableContainingX(brls::View* root, float x) {
        if (!root || root->getVisibility() != brls::Visibility::VISIBLE) return nullptr;
        if (root->isFocusable()) {
            float cl = root->getX();
            float cr = cl + root->getWidth();
            if (x >= cl && x < cr) return root;
        }
        brls::Box* box = dynamic_cast<brls::Box*>(root);
        if (box) {
            for (auto* child : box->getChildren()) {
                if (brls::View* hit = findFocusableContainingX(child, x)) return hit;
            }
        }
        return nullptr;
    }
};

// ============================================================================
// Design tokens
// ============================================================================
// Borealis already exposes most of these via the dark theme, but the hero
// + guide redesign mixes a couple of bespoke shades (current-cell tint,
// cyan accent for the now-line) so we collect them in one place for
// consistency rather than scattering nvgRGBA literals across the build
// methods.
namespace tok {
    // Plex brand colours — the standard yellow (#E5A00D) and a deeper
    // gold (#CC7B19) for the EPG "on now" outline / current-time line
    // / progress bar fill so the app reads as a Plex client instead
    // of a generic teal-accented one.
    static inline NVGcolor accent()       { return vitaplex::palette::gold; }
    static inline NVGcolor accentDeep()   { return vitaplex::palette::goldDeep; }
    static inline NVGcolor live()         { return vitaplex::palette::live; }
    static inline NVGcolor text()         { return vitaplex::palette::text; }
    static inline NVGcolor muted()        { return vitaplex::palette::muted; }
    static inline NVGcolor dim()          { return vitaplex::palette::dim; }
    static inline NVGcolor card()         { return vitaplex::palette::surface; }
    static inline NVGcolor cardRaised()   { return vitaplex::palette::surface2; }
    static inline NVGcolor hairline()     { return vitaplex::palette::line; }
    static inline NVGcolor hero()         { return vitaplex::palette::panel; }
    static inline NVGcolor cellUpcoming() { return vitaplex::palette::surface2; }
    // "On now" cell fill — warm tint that reads as Plex-yellow-adjacent
    // instead of the old teal-blue (47,68,82) which fought the accent.
    static inline NVGcolor cellNow()      { return nvgRGB(70, 56, 32); }
    static inline NVGcolor placeholder()  { return vitaplex::palette::surface; }
    static inline NVGcolor primaryInk()   { return vitaplex::palette::goldInk; }
    static inline NVGcolor btnSecondary() { return vitaplex::palette::surface3; }
}

// Constants for EPG grid layout. Time-slot / row / channel-column widths are
// derived from the platform layer at runtime so larger-screen builds get wider
// cells than the Vita default.
static const int TIME_HEADER_HEIGHT = 36;

static inline int livetvTimeSlotWidth() {
    return platform::getImageConstraints().livetvChannelCardWidth;
}
static inline int livetvRowHeight() {
    // Just tall enough to fit the channel logo + channel-number label in
    // the sticky column. The cell only needs ~32px for the title +
    // time-range stack, the rest was empty space the user flagged.
    return std::max(50, platform::getImageConstraints().listRowHeight - 2);
}
// Sticky channel-column logo: a wide 16:9 chip (channel logos are
// banners, not avatars). Height anchored to livetvRowHeight() so it
// scales with the row.
static inline int gridChannelLogoHeight() {
    return std::max(28, livetvRowHeight() - 18);
}
static inline int gridChannelLogoWidth() {
    return (int)((float)gridChannelLogoHeight() * 16.0f / 9.0f);
}
// Column hugs the logo with a small horizontal margin — was max(110, …),
// which left a lot of dead space either side of the logo.
static inline int livetvChannelColWidth() {
    return gridChannelLogoWidth() + 14;
}

// Cache durations
static const int64_t FULL_RELOAD_INTERVAL = 300;   // 5 minutes between full EPG reloads
static const int64_t REFRESH_INTERVAL = 60;         // 1 minute between "now playing" refreshes

// EPG grid render window. Each row's program cells now sit inside an
// HScrollingFrame, so cells past the visible width are reachable via the
// dpad (cf. m_rowProgramScrolls). The render path uses whatever window
// LiveTVTab::m_hoursToShow holds — seeded from Application::getSettings()
// .liveTvGuideHours, which is clamped to 1-48 on load.

// Hero dimensions. Smaller than the original so more guide rows are
// visible below — thumbnail scales down with the hero height, and the
// info column rearranges around it. Was max(220, channelRowHeight*2+40)
// which left only ~3 guide rows on Vita; this drops the hero by ~70px
// for an extra ~1.5 channel rows.
static inline int heroHeight() {
    return std::max(150, platform::getImageConstraints().livetvChannelRowHeight + 50);
}
static inline int heroThumbWidth() {
    // 4:3 of the hero's inner height — slightly less wide than the
    // original 16:9 so the info column gets more horizontal real
    // estate for the title + summary at the smaller hero size.
    return std::max(170, (int)((heroHeight() - 16) * 4.0 / 3.0));
}


// ── LTVPROF: one-shot build/load profiling ─────────────────────────────
// Every step of building the Live TV UI logs its duration with an LTVPROF
// prefix (grep vitaplex.log for LTVPROF): fetch + JSON parse phases in
// PlexClient::fetchEPGGrid, view construction here, logo loads, hero
// updates, and the first frame after a grid rebuild (which pays the
// initial full-tree yoga layout).
static bool                 s_profFirstDrawPending = false;
static std::atomic<int>     s_profLogoQueued{0};
static std::atomic<int>     s_profLogoDone{0};
static std::atomic<int64_t> s_profLogoLatencyUs{0};
// formatTime() cost (localtime+strftime per call — a suspect for the row
// build: ~500 cells x 2+ calls). Reset per buildEPGGrid.
static std::atomic<int64_t> s_profFmtUs{0};
static std::atomic<int>     s_profFmtCalls{0};

LiveTVTab::LiveTVTab() {
    const int64_t profCtor0 = brls::getCPUTimeUsec();
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // The whole tab is a flex column with NO outer page scroll — the
    // guide gets its own inner scroll instead, so dpad-driven hover only
    // moves one scroll frame at a time. (Two CENTERED scroll frames
    // stacked would each scroll on every focus change and "double-scroll"
    // the page.) The hero, guide and DVR sit at fixed positions; only the
    // guide rows scroll inside m_guideScrollV.
    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);
    m_scrollContent->setPadding(20);
    m_scrollContent->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Live TV");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(16);
    m_scrollContent->addView(m_titleLabel);

    // On-Now hero (single big focusable card at the top)
    const int64_t profHero0 = brls::getCPUTimeUsec();
    buildHero();
    m_scrollContent->addView(m_heroBox);
    brls::Logger::info("LTVPROF buildHero: {}ms",
                       (brls::getCPUTimeUsec() - profHero0) / 1000);

    // EPG Guide — the dominant block.
    m_guideLabel = new brls::Label();
    m_guideLabel->setText("Program Guide");
    m_guideLabel->setFontSize(20);
    m_guideLabel->setMarginBottom(8);
    m_guideLabel->setMarginTop(4);
    m_scrollContent->addView(m_guideLabel);

    // Guide block: takes whatever space is left after hero + DVR. Uses
    // setGrow so it expands on taller screens and shrinks on Vita.
    m_guideContainer = new brls::Box();
    m_guideContainer->setAxis(brls::Axis::COLUMN);
    m_guideContainer->setGrow(1.0f);
    m_guideContainer->setMarginBottom(20);
    m_guideContainer->setBackgroundColor(tok::card());
    m_guideContainer->setCornerRadius(14);
    m_guideContainer->setBorderColor(tok::hairline());
    m_guideContainer->setBorderThickness(1);

    // Time header row (horizontal scroll, offset to clear the sticky
    // channel column on the left). Mark it non-focusable because the
    // slot labels inside are not interactive — if we left it focusable,
    // DOWN from the hero would land on the empty time strip first (it's
    // the guide container's first child), forcing a second press to
    // descend into the rows.
    m_timeHeaderScroll = new brls::HScrollingFrame();
    m_timeHeaderScroll->setHeight(TIME_HEADER_HEIGHT);
    m_timeHeaderScroll->setMarginLeft(livetvChannelColWidth());
    m_timeHeaderScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    m_timeHeaderScroll->setFocusable(false);

    m_timeHeaderBox = new brls::Box();
    m_timeHeaderBox->setAxis(brls::Axis::ROW);
    m_timeHeaderBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_timeHeaderBox->setBackgroundColor(nvgRGBA(0, 0, 0, 56));  // ~22% black overlay
    m_timeHeaderScroll->setContentView(m_timeHeaderBox);
    m_guideContainer->addView(m_timeHeaderScroll);

    // Inner vertical scroll for channel rows. This is the *only* scroll
    // frame on the page so hover-driven scroll moves it alone, leaving
    // the hero + DVR pinned to their slots above and below.
    m_guideScrollV = new brls::ScrollingFrame();
    m_guideScrollV->setGrow(1.0f);
    m_guideScrollV->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    // The ScrollingFrame ctor sets focusable=true, but our cells inside
    // are the real focus targets. Leaving the scroll focusable made
    // DOWN from the hero land on the scroll frame itself first (felt
    // like an invisible stop in between), forcing a second DOWN to
    // actually reach a guide cell. Mark it non-focusable so
    // getDefaultFocus descends straight to the cells.
    m_guideScrollV->setFocusable(false);

    m_guideBox = new GuideBox();
    m_guideBox->setAxis(brls::Axis::COLUMN);
    m_guideBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_guideBox->setAlignItems(brls::AlignItems::STRETCH);
    m_guideScrollV->setContentView(m_guideBox);
    m_guideContainer->addView(m_guideScrollV);

    // Current-time vertical line: an absolutely-positioned cyan rule
    // overlaid on the guide container. Positioned by updateCurrentTimeLine
    // once the grid is built and any time the layout refreshes.
    m_currentTimeLine = new brls::Box();
    m_currentTimeLine->setPositionType(brls::PositionType::ABSOLUTE);
    m_currentTimeLine->setBackgroundColor(tok::accent());
    m_currentTimeLine->setWidth(2);
    m_currentTimeLine->setHeight(1);  // grows in updateCurrentTimeLine
    m_currentTimeLine->setPositionTop(0);
    m_currentTimeLine->setPositionLeft(-1);
    m_currentTimeLine->setVisibility(brls::Visibility::INVISIBLE);
    m_guideContainer->addView(m_currentTimeLine);

    m_scrollContent->addView(m_guideContainer);

    // No outer scroll: m_scrollContent goes straight into the tab so the
    // hero stays pinned at the top and only the guide scrolls.
    this->addView(m_scrollContent);

    brls::Logger::debug("LiveTVTab: Loading content...");
    brls::Logger::info("LTVPROF LiveTVTab ctor (shell views): {}ms",
                       (brls::getCPUTimeUsec() - profCtor0) / 1000);
    loadChannels();
}

LiveTVTab::~LiveTVTab() {
    if (m_alive) { *m_alive = false; }
    if (m_heroThumbAlive) { m_heroThumbAlive->store(false); }
}

void LiveTVTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    if (m_heroThumbAlive) m_heroThumbAlive->store(false);
    ImageLoader::cancelAll();
}

bool LiveTVTab::isDescendantOf(brls::View* view, brls::View* ancestor) {
    brls::View* current = view;
    while (current) {
        if (current == ancestor) return true;
        current = current->getParent();
    }
    return false;
}

brls::View* LiveTVTab::findFirstFocusableInBox(brls::Box* box) {
    if (!box) return nullptr;
    for (auto* child : box->getChildren()) {
        if (child->getVisibility() != brls::Visibility::VISIBLE) continue;
        if (child->isFocusable()) return child;
        brls::Box* childBox = dynamic_cast<brls::Box*>(child);
        if (childBox) {
            brls::View* found = findFirstFocusableInBox(childBox);
            if (found) return found;
        }
    }
    return nullptr;
}

brls::View* LiveTVTab::findLastFocusableInBox(brls::Box* box) {
    if (!box) return nullptr;
    auto& children = box->getChildren();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        brls::View* child = *it;
        if (child->getVisibility() != brls::Visibility::VISIBLE) continue;
        brls::Box* childBox = dynamic_cast<brls::Box*>(child);
        if (childBox) {
            brls::View* found = findLastFocusableInBox(childBox);
            if (found) return found;
        }
        if (child->isFocusable()) return child;
    }
    return nullptr;
}

brls::View* LiveTVTab::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    // Section layout (top to bottom):  Hero (m_heroBox) -> Guide (m_guideBox)
    // Row-to-row movement *inside* the guide is handled by borealis' natural
    // navigation (m_guideBox is a COLUMN box, so it resolves UP/DOWN between
    // rows itself). This override only kicks in when that bubbles up to the
    // tab — i.e. at a section boundary — so all we do is hop to the adjacent
    // section.
    const bool inHero  = isDescendantOf(currentView, m_heroBox);
    const bool inGuide = isDescendantOf(currentView, m_guideContainer);

    if (direction == brls::FocusDirection::DOWN && inHero) {
        if (brls::View* t = findFirstFocusableInBox(m_guideBox)) return t;
    }
    else if (direction == brls::FocusDirection::UP && inGuide) {
        if (brls::View* t = findFirstFocusableInBox(m_heroBox)) return t;
    }

    // Default behavior for left/right and unhandled cases
    return brls::Box::getNextFocus(direction, currentView);
}

void LiveTVTab::cullToViewport(brls::Box* content, brls::View* viewport, bool vertical) {
    if (!content || !viewport) return;

    const float margin = vertical ? (float)livetvRowHeight() : (float)livetvTimeSlotWidth();

    const float vpStart = vertical ? viewport->getY() : viewport->getX();
    const float vpEnd   = vpStart + (vertical ? viewport->getHeight() : viewport->getWidth());

    brls::View* focus = brls::Application::getCurrentFocus();

    for (brls::View* child : content->getChildren()) {
        const float cStart = vertical ? child->getY() : child->getX();
        const float cEnd   = cStart + (vertical ? child->getHeight() : child->getWidth());

        bool visible = (cEnd >= vpStart - margin) && (cStart <= vpEnd + margin);

        if (!visible && focus && isDescendantOf(focus, child))
            visible = true;

        const brls::Visibility want = visible ? brls::Visibility::VISIBLE
                                              : brls::Visibility::INVISIBLE;
        if (child->getVisibility() != want)
            child->setVisibility(want);
    }
}

void LiveTVTab::draw(NVGcontext* vg, float x, float y, float width, float height,
                     brls::Style style, brls::FrameContext* ctx) {
    const int64_t pf0 = brls::getCPUTimeUsec();
    if (m_perfLastFrameUs > 0) m_perfFrameUs += pf0 - m_perfLastFrameUs;
    m_perfLastFrameUs = pf0;

    // With 100+ channels the EPG grid holds ~100 rows and the favourites /
    // DVR rows hold ~100 cards each. borealis only culls off-screen *leaf*
    // views, never nested Boxes, so every off-screen row/card still painted
    // every frame — pure overdraw. Toggle INVISIBLE on anything scrolled
    // out of its viewport so frame() early-outs for the entire subtree.
    cullToViewport(m_guideBox, m_guideScrollV, /*vertical=*/true);

    // Same idea horizontally, inside the rows that survived the vertical
    // cull: a row spans the whole guide window (~15-25 cell Boxes) but only
    // ~5 cells are on screen. The HScrollingFrame scissor only hides the
    // result — every off-screen cell still recorded its rounded-rect
    // background each frame.
    for (size_t i = 0; i < m_rowProgramScrolls.size() && i < m_rowProgramBoxes.size(); i++) {
        brls::HScrollingFrame* hs = m_rowProgramScrolls[i];
        brls::Box* content = m_rowProgramBoxes[i];
        if (!hs || !content) continue;
        brls::View* row = hs->getParent();
        if (row && row->getVisibility() != brls::Visibility::VISIBLE) continue;
        cullToViewport(content, hs, /*vertical=*/false);
    }

    // The time header rides its own HScrollingFrame with 2-4x more slot
    // boxes (label + hairline each) than fit on screen — cull those too.
    if (m_timeHeaderBox && m_timeHeaderScroll)
        cullToViewport(m_timeHeaderBox, m_timeHeaderScroll, /*vertical=*/false);

    // Lazy logo loading: request each channel's logo the first time its row
    // survives the cull above. O(rows) flag checks per frame.
    for (RowLogo& rl : m_rowLogos) {
        if (rl.requested || !rl.row || !rl.img) continue;
        if (rl.row->getVisibility() != brls::Visibility::VISIBLE) continue;
        rl.requested = true;
        s_profLogoQueued.fetch_add(1);
        const int64_t profQ0 = brls::getCPUTimeUsec();
        ImageLoader::loadAsync(rl.url, [profQ0](brls::Image* img) {
            if (img) img->setVisibility(brls::Visibility::VISIBLE);
            const int done = s_profLogoDone.fetch_add(1) + 1;
            s_profLogoLatencyUs.fetch_add(brls::getCPUTimeUsec() - profQ0);
            const int queued = s_profLogoQueued.load();
            if (done % 10 == 0 || done == queued)
                brls::Logger::info("LTVPROF logos: {}/{} loaded, avg {}ms each",
                                   done, queued,
                                   s_profLogoLatencyUs.load() / done / 1000);
        }, rl.img, m_logoAlive);
    }

    const int64_t pf1 = brls::getCPUTimeUsec();
    m_perfCullUs += pf1 - pf0;

    // Slide the cyan "now" line each frame so it tracks the wall clock
    // even when the guide grid itself doesn't rebuild.
    updateCurrentTimeLine();

    // Horizontal scroll sync. Each guide row has its own HScrollingFrame
    // for the program cells (so RIGHT navigation can scroll past the
    // visible width), but the user only sees one row's scroll *react*
    // to their input. Match the other rows + the time header to the
    // focused row's offset so the visible time slice is consistent
    // across the whole guide — and so the time labels above always
    // align with the cells below.
    if (!m_rowProgramScrolls.empty()) {
        brls::View* focused = brls::Application::getCurrentFocus();
        brls::HScrollingFrame* anchor = nullptr;
        if (focused) {
            for (brls::HScrollingFrame* hs : m_rowProgramScrolls) {
                if (isDescendantOf(focused, hs)) { anchor = hs; break; }
            }
        }
        if (anchor) {
            // Focus moved to a different row: align its internal offset with
            // the shared offset every row is already displaying, so its own
            // centering starts from what's on screen instead of a stale
            // value. One invalidate per vertical move — a discrete event.
            if (anchor != m_lastAnchorScroll) {
                if (m_lastSyncedScrollX >= 0 &&
                    std::abs(anchor->getContentOffsetX() - m_lastSyncedScrollX) > 0.5f)
                    anchor->setContentOffsetX(m_lastSyncedScrollX, false);
                m_lastAnchorScroll = anchor;
            }
            const float target = anchor->getContentOffsetX();
            // Steady-state (not actively scrolling) is the common case —
            // the anchor's offset matches what we already pushed to the
            // other rows. Skip the whole O(rows) sync loop in that case.
            if (std::abs(target - m_lastSyncedScrollX) > 0.5f) {
                // Follow the anchor by translating the other rows' content
                // boxes directly — a plain float store, and exactly how
                // borealis applies scroll offsets internally (see
                // HScrollingFrame::scrollAnimationTick). The previous
                // setContentOffsetX path ran invalidate() per row, i.e. a
                // full-tree Yoga relayout of the ~1500-view grid PER ROW
                // PER FRAME while the anchor's scroll animated — the
                // guide's 4 FPS on Vita.
                for (size_t i = 0; i < m_rowProgramScrolls.size(); i++) {
                    brls::HScrollingFrame* hs = m_rowProgramScrolls[i];
                    brls::Box* content = i < m_rowProgramBoxes.size() ? m_rowProgramBoxes[i] : nullptr;
                    if (!hs || !content || hs == anchor) continue;
                    // Clamp to this row's own scrollable range, like
                    // borealis does — rows whose programs end early have
                    // narrower content than the full guide window.
                    float limit = content->getWidth() - hs->getWidth();
                    if (limit < 0) limit = 0;
                    float t = target < 0 ? 0 : (target > limit ? limit : target);
                    content->setTranslationX(-t);
                }
                if (m_timeHeaderBox && m_timeHeaderScroll) {
                    float limit = m_timeHeaderBox->getWidth() - m_timeHeaderScroll->getWidth();
                    if (limit < 0) limit = 0;
                    float t = target < 0 ? 0 : (target > limit ? limit : target);
                    m_timeHeaderBox->setTranslationX(-t);
                }
                m_lastSyncedScrollX = target;
            }
        }
    }

    // Hover-driven hero refresh, applied only once focus has rested.
    applyPendingHero();
    const int64_t pf2 = brls::getCPUTimeUsec();
    m_perfSyncUs += pf2 - pf1;

    brls::Box::draw(vg, x, y, width, height, style, ctx);
    const int64_t pf3 = brls::getCPUTimeUsec();
    m_perfDrawUs += pf3 - pf2;

    // Batch text rendering for the EPG cells. Every cell that's visible
    // in its row's HScrollingFrame contributes one title and one subtitle
    // string; the patched nvgTextBatchBegin/End API lets us flush all of
    // them as a single render call per style. A typical visible window
    // is ~80–120 cells, so this collapses ~200 per-Label draw calls into
    // 2 batched ones. No path fills may happen between Begin and End or
    // they'd clobber the batch — only nvgText.
    [&]() {
        if (m_epgRowRanges.empty() || m_epgCells.empty() || !m_guideScrollV) return;
        const float vScrollTop    = m_guideScrollV->getY();
        const float vScrollBottom = vScrollTop + m_guideScrollV->getHeight();

        // All HScrollingFrames live in the same column (channel column
        // is fixed-width on the left), so any cell's scroll frame gives
        // the program-area X range. Pick the first cell whose scroll
        // frame is laid out and use it to clip the batch — without the
        // scissor, cells panning under the channel column would still
        // paint their text on top of the logo because batched draws
        // bypass the per-frame scissor that the HScrollingFrame sets.
        float clipX = 0.0f, clipW = 0.0f;
        for (const EpgCellInfo& info : m_epgCells) {
            if (!info.scroll) continue;
            const float w = info.scroll->getWidth();
            if (w <= 0) continue;
            clipX = info.scroll->getX();
            clipW = w;
            break;
        }
        if (clipW <= 0) return;

        // Resolve visible cells once, then iterate the (much smaller)
        // filtered list twice — once per batch — instead of redoing the
        // ~7 getter calls per cell on every pass.
        struct VisibleCell {
            float x, y, w, h;   // cell rect (batched background fill)
            float tx, ty;       // text origin
            bool onNow;
            const std::string* title;
            const std::string* subtitle;
        };
        std::vector<VisibleCell> visible;
        visible.reserve(m_epgCells.size());

        // Subtitles (time ranges) render only for the FOCUSED row: LTVPROF
        // measured the batched text pass at ~15.6ms/frame while navigating,
        // roughly half of it the time-range line under every visible cell.
        // Titles stay on every cell; the hero shows full times for the
        // focused program anyway.
        brls::Box* focusedRowBox = nullptr;
        {
            brls::View* fc = brls::Application::getCurrentFocus();
            if (fc) {
                for (const EpgRowRange& rr : m_epgRowRanges) {
                    if (rr.row && isDescendantOf(fc, rr.row)) { focusedRowBox = rr.row; break; }
                }
            }
        }
        static const std::string kNoSubtitle;

        for (const EpgRowRange& rr : m_epgRowRanges) {
            // One visibility check skips a culled row's entire cell range —
            // previously every cell struct in the grid (channels × programs,
            // easily >1000 entries) was walked per frame just to find the
            // on-screen handful.
            if (rr.row && rr.row->getVisibility() != brls::Visibility::VISIBLE) continue;
            for (size_t ci = rr.begin; ci < rr.end && ci < m_epgCells.size(); ci++) {
            const EpgCellInfo& info = m_epgCells[ci];
            if (!info.cell || !info.scroll) continue;
            // Horizontally-culled cells are INVISIBLE (cell cull above) —
            // skip before paying the recursive position getters.
            if (info.cell->getVisibility() != brls::Visibility::VISIBLE) continue;

            const float cx = info.cell->getX();
            const float cy = info.cell->getY();
            const float cw = info.cell->getWidth();
            const float ch = info.cell->getHeight();
            if (cw <= 0 || ch <= 0) continue;

            const float hScrollLeft  = info.scroll->getX();
            const float hScrollRight = hScrollLeft + info.scroll->getWidth();
            if (cx + cw < hScrollLeft || cx > hScrollRight) continue;
            if (cy + ch < vScrollTop  || cy > vScrollBottom) continue;

            VisibleCell v;
            v.x = cx; v.y = cy; v.w = cw; v.h = ch;
            v.tx = cx + 8.0f;
            v.ty = cy + (ch - 26.0f) * 0.5f;
            v.onNow    = info.onNow;
            v.title    = &info.title;
            v.subtitle = (rr.row && rr.row == focusedRowBox) ? &info.subtitle
                                                             : &kNoSubtitle;
            visible.push_back(v);
            }
        }

        if (visible.empty()) return;

        nvgSave(vg);
        nvgScissor(vg, clipX, vScrollTop, clipW, vScrollBottom - vScrollTop);

        // Batched cell backgrounds. Every visible cell's rounded rect merges
        // into ONE path per colour (plus one stroke pass for the on-now
        // borders), collapsing ~50-90 individually stenciled antialiased
        // NanoVG fills into 3. The hardware profile showed the GPU pinned at
        // 100% while CPU cores sat half idle — per-path stencil work is the
        // wall on the Vita's tile-based GPU, so path count is what matters.
        int nowCells = 0, upcomingCells = 0;
        for (const VisibleCell& v : visible) (v.onNow ? nowCells : upcomingCells)++;

        // AA fringes are the bulk of NanoVG's per-path stencil geometry, and
        // on 6px-radius guide cells they're visually irrelevant — turn shape
        // AA off for the batched fills/strokes (restored below).
        nvgShapeAntiAlias(vg, 0);

        if (upcomingCells > 0) {
            nvgBeginPath(vg);
            for (const VisibleCell& v : visible)
                if (!v.onNow) nvgRoundedRect(vg, v.x, v.y, v.w, v.h, 6.0f);
            nvgFillColor(vg, tok::cellUpcoming());
            nvgFill(vg);
        }
        if (nowCells > 0) {
            nvgBeginPath(vg);
            for (const VisibleCell& v : visible)
                if (v.onNow) nvgRoundedRect(vg, v.x, v.y, v.w, v.h, 6.0f);
            nvgFillColor(vg, tok::cellNow());
            nvgFill(vg);

            nvgBeginPath(vg);
            for (const VisibleCell& v : visible)
                if (v.onNow) nvgRoundedRect(vg, v.x + 0.5f, v.y + 0.5f,
                                            v.w - 1.0f, v.h - 1.0f, 6.0f);
            nvgStrokeColor(vg, tok::accent());
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }

        nvgShapeAntiAlias(vg, 1);

        nvgFontFace(vg, "regular");
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

        // Pass 1: titles
        nvgFontSize(vg, 13);
        nvgFillColor(vg, tok::text());
        nvgTextBatchBegin(vg);
        for (const VisibleCell& v : visible) {
            if (v.title->empty()) continue;
            nvgText(vg, v.tx, v.ty, v.title->c_str(), nullptr);
        }
        nvgTextBatchEnd(vg);

        // Pass 2: subtitles (smaller, dim)
        nvgFontSize(vg, 10);
        nvgFillColor(vg, tok::dim());
        nvgTextBatchBegin(vg);
        for (const VisibleCell& v : visible) {
            if (v.subtitle->empty()) continue;
            nvgText(vg, v.tx, v.ty + 16.0f, v.subtitle->c_str(), nullptr);
        }
        nvgTextBatchEnd(vg);
        nvgRestore(vg);
    }();

    const int64_t pf4 = brls::getCPUTimeUsec();
    m_perfTextUs += pf4 - pf3;

    // Periodic cost report (~every 5-20s depending on frame rate). On Vita
    // this lands in ux0:data/VitaPlex/vitaplex.log, so a hardware log shows
    // exactly where guide frame time goes: total = wall time between draws
    // (everything incl. GPU/other views), the rest are this tab's sections.
    if (s_profFirstDrawPending) {
        s_profFirstDrawPending = false;
        // The first frame after a grid rebuild pays the initial full-tree
        // yoga layout of the freshly built forest — worth its own line.
        brls::Logger::info(
            "LTVPROF first frame after grid build: cull={}us sync={}us boxdraw={}us text={}us",
            (int)(pf1 - pf0), (int)(pf2 - pf1), (int)(pf3 - pf2), (int)(pf4 - pf3));
    }

    if (++m_perfFrames >= 300) {
        brls::Logger::info(
            "LiveTV perf avg us/frame over {} frames: total={} cull={} sync={} boxdraw={} text={}",
            m_perfFrames,
            m_perfFrameUs / m_perfFrames, m_perfCullUs / m_perfFrames,
            m_perfSyncUs / m_perfFrames, m_perfDrawUs / m_perfFrames,
            m_perfTextUs / m_perfFrames);
        m_perfFrameUs = m_perfCullUs = m_perfSyncUs = m_perfDrawUs = m_perfTextUs = 0;
        m_perfFrames  = 0;
    }
}

void LiveTVTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    // Pick up the user's EPG-hours setting on every focus so a change in
    // Settings → Live TV takes effect next time they tab back here,
    // forcing a fresh fetch when the window size shifted.
    int settingsHours = Application::getInstance().getSettings().liveTvGuideHours;
    if (settingsHours > 0 && settingsHours != m_hoursToShow) {
        m_hoursToShow = settingsHours;
        m_loaded = false;
    }

    if (!m_loaded) {
        loadChannels();
    } else {
        int64_t now = time(nullptr);
        if (now - m_lastFullLoadTime > FULL_RELOAD_INTERVAL) {
            brls::Logger::debug("LiveTVTab: Full EPG reload (stale data)");
            loadChannels();
        } else if (now - m_lastRefreshTime > REFRESH_INTERVAL) {
            brls::Logger::debug("LiveTVTab: Refreshing current programs only");
            refreshCurrentPrograms();
        }
    }
}

std::string LiveTVTab::formatTime(int64_t timestamp) {
    const int64_t profF0 = brls::getCPUTimeUsec();
    if (timestamp == 0) timestamp = time(nullptr);

    time_t t = (time_t)timestamp;
    struct tm* tm_info = localtime(&t);

    char buffer[16];
    strftime(buffer, sizeof(buffer), "%I:%M %p", tm_info);
    s_profFmtUs.fetch_add(brls::getCPUTimeUsec() - profF0);
    s_profFmtCalls.fetch_add(1);
    if (buffer[0] == '0') return std::string(buffer + 1);
    return std::string(buffer);
}

// ============================================================================
// Hero panel
// ============================================================================

void LiveTVTab::buildHero() {
    m_heroBox = new brls::Box();
    m_heroBox->setAxis(brls::Axis::ROW);
    m_heroBox->setHeight(heroHeight());
    m_heroBox->setMarginBottom(16);
    m_heroBox->setPadding(8);
    m_heroBox->setBackgroundColor(tok::hero());
    m_heroBox->setCornerRadius(14);
    m_heroBox->setBorderColor(tok::hairline());
    m_heroBox->setBorderThickness(1);
    // Deliberately NOT focusable: borealis' getDefaultFocus stops at the
    // first focusable ancestor, so making the whole hero focusable would
    // hide the Watch live / Record buttons inside it from the focus chain.
    // The buttons themselves are the actual focus targets.

    // Thumbnail holder. The Image itself is added invisible — we flip it
    // on once the async load finishes; the holder Box owns the slot in
    // the meantime so layout doesn't jump.
    m_heroThumbHolder = new brls::Box();
    m_heroThumbHolder->setWidth(heroThumbWidth());
    m_heroThumbHolder->setHeight(heroHeight() - 16);
    m_heroThumbHolder->setMarginRight(14);
    // Match the hero card's background so the letterbox/pillarbox bars
    // that a FIT-scaled image leaves around its natural aspect ratio
    // blend in with the surrounding card instead of reading as a slate
    // panel under the image.
    m_heroThumbHolder->setBackgroundColor(tok::hero());
    m_heroThumbHolder->setCornerRadius(10);

    m_heroThumb = new brls::Image();
    m_heroThumb->setWidth(heroThumbWidth());
    m_heroThumb->setHeight(heroHeight() - 16);
    m_heroThumb->setScalingType(brls::ImageScalingType::FIT);
    m_heroThumb->setCornerRadius(10);
    m_heroThumb->setVisibility(brls::Visibility::INVISIBLE);
    m_heroThumbHolder->addView(m_heroThumb);
    m_heroBox->addView(m_heroThumbHolder);

    // Right info column.
    auto* info = new brls::Box();
    info->setAxis(brls::Axis::COLUMN);
    info->setGrow(1.0f);
    info->setPadding(4);
    info->setJustifyContent(brls::JustifyContent::FLEX_START);

    // Row: LIVE badge + channel name + channel id.
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setAlignItems(brls::AlignItems::CENTER);
    topRow->setHeight(22);
    topRow->setMarginBottom(8);

    m_heroLiveBadge = new brls::Box();
    m_heroLiveBadge->setWidth(46);
    m_heroLiveBadge->setHeight(20);
    m_heroLiveBadge->setBackgroundColor(tok::live());
    m_heroLiveBadge->setCornerRadius(4);
    m_heroLiveBadge->setJustifyContent(brls::JustifyContent::CENTER);
    m_heroLiveBadge->setAlignItems(brls::AlignItems::CENTER);
    m_heroLiveBadge->setMarginRight(10);
    auto* liveTxt = new brls::Label();
    liveTxt->setText("LIVE");
    liveTxt->setFontSize(11);
    liveTxt->setTextColor(tok::text());
    m_heroLiveBadge->addView(liveTxt);
    topRow->addView(m_heroLiveBadge);

    m_heroChannelName = new brls::Label();
    m_heroChannelName->setFontSize(14);
    m_heroChannelName->setTextColor(tok::text());
    m_heroChannelName->setMarginRight(8);
    topRow->addView(m_heroChannelName);

    m_heroChannelId = new brls::Label();
    m_heroChannelId->setFontSize(12);
    m_heroChannelId->setTextColor(tok::muted());
    topRow->addView(m_heroChannelId);

    info->addView(topRow);

    // Program title — single line so a long episode title can't wrap
    // into the summary's slot below. The text is clamped in
    // updateHeroForProgram so it doesn't overflow horizontally either.
    m_heroTitleLabel = new brls::Label();
    m_heroTitleLabel->setFontSize(22);
    m_heroTitleLabel->setTextColor(tok::text());
    m_heroTitleLabel->setSingleLine(true);
    m_heroTitleLabel->setMarginBottom(4);
    info->addView(m_heroTitleLabel);

    // Summary — let it use the remaining vertical space between the title
    // and the progress row so two or three lines of description fit
    // instead of getting clipped.
    m_heroSummaryLabel = new brls::Label();
    m_heroSummaryLabel->setFontSize(13);
    m_heroSummaryLabel->setTextColor(tok::muted());
    m_heroSummaryLabel->setGrow(1.0f);
    m_heroSummaryLabel->setMarginBottom(8);
    info->addView(m_heroSummaryLabel);

    // Progress row: start time · [bar] · end time · pct.
    auto* progressRow = new brls::Box();
    progressRow->setAxis(brls::Axis::ROW);
    progressRow->setAlignItems(brls::AlignItems::CENTER);
    progressRow->setHeight(18);
    progressRow->setMarginBottom(10);

    m_heroStartLabel = new brls::Label();
    m_heroStartLabel->setFontSize(11);
    m_heroStartLabel->setTextColor(tok::dim());
    m_heroStartLabel->setWidth(56);
    progressRow->addView(m_heroStartLabel);

    m_heroProgressTrack = new brls::Box();
    m_heroProgressTrack->setHeight(5);
    m_heroProgressTrack->setGrow(1.0f);
    m_heroProgressTrack->setBackgroundColor(nvgRGBA(255, 255, 255, 33));
    m_heroProgressTrack->setCornerRadius(3);
    m_heroProgressTrack->setMarginLeft(8);
    m_heroProgressTrack->setMarginRight(8);

    m_heroProgressFill = new brls::Box();
    m_heroProgressFill->setHeight(5);
    m_heroProgressFill->setWidth(0);
    m_heroProgressFill->setBackgroundColor(tok::accent());
    m_heroProgressFill->setCornerRadius(3);
    m_heroProgressTrack->addView(m_heroProgressFill);
    progressRow->addView(m_heroProgressTrack);

    m_heroEndLabel = new brls::Label();
    m_heroEndLabel->setFontSize(11);
    m_heroEndLabel->setTextColor(tok::dim());
    m_heroEndLabel->setWidth(56);
    m_heroEndLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    progressRow->addView(m_heroEndLabel);

    m_heroPctLabel = new brls::Label();
    m_heroPctLabel->setFontSize(11);
    m_heroPctLabel->setTextColor(tok::accent());
    m_heroPctLabel->setWidth(40);
    m_heroPctLabel->setMarginLeft(8);
    m_heroPctLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    progressRow->addView(m_heroPctLabel);

    info->addView(progressRow);

    // Button row.
    auto* btnRow = new brls::Box();
    btnRow->setAxis(brls::Axis::ROW);
    btnRow->setHeight(36);
    btnRow->setAlignItems(brls::AlignItems::CENTER);

    m_heroWatchBtn = new brls::Box();
    m_heroWatchBtn->setHeight(34);
    m_heroWatchBtn->setWidth(140);
    m_heroWatchBtn->setMarginRight(10);
    m_heroWatchBtn->setPadding(8);
    m_heroWatchBtn->setCornerRadius(8);
    m_heroWatchBtn->setBackgroundColor(tok::accent());
    m_heroWatchBtn->setJustifyContent(brls::JustifyContent::CENTER);
    m_heroWatchBtn->setAlignItems(brls::AlignItems::CENTER);
    m_heroWatchBtn->setFocusable(true);
    // Drop the dark grey highlight overlay borealis paints behind a
    // focused view — on the cyan Watch live button it muddies the
    // background and makes the dark "▶  Watch live" text hard to read.
    // The focus border glow still draws so the user sees what's
    // selected.
    m_heroWatchBtn->setHideHighlightBackground(true);
    auto* watchTxt = new brls::Label();
    watchTxt->setText("▶  Watch live");
    watchTxt->setFontSize(14);
    watchTxt->setTextColor(tok::primaryInk());
    m_heroWatchBtn->addView(watchTxt);
    m_heroWatchBtn->registerClickAction([this](brls::View*) {
        onChannelSelected(m_heroChannel);
        return true;
    });
    // Touch support: a tap on the button triggers the primary action
    // (BUTTON_A click) just like the dpad would.
    m_heroWatchBtn->addGestureRecognizer(new brls::TapGestureRecognizer(m_heroWatchBtn));
    btnRow->addView(m_heroWatchBtn);

    m_heroRecordBtn = new brls::Box();
    m_heroRecordBtn->setHeight(34);
    m_heroRecordBtn->setWidth(110);
    m_heroRecordBtn->setPadding(8);
    m_heroRecordBtn->setCornerRadius(8);
    m_heroRecordBtn->setBackgroundColor(tok::btnSecondary());
    m_heroRecordBtn->setJustifyContent(brls::JustifyContent::CENTER);
    m_heroRecordBtn->setAlignItems(brls::AlignItems::CENTER);
    m_heroRecordBtn->setFocusable(true);
    m_heroRecordBtn->setHideHighlightBackground(true);
    auto* recTxt = new brls::Label();
    recTxt->setText("●  Record");
    recTxt->setFontSize(14);
    recTxt->setTextColor(tok::text());
    m_heroRecordBtn->addView(recTxt);
    m_heroRecordBtn->registerClickAction([this](brls::View*) {
        if (m_heroProgramValid) {
            scheduleRecording(m_heroProgram, m_heroChannel);
        } else {
            brls::Dialog* dialog = new brls::Dialog(
                std::string("No current program to record on ") + m_heroChannel.title);
            dialog->addButton("OK", []() {});
            dialog->open();
        }
        return true;
    });
    m_heroRecordBtn->addGestureRecognizer(new brls::TapGestureRecognizer(m_heroRecordBtn));
    btnRow->addView(m_heroRecordBtn);

    info->addView(btnRow);

    m_heroBox->addView(info);

    // Note: no card-wide click action — the box isn't focusable, so the
    // dpad lands on the Watch live / Record buttons directly. The thumbnail
    // remains a non-interactive visual.
}

// setText() always invalidates (a synchronous full-tree Yoga relayout),
// even for identical text. Guard every hero label so repeat updates only
// pay for the labels that actually changed.
static void setLabelText(brls::Label* label, const std::string& text) {
    if (label && label->getFullText() != text) label->setText(text);
}

void LiveTVTab::queueHeroForChannel(const LiveTVChannel& channel) {
    m_pendingHeroChannel    = channel;
    m_pendingHeroHasProgram = false;
    m_heroUpdatePending     = true;
    m_lastHoverUs           = brls::getCPUTimeUsec();
}

void LiveTVTab::queueHeroForProgram(const LiveTVChannel& channel, const GuideProgram& program) {
    m_pendingHeroChannel    = channel;
    m_pendingHeroProgram    = program;
    m_pendingHeroHasProgram = true;
    m_heroUpdatePending     = true;
    m_lastHoverUs           = brls::getCPUTimeUsec();
}

void LiveTVTab::applyPendingHero() {
    if (!m_heroUpdatePending) return;
    // Apply once focus has rested ~180 ms. Refreshing the hero on every
    // hover cost a dozen label/width relayouts plus a thumbnail HTTP
    // fetch per dpad press — dpad-repeat across the guide became a
    // relayout + network storm. Deferring to focus-rest keeps navigation
    // at full frame rate; the hero fills in the moment you stop.
    if (brls::getCPUTimeUsec() - m_lastHoverUs < 180000) return;
    m_heroUpdatePending = false;
    const int64_t profH0 = brls::getCPUTimeUsec();
    if (m_pendingHeroHasProgram)
        updateHeroForProgram(m_pendingHeroChannel, m_pendingHeroProgram);
    else
        updateHeroForChannel(m_pendingHeroChannel);
    // One line per hover-rest: the hero's ~10 label/width relayouts + the
    // thumb request are the main per-interaction cost left in this tab.
    brls::Logger::info("LTVPROF hero update (hover apply): {}ms",
                       (brls::getCPUTimeUsec() - profH0) / 1000);
}

void LiveTVTab::resizeHeroThumbToImage(brls::Image* img) {
    if (!img || !m_heroThumb || !m_heroThumbHolder) return;
    float nw = img->getOriginalImageWidth();
    float nh = img->getOriginalImageHeight();
    if (nw <= 0 || nh <= 0) return;

    // Keep the hero's inner height fixed; compute the width that matches
    // the loaded image's natural aspect ratio so a portrait poster gets
    // a portrait holder and a 16:9 still gets a 16:9 holder. Clamp to
    // sensible bounds so an extreme aspect doesn't squeeze the info
    // column or run off the card.
    int innerH = heroHeight() - 16;
    int targetW = (int)((float)innerH * (nw / nh));
    const int minW = 90;
    const int maxW = (int)((float)innerH * 16.0f / 9.0f) + 40;
    if (targetW < minW) targetW = minW;
    if (targetW > maxW) targetW = maxW;

    m_heroThumb->setWidth(targetW);
    m_heroThumbHolder->setWidth(targetW);
}

void LiveTVTab::updateHeroForChannel(const LiveTVChannel& channel) {
    // Find the currently-airing program and forward to the per-program
    // updater. Falls back to the channel's legacy currentProgram/start/end
    // fields if the EPG didn't fill the programs vector.
    time_t now = time(nullptr);
    GuideProgram nowProg;
    bool found = false;
    for (const auto& p : channel.programs) {
        if (p.startTime <= (int64_t)now && p.endTime > (int64_t)now) {
            nowProg.title       = p.title;
            nowProg.summary     = p.summary;
            nowProg.startTime   = p.startTime;
            nowProg.endTime     = p.endTime;
            nowProg.ratingKey   = p.ratingKey;
            nowProg.metadataKey = p.metadataKey;
            nowProg.thumb       = p.thumb;
            found = true;
            break;
        }
    }
    if (!found && !channel.currentProgram.empty() && channel.programStart > 0) {
        nowProg.title     = channel.currentProgram;
        nowProg.startTime = channel.programStart;
        nowProg.endTime   = channel.programEnd > 0
                            ? channel.programEnd
                            : channel.programStart + 1800;
        found = true;
    }

    if (found) {
        updateHeroForProgram(channel, nowProg);
        return;
    }

    // No program data — show channel-only state.
    m_heroChannel = channel;
    m_heroProgramValid = false;
    if (m_heroChannelName)
        setLabelText(m_heroChannelName, channel.callSign.empty() ? channel.title : channel.callSign);
    if (m_heroChannelId) {
        std::string chId = !channel.channelIdentifier.empty()
            ? "CH " + channel.channelIdentifier
            : (channel.channelNumber > 0 ? "CH " + std::to_string(channel.channelNumber) : "");
        setLabelText(m_heroChannelId, chId);
    }
    setLabelText(m_heroTitleLabel, channel.title);
    setLabelText(m_heroSummaryLabel, "No guide data");
    setLabelText(m_heroStartLabel, "");
    setLabelText(m_heroEndLabel, "");
    setLabelText(m_heroPctLabel, "");
    if (m_heroProgressFill) m_heroProgressFill->setWidth(0);
    if (m_heroLiveBadge)    m_heroLiveBadge->setVisibility(brls::Visibility::INVISIBLE);

    // Thumb falls back to the channel's own art.
    if (m_heroThumbAlive) m_heroThumbAlive->store(false);
    m_heroThumbAlive = std::make_shared<std::atomic<bool>>(true);
    if (m_heroThumb) m_heroThumb->setVisibility(brls::Visibility::INVISIBLE);
    // Reset to the default width while the new image loads so the
    // holder doesn't stay sized to the previous show's aspect ratio.
    if (m_heroThumbHolder) m_heroThumbHolder->setWidth(heroThumbWidth());
    if (m_heroThumb)       m_heroThumb->setWidth(heroThumbWidth());
    if (!channel.thumb.empty()) {
        PlexClient& client = PlexClient::getInstance();
        std::string url = client.getThumbnailUrl(channel.thumb, heroThumbWidth(), heroHeight() - 16);
        ImageLoader::loadAsync(url, [this](brls::Image* img) {
            if (!img) return;
            img->setVisibility(brls::Visibility::VISIBLE);
            resizeHeroThumbToImage(img);
        }, m_heroThumb, m_heroThumbAlive);
    }
}

void LiveTVTab::updateHeroForProgram(const LiveTVChannel& channel,
                                     const GuideProgram& program) {
    m_heroChannel      = channel;
    m_heroProgram      = program;
    m_heroProgramValid = true;

    // Channel chrome.
    if (m_heroChannelName)
        setLabelText(m_heroChannelName, channel.callSign.empty() ? channel.title : channel.callSign);
    if (m_heroChannelId) {
        std::string chId = !channel.channelIdentifier.empty()
            ? "CH " + channel.channelIdentifier
            : (channel.channelNumber > 0 ? "CH " + std::to_string(channel.channelNumber) : "");
        setLabelText(m_heroChannelId, chId);
    }

    // Title is single-line. Trim very long titles with an ellipsis so
    // they don't get cut off mid-word at the cell edge. ~46 chars is
    // about what fits at 22px on Vita's hero info column.
    if (m_heroTitleLabel) {
        std::string title = program.title;
        const size_t maxTitleChars = 46;
        if (title.length() > maxTitleChars)
            title = title.substr(0, maxTitleChars - 1) + "…";
        setLabelText(m_heroTitleLabel, title);
    }
    if (m_heroSummaryLabel)
        setLabelText(m_heroSummaryLabel, program.summary.empty() ? std::string(" ") : program.summary);
    setLabelText(m_heroStartLabel, formatTime(program.startTime));
    setLabelText(m_heroEndLabel, formatTime(program.endTime));

    // Progress + LIVE badge are only meaningful when the program is
    // currently airing; future / past programs show 0% and no badge.
    time_t now = time(nullptr);
    bool isLive = (program.startTime <= (int64_t)now && program.endTime > (int64_t)now);

    int pct = 0;
    if (isLive && program.endTime > program.startTime) {
        int64_t elapsed = std::max<int64_t>(0, (int64_t)now - program.startTime);
        int64_t dur     = program.endTime - program.startTime;
        pct = (int)std::min<int64_t>(100, (elapsed * 100) / dur);
    }
    setLabelText(m_heroPctLabel, std::to_string(pct) + "%");
    if (m_heroProgressFill && m_heroProgressTrack) {
        float trackW = m_heroProgressTrack->getWidth();
        if (trackW <= 0) trackW = 200;  // pre-layout fallback
        m_heroProgressFill->setWidth(trackW * (pct / 100.0f));
    }
    if (m_heroLiveBadge)
        m_heroLiveBadge->setVisibility(isLive ? brls::Visibility::VISIBLE
                                              : brls::Visibility::INVISIBLE);

    // Thumbnail: prefer the program's own artwork; fall back to channel
    // logo if EPG didn't carry program art for this entry.
    if (m_heroThumbAlive) m_heroThumbAlive->store(false);
    m_heroThumbAlive = std::make_shared<std::atomic<bool>>(true);
    if (m_heroThumb) m_heroThumb->setVisibility(brls::Visibility::INVISIBLE);

    // Reset to the default width while the new image loads.
    if (m_heroThumbHolder) m_heroThumbHolder->setWidth(heroThumbWidth());
    if (m_heroThumb)       m_heroThumb->setWidth(heroThumbWidth());

    std::string thumbSrc = !program.thumb.empty() ? program.thumb : channel.thumb;
    if (!thumbSrc.empty()) {
        PlexClient& client = PlexClient::getInstance();
        std::string url = client.getThumbnailUrl(thumbSrc, heroThumbWidth(), heroHeight() - 16);
        ImageLoader::loadAsync(url, [this](brls::Image* img) {
            if (!img) return;
            img->setVisibility(brls::Visibility::VISIBLE);
            resizeHeroThumbToImage(img);
        }, m_heroThumb, m_heroThumbAlive);
    }
}

void LiveTVTab::refreshCurrentPrograms() {
    // Lightweight refresh: fetch EPG data and just update the hero's
    // "now playing" info without rebuilding the entire UI.
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<LiveTVChannel> freshChannels;
        bool success = client.fetchEPGGrid(freshChannels, m_hoursToShow);

        if (success) {
            brls::sync([this, freshChannels, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_lastRefreshTime = time(nullptr);

                for (size_t i = 0; i < m_channels.size() && i < freshChannels.size(); i++) {
                    for (const auto& freshCh : freshChannels) {
                        if (freshCh.channelIdentifier == m_channels[i].channelIdentifier ||
                            freshCh.channelNumber == m_channels[i].channelNumber) {
                            m_channels[i].currentProgram = freshCh.currentProgram;
                            m_channels[i].programs = freshCh.programs;
                            m_channels[i].programStart = freshCh.programStart;
                            m_channels[i].programEnd = freshCh.programEnd;
                            break;
                        }
                    }
                }

                if (!m_channels.empty()) updateHeroForChannel(m_channels.front());
            });
        }
    });
}

void LiveTVTab::loadChannels() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LiveTVTab: Fetching EPG data (async)...");
        PlexClient& client = PlexClient::getInstance();

        std::vector<LiveTVChannel> channels;
        const int64_t profFetch0 = brls::getCPUTimeUsec();
        bool success = client.fetchEPGGrid(channels, m_hoursToShow);
        brls::Logger::info("LTVPROF fetch thread: fetchEPGGrid -> {} channels in {}ms",
                           channels.size(),
                           (brls::getCPUTimeUsec() - profFetch0) / 1000);

        if (success) {
            brls::Logger::info("LiveTVTab: Got {} channels with EPG", channels.size());

            brls::sync([this, channels, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                const int64_t profCopy0 = brls::getCPUTimeUsec();
                m_channels = channels;
                const int64_t profUi0 = brls::getCPUTimeUsec();

                if (!m_channels.empty()) {
                    updateHeroForChannel(m_channels.front());
                }
                const int64_t profUi1 = brls::getCPUTimeUsec();

                buildEPGGrid();
                brls::Logger::info(
                    "LTVPROF UI apply: channel copy={}ms initial hero={}ms buildEPGGrid={}ms",
                    (profUi0 - profCopy0) / 1000,
                    (profUi1 - profUi0) / 1000,
                    (brls::getCPUTimeUsec() - profUi1) / 1000);

                m_loaded = true;
                m_lastFullLoadTime = time(nullptr);
                m_lastRefreshTime = m_lastFullLoadTime;
            });
        } else {
            brls::Logger::error("LiveTVTab: Failed to fetch EPG data");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });

    loadRecordings();
}

void LiveTVTab::updateCurrentTimeLine() {
    if (!m_currentTimeLine || !m_guideContainer || m_guideStartTime == 0) return;

    // Called every frame from draw() — but the wall clock only ticks
    // once per second, and at typical slot widths the line moves under
    // one pixel per second. Throttle the actual recompute to one per
    // second; everything else this would do is a no-op anyway.
    int64_t now = (int64_t)time(nullptr);
    if (now == m_lastTimeLineUpdateSec) return;
    m_lastTimeLineUpdateSec = now;

    int gridHours = m_hoursToShow;
    int64_t guideEnd = m_guideStartTime + (gridHours * 3600);

    if (now < m_guideStartTime || now > guideEnd) {
        if (m_currentTimeLine->getVisibility() != brls::Visibility::INVISIBLE)
            m_currentTimeLine->setVisibility(brls::Visibility::INVISIBLE);
        return;
    }

    float xOffset = (float)((now - m_guideStartTime) * livetvTimeSlotWidth() / 1800.0);
    float height = m_guideContainer->getHeight();
    if (height <= 1) {
        // pre-layout — try again next second
        m_lastTimeLineUpdateSec = 0;
        return;
    }

    // Anchor the line's yoga position once (at the program-area origin),
    // then ride setTranslationX for the per-second movement: a plain float
    // store with no invalidate. setPositionLeft every second re-ran Yoga
    // layout over the whole grid — a once-a-second frame hitch while just
    // *sitting* on the tab.
    if (!m_timeLineBasePlaced) {
        m_currentTimeLine->setPositionLeft((float)livetvChannelColWidth());
        m_timeLineBasePlaced = true;
    }
    m_currentTimeLine->setTranslationX(xOffset);
    if (height != m_lastTimeLineHeight) {
        m_currentTimeLine->setHeight(height);
        m_lastTimeLineHeight = height;
    }
    if (m_currentTimeLine->getVisibility() != brls::Visibility::VISIBLE)
        m_currentTimeLine->setVisibility(brls::Visibility::VISIBLE);
}

void LiveTVTab::buildEPGGrid() {
    const int64_t profG0 = brls::getCPUTimeUsec();
    int64_t profClear  = profG0;   // after old-view teardown
    int64_t profHeader = profG0;   // after time-header slot build
    s_profLogoQueued.store(0);
    s_profLogoDone.store(0);
    s_profLogoLatencyUs.store(0);
    // Rotate the logo generation: in-flight loads from the previous grid
    // captured the old flag and bail instead of touching freed Images.
    if (m_logoAlive) m_logoAlive->store(false);
    m_logoAlive = std::make_shared<std::atomic<bool>>(true);
    m_rowLogos.clear();
    s_profFmtUs.store(0);
    s_profFmtCalls.store(0);
    // Row-loop phase accumulators: channel column (incl. logo request),
    // program cells, and the per-row attach (setContentView + addView,
    // i.e. the yoga work). Whichever dominates is the real freeze cause.
    int64_t profColUs = 0, profCellsUs = 0, profAttachUs = 0;

    m_timeHeaderBox->clearViews();

    // Build the new rows into an ORPHAN container and swap it into the
    // scroll frame once at the end. Adding rows to the live guide box
    // re-ran layout over the whole growing guide subtree on every single
    // addView (amplified by each row's HScrollingFrame) — LTVPROF measured
    // a 9.0-SECOND UI freeze for 32 rows / 503 cells. An orphan box has no
    // attached scroll-frame cascade, so the same build costs milliseconds;
    // setContentView() then pays ONE full layout and deletes the old tree.
    GuideBox* newGuideBox = new GuideBox();
    newGuideBox->setAxis(brls::Axis::COLUMN);
    newGuideBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    newGuideBox->setAlignItems(brls::AlignItems::STRETCH);

    // Per-row HScrollingFrame pointers are owned by their rowBoxes (which
    // the swap below deletes with the old guide box), so the pointers in
    // this vector are about to dangle — purge them before rebuilding.
    m_rowProgramScrolls.clear();
    m_rowProgramBoxes.clear();
    m_lastAnchorScroll = nullptr;
    // Cell info entries reference Boxes owned by the row hierarchy we
    // just wiped via m_guideBox->clearViews(); purge before rebuilding
    // so draw()'s batch pass doesn't dereference freed cells.
    m_epgCells.clear();
    m_epgRowRanges.clear();
    // Reset the per-frame sync / time-line caches — the grid start time
    // shifts on rebuild and the row scroll frames have been recreated.
    m_lastSyncedScrollX     = -1;
    m_lastTimeLineUpdateSec = 0;
    m_lastTimeLineHeight    = -1;

    profClear = brls::getCPUTimeUsec();

    if (m_channels.empty()) {
        auto* noDataLabel = new brls::Label();
        noDataLabel->setText("No program guide data available");
        noDataLabel->setFontSize(14);
        noDataLabel->setTextColor(tok::muted());
        noDataLabel->setMarginLeft(12);
        noDataLabel->setMarginTop(12);
        newGuideBox->addView(noDataLabel);
        swapInGuideBox(newGuideBox);
        return;
    }

    // Set guide start time to current time rounded down to 30 minutes.
    time_t now = time(nullptr);
    m_guideStartTime = now - (now % 1800);

    int gridHours = m_hoursToShow;
    int totalSlots = gridHours * 2;

    // Time header — each 30-min slot. Bold muted labels, left hairline
    // separating slots.
    for (int i = 0; i < totalSlots; i++) {
        int64_t slotTime = m_guideStartTime + (i * 1800);

        auto* timeSlot = new brls::Box();
        timeSlot->setWidth(livetvTimeSlotWidth());
        timeSlot->setHeight(TIME_HEADER_HEIGHT);
        timeSlot->setJustifyContent(brls::JustifyContent::CENTER);
        timeSlot->setAlignItems(brls::AlignItems::CENTER);
        if (i > 0) {
            timeSlot->setBorderColor(tok::hairline());
            timeSlot->setBorderThickness(0);  // border drawn via line below
            // borealis Box draws a uniform border; we want only a left
            // hairline. Skip the border and draw a thin vertical strip
            // instead so the visual stays clean.
            auto* leftRule = new brls::Box();
            leftRule->setPositionType(brls::PositionType::ABSOLUTE);
            leftRule->setWidth(1);
            leftRule->setHeight(TIME_HEADER_HEIGHT - 12);
            leftRule->setPositionLeft(0);
            leftRule->setPositionTop(6);
            leftRule->setBackgroundColor(tok::hairline());
            timeSlot->addView(leftRule);
        }

        auto* timeLabel = new brls::Label();
        timeLabel->setText(formatTime(slotTime));
        timeLabel->setFontSize(13);
        timeLabel->setTextColor(tok::muted());
        timeSlot->addView(timeLabel);

        m_timeHeaderBox->addView(timeSlot);
    }
    profHeader = brls::getCPUTimeUsec();

    // Build channel rows
    for (const auto& channel : m_channels) {
        auto* rowBox = new brls::Box();
        const size_t rowCellsBegin = m_epgCells.size();  // cells appended below
        const int64_t profRow0 = brls::getCPUTimeUsec();
                                                         // belong to this row
        rowBox->setAxis(brls::Axis::ROW);
        rowBox->setHeight(livetvRowHeight());
        rowBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        rowBox->setAlignItems(brls::AlignItems::CENTER);

        // Sticky channel column — wide 16:9 logo on top, channel number
        // underneath. No call sign so a user scrolling 100+ channels
        // scans by logo instead of reading.
        auto* channelCol = new brls::Box();
        channelCol->setAxis(brls::Axis::COLUMN);
        channelCol->setWidth(livetvChannelColWidth());
        channelCol->setHeight(livetvRowHeight());
        channelCol->setPadding(4);
        channelCol->setBackgroundColor(nvgRGBA(0, 0, 0, 31));  // ~12% black overlay
        channelCol->setJustifyContent(brls::JustifyContent::CENTER);
        channelCol->setAlignItems(brls::AlignItems::CENTER);
        // Right hairline separating the column from the program track.
        auto* colRule = new brls::Box();
        colRule->setPositionType(brls::PositionType::ABSOLUTE);
        colRule->setWidth(1);
        colRule->setHeight(livetvRowHeight() - 6);
        colRule->setPositionLeft(livetvChannelColWidth() - 1);
        colRule->setPositionTop(3);
        colRule->setBackgroundColor(tok::hairline());
        channelCol->addView(colRule);

        const int logoW = gridChannelLogoWidth();
        const int logoH = gridChannelLogoHeight();
        auto* logoBox = new brls::Box();
        logoBox->setWidth(logoW);
        logoBox->setHeight(logoH);
        logoBox->setCornerRadius(6);
        logoBox->setBackgroundColor(tok::placeholder());
        logoBox->setMarginBottom(2);

        auto* logo = new brls::Image();
        logo->setWidth(logoW);
        logo->setHeight(logoH);
        logo->setScalingType(brls::ImageScalingType::FIT);
        logo->setCornerRadius(6);
        logo->setVisibility(brls::Visibility::INVISIBLE);
        logoBox->addView(logo);
        channelCol->addView(logoBox);

        if (!channel.thumb.empty()) {
            PlexClient& client = PlexClient::getInstance();
            std::string url = client.getThumbnailUrl(channel.thumb,
                                                     logoW * 2, logoH * 2);
            auto alive = std::make_shared<std::atomic<bool>>(true);
            s_profLogoQueued.fetch_add(1);
            const int64_t profQ0 = brls::getCPUTimeUsec();
            ImageLoader::loadAsync(url, [profQ0](brls::Image* img) {
                if (img) img->setVisibility(brls::Visibility::VISIBLE);
                // Aggregate queue->visible latency; one log line per 10
                // completions so ~100 channels don't spam the file.
                const int done = s_profLogoDone.fetch_add(1) + 1;
                s_profLogoLatencyUs.fetch_add(brls::getCPUTimeUsec() - profQ0);
                const int queued = s_profLogoQueued.load();
                if (done % 10 == 0 || done == queued)
                    brls::Logger::info("LTVPROF logos: {}/{} loaded, avg {}ms each",
                                       done, queued,
                                       s_profLogoLatencyUs.load() / done / 1000);
            }, logo, alive);
        }

        auto* chNumLabel = new brls::Label();
        chNumLabel->setText(!channel.channelIdentifier.empty()
                            ? channel.channelIdentifier
                            : std::to_string(channel.channelNumber));
        chNumLabel->setFontSize(11);
        // Channel number is body text in a long vertical list — keep
        // it muted so the gold accent stays reserved for fills,
        // selected states, and key hero numbers rather than shimmering
        // down every row of the channel sidebar.
        chNumLabel->setTextColor(tok::muted());
        chNumLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        channelCol->addView(chNumLabel);

        // ONE channel copy per row, shared by every lambda in the row.
        // Capturing LiveTVChannel by value in each cell's click + hover
        // lambdas deep-copied the channel AND its ~40-program vector
        // (hundreds of string allocs) twice per cell — LTVPROF measured
        // cells=7368ms of the guide-build freeze from exactly this.
        auto capturedChannel = std::make_shared<const LiveTVChannel>(channel);
        channelCol->setFocusable(true);
        channelCol->registerClickAction([this, capturedChannel](brls::View*) {
            onChannelSelected(*capturedChannel);
            return true;
        });
        channelCol->addGestureRecognizer(new brls::TapGestureRecognizer(channelCol));
        // Hover on the channel column shows the channel's current show
        // in the hero — keeps the preview in sync as the user scrolls
        // through the channel list with the dpad.
        channelCol->getFocusEvent()->subscribe(
            [this, capturedChannel](brls::View*) {
                queueHeroForChannel(*capturedChannel);
            });

        rowBox->addView(channelCol);
        const int64_t profRow1 = brls::getCPUTimeUsec();
        profColUs += profRow1 - profRow0;

        // Program cells live inside their own HScrollingFrame so RIGHT
        // arrow can pan past the visible width and bring later shows
        // into view. CENTERED behaviour keeps the focused cell visible
        // and the per-row scroll positions are synced together (and to
        // the time header) in draw().
        //
        // The explicit height matters: rowBox's alignItems=CENTER won't
        // stretch the scroll frame to fill the row's cross axis, and
        // HScrollingFrame::setContentView wires contentView.height to
        // self.height — without setHeight here the scroll collapses
        // to ~0 high, the contentView follows, and the cells inside
        // end up with no layout slot for their labels (text invisible,
        // cell margins gone, focus highlight floats half a row down).
        auto* programsScroll = new brls::HScrollingFrame();
        programsScroll->setGrow(1.0f);
        programsScroll->setHeight(livetvRowHeight());
        programsScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        programsScroll->setFocusable(false);

        auto* programsBox = new brls::Box();
        programsBox->setAxis(brls::Axis::ROW);
        programsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        programsBox->setAlignItems(brls::AlignItems::CENTER);

        if (!channel.programs.empty()) {
            int64_t guideEndTime = m_guideStartTime + (gridHours * 3600);
            int64_t lastEndTime = m_guideStartTime;

            for (size_t pi = 0; pi < channel.programs.size(); pi++) {
                const auto& prog = channel.programs[pi];

                if (prog.endTime <= m_guideStartTime || prog.startTime >= guideEndTime) continue;

                int64_t visStart = std::max(prog.startTime, m_guideStartTime);
                int64_t visEnd = std::min(prog.endTime > 0 ? prog.endTime : visStart + 1800, guideEndTime);

                if (visStart > lastEndTime) {
                    int gapPixels = (int)((visStart - lastEndTime) * livetvTimeSlotWidth() / 1800);
                    if (gapPixels > 0) {
                        auto* spacer = new brls::Box();
                        spacer->setWidth(gapPixels);
                        spacer->setHeight(livetvRowHeight() - 4);
                        programsBox->addView(spacer);
                    }
                }

                int64_t durationSec = visEnd - visStart;
                int cellWidth = (int)(durationSec * livetvTimeSlotWidth() / 1800);
                if (cellWidth < 40) cellWidth = 40;

                time_t nowSec = time(nullptr);
                bool isCurrently = (prog.startTime <= (int64_t)nowSec && prog.endTime > (int64_t)nowSec);

                auto* progCell = new brls::Box();
                progCell->setWidth(cellWidth);
                progCell->setHeight(livetvRowHeight() - 8);
                progCell->setMargins(2, 2, 2, 2);
                // No per-cell background/border: each one is a separate
                // stenciled antialiased NanoVG path — with ~50 cells visible
                // the GPU sat at 100% (CPU half idle). draw() paints all
                // visible cell rects as a few merged batched paths instead.
                progCell->setCornerRadius(6);
                progCell->setFocusable(true);
                progCell->setHideHighlightBackground(true);  // ring comes from the
                                                             // app highlight pass

                // Cell is intentionally label-less — the title and
                // time-range strings are batched in draw() through the
                // patched nvgTextBatchBegin/End API so all visible cell
                // text flushes as one render call per style instead of
                // one per Label.
                EpgCellInfo info;
                info.cell = progCell;
                info.scroll = programsScroll;
                info.row = rowBox;
                info.onNow = isCurrently;
                std::string title = prog.title;
                int maxChars = cellWidth / 8;
                if (maxChars < 4) maxChars = 4;
                if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
                info.title = std::move(title);
                std::string sub = formatTime(prog.startTime) + " – " + formatTime(prog.endTime);
                if (isCurrently) sub += "  ·  on now";
                info.subtitle = std::move(sub);
                m_epgCells.push_back(info);

                // One shared copy of the program per cell — capturing
                // GuideProgram by value in the click AND hover lambdas
                // duplicated its strings (summary is a whole paragraph)
                // twice more per cell.
                auto gp = std::make_shared<const GuideProgram>();
                {
                    auto* g = const_cast<GuideProgram*>(gp.get());
                    g->title = prog.title;
                    g->summary = prog.summary;
                    g->startTime = prog.startTime;
                    g->endTime = prog.endTime;
                    g->ratingKey = prog.ratingKey;
                    g->metadataKey = prog.metadataKey;
                    g->thumb = prog.thumb;
                }

                progCell->registerClickAction([this, gp, capturedChannel](brls::View*) {
                    onProgramSelected(*gp, *capturedChannel);
                    return true;
                });
                progCell->addGestureRecognizer(new brls::TapGestureRecognizer(progCell));
                // Hover → live-update the hero with this program's
                // details (title, summary, thumb, progress, channel
                // chrome). Watch live + Record buttons follow because
                // they read from m_heroChannel / m_heroProgram.
                progCell->getFocusEvent()->subscribe(
                    [this, gp, capturedChannel](brls::View*) {
                        queueHeroForProgram(*capturedChannel, *gp);
                    });

                programsBox->addView(progCell);
                lastEndTime = visEnd;
            }
        } else if (!channel.currentProgram.empty() && channel.programStart > 0) {
            int64_t progStart = std::max(channel.programStart, m_guideStartTime);
            int64_t guideEndTime = m_guideStartTime + (gridHours * 3600);
            int64_t progEnd = std::min(channel.programEnd > 0 ? channel.programEnd : progStart + 1800, guideEndTime);

            int startOffset = (int)((progStart - m_guideStartTime) * livetvTimeSlotWidth() / 1800);
            int cellWidth = (int)((progEnd - progStart) * livetvTimeSlotWidth() / 1800);
            if (cellWidth < 40) cellWidth = 40;

            if (startOffset > 0) {
                auto* spacer = new brls::Box();
                spacer->setWidth(startOffset);
                spacer->setHeight(livetvRowHeight() - 4);
                programsBox->addView(spacer);
            }

            auto* progCell = new brls::Box();
            progCell->setWidth(cellWidth);
            progCell->setHeight(livetvRowHeight() - 8);
            progCell->setMargins(2, 2, 2, 2);
            // Background/border painted batched in draw() (see main loop).
            progCell->setCornerRadius(6);
            progCell->setFocusable(true);
            progCell->setHideHighlightBackground(true);

            // Label-less cell — text painted in batch by draw() via
            // nvgTextBatchBegin/End. See the main loop above.
            EpgCellInfo info;
            info.cell = progCell;
            info.scroll = programsScroll;
            info.row = rowBox;
            info.onNow = true;
            std::string title = channel.currentProgram;
            int maxChars = cellWidth / 8;
            if (maxChars < 4) maxChars = 4;
            if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
            info.title = std::move(title);
            info.subtitle = formatTime(channel.programStart) + " – " + formatTime(channel.programEnd) + "  ·  on now";
            m_epgCells.push_back(info);

            // Hover on the legacy fallback updates the hero with this
            // channel's "now playing" — we don't have a full program
            // struct here, so synthesize one from the channel fields.
            GuideProgram legacyGp;
            legacyGp.title     = channel.currentProgram;
            legacyGp.startTime = channel.programStart;
            legacyGp.endTime   = channel.programEnd > 0
                                  ? channel.programEnd
                                  : channel.programStart + 1800;
            progCell->getFocusEvent()->subscribe(
                [this, legacyGp, capturedChannel](brls::View*) {
                    queueHeroForProgram(*capturedChannel, legacyGp);
                });

            programsBox->addView(progCell);
        } else {
            auto* emptyCell = new brls::Box();
            emptyCell->setWidth(livetvTimeSlotWidth() * 2);
            emptyCell->setHeight(livetvRowHeight() - 6);
            emptyCell->setMargins(3, 3, 3, 3);
            emptyCell->setBackgroundColor(tok::cardRaised());
            emptyCell->setCornerRadius(7);
            emptyCell->setFocusable(true);
            emptyCell->setPadding(8);

            auto* noInfo = new brls::Label();
            noInfo->setText("No guide data");
            noInfo->setFontSize(11);
            noInfo->setTextColor(tok::dim());
            emptyCell->addView(noInfo);

            emptyCell->registerClickAction([this, capturedChannel](brls::View*) {
                onChannelSelected(*capturedChannel);
                return true;
            });
            emptyCell->addGestureRecognizer(new brls::TapGestureRecognizer(emptyCell));
            // Hover on a no-data cell still updates the channel chrome
            // on the hero so the user knows which channel they'd tune.
            emptyCell->getFocusEvent()->subscribe(
                [this, capturedChannel](brls::View*) {
                    queueHeroForChannel(*capturedChannel);
                });

            programsBox->addView(emptyCell);
        }

        const int64_t profRow2 = brls::getCPUTimeUsec();
        profCellsUs += profRow2 - profRow1;
        programsScroll->setContentView(programsBox);
        rowBox->addView(programsScroll);
        m_rowProgramScrolls.push_back(programsScroll);
        m_rowProgramBoxes.push_back(programsBox);
        if (m_epgCells.size() > rowCellsBegin)
            m_epgRowRanges.push_back({ rowBox, rowCellsBegin, m_epgCells.size() });
        newGuideBox->addView(rowBox);
        profAttachUs += brls::getCPUTimeUsec() - profRow2;
    }

    // Attach the finished rows — one relayout, old tree freed.
    const int64_t profSwap0 = brls::getCPUTimeUsec();
    swapInGuideBox(newGuideBox);

    // Update the cyan time-line position now that the grid exists; the
    // draw() override keeps it tracking the wall clock thereafter.
    updateCurrentTimeLine();

    s_profFirstDrawPending = true;
    brls::Logger::info(
        "LTVPROF buildEPGGrid: total={}ms (teardown={}ms header={}ms rows={}ms swap={}ms) "
        "rows={} cells={} logosQueued={}",
        (brls::getCPUTimeUsec() - profG0) / 1000,
        (profClear - profG0) / 1000,
        (profHeader - profClear) / 1000,
        (profSwap0 - profHeader) / 1000,
        (brls::getCPUTimeUsec() - profSwap0) / 1000,
        (int)m_rowProgramScrolls.size(), (int)m_epgCells.size(),
        s_profLogoQueued.load());
    brls::Logger::info(
        "LTVPROF buildEPGGrid rows split: channelCol={}ms cells={}ms attach={}ms | "
        "formatTime={}ms over {} calls",
        profColUs / 1000, profCellsUs / 1000, profAttachUs / 1000,
        s_profFmtUs.load() / 1000, s_profFmtCalls.load());
}

void LiveTVTab::swapInGuideBox(brls::Box* newGuideBox) {
    // If focus currently sits inside the old guide tree, park it on the
    // hero first — setContentView() deletes the old rows, and a dangling
    // Application::currentFocus would crash the next input event.
    brls::View* fc = brls::Application::getCurrentFocus();
    if (fc && m_guideBox && isDescendantOf(fc, m_guideBox)) {
        if (brls::View* heroTarget = findFirstFocusableInBox(m_heroBox))
            brls::Application::giveFocus(heroTarget);
    }
    if (m_guideScrollV) m_guideScrollV->setLastFocusedView(nullptr);
    m_guideScrollV->setContentView(newGuideBox);  // deletes the old box
    m_guideBox = newGuideBox;
}

void LiveTVTab::loadGuide() {
    // Already handled in loadChannels with fetchEPGGrid
}

void LiveTVTab::loadRecordings() {
    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LiveTVTab: Fetching DVR recordings...");

        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        std::string subsUrl = client.buildApiUrlPublic("/media/subscriptions?includeGrabs=1");
        HttpRequest req;
        req.url = subsUrl;
        req.method = "GET";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        const int64_t profR0 = brls::getCPUTimeUsec();
        HttpResponse resp = httpClient.request(req);
        brls::Logger::info("LTVPROF DVR subscriptions fetch: {}ms ({} bytes)",
                           (brls::getCPUTimeUsec() - profR0) / 1000,
                           (int)resp.body.size());

        std::vector<DVRRecording> recordings;

        if (resp.statusCode == 200 && !resp.body.empty()) {
            brls::Logger::debug("LiveTVTab: DVR subscriptions response ({} bytes)", resp.body.length());

            size_t msPos = resp.body.find("\"MediaSubscription\"");
            if (msPos != std::string::npos) {
                size_t arrStart = resp.body.find('[', msPos);
                if (arrStart != std::string::npos) {
                    size_t pos = arrStart + 1;
                    while (pos < resp.body.length()) {
                        size_t objStart = resp.body.find('{', pos);
                        if (objStart == std::string::npos) break;

                        std::string between = resp.body.substr(pos, objStart - pos);
                        if (between.find(']') != std::string::npos) break;

                        int braceCount = 1;
                        size_t objEnd = objStart + 1;
                        while (braceCount > 0 && objEnd < resp.body.length()) {
                            if (resp.body[objEnd] == '{') braceCount++;
                            else if (resp.body[objEnd] == '}') braceCount--;
                            objEnd++;
                        }
                        std::string obj = resp.body.substr(objStart, objEnd - objStart);

                        std::string title = client.extractJsonValuePublic(obj, "title");
                        std::string key = client.extractJsonValuePublic(obj, "key");

                        if (!title.empty() && !key.empty()) {
                            DVRRecording rec;
                            rec.title = title;
                            rec.ratingKey = key;
                            rec.mediaSubscriptionId = key;

                            std::string beginsAt = client.extractJsonValuePublic(obj, "beginsAt");
                            if (!beginsAt.empty()) rec.scheduledTime = atoll(beginsAt.c_str());

                            std::string status = client.extractJsonValuePublic(obj, "status");
                            if (status == "2" || status == "recording") rec.status = "recording";
                            else rec.status = "scheduled";

                            recordings.push_back(rec);
                        }

                        pos = objEnd;
                    }
                }
            }
        }

        brls::Logger::info("LiveTVTab: Found {} DVR recordings/subscriptions", recordings.size());

        // Data-only refresh — the DVR strip is no longer rendered, but
        // the fetch still keeps m_recordings consistent so other paths
        // (cancelRecording, future re-introduction of the row) work.
        brls::sync([this, recordings, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            m_recordings = recordings;
        });
    });
}

void LiveTVTab::onChannelSelected(const LiveTVChannel& channel) {
    brls::Logger::info("LiveTVTab: Selected channel: {} ({})", channel.title, channel.channelNumber);

    std::string tuneChannel = channel.key;
    if (tuneChannel.empty()) tuneChannel = channel.channelIdentifier;
    if (tuneChannel.empty()) tuneChannel = std::to_string(channel.channelNumber);

    std::string programMetadataKey;
    time_t now = time(nullptr);
    for (const auto& prog : channel.programs) {
        if (prog.startTime <= (int64_t)now && prog.endTime > (int64_t)now && !prog.metadataKey.empty()) {
            programMetadataKey = prog.metadataKey;
            brls::Logger::info("LiveTVTab: Current program metadata key: {}", programMetadataKey);
            break;
        }
    }

    asyncRun([this, channel, tuneChannel, programMetadataKey, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::string streamUrl;
        std::string liveSessionUuid;

        if (client.tuneLiveTVChannel(tuneChannel, streamUrl, liveSessionUuid, programMetadataKey)) {
            brls::Logger::info("LiveTVTab: Got stream URL for channel {}", channel.title);
            brls::sync([streamUrl, liveSessionUuid, channel]() {
                std::string title = channel.title;
                if (!channel.currentProgram.empty()) title += " - " + channel.currentProgram;
                Application::getInstance().pushLiveTVPlayerActivity(streamUrl, title, liveSessionUuid);
            });
        } else {
            brls::Logger::error("LiveTVTab: Failed to tune channel {}", channel.title);
            brls::sync([channel]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to tune channel: " + channel.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
        }
    });
}

void LiveTVTab::onProgramSelected(const GuideProgram& program, const LiveTVChannel& channel) {
    std::string message = program.title;
    if (program.startTime > 0 && program.endTime > 0) {
        message += "\n\n" + formatTime(program.startTime) + " - " + formatTime(program.endTime);
    }
    if (!program.summary.empty()) message += "\n\n" + program.summary;

    brls::Dialog* dialog = new brls::Dialog(message);
    dialog->addButton("Watch Now", [this, channel]() { onChannelSelected(channel); });
    dialog->addButton("Record", [this, program, channel]() { scheduleRecording(program, channel); });
    dialog->addButton("Cancel", []() {});
    dialog->open();
}

void LiveTVTab::scheduleRecording(const GuideProgram& program, const LiveTVChannel& channel) {
    (void)channel;

    if (program.ratingKey.empty()) {
        brls::Logger::error("LiveTVTab: scheduleRecording: missing program ratingKey");
        brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
        dialog->addButton("OK", []() {});
        dialog->open();
        return;
    }

    asyncRun([this, program, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // GET /media/subscriptions/template?guid=<programGuid> returns the
        // pre-encoded querystring (hints[*] and params[*]) the server
        // expects, plus the recommended type and target library section.
        // We paste it verbatim and only layer recording prefs on top.
        std::string tmplUrl = client.buildApiUrlPublic(
            "/media/subscriptions/template?guid=" + program.ratingKey);

        HttpRequest tmplReq;
        tmplReq.url = tmplUrl;
        tmplReq.method = "GET";
        tmplReq.headers["Accept"] = "application/json";
        tmplReq.timeout = 15;

        brls::Logger::debug("LiveTVTab: Recording template URL: {}", tmplUrl);
        HttpResponse tmplResp = httpClient.request(tmplReq);
        if (tmplResp.statusCode != 200 || tmplResp.body.empty()) {
            brls::Logger::error("LiveTVTab: subscription template failed ({}): {}",
                                tmplResp.statusCode,
                                tmplResp.body.empty() ? "(empty)" : tmplResp.body.substr(0, 300));
            brls::sync([program]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
            return;
        }

        const std::string& body = tmplResp.body;
        size_t pickAt = std::string::npos;
        {
            size_t scan = 0;
            const std::string sel = "\"selected\":true";
            while (true) {
                size_t at = body.find(sel, scan);
                if (at == std::string::npos) break;
                int depth = 0;
                for (size_t i = at; i > 0; i--) {
                    if (body[i] == '}') depth++;
                    else if (body[i] == '{') {
                        if (depth == 0) { pickAt = i; break; }
                        depth--;
                    }
                }
                if (pickAt != std::string::npos) break;
                scan = at + sel.length();
            }
        }
        if (pickAt == std::string::npos) {
            size_t msArr = body.find("\"MediaSubscription\"");
            if (msArr != std::string::npos) pickAt = body.find('{', msArr);
        }
        if (pickAt == std::string::npos) {
            brls::Logger::error("LiveTVTab: subscription template parse failed: {}",
                                body.substr(0, 300));
            brls::sync([program]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
            return;
        }

        size_t depth = 0;
        size_t objEnd = pickAt;
        for (; objEnd < body.length(); objEnd++) {
            if (body[objEnd] == '{') depth++;
            else if (body[objEnd] == '}') {
                if (--depth == 0) { objEnd++; break; }
            }
        }
        std::string ms = body.substr(pickAt, objEnd - pickAt);

        std::string parameters    = client.extractJsonValuePublic(ms, "parameters");
        std::string typeStr       = client.extractJsonValuePublic(ms, "type");
        std::string targetSection = client.extractJsonValuePublic(ms, "targetLibrarySectionID");

        // User-configured default DVR library wins over the template's
        // recommendation. Lets the user route every recording to one
        // section ("DVR TV Shows") instead of whatever Plex picked for
        // each individual program.
        const std::string& userTarget = Application::getInstance()
                                            .getSettings().defaultDvrSectionId;
        if (!userTarget.empty()) {
            brls::Logger::debug("LiveTVTab: overriding targetLibrarySectionID "
                                "{} → {} (user default)",
                                targetSection.empty() ? "(none)" : targetSection,
                                userTarget);
            targetSection = userTarget;
        }

        if (parameters.empty() || typeStr.empty()) {
            brls::Logger::error("LiveTVTab: template missing required fields (parameters={}, type={})",
                                parameters.empty() ? "(empty)" : "ok",
                                typeStr.empty() ? "(empty)" : typeStr);
            brls::sync([program]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + program.title);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
            return;
        }

        const AppSettings& settings = Application::getInstance().getSettings();

        std::string post = client.buildApiUrlPublic("/media/subscriptions");
        post += "&" + parameters;
        post += "&type=" + typeStr;
        if (!targetSection.empty()) post += "&targetLibrarySectionID=" + targetSection;
        post += "&includeGrabs=1";
        post += "&prefs[oneShot]=true";
        post += std::string("&prefs[recordPartials]=") + (settings.dvrRecordPartials ? "true" : "false");
        post += "&prefs[minVideoQuality]=" + std::to_string(settings.dvrMinVideoQuality);
        post += "&prefs[startOffsetMinutes]=" + std::to_string(settings.dvrStartOffsetMinutes);
        post += "&prefs[endOffsetMinutes]=" + std::to_string(settings.dvrEndOffsetMinutes);

        HttpRequest req;
        req.url = post;
        req.method = "POST";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        brls::Logger::debug("LiveTVTab: Recording POST URL: {}", post);
        HttpResponse resp = httpClient.request(req);

        bool success = (resp.statusCode == 200 || resp.statusCode == 201);
        std::string title = program.title;

        brls::Logger::debug("LiveTVTab: Recording response: {} ({} bytes): {}",
                            resp.statusCode, resp.body.length(),
                            resp.body.substr(0, 500));

        brls::sync([this, success, title, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            if (success) {
                brls::Dialog* dialog = new brls::Dialog("Recording scheduled: " + title);
                dialog->addButton("OK", []() {});
                dialog->open();
                loadRecordings();
            } else {
                brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + title);
                dialog->addButton("OK", []() {});
                dialog->open();
            }
        });
    });
}

void LiveTVTab::cancelRecording(const DVRRecording& recording) {
    asyncRun([this, recording, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // DELETE /media/subscriptions/{id}
        std::string delUrl = client.buildApiUrlPublic("/media/subscriptions/" + recording.mediaSubscriptionId);

        HttpRequest req;
        req.url = delUrl;
        req.method = "DELETE";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        HttpResponse resp = httpClient.request(req);
        bool success = (resp.statusCode == 200 || resp.statusCode == 204);

        brls::sync([this, success, recording, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            if (success) {
                brls::Dialog* dialog = new brls::Dialog("Cancelled recording: " + recording.title);
                dialog->addButton("OK", []() {});
                dialog->open();
                loadRecordings();
            } else {
                brls::Dialog* dialog = new brls::Dialog("Failed to cancel recording");
                dialog->addButton("OK", []() {});
                dialog->open();
            }
        });
    });
}

} // namespace vitaplex
