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
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Home");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Continue Watching section
    auto* continueLabel = new brls::Label();
    continueLabel->setText("Continue Watching");
    continueLabel->setFontSize(22);
    continueLabel->setMarginBottom(10);
    this->addView(continueLabel);

    m_continueWatchingRow = createMediaRow();
    this->addView(m_continueWatchingRow);

    // Recently Added Movies section
    auto* moviesLabel = new brls::Label();
    moviesLabel->setText("Recently Added Movies");
    moviesLabel->setFontSize(22);
    moviesLabel->setMarginBottom(10);
    moviesLabel->setMarginTop(15);
    this->addView(moviesLabel);

    m_moviesRow = createMediaRow();
    this->addView(m_moviesRow);

    // Recently Added TV Shows section
    auto* showsLabel = new brls::Label();
    showsLabel->setText("Recently Added TV Shows");
    showsLabel->setFontSize(22);
    showsLabel->setMarginBottom(10);
    showsLabel->setMarginTop(15);
    this->addView(showsLabel);

    m_showsRow = createMediaRow();
    this->addView(m_showsRow);

    // Recently Added Music section
    auto* musicLabel = new brls::Label();
    musicLabel->setText("Recently Added Music");
    musicLabel->setFontSize(22);
    musicLabel->setMarginBottom(10);
    musicLabel->setMarginTop(15);
    this->addView(musicLabel);

    m_musicRow = createMediaRow();
    this->addView(m_musicRow);

    // Load content immediately
    brls::Logger::debug("HomeTab: Loading content...");
    loadContent();
}

brls::HScrollingFrame* HomeTab::createMediaRow() {
    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(180);
    scrollFrame->setMarginBottom(10);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setJustifyContent(brls::JustifyContent::FLEX_START);
    content->setAlignItems(brls::AlignItems::CENTER);

    scrollFrame->setContentView(content);

    // Store content box reference based on which row this is
    if (m_continueWatchingRow == nullptr) {
        m_continueWatchingContent = content;
    } else if (m_moviesRow == nullptr) {
        m_moviesContent = content;
    } else if (m_showsRow == nullptr) {
        m_showsContent = content;
    } else {
        m_musicContent = content;
    }

    return scrollFrame;
}

void HomeTab::populateRow(brls::Box* rowContent, const std::vector<MediaItem>& items) {
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

    // Load recently added movies asynchronously
    asyncRun([this]() {
        brls::Logger::debug("HomeTab: Fetching recently added movies (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchRecentlyAddedByType(MediaType::MOVIE, items)) {
            brls::Logger::info("HomeTab: Got {} recently added movies", items.size());

            brls::sync([this, items]() {
                m_recentMovies = items;
                populateRow(m_moviesContent, m_recentMovies);
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch recently added movies");
        }
    });

    // Load recently added TV shows asynchronously
    asyncRun([this]() {
        brls::Logger::debug("HomeTab: Fetching recently added TV shows (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchRecentlyAddedByType(MediaType::SHOW, items)) {
            brls::Logger::info("HomeTab: Got {} recently added TV shows", items.size());

            brls::sync([this, items]() {
                m_recentShows = items;
                populateRow(m_showsContent, m_recentShows);
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch recently added TV shows");
        }
    });

    // Load recently added music asynchronously
    asyncRun([this]() {
        brls::Logger::debug("HomeTab: Fetching recently added music (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchRecentlyAddedByType(MediaType::MUSIC_ALBUM, items)) {
            brls::Logger::info("HomeTab: Got {} recently added music items", items.size());

            brls::sync([this, items]() {
                m_recentMusic = items;
                populateRow(m_musicContent, m_recentMusic);
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch recently added music");
        }
    });

    m_loaded = true;
    brls::Logger::debug("HomeTab: Async content loading started");
}

void HomeTab::onItemSelected(const MediaItem& item) {
    // Show media detail view
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
