/**
 * VitaPlex - Search Tab implementation
 * On-screen keyboard (left, 300px) + type-grouped result grids (right).
 */

#include "view/search_tab.hpp"
#include "view/media_detail_view.hpp"
#include "view/horizontal_scroll_row.hpp"
#include "view/long_press_gesture.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include "platform/platform.hpp"
#include "utils/air_time.hpp"
#include "view/livetv_actions.hpp"

#include <atomic>
#include <cctype>

namespace vitaplex {

// ── Search palette (reference design tokens) ──
namespace spal {
    inline NVGcolor bg()      { return nvgRGB(0x1b, 0x1b, 0x1d); }
    inline NVGcolor field()   { return nvgRGB(0x2b, 0x2b, 0x2e); }
    inline NVGcolor key()     { return nvgRGB(0x2c, 0x2c, 0x2f); }
    inline NVGcolor keyLine() { return nvgRGBA(255, 255, 255, 15); }  // .06
    inline NVGcolor fldLine() { return nvgRGBA(255, 255, 255, 26); }  // .10
    inline NVGcolor amber()   { return nvgRGB(0xe5, 0xa0, 0x0d); }
    inline NVGcolor qText()   { return nvgRGB(0xf2, 0xf2, 0xf4); }
    inline NVGcolor keyText() { return nvgRGB(0xdf, 0xdf, 0xe3); }
    inline NVGcolor h2()      { return nvgRGB(0xf4, 0xf4, 0xf6); }
    inline NVGcolor cap()     { return nvgRGB(0xe7, 0xe7, 0xea); }
    inline NVGcolor muted()   { return nvgRGB(0x8c, 0x8c, 0x93); }
    inline NVGcolor poster()  { return nvgRGB(0x2a, 0x2c, 0x34); }
}

namespace {

// A keyboard key: focusable box (default gold highlight), gold ring on focus.
brls::Box* makeKeyBox(int height, int radius = 6) {
    auto* key = new brls::Box();
    key->setHeight((float)height);
    key->setGrow(1.0f);                 // equal columns within the row
    key->setJustifyContent(brls::JustifyContent::CENTER);
    key->setAlignItems(brls::AlignItems::CENTER);
    key->setCornerRadius((float)radius);
    key->setBackgroundColor(spal::key());
    key->setBorderColor(spal::keyLine());
    key->setBorderThickness(1.0f);
    key->setFocusable(true);
    return key;
}

brls::Box* makeCharKey(const std::string& ch, bool dock = false) {
    auto* key = makeKeyBox(dock ? 46 : 30, dock ? 9 : 6);
    auto* lbl = new brls::Label();
    lbl->setText(ch);
    lbl->setFontSize(dock ? 18.0f : 14.0f);
    lbl->setTextColor(spal::keyText());
    key->addView(lbl);
    return key;
}

brls::Box* makeSpecialKey(const std::string& iconRes, bool dock = false) {
    auto* key = makeKeyBox(dock ? 46 : 34, dock ? 9 : 6);
    auto* icn = new brls::Image();
    icn->setImageFromRes(iconRes);
    icn->setWidth(dock ? 20 : 16);
    icn->setHeight(dock ? 20 : 16);
    icn->setScalingType(brls::ImageScalingType::FIT);
    key->addView(icn);
    return key;
}

// Map a column index when stepping between rows of differing width.
size_t mapCol(size_t c, size_t fromN, size_t toN) {
    if (fromN == 0 || toN == 0) return 0;
    size_t tc = (c * toN) / fromN;
    return tc >= toN ? toN - 1 : tc;
}

// Case-insensitive substring test.
bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        for (char& ch : s) ch = (char)tolower((unsigned char)ch);
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::string cardTitle(const MediaItem& it) {
    if (it.mediaType == MediaType::EPISODE && !it.grandparentTitle.empty())
        return it.grandparentTitle;       // show name (episode title goes in the sub)
    return it.title;
}

std::string cardSub(const MediaItem& it) {
    if (it.mediaType == MediaType::EPISODE) {
        std::string s;
        if (it.parentIndex > 0 || it.index > 0)
            s = "S" + std::to_string(it.parentIndex) + "E" + std::to_string(it.index);
        if (!it.title.empty()) s += (s.empty() ? "" : " \xC2\xB7 ") + it.title;
        return s;
    }
    if ((it.mediaType == MediaType::MUSIC_ALBUM || it.mediaType == MediaType::MUSIC_TRACK)
        && !it.parentTitle.empty())
        return it.parentTitle;            // artist
    if (it.year > 0) return std::to_string(it.year);
    return "";
}

// START / long-press context menu wiring. Both go through the shared
// dispatcher so this cannot drift from the other views that list items.
void wireContextMenu(brls::View* cell, const MediaItem& item) {
    if (!MediaDetailView::hasContextMenu(item)) return;
    cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                         [item](brls::View*) {
                             MediaDetailView::showContextMenuFor(item);
                             return true;
                         });
    cell->addGestureRecognizer(new LongPressGestureRecognizer(
        cell, [item](LongPressGestureStatus status) {
            if (status.state == brls::GestureState::START)
                MediaDetailView::showContextMenuFor(item);
        }));
}

} // namespace

bool SearchTab::isPhoneSized() {
    // Width is what actually breaks the desktop layout: a 300px keyboard column
    // beside results needs room the phone doesn't have. contentWidth is in the
    // same units the views are laid out in, so it compares directly against the
    // column. Portrait on a handheld platform counts too, for a tablet-ish
    // width that still wants the touch layout.
    if (brls::Application::contentWidth > 0.0f &&
        brls::Application::contentWidth < 600.0f) return true;
#if defined(ANDROID) || defined(PLATFORM_IOS)
    if (platform::isPortrait()) return true;
#endif
    return false;
}

SearchTab::Layout SearchTab::resolveLayout() {
    const int setting = Application::getInstance().getSettings().searchMobileLayout;
    if (setting == 1) return Layout::NativeIme;
    if (setting == 2) return Layout::OnScreen;
    // Auto: the phone gets the IME, everything else keeps today's layout.
    return isPhoneSized() ? Layout::NativeIme : Layout::TwoColumn;
}

SearchTab::SearchTab() {
    m_layout = resolveLayout();
    buildLayout();

    // ---------------- Physical keyboard typing ----------------
    // The raw key event fires app-wide, so only act while focus is inside this
    // tab. A-Z / 0-9 / space / backspace aren't mapped to controller buttons,
    // so there's no conflict with navigation (Enter still clicks the on-screen
    // key, Escape still goes back).
    m_inputManager = brls::Application::getPlatform()
                         ? brls::Application::getPlatform()->getInputManager() : nullptr;
    if (m_inputManager) {
        m_kbSub = m_inputManager->getKeyboardKeyStateChanged()->subscribe(
            [this](brls::KeyState ks) {
                if (!ks.pressed) return;
                brls::View* f = brls::Application::getCurrentFocus();
                bool inSearch = false;
                for (brls::View* v = f; v != nullptr; v = v->getParent())
                    if (v == this) { inSearch = true; break; }
                if (!inSearch) return;

                int k = (int)ks.key;
                if (k >= brls::BRLS_KBD_KEY_A && k <= brls::BRLS_KBD_KEY_Z)
                    appendChar(std::string(1, (char)('A' + (k - brls::BRLS_KBD_KEY_A))));
                else if (k >= brls::BRLS_KBD_KEY_0 && k <= brls::BRLS_KBD_KEY_9)
                    appendChar(std::string(1, (char)('0' + (k - brls::BRLS_KBD_KEY_0))));
                else if (k == brls::BRLS_KBD_KEY_SPACE)     appendChar(" ");
                else if (k == brls::BRLS_KBD_KEY_BACKSPACE) backspace();
            });
        m_kbSubscribed = true;
    }
}

void SearchTab::buildLayout() {
    this->setGrow(1.0f);
    this->setBackgroundColor(spal::bg());
    m_queryLabel = nullptr;
    m_keyboardFirstKey = nullptr;
    m_clearButton = nullptr;
    m_resultsScroll = nullptr;
    m_resultsContent = nullptr;

    if (m_layout == Layout::TwoColumn) buildTwoColumn();
    else                               buildMobile();

    updateField();
    // Results survive a layout switch — re-render into the new tree rather than
    // re-querying the server for a search the user already ran.
    if (!m_query.empty()) rebuildResults();
}

void SearchTab::switchMobileLayout(Layout to) {
    if (to == m_layout) return;
    m_layout = to;

    // Park focus outside the subtree before deleting it. ~View() tries to clear
    // the focus pointer with giveFocus(nullptr), but that call returns early on
    // a null target and leaves it dangling — and the hints-update event fired
    // while the new tree is built reads it. The sidebar is the nearest thing
    // that outlives us; the rebuild takes focus back straight after.
    if (brls::View* sidebar = this->getNearestView("brls/tab_frame/sidebar"))
        brls::Application::giveFocus(sidebar);

    this->clearViews();
    buildLayout();
    brls::Application::giveFocus(this);
}

void SearchTab::openIme() {
    auto* ime = brls::Application::getImeManager();
    if (!ime) return;
    std::weak_ptr<bool> aliveWeak = m_alive;
    // The callback delivers the finished string, not each keystroke, so results
    // refresh on commit. Seeded with the current query so reopening the IME
    // continues an existing search instead of starting over.
    ime->openForText(
        [this, aliveWeak](std::string text) {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            m_query = std::move(text);
            updateField();
            performSearch();
        },
        "Search", "", 64, m_query);
}

void SearchTab::buildTwoColumn() {
    this->setAxis(brls::Axis::ROW);
    this->setAlignItems(brls::AlignItems::STRETCH);

    // ---------------- Left column: field + keyboard (300px) ----------------
    auto* left = new brls::Box();
    left->setAxis(brls::Axis::COLUMN);
    left->setAlignItems(brls::AlignItems::STRETCH);   // field + keyboard fill the column
    left->setWidth(300);
    left->setPadding(18, 16, 0, 20);

    // Search field (display only — the keyboard drives input).
    auto* field = new brls::Box();
    field->setAxis(brls::Axis::ROW);
    field->setAlignItems(brls::AlignItems::CENTER);
    field->setHeight(38);
    field->setCornerRadius(7);
    field->setBackgroundColor(spal::field());
    field->setBorderColor(spal::fldLine());
    field->setBorderThickness(1.0f);
    field->setPadding(0, 12, 0, 12);

    auto* fieldIcon = new brls::Image();
    fieldIcon->setImageFromRes("icons/magnify.png");
    fieldIcon->setWidth(16);
    fieldIcon->setHeight(16);
    fieldIcon->setMarginRight(10);
    field->addView(fieldIcon);

    m_queryLabel = new brls::Label();
    m_queryLabel->setText("");
    m_queryLabel->setFontSize(15);
    m_queryLabel->setTextColor(spal::qText());
    m_queryLabel->setSingleLine(true);   // sizes to content so the caret follows the text
    field->addView(m_queryLabel);

    auto* caret = new brls::Rectangle();
    caret->setColor(spal::amber());
    caret->setWidth(2);
    caret->setHeight(18);
    caret->setMarginLeft(1);
    field->addView(caret);

    left->addView(field);

    buildKeyboard(left);
    this->addView(left);

    // ---------------- Right column: results ----------------
    m_resultsScroll = new brls::ScrollingFrame();
    m_resultsScroll->setGrow(1.0f);
    m_resultsScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    m_resultsContent = new brls::Box();
    m_resultsContent->setAxis(brls::Axis::COLUMN);
    m_resultsContent->setAlignItems(brls::AlignItems::STRETCH);   // rows fill width + scroll
    m_resultsContent->setPadding(24, 18, 0, 8);

    m_resultsScroll->setContentView(m_resultsContent);
    this->addView(m_resultsScroll);
}

void SearchTab::buildMobile() {
    const bool dock = (m_layout == Layout::OnScreen);

    // Stacked, not side-by-side: the full width goes to the field and results,
    // and the keyboard (layout B only) takes a bottom dock instead of a column.
    this->setAxis(brls::Axis::COLUMN);
    this->setAlignItems(brls::AlignItems::STRETCH);

    // ---------------- Top bar: the search field ----------------
    // No hamburger: this app has no mobile sidebar drawer to open, so the field
    // takes the full width.
    auto* topBar = new brls::Box();
    topBar->setAxis(brls::Axis::ROW);
    topBar->setAlignItems(brls::AlignItems::CENTER);
    topBar->setPadding(10, 14, 10, 14);

    auto* field = new brls::Box();
    field->setAxis(brls::Axis::ROW);
    field->setAlignItems(brls::AlignItems::CENTER);
    field->setGrow(1.0f);
    field->setHeight(48);
    field->setCornerRadius(12);
    field->setBackgroundColor(spal::field());
    field->setBorderColor(spal::fldLine());
    field->setBorderThickness(1.0f);
    field->setPadding(0, 12, 0, 14);
    field->setFocusable(true);
    field->setHighlightCornerRadius(12);

    auto* fieldIcon = new brls::Image();
    fieldIcon->setImageFromRes("icons/magnify.png");
    fieldIcon->setWidth(18);
    fieldIcon->setHeight(18);
    fieldIcon->setMarginRight(10);
    field->addView(fieldIcon);

    m_queryLabel = new brls::Label();
    m_queryLabel->setText(m_query);
    m_queryLabel->setFontSize(16);
    m_queryLabel->setTextColor(spal::qText());
    m_queryLabel->setSingleLine(true);
    field->addView(m_queryLabel);

    auto* caret = new brls::Rectangle();
    caret->setColor(spal::amber());
    caret->setWidth(2);
    caret->setHeight(20);
    caret->setMarginLeft(1);
    field->addView(caret);

    // Spacer so the clear button sits hard against the right edge.
    auto* fieldGap = new brls::Box();
    fieldGap->setGrow(1.0f);
    field->addView(fieldGap);

    m_clearButton = new brls::Box();
    m_clearButton->setAxis(brls::Axis::ROW);
    m_clearButton->setJustifyContent(brls::JustifyContent::CENTER);
    m_clearButton->setAlignItems(brls::AlignItems::CENTER);
    m_clearButton->setWidth(22);
    m_clearButton->setHeight(22);
    m_clearButton->setCornerRadius(11);
    m_clearButton->setBackgroundColor(spal::key());
    m_clearButton->setFocusable(true);
    m_clearButton->setHighlightCornerRadius(11);
    auto* clearIcon = new brls::Image();
    clearIcon->setImageFromRes("icons/delete-outline.png");
    clearIcon->setWidth(13);
    clearIcon->setHeight(13);
    m_clearButton->addView(clearIcon);
    m_clearButton->registerClickAction([this](brls::View*) { clearQuery(); return true; });
    m_clearButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_clearButton));
    field->addView(m_clearButton);

    // Tapping the field is how you type in layout A; in B it re-opens the IME
    // as an escape hatch for long queries the grid makes tedious.
    field->registerClickAction([this](brls::View*) { openIme(); return true; });
    field->addGestureRecognizer(new brls::TapGestureRecognizer(field));

    topBar->addView(field);
    this->addView(topBar);

    // ---------------- Results ----------------
    m_resultsScroll = new brls::ScrollingFrame();
    m_resultsScroll->setGrow(1.0f);
    m_resultsScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    m_resultsContent = new brls::Box();
    m_resultsContent->setAxis(brls::Axis::COLUMN);
    m_resultsContent->setAlignItems(brls::AlignItems::STRETCH);
    m_resultsContent->setPadding(4, 0, dock ? 8 : 76, 0);

    m_resultsScroll->setContentView(m_resultsContent);
    this->addView(m_resultsScroll);

    if (dock) {
        // ---------------- B: keyboard docked along the bottom ----------------
        auto* dockBox = new brls::Box();
        dockBox->setAxis(brls::Axis::COLUMN);
        dockBox->setAlignItems(brls::AlignItems::STRETCH);
        dockBox->setBackgroundColor(nvgRGB(0x21, 0x21, 0x24));
        dockBox->setLineColor(nvgRGB(0x31, 0x31, 0x38));
        dockBox->setLineTop(1.0f);
        dockBox->setPadding(10, 10, 10, 10);

        auto* dockHead = new brls::Box();
        dockHead->setAxis(brls::Axis::ROW);
        dockHead->setAlignItems(brls::AlignItems::CENTER);
        dockHead->setMarginBottom(8);

        auto* dockTitle = new brls::Label();
        dockTitle->setText("ON-SCREEN KEYBOARD");
        dockTitle->setFontSize(11);
        dockTitle->setTextColor(spal::muted());
        dockHead->addView(dockTitle);

        auto* headGap = new brls::Box();
        headGap->setGrow(1.0f);
        dockHead->addView(headGap);

        auto* hide = new brls::Box();
        hide->setAxis(brls::Axis::ROW);
        hide->setAlignItems(brls::AlignItems::CENTER);
        hide->setHeight(26);
        hide->setPadding(0, 8, 0, 8);
        hide->setCornerRadius(7);
        hide->setFocusable(true);
        hide->setHighlightCornerRadius(7);
        auto* hideLbl = new brls::Label();
        hideLbl->setText("Hide");
        hideLbl->setFontSize(13);
        hideLbl->setTextColor(spal::amber());
        hide->addView(hideLbl);
        hide->registerClickAction([this](brls::View*) {
            switchMobileLayout(Layout::NativeIme);
            return true;
        });
        hide->addGestureRecognizer(new brls::TapGestureRecognizer(hide));
        dockHead->addView(hide);

        dockBox->addView(dockHead);
        buildKeyboard(dockBox, /*dock=*/true);
        this->addView(dockBox);
    } else {
        // ---------------- A: FAB back to the on-screen keyboard ----------------
        // Absolute so it floats over the results rather than reserving a row;
        // the results already carry bottom padding to clear it.
        auto* fab = new brls::Box();
        fab->setPositionType(brls::PositionType::ABSOLUTE);
        fab->setPositionBottom(16);
        fab->setPositionRight(16);
        fab->setWidth(52);
        fab->setHeight(52);
        fab->setCornerRadius(16);
        fab->setBackgroundColor(spal::amber());
        fab->setJustifyContent(brls::JustifyContent::CENTER);
        fab->setAlignItems(brls::AlignItems::CENTER);
        fab->setFocusable(true);
        fab->setHighlightCornerRadius(16);
        auto* fabIcon = new brls::Image();
        fabIcon->setImageFromRes("icons/keyboard-space.png");
        fabIcon->setWidth(24);
        fabIcon->setHeight(24);
        fab->addView(fabIcon);
        fab->registerClickAction([this](brls::View*) {
            switchMobileLayout(Layout::OnScreen);
            return true;
        });
        fab->addGestureRecognizer(new brls::TapGestureRecognizer(fab));
        this->addView(fab);
    }
}

void SearchTab::buildKeyboard(brls::Box* parent, bool dock) {
    auto* kb = new brls::Box();
    kb->setAxis(brls::Axis::COLUMN);
    kb->setAlignItems(brls::AlignItems::STRETCH);   // rows fill the column width
    kb->setMarginTop(dock ? 0.0f : 14.0f);

    std::vector<std::vector<brls::Box*>> grid;   // [row][col] for nav wiring

    // Special row: Clear, Backspace, Space, Search.
    auto* sprow = new brls::Box();
    sprow->setAxis(brls::Axis::ROW);
    sprow->setMarginBottom(dock ? 7.0f : 7.0f);
    std::vector<brls::Box*> srowKeys;
    struct Spec { const char* icon; int act; };           // 0 clear,1 bksp,2 space,3 search
    const Spec specials[4] = {
        {"icons/delete-outline.png",    0},
        {"icons/backspace-outline.png", 1},
        {"icons/keyboard-space.png",    2},
        {"icons/magnify.png",           3},
    };
    for (int i = 0; i < 4; i++) {
        auto* key = makeSpecialKey(specials[i].icon, dock);
        if (i > 0) key->setMarginLeft(6);
        int act = specials[i].act;
        key->registerClickAction([this, act](brls::View*) {
            if (act == 0)      clearQuery();
            else if (act == 1) backspace();
            else if (act == 2) appendChar(" ");
            else               performSearch();
            return true;
        });
        key->addGestureRecognizer(new brls::TapGestureRecognizer(key));
        sprow->addView(key);
        srowKeys.push_back(key);
    }
    kb->addView(sprow);
    grid.push_back(srowKeys);

    // Character rows.
    static const char* const rows[6] = {
        "ABCDEF", "GHIJKL", "MNOPQR", "STUVWX", "YZ1234", "567890"
    };
    for (int r = 0; r < 6; r++) {
        auto* crow = new brls::Box();
        crow->setAxis(brls::Axis::ROW);
        if (r < 5) crow->setMarginBottom(7);
        std::vector<brls::Box*> rowKeys;
        for (int c = 0; c < 6; c++) {
            std::string ch(1, rows[r][c]);
            auto* key = makeCharKey(ch, dock);
            if (c > 0) key->setMarginLeft(6);
            key->registerClickAction([this, ch](brls::View*) { appendChar(ch); return true; });
            key->addGestureRecognizer(new brls::TapGestureRecognizer(key));
            crow->addView(key);
            rowKeys.push_back(key);
        }
        kb->addView(crow);
        grid.push_back(rowKeys);
    }

    // Wire UP / DOWN so vertical moves stay column-aligned (borealis' default
    // would land on the first key of the next row). LEFT / RIGHT use the row
    // box's own navigation, and RIGHT off the last key exits to the results.
    for (size_t r = 0; r < grid.size(); r++) {
        for (size_t c = 0; c < grid[r].size(); c++) {
            brls::Box* key = grid[r][c];
            if (r + 1 < grid.size()) {
                auto& below = grid[r + 1];
                key->setCustomNavigationRoute(brls::FocusDirection::DOWN,
                                              below[mapCol(c, grid[r].size(), below.size())]);
            }
            if (r > 0) {
                auto& above = grid[r - 1];
                key->setCustomNavigationRoute(brls::FocusDirection::UP,
                                              above[mapCol(c, grid[r].size(), above.size())]);
            }
        }
    }

    m_keyboardFirstKey = grid.size() > 1 ? grid[1][0] : grid[0][0];   // 'A'
    parent->addView(kb);
}

void SearchTab::appendChar(const std::string& c) { m_query += c; updateField(); performSearch(); }
void SearchTab::backspace()                      { if (!m_query.empty()) m_query.pop_back(); updateField(); performSearch(); }
void SearchTab::clearQuery()                     { m_query.clear(); updateField(); performSearch(); }
void SearchTab::updateField() {
    if (m_queryLabel) m_queryLabel->setText(m_query);
    // The clear affordance only exists in the mobile field, and only earns its
    // space once there is something to clear. Focusability tracks visibility —
    // borealis tests only a view's own flag, so a hidden button stays reachable.
    if (m_clearButton) {
        const bool show = !m_query.empty();
        m_clearButton->setVisibility(show ? brls::Visibility::VISIBLE
                                          : brls::Visibility::GONE);
        m_clearButton->setFocusable(show);
    }
}

void SearchTab::performSearch() {
    if (m_query.empty()) {
        m_movies.clear(); m_episodes.clear(); m_shows.clear();
        m_artists.clear(); m_albums.clear(); m_tracks.clear();
        m_liveTV.clear();
        rebuildResults();
        return;
    }

    int gen = ++m_loadGeneration;
    std::string q = m_query;
    asyncRun([this, q, gen, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> results;
        bool ok = client.search(q, results);

        // The EPG provider is searched separately (see searchLiveTV). This
        // runs per keystroke, so hold off until the query is long enough to
        // mean something -- one or two characters match half the schedule
        // and cost a request each.
        std::vector<MediaItem> live;
        if (q.size() >= 3) client.searchLiveTV(q, live);

        brls::sync([this, q, ok, results, live, gen, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            if (gen != m_loadGeneration) return;   // stale

            m_movies.clear(); m_episodes.clear(); m_shows.clear();
            m_artists.clear(); m_albums.clear(); m_tracks.clear();
            m_liveTV = live;
            if (ok) {
                for (const auto& it : results) {
                    switch (it.mediaType) {
                        case MediaType::MOVIE:        m_movies.push_back(it);   break;
                        case MediaType::EPISODE:
                            // Plex returns every episode of a show whose NAME
                            // matches (all of "One Piece" for "one"); keep only
                            // episodes whose own title contains the query — the
                            // show itself is already in the TV Shows row.
                            if (icontains(it.title, q)) m_episodes.push_back(it);
                            break;
                        case MediaType::SHOW:
                        case MediaType::SEASON:       m_shows.push_back(it);    break;
                        case MediaType::MUSIC_ARTIST: m_artists.push_back(it);  break;
                        case MediaType::MUSIC_ALBUM:  m_albums.push_back(it);   break;
                        case MediaType::MUSIC_TRACK:  m_tracks.push_back(it);   break;
                        default: break;
                    }
                }
            }
            rebuildResults();
        });
    });
}

void SearchTab::rebuildResults() {
    if (!m_resultsContent) return;
    m_resultsContent->clearViews();

    // Fresh image-alive token so loads from the previous result set bail.
    if (m_imgAlive) *m_imgAlive = false;
    m_imgAlive = std::make_shared<std::atomic<bool>>(true);

    addSection("Movies",   m_movies);
    addSection("Episodes", m_episodes);
    addSection("TV Shows", m_shows);
    addSection("Artists",  m_artists);
    addSection("Albums",   m_albums);
    addSection("Tracks",   m_tracks);
    // Last, like the Live TV rails on Home: schedule matches are a
    // different kind of answer from what is in the library.
    addSection("Live TV",  m_liveTV);

    if (m_resultsContent->getChildren().empty()) {
        auto* empty = new brls::Label();
        empty->setText(m_query.empty() ? "Type to search your library"
                                       : "No results for \"" + m_query + "\"");
        empty->setFontSize(15);
        empty->setTextColor(spal::muted());
        m_resultsContent->addView(empty);
    }
}

void SearchTab::addSection(const std::string& title, const std::vector<MediaItem>& items) {
    if (items.empty()) return;

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setAlignItems(brls::AlignItems::STRETCH);   // header + row fill the width
    section->setMarginBottom(18);

    // Header: title + count.
    auto* head = new brls::Box();
    head->setAxis(brls::Axis::ROW);
    head->setAlignItems(brls::AlignItems::CENTER);
    head->setMarginBottom(11);
    const bool phone = (m_layout != Layout::TwoColumn);
    if (phone) head->setPadding(0, 16, 0, 16);
    auto* h2 = new brls::Label();
    h2->setText(title);
    h2->setFontSize(phone ? 17.0f : 16.0f);
    h2->setTextColor(spal::h2());
    h2->setMarginRight(9);
    head->addView(h2);
    auto* ct = new brls::Label();
    ct->setText(std::to_string(items.size()) + (items.size() == 1 ? " result" : " results"));
    ct->setFontSize(12);
    ct->setTextColor(spal::muted());
    head->addView(ct);
    section->addView(head);

    // One horizontal scrolling carousel of cards, like the home screen. The row
    // height fits this type's poster + labels; HorizontalScrollRow handles
    // LEFT/RIGHT + hands focus back to the keyboard / next section at its edges.
    int posterH = 140;                       // portrait (movies / shows)
    switch (items[0].mediaType) {
        case MediaType::EPISODE:
        case MediaType::CLIP:          posterH = 84; break;   // 16:9
        case MediaType::MUSIC_ALBUM:
        case MediaType::MUSIC_TRACK:
        case MediaType::MUSIC_ARTIST:  posterH = 96; break;   // square
        default: break;
    }
    auto* row = new HorizontalScrollRow();
    row->setHeight((float)(posterH + 44));
    for (const auto& it : items) {
        auto* card = makeCard(it);
        card->setMarginRight(14);
        row->addView(card);
    }
    section->addView(row);
    m_resultsContent->addView(section);
}

brls::Box* SearchTab::makeCard(const MediaItem& item) {
    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setCornerRadius(6);
    card->setFocusable(true);

    // Poster aspect (and width) match the media type so square covers and
    // landscape stills aren't cropped into a portrait frame. Episodes get a
    // larger 16:9 card; movies/shows portrait, albums/tracks/artists square.
    // Phone cards run larger than the desktop's — a 96px poster is unreadable
    // at arm's length on a handset. B is a touch smaller than A because its
    // keyboard dock takes the bottom of the screen. Thumb requests stay at 2x
    // the drawn size.
    const bool phoneA = (m_layout == Layout::NativeIme);
    const bool phoneB = (m_layout == Layout::OnScreen);
    int cw = 96, ph = 140, rw = 192, rh = 280;     // portrait default (movies / shows)
    if (phoneA)      { cw = 116; ph = 170; rw = 232; rh = 340; }
    else if (phoneB) { cw = 104; ph = 152; rw = 208; rh = 304; }
    switch (item.mediaType) {
        case MediaType::EPISODE:
        case MediaType::CLIP:
            cw = 150; ph = 84;  rw = 300; rh = 168; // bigger 16:9 still
            if (phoneA)      { cw = 190; ph = 107; rw = 380; rh = 214; }
            else if (phoneB) { cw = 172; ph = 97;  rw = 344; rh = 194; }
            break;
        case MediaType::MUSIC_ALBUM:
        case MediaType::MUSIC_TRACK:
        case MediaType::MUSIC_ARTIST:
            cw = 96;  ph = 96;  rw = 192; rh = 192; // square cover
            if (phoneA)      { cw = 116; ph = 116; rw = 232; rh = 232; }
            else if (phoneB) { cw = 104; ph = 104; rw = 208; rh = 208; }
            break;
        default:
            break;
    }
    card->setWidth((float)cw);

    auto* poster = new brls::Box();
    poster->setWidth((float)cw);
    poster->setHeight((float)ph);
    poster->setCornerRadius(6);
    poster->setBackgroundColor(spal::poster());

    auto* img = new brls::Image();
    img->setPositionType(brls::PositionType::ABSOLUTE);
    img->setPositionTop(0);
    img->setPositionLeft(0);
    img->setPositionRight(0);
    img->setHeight((float)ph);
    img->setCornerRadius(6);
    img->setScalingType(brls::ImageScalingType::FILL);
    img->setVisibility(brls::Visibility::INVISIBLE);
    poster->addView(img);

    if (!item.thumb.empty()) {
        std::string url = PlexClient::getInstance().getThumbnailUrl(item.thumb, rw, rh);
        ImageLoader::loadAsync(url, [](brls::Image* im) {
            if (im) im->setVisibility(brls::Visibility::VISIBLE);
        }, img, m_imgAlive);
    }

    // Watched badge (amber circle + ink check), top-right.
    if (item.watched) {
        auto* badge = new brls::Box();
        badge->setPositionType(brls::PositionType::ABSOLUTE);
        badge->setPositionTop(5);
        badge->setPositionRight(5);
        badge->setWidth((phoneA || phoneB) ? 20.0f : 18.0f);
        badge->setHeight((phoneA || phoneB) ? 20.0f : 18.0f);
        badge->setCornerRadius(9);
        badge->setBackgroundColor(spal::amber());
        badge->setJustifyContent(brls::JustifyContent::CENTER);
        badge->setAlignItems(brls::AlignItems::CENTER);
        auto* chk = new brls::Image();
        chk->setImageFromRes("icons/search-check.png");
        chk->setWidth((phoneA || phoneB) ? 12.0f : 11.0f);
        chk->setHeight((phoneA || phoneB) ? 12.0f : 11.0f);
        chk->setScalingType(brls::ImageScalingType::FIT);
        badge->addView(chk);
        poster->addView(badge);
    }
    card->addView(poster);

    // Title (single line, ellipsised).
    auto* cap = new brls::Label();
    cap->setText(cardTitle(item));
    cap->setFontSize((phoneA || phoneB) ? 13.0f : 12.0f);
    cap->setTextColor(spal::cap());
    cap->setSingleLine(true);
    cap->setWidth((float)cw);
    cap->setMarginTop(6);
    card->addView(cap);

    std::string sub = cardSub(item);
    if (!sub.empty()) {
        auto* s = new brls::Label();
        s->setText(sub);
        s->setFontSize((phoneA || phoneB) ? 11.5f : 11.0f);
        s->setTextColor(spal::muted());
        s->setSingleLine(true);
        s->setWidth((float)cw);
        card->addView(s);
    }

    MediaItem captured = item;
    card->registerClickAction([this, captured](brls::View*) { onItemSelected(captured); return true; });
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));
    wireContextMenu(card, item);

    return card;
}

void SearchTab::onItemSelected(const MediaItem& item) {
    // Live TV results carry an EPG ratingKey, which /library/metadata 404s
    // on — opening the detail view gives an empty page and playing fails.
    // The guide's choice applies here too: watch it now, or record it.
    if (item.isLiveTV) {
        showLiveTVProgramMenu(item);
        return;
    }

    // Tracks follow the default track action; everything else opens detail.
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        MediaDetailView::performTrackActionStatic(item);
        return;
    }
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

SearchTab::~SearchTab() {
    if (m_alive) *m_alive = false;
    if (m_imgAlive) *m_imgAlive = false;
    if (m_inputManager && m_kbSubscribed)
        m_inputManager->getKeyboardKeyStateChanged()->unsubscribe(m_kbSub);
}

void SearchTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    if (m_imgAlive) *m_imgAlive = false;
    m_loadGeneration++;
    ImageLoader::cancelAll();
    ImageLoader::clearCache();
}

void SearchTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);
    // Land on the keyboard so the user can start typing immediately.
    if (m_keyboardFirstKey) brls::Application::giveFocus(m_keyboardFirstKey);
}

} // namespace vitaplex
