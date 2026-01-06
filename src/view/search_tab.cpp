/**
 * VitaPlex - Search Tab implementation
 */

#include "view/search_tab.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"

namespace vitaplex {

SearchTab::SearchTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Search");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Search input label (acts as button to open keyboard)
    m_searchLabel = new brls::Label();
    m_searchLabel->setText("Tap to search...");
    m_searchLabel->setFontSize(20);
    m_searchLabel->setMarginBottom(20);
    m_searchLabel->setFocusable(true);

    m_searchLabel->registerClickAction([this](brls::View* view) {
        brls::Swkbd::openForText([this](std::string text) {
            m_searchQuery = text;
            m_searchLabel->setText("Search: " + text);
            performSearch(text);
        }, "Search", "Enter search query", 256, m_searchQuery);
        return true;
    });
    m_searchLabel->addGestureRecognizer(new brls::TapGestureRecognizer(m_searchLabel));

    this->addView(m_searchLabel);

    // Results label
    m_resultsLabel = new brls::Label();
    m_resultsLabel->setText("");
    m_resultsLabel->setFontSize(18);
    m_resultsLabel->setMarginBottom(10);
    this->addView(m_resultsLabel);

    // Results grid
    m_resultsGrid = new RecyclingGrid();
    m_resultsGrid->setGrow(1.0f);
    m_resultsGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    this->addView(m_resultsGrid);
}

void SearchTab::onFocusGained() {
    brls::Box::onFocusGained();

    // Focus search label
    if (m_searchLabel) {
        brls::Application::giveFocus(m_searchLabel);
    }
}

void SearchTab::performSearch(const std::string& query) {
    if (query.empty()) {
        m_resultsLabel->setText("");
        m_results.clear();
        m_resultsGrid->setDataSource(m_results);
        return;
    }

    PlexClient& client = PlexClient::getInstance();

    if (client.search(query, m_results)) {
        m_resultsLabel->setText("Found " + std::to_string(m_results.size()) + " results");
        m_resultsGrid->setDataSource(m_results);
    } else {
        m_resultsLabel->setText("Search failed");
        m_results.clear();
        m_resultsGrid->setDataSource(m_results);
    }
}

void SearchTab::onItemSelected(const MediaItem& item) {
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
