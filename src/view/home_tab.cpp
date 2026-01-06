/**
 * VitaPlex - Home Tab implementation
 */

#include "view/home_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"

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
    PlexClient& client = PlexClient::getInstance();

    brls::Logger::debug("HomeTab::loadContent - Server: {}", client.getServerUrl());

    // Load continue watching
    brls::Logger::debug("HomeTab: Fetching continue watching...");
    if (client.fetchContinueWatching(m_continueWatching)) {
        brls::Logger::info("HomeTab: Got {} continue watching items", m_continueWatching.size());
        m_continueWatchingBox->clearViews();

        for (const auto& item : m_continueWatching) {
            auto* cell = new MediaItemCell();
            cell->setItem(item);
            cell->setWidth(120);
            cell->setHeight(170);
            cell->setMarginRight(10);

            cell->registerClickAction([this, item](brls::View* view) {
                onItemSelected(item);
                return true;
            });

            m_continueWatchingBox->addView(cell);
        }
    } else {
        brls::Logger::error("HomeTab: Failed to fetch continue watching");
    }

    // Load recently added
    brls::Logger::debug("HomeTab: Fetching recently added...");
    if (client.fetchRecentlyAdded(m_recentlyAdded)) {
        brls::Logger::info("HomeTab: Got {} recently added items", m_recentlyAdded.size());
        m_recentlyAddedBox->clearViews();

        for (const auto& item : m_recentlyAdded) {
            auto* cell = new MediaItemCell();
            cell->setItem(item);
            cell->setWidth(120);
            cell->setHeight(170);
            cell->setMarginRight(10);

            cell->registerClickAction([this, item](brls::View* view) {
                onItemSelected(item);
                return true;
            });

            m_recentlyAddedBox->addView(cell);
        }
    } else {
        brls::Logger::error("HomeTab: Failed to fetch recently added");
    }

    m_loaded = true;
    brls::Logger::debug("HomeTab: Content loading complete");
}

void HomeTab::onItemSelected(const MediaItem& item) {
    // Show media detail view
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
