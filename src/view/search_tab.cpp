/**
 * VitaPlex - Search Tab implementation
 * On-screen keyboard (left, 300px) + type-grouped result grids (right).
 */

#include "view/search_tab.hpp"
#include "view/media_detail_view.hpp"
#include "view/long_press_gesture.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include "platform/platform.hpp"

#include <atomic>

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
brls::Box* makeKeyBox(int height) {
    auto* key = new brls::Box();
    key->setHeight((float)height);
    key->setGrow(1.0f);                 // equal columns within the row
    key->setJustifyContent(brls::JustifyContent::CENTER);
    key->setAlignItems(brls::AlignItems::CENTER);
    key->setCornerRadius(6);
    key->setBackgroundColor(spal::key());
    key->setBorderColor(spal::keyLine());
    key->setBorderThickness(1.0f);
    key->setFocusable(true);
    return key;
}

brls::Box* makeCharKey(const std::string& ch) {
    auto* key = makeKeyBox(30);
    auto* lbl = new brls::Label();
    lbl->setText(ch);
    lbl->setFontSize(14);
    lbl->setTextColor(spal::keyText());
    key->addView(lbl);
    return key;
}

brls::Box* makeSpecialKey(const std::string& iconRes) {
    auto* key = makeKeyBox(34);
    auto* icn = new brls::Image();
    icn->setImageFromRes(iconRes);
    icn->setWidth(16);
    icn->setHeight(16);
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

// START / long-press context menu wiring, matching the previous row behaviour.
void wireContextMenu(brls::View* cell, const MediaItem& item) {
    auto open = [item]() {
        switch (item.mediaType) {
            case MediaType::MOVIE:        MediaDetailView::showMovieContextMenuStatic(item);  break;
            case MediaType::SHOW:         MediaDetailView::showShowContextMenuStatic(item);   break;
            case MediaType::SEASON:       MediaDetailView::showSeasonContextMenuStatic(item); break;
            case MediaType::EPISODE:      MediaDetailView::showEpisodeContextMenu(item);      break;
            case MediaType::MUSIC_ARTIST: MediaDetailView::showArtistContextMenuStatic(item); break;
            case MediaType::MUSIC_ALBUM:  MediaDetailView::showAlbumContextMenuStatic(item);  break;
            case MediaType::MUSIC_TRACK:  MediaDetailView::showTrackContextMenuStatic(item);  break;
            default: break;
        }
    };
    cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                         [open](brls::View*) { open(); return true; });
    cell->addGestureRecognizer(new LongPressGestureRecognizer(
        cell, [open](LongPressGestureStatus status) {
            if (status.state == brls::GestureState::START) open();
        }));
}

} // namespace

SearchTab::SearchTab() {
    this->setAxis(brls::Axis::ROW);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);
    this->setBackgroundColor(spal::bg());

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
    m_resultsContent->setAlignItems(brls::AlignItems::FLEX_START);
    m_resultsContent->setPadding(24, 18, 0, 8);

    m_resultsScroll->setContentView(m_resultsContent);
    this->addView(m_resultsScroll);
}

void SearchTab::buildKeyboard(brls::Box* parent) {
    auto* kb = new brls::Box();
    kb->setAxis(brls::Axis::COLUMN);
    kb->setAlignItems(brls::AlignItems::STRETCH);   // rows fill the column width
    kb->setMarginTop(14);

    std::vector<std::vector<brls::Box*>> grid;   // [row][col] for nav wiring

    // Special row: Clear, Backspace, Space, Search.
    auto* sprow = new brls::Box();
    sprow->setAxis(brls::Axis::ROW);
    sprow->setMarginBottom(7);
    std::vector<brls::Box*> srowKeys;
    struct Spec { const char* icon; int act; };           // 0 clear,1 bksp,2 space,3 search
    const Spec specials[4] = {
        {"icons/delete-outline.png",    0},
        {"icons/backspace-outline.png", 1},
        {"icons/keyboard-space.png",    2},
        {"icons/magnify.png",           3},
    };
    for (int i = 0; i < 4; i++) {
        auto* key = makeSpecialKey(specials[i].icon);
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
            auto* key = makeCharKey(ch);
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
void SearchTab::updateField()                    { if (m_queryLabel) m_queryLabel->setText(m_query); }

void SearchTab::performSearch() {
    if (m_query.empty()) {
        m_movies.clear(); m_episodes.clear(); m_shows.clear();
        m_artists.clear(); m_albums.clear(); m_tracks.clear();
        rebuildResults();
        return;
    }

    int gen = ++m_loadGeneration;
    std::string q = m_query;
    asyncRun([this, q, gen, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> results;
        bool ok = client.search(q, results);

        brls::sync([this, ok, results, gen, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            if (gen != m_loadGeneration) return;   // stale

            m_movies.clear(); m_episodes.clear(); m_shows.clear();
            m_artists.clear(); m_albums.clear(); m_tracks.clear();
            if (ok) {
                for (const auto& it : results) {
                    switch (it.mediaType) {
                        case MediaType::MOVIE:        m_movies.push_back(it);   break;
                        case MediaType::EPISODE:      m_episodes.push_back(it); break;
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
    section->setAlignItems(brls::AlignItems::FLEX_START);
    section->setMarginBottom(18);

    // Header: title + count.
    auto* head = new brls::Box();
    head->setAxis(brls::Axis::ROW);
    head->setAlignItems(brls::AlignItems::CENTER);
    head->setMarginBottom(11);
    auto* h2 = new brls::Label();
    h2->setText(title);
    h2->setFontSize(16);
    h2->setTextColor(spal::h2());
    h2->setMarginRight(9);
    head->addView(h2);
    auto* ct = new brls::Label();
    ct->setText(std::to_string(items.size()) + (items.size() == 1 ? " result" : " results"));
    ct->setFontSize(12);
    ct->setTextColor(spal::muted());
    head->addView(ct);
    section->addView(head);

    // Grid: 5 columns, column-gap 16, row-gap 14. Built as a column of rows so
    // borealis' box navigation handles LEFT/RIGHT within a row and UP/DOWN
    // between rows / sections.
    auto* grid = new brls::Box();
    grid->setAxis(brls::Axis::COLUMN);
    grid->setAlignItems(brls::AlignItems::FLEX_START);
    const int COLS = 5;
    brls::Box* row = nullptr;
    for (size_t i = 0; i < items.size(); i++) {
        if (i % COLS == 0) {
            row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            if (i > 0) row->setMarginTop(14);
            grid->addView(row);
        }
        auto* card = makeCard(items[i]);
        if (i % COLS != 0) card->setMarginLeft(16);
        row->addView(card);
    }
    section->addView(grid);
    m_resultsContent->addView(section);
}

brls::Box* SearchTab::makeCard(const MediaItem& item) {
    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setWidth(96);
    card->setCornerRadius(6);
    card->setFocusable(true);

    // Poster (96x140), placeholder behind the async image.
    auto* poster = new brls::Box();
    poster->setWidth(96);
    poster->setHeight(140);
    poster->setCornerRadius(6);
    poster->setBackgroundColor(spal::poster());

    auto* img = new brls::Image();
    img->setPositionType(brls::PositionType::ABSOLUTE);
    img->setPositionTop(0);
    img->setPositionLeft(0);
    img->setPositionRight(0);
    img->setHeight(140);
    img->setCornerRadius(6);
    img->setScalingType(brls::ImageScalingType::FILL);
    img->setVisibility(brls::Visibility::INVISIBLE);
    poster->addView(img);

    // Episodes use the show poster (portrait) rather than the landscape still.
    std::string thumb = item.thumb;
    if (item.mediaType == MediaType::EPISODE && !item.grandparentThumb.empty())
        thumb = item.grandparentThumb;
    if (!thumb.empty()) {
        std::string url = PlexClient::getInstance().getThumbnailUrl(thumb, 192, 280);
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
        badge->setWidth(18);
        badge->setHeight(18);
        badge->setCornerRadius(9);
        badge->setBackgroundColor(spal::amber());
        badge->setJustifyContent(brls::JustifyContent::CENTER);
        badge->setAlignItems(brls::AlignItems::CENTER);
        auto* chk = new brls::Image();
        chk->setImageFromRes("icons/search-check.png");
        chk->setWidth(11);
        chk->setHeight(11);
        chk->setScalingType(brls::ImageScalingType::FIT);
        badge->addView(chk);
        poster->addView(badge);
    }
    card->addView(poster);

    // Title (single line, ellipsised).
    auto* cap = new brls::Label();
    cap->setText(cardTitle(item));
    cap->setFontSize(12);
    cap->setTextColor(spal::cap());
    cap->setSingleLine(true);
    cap->setWidth(96);
    cap->setMarginTop(6);
    card->addView(cap);

    std::string sub = cardSub(item);
    if (!sub.empty()) {
        auto* s = new brls::Label();
        s->setText(sub);
        s->setFontSize(11);
        s->setTextColor(spal::muted());
        s->setSingleLine(true);
        s->setWidth(96);
        card->addView(s);
    }

    MediaItem captured = item;
    card->registerClickAction([this, captured](brls::View*) { onItemSelected(captured); return true; });
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));
    wireContextMenu(card, item);

    return card;
}

void SearchTab::onItemSelected(const MediaItem& item) {
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
