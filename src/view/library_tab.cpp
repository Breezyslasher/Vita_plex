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
}

void LibraryTab::onFocusGained() {
    brls::Box::onFocusGained();

    if (!m_loaded) {
        loadSections();
    }
}

void LibraryTab::loadSections() {
    PlexClient& client = PlexClient::getInstance();

    if (client.fetchLibrarySections(m_sections)) {
        m_sectionsBox->clearViews();

        for (const auto& section : m_sections) {
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
            onSectionSelected(m_sections[0]);
        }
    }

    m_loaded = true;
}

void LibraryTab::loadContent(const std::string& sectionKey) {
    PlexClient& client = PlexClient::getInstance();

    if (client.fetchLibraryContent(sectionKey, m_items)) {
        m_contentGrid->setDataSource(m_items);
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
