/**
 * VitaPlex - Library Section Tab implementation
 */

#include "view/library_section_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"

namespace vitaplex {

LibrarySectionTab::LibrarySectionTab(const std::string& sectionKey, const std::string& title)
    : m_sectionKey(sectionKey), m_title(title) {

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    this->addView(m_contentGrid);

    // Load content immediately
    brls::Logger::debug("LibrarySectionTab: Created for section {} ({})", m_sectionKey, m_title);
    loadContent();
}

void LibrarySectionTab::onFocusGained() {
    brls::Box::onFocusGained();

    if (!m_loaded) {
        loadContent();
    }
}

void LibrarySectionTab::loadContent() {
    brls::Logger::debug("LibrarySectionTab::loadContent - section: {} (async)", m_sectionKey);

    std::string key = m_sectionKey;
    asyncRun([this, key]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchLibraryContent(key, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items for section {}", items.size(), key);

            brls::sync([this, items]() {
                m_items = items;
                m_contentGrid->setDataSource(m_items);
                m_loaded = true;
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load content for section {}", key);
            brls::sync([this]() {
                m_loaded = true;
            });
        }
    });
}

void LibrarySectionTab::onItemSelected(const MediaItem& item) {
    // For tracks, play directly instead of showing detail view
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Clear focus before pushing to avoid visual artifacts
    brls::Application::giveFocus(nullptr);

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
