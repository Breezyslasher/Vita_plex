/**
 * VitaPlex - Library Section Tab implementation
 */

#include "view/library_section_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "view/filter_chip.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/plex_palette.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"
#include "app/music_queue.hpp"
#include "app/downloads_manager.hpp"
#include "platform/platform.hpp"
#include <cctype>

namespace vitaplex {

// A-Z jump-rail buckets. Parallel to m_azLetters; '#' collects digits/symbols
// (which Plex titleSort places before 'A').
static const char* kAzLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";

size_t LibrarySectionTab::libraryPageSize() {
    int v = platform::getImageConstraints().libraryPageSize;
    return v > 0 ? static_cast<size_t>(v) : 60;
}

size_t LibrarySectionTab::playlistTrackPageSize() {
    int v = platform::getImageConstraints().playlistTrackPageSize;
    return v > 0 ? static_cast<size_t>(v) : 50;
}

// Remembers each library section's chosen sort for the session, keyed by section
// key, so navigating away and back (which recreates the tab) keeps the user's
// sort instead of snapping to the default. Value = {sortParam, sortLabel}.
static std::map<std::string, std::pair<std::string, std::string>> s_sortBySection;

LibrarySectionTab::LibrarySectionTab(const std::string& sectionKey, const std::string& title, const std::string& sectionType)
    : m_sectionKey(sectionKey), m_title(title), m_sectionType(sectionType) {

    // Create alive flag for async callback safety
    m_alive = std::make_shared<bool>(true);

    // Restore this section's remembered sort (if the user picked one before).
    // Must happen before the Sort chip is built and before loadContent() reads
    // m_sortParam, so both reflect the saved choice.
    auto savedSort = s_sortBySection.find(sectionKey);
    if (savedSort != s_sortBySection.end()) {
        m_sortParam = savedSort->second.first;
        m_sortLabel = savedSort->second.second;
    }

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title row — page title on the left, total-count on the right.
    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::FLEX_END);
    titleRow->setMarginBottom(15);

    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(28);
    titleRow->addView(m_titleLabel);

    auto* titleSpacer = new brls::Box();
    titleSpacer->setGrow(1.0f);
    titleRow->addView(titleSpacer);

    m_countLabel = new brls::Label();
    m_countLabel->setFontSize(13);
    m_countLabel->setTextColor(nvgRGB(0x8a, 0x8a, 0x90));
    m_countLabel->setMarginBottom(5);   // sit roughly on the title's baseline
    titleRow->addView(m_countLabel);

    this->addView(titleRow);

    const auto& settings = Application::getInstance().getSettings();

    // View mode selector (Collections / Categories / Filters / Sort)
    m_viewModeBox = new brls::Box();
    m_viewModeBox->setAxis(brls::Axis::ROW);
    m_viewModeBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_viewModeBox->setAlignItems(brls::AlignItems::CENTER);
    m_viewModeBox->setMarginBottom(15);

    // Collections button (only show if enabled)
    if (settings.showCollections) {
        m_collectionsBtn = new vitaplex::FilterChip();
        m_collectionsBtn->setText("Collections");
        m_collectionsBtn->setMarginRight(10);
        styleButton(m_collectionsBtn, false);
        m_collectionsBtn->registerClickAction([this](brls::View* view) {
            showCollections();
            return true;
        });
        m_viewModeBox->addView(m_collectionsBtn);
    }

    // Categories button (genre browse). Movie / show / music sections instead
    // get the inline Filters menu below, which filters the grid by genre in
    // place rather than opening a separate genre-cards browse mode.
    if (settings.showGenres && sectionType != "movie" && sectionType != "show" &&
        sectionType != "artist") {
        m_categoriesBtn = new vitaplex::FilterChip();
        m_categoriesBtn->setText("Categories");
        m_categoriesBtn->setMarginRight(10);
        styleButton(m_categoriesBtn, false);
        m_categoriesBtn->registerClickAction([this](brls::View* view) {
            showCategories();
            return true;
        });
        m_viewModeBox->addView(m_categoriesBtn);
    }

    // Playlists button (only for music/artist sections)
    if (settings.showPlaylists && sectionType == "artist") {
        m_playlistsBtn = new vitaplex::FilterChip();
        m_playlistsBtn->setText("Playlists");
        m_playlistsBtn->setMarginRight(10);
        styleButton(m_playlistsBtn, false);
        m_playlistsBtn->registerClickAction([this](brls::View* view) {
            showPlaylists();
            return true;
        });
        m_viewModeBox->addView(m_playlistsBtn);
    }

    // ── Direction-A discovery controls ──
    // Filters chip (movie / show / music): opens an inline filter menu. Active
    // filters turn the chip gold, bump a count badge, and appear as removable
    // chips to its right (matching the Movies reference toolbar). The available
    // fields differ by section type (see filterFieldsFor).
    if (sectionType == "movie" || sectionType == "show" || sectionType == "artist") {
        m_filtersBtn = new vitaplex::FilterChip();
        m_filtersBtn->setText("Filters");
        m_filtersBtn->setMarginRight(10);
        styleButton(m_filtersBtn, false);
        m_filtersBtn->registerClickAction([this](brls::View*) {
            showFilterMenu();
            return true;
        });

        // Count badge pinned to the chip's top-right corner. Absolute position
        // keeps it out of the button's flex flow (so it doesn't widen the
        // chip); hidden until at least one filter is active. Dark fill + gold
        // text so it reads against the gold "active" chip beneath it.
        m_filtersBadge = new brls::Box();
        m_filtersBadge->setAxis(brls::Axis::ROW);
        m_filtersBadge->setJustifyContent(brls::JustifyContent::CENTER);
        m_filtersBadge->setAlignItems(brls::AlignItems::CENTER);
        m_filtersBadge->setHeight(18);
        m_filtersBadge->setMinWidth(18);
        m_filtersBadge->setCornerRadius(9);
        m_filtersBadge->setPaddingLeft(5);
        m_filtersBadge->setPaddingRight(5);
        m_filtersBadge->setBackgroundColor(vitaplex::palette::goldInk);
        m_filtersBadge->setPositionType(brls::PositionType::ABSOLUTE);
        m_filtersBadge->setPositionTop(-7);
        m_filtersBadge->setPositionRight(-7);
        m_filtersBadge->setVisibility(brls::Visibility::GONE);
        m_filtersBadgeLabel = new brls::Label();
        m_filtersBadgeLabel->setText("0");
        m_filtersBadgeLabel->setFontSize(12);
        m_filtersBadgeLabel->setTextColor(vitaplex::palette::gold);
        m_filtersBadge->addView(m_filtersBadgeLabel);
        m_filtersBtn->addView(m_filtersBadge);

        m_viewModeBox->addView(m_filtersBtn);

        // Removable applied-filter chips ("Action", "2010s", …) live in their
        // own row box immediately to the right of the Filters chip.
        m_appliedFiltersBox = new brls::Box();
        m_appliedFiltersBox->setAxis(brls::Axis::ROW);
        m_appliedFiltersBox->setAlignItems(brls::AlignItems::CENTER);
        m_viewModeBox->addView(m_appliedFiltersBox);
    }

    // (Unwatched moved into the Filters menu as a "Watch Status" entry.)

    // Flex spacer pushes the Sort chip to the right edge of the toolbar.
    m_toolbarSpacer = new brls::Box();
    m_toolbarSpacer->setGrow(1.0f);
    m_viewModeBox->addView(m_toolbarSpacer);

    // Sort chip — neutral (never "picked"); opens the sort menu.
    m_sortBtn = new vitaplex::FilterChip();
    m_sortBtn->setText("Sort: " + m_sortLabel);
    styleButton(m_sortBtn, false);
    m_sortBtn->registerClickAction([this](brls::View*) {
        showSortMenu();
        return true;
    });
    m_viewModeBox->addView(m_sortBtn);

    // Back button (hidden by default, shown in filtered view)
    m_backBtn = new brls::Button();
    m_backBtn->setText("< Back");
    m_backBtn->setVisibility(brls::Visibility::GONE);
    styleButton(m_backBtn, false);
    m_backBtn->registerClickAction([this](brls::View* view) {
        // Move focus off the back button and defer navigateBack() to the
        // next frame so the click event (action loop walk-up + click
        // animation) fully completes before we start destroying cells
        // and hiding views. Without the defer + focus pre-transfer,
        // mid-dispatch the click loop on m_backBtn could walk into a
        // hidden subtree as showPlaylists()/showCategories()/etc.
        // flipped visibility and rebuildGrid freed the old cells.
        brls::View* target = nullptr;
        if (m_trackListScroll &&
            m_trackListScroll->getVisibility() == brls::Visibility::VISIBLE) {
            target = m_trackListScroll;
        } else if (m_contentGrid) {
            target = m_contentGrid;
        }
        if (target) brls::Application::giveFocus(target);
        brls::sync([this]() { navigateBack(); });
        return true;
    });
    m_viewModeBox->addView(m_backBtn);

    this->addView(m_viewModeBox);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    // Infinite scroll: load next page when user scrolls to bottom
    m_contentGrid->setOnLoadMore([this]() {
        loadNextPage();
    });
    m_contentGrid->setOnItemStartAction([this](const MediaItem& item) {
        if (item.type == "playlist") {
            // Find the matching Playlist struct for full metadata
            for (const auto& playlist : m_playlists) {
                if (playlist.ratingKey == item.ratingKey) {
                    showPlaylistContextMenu(playlist);
                    return;
                }
            }
        }
    });
    this->addView(m_contentGrid);

    // Track list view (for playlist contents - hidden by default)
    m_trackListScroll = new brls::ScrollingFrame();
    m_trackListScroll->setGrow(1.0f);
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_trackListBox = new brls::Box();
    m_trackListBox->setAxis(brls::Axis::COLUMN);
    m_trackListBox->setPadding(5);
    m_trackListScroll->setContentView(m_trackListBox);
    this->addView(m_trackListScroll);

    // Controller B / remote back / keyboard Esc all route through
    // navigateBack(). Registered on m_contentGrid AND m_trackListScroll
    // — between the two, the focused view (a grid cell OR a playlist
    // track row) always has the handler in its focus walk-up chain.
    // Can't put the handler on `this` (LibrarySectionTab) because
    // TabFrame::onContentChanged registers its own BUTTON_B on the
    // active tab's content view every time the tab is selected, and
    // View::registerAction REPLACES any existing handler for the same
    // button (view.cpp:770-774) — so any handler on `this` would be
    // silently wiped and B would fall through to TabFrame's default
    // (give focus to the sidebar). Child views are out of TabFrame's
    // reach.
    //
    // Two-step back: first park focus on a view that won't get hidden
    // by the navigation about to run, THEN defer navigateBack one frame
    // so the action-dispatch loop fully completes before we destroy
    // cells / flip visibility.
    //
    // Without the focus park, leaving a playlist still crashed even
    // with the defer: focus stayed on a track row, showPlaylists() hid
    // m_trackListScroll, and the next input event walked into a focused
    // view buried in a Visibility::GONE subtree. m_backBtn is the right
    // park target — it's always visible in FILTERED view (the only mode
    // that's reachable here) and we hide it inside updateViewModeButtons
    // where the existing "focused button about to hide" path catches
    // it and transfers focus onto the freshly-populated content area.
    auto backHandler = [this](brls::View*) {
        // At the TOP of the library (ALL_ITEMS) we have nothing to pop —
        // returning false here lets borealis fall through to the
        // MainActivity root handler that brings up the music player when
        // a queue is active. Previously we always returned true, which
        // consumed BUTTON_B and made the player only reachable from the
        // sidebar (where the library's handler isn't in the focus walk).
        if (m_viewMode == LibraryViewMode::ALL_ITEMS) {
            return false;
        }
        if (m_backBtn &&
            m_backBtn->getVisibility() == brls::Visibility::VISIBLE) {
            brls::Application::giveFocus(m_backBtn);
        }
        brls::sync([this]() { navigateBack(); });
        return true;
    };
    m_contentGrid->registerAction("Back", brls::ControllerButton::BUTTON_B, backHandler);
    m_trackListScroll->registerAction("Back", brls::ControllerButton::BUTTON_B, backHandler);
    // Also on the toolbar row, so Back works while a chip (Filters / Sort /
    // Collections / Playlists / a filter chip) is focused — its walk-up reaches
    // m_viewModeBox, whereas the grid/track-list handlers are not in that chain.
    // m_viewModeBox is a child of `this`, so TabFrame's BUTTON_B override (which
    // only lands on the tab's content view) doesn't wipe it.
    m_viewModeBox->registerAction("Back", brls::ControllerButton::BUTTON_B, backHandler);

    // A-Z jump rail (absolute, right edge) — starts hidden; refreshAzRail shows
    // it only while the all-items grid is sorted Title A-Z.
    buildAzRail();

    // Load content immediately
    brls::Logger::debug("LibrarySectionTab: Created for section {} ({}) type={}", m_sectionKey, m_title, m_sectionType);
    loadContent();
}

LibrarySectionTab::~LibrarySectionTab() {
    // Mark as no longer alive to prevent async callbacks from updating destroyed UI
    if (m_alive) {
        *m_alive = false;
    }
    brls::Logger::debug("LibrarySectionTab: Destroyed for section {}", m_sectionKey);
}

void LibrarySectionTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
    // Free image cache when leaving tab to reclaim memory
    ImageLoader::clearCache();
}

void LibrarySectionTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadContent();
    }
}

void LibrarySectionTab::loadContent() {
    brls::Logger::debug("LibrarySectionTab::loadContent - section: {} (async)", m_sectionKey);

    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;  // Capture weak_ptr for async safety

    m_pageOffset = 0;
    m_totalItemCount = 0;

    std::string sectionType = m_sectionType;
    std::string params = buildListParams();   // current sort + filter fragment
    asyncRun([this, key, sectionType, params, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;
        int totalCount = 0;

        // Plex type codes: 1=movie, 2=show, 8=artist, 9=album, 10=track
        int metadataType = 0;
        if (sectionType == "movie") metadataType = 1;
        else if (sectionType == "show") metadataType = 2;
        else if (sectionType == "artist") metadataType = 8;

        if (client.fetchLibraryContent(key, items, metadataType, libraryPageSize(), 0, &totalCount, params)) {
            brls::Logger::info("LibrarySectionTab: Got {} of {} items for section {}", items.size(), totalCount, key);

            // Trim heavy fields to reduce per-item memory in large libraries
            for (auto& item : items) {
                item.trimForGrid();
            }

            brls::sync([this, items, totalCount, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) {
                    brls::Logger::debug("LibrarySectionTab: Tab destroyed, skipping UI update");
                    return;
                }

                m_items = items;
                m_pageOffset = items.size();
                m_totalItemCount = totalCount;

                if (m_viewMode == LibraryViewMode::ALL_ITEMS) {
                    m_contentGrid->setDataSource(m_items);
                    m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
                }
                updateCountLabel();
                refreshAzRail();
                m_loaded = true;
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load content for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });

    // Preload collections and genres for quick switching
    const auto& settings = Application::getInstance().getSettings();
    if (settings.showCollections) {
        loadCollections();
    }
    if (settings.showGenres) {
        loadGenres();
    }
    if (settings.showPlaylists && m_sectionType == "artist") {
        loadPlaylists();
    }
}

void LibrarySectionTab::loadCollections() {
    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> collections;

        if (client.fetchCollections(key, collections)) {
            brls::Logger::info("LibrarySectionTab: Got {} collections for section {}", collections.size(), key);

            brls::sync([this, collections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_collections = collections;
                m_collectionsLoaded = true;

                // Hide collections button if none available
                if (m_collections.empty() && m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: No collections for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_collectionsLoaded = true;
                if (m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibrarySectionTab::loadGenres() {
    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<GenreItem> genres;

        if (client.fetchGenreItems(key, genres) && !genres.empty()) {
            brls::Logger::info("LibrarySectionTab: Got {} genres for section {}", genres.size(), key);

            brls::sync([this, genres, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_genres = genres;
                m_genresLoaded = true;
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: No genres for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_genresLoaded = true;
                if (m_categoriesBtn) {
                    m_categoriesBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibrarySectionTab::loadNextPage() {
    if (m_pageOffset >= (size_t)m_totalItemCount) {
        m_contentGrid->setHasMore(false);
        return;
    }

    std::string key = m_sectionKey;
    std::string sectionType = m_sectionType;
    size_t offset = m_pageOffset;
    std::string params = buildListParams();   // keep pages consistent with the current sort/filter
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, sectionType, offset, params, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        int metadataType = 0;
        if (sectionType == "movie") metadataType = 1;
        else if (sectionType == "show") metadataType = 2;
        else if (sectionType == "artist") metadataType = 8;

        if (client.fetchLibraryContent(key, items, metadataType, libraryPageSize(), (int)offset, nullptr, params)) {
            for (auto& item : items) {
                item.trimForGrid();
            }

            brls::sync([this, items, offset, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_pageOffset = offset + items.size();
                // Append to our stored items too
                m_items.insert(m_items.end(), items.begin(), items.end());
                m_contentGrid->appendItems(items);
                m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
            });
        } else {
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_contentGrid->setHasMore(false);
            });
        }
    });
}

std::string LibrarySectionTab::buildListParams() const {
    std::string p = "&sort=" + m_sortParam;
    // Each active filter contributes its selected values. Plex semantics:
    //   OR  -> one param, comma-joined:   &genre=1,2,3   (match any)
    //   AND -> the param repeated:        &genre=1&genre=2  (match all)
    // Values are URL-encoded (content ratings etc. can contain spaces); the
    // comma separators stay literal. Watch Status rides in as field "unwatched".
    for (const auto& kv : m_activeFilters) {
        const std::string& field = kv.first;
        const ActiveFilter& af = kv.second;
        if (af.values.empty()) continue;
        if (af.andMode) {
            for (const auto& v : af.values)
                p += "&" + field + "=" + HttpClient::urlEncode(v.first);
        } else {
            p += "&" + field + "=";
            for (size_t i = 0; i < af.values.size(); i++) {
                if (i) p += ",";
                p += HttpClient::urlEncode(af.values[i].first);
            }
        }
    }
    return p;
}

// Re-query the all-items grid from offset 0 with the current sort + filter.
// Mirrors loadContent's item fetch but skips re-preloading collections/genres.
void LibrarySectionTab::reloadAllItems() {
    m_viewMode = LibraryViewMode::ALL_ITEMS;
    m_titleLabel->setText(m_title);
    if (m_trackListScroll) m_trackListScroll->setVisibility(brls::Visibility::GONE);
    if (m_contentGrid) m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
    updateViewModeButtons();

    m_pageOffset = 0;
    m_totalItemCount = 0;

    std::string key = m_sectionKey;
    std::string sectionType = m_sectionType;
    std::string params = buildListParams();
    std::weak_ptr<bool> aliveWeak = m_alive;
    asyncRun([this, key, sectionType, params, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;
        int totalCount = 0;
        int metadataType = 0;
        if (sectionType == "movie") metadataType = 1;
        else if (sectionType == "show") metadataType = 2;
        else if (sectionType == "artist") metadataType = 8;

        if (client.fetchLibraryContent(key, items, metadataType, libraryPageSize(), 0, &totalCount, params)) {
            for (auto& item : items) item.trimForGrid();
            brls::sync([this, items, totalCount, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_items = items;
                m_pageOffset = items.size();
                m_totalItemCount = totalCount;
                if (m_viewMode == LibraryViewMode::ALL_ITEMS) {
                    m_contentGrid->setDataSource(m_items);
                    m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
                }
                m_currentAzLetter = 0;   // fresh result set — clear the rail highlight
                updateCountLabel();
                refreshAzRail();
            });
        }
    });
}

void LibrarySectionTab::showSortMenu() {
    // Each sort field offered in both directions (ascending + descending), so
    // the direction is an explicit, one-tap choice.
    struct SortOpt { const char* label; const char* param; };
    static const SortOpt kOpts[] = {
        {"Title A-Z",       "titleSort:asc"},
        {"Title Z-A",       "titleSort:desc"},
        {"Newest Added",    "addedAt:desc"},
        {"Oldest Added",    "addedAt:asc"},
        {"Newest Release",  "originallyAvailableAt:desc"},
        {"Oldest Release",  "originallyAvailableAt:asc"},
        {"Highest Rated",   "rating:desc"},
        {"Lowest Rated",    "rating:asc"},
    };

    std::vector<OptionRow> rows;
    for (const auto& o : kOpts) {
        std::string label = o.label;
        std::string param = o.param;
        bool current = (m_sortParam == param);
        // check-circle marks the active sort, neutral label — same selection
        // cue as the Filters dialog.
        rows.push_back({ current ? "check-circle.png" : "",
                         label, "", false, false,
            [this, label, param](brls::View*) {
                m_sortParam = param;
                m_sortLabel = label;
                s_sortBySection[m_sectionKey] = { param, label };  // remember for this section
                if (m_sortBtn) m_sortBtn->setText("Sort: " + label);
                reloadAllItems();
                return true;
            }});
    }
    // Centered, audio-picker style — matches the Filters dialog (global toolbar
    // actions center; item context menus stay anchored to their item).
    MediaDetailView::showCenteredChoice("Sort by", "", std::move(rows), /*scrollable=*/true);
}

int LibrarySectionTab::activeFilterCount() const {
    // Count every selected value (one applied chip each), not just the fields.
    int n = 0;
    for (const auto& kv : m_activeFilters) n += static_cast<int>(kv.second.values.size());
    return n;
}

// The filter fields offered per section type — every standard Plex library
// filter except the people fields (director/actor/writer), whose value lists
// run to thousands and the picker has no search yet. {Plex field, display name}.
static const std::vector<std::pair<std::string, std::string>>& filterFieldsFor(
    const std::string& sectionType) {
    // "unwatched" is the Watch Status filter (handled specially — fixed values,
    // no value endpoint). Resolution is movie-only (shows have no section-level
    // resolution filter on Plex).
    static const std::vector<std::pair<std::string, std::string>> kMovie = {
        {"unwatched", "Watch Status"},
        {"genre", "Genre"}, {"year", "Year"}, {"decade", "Decade"},
        {"contentRating", "Content Rating"}, {"resolution", "Resolution"},
        {"audioLanguage", "Audio Language"}, {"subtitleLanguage", "Subtitle Language"},
        {"studio", "Studio"}, {"country", "Country"},
    };
    static const std::vector<std::pair<std::string, std::string>> kShow = {
        {"unwatched", "Watch Status"},
        {"genre", "Genre"}, {"year", "Year"}, {"decade", "Decade"},
        {"contentRating", "Content Rating"},
        {"audioLanguage", "Audio Language"}, {"subtitleLanguage", "Subtitle Language"},
        {"studio", "Studio"}, {"country", "Country"},
    };
    // Music (artist) has its own set — Mood / Style are music-only; no video
    // concepts (resolution, content rating, languages) and no Watch Status.
    // "studio" is the record label here.
    static const std::vector<std::pair<std::string, std::string>> kMusic = {
        {"genre", "Genre"}, {"mood", "Mood"}, {"style", "Style"},
        {"country", "Country"}, {"studio", "Record Label"},
        {"decade", "Decade"}, {"year", "Year"},
    };
    if (sectionType == "show")   return kShow;
    if (sectionType == "artist") return kMusic;
    return kMovie;
}

// Plex returns language names in their native script ("日本語", "中文", "العربية").
// The UI font covers Latin / Cyrillic / Greek but not CJK / Arabic / Thai /
// Indic, so those would render as tofu boxes. For languages in a non-renderable
// script we substitute the English name (keyed off the Plex language code);
// everything else keeps Plex's nice native name. Bundling full CJK fonts would
// be the only way to show the native names, but that's many MB per script.
static std::string languageDisplayName(const std::string& code,
                                       const std::string& nativeTitle) {
    static const std::map<std::string, std::string> kEnglish = {
        {"zh", "Chinese"}, {"zho", "Chinese"}, {"chi", "Chinese"}, {"cmn", "Chinese"},
        {"yue", "Cantonese"},
        {"ja", "Japanese"}, {"jpn", "Japanese"},
        {"ko", "Korean"}, {"kor", "Korean"},
        {"ar", "Arabic"}, {"ara", "Arabic"},
        {"he", "Hebrew"}, {"heb", "Hebrew"}, {"iw", "Hebrew"},
        {"th", "Thai"}, {"tha", "Thai"},
        {"hi", "Hindi"}, {"hin", "Hindi"},
        {"bn", "Bengali"}, {"ben", "Bengali"},
        {"ta", "Tamil"}, {"tam", "Tamil"},
        {"te", "Telugu"}, {"tel", "Telugu"},
        {"kn", "Kannada"}, {"kan", "Kannada"},
        {"ml", "Malayalam"}, {"mal", "Malayalam"},
        {"fa", "Persian"}, {"fas", "Persian"}, {"per", "Persian"},
        {"ur", "Urdu"}, {"urd", "Urdu"},
        {"pa", "Punjabi"}, {"pan", "Punjabi"},
        {"gu", "Gujarati"}, {"guj", "Gujarati"},
        {"mr", "Marathi"}, {"mar", "Marathi"},
        {"am", "Amharic"}, {"amh", "Amharic"},
        {"my", "Burmese"}, {"mya", "Burmese"}, {"bur", "Burmese"},
        {"km", "Khmer"}, {"khm", "Khmer"},
        {"lo", "Lao"}, {"lao", "Lao"},
        {"si", "Sinhala"}, {"sin", "Sinhala"},
        {"ka", "Georgian"}, {"kat", "Georgian"}, {"geo", "Georgian"},
        {"hy", "Armenian"}, {"hye", "Armenian"}, {"arm", "Armenian"},
        {"bo", "Tibetan"}, {"bod", "Tibetan"}, {"tib", "Tibetan"},
        {"ne", "Nepali"}, {"nep", "Nepali"},
        {"ps", "Pashto"}, {"pus", "Pashto"},
        {"ug", "Uyghur"}, {"uig", "Uyghur"},
    };
    // Normalize: lowercase and strip any region/script suffix ("zh-Hans" → "zh").
    std::string c = code;
    for (auto& ch : c) ch = (char)std::tolower((unsigned char)ch);
    size_t dash = c.find('-');
    if (dash != std::string::npos) c = c.substr(0, dash);
    auto it = kEnglish.find(c);
    return it != kEnglish.end() ? it->second : nativeTitle;
}

// Top-level Filters chooser — a centered dialog (audio-picker style) listing
// every filter field with its current value, plus "Clear all". Picking a field
// opens its value picker. showCenteredChoice dismisses itself before running a
// row action, so opening the value picker from a row is safe (no stacked modals).
void LibrarySectionTab::showFilterMenu() {
    const auto& fields = filterFieldsFor(m_sectionType);

    std::vector<OptionRow> rows;
    for (const auto& f : fields) {
        const std::string field = f.first;
        const std::string label = f.second;
        auto it = m_activeFilters.find(field);
        const bool active = (it != m_activeFilters.end() && !it->second.values.empty());
        // Show the selected value(s) joined; the row's trailing label truncates.
        std::string sub = "Any";
        if (active) {
            sub.clear();
            for (size_t i = 0; i < it->second.values.size(); i++) {
                if (i) sub += ", ";
                sub += it->second.values[i].second;
            }
        }
        rows.push_back({ active ? "check-circle.png" : "", label, sub, false, false,
            [this, field, label](brls::View*) {
                openFilterValuePicker(field, label);
                return true;
            }});
    }

    if (activeFilterCount() > 0) {
        rows.push_back({ "cross.png", "Clear all filters", "", false, true,
            [this](brls::View*) {
                m_activeFilters.clear();
                applyFilters();
                return true;
            }});
    }

    // Field list is short and bounded — no scrolling needed here (the long
    // lists are the per-field value pickers).
    MediaDetailView::showCenteredChoice("Filters", "Narrow this library",
                                        std::move(rows), /*scrollable=*/false);
}

// Fetch a field's available values (cached after the first time) and then show
// the value picker. The fetch is async; the dialog appears once it returns.
void LibrarySectionTab::openFilterValuePicker(const std::string& field,
                                              const std::string& fieldLabel) {
    // Watch Status is a boolean Plex filter (no /unwatched value endpoint), so
    // its values are fixed rather than fetched: unwatched=1 unplayed, =0 played.
    if (field == "unwatched") {
        std::vector<GenreItem> vals;
        GenreItem unplayed; unplayed.title = "Unplayed"; unplayed.key = "1";
        GenreItem played;   played.title   = "Played";   played.key   = "0";
        vals.push_back(unplayed);
        vals.push_back(played);
        showFilterValues(field, fieldLabel, vals);
        return;
    }

    auto cached = m_filterValueCache.find(field);
    if (cached != m_filterValueCache.end()) {
        showMultiSelect(field, fieldLabel, cached->second);
        return;
    }

    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;
    asyncRun([this, key, field, fieldLabel, aliveWeak]() {
        std::vector<GenreItem> values;
        PlexClient::getInstance().fetchFilterValues(key, field, values);
        brls::sync([this, field, fieldLabel, values, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            m_filterValueCache[field] = values;
            showMultiSelect(field, fieldLabel, values);
        });
    });
}

// Multi-value picker for a filter field (genre, studio, country, languages, …):
// toggle several values and choose OR/AND matching. Commits the whole set at
// once via the dialog's apply-on-close callback.
void LibrarySectionTab::showMultiSelect(const std::string& field,
                                        const std::string& fieldLabel,
                                        const std::vector<GenreItem>& values) {
    const bool isLanguage = (field == "audioLanguage" || field == "subtitleLanguage");

    std::vector<std::pair<std::string, std::string>> current;
    bool andMode = false;
    auto it = m_activeFilters.find(field);
    if (it != m_activeFilters.end()) {
        current = it->second.values;
        andMode = it->second.andMode;
    }
    auto isSelected = [&current](const std::string& key) {
        for (const auto& v : current) if (v.first == key) return true;
        return false;
    };

    std::vector<MultiSelectItem> items;
    items.reserve(values.size());
    for (const auto& v : values) {
        std::string disp = isLanguage ? languageDisplayName(v.key, v.title) : v.title;
        items.push_back({ v.key, disp, isSelected(v.key) });
    }

    std::string fieldCopy = field;
    MediaDetailView::showMultiSelectFilter(
        fieldLabel, andMode, std::move(items),
        [this, fieldCopy](const std::vector<std::pair<std::string, std::string>>& selected,
                          bool andMode) {
            if (selected.empty()) {
                m_activeFilters.erase(fieldCopy);
            } else {
                ActiveFilter af;
                af.values = selected;
                af.andMode = andMode;
                m_activeFilters[fieldCopy] = af;
            }
            applyFilters();
            // Return to the filter menu (not the grid) so the user can keep
            // picking other filters; they back out of the menu to the grid.
            showFilterMenu();
        });
}

// Centered, scrollable value picker for one filter field. "Any <field>" clears
// it; a leading check marks the active value.
void LibrarySectionTab::showFilterValues(const std::string& field,
                                         const std::string& fieldLabel,
                                         const std::vector<GenreItem>& values) {
    // Single-select path (Watch Status). Sets a one-value ActiveFilter.
    auto it = m_activeFilters.find(field);
    std::string currentKey;
    if (it != m_activeFilters.end() && !it->second.values.empty())
        currentKey = it->second.values[0].first;

    std::vector<OptionRow> rows;
    rows.push_back({ currentKey.empty() ? "check-circle.png" : "",
                     "Any " + fieldLabel, "", false, false,
        [this, field](brls::View*) {
            m_activeFilters.erase(field);
            applyFilters();
            showFilterMenu();   // back to the filter menu, like the multi-select picker
            return true;
        }});

    for (const auto& v : values) {
        std::string vkey   = v.key;
        std::string vtitle = v.title;
        const bool current = (vkey == currentKey);
        rows.push_back({ current ? "check-circle.png" : "", vtitle, "", false, false,
            [this, field, vkey, vtitle](brls::View*) {
                ActiveFilter af;
                af.values.push_back({ vkey, vtitle });
                m_activeFilters[field] = af;
                applyFilters();
                showFilterMenu();
                return true;
            }});
    }

    MediaDetailView::showCenteredChoice(fieldLabel, "", std::move(rows), /*scrollable=*/false);
}

// Push the current filter state into the UI (chip styling, count badge, applied
// chips) and re-query the grid.
void LibrarySectionTab::applyFilters() {
    const int n = activeFilterCount();

    if (m_filtersBtn) {
        styleButton(m_filtersBtn, n > 0);   // gold when any filter is active
    }
    if (m_filtersBadge && m_filtersBadgeLabel) {
        if (n > 0) {
            m_filtersBadgeLabel->setText(std::to_string(n));
            m_filtersBadge->setVisibility(brls::Visibility::VISIBLE);
        } else {
            m_filtersBadge->setVisibility(brls::Visibility::GONE);
        }
    }

    rebuildAppliedFilterChips();
    reloadAllItems();
}

void LibrarySectionTab::rebuildAppliedFilterChips() {
    if (!m_appliedFiltersBox) return;
    m_appliedFiltersBox->clearViews();   // frees the old chips

    auto addChip = [this](const std::string& label, std::function<void()> onRemove) {
        auto* chip = new vitaplex::FilterChip();
        chip->setText(label);
        chip->setMarginRight(10);
        chip->setPicked(false);   // neutral fill, like the reference's applied chips
        chip->registerClickAction([this, onRemove](brls::View*) {
            // Removing rebuilds (and frees) this very chip. Park focus on the
            // Filters chip and defer a frame so the click dispatch finishes
            // before the chip is destroyed (same hazard the < Back button
            // guards against).
            if (m_filtersBtn) brls::Application::giveFocus(m_filtersBtn);
            brls::sync([onRemove]() { onRemove(); });
            return true;
        });
        m_appliedFiltersBox->addView(chip);
    };

    // One removable chip per selected value (across all fields).
    for (const auto& kv : m_activeFilters) {
        const std::string field = kv.first;
        for (const auto& v : kv.second.values) {
            const std::string key = v.first;
            addChip(v.second, [this, field, key]() {
                auto it = m_activeFilters.find(field);
                if (it != m_activeFilters.end()) {
                    auto& vals = it->second.values;
                    for (size_t i = 0; i < vals.size(); i++) {
                        if (vals[i].first == key) { vals.erase(vals.begin() + i); break; }
                    }
                    if (vals.empty()) m_activeFilters.erase(it);
                }
                applyFilters();
            });
        }
    }
}

void LibrarySectionTab::updateCountLabel() {
    if (!m_countLabel) return;
    m_countLabel->setText(m_totalItemCount > 0
                              ? std::to_string(m_totalItemCount) + " titles"
                              : "");
}

void LibrarySectionTab::buildAzRail() {
    m_azRail = new brls::Box();
    m_azRail->setAxis(brls::Axis::COLUMN);
    m_azRail->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    m_azRail->setAlignItems(brls::AlignItems::CENTER);
    m_azRail->setPositionType(brls::PositionType::ABSOLUTE);
    m_azRail->setPositionRight(4);
    m_azRail->setPositionTop(118);    // below the title + toolbar
    m_azRail->setPositionBottom(34);
    m_azRail->setWidth(18);
    m_azRail->setVisibility(brls::Visibility::GONE);

    for (const char* c = kAzLetters; *c; ++c) {
        char ch = *c;
        auto* lbl = new brls::Label();
        lbl->setText(std::string(1, ch));
        lbl->setFontSize(9.5f);
        lbl->setTextColor(nvgRGB(0x8a, 0x8a, 0x90));
        lbl->setFocusable(true);
        lbl->registerClickAction([this, ch](brls::View*) { jumpToLetter(ch); return true; });
        lbl->addGestureRecognizer(new brls::TapGestureRecognizer(lbl));
        m_azRail->addView(lbl);
        m_azLetters.push_back(lbl);
    }
    this->addView(m_azRail);
}

// First letter of a title with a leading article dropped, to approximate Plex's
// titleSort bucketing; digits / symbols collapse to '#'.
static char azBucket(const std::string& t) {
    auto lc = [](char c) { return (char)tolower((unsigned char)c); };
    auto startsCI = [&](const char* p) {
        size_t n = 0; while (p[n]) n++;
        if (t.size() < n) return false;
        for (size_t k = 0; k < n; k++) if (lc(t[k]) != lc(p[k])) return false;
        return true;
    };
    size_t i = 0;
    if (startsCI("the ")) i = 4;
    else if (startsCI("an ")) i = 3;
    else if (startsCI("a ")) i = 2;
    while (i < t.size() && t[i] == ' ') i++;
    if (i >= t.size()) return '#';
    char c = (char)toupper((unsigned char)t[i]);
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

void LibrarySectionTab::jumpToLetter(char letter) {
    if (m_items.empty() || !m_contentGrid) return;

    char target = (char)toupper((unsigned char)letter);
    auto rank = [](char b) { return b == '#' ? 0 : (int)b; };   // '#' sorts first

    // m_items is sorted by titleSort asc → first item whose bucket reaches target.
    size_t found = m_items.size();
    for (size_t i = 0; i < m_items.size(); i++) {
        if (rank(azBucket(m_items[i].title)) >= rank(target)) { found = i; break; }
    }
    if (found >= m_items.size()) found = m_items.size() - 1;   // letter beyond loaded range

    m_contentGrid->scrollToItemIndex(found, true);
    m_currentAzLetter = target;
    refreshAzRail();
}

void LibrarySectionTab::refreshAzRail() {
    if (!m_azRail) return;
    bool active = (m_viewMode == LibraryViewMode::ALL_ITEMS &&
                   m_sortParam == "titleSort:asc");
    m_azRail->setVisibility(active ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (!active) return;

    for (size_t i = 0; i < m_azLetters.size(); i++) {
        if (!m_azLetters[i]) continue;
        bool cur = (kAzLetters[i] == m_currentAzLetter);
        m_azLetters[i]->setTextColor(cur ? vitaplex::palette::gold
                                         : nvgRGB(0x8a, 0x8a, 0x90));
        m_azLetters[i]->setFontSize(cur ? 11.0f : 9.5f);
    }
}

bool LibrarySectionTab::navigateBack() {
    // FILTERED came from CATEGORIES / COLLECTIONS / PLAYLISTS / ALL_ITEMS
    // depending on what the user clicked. Drop them back there using the
    // mode we stashed at the FILTERED transition instead of always going
    // to ALL_ITEMS (which was the old behaviour and felt wrong — clicking
    // a category, then Back, used to lose the category list).
    if (m_viewMode == LibraryViewMode::FILTERED) {
        switch (m_modeBeforeFilter) {
            case LibraryViewMode::CATEGORIES:  showCategories();  return true;
            case LibraryViewMode::COLLECTIONS: showCollections(); return true;
            case LibraryViewMode::PLAYLISTS:   showPlaylists();   return true;
            default:                           showAllItems();    return true;
        }
    }
    // Sub-modes (CATEGORIES / COLLECTIONS / PLAYLISTS) all fall back to
    // ALL_ITEMS on Back. From ALL_ITEMS we return false so borealis's
    // default Back handler pops the activity / closes the sidebar.
    if (m_viewMode != LibraryViewMode::ALL_ITEMS) {
        showAllItems();
        return true;
    }
    return false;
}

void LibrarySectionTab::showAllItems() {
    m_viewMode = LibraryViewMode::ALL_ITEMS;
    m_titleLabel->setText(m_title);
    // Free playlist tracks memory when leaving playlist view
    m_playlistTracks.clear();
    m_playlistTracks.shrink_to_fit();
    m_currentPlaylistId.clear();
    m_trackListRendered = 0;
    m_trackListLoadMoreBtn = nullptr;
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
    m_contentGrid->setDataSource(m_items);
    m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
    updateViewModeButtons();
}

void LibrarySectionTab::showCollections() {
    if (!m_collectionsLoaded) {
        brls::Application::notify("Loading collections...");
        return;
    }

    if (m_collections.empty()) {
        brls::Application::notify("No collections available");
        return;
    }

    m_viewMode = LibraryViewMode::COLLECTIONS;
    m_titleLabel->setText(m_title + " - Collections");
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);

    // Show collections in the grid
    m_contentGrid->setDataSource(m_collections);
    updateViewModeButtons();
}

void LibrarySectionTab::showCategories() {
    if (!m_genresLoaded) {
        brls::Application::notify("Loading categories...");
        return;
    }

    if (m_genres.empty()) {
        brls::Application::notify("No categories available");
        return;
    }

    m_viewMode = LibraryViewMode::CATEGORIES;
    m_titleLabel->setText(m_title + " - Categories");
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);

    // Convert genres to MediaItem format for the grid
    std::vector<MediaItem> genreItems;
    for (const auto& genre : m_genres) {
        MediaItem item;
        item.title = genre.title;
        item.ratingKey = genre.key;  // Use genre key for filtering
        item.type = "genre";
        item.mediaType = MediaType::UNKNOWN;
        genreItems.push_back(item);
    }

    m_contentGrid->setDataSource(genreItems);
    updateViewModeButtons();
}

void LibrarySectionTab::styleButton(brls::Button* btn, bool active) {
    // The view-mode pills are FilterChips: they own the full pick ladder
    // (gold fill picked, gold-bright when picked+focused — palette rules 1-2).
    if (auto* chip = dynamic_cast<vitaplex::FilterChip*>(btn)) {
        chip->setPicked(active);
        return;
    }
    // Non-chip buttons (e.g. the Back pill) get the neutral resting style.
    namespace pal = vitaplex::palette;
    btn->setCornerRadius(16);
    btn->setHighlightCornerRadius(16);
    btn->setPadding(6, 16, 6, 16);
    // setTextColor() triggers Button::applyStyle() which resets the bg, so
    // set the label colour first and the fill/border last (see FilterChip).
    btn->setTextColor(active ? pal::goldInk : pal::text);
    btn->setBackgroundColor(active ? pal::gold : pal::surface3);
    btn->setBorderColor(active ? pal::goldBright : nvgRGBA(0, 0, 0, 0));
    btn->setBorderThickness(active ? 1.5f : 0.0f);
}

void LibrarySectionTab::updateViewModeButtons() {
    // Move focus off any view that's about to disappear from this pass —
    // either a mode button being hidden, or anything inside the content
    // area whose ancestor was just flipped to Visibility::GONE by the
    // show* method that called us. Without this, borealis ends up with a
    // focused-but-hidden view and the next input lands on something
    // unrelated (or crashes when the input dispatcher walks into a
    // hidden subtree). Two distinct failure modes both fall out of this:
    //   - "< Back" button click hides itself while focused → stuck on
    //     a hidden button.
    //   - Leaving a playlist hides the track list while a track row is
    //     focused → focus stays on the hidden row.
    brls::View* focused = brls::Application::getCurrentFocus();
    bool inFilteredView = (m_viewMode == LibraryViewMode::FILTERED);
    bool showModeButtons = !inFilteredView;
    // Sort / Unwatched / spacer only apply to the all-items grid.
    bool allItems = (m_viewMode == LibraryViewMode::ALL_ITEMS);
    auto wouldHide = [&](brls::Button* btn, bool willBeVisible) {
        return btn && focused == btn && !willBeVisible;
    };
    bool focusedAboutToHide =
        wouldHide(m_backBtn, inFilteredView) ||
        wouldHide(m_collectionsBtn, showModeButtons && !m_collections.empty()) ||
        wouldHide(m_categoriesBtn, showModeButtons && !m_genres.empty()) ||
        wouldHide(m_playlistsBtn, showModeButtons && !m_playlists.empty()) ||
        wouldHide(m_sortBtn, allItems) ||
        wouldHide(m_filtersBtn, allItems);

    // Walk up from the focused view; if any ancestor is currently
    // Visibility::GONE the focus is sitting in a hidden subtree.
    if (!focusedAboutToHide && focused) {
        for (brls::View* p = focused; p != nullptr; p = p->getParent()) {
            if (p->getVisibility() != brls::Visibility::VISIBLE) {
                focusedAboutToHide = true;
                break;
            }
        }
    }

    if (focusedAboutToHide) {
        // Transfer focus to whichever content area is going to be the
        // visible one after the show* method that called us. The grid
        // takes precedence in every mode except playlist-track view,
        // where m_trackListScroll holds the rows.
        brls::View* target = nullptr;
        if (m_trackListScroll &&
            m_trackListScroll->getVisibility() == brls::Visibility::VISIBLE) {
            target = m_trackListScroll;
        } else if (m_contentGrid &&
                   m_contentGrid->getVisibility() == brls::Visibility::VISIBLE) {
            target = m_contentGrid;
        }
        if (target) brls::Application::giveFocus(target);
    }

    m_backBtn->setVisibility(inFilteredView ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    if (m_collectionsBtn) {
        m_collectionsBtn->setVisibility(showModeButtons && !m_collections.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (m_categoriesBtn) {
        m_categoriesBtn->setVisibility(showModeButtons && !m_genres.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (m_playlistsBtn) {
        m_playlistsBtn->setVisibility(showModeButtons && !m_playlists.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    // Sort + Filters chips and their pushing spacer ride the all-items grid
    // only (sorting/filtering a genre/collection list makes no sense).
    auto vis = [](bool on) { return on ? brls::Visibility::VISIBLE : brls::Visibility::GONE; };
    if (m_toolbarSpacer) m_toolbarSpacer->setVisibility(vis(allItems));
    if (m_sortBtn)       m_sortBtn->setVisibility(vis(allItems));
    if (m_filtersBtn)        m_filtersBtn->setVisibility(vis(allItems));
    if (m_appliedFiltersBox) m_appliedFiltersBox->setVisibility(vis(allItems));

    // Update active styling on mode buttons
    if (showModeButtons) {
        if (m_collectionsBtn) styleButton(m_collectionsBtn, m_viewMode == LibraryViewMode::COLLECTIONS);
        if (m_categoriesBtn) styleButton(m_categoriesBtn, m_viewMode == LibraryViewMode::CATEGORIES);
        if (m_playlistsBtn) styleButton(m_playlistsBtn, m_viewMode == LibraryViewMode::PLAYLISTS);
    }

    // DPAD-DOWN from the "< Back" button should land on whichever
    // content area is currently visible (filtered grid items or track
    // list rows). Borealis's default navigation walked sideways into
    // m_viewModeBox's other children — all GONE in FILTERED view —
    // which made DOWN visibly hop to a hidden Playlists/Collections
    // button.
    if (m_backBtn) {
        brls::View* downTarget = nullptr;
        if (m_trackListScroll &&
            m_trackListScroll->getVisibility() == brls::Visibility::VISIBLE) {
            downTarget = m_trackListScroll;
        } else if (m_contentGrid &&
                   m_contentGrid->getVisibility() == brls::Visibility::VISIBLE) {
            downTarget = m_contentGrid;
        }
        if (downTarget) {
            m_backBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, downTarget);
        }
    }

    // The A-Z rail rides the all-items grid; hide it in any sub-view.
    refreshAzRail();
}

void LibrarySectionTab::onItemSelected(const MediaItem& item) {
    // Handle selection based on current view mode
    if (m_viewMode == LibraryViewMode::COLLECTIONS) {
        // Selected a collection - show its contents
        onCollectionSelected(item);
        return;
    }

    if (m_viewMode == LibraryViewMode::CATEGORIES) {
        // Selected a category/genre - filter by it
        GenreItem genre;
        genre.title = item.title;
        genre.key = item.ratingKey;
        onGenreSelected(genre);
        return;
    }

    if (m_viewMode == LibraryViewMode::PLAYLISTS) {
        // Selected a playlist - show its contents
        for (const auto& playlist : m_playlists) {
            if (playlist.ratingKey == item.ratingKey) {
                onPlaylistSelected(playlist);
                return;
            }
        }
        return;
    }

    // Normal item selection
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void LibrarySectionTab::onCollectionSelected(const MediaItem& collection) {
    brls::Logger::debug("LibrarySectionTab: Selected collection: {}", collection.title);

    m_modeBeforeFilter = m_viewMode;
    m_filterTitle = collection.title;
    std::string collectionKey = collection.ratingKey;
    std::string filterTitle = m_filterTitle;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, collectionKey, filterTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchChildren(collectionKey, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items in collection", items.size());

            brls::sync([this, items, filterTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryViewMode::FILTERED;
                m_titleLabel->setText(m_title + " - " + filterTitle);
                m_trackListScroll->setVisibility(brls::Visibility::GONE);
                m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
                m_contentGrid->setDataSource(items);
                updateViewModeButtons();
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load collection content");
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load collection");
            });
        }
    });
}

void LibrarySectionTab::onGenreSelected(const GenreItem& genre) {
    brls::Logger::debug("LibrarySectionTab: Selected genre: {} (key: {})", genre.title, genre.key);

    m_modeBeforeFilter = m_viewMode;
    m_filterTitle = genre.title;
    std::string key = m_sectionKey;
    std::string genreKey = genre.key;
    std::string genreTitle = genre.title;
    std::string filterTitle = m_filterTitle;
    std::weak_ptr<bool> aliveWeak = m_alive;

    std::string secType = m_sectionType;
    asyncRun([this, key, genreKey, genreTitle, filterTitle, secType, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        // Plex type codes: 1=movie, 2=show, 8=artist
        int metadataType = 0;
        if (secType == "movie") metadataType = 1;
        else if (secType == "show") metadataType = 2;
        else if (secType == "artist") metadataType = 8;

        // Try with genre key first, fall back to title
        if (client.fetchByGenreKey(key, genreKey, items, metadataType) || client.fetchByGenre(key, genreTitle, items, metadataType)) {
            brls::Logger::info("LibrarySectionTab: Got {} items for genre", items.size());

            brls::sync([this, items, filterTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryViewMode::FILTERED;
                m_titleLabel->setText(m_title + " - " + filterTitle);
                m_trackListScroll->setVisibility(brls::Visibility::GONE);
                m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
                m_contentGrid->setDataSource(items);
                updateViewModeButtons();
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load genre content");
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load category");
            });
        }
    });
}

void LibrarySectionTab::loadPlaylists() {
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<Playlist> playlists;

        if (client.fetchMusicPlaylists(playlists)) {
            brls::Logger::info("LibrarySectionTab: Got {} music playlists", playlists.size());

            brls::sync([this, playlists, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_playlists = playlists;
                m_playlistsLoaded = true;

                if (m_playlists.empty() && m_playlistsBtn) {
                    m_playlistsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: Failed to load playlists or none found");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_playlistsLoaded = true;
                if (m_playlistsBtn) {
                    m_playlistsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibrarySectionTab::showPlaylists() {
    if (!m_playlistsLoaded) {
        brls::Application::notify("Loading playlists...");
        return;
    }

    if (m_playlists.empty()) {
        brls::Application::notify("No playlists available");
        return;
    }

    m_viewMode = LibraryViewMode::PLAYLISTS;
    m_titleLabel->setText(m_title + " - Playlists");
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);

    // Convert playlists to MediaItem format for the grid
    std::vector<MediaItem> playlistItems;
    for (size_t i = 0; i < m_playlists.size(); i++) {
        MediaItem item;
        item.title = m_playlists[i].title;
        if (m_playlists[i].leafCount > 0) {
            item.title += " (" + std::to_string(m_playlists[i].leafCount) + " tracks)";
        }
        item.ratingKey = m_playlists[i].ratingKey;
        item.thumb = m_playlists[i].composite.empty() ? m_playlists[i].thumb : m_playlists[i].composite;
        item.type = "playlist";
        item.mediaType = MediaType::UNKNOWN;
        playlistItems.push_back(item);
    }

    m_contentGrid->setDataSource(playlistItems);
    updateViewModeButtons();
}

void LibrarySectionTab::onPlaylistSelected(const Playlist& playlist) {
    brls::Logger::debug("LibrarySectionTab: Selected playlist: {}", playlist.title);

    std::string playlistId = playlist.ratingKey;
    std::string playlistTitle = playlist.title;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, playlistId, playlistTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<PlaylistItem> items;

        if (client.fetchPlaylistItems(playlistId, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items in playlist", items.size());

            // Build MediaItem vector, then move into sync lambda to avoid extra copy
            auto mediaItems = std::make_shared<std::vector<MediaItem>>();
            mediaItems->reserve(items.size());
            for (auto& item : items) {
                mediaItems->push_back(std::move(item.media));
            }

            brls::sync([this, mediaItems, playlistTitle, playlistId, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                showPlaylistTrackList(std::move(*mediaItems), playlistTitle, playlistId);
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load playlist content");
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load playlist");
            });
        }
    });
}

void LibrarySectionTab::showPlaylistTrackList(std::vector<MediaItem>&& tracks,
                                                const std::string& playlistTitle,
                                                const std::string& playlistId) {
    m_modeBeforeFilter = m_viewMode;
    m_viewMode = LibraryViewMode::FILTERED;
    m_titleLabel->setText(m_title + " - " + playlistTitle + " (" + std::to_string(tracks.size()) + " tracks)");

    // Store tracks as member to avoid per-row copies (was causing OOM with large playlists)
    m_playlistTracks = std::move(tracks);
    m_currentPlaylistId = playlistId;
    m_trackListRendered = 0;
    m_trackListLoadMoreBtn = nullptr;

    // Flip visibility BEFORE updateViewModeButtons() so its focus-
    // transfer logic sees the right target (m_trackListScroll, not
    // m_contentGrid). The clicked playlist cell still owns the focus
    // at this point — the grid is about to hide, so we can't park it
    // on a grid cell.
    m_contentGrid->setVisibility(brls::Visibility::GONE);
    m_trackListScroll->setVisibility(brls::Visibility::VISIBLE);
    m_trackListBox->clearViews();

    // Render first page of tracks (must come before updateViewModeButtons
    // so the focus transfer in there has a focusable row to land on).
    appendTrackListPage();

    // Transfer focus into the new track list explicitly — the clicked
    // playlist cell is now inside a Visibility::GONE grid, and without
    // this nudge borealis leaves focus on the hidden cell and the user
    // can't interact with the playlist they just opened.
    if (m_trackListBox && !m_trackListBox->getChildren().empty()) {
        brls::Application::giveFocus(m_trackListScroll);
    }

    updateViewModeButtons();
}

void LibrarySectionTab::appendTrackListPage() {
    // Remove existing "Load More" button
    if (m_trackListLoadMoreBtn) {
        m_trackListBox->removeView(m_trackListLoadMoreBtn);
        m_trackListLoadMoreBtn = nullptr;
    }

    size_t end = std::min(m_trackListRendered + playlistTrackPageSize(), m_playlistTracks.size());

    for (size_t i = m_trackListRendered; i < end; i++) {
        const auto& track = m_playlistTracks[i];

        const auto& ic = platform::getImageConstraints();
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        // Tracks are a dense list — scale slightly below the standard list
        // row height so more titles fit on screen.
        row->setHeight(std::max(40, ic.listRowHeight - 8));
        row->setPadding(8, 12, 8, 12);
        row->setMarginBottom(3);
        row->setCornerRadius(6);
        row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
        row->setFocusable(true);

        // Only the topmost track row escapes UP to the on-screen
        // "< Back" button. Every other row keeps default UP navigation
        // so DPAD-UP moves to the previous track. Without the explicit
        // route on the first row, UP gets stuck because
        // ScrollingFrame::getNextFocus(UP) consumes the input while the
        // scroll has any offset, and Box::getNextFocus past the top
        // doesn't reliably reach m_viewModeBox with a GONE m_contentGrid
        // sitting between them.
        if (i == 0 && m_backBtn) {
            row->setCustomNavigationRoute(brls::FocusDirection::UP, m_backBtn);
        }

        // Left side: track number + artist + title
        auto* leftBox = new brls::Box();
        leftBox->setAxis(brls::Axis::ROW);
        leftBox->setAlignItems(brls::AlignItems::CENTER);
        leftBox->setGrow(1.0f);

        auto* trackNum = new brls::Label();
        trackNum->setFontSize(13);
        trackNum->setMarginRight(10);
        trackNum->setTextColor(nvgRGBA(150, 150, 150, 255));
        trackNum->setText(std::to_string(i + 1));
        leftBox->addView(trackNum);

        auto* titleLabel = new brls::Label();
        titleLabel->setFontSize(13);
        std::string displayTitle = track.title;
        if (!track.grandparentTitle.empty()) {
            displayTitle = track.grandparentTitle + " - " + displayTitle;
        }
        // Truncate to whatever the platform budget allows (55 on Vita, ~110 on desktop).
        {
            size_t maxChars = (size_t)ic.maxListTitleChars;
            if (maxChars > 3 && displayTitle.length() > maxChars) {
                displayTitle = displayTitle.substr(0, maxChars - 3) + "...";
            }
        }
        titleLabel->setText(displayTitle);
        leftBox->addView(titleLabel);

        row->addView(leftBox);

        // Right side: duration
        if (track.duration > 0) {
            auto* durLabel = new brls::Label();
            durLabel->setFontSize(12);
            durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
            int totalSec = track.duration / 1000;
            int min = totalSec / 60;
            int sec = totalSec % 60;
            char durStr[16];
            snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
            durLabel->setText(durStr);
            row->addView(durLabel);
        }

        // Click to play track - only capture the index, reference m_playlistTracks via 'this'
        size_t idx = i;
        row->registerClickAction([this, idx](brls::View*) {
            performPlaylistTrackAction(idx);
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

        m_trackListBox->addView(row);
    }

    m_trackListRendered = end;

    // Add "Load More" button if there are remaining tracks
    if (m_trackListRendered < m_playlistTracks.size()) {
        size_t remaining = m_playlistTracks.size() - m_trackListRendered;
        m_trackListLoadMoreBtn = new brls::Button();
        auto* label = new brls::Label();
        label->setText("Load More (" + std::to_string(remaining) + " remaining)");
        label->setFontSize(16);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_trackListLoadMoreBtn->addView(label);
        m_trackListLoadMoreBtn->setMarginTop(10);
        m_trackListLoadMoreBtn->setHeight(44);
        m_trackListLoadMoreBtn->registerClickAction([this](brls::View*) {
            appendTrackListPage();
            return true;
        });
        m_trackListLoadMoreBtn->addGestureRecognizer(new brls::TapGestureRecognizer(m_trackListLoadMoreBtn));
        m_trackListBox->addView(m_trackListLoadMoreBtn);
    }
}

void LibrarySectionTab::performPlaylistTrackAction(size_t trackIndex) {
    if (trackIndex >= m_playlistTracks.size()) return;

    const MediaItem& track = m_playlistTracks[trackIndex];
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;
    MusicQueue& queue = MusicQueue::getInstance();

    auto executeAction = [this, &track, trackIndex, &queue](TrackDefaultAction act) {
        switch (act) {
            case TrackDefaultAction::PLAY_NOW_CLEAR:
            default:
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                break;

            case TrackDefaultAction::PLAY_NEXT:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.insertTrackAfterCurrent(track);
                    brls::Application::notify("Playing next: " + track.title);
                }
                break;

            case TrackDefaultAction::ADD_TO_BOTTOM:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.addTrack(track);
                    brls::Application::notify("Added to queue: " + track.title);
                }
                break;

            case TrackDefaultAction::PLAY_NOW_REPLACE:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.insertTrackAfterCurrent(track);
                    if (queue.playNext()) {
                        brls::Application::notify("Now playing: " + track.title);
                    }
                }
                break;

            case TrackDefaultAction::ASK_EACH_TIME:
                break;  // Handled below
        }
    };

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        auto* dialog = new brls::Dialog("Choose Action");
        auto* optionsBox = new brls::Box();
        optionsBox->setAxis(brls::Axis::COLUMN);
        optionsBox->setPadding(20);

        auto addBtn = [this, trackIndex, dialog, &optionsBox](const std::string& text, TrackDefaultAction act) {
            auto* btn = new brls::Button();
            btn->setText(text);
            btn->setHeight(44);
            btn->setMarginBottom(10);
            btn->registerClickAction([this, dialog, trackIndex, act](brls::View*) {
                dialog->dismiss();
                if (trackIndex >= m_playlistTracks.size()) return true;
                const MediaItem& t = m_playlistTracks[trackIndex];
                if (act == TrackDefaultAction::PLAY_NOW_CLEAR) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else if (act == TrackDefaultAction::PLAY_NEXT) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.insertTrackAfterCurrent(t);
                        brls::Application::notify("Playing next: " + t.title);
                    }
                } else if (act == TrackDefaultAction::ADD_TO_BOTTOM) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.addTrack(t);
                        brls::Application::notify("Added to queue: " + t.title);
                    }
                } else if (act == TrackDefaultAction::PLAY_NOW_REPLACE) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.insertTrackAfterCurrent(t);
                        if (q.playNext()) {
                            brls::Application::notify("Now playing: " + t.title);
                        }
                    }
                }
                return true;
            });
            btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
            optionsBox->addView(btn);
        };

        addBtn("Play Playlist from Here", TrackDefaultAction::PLAY_NOW_CLEAR);
        addBtn("Play Next", TrackDefaultAction::PLAY_NEXT);
        addBtn("Add to Bottom of Queue", TrackDefaultAction::ADD_TO_BOTTOM);
        addBtn("Play Now (Replace Current)", TrackDefaultAction::PLAY_NOW_REPLACE);

        dialog->addView(optionsBox);
        dialog->open();
    } else {
        executeAction(action);
    }
}

void LibrarySectionTab::showPlaylistContextMenu(const Playlist& playlist) {
    auto* dialog = new brls::Dialog(playlist.title);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    Playlist capturedPlaylist = playlist;

    addDialogButton("Play Now (Clear Queue)", [this, capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        playPlaylistWithQueue(capturedPlaylist.ratingKey, 0);
        return true;
    });

    addDialogButton("Add to Queue", [capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        std::string playlistId = capturedPlaylist.ratingKey;
        asyncRun([playlistId]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<PlaylistItem> items;
            if (client.fetchPlaylistItems(playlistId, items) && !items.empty()) {
                std::vector<MediaItem> tracks;
                for (const auto& item : items) {
                    tracks.push_back(item.media);
                }
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(tracks, 0));
                    } else {
                        queue.addTracks(tracks);
                        brls::Application::notify("Playlist added to queue");
                    }
                });
            } else {
                brls::sync([]() {
                    brls::Application::notify("Cannot queue playlist - server unreachable");
                });
            }
        });
        return true;
    });

    addDialogButton("Download", [capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        std::string playlistId = capturedPlaylist.ratingKey;
        std::string playlistTitle = capturedPlaylist.title;
        std::string playlistThumb = capturedPlaylist.thumb.empty() ? capturedPlaylist.composite : capturedPlaylist.thumb;
        asyncRun([playlistId, playlistTitle, playlistThumb]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<PlaylistItem> items;
            int queued = 0;
            int skipped = 0;

            if (client.fetchPlaylistItems(playlistId, items)) {
                auto& mgr = DownloadsManager::getInstance();
                for (const auto& item : items) {
                    // Skip items already downloaded or in queue
                    if (mgr.isDownloaded(item.media.ratingKey) ||
                        mgr.getDownload(item.media.ratingKey) != nullptr) {
                        skipped++;
                        continue;
                    }
                    MediaItem fullItem;
                    if (client.fetchMediaDetails(item.media.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                        if (mgr.queueDownload(
                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                            fullItem.duration, "track",
                            playlistTitle, 0, fullItem.index,
                            fullItem.thumb,
                            DownloadGroupType::PLAYLIST, playlistId,
                            playlistTitle, playlistThumb)) {
                            queued++;
                        }
                    }
                }
            }

            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " tracks for download";
                if (skipped > 0) {
                    msg += " (" + std::to_string(skipped) + " already downloaded)";
                }
                brls::Application::notify(msg);
            });
        });
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void LibrarySectionTab::showPlaylistOptionsDialog(const Playlist& playlist) {
    brls::Dialog* dialog = new brls::Dialog(playlist.title);

    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setPadding(20);

    auto* trackCount = new brls::Label();
    trackCount->setText("Tracks: " + std::to_string(playlist.leafCount));
    trackCount->setMarginBottom(10);
    infoBox->addView(trackCount);

    if (playlist.smart) {
        auto* smartLabel = new brls::Label();
        smartLabel->setText("(Smart Playlist - cannot be edited)");
        smartLabel->setFontSize(14);
        smartLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
        infoBox->addView(smartLabel);
    }

    dialog->addView(infoBox);

    std::string playlistId = playlist.ratingKey;
    dialog->addButton("Play All", [this, playlistId]() {
        playPlaylistWithQueue(playlistId, 0);
    });

    if (!playlist.smart) {
        dialog->addButton("Delete", [this, playlistId]() {
            brls::Dialog* confirmDialog = new brls::Dialog("Delete this playlist?");
            confirmDialog->addButton("Yes, Delete", [this, playlistId]() {
                std::weak_ptr<bool> aliveWeak = m_alive;

                asyncRun([this, playlistId, aliveWeak]() {
                    PlexClient& client = PlexClient::getInstance();

                    if (client.deletePlaylist(playlistId)) {
                        brls::Logger::info("LibrarySectionTab: Deleted playlist");

                        brls::sync([this, aliveWeak]() {
                            auto alive = aliveWeak.lock();
                            if (!alive || !*alive) return;

                            loadPlaylists();
                            showAllItems();
                        });
                    }
                });
            });
            confirmDialog->addButton("Cancel", []() {});
            confirmDialog->open();
        });
    }

    dialog->addButton("Cancel", []() {});
    dialog->open();
}

void LibrarySectionTab::playPlaylistWithQueue(const std::string& playlistId, int startIndex) {
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, playlistId, startIndex, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<PlaylistItem> items;

        if (client.fetchPlaylistItems(playlistId, items) && !items.empty()) {
            std::vector<MediaItem> tracks;
            for (const auto& item : items) {
                tracks.push_back(item.media);
            }

            brls::sync([tracks, startIndex, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(tracks, startIndex)
                );
            });
        } else {
            brls::sync([]() {
                brls::Application::notify("Cannot play playlist - server unreachable");
            });
        }
    });
}

} // namespace vitaplex
