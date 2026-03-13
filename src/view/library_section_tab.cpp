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
#include "app/music_queue.hpp"
#include "app/downloads_manager.hpp"

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
    styleButton(m_allBtn, true);  // Active by default
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
        styleButton(m_collectionsBtn, false);
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
        styleButton(m_categoriesBtn, false);
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
        styleButton(m_playlistsBtn, false);
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
    styleButton(m_backBtn, false);
    m_backBtn->setBackgroundColor(nvgRGBA(80, 60, 50, 200));
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
    m_contentGrid->setOnItemStartAction([this](const MediaItem& item) {
        if (item.type == "playlist") {
            // Find the matching Playlist struct for full metadata
            for (const auto& playlist : m_playlists) {
                if (playlist.ratingKey == item.ratingKey) {
                    showPlaylistContextMenu(playlist);
                    return;
                }
            }
        }
    });
    this->addView(m_contentGrid);

    // Track list view (for playlist contents - hidden by default)
    m_trackListScroll = new brls::ScrollingFrame();
    m_trackListScroll->setGrow(1.0f);
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_trackListBox = new brls::Box();
    m_trackListBox->setAxis(brls::Axis::COLUMN);
    m_trackListBox->setPadding(5);
    m_trackListScroll->setContentView(m_trackListBox);
    this->addView(m_trackListScroll);

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
                // Only update grid if still in default view (user may have
                // switched to playlists/collections/categories while loading)
                if (m_viewMode == LibraryViewMode::ALL_ITEMS) {
                    m_contentGrid->setDataSource(m_items);
                }
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
    // Free playlist tracks memory when leaving playlist view
    m_playlistTracks.clear();
    m_playlistTracks.shrink_to_fit();
    m_currentPlaylistId.clear();
    m_trackListRendered = 0;
    m_trackListLoadMoreBtn = nullptr;
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
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
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);

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
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);

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

void LibrarySectionTab::styleButton(brls::Button* btn, bool active) {
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

    // Update active styling on mode buttons
    if (showModeButtons) {
        styleButton(m_allBtn, m_viewMode == LibraryViewMode::ALL_ITEMS);
        if (m_collectionsBtn) styleButton(m_collectionsBtn, m_viewMode == LibraryViewMode::COLLECTIONS);
        if (m_categoriesBtn) styleButton(m_categoriesBtn, m_viewMode == LibraryViewMode::CATEGORIES);
        if (m_playlistsBtn) styleButton(m_playlistsBtn, m_viewMode == LibraryViewMode::PLAYLISTS);
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
                m_trackListScroll->setVisibility(brls::Visibility::GONE);
                m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
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
                m_trackListScroll->setVisibility(brls::Visibility::GONE);
                m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
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
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);

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

            // Build MediaItem vector, then move into sync lambda to avoid extra copy
            auto mediaItems = std::make_shared<std::vector<MediaItem>>();
            mediaItems->reserve(items.size());
            for (auto& item : items) {
                mediaItems->push_back(std::move(item.media));
            }

            brls::sync([this, mediaItems, playlistTitle, playlistId, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                showPlaylistTrackList(std::move(*mediaItems), playlistTitle, playlistId);
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

void LibrarySectionTab::showPlaylistTrackList(std::vector<MediaItem>&& tracks,
                                                const std::string& playlistTitle,
                                                const std::string& playlistId) {
    m_viewMode = LibraryViewMode::FILTERED;
    m_titleLabel->setText(m_title + " - " + playlistTitle + " (" + std::to_string(tracks.size()) + " tracks)");
    updateViewModeButtons();

    // Store tracks as member to avoid per-row copies (was causing OOM with large playlists)
    m_playlistTracks = std::move(tracks);
    m_currentPlaylistId = playlistId;
    m_trackListRendered = 0;
    m_trackListLoadMoreBtn = nullptr;

    // Hide grid, show track list
    m_contentGrid->setVisibility(brls::Visibility::GONE);
    m_trackListScroll->setVisibility(brls::Visibility::VISIBLE);
    m_trackListBox->clearViews();

    // Render first page of tracks
    appendTrackListPage();
}

void LibrarySectionTab::appendTrackListPage() {
    // Remove existing "Load More" button
    if (m_trackListLoadMoreBtn) {
        m_trackListBox->removeView(m_trackListLoadMoreBtn);
        m_trackListLoadMoreBtn = nullptr;
    }

    size_t end = std::min(m_trackListRendered + TRACK_LIST_PAGE_SIZE, m_playlistTracks.size());

    for (size_t i = m_trackListRendered; i < end; i++) {
        const auto& track = m_playlistTracks[i];

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(48);
        row->setPadding(8, 12, 8, 12);
        row->setMarginBottom(3);
        row->setCornerRadius(6);
        row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
        row->setFocusable(true);

        // Left side: track number + artist + title
        auto* leftBox = new brls::Box();
        leftBox->setAxis(brls::Axis::ROW);
        leftBox->setAlignItems(brls::AlignItems::CENTER);
        leftBox->setGrow(1.0f);

        auto* trackNum = new brls::Label();
        trackNum->setFontSize(13);
        trackNum->setMarginRight(10);
        trackNum->setTextColor(nvgRGBA(150, 150, 150, 255));
        trackNum->setText(std::to_string(i + 1));
        leftBox->addView(trackNum);

        auto* titleLabel = new brls::Label();
        titleLabel->setFontSize(13);
        std::string displayTitle = track.title;
        if (!track.grandparentTitle.empty()) {
            displayTitle = track.grandparentTitle + " - " + displayTitle;
        }
        // Truncate for Vita screen
        if (displayTitle.length() > 55) {
            displayTitle = displayTitle.substr(0, 52) + "...";
        }
        titleLabel->setText(displayTitle);
        leftBox->addView(titleLabel);

        row->addView(leftBox);

        // Right side: duration
        if (track.duration > 0) {
            auto* durLabel = new brls::Label();
            durLabel->setFontSize(12);
            durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
            int totalSec = track.duration / 1000;
            int min = totalSec / 60;
            int sec = totalSec % 60;
            char durStr[16];
            snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
            durLabel->setText(durStr);
            row->addView(durLabel);
        }

        // Click to play track - only capture the index, reference m_playlistTracks via 'this'
        size_t idx = i;
        row->registerClickAction([this, idx](brls::View*) {
            performPlaylistTrackAction(idx);
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

        m_trackListBox->addView(row);
    }

    m_trackListRendered = end;

    // Add "Load More" button if there are remaining tracks
    if (m_trackListRendered < m_playlistTracks.size()) {
        size_t remaining = m_playlistTracks.size() - m_trackListRendered;
        m_trackListLoadMoreBtn = new brls::Button();
        auto* label = new brls::Label();
        label->setText("Load More (" + std::to_string(remaining) + " remaining)");
        label->setFontSize(16);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_trackListLoadMoreBtn->addView(label);
        m_trackListLoadMoreBtn->setMarginTop(10);
        m_trackListLoadMoreBtn->setHeight(44);
        m_trackListLoadMoreBtn->registerClickAction([this](brls::View*) {
            appendTrackListPage();
            return true;
        });
        m_trackListLoadMoreBtn->addGestureRecognizer(new brls::TapGestureRecognizer(m_trackListLoadMoreBtn));
        m_trackListBox->addView(m_trackListLoadMoreBtn);
    }
}

void LibrarySectionTab::performPlaylistTrackAction(size_t trackIndex) {
    if (trackIndex >= m_playlistTracks.size()) return;

    const MediaItem& track = m_playlistTracks[trackIndex];
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;
    MusicQueue& queue = MusicQueue::getInstance();

    auto executeAction = [this, &track, trackIndex, &queue](TrackDefaultAction act) {
        switch (act) {
            case TrackDefaultAction::PLAY_NOW_CLEAR:
            default:
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                break;

            case TrackDefaultAction::PLAY_NEXT:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.insertTrackAfterCurrent(track);
                    brls::Application::notify("Playing next: " + track.title);
                }
                break;

            case TrackDefaultAction::ADD_TO_BOTTOM:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.addTrack(track);
                    brls::Application::notify("Added to queue: " + track.title);
                }
                break;

            case TrackDefaultAction::PLAY_NOW_REPLACE:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.insertTrackAfterCurrent(track);
                    if (queue.playNext()) {
                        brls::Application::notify("Now playing: " + track.title);
                    }
                }
                break;

            case TrackDefaultAction::ASK_EACH_TIME:
                break;  // Handled below
        }
    };

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        auto* dialog = new brls::Dialog("Choose Action");
        auto* optionsBox = new brls::Box();
        optionsBox->setAxis(brls::Axis::COLUMN);
        optionsBox->setPadding(20);

        auto addBtn = [this, trackIndex, dialog, &optionsBox](const std::string& text, TrackDefaultAction act) {
            auto* btn = new brls::Button();
            btn->setText(text);
            btn->setHeight(44);
            btn->setMarginBottom(10);
            btn->registerClickAction([this, dialog, trackIndex, act](brls::View*) {
                dialog->dismiss();
                if (trackIndex >= m_playlistTracks.size()) return true;
                const MediaItem& t = m_playlistTracks[trackIndex];
                if (act == TrackDefaultAction::PLAY_NOW_CLEAR) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else if (act == TrackDefaultAction::PLAY_NEXT) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.insertTrackAfterCurrent(t);
                        brls::Application::notify("Playing next: " + t.title);
                    }
                } else if (act == TrackDefaultAction::ADD_TO_BOTTOM) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.addTrack(t);
                        brls::Application::notify("Added to queue: " + t.title);
                    }
                } else if (act == TrackDefaultAction::PLAY_NOW_REPLACE) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.insertTrackAfterCurrent(t);
                        if (q.playNext()) {
                            brls::Application::notify("Now playing: " + t.title);
                        }
                    }
                }
                return true;
            });
            btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
            optionsBox->addView(btn);
        };

        addBtn("Play Playlist from Here", TrackDefaultAction::PLAY_NOW_CLEAR);
        addBtn("Play Next", TrackDefaultAction::PLAY_NEXT);
        addBtn("Add to Bottom of Queue", TrackDefaultAction::ADD_TO_BOTTOM);
        addBtn("Play Now (Replace Current)", TrackDefaultAction::PLAY_NOW_REPLACE);

        dialog->addView(optionsBox);
        dialog->open();
    } else {
        executeAction(action);
    }
}

void LibrarySectionTab::showPlaylistContextMenu(const Playlist& playlist) {
    auto* dialog = new brls::Dialog(playlist.title);

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

    Playlist capturedPlaylist = playlist;

    addDialogButton("Play Now (Clear Queue)", [this, capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        playPlaylistWithQueue(capturedPlaylist.ratingKey, 0);
        return true;
    });

    addDialogButton("Add to Queue", [capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        std::string playlistId = capturedPlaylist.ratingKey;
        asyncRun([playlistId]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<PlaylistItem> items;
            if (client.fetchPlaylistItems(playlistId, items) && !items.empty()) {
                std::vector<MediaItem> tracks;
                for (const auto& item : items) {
                    tracks.push_back(item.media);
                }
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(tracks, 0));
                    } else {
                        queue.addTracks(tracks);
                        brls::Application::notify("Playlist added to queue");
                    }
                });
            } else {
                brls::sync([]() {
                    brls::Application::notify("Cannot queue playlist - server unreachable");
                });
            }
        });
        return true;
    });

    addDialogButton("Download", [capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        std::string playlistId = capturedPlaylist.ratingKey;
        std::string playlistTitle = capturedPlaylist.title;
        std::string playlistThumb = capturedPlaylist.thumb.empty() ? capturedPlaylist.composite : capturedPlaylist.thumb;
        asyncRun([playlistId, playlistTitle, playlistThumb]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<PlaylistItem> items;
            int queued = 0;
            int skipped = 0;

            if (client.fetchPlaylistItems(playlistId, items)) {
                auto& mgr = DownloadsManager::getInstance();
                for (const auto& item : items) {
                    // Skip items already downloaded or in queue
                    if (mgr.isDownloaded(item.media.ratingKey) ||
                        mgr.getDownload(item.media.ratingKey) != nullptr) {
                        skipped++;
                        continue;
                    }
                    MediaItem fullItem;
                    if (client.fetchMediaDetails(item.media.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                        if (mgr.queueDownload(
                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                            fullItem.duration, "track",
                            playlistTitle, 0, fullItem.index,
                            fullItem.thumb,
                            DownloadGroupType::PLAYLIST, playlistId,
                            playlistTitle, playlistThumb)) {
                            queued++;
                        }
                    }
                }
            }

            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued, skipped]() {
                std::string msg = "Queued " + std::to_string(queued) + " tracks for download";
                if (skipped > 0) {
                    msg += " (" + std::to_string(skipped) + " already downloaded)";
                }
                brls::Application::notify(msg);
            });
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
        } else {
            brls::sync([]() {
                brls::Application::notify("Cannot play playlist - server unreachable");
            });
        }
    });
}

} // namespace vitaplex
