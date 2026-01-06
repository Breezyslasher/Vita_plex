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

    m_continueWatchingBox = new brls::Box();
    m_continueWatchingBox->setAxis(brls::Axis::ROW);
    m_continueWatchingBox->setHeight(180);
    m_continueWatchingBox->setMarginBottom(20);
    this->addView(m_continueWatchingBox);

    // Recently Added section
    auto* recentLabel = new brls::Label();
    recentLabel->setText("Recently Added");
    recentLabel->setFontSize(22);
    recentLabel->setMarginBottom(10);
    this->addView(recentLabel);

    m_recentlyAddedBox = new brls::Box();
    m_recentlyAddedBox->setAxis(brls::Axis::ROW);
    m_recentlyAddedBox->setHeight(180);
    this->addView(m_recentlyAddedBox);

    // Load content immediately
    brls::Logger::debug("HomeTab: Loading content...");
    loadContent();
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

            // Update UI on main thread
            brls::sync([this, items]() {
                m_continueWatching = items;
                m_continueWatchingBox->clearViews();

                for (const auto& item : m_continueWatching) {
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

                    m_continueWatchingBox->addView(cell);
                }
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch continue watching");
        }
    });

    // Load recently added asynchronously
    asyncRun([this]() {
        brls::Logger::debug("HomeTab: Fetching recently added (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchRecentlyAdded(items)) {
            brls::Logger::info("HomeTab: Got {} recently added items", items.size());

            // Update UI on main thread
            brls::sync([this, items]() {
                m_recentlyAdded = items;
                m_recentlyAddedBox->clearViews();

                for (const auto& item : m_recentlyAdded) {
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

                    m_recentlyAddedBox->addView(cell);
                }
            });
        } else {
            brls::Logger::error("HomeTab: Failed to fetch recently added");
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
