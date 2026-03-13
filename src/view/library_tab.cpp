/**
 * VitaPlex - Library Tab implementation
 */

#include "view/library_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/music_queue.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"

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

    // Sections row with horizontal scrolling
    m_sectionsScroll = new brls::HScrollingFrame();
    m_sectionsScroll->setHeight(50);
    m_sectionsScroll->setMarginBottom(15);

    m_sectionsBox = new brls::Box();
    m_sectionsBox->setAxis(brls::Axis::ROW);
    m_sectionsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_sectionsBox->setAlignItems(brls::AlignItems::CENTER);

    m_sectionsScroll->setContentView(m_sectionsBox);
    this->addView(m_sectionsScroll);

    const auto& settings = Application::getInstance().getSettings();

    // View mode buttons row (All / Collections / Categories / < Back)
    m_viewModeBox = new brls::Box();
    m_viewModeBox->setAxis(brls::Axis::ROW);
    m_viewModeBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_viewModeBox->setAlignItems(brls::AlignItems::CENTER);
    m_viewModeBox->setMarginBottom(15);

    m_allBtn = new brls::Button();
    m_allBtn->setText("All");
    m_allBtn->setMarginRight(10);
    styleButton(m_allBtn, true);
    m_allBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_allBtn);

    if (settings.showCollections) {
        m_collectionsBtn = new brls::Button();
        m_collectionsBtn->setText("Collections");
        m_collectionsBtn->setMarginRight(10);
        styleButton(m_collectionsBtn, false);
        m_collectionsBtn->registerClickAction([this](brls::View* view) {
            showCollections();
            return true;
        });
        m_viewModeBox->addView(m_collectionsBtn);
    }

    if (settings.showGenres) {
        m_categoriesBtn = new brls::Button();
        m_categoriesBtn->setText("Categories");
        m_categoriesBtn->setMarginRight(10);
        styleButton(m_categoriesBtn, false);
        m_categoriesBtn->registerClickAction([this](brls::View* view) {
            showCategories();
            return true;
        });
        m_viewModeBox->addView(m_categoriesBtn);
    }

    m_backBtn = new brls::Button();
    m_backBtn->setText("< Back");
    m_backBtn->setVisibility(brls::Visibility::GONE);
    styleButton(m_backBtn, false);
    m_backBtn->setBackgroundColor(nvgRGBA(80, 60, 50, 200));
    m_backBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_backBtn);

    // Only show view mode box if there are extra buttons beyond "All"
    if (settings.showCollections || settings.showGenres) {
        this->addView(m_viewModeBox);
    }

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    m_contentGrid->setOnItemStartAction([this](const MediaItem& item) {
        showAlbumContextMenu(item);
    });
    this->addView(m_contentGrid);

    // Load sections immediately
    brls::Logger::debug("LibraryTab: Loading sections...");
    loadSections();
}

LibraryTab::~LibraryTab() {
    if (m_alive) { *m_alive = false; }
}

void LibraryTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
}

void LibraryTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadSections();
    }
}

void LibraryTab::styleButton(brls::Button* btn, bool active) {
    btn->setCornerRadius(16);
    btn->setHighlightCornerRadius(16);
    btn->setPadding(6, 16, 6, 16);
    if (active) {
        btn->setBackgroundColor(nvgRGBA(70, 90, 210, 220));
        btn->setBorderColor(nvgRGBA(120, 160, 255, 200));
        btn->setBorderThickness(1.5f);
    } else {
        btn->setBackgroundColor(nvgRGBA(60, 60, 70, 180));
        btn->setBorderColor(nvgRGBA(0, 0, 0, 0));
        btn->setBorderThickness(0);
    }
}

void LibraryTab::updateSectionButtonStyles() {
    if (!m_sectionsBox) return;
    for (auto* child : m_sectionsBox->getChildren()) {
        auto* btn = dynamic_cast<brls::Button*>(child);
        if (btn) {
            styleButton(btn, btn == m_activeSectionBtn);
        }
    }
}

void LibraryTab::updateViewModeButtons() {
    bool inFilteredView = (m_viewMode == LibraryTabViewMode::FILTERED);
    m_backBtn->setVisibility(inFilteredView ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    bool showModeButtons = (m_viewMode != LibraryTabViewMode::FILTERED);
    m_allBtn->setVisibility(showModeButtons ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (m_collectionsBtn) {
        m_collectionsBtn->setVisibility(showModeButtons && !m_collections.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (m_categoriesBtn) {
        m_categoriesBtn->setVisibility(showModeButtons && !m_genres.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    // Update active styling
    if (showModeButtons) {
        styleButton(m_allBtn, m_viewMode == LibraryTabViewMode::ALL_ITEMS);
        if (m_collectionsBtn) styleButton(m_collectionsBtn, m_viewMode == LibraryTabViewMode::COLLECTIONS);
        if (m_categoriesBtn) styleButton(m_categoriesBtn, m_viewMode == LibraryTabViewMode::CATEGORIES);
    }
}

// Helper function to check if a library is hidden
static bool isLibraryHidden(const std::string& key, const std::string& hiddenLibraries) {
    if (hiddenLibraries.empty()) return false;

    std::string hidden = hiddenLibraries;
    size_t pos = 0;
    while ((pos = hidden.find(',')) != std::string::npos) {
        std::string hiddenKey = hidden.substr(0, pos);
        if (hiddenKey == key) return true;
        hidden.erase(0, pos + 1);
    }
    return (hidden == key);
}

void LibraryTab::loadSections() {
    brls::Logger::debug("LibraryTab::loadSections - Starting async load");

    asyncRun([this, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        brls::Logger::debug("LibraryTab: Fetching library sections (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<LibrarySection> sections;

        if (client.fetchLibrarySections(sections)) {
            brls::Logger::info("LibraryTab: Got {} sections", sections.size());

            // Get hidden libraries setting
            std::string hiddenLibraries = Application::getInstance().getSettings().hiddenLibraries;

            // Filter out hidden sections
            std::vector<LibrarySection> visibleSections;
            for (const auto& section : sections) {
                if (!isLibraryHidden(section.key, hiddenLibraries)) {
                    visibleSections.push_back(section);
                } else {
                    brls::Logger::debug("LibraryTab: Hiding section: {}", section.title);
                }
            }

            // Update UI on main thread
            brls::sync([this, visibleSections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_sections = visibleSections;
                m_sectionsBox->clearViews();

                for (const auto& section : m_sections) {
                    brls::Logger::debug("LibraryTab: Adding section button: {}", section.title);
                    auto* btn = new brls::Button();
                    btn->setText(section.title);
                    btn->setMarginRight(10);
                    styleButton(btn, false);

                    LibrarySection capturedSection = section;
                    btn->registerClickAction([this, capturedSection, btn](brls::View* view) {
                        m_activeSectionBtn = btn;
                        updateSectionButtonStyles();
                        onSectionSelected(capturedSection);
                        return true;
                    });

                    m_sectionsBox->addView(btn);
                }

                // Load first section by default
                if (!m_sections.empty()) {
                    brls::Logger::debug("LibraryTab: Loading first section: {}", m_sections[0].title);
                    if (!m_sectionsBox->getChildren().empty()) {
                        m_activeSectionBtn = dynamic_cast<brls::Button*>(m_sectionsBox->getChildren()[0]);
                        updateSectionButtonStyles();
                    }
                    onSectionSelected(m_sections[0]);
                }

                m_loaded = true;
                brls::Logger::debug("LibraryTab: Sections loading complete");
            });
        } else {
            brls::Logger::error("LibraryTab: Failed to fetch sections");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });
}

void LibraryTab::loadContent(const std::string& sectionKey) {
    brls::Logger::debug("LibraryTab::loadContent - section: {} (async)", sectionKey);

    std::string key = sectionKey;
    std::string sectionType = m_currentSectionType;
    asyncRun([this, key, sectionType, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        // Use type-specific metadata type for proper content loading
        int metadataType = 0;
        if (sectionType == "movie") metadataType = 1;
        else if (sectionType == "show") metadataType = 2;
        else if (sectionType == "artist") metadataType = 8;

        if (client.fetchLibraryContent(key, items, metadataType)) {
            brls::Logger::info("LibraryTab: Got {} items for section {}", items.size(), key);

            // Update UI on main thread
            brls::sync([this, items, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_items = items;
                if (m_viewMode == LibraryTabViewMode::ALL_ITEMS) {
                    m_contentGrid->setDataSource(m_items);
                }
            });
        } else {
            brls::Logger::error("LibraryTab: Failed to load content for section {}", key);
        }
    });
}

void LibraryTab::loadCollections(const std::string& sectionKey) {
    std::string key = sectionKey;
    asyncRun([this, key, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> collections;

        if (client.fetchCollections(key, collections)) {
            brls::Logger::info("LibraryTab: Got {} collections for section {}", collections.size(), key);

            brls::sync([this, collections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_collections = collections;
                m_collectionsLoaded = true;

                if (m_collections.empty() && m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::GONE);
                } else if (m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::VISIBLE);
                }
            });
        } else {
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_collectionsLoaded = true;
                m_collections.clear();
                if (m_collectionsBtn) {
                    m_collectionsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibraryTab::loadGenres(const std::string& sectionKey) {
    std::string key = sectionKey;
    asyncRun([this, key, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<GenreItem> genres;

        if (client.fetchGenreItems(key, genres) && !genres.empty()) {
            brls::Logger::info("LibraryTab: Got {} genres for section {}", genres.size(), key);

            brls::sync([this, genres, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_genres = genres;
                m_genresLoaded = true;
            });
        } else {
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_genresLoaded = true;
                m_genres.clear();
                if (m_categoriesBtn) {
                    m_categoriesBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibraryTab::onSectionSelected(const LibrarySection& section) {
    m_currentSection = section.key;
    m_currentSectionType = section.type;
    m_titleLabel->setText("Library - " + section.title);

    // Reset view mode to All when switching sections
    m_viewMode = LibraryTabViewMode::ALL_ITEMS;
    m_collectionsLoaded = false;
    m_genresLoaded = false;
    m_collections.clear();
    m_genres.clear();

    loadContent(section.key);

    // Preload collections and genres for the new section
    const auto& settings = Application::getInstance().getSettings();
    if (settings.showCollections) {
        loadCollections(section.key);
    }
    if (settings.showGenres) {
        loadGenres(section.key);
    }

    updateViewModeButtons();
}

void LibraryTab::showAllItems() {
    m_viewMode = LibraryTabViewMode::ALL_ITEMS;

    // Find the current section title
    for (const auto& sec : m_sections) {
        if (sec.key == m_currentSection) {
            m_titleLabel->setText("Library - " + sec.title);
            break;
        }
    }

    m_contentGrid->setDataSource(m_items);
    updateViewModeButtons();
}

void LibraryTab::showCollections() {
    if (!m_collectionsLoaded) {
        brls::Application::notify("Loading collections...");
        return;
    }
    if (m_collections.empty()) {
        brls::Application::notify("No collections available");
        return;
    }

    m_viewMode = LibraryTabViewMode::COLLECTIONS;

    for (const auto& sec : m_sections) {
        if (sec.key == m_currentSection) {
            m_titleLabel->setText("Library - " + sec.title + " - Collections");
            break;
        }
    }

    m_contentGrid->setDataSource(m_collections);
    updateViewModeButtons();
}

void LibraryTab::showCategories() {
    if (!m_genresLoaded) {
        brls::Application::notify("Loading categories...");
        return;
    }
    if (m_genres.empty()) {
        brls::Application::notify("No categories available");
        return;
    }

    m_viewMode = LibraryTabViewMode::CATEGORIES;

    for (const auto& sec : m_sections) {
        if (sec.key == m_currentSection) {
            m_titleLabel->setText("Library - " + sec.title + " - Categories");
            break;
        }
    }

    // Convert genres to MediaItem format for the grid
    std::vector<MediaItem> genreItems;
    for (const auto& genre : m_genres) {
        MediaItem item;
        item.title = genre.title;
        item.ratingKey = genre.key;
        item.type = "genre";
        item.mediaType = MediaType::UNKNOWN;
        genreItems.push_back(item);
    }

    m_contentGrid->setDataSource(genreItems);
    updateViewModeButtons();
}

void LibraryTab::onItemSelected(const MediaItem& item) {
    // Handle selection based on current view mode
    if (m_viewMode == LibraryTabViewMode::COLLECTIONS) {
        onCollectionSelected(item);
        return;
    }

    if (m_viewMode == LibraryTabViewMode::CATEGORIES) {
        GenreItem genre;
        genre.title = item.title;
        genre.key = item.ratingKey;
        onGenreSelected(genre);
        return;
    }

    // For tracks, play directly instead of showing detail view
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void LibraryTab::onCollectionSelected(const MediaItem& collection) {
    brls::Logger::debug("LibraryTab: Selected collection: {}", collection.title);

    m_filterTitle = collection.title;
    std::string collectionKey = collection.ratingKey;
    std::string filterTitle = m_filterTitle;

    asyncRun([this, collectionKey, filterTitle, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchChildren(collectionKey, items)) {
            brls::Logger::info("LibraryTab: Got {} items in collection", items.size());

            brls::sync([this, items, filterTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryTabViewMode::FILTERED;
                m_titleLabel->setText("Library - " + filterTitle);
                m_contentGrid->setDataSource(items);
                updateViewModeButtons();
            });
        } else {
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load collection");
            });
        }
    });
}

void LibraryTab::onGenreSelected(const GenreItem& genre) {
    brls::Logger::debug("LibraryTab: Selected genre: {} (key: {})", genre.title, genre.key);

    m_filterTitle = genre.title;
    std::string key = m_currentSection;
    std::string genreKey = genre.key;
    std::string genreTitle = genre.title;
    std::string filterTitle = m_filterTitle;
    std::string secType = m_currentSectionType;

    asyncRun([this, key, genreKey, genreTitle, filterTitle, secType, aliveWeak = std::weak_ptr<bool>(m_alive)]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        int metadataType = 0;
        if (secType == "movie") metadataType = 1;
        else if (secType == "show") metadataType = 2;
        else if (secType == "artist") metadataType = 8;

        if (client.fetchByGenreKey(key, genreKey, items, metadataType) || client.fetchByGenre(key, genreTitle, items, metadataType)) {
            brls::Logger::info("LibraryTab: Got {} items for genre", items.size());

            brls::sync([this, items, filterTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryTabViewMode::FILTERED;
                m_titleLabel->setText("Library - " + filterTitle);
                m_contentGrid->setDataSource(items);
                updateViewModeButtons();
            });
        } else {
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load category");
            });
        }
    });
}

void LibraryTab::showAlbumContextMenu(const MediaItem& album) {
    auto* dialog = new brls::Dialog(album.title);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    MediaItem capturedAlbum = album;

    addDialogButton("Play Now (Clear Queue)", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks) && !tracks.empty()) {
                brls::sync([tracks]() {
                    auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                    brls::Application::pushActivity(playerActivity);
                });
            }
        });
        return true;
    });

    addDialogButton("Play Next", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        for (int i = (int)tracks.size() - 1; i >= 0; i--) {
                            queue.insertTrackAfterCurrent(tracks[i]);
                        }
                        brls::Application::notify("Album queued next");
                    }
                });
            }
        });
        return true;
    });

    addDialogButton("Add to Bottom of Queue", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> tracks;
            if (client.fetchChildren(capturedAlbum.ratingKey, tracks)) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        queue.addTracks(tracks);
                        brls::Application::notify("Album added to queue");
                    }
                });
            }
        });
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    brls::Application::pushActivity(new brls::Activity(dialog));
}

} // namespace vitaplex
