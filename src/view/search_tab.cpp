/**
 * VitaPlex - Search Tab implementation
 */

#include "view/search_tab.hpp"
#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "app/application.hpp"
#include "app/downloads_manager.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"

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
    m_scrollView->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

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

    m_moviesRow = new HorizontalScrollRow();
    m_moviesRow->setHeight(210);
    m_moviesRow->setMarginBottom(15);
    m_moviesRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_moviesRow);

    // TV Shows row
    m_showsLabel = new brls::Label();
    m_showsLabel->setText("TV Shows");
    m_showsLabel->setFontSize(20);
    m_showsLabel->setMarginBottom(10);
    m_showsLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_showsLabel);

    m_showsRow = new HorizontalScrollRow();
    m_showsRow->setHeight(210);
    m_showsRow->setMarginBottom(15);
    m_showsRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_showsRow);

    // Episodes row
    m_episodesLabel = new brls::Label();
    m_episodesLabel->setText("Episodes");
    m_episodesLabel->setFontSize(20);
    m_episodesLabel->setMarginBottom(10);
    m_episodesLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_episodesLabel);

    m_episodesRow = new HorizontalScrollRow();
    m_episodesRow->setHeight(145);
    m_episodesRow->setMarginBottom(15);
    m_episodesRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_episodesRow);

    // Albums row
    m_albumsLabel = new brls::Label();
    m_albumsLabel->setText("Albums");
    m_albumsLabel->setFontSize(20);
    m_albumsLabel->setMarginBottom(10);
    m_albumsLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_albumsLabel);

    m_albumsRow = new HorizontalScrollRow();
    m_albumsRow->setHeight(160);
    m_albumsRow->setMarginBottom(15);
    m_albumsRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_albumsRow);

    // Tracks row
    m_tracksLabel = new brls::Label();
    m_tracksLabel->setText("Tracks");
    m_tracksLabel->setFontSize(20);
    m_tracksLabel->setMarginBottom(10);
    m_tracksLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_tracksLabel);

    m_tracksRow = new HorizontalScrollRow();
    m_tracksRow->setHeight(160);
    m_tracksRow->setMarginBottom(15);
    m_tracksRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_tracksRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);
}

SearchTab::~SearchTab() {
    if (m_alive) *m_alive = false;
}

void SearchTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    m_loadGeneration++;
    ImageLoader::cancelAll();
    ImageLoader::clearCache();
}

void SearchTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    // Focus search label
    if (m_searchLabel) {
        brls::Application::giveFocus(m_searchLabel);
    }
}

void SearchTab::populateRow(HorizontalScrollRow* row, const std::vector<MediaItem>& items) {
    if (!row) return;

    row->clearViews();

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setItem(item);
        cell->setMarginRight(10);

        MediaItem capturedItem = item;
        cell->registerClickAction([this, capturedItem](brls::View* view) {
            onItemSelected(capturedItem);
            return true;
        });
        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

        // Register START button context menus for movies, shows, and seasons
        if (capturedItem.mediaType == MediaType::MOVIE) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showMovieContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::SHOW) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showShowContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::SEASON) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showSeasonContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::MUSIC_ARTIST) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showArtistContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::MUSIC_ALBUM) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showAlbumContextMenuStatic(capturedItem);
                    return true;
                });
        }

        row->addView(cell);
    }
}

void SearchTab::performSearch(const std::string& query) {
    if (query.empty()) {
        m_resultsLabel->setText("");
        m_results.clear();
        m_movies.clear();
        m_shows.clear();
        m_episodes.clear();
        m_albums.clear();
        m_tracks.clear();

        // Hide all rows and labels
        m_moviesLabel->setVisibility(brls::Visibility::GONE);
        m_moviesRow->setVisibility(brls::Visibility::GONE);
        m_showsLabel->setVisibility(brls::Visibility::GONE);
        m_showsRow->setVisibility(brls::Visibility::GONE);
        m_episodesLabel->setVisibility(brls::Visibility::GONE);
        m_episodesRow->setVisibility(brls::Visibility::GONE);
        m_albumsLabel->setVisibility(brls::Visibility::GONE);
        m_albumsRow->setVisibility(brls::Visibility::GONE);
        m_tracksLabel->setVisibility(brls::Visibility::GONE);
        m_tracksRow->setVisibility(brls::Visibility::GONE);
        return;
    }

    m_resultsLabel->setText("Searching...");

    // Run search async with alive guard and generation counter
    int gen = ++m_loadGeneration;
    asyncRun([this, query, gen, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> results;

        bool success = client.search(query, results);

        brls::sync([this, success, results, gen, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            if (gen != m_loadGeneration) return;  // Stale result

            if (success) {
                m_results = results;

                // Organize results by type
                m_movies.clear();
                m_shows.clear();
                m_episodes.clear();
                m_albums.clear();
                m_tracks.clear();

                for (const auto& item : m_results) {
                    if (item.mediaType == MediaType::MOVIE) {
                        m_movies.push_back(item);
                    } else if (item.mediaType == MediaType::SHOW || item.mediaType == MediaType::SEASON) {
                        m_shows.push_back(item);
                    } else if (item.mediaType == MediaType::EPISODE) {
                        m_episodes.push_back(item);
                    } else if (item.mediaType == MediaType::MUSIC_ALBUM ||
                               item.mediaType == MediaType::MUSIC_ARTIST) {
                        m_albums.push_back(item);
                    } else if (item.mediaType == MediaType::MUSIC_TRACK) {
                        m_tracks.push_back(item);
                    }
                }

                // Show result count per type (like Suwayomi source grouping)
                m_resultsLabel->setText("Found " + std::to_string(m_results.size()) + " results");

                // Update rows with count in header labels
                if (!m_movies.empty()) {
                    m_moviesLabel->setText("Movies (" + std::to_string(m_movies.size()) + ")");
                    m_moviesLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_moviesRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_moviesRow, m_movies);
                } else {
                    m_moviesLabel->setVisibility(brls::Visibility::GONE);
                    m_moviesRow->setVisibility(brls::Visibility::GONE);
                }

                if (!m_shows.empty()) {
                    m_showsLabel->setText("TV Shows (" + std::to_string(m_shows.size()) + ")");
                    m_showsLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_showsRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_showsRow, m_shows);
                } else {
                    m_showsLabel->setVisibility(brls::Visibility::GONE);
                    m_showsRow->setVisibility(brls::Visibility::GONE);
                }

                if (!m_episodes.empty()) {
                    m_episodesLabel->setText("Episodes (" + std::to_string(m_episodes.size()) + ")");
                    m_episodesLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_episodesRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_episodesRow, m_episodes);
                } else {
                    m_episodesLabel->setVisibility(brls::Visibility::GONE);
                    m_episodesRow->setVisibility(brls::Visibility::GONE);
                }

                if (!m_albums.empty()) {
                    m_albumsLabel->setText("Albums (" + std::to_string(m_albums.size()) + ")");
                    m_albumsLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_albumsRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_albumsRow, m_albums);
                } else {
                    m_albumsLabel->setVisibility(brls::Visibility::GONE);
                    m_albumsRow->setVisibility(brls::Visibility::GONE);
                }

                if (!m_tracks.empty()) {
                    m_tracksLabel->setText("Tracks (" + std::to_string(m_tracks.size()) + ")");
                    m_tracksLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_tracksRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_tracksRow, m_tracks);
                } else {
                    m_tracksLabel->setVisibility(brls::Visibility::GONE);
                    m_tracksRow->setVisibility(brls::Visibility::GONE);
                }

            } else {
                m_resultsLabel->setText("Search failed");
                m_results.clear();
            }
        });
    });
}

void SearchTab::onItemSelected(const MediaItem& item) {
    // For tracks, follow the default track action setting
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        MediaDetailView::performTrackActionStatic(item);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vitaplex
