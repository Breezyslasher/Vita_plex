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

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Scrollable main container
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_mainContainer = new brls::Box();
    m_mainContainer->setAxis(brls::Axis::COLUMN);
    m_mainContainer->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_mainContainer->setAlignItems(brls::AlignItems::STRETCH);
    m_mainContainer->setPadding(20);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    m_mainContainer->addView(m_titleLabel);

    const auto& settings = Application::getInstance().getSettings();

    // Collections row (hidden by default, shown when data loads)
    if (settings.showCollections) {
        m_collectionsRow = createHorizontalRow("Collections");
        m_collectionsRow->setVisibility(brls::Visibility::GONE);
        m_mainContainer->addView(m_collectionsRow);
    }

    // Genres row (hidden by default, shown when data loads)
    if (settings.showGenres) {
        m_genresRow = createHorizontalRow("Categories");
        m_genresRow->setVisibility(brls::Visibility::GONE);
        m_mainContainer->addView(m_genresRow);
    }

    // "All Items" label
    auto* allItemsLabel = new brls::Label();
    allItemsLabel->setText("All Items");
    allItemsLabel->setFontSize(22);
    allItemsLabel->setMarginTop(10);
    allItemsLabel->setMarginBottom(10);
    m_mainContainer->addView(allItemsLabel);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setHeight(400);  // Fixed height for scrolling
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    m_mainContainer->addView(m_contentGrid);

    m_scrollView->setContentView(m_mainContainer);
    this->addView(m_scrollView);

    // Load content immediately
    brls::Logger::debug("LibrarySectionTab: Created for section {} ({}) type={}", m_sectionKey, m_title, m_sectionType);
    loadContent();
}

brls::Box* LibrarySectionTab::createHorizontalRow(const std::string& title) {
    auto* rowBox = new brls::Box();
    rowBox->setAxis(brls::Axis::COLUMN);
    rowBox->setMarginBottom(15);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(title);
    titleLabel->setFontSize(22);
    titleLabel->setMarginBottom(10);
    rowBox->addView(titleLabel);

    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(120);

    auto* container = new brls::Box();
    container->setAxis(brls::Axis::ROW);
    container->setJustifyContent(brls::JustifyContent::FLEX_START);
    container->setAlignItems(brls::AlignItems::CENTER);

    scrollFrame->setContentView(container);
    rowBox->addView(scrollFrame);

    // Store container reference based on title
    if (title == "Collections") {
        m_collectionsContainer = container;
    } else if (title == "Categories") {
        m_genresContainer = container;
    }

    return rowBox;
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

    // Also load collections and genres if enabled
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
    asyncRun([this, key]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> collections;

        if (client.fetchCollections(key, collections) && !collections.empty()) {
            brls::Logger::info("LibrarySectionTab: Got {} collections for section {}", collections.size(), key);

            brls::sync([this, collections]() {
                m_collections = collections;
                if (m_collectionsContainer && m_collectionsRow) {
                    m_collectionsContainer->clearViews();

                    for (const auto& collection : m_collections) {
                        auto* btn = new brls::Button();
                        btn->setText(collection.title);
                        btn->setMarginRight(10);
                        btn->setHeight(40);

                        MediaItem capturedCollection = collection;
                        btn->registerClickAction([this, capturedCollection](brls::View* view) {
                            onCollectionSelected(capturedCollection);
                            return true;
                        });

                        m_collectionsContainer->addView(btn);
                    }

                    m_collectionsRow->setVisibility(brls::Visibility::VISIBLE);
                }
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: No collections for section {}", key);
        }
    });
}

void LibrarySectionTab::loadGenres() {
    std::string key = m_sectionKey;
    asyncRun([this, key]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<std::string> genres;

        if (client.fetchGenres(key, genres) && !genres.empty()) {
            brls::Logger::info("LibrarySectionTab: Got {} genres for section {}", genres.size(), key);

            brls::sync([this, genres]() {
                m_genres = genres;
                if (m_genresContainer && m_genresRow) {
                    m_genresContainer->clearViews();

                    for (const auto& genre : m_genres) {
                        auto* btn = new brls::Button();
                        btn->setText(genre);
                        btn->setMarginRight(10);
                        btn->setHeight(40);

                        std::string capturedGenre = genre;
                        btn->registerClickAction([this, capturedGenre](brls::View* view) {
                            onGenreSelected(capturedGenre);
                            return true;
                        });

                        m_genresContainer->addView(btn);
                    }

                    m_genresRow->setVisibility(brls::Visibility::VISIBLE);
                }
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: No genres for section {}", key);
        }
    });
}

void LibrarySectionTab::onItemSelected(const MediaItem& item) {
    // For tracks, play directly instead of showing detail view
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

    // Fetch collection children and display them
    std::string collectionKey = collection.ratingKey;
    std::string collectionTitle = collection.title;

    asyncRun([this, collectionKey, collectionTitle]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchChildren(collectionKey, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items in collection", items.size());

            brls::sync([this, items, collectionTitle]() {
                m_titleLabel->setText(m_title + " - " + collectionTitle);
                m_items = items;
                m_contentGrid->setDataSource(m_items);
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load collection content");
        }
    });
}

void LibrarySectionTab::onGenreSelected(const std::string& genre) {
    brls::Logger::debug("LibrarySectionTab: Selected genre: {}", genre);

    std::string key = m_sectionKey;
    std::string genreCopy = genre;

    asyncRun([this, key, genreCopy]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchByGenre(key, genreCopy, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items for genre {}", items.size(), genreCopy);

            brls::sync([this, items, genreCopy]() {
                m_titleLabel->setText(m_title + " - " + genreCopy);
                m_items = items;
                m_contentGrid->setDataSource(m_items);
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load genre content");
        }
    });
}

} // namespace vitaplex
