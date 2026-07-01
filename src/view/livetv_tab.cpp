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
#include <cmath>
#include <ctime>
#include <functional>

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

// EPG grid render window. Cells past the visible width are reachable via
// the dpad — EpgGridView pans its internal scroll offset as the virtual
// cursor moves. The render path uses whatever window
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

// ============================================================================
// EpgGridView
// ============================================================================
// The whole program guide — time header, channel column, program cells,
// now-line, focus ring — drawn by ONE view in a single draw() pass.
//
// Hardware profiling showed the previous implementation (a borealis view
// per row / cell: ~100 channels × ~25 views ≈ 2500 views + yoga nodes)
// cost ~24 ms/frame at idle with all content stripped — bare boxes alone
// held the guide at 42 FPS, and text/images added ~18 ms more (24 FPS
// full). The per-view frame()/yoga overhead was the structure tax; this
// class removes the structure entirely. There are no children: focus is a
// virtual (row, cell) cursor moved by hidden repeating dpad actions, and
// scrolling is two plain floats applied at draw time (no animation, no
// relayout — a scroll is a subtraction).
class EpgGridView : public brls::Box {
public:
    // One program cell, fully precomputed by LiveTVTab::buildEPGGrid so
    // draw() does zero string work per frame: x/w are pixel offsets into
    // the (unscrolled) program track, title/subtitle are pre-truncated /
    // pre-formatted.
    struct Cell {
        GuideProgram prog;
        float x = 0, w = 0;
        bool onNow = false;
        bool noData = false;   // placeholder cell — A tunes the channel
        std::string title, subtitle;
    };
    // One channel row. logoImg is a raw NVG handle loaded lazily the
    // first time the row scrolls into view (loadCoverAsync — same
    // lifetime pattern as MediaItemCell::loadThumbnail); logoW/logoH are
    // the decoded image's natural size for the letterbox-fit math.
    struct Row {
        LiveTVChannel channel;
        std::vector<Cell> cells;
        std::string chLabel;
        int logoImg = 0;
        int logoW = 0, logoH = 0;
        bool logoRequested = false;
    };

    // Wired by LiveTVTab after construction.
    std::function<void(const GuideProgram&, const LiveTVChannel&)> onCellSelected;
    std::function<void(const LiveTVChannel&)> onChannelSelected;
    std::function<void(const LiveTVChannel&, const GuideProgram&, bool hasProgram)> onHover;
    // LiveTVTab::formatTime is a member of the tab, so the grid can't
    // call it directly — the tab hands us a wrapping lambda instead.
    // Only the header slot labels format at draw time; cell subtitles
    // are pre-formatted in buildEPGGrid.
    std::function<std::string(int64_t)> formatTime;

    EpgGridView() {
        setFocusable(true);
        // We paint our own focus ring around the cursor cell; borealis'
        // highlight would wrap the entire grid instead.
        setHideHighlight(true);

        // Hidden, repeating dpad actions. Actions run BEFORE
        // Application::navigate, so returning true consumes the press —
        // the cursor moved internally and focus stays on the grid —
        // while returning false at a boundary lets normal borealis
        // navigation take over (UP at row 0 escapes to the hero, LEFT at
        // cell 0 escapes toward the sidebar).
        registerAction("", brls::ControllerButton::BUTTON_NAV_LEFT,
                       [this](brls::View*) { return this->moveCursor(-1, 0); },
                       true, true, brls::SOUND_FOCUS_CHANGE);
        registerAction("", brls::ControllerButton::BUTTON_NAV_RIGHT,
                       [this](brls::View*) { return this->moveCursor(1, 0); },
                       true, true, brls::SOUND_FOCUS_CHANGE);
        registerAction("", brls::ControllerButton::BUTTON_NAV_UP,
                       [this](brls::View*) { return this->moveCursor(0, -1); },
                       true, true, brls::SOUND_FOCUS_CHANGE);
        registerAction("", brls::ControllerButton::BUTTON_NAV_DOWN,
                       [this](brls::View*) { return this->moveCursor(0, 1); },
                       true, true, brls::SOUND_FOCUS_CHANGE);

        // A activates the cursor cell; a touch tap fires the same click
        // action (activates the cursor cell — fine for v1).
        registerClickAction([this](brls::View*) { return this->activateCursorCell(); });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    ~EpgGridView() override {
        // Flip the alive flag so in-flight logo loads are dropped by
        // ImageLoader (it deletes the decoded handle on our behalf),
        // then free the handles we already own.
        if (m_imgAlive) m_imgAlive->store(false);
        NVGcontext* vg = brls::Application::getNVGContext();
        for (Row& row : m_rows) {
            if (row.logoImg != 0 && vg) nvgDeleteImage(vg, row.logoImg);
        }
    }

    void setData(std::vector<Row> rows, int64_t guideStart, int hours) {
        // Invalidate the previous generation of logo loads: any callback
        // still in flight captured the OLD flag, so ImageLoader discards
        // its handle instead of writing into a row that no longer exists.
        if (m_imgAlive) m_imgAlive->store(false);
        m_imgAlive = std::make_shared<std::atomic<bool>>(true);

        // The old rows' logo handles would leak with the row structs —
        // free them before the swap (the destructor only sees the new set).
        NVGcontext* vg = brls::Application::getNVGContext();
        for (Row& row : m_rows) {
            if (row.logoImg != 0 && vg) nvgDeleteImage(vg, row.logoImg);
        }

        m_rows       = std::move(rows);
        m_guideStart = guideStart;
        m_hours      = hours;
        m_scrollX    = 0;
        m_scrollY    = 0;

        // Cursor to the first row that has cells (cell 0). No yoga /
        // view churn here — the grid has no children, so a full data
        // swap is O(data) with zero relayout.
        m_curRow  = 0;
        m_curCell = 0;
        for (size_t r = 0; r < m_rows.size(); r++) {
            if (!m_rows[r].cells.empty()) { m_curRow = (int)r; break; }
        }

        // An empty grid must not take focus: it draws no cursor ring and
        // suppresses the borealis highlight, so focus would just vanish.
        this->setFocusable(!m_rows.empty());
    }

    // The tab's willDisappear cancels all in-flight image loads
    // (ImageLoader::cancelAll bumps the loader generation), which strands
    // rows marked logoRequested with no texture — and nothing would retry
    // until the next full EPG reload (5 min). Called when the tab regains
    // focus so visible rows simply re-request on the next draw.
    void retryMissingLogos() {
        for (Row& row : m_rows)
            if (row.logoImg == 0) row.logoRequested = false;
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        // Fill the hero the moment focus enters the grid, not on the
        // first cursor move.
        fireHover();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        (void)style; (void)ctx;   // no children — Box::draw intentionally NOT called

        // Empty guide (fetch failed / not loaded yet): paint the message the
        // old label-based build showed instead of a silent blank card.
        if (m_rows.empty()) {
            nvgFontFace(vg, "regular");
            nvgFontSize(vg, 14);
            nvgFillColor(vg, tok::muted());
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgText(vg, x + 12.0f, y + 12.0f, "No program guide data available", nullptr);
            return;
        }

        const float headerH  = (float)TIME_HEADER_HEIGHT;
        const float rowH     = (float)livetvRowHeight();
        const float colW     = (float)livetvChannelColWidth();
        const float slotW    = (float)livetvTimeSlotWidth();
        const float pxPerSec = slotW / 1800.0f;

        // Program area / row area.
        const float px = x + colW, pw = width - colW;
        const float gy = y + headerH, gh = height - headerH;

        // Visible row window — everything below iterates only these.
        const int rows = (int)m_rows.size();
        const int r0 = std::max(0, (int)(m_scrollY / rowH));
        const int r1 = std::min(rows, (int)((m_scrollY + gh) / rowH) + 1);

        // ── 1) TIME HEADER ─────────────────────────────────────────────
        // Dim strip + batched slot labels. Scissored to the program strip
        // so a half-scrolled label can't bleed over the channel column.
        nvgSave(vg);
        nvgScissor(vg, px, y, pw, headerH);
        nvgBeginPath(vg);
        nvgRect(vg, px, y, pw, headerH);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 56));  // ~22% black overlay
        nvgFill(vg);
        if (m_guideStart > 0 && formatTime) {
            nvgFontFace(vg, "regular");
            nvgFontSize(vg, 13);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, tok::muted());
            // Only the on-screen slots format + draw (~6-8 of up to 96),
            // flushed as one render call via the patched batch API.
            nvgTextBatchBegin(vg);
            const int totalSlots = m_hours * 2;
            const int firstSlot  = std::max(0, (int)(m_scrollX / slotW));
            for (int i = firstSlot; i < totalSlots; i++) {
                const float sx = px + (float)i * slotW - m_scrollX + 8.0f;
                if (sx > x + width) break;
                std::string label = formatTime(m_guideStart + (int64_t)i * 1800);
                if (!label.empty())
                    nvgText(vg, sx, y + 10.0f, label.c_str(), nullptr);
            }
            nvgTextBatchEnd(vg);
        }
        nvgRestore(vg);

        // ── 2) PROGRAM AREA ────────────────────────────────────────────
        nvgSave(vg);
        nvgScissor(vg, px, gy, pw, gh);

        // Resolve the visible cells once; every pass below (fills, ring,
        // two text batches) walks this short list instead of re-testing
        // the whole grid.
        struct VisCell {
            float x, y, w, h;
            const Cell* cell;
        };
        std::vector<VisCell> vis;
        vis.reserve(128);
        for (int r = r0; r < r1; r++) {
            const Row& row  = m_rows[r];
            const float cy  = gy + (float)r * rowH - m_scrollY + 4.0f;
            const float chh = rowH - 8.0f;
            for (const Cell& c : row.cells) {
                const float cx = px + c.x - m_scrollX;
                if (cx + c.w < px || cx > px + pw) continue;
                vis.push_back({ cx, cy, c.w, chh, &c });
            }
        }

        int nowCells = 0, upcomingCells = 0;
        for (const VisCell& v : vis) (v.cell->onNow ? nowCells : upcomingCells)++;

        // Batched cell backgrounds: ONE merged path per colour + one
        // merged stroke pass, with shape AA off. Per-path stencil work is
        // the wall on the Vita's tile-based GPU — path count is what
        // matters, and AA fringes are the bulk of NanoVG's per-path
        // geometry (invisible on 6px-radius guide cells anyway).
        nvgShapeAntiAlias(vg, 0);
        if (upcomingCells > 0) {
            nvgBeginPath(vg);
            for (const VisCell& v : vis)
                if (!v.cell->onNow) nvgRoundedRect(vg, v.x, v.y, v.w, v.h, 6.0f);
            nvgFillColor(vg, tok::cellUpcoming());
            nvgFill(vg);
        }
        if (nowCells > 0) {
            nvgBeginPath(vg);
            for (const VisCell& v : vis)
                if (v.cell->onNow) nvgRoundedRect(vg, v.x, v.y, v.w, v.h, 6.0f);
            nvgFillColor(vg, tok::cellNow());
            nvgFill(vg);

            nvgBeginPath(vg);
            for (const VisCell& v : vis)
                if (v.cell->onNow) nvgRoundedRect(vg, v.x + 0.5f, v.y + 0.5f,
                                                  v.w - 1.0f, v.h - 1.0f, 6.0f);
            nvgStrokeColor(vg, tok::accent());
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
        nvgShapeAntiAlias(vg, 1);

        // Focus ring around the cursor cell (after the fills so it sits
        // on top; AA on — it's a single path, cost is irrelevant).
        if (this->isFocused() && cursorValid()) {
            const Cell& c  = m_rows[m_curRow].cells[m_curCell];
            const float fx = px + c.x - m_scrollX;
            const float fy = gy + (float)m_curRow * rowH - m_scrollY + 4.0f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, fx - 1.0f, fy - 1.0f, c.w + 2.0f, rowH - 8.0f + 2.0f, 6.0f);
            nvgStrokeColor(vg, tok::accent());
            nvgStrokeWidth(vg, 2.0f);
            nvgStroke(vg);
        }

        // Cell text: two batched passes (titles, then subtitles), one
        // render flush per style instead of one per label. Strings are
        // pre-truncated in buildEPGGrid — no per-frame allocation here.
        nvgFontFace(vg, "regular");
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

        nvgFontSize(vg, 13);
        nvgFillColor(vg, tok::text());
        nvgTextBatchBegin(vg);
        for (const VisCell& v : vis) {
            if (v.cell->title.empty()) continue;
            nvgText(vg, v.x + 8.0f, v.y + (v.h - 26.0f) * 0.5f,
                    v.cell->title.c_str(), nullptr);
        }
        nvgTextBatchEnd(vg);

        nvgFontSize(vg, 10);
        nvgFillColor(vg, tok::dim());
        nvgTextBatchBegin(vg);
        for (const VisCell& v : vis) {
            if (v.cell->subtitle.empty()) continue;
            nvgText(vg, v.x + 8.0f, v.y + (v.h - 26.0f) * 0.5f + 16.0f,
                    v.cell->subtitle.c_str(), nullptr);
        }
        nvgTextBatchEnd(vg);

        // Now-line: tracks the wall clock for free — it's recomputed
        // from time() every frame instead of nudging an absolutely
        // positioned Box through yoga once a second like the old
        // overlay rule did.
        if (m_guideStart > 0) {
            const float nowX = px + (float)((double)(time(nullptr) - m_guideStart) * (double)pxPerSec) - m_scrollX;
            if (nowX >= px && nowX <= px + pw) {
                nvgBeginPath(vg);
                nvgRect(vg, nowX, gy, 2.0f, gh);
                nvgFillColor(vg, tok::accent());
                nvgFill(vg);
            }
        }
        nvgRestore(vg);

        // ── 3) CHANNEL COLUMN ──────────────────────────────────────────
        nvgSave(vg);
        nvgScissor(vg, x, gy, colW, gh);

        // One column background + one 1px hairline — not per-row boxes.
        nvgBeginPath(vg);
        nvgRect(vg, x, gy, colW, gh);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 31));  // ~12% black overlay
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, x + colW - 1.0f, gy, 1.0f, gh);
        nvgFillColor(vg, tok::hairline());
        nvgFill(vg);

        const int logoW = gridChannelLogoWidth();
        const int logoH = gridChannelLogoHeight();
        for (int r = r0; r < r1; r++) {
            Row& row = m_rows[r];

            // Lazy logo load, first time this row scrolls into view.
            // Raw-NVG covers (no brls::Image = no extra view); the alive
            // flag + row-index guard follow the MediaItemCell::loadThumbnail
            // lifetime pattern — ImageLoader only runs the callback while
            // the captured flag is still true, and setData/destruction
            // flip it before the rows go away.
            if (!row.logoRequested && !row.channel.thumb.empty()) {
                row.logoRequested = true;
                PlexClient& client = PlexClient::getInstance();
                std::string url = client.getThumbnailUrl(row.channel.thumb,
                                                         logoW * 2, logoH * 2);
                EpgGridView* self = this;
                ImageLoader::loadCoverAsync(url,
                    [self, aliveCopy = m_imgAlive, r](int nvgImg, int w, int h) {
                        // Stale-row guard: if the data was swapped out from
                        // under this load, discard the handle ourselves.
                        if (!aliveCopy->load() || r >= (int)self->m_rows.size()) {
                            NVGcontext* c = brls::Application::getNVGContext();
                            if (c && nvgImg != 0) nvgDeleteImage(c, nvgImg);
                            return;
                        }
                        Row& target = self->m_rows[r];
                        if (target.logoImg != 0 && target.logoImg != nvgImg) {
                            NVGcontext* c = brls::Application::getNVGContext();
                            if (c) nvgDeleteImage(c, target.logoImg);
                        }
                        target.logoImg = nvgImg;
                        target.logoW   = w;
                        target.logoH   = h;
                    },
                    m_imgAlive);
            }

            if (row.logoImg != 0 && row.logoW > 0 && row.logoH > 0) {
                // Letterbox-FIT inside a logoW×logoH slot, centered
                // horizontally, vertically centered minus ~7px to leave
                // room for the channel label underneath — same paint
                // math as MediaItemCell::draw's cover.
                const float rowTop = gy + (float)r * rowH - m_scrollY;
                const float slotX  = x + (colW - (float)logoW) * 0.5f;
                const float slotY  = rowTop + (rowH - (float)logoH) * 0.5f - 7.0f;
                const float scale  = std::min((float)logoW / (float)row.logoW,
                                              (float)logoH / (float)row.logoH);
                const float sw = (float)row.logoW * scale;
                const float sh = (float)row.logoH * scale;
                const float ox = slotX + ((float)logoW - sw) * 0.5f;
                const float oy = slotY + ((float)logoH - sh) * 0.5f;
                NVGpaint paint = nvgImagePattern(vg, ox, oy, sw, sh, 0,
                                                 row.logoImg, 1.0f);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, ox, oy, sw, sh, 6.0f);
                nvgFillPaint(vg, paint);
                nvgFill(vg);
            }
        }

        // Channel labels, batched. Left-aligned near the row bottom —
        // per-row nvgTextBounds centering would cost more than it's worth.
        nvgFontFace(vg, "regular");
        nvgFontSize(vg, 11);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, tok::muted());
        nvgTextBatchBegin(vg);
        for (int r = r0; r < r1; r++) {
            const Row& row = m_rows[r];
            if (row.chLabel.empty()) continue;
            const float rowBottom = gy + (float)(r + 1) * rowH - m_scrollY;
            nvgText(vg, x + 10.0f, rowBottom - 16.0f, row.chLabel.c_str(), nullptr);
        }
        nvgTextBatchEnd(vg);
        nvgRestore(vg);
    }

private:
    bool cursorValid() const {
        return m_curRow >= 0 && m_curRow < (int)m_rows.size() &&
               m_curCell >= 0 && m_curCell < (int)m_rows[m_curRow].cells.size();
    }

    bool activateCursorCell() {
        if (!cursorValid()) return false;
        const Row& row   = m_rows[m_curRow];
        const Cell& cell = row.cells[m_curCell];
        if (cell.noData) {
            if (onChannelSelected) onChannelSelected(row.channel);
        } else {
            if (onCellSelected) onCellSelected(cell.prog, row.channel);
        }
        return true;
    }

    void fireHover() {
        if (!onHover) return;
        if (m_curRow < 0 || m_curRow >= (int)m_rows.size()) return;
        const Row& row = m_rows[m_curRow];
        if (m_curCell < 0 || m_curCell >= (int)row.cells.size()) {
            onHover(row.channel, GuideProgram{}, false);
            return;
        }
        const Cell& cell = row.cells[m_curCell];
        if (cell.noData) onHover(row.channel, GuideProgram{}, false);
        else             onHover(row.channel, cell.prog, true);
    }

    // Virtual focus movement. Returns false at a boundary so the press
    // falls through to normal borealis navigation (escape to hero /
    // sidebar); true consumes it.
    bool moveCursor(int dx, int dy) {
        if (m_rows.empty()) return false;

        if (dy != 0) {
            const int target = m_curRow + (dy > 0 ? 1 : -1);
            if (target < 0 || target >= (int)m_rows.size()) return false;
            const Row& next = m_rows[target];
            // buildEPGGrid guarantees every row has at least one cell
            // (noData placeholder), but guard anyway.
            if (next.cells.empty()) return false;

            // Pick the cell in the new row whose [x, x+w) range contains
            // the CURRENT cell's x start — mirrors the old view-based
            // X-alignment behaviour (DOWN from a 1-hour show onto two
            // 30-minute cells picks the first, not the boundary
            // neighbour). Fall back to the nearest cell by |x - curX|.
            const float curX = cursorValid() ? m_rows[m_curRow].cells[m_curCell].x : 0.0f;
            int pick = -1;
            for (size_t i = 0; i < next.cells.size(); i++) {
                const Cell& c = next.cells[i];
                if (curX >= c.x && curX < c.x + c.w) { pick = (int)i; break; }
            }
            if (pick < 0) {
                float best = 0.0f;
                for (size_t i = 0; i < next.cells.size(); i++) {
                    const float d = std::fabs(next.cells[i].x - curX);
                    if (pick < 0 || d < best) { best = d; pick = (int)i; }
                }
            }
            m_curRow  = target;
            m_curCell = pick;
        } else if (dx != 0) {
            if (m_curRow < 0 || m_curRow >= (int)m_rows.size()) return false;
            const Row& row   = m_rows[m_curRow];
            const int target = m_curCell + (dx > 0 ? 1 : -1);
            if (target < 0 || target >= (int)row.cells.size()) return false;
            m_curCell = target;
        } else {
            return false;
        }

        ensureCursorVisible();
        fireHover();
        return true;
    }

    // Instant scroll (plain float stores — no animation, no relayout):
    // clamp so the cursor row is fully inside the row viewport and the
    // cursor cell's start (plus as much of its width as fits) is inside
    // the program viewport.
    void ensureCursorVisible() {
        const float rowH  = (float)livetvRowHeight();
        const float slotW = (float)livetvTimeSlotWidth();
        const float gh    = getHeight() - (float)TIME_HEADER_HEIGHT;
        const float pw    = getWidth() - (float)livetvChannelColWidth();

        const float rowTop = (float)m_curRow * rowH;
        const float rowBot = rowTop + rowH;
        if (rowTop < m_scrollY)               m_scrollY = rowTop;
        else if (gh > 0 && rowBot > m_scrollY + gh) m_scrollY = rowBot - gh;

        if (cursorValid()) {
            const Cell& c = m_rows[m_curRow].cells[m_curCell];
            if (c.x < m_scrollX)                       m_scrollX = c.x;
            else if (pw > 0 && c.x + c.w > m_scrollX + pw)
                // Cells wider than the viewport pin their start to the
                // left edge instead of pushing it off-screen.
                m_scrollX = std::min(c.x, c.x + c.w - pw);
        }

        const float maxY = std::max(0.0f, (float)m_rows.size() * rowH - gh);
        const float maxX = std::max(0.0f, (float)(m_hours * 2) * slotW - pw);
        m_scrollY = std::min(std::max(m_scrollY, 0.0f), maxY);
        m_scrollX = std::min(std::max(m_scrollX, 0.0f), maxX);
    }

    std::vector<Row> m_rows;
    int64_t m_guideStart = 0;
    int m_hours = 12;
    float m_scrollX = 0, m_scrollY = 0;   // instant scroll, no animation
    int m_curRow = 0, m_curCell = 0;      // virtual focus cursor
    // Cancel handle for loadCoverAsync — regenerated on every setData so
    // stale loads can't write into replaced rows.
    std::shared_ptr<std::atomic<bool>> m_imgAlive =
        std::make_shared<std::atomic<bool>>(true);
};


LiveTVTab::LiveTVTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // The whole tab is a flex column with NO outer page scroll — the
    // guide scrolls internally (EpgGridView pans two plain floats), so
    // the hero stays pinned at the top while the guide moves.
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
    buildHero();
    m_scrollContent->addView(m_heroBox);

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

    // The entire guide — time header, channel column, cells, now-line —
    // is ONE custom-drawn view. No scroll frames, no per-row boxes, no
    // absolutely positioned time line: EpgGridView draws everything in a
    // single pass and handles dpad movement with a virtual cursor.
    m_grid = new EpgGridView();
    m_grid->setGrow(1.0f);
    m_grid->formatTime = [this](int64_t t) { return this->formatTime(t); };
    m_grid->onCellSelected = [this](const GuideProgram& prog, const LiveTVChannel& ch) {
        onProgramSelected(prog, ch);
    };
    m_grid->onChannelSelected = [this](const LiveTVChannel& ch) {
        onChannelSelected(ch);
    };
    // Cursor movement drives the hero exactly like per-cell focus events
    // used to — through the debounced queueHeroFor* path so dpad-repeat
    // across the guide doesn't turn into a relayout + network storm.
    m_grid->onHover = [this](const LiveTVChannel& ch, const GuideProgram& prog, bool hasProgram) {
        if (hasProgram) queueHeroForProgram(ch, prog);
        else            queueHeroForChannel(ch);
    };
    m_guideContainer->addView(m_grid);

    m_scrollContent->addView(m_guideContainer);

    // No outer scroll: m_scrollContent goes straight into the tab so the
    // hero stays pinned at the top and only the guide scrolls.
    this->addView(m_scrollContent);

    brls::Logger::debug("LiveTVTab: Loading content...");
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

brls::View* LiveTVTab::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    // Section layout (top to bottom): Hero (m_heroBox) -> Guide (m_grid).
    // Movement *inside* the guide is handled by EpgGridView's hidden dpad
    // actions (virtual cursor); this override only kicks in when a press
    // escapes the grid (moveCursor returned false at a boundary) or
    // leaves the hero — so all we do is hop to the adjacent section.
    const bool inHero  = isDescendantOf(currentView, m_heroBox);
    const bool inGuide = isDescendantOf(currentView, m_guideContainer);

    if (direction == brls::FocusDirection::DOWN && inHero) {
        if (m_grid) return m_grid;
    }
    else if (direction == brls::FocusDirection::UP && inGuide) {
        if (brls::View* t = findFirstFocusableInBox(m_heroBox)) return t;
    }

    // Default behavior for left/right and unhandled cases
    return brls::Box::getNextFocus(direction, currentView);
}

void LiveTVTab::draw(NVGcontext* vg, float x, float y, float width, float height,
                     brls::Style style, brls::FrameContext* ctx) {
    // Frame-total accounting: wall time between draws captures everything
    // (GPU, other views), while boxdraw is this tab's subtree — which now
    // includes the entire guide via EpgGridView::draw. The old cull /
    // scroll-sync / batch-text passes are gone: the grid has no view
    // forest to cull or sync, and its text batches live inside its own
    // draw().
    const int64_t pf0 = brls::getCPUTimeUsec();
    if (m_perfLastFrameUs > 0) m_perfFrameUs += pf0 - m_perfLastFrameUs;
    m_perfLastFrameUs = pf0;

    // Hover-driven hero refresh, applied only once focus has rested.
    applyPendingHero();

    const int64_t pf1 = brls::getCPUTimeUsec();
    brls::Box::draw(vg, x, y, width, height, style, ctx);
    m_perfDrawUs += brls::getCPUTimeUsec() - pf1;

    // Periodic cost report (~every 5-20s depending on frame rate). On Vita
    // this lands in ux0:data/VitaPlex/vitaplex.log.
    if (++m_perfFrames >= 300) {
        brls::Logger::info(
            "LiveTV perf avg us/frame over {} frames: total={} boxdraw={}",
            m_perfFrames,
            m_perfFrameUs / m_perfFrames, m_perfDrawUs / m_perfFrames);
        m_perfFrameUs = m_perfDrawUs = 0;
        m_perfFrames  = 0;
    }
}

void LiveTVTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    // willDisappear cancelled any in-flight logo loads; let the grid
    // re-request the ones that never landed.
    if (m_grid) m_grid->retryMissingLogos();

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
    if (timestamp == 0) timestamp = time(nullptr);

    time_t t = (time_t)timestamp;
    struct tm* tm_info = localtime(&t);

    char buffer[16];
    strftime(buffer, sizeof(buffer), "%I:%M %p", tm_info);
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
    if (m_pendingHeroHasProgram)
        updateHeroForProgram(m_pendingHeroChannel, m_pendingHeroProgram);
    else
        updateHeroForChannel(m_pendingHeroChannel);
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
        bool success = client.fetchEPGGrid(channels, m_hoursToShow);

        if (success) {
            brls::Logger::info("LiveTVTab: Got {} channels with EPG", channels.size());

            brls::sync([this, channels, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_channels = channels;

                if (!m_channels.empty()) {
                    updateHeroForChannel(m_channels.front());
                }

                buildEPGGrid();

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

void LiveTVTab::buildEPGGrid() {
    // Pure data build — NO views are created here. The old implementation
    // manufactured a Box per row / spacer / cell (~2500 views + yoga nodes
    // for 100 channels), so opening the guide paid a full-tree layout and
    // every frame afterwards paid the per-view frame() walk. Now this
    // just formats strings and computes pixel offsets, then hands the
    // whole set to EpgGridView in one O(data) swap.

    // Set guide start time to current time rounded down to 30 minutes.
    time_t now = time(nullptr);
    m_guideStartTime = now - (now % 1800);

    const int gridHours = m_hoursToShow;
    const int64_t guideEndTime = m_guideStartTime + (int64_t)gridHours * 3600;
    const int slotW = livetvTimeSlotWidth();

    std::vector<EpgGridView::Row> rows;
    rows.reserve(m_channels.size());

    for (const auto& channel : m_channels) {
        EpgGridView::Row row;
        row.channel = channel;
        row.chLabel = !channel.channelIdentifier.empty()
                          ? channel.channelIdentifier
                          : std::to_string(channel.channelNumber);

        if (!channel.programs.empty()) {
            for (const auto& prog : channel.programs) {
                if (prog.endTime <= m_guideStartTime || prog.startTime >= guideEndTime) continue;

                int64_t visStart = std::max(prog.startTime, m_guideStartTime);
                int64_t visEnd   = std::min(prog.endTime > 0 ? prog.endTime : visStart + 1800,
                                            guideEndTime);

                int cellWidth = (int)((visEnd - visStart) * slotW / 1800);
                if (cellWidth < 40) cellWidth = 40;

                bool isCurrently = (prog.startTime <= (int64_t)now && prog.endTime > (int64_t)now);

                EpgGridView::Cell cell;
                // +2 / -4: the old cells carried 2px margins on each side;
                // bake them into the rect so the visuals are identical.
                cell.x = (float)((visStart - m_guideStartTime) * slotW / 1800.0) + 2.0f;
                cell.w = (float)cellWidth - 4.0f;
                cell.onNow = isCurrently;

                // Truncate the title to what fits at ~8px/char so the
                // batched text pass never paints outside its cell (there
                // is no per-cell scissor — one scissor covers the whole
                // program area).
                std::string title = prog.title;
                int maxChars = cellWidth / 8;
                if (maxChars < 4) maxChars = 4;
                if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
                cell.title = std::move(title);

                std::string sub = formatTime(prog.startTime) + " – " + formatTime(prog.endTime);
                if (isCurrently) sub += "  ·  on now";
                cell.subtitle = std::move(sub);

                cell.prog.title       = prog.title;
                cell.prog.summary     = prog.summary;
                cell.prog.startTime   = prog.startTime;
                cell.prog.endTime     = prog.endTime;
                cell.prog.ratingKey   = prog.ratingKey;
                cell.prog.metadataKey = prog.metadataKey;
                cell.prog.thumb       = prog.thumb;

                row.cells.push_back(std::move(cell));
            }
        } else if (!channel.currentProgram.empty() && channel.programStart > 0) {
            // Legacy fallback: the EPG didn't fill the programs vector but
            // the channel still carries "now playing" fields — one on-now
            // cell synthesized from those.
            int64_t progStart = std::max(channel.programStart, m_guideStartTime);
            int64_t progEnd   = std::min(channel.programEnd > 0 ? channel.programEnd
                                                                : progStart + 1800,
                                         guideEndTime);

            int cellWidth = (int)((progEnd - progStart) * slotW / 1800);
            if (cellWidth < 40) cellWidth = 40;

            EpgGridView::Cell cell;
            cell.x = (float)((progStart - m_guideStartTime) * slotW / 1800.0) + 2.0f;
            cell.w = (float)cellWidth - 4.0f;
            cell.onNow = true;

            std::string title = channel.currentProgram;
            int maxChars = cellWidth / 8;
            if (maxChars < 4) maxChars = 4;
            if ((int)title.length() > maxChars) title = title.substr(0, maxChars - 2) + "..";
            cell.title = std::move(title);
            cell.subtitle = formatTime(channel.programStart) + " – "
                          + formatTime(channel.programEnd) + "  ·  on now";

            cell.prog.title     = channel.currentProgram;
            cell.prog.startTime = channel.programStart;
            cell.prog.endTime   = channel.programEnd > 0 ? channel.programEnd
                                                         : channel.programStart + 1800;

            row.cells.push_back(std::move(cell));
        }

        if (row.cells.empty()) {
            // No guide data at all (or every program fell outside the
            // window): a placeholder cell so A still tunes the channel
            // and the vertical cursor never dead-ends on an empty row.
            EpgGridView::Cell cell;
            cell.noData = true;
            cell.title  = "No guide data";
            cell.x = 2.0f;
            cell.w = (float)(slotW * 2) - 4.0f;
            row.cells.push_back(std::move(cell));
        }

        rows.push_back(std::move(row));
    }

    m_grid->setData(std::move(rows), m_guideStartTime, m_hoursToShow);
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

        HttpResponse resp = httpClient.request(req);

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
