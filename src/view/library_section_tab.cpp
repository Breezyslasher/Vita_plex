/**
 * VitaPlex - Library Section Tab implementation
 */

#include "view/library_section_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "utils/image_loader.hpp"
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

    // Playlists button (only for music/artist sections)
    if (settings.showPlaylists && sectionType == "artist") {
        m_playlistsBtn = new brls::Button();
        m_playlistsBtn->setText("Playlists");
        m_playlistsBtn->setMarginRight(10);
        m_playlistsBtn->registerClickAction([this](brls::View* view) {
            showPlaylists();
            return true;
        });
        m_viewModeBox->addView(m_playlistsBtn);
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

void LibrarySectionTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
}

void LibrarySectionTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadContent();
    }
}

void LibrarySectionTab::loadContent() {
    brls::Logger::debug("LibrarySectionTab::loadContent - section: {} (async)", m_sectionKey);

    std::string key = m_sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;  // Capture weak_ptr for async safety

    std::string sectionType = m_sectionType;
    asyncRun([this, key, sectionType, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        // Plex type codes: 1=movie, 2=show, 8=artist, 9=album, 10=track
        int metadataType = 0;
        if (sectionType == "movie") metadataType = 1;
        else if (sectionType == "show") metadataType = 2;
        else if (sectionType == "artist") metadataType = 8;

        if (client.fetchLibraryContent(key, items, metadataType)) {
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
    if (settings.showPlaylists && m_sectionType == "artist") {
        loadPlaylists();
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
    if (m_playlistsBtn) {
        m_playlistsBtn->setVisibility(showModeButtons && !m_playlists.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
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

    if (m_viewMode == LibraryViewMode::PLAYLISTS) {
        // Selected a playlist - show its contents
        for (const auto& playlist : m_playlists) {
            if (playlist.ratingKey == item.ratingKey) {
                onPlaylistSelected(playlist);
                return;
            }
        }
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

    std::string secType = m_sectionType;
    asyncRun([this, key, genreKey, genreTitle, filterTitle, secType, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        // Plex type codes: 1=movie, 2=show, 8=artist
        int metadataType = 0;
        if (secType == "movie") metadataType = 1;
        else if (secType == "show") metadataType = 2;
        else if (secType == "artist") metadataType = 8;

        // Try with genre key first, fall back to title
        if (client.fetchByGenreKey(key, genreKey, items, metadataType) || client.fetchByGenre(key, genreTitle, items, metadataType)) {
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

void LibrarySectionTab::loadPlaylists() {
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<Playlist> playlists;

        if (client.fetchMusicPlaylists(playlists)) {
            brls::Logger::info("LibrarySectionTab: Got {} music playlists", playlists.size());

            brls::sync([this, playlists, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_playlists = playlists;
                m_playlistsLoaded = true;

                if (m_playlists.empty() && m_playlistsBtn) {
                    m_playlistsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        } else {
            brls::Logger::debug("LibrarySectionTab: Failed to load playlists or none found");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_playlistsLoaded = true;
                if (m_playlistsBtn) {
                    m_playlistsBtn->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void LibrarySectionTab::showPlaylists() {
    if (!m_playlistsLoaded) {
        brls::Application::notify("Loading playlists...");
        return;
    }

    if (m_playlists.empty()) {
        brls::Application::notify("No playlists available");
        return;
    }

    m_viewMode = LibraryViewMode::PLAYLISTS;
    m_titleLabel->setText(m_title + " - Playlists");

    // Convert playlists to MediaItem format for the grid
    std::vector<MediaItem> playlistItems;
    for (size_t i = 0; i < m_playlists.size(); i++) {
        MediaItem item;
        item.title = m_playlists[i].title;
        if (m_playlists[i].leafCount > 0) {
            item.title += " (" + std::to_string(m_playlists[i].leafCount) + " tracks)";
        }
        item.ratingKey = m_playlists[i].ratingKey;
        item.thumb = m_playlists[i].composite.empty() ? m_playlists[i].thumb : m_playlists[i].composite;
        item.type = "playlist";
        item.mediaType = MediaType::UNKNOWN;
        playlistItems.push_back(item);
    }

    m_contentGrid->setDataSource(playlistItems);
    updateViewModeButtons();
}

void LibrarySectionTab::onPlaylistSelected(const Playlist& playlist) {
    brls::Logger::debug("LibrarySectionTab: Selected playlist: {}", playlist.title);

    std::string playlistId = playlist.ratingKey;
    std::string playlistTitle = playlist.title;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, playlistId, playlistTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<PlaylistItem> items;

        if (client.fetchPlaylistItems(playlistId, items)) {
            brls::Logger::info("LibrarySectionTab: Got {} items in playlist", items.size());

            std::vector<MediaItem> mediaItems;
            for (const auto& item : items) {
                mediaItems.push_back(item.media);
            }

            brls::sync([this, mediaItems, playlistTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_viewMode = LibraryViewMode::FILTERED;
                m_titleLabel->setText(m_title + " - " + playlistTitle);
                m_contentGrid->setDataSource(mediaItems);
                updateViewModeButtons();
            });
        } else {
            brls::Logger::error("LibrarySectionTab: Failed to load playlist content");
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load playlist");
            });
        }
    });
}

void LibrarySectionTab::showPlaylistOptionsDialog(const Playlist& playlist) {
    brls::Dialog* dialog = new brls::Dialog(playlist.title);

    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setPadding(20);

    auto* trackCount = new brls::Label();
    trackCount->setText("Tracks: " + std::to_string(playlist.leafCount));
    trackCount->setMarginBottom(10);
    infoBox->addView(trackCount);

    if (playlist.smart) {
        auto* smartLabel = new brls::Label();
        smartLabel->setText("(Smart Playlist - cannot be edited)");
        smartLabel->setFontSize(14);
        smartLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
        infoBox->addView(smartLabel);
    }

    dialog->addView(infoBox);

    std::string playlistId = playlist.ratingKey;
    dialog->addButton("Play All", [this, playlistId]() {
        playPlaylistWithQueue(playlistId, 0);
    });

    if (!playlist.smart) {
        dialog->addButton("Delete", [this, playlistId]() {
            brls::Dialog* confirmDialog = new brls::Dialog("Delete this playlist?");
            confirmDialog->addButton("Yes, Delete", [this, playlistId]() {
                std::weak_ptr<bool> aliveWeak = m_alive;

                asyncRun([this, playlistId, aliveWeak]() {
                    PlexClient& client = PlexClient::getInstance();

                    if (client.deletePlaylist(playlistId)) {
                        brls::Logger::info("LibrarySectionTab: Deleted playlist");

                        brls::sync([this, aliveWeak]() {
                            auto alive = aliveWeak.lock();
                            if (!alive || !*alive) return;

                            loadPlaylists();
                            showAllItems();
                        });
                    }
                });
            });
            confirmDialog->addButton("Cancel", []() {});
            confirmDialog->open();
        });
    }

    dialog->addButton("Cancel", []() {});
    dialog->open();
}

void LibrarySectionTab::playPlaylistWithQueue(const std::string& playlistId, int startIndex) {
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, playlistId, startIndex, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<PlaylistItem> items;

        if (client.fetchPlaylistItems(playlistId, items) && !items.empty()) {
            std::vector<MediaItem> tracks;
            for (const auto& item : items) {
                tracks.push_back(item.media);
            }

            brls::sync([tracks, startIndex, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(tracks, startIndex)
                );
            });
        }
    });
}

} // namespace vitaplex
