/**
 * VitaPlex - Library Tab implementation
 */

#include "view/library_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"

namespace vitaplex {

LibraryTab::LibraryTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Library");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Sections row
    m_sectionsBox = new brls::Box();
    m_sectionsBox->setAxis(brls::Axis::ROW);
    m_sectionsBox->setHeight(50);
    m_sectionsBox->setMarginBottom(20);
    this->addView(m_sectionsBox);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    this->addView(m_contentGrid);

    // Load sections immediately
    brls::Logger::debug("LibraryTab: Loading sections...");
    loadSections();
}

void LibraryTab::onFocusGained() {
    brls::Box::onFocusGained();

    if (!m_loaded) {
        loadSections();
    }
}

void LibraryTab::loadSections() {
    PlexClient& client = PlexClient::getInstance();

    brls::Logger::debug("LibraryTab::loadSections - Server: {}", client.getServerUrl());
    brls::Logger::debug("LibraryTab: Fetching library sections...");

    if (client.fetchLibrarySections(m_sections)) {
        brls::Logger::info("LibraryTab: Got {} sections", m_sections.size());
        m_sectionsBox->clearViews();

        for (const auto& section : m_sections) {
            brls::Logger::debug("LibraryTab: Adding section button: {}", section.title);
            auto* btn = new brls::Button();
            btn->setText(section.title);
            btn->setMarginRight(10);

            btn->registerClickAction([this, section](brls::View* view) {
                onSectionSelected(section);
                return true;
            });

            m_sectionsBox->addView(btn);
        }

        // Load first section by default
        if (!m_sections.empty()) {
            brls::Logger::debug("LibraryTab: Loading first section: {}", m_sections[0].title);
            onSectionSelected(m_sections[0]);
        }
    } else {
        brls::Logger::error("LibraryTab: Failed to fetch sections");
    }

    m_loaded = true;
    brls::Logger::debug("LibraryTab: Sections loading complete");
}

void LibraryTab::loadContent(const std::string& sectionKey) {
    PlexClient& client = PlexClient::getInstance();

    brls::Logger::debug("LibraryTab::loadContent - section: {}", sectionKey);

    if (client.fetchLibraryContent(sectionKey, m_items)) {
        brls::Logger::info("LibraryTab: Got {} items for section {}", m_items.size(), sectionKey);
        m_contentGrid->setDataSource(m_items);
    } else {
        brls::Logger::error("LibraryTab: Failed to load content for section {}", sectionKey);
    }
}

void LibraryTab::onSectionSelected(const LibrarySection& section) {
    m_currentSection = section.key;
    m_titleLabel->setText("Library - " + section.title);
    loadContent(section.key);
}

void LibraryTab::onItemSelected(const MediaItem& item) {
    // Show media detail view
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
