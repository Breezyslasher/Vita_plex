/**
 * VitaPlex - Home Tab implementation
 */

#include "view/home_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"

namespace vitaplex {

HomeTab::HomeTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Create vertical scrolling container for the entire tab
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);
    m_scrollContent->setPadding(20);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Home");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    m_scrollContent->addView(m_titleLabel);

    // Continue Watching section
    auto* continueLabel = new brls::Label();
    continueLabel->setText("Continue Watching");
    continueLabel->setFontSize(22);
    continueLabel->setMarginBottom(10);
    m_scrollContent->addView(continueLabel);

    m_continueWatchingRow = createMediaRow(&m_continueWatchingContent);
    m_scrollContent->addView(m_continueWatchingRow);

    // Recently Added Movies section
    auto* moviesLabel = new brls::Label();
    moviesLabel->setText("Recently Added Movies");
    moviesLabel->setFontSize(22);
    moviesLabel->setMarginBottom(10);
    moviesLabel->setMarginTop(15);
    m_scrollContent->addView(moviesLabel);

    m_moviesRow = createMediaRow(&m_moviesContent);
    m_scrollContent->addView(m_moviesRow);

    // Recently Added TV Shows section
    auto* showsLabel = new brls::Label();
    showsLabel->setText("Recently Added TV Shows");
    showsLabel->setFontSize(22);
    showsLabel->setMarginBottom(10);
    showsLabel->setMarginTop(15);
    m_scrollContent->addView(showsLabel);

    m_showsRow = createMediaRow(&m_showsContent);
    m_scrollContent->addView(m_showsRow);

    // Recently Added Music section
    auto* musicLabel = new brls::Label();
    musicLabel->setText("Recently Added Music");
    musicLabel->setFontSize(22);
    musicLabel->setMarginBottom(10);
    musicLabel->setMarginTop(15);
    m_scrollContent->addView(musicLabel);

    m_musicRow = createMediaRow(&m_musicContent);
    m_scrollContent->addView(m_musicRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);

    // Load content immediately
    brls::Logger::debug("HomeTab: Loading content...");
    loadContent();
}

brls::HScrollingFrame* HomeTab::createMediaRow(brls::Box** contentOut) {
    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(180);
    scrollFrame->setMarginBottom(10);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setJustifyContent(brls::JustifyContent::FLEX_START);
    content->setAlignItems(brls::AlignItems::CENTER);

    scrollFrame->setContentView(content);

    // Return content box via output parameter
    if (contentOut) {
        *contentOut = content;
    }

    return scrollFrame;
}

void HomeTab::populateRow(brls::Box* rowContent, const std::vector<MediaItem>& items) {
    if (!rowContent) return;

    rowContent->clearViews();

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setItem(item);
        cell->setWidth(120);
        cell->setHeight(170);
        cell->setMarginRight(10);

        MediaItem capturedItem = item;
        cell->registerClickAction([this, capturedItem](brls::View* view) {
            onItemSelected(capturedItem);
            return true;
        });

        rowContent->addView(cell);
    }

    // Add placeholder if empty
    if (items.empty()) {
        auto* placeholder = new brls::Label();
        placeholder->setText("No items");
        placeholder->setFontSize(16);
        placeholder->setMarginLeft(10);
        rowContent->addView(placeholder);
    }
}

void HomeTab::onFocusGained() {
    brls::Box::onFocusGained();

    if (!m_loaded) {
        loadContent();
    }
}

void HomeTab::loadContent() {
    brls::Logger::debug("HomeTab::loadContent - Starting async load");

    // Load continue watching asynchronously
    asyncRun([this]() {
        brls::Logger::debug("HomeTab: Fetching continue watching (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchContinueWatching(items)) {
            brls::Logger::info("HomeTab: Got {} continue watching items", items.size());

            brls::sync([this, items]() {
                m_continueWatching = items;
                populateRow(m_continueWatchingContent, m_continueWatching);
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch continue watching");
        }
    });

    // Load recently added by fetching from library sections
    asyncRun([this]() {
        brls::Logger::debug("HomeTab: Fetching library sections for recently added...");
        PlexClient& client = PlexClient::getInstance();

        // First get all library sections
        std::vector<LibrarySection> sections;
        if (!client.fetchLibrarySections(sections)) {
            brls::Logger::error("HomeTab: Failed to fetch library sections");
            return;
        }

        // Get hidden libraries setting
        std::string hiddenLibraries = Application::getInstance().getSettings().hiddenLibraries;

        std::vector<MediaItem> movies;
        std::vector<MediaItem> shows;
        std::vector<MediaItem> music;

        // Helper to check if library is hidden
        auto isHidden = [&hiddenLibraries](const std::string& key) -> bool {
            if (hiddenLibraries.empty()) return false;
            std::string hidden = hiddenLibraries;
            size_t pos = 0;
            while ((pos = hidden.find(',')) != std::string::npos) {
                if (hidden.substr(0, pos) == key) return true;
                hidden.erase(0, pos + 1);
            }
            return (hidden == key);
        };

        // Fetch recently added from each section by type
        for (const auto& section : sections) {
            // Skip hidden libraries
            if (isHidden(section.key)) {
                brls::Logger::debug("HomeTab: Skipping hidden library: {}", section.title);
                continue;
            }

            std::vector<MediaItem> sectionItems;

            // Fetch recently added using the correct API endpoint
            if (client.fetchSectionRecentlyAdded(section.key, sectionItems)) {
                // Sort items by type
                for (auto& item : sectionItems) {
                    if (section.type == "movie") {
                        if (movies.size() < 20) movies.push_back(item);
                    } else if (section.type == "show") {
                        if (shows.size() < 20) shows.push_back(item);
                    } else if (section.type == "artist") {
                        if (music.size() < 20) music.push_back(item);
                    }
                }
            }
        }

        brls::Logger::info("HomeTab: Got {} movies, {} shows, {} music items",
                           movies.size(), shows.size(), music.size());

        // Update UI on main thread
        brls::sync([this, movies, shows, music]() {
            m_recentMovies = movies;
            m_recentShows = shows;
            m_recentMusic = music;

            populateRow(m_moviesContent, m_recentMovies);
            populateRow(m_showsContent, m_recentShows);
            populateRow(m_musicContent, m_recentMusic);
        });
    });

    m_loaded = true;
    brls::Logger::debug("HomeTab: Async content loading started");
}

void HomeTab::onItemSelected(const MediaItem& item) {
    // For tracks, play directly instead of showing detail view
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
