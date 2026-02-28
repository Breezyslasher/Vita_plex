/**
 * VitaPlex - Search Tab implementation
 */

#include "view/search_tab.hpp"
#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
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
    m_searchLabel->setMarginBottom(10);
    m_searchLabel->setFocusable(true);

    m_searchLabel->registerClickAction([this](brls::View* view) {
        brls::Application::getImeManager()->openForText([this](std::string text) {
            m_searchQuery = text;
            m_searchLabel->setText(std::string("Search: ") + text);
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

    // Scrollable content for results
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);

    // Movies row
    m_moviesLabel = new brls::Label();
    m_moviesLabel->setText("Movies");
    m_moviesLabel->setFontSize(20);
    m_moviesLabel->setMarginBottom(10);
    m_moviesLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_moviesLabel);

    m_moviesRow = new brls::HScrollingFrame();
    m_moviesRow->setHeight(180);
    m_moviesRow->setMarginBottom(15);
    m_moviesRow->setVisibility(brls::Visibility::GONE);

    m_moviesContent = new brls::Box();
    m_moviesContent->setAxis(brls::Axis::ROW);
    m_moviesContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_moviesRow->setContentView(m_moviesContent);
    m_scrollContent->addView(m_moviesRow);

    // TV Shows row
    m_showsLabel = new brls::Label();
    m_showsLabel->setText("TV Shows");
    m_showsLabel->setFontSize(20);
    m_showsLabel->setMarginBottom(10);
    m_showsLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_showsLabel);

    m_showsRow = new brls::HScrollingFrame();
    m_showsRow->setHeight(180);
    m_showsRow->setMarginBottom(15);
    m_showsRow->setVisibility(brls::Visibility::GONE);

    m_showsContent = new brls::Box();
    m_showsContent->setAxis(brls::Axis::ROW);
    m_showsContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_showsRow->setContentView(m_showsContent);
    m_scrollContent->addView(m_showsRow);

    // Episodes row (separate from TV Shows)
    m_episodesLabel = new brls::Label();
    m_episodesLabel->setText("Episodes");
    m_episodesLabel->setFontSize(20);
    m_episodesLabel->setMarginBottom(10);
    m_episodesLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_episodesLabel);

    m_episodesRow = new brls::HScrollingFrame();
    m_episodesRow->setHeight(180);
    m_episodesRow->setMarginBottom(15);
    m_episodesRow->setVisibility(brls::Visibility::GONE);

    m_episodesContent = new brls::Box();
    m_episodesContent->setAxis(brls::Axis::ROW);
    m_episodesContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_episodesRow->setContentView(m_episodesContent);
    m_scrollContent->addView(m_episodesRow);

    // Music row
    m_musicLabel = new brls::Label();
    m_musicLabel->setText("Music");
    m_musicLabel->setFontSize(20);
    m_musicLabel->setMarginBottom(10);
    m_musicLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_musicLabel);

    m_musicRow = new brls::HScrollingFrame();
    m_musicRow->setHeight(160);
    m_musicRow->setMarginBottom(15);
    m_musicRow->setVisibility(brls::Visibility::GONE);

    m_musicContent = new brls::Box();
    m_musicContent->setAxis(brls::Axis::ROW);
    m_musicContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_musicRow->setContentView(m_musicContent);
    m_scrollContent->addView(m_musicRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);
}

void SearchTab::onFocusGained() {
    brls::Box::onFocusGained();

    // Focus search label
    if (m_searchLabel) {
        brls::Application::giveFocus(m_searchLabel);
    }
}

void SearchTab::populateRow(brls::Box* rowContent, const std::vector<MediaItem>& items) {
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
}

void SearchTab::performSearch(const std::string& query) {
    if (query.empty()) {
        m_resultsLabel->setText("");
        m_results.clear();
        m_movies.clear();
        m_shows.clear();
        m_episodes.clear();
        m_music.clear();

        // Hide all rows and labels
        m_moviesLabel->setVisibility(brls::Visibility::GONE);
        m_moviesRow->setVisibility(brls::Visibility::GONE);
        m_showsLabel->setVisibility(brls::Visibility::GONE);
        m_showsRow->setVisibility(brls::Visibility::GONE);
        m_episodesLabel->setVisibility(brls::Visibility::GONE);
        m_episodesRow->setVisibility(brls::Visibility::GONE);
        m_musicLabel->setVisibility(brls::Visibility::GONE);
        m_musicRow->setVisibility(brls::Visibility::GONE);
        return;
    }

    PlexClient& client = PlexClient::getInstance();

    if (client.search(query, m_results)) {
        m_resultsLabel->setText("Found " + std::to_string(m_results.size()) + " results");

        // Organize results by type - separate shows from episodes
        m_movies.clear();
        m_shows.clear();
        m_episodes.clear();
        m_music.clear();

        for (const auto& item : m_results) {
            if (item.mediaType == MediaType::MOVIE) {
                m_movies.push_back(item);
            } else if (item.mediaType == MediaType::SHOW || item.mediaType == MediaType::SEASON) {
                // Shows and seasons go to TV Shows row
                m_shows.push_back(item);
            } else if (item.mediaType == MediaType::EPISODE) {
                // Episodes get their own row
                m_episodes.push_back(item);
            } else if (item.mediaType == MediaType::MUSIC_ARTIST ||
                       item.mediaType == MediaType::MUSIC_ALBUM ||
                       item.mediaType == MediaType::MUSIC_TRACK) {
                m_music.push_back(item);
            }
        }

        // Update rows visibility and content using stored label references
        if (!m_movies.empty()) {
            m_moviesLabel->setVisibility(brls::Visibility::VISIBLE);
            m_moviesRow->setVisibility(brls::Visibility::VISIBLE);
            populateRow(m_moviesContent, m_movies);
        } else {
            m_moviesLabel->setVisibility(brls::Visibility::GONE);
            m_moviesRow->setVisibility(brls::Visibility::GONE);
        }

        if (!m_shows.empty()) {
            m_showsLabel->setVisibility(brls::Visibility::VISIBLE);
            m_showsRow->setVisibility(brls::Visibility::VISIBLE);
            populateRow(m_showsContent, m_shows);
        } else {
            m_showsLabel->setVisibility(brls::Visibility::GONE);
            m_showsRow->setVisibility(brls::Visibility::GONE);
        }

        if (!m_episodes.empty()) {
            m_episodesLabel->setVisibility(brls::Visibility::VISIBLE);
            m_episodesRow->setVisibility(brls::Visibility::VISIBLE);
            populateRow(m_episodesContent, m_episodes);
        } else {
            m_episodesLabel->setVisibility(brls::Visibility::GONE);
            m_episodesRow->setVisibility(brls::Visibility::GONE);
        }

        if (!m_music.empty()) {
            m_musicLabel->setVisibility(brls::Visibility::VISIBLE);
            m_musicRow->setVisibility(brls::Visibility::VISIBLE);
            populateRow(m_musicContent, m_music);
        } else {
            m_musicLabel->setVisibility(brls::Visibility::GONE);
            m_musicRow->setVisibility(brls::Visibility::GONE);
        }

    } else {
        m_resultsLabel->setText("Search failed");
        m_results.clear();
    }
}

void SearchTab::onItemSelected(const MediaItem& item) {
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
