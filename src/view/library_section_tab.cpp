/**
 * VitaPlex - Library Section Tab implementation
 */

#include "view/library_section_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"

namespace vitaplex {

LibrarySectionTab::LibrarySectionTab(const std::string& sectionKey, const std::string& title, const std::string& sectionType)
    : m_sectionKey(sectionKey), m_title(title), m_sectionType(sectionType) {

    // Create alive flag for async callback safety
    m_alive = std::make_shared<bool>(true);

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(15);
    this->addView(m_titleLabel);

    const auto& settings = Application::getInstance().getSettings();

    // View mode selector (All / Collections / Categories)
    m_viewModeBox = new brls::Box();
    m_viewModeBox->setAxis(brls::Axis::ROW);
    m_viewModeBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_viewModeBox->setAlignItems(brls::AlignItems::CENTER);
    m_viewModeBox->setMarginBottom(15);

    // All Items button
    m_allBtn = new brls::Button();
    m_allBtn->setText("All");
    m_allBtn->setMarginRight(10);
    m_allBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_allBtn);

    // Collections button (only show if enabled)
    if (settings.showCollections) {
        m_collectionsBtn = new brls::Button();
        m_collectionsBtn->setText("Collections");
        m_collectionsBtn->setMarginRight(10);
        m_collectionsBtn->registerClickAction([this](brls::View* view) {
            showCollections();
            return true;
        });
        m_viewModeBox->addView(m_collectionsBtn);
    }

    // Categories button (only show if enabled)
    if (settings.showGenres) {
        m_categoriesBtn = new brls::Button();
        m_categoriesBtn->setText("Categories");
        m_categoriesBtn->setMarginRight(10);
        m_categoriesBtn->registerClickAction([this](brls::View* view) {
            showCategories();
            return true;
        });
        m_viewModeBox->addView(m_categoriesBtn);
    }

    // Back button (hidden by default, shown in filtered view)
    m_backBtn = new brls::Button();
    m_backBtn->setText("< Back");
    m_backBtn->setVisibility(brls::Visibility::GONE);
    m_backBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_backBtn);

    this->addView(m_viewModeBox);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    this->addView(m_contentGrid);

    // Load content immediately
    brls::Logger::debug("LibrarySectionTab: Created for section {} ({}) type={}", m_sectionKey, m_title, m_sectionType);
    loadContent();
}

LibrarySectionTab::~LibrarySectionTab() {
    // Mark as no longer alive to prevent async callbacks from updating destroyed UI
    if (m_alive) {
        *m_alive = false;
    }
    brls::Logger::debug("LibrarySectionTab: Destroyed for section {}", m_sectionKey);
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
    std::weak_ptr<bool> aliveWeak = m_alive;  // Capture weak_ptr for async safety

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchLibraryContent(key, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items for section {}", items.size(), key);

            brls::sync([this, items, aliveWeak]() {
                // Check if object is still alive before updating UI
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) {
                    brls::Logger::debug("LibrarySectionTab: Tab destroyed, skipping UI update");
                    return;
                }

                m_items = items;
                m_contentGrid->setDataSource(m_items);
                m_loaded = true;
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load content for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });

    // Preload collections and genres for quick switching
    const auto& settings = Application::getInstance().getSettings();
    if (settings.showCollections) {
        loadCollections();
    }
    if (settings.showGenres) {
        loadGenres();
    }
}

void LibrarySectionTab::loadCollections() {
    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> collections;

        if (client.fetchCollections(key, collections)) {
            brls::Logger::info("LibrarySectionTab: Got {} collections for section {}", collections.size(), key);

            brls::sync([this, collections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_collections = collections;
                m_collectionsLoaded = true;

                // Hide collections button if none available
                if (m_collections.empty() && m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: No collections for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_collectionsLoaded = true;
                if (m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibrarySectionTab::loadGenres() {
    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<GenreItem> genres;

        if (client.fetchGenreItems(key, genres) && !genres.empty()) {
            brls::Logger::info("LibrarySectionTab: Got {} genres for section {}", genres.size(), key);

            brls::sync([this, genres, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_genres = genres;
                m_genresLoaded = true;
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: No genres for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_genresLoaded = true;
                if (m_categoriesBtn) {
                    m_categoriesBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibrarySectionTab::showAllItems() {
    m_viewMode = LibraryViewMode::ALL_ITEMS;
    m_titleLabel->setText(m_title);
    m_contentGrid->setDataSource(m_items);
    updateViewModeButtons();
}

void LibrarySectionTab::showCollections() {
    if (!m_collectionsLoaded) {
        brls::Application::notify("Loading collections...");
        return;
    }

    if (m_collections.empty()) {
        brls::Application::notify("No collections available");
        return;
    }

    m_viewMode = LibraryViewMode::COLLECTIONS;
    m_titleLabel->setText(m_title + " - Collections");

    // Show collections in the grid
    m_contentGrid->setDataSource(m_collections);
    updateViewModeButtons();
}

void LibrarySectionTab::showCategories() {
    if (!m_genresLoaded) {
        brls::Application::notify("Loading categories...");
        return;
    }

    if (m_genres.empty()) {
        brls::Application::notify("No categories available");
        return;
    }

    m_viewMode = LibraryViewMode::CATEGORIES;
    m_titleLabel->setText(m_title + " - Categories");

    // Convert genres to MediaItem format for the grid
    std::vector<MediaItem> genreItems;
    for (const auto& genre : m_genres) {
        MediaItem item;
        item.title = genre.title;
        item.ratingKey = genre.key;  // Use genre key for filtering
        item.type = "genre";
        item.mediaType = MediaType::UNKNOWN;
        genreItems.push_back(item);
    }

    m_contentGrid->setDataSource(genreItems);
    updateViewModeButtons();
}

void LibrarySectionTab::updateViewModeButtons() {
    // Show/hide back button
    bool inFilteredView = (m_viewMode == LibraryViewMode::FILTERED);
    m_backBtn->setVisibility(inFilteredView ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    // Show/hide mode buttons
    bool showModeButtons = (m_viewMode != LibraryViewMode::FILTERED);
    m_allBtn->setVisibility(showModeButtons ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (m_collectionsBtn) {
        m_collectionsBtn->setVisibility(showModeButtons && !m_collections.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (m_categoriesBtn) {
        m_categoriesBtn->setVisibility(showModeButtons && !m_genres.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

void LibrarySectionTab::onItemSelected(const MediaItem& item) {
    // Handle selection based on current view mode
    if (m_viewMode == LibraryViewMode::COLLECTIONS) {
        // Selected a collection - show its contents
        onCollectionSelected(item);
        return;
    }

    if (m_viewMode == LibraryViewMode::CATEGORIES) {
        // Selected a category/genre - filter by it
        GenreItem genre;
        genre.title = item.title;
        genre.key = item.ratingKey;
        onGenreSelected(genre);
        return;
    }

    // Normal item selection
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void LibrarySectionTab::onCollectionSelected(const MediaItem& collection) {
    brls::Logger::debug("LibrarySectionTab: Selected collection: {}", collection.title);

    m_filterTitle = collection.title;
    std::string collectionKey = collection.ratingKey;
    std::string filterTitle = m_filterTitle;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, collectionKey, filterTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchChildren(collectionKey, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items in collection", items.size());

            brls::sync([this, items, filterTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryViewMode::FILTERED;
                m_titleLabel->setText(m_title + " - " + filterTitle);
                m_contentGrid->setDataSource(items);
                updateViewModeButtons();
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load collection content");
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load collection");
            });
        }
    });
}

void LibrarySectionTab::onGenreSelected(const GenreItem& genre) {
    brls::Logger::debug("LibrarySectionTab: Selected genre: {} (key: {})", genre.title, genre.key);

    m_filterTitle = genre.title;
    std::string key = m_sectionKey;
    std::string genreKey = genre.key;
    std::string genreTitle = genre.title;
    std::string filterTitle = m_filterTitle;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, genreKey, genreTitle, filterTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        // Try with genre key first, fall back to title
        if (client.fetchByGenreKey(key, genreKey, items) || client.fetchByGenre(key, genreTitle, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items for genre", items.size());

            brls::sync([this, items, filterTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryViewMode::FILTERED;
                m_titleLabel->setText(m_title + " - " + filterTitle);
                m_contentGrid->setDataSource(items);
                updateViewModeButtons();
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load genre content");
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load category");
            });
        }
    });
}

} // namespace vitaplex
