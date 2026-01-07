/**
 * VitaPlex - Music Tab implementation
 */

#include "view/music_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "utils/async.hpp"

namespace vitaplex {

MusicTab::MusicTab() {
    // Create alive flag for async callback safety
    m_alive = std::make_shared<bool>(true);

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
    m_titleLabel->setText("Music");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    m_mainContainer->addView(m_titleLabel);

    // Sections row with horizontal scrolling
    m_sectionsScroll = new brls::HScrollingFrame();
    m_sectionsScroll->setHeight(50);
    m_sectionsScroll->setMarginBottom(20);

    m_sectionsBox = new brls::Box();
    m_sectionsBox->setAxis(brls::Axis::ROW);
    m_sectionsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_sectionsBox->setAlignItems(brls::AlignItems::CENTER);

    m_sectionsScroll->setContentView(m_sectionsBox);
    m_mainContainer->addView(m_sectionsScroll);

    const auto& settings = Application::getInstance().getSettings();

    // Playlists row (hidden by default, shown when data loads)
    if (settings.showPlaylists) {
        m_playlistsRow = createHorizontalRow("Playlists");
        m_playlistsRow->setVisibility(brls::Visibility::GONE);
        m_mainContainer->addView(m_playlistsRow);
    }

    // Collections row (hidden by default, shown when data loads)
    if (settings.showCollections) {
        m_collectionsRow = createHorizontalRow("Collections");
        m_collectionsRow->setVisibility(brls::Visibility::GONE);
        m_mainContainer->addView(m_collectionsRow);
    }

    // "Artists" label
    auto* artistsLabel = new brls::Label();
    artistsLabel->setText("Artists");
    artistsLabel->setFontSize(22);
    artistsLabel->setMarginTop(10);
    artistsLabel->setMarginBottom(10);
    m_mainContainer->addView(artistsLabel);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setHeight(400);
    m_contentGrid->setOnItemSelected([this](const MediaItem& item) {
        onItemSelected(item);
    });
    m_mainContainer->addView(m_contentGrid);

    m_scrollView->setContentView(m_mainContainer);
    this->addView(m_scrollView);

    // Load sections immediately
    brls::Logger::debug("MusicTab: Loading sections...");
    loadSections();
}

MusicTab::~MusicTab() {
    // Mark as no longer alive to prevent async callbacks from updating destroyed UI
    if (m_alive) {
        *m_alive = false;
    }
    brls::Logger::debug("MusicTab: Destroyed");
}

brls::Box* MusicTab::createHorizontalRow(const std::string& title) {
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
    if (title == "Playlists") {
        m_playlistsContainer = container;
    } else if (title == "Collections") {
        m_collectionsContainer = container;
    }

    return rowBox;
}

void MusicTab::onFocusGained() {
    brls::Box::onFocusGained();

    if (!m_loaded) {
        loadSections();
    }
}

void MusicTab::loadSections() {
    brls::Logger::debug("MusicTab::loadSections - Starting async load");

    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, aliveWeak]() {
        brls::Logger::debug("MusicTab: Fetching library sections (async)...");
        PlexClient& client = PlexClient::getInstance();
        std::vector<LibrarySection> allSections;

        if (client.fetchLibrarySections(allSections)) {
            // Filter for music sections only (type = "artist")
            std::vector<LibrarySection> musicSections;
            for (const auto& section : allSections) {
                if (section.type == "artist") {
                    musicSections.push_back(section);
                }
            }

            brls::Logger::info("MusicTab: Got {} music sections", musicSections.size());

            // Update UI on main thread
            brls::sync([this, musicSections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_sections = musicSections;
                m_sectionsBox->clearViews();

                for (const auto& section : m_sections) {
                    brls::Logger::debug("MusicTab: Adding section button: {}", section.title);
                    auto* btn = new brls::Button();
                    btn->setText(section.title);
                    btn->setMarginRight(10);

                    LibrarySection capturedSection = section;
                    btn->registerClickAction([this, capturedSection](brls::View* view) {
                        m_currentSection = capturedSection.key;
                        m_titleLabel->setText("Music - " + capturedSection.title);
                        loadContent(capturedSection.key);
                        loadCollections(capturedSection.key);
                        return true;
                    });

                    m_sectionsBox->addView(btn);
                }

                // Load first section by default
                if (!m_sections.empty()) {
                    brls::Logger::debug("MusicTab: Loading first section: {}", m_sections[0].title);
                    m_currentSection = m_sections[0].key;
                    m_titleLabel->setText("Music - " + m_sections[0].title);
                    loadContent(m_sections[0].key);
                    loadCollections(m_sections[0].key);
                }

                m_loaded = true;
                brls::Logger::debug("MusicTab: Sections loading complete");
            });
        } else {
            brls::Logger::error("MusicTab: Failed to fetch sections");
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loaded = true;
            });
        }
    });

    // Load playlists (audio playlists)
    const auto& settings = Application::getInstance().getSettings();
    if (settings.showPlaylists) {
        loadPlaylists();
    }
}

void MusicTab::loadContent(const std::string& sectionKey) {
    brls::Logger::debug("MusicTab::loadContent - section: {} (async)", sectionKey);

    std::string key = sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchLibraryContent(key, items)) {
            brls::Logger::info("MusicTab: Got {} items for section {}", items.size(), key);

            // Update UI on main thread
            brls::sync([this, items, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_items = items;
                m_contentGrid->setDataSource(m_items);
            });
        } else {
            brls::Logger::error("MusicTab: Failed to load content for section {}", key);
        }
    });
}

void MusicTab::loadPlaylists() {
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> allPlaylists;

        if (client.fetchPlaylists(allPlaylists)) {
            // Filter for audio playlists only
            std::vector<MediaItem> audioPlaylists;
            for (const auto& playlist : allPlaylists) {
                if (playlist.type == "playlist" &&
                    (playlist.subtype == "audio" || playlist.subtype.empty())) {
                    audioPlaylists.push_back(playlist);
                }
            }

            if (!audioPlaylists.empty()) {
                brls::Logger::info("MusicTab: Got {} audio playlists", audioPlaylists.size());

                brls::sync([this, audioPlaylists, aliveWeak]() {
                    auto alive = aliveWeak.lock();
                    if (!alive || !*alive) return;

                    m_playlists = audioPlaylists;
                    if (m_playlistsContainer && m_playlistsRow) {
                        m_playlistsContainer->clearViews();

                        for (const auto& playlist : m_playlists) {
                            auto* btn = new brls::Button();
                            btn->setText(playlist.title);
                            btn->setMarginRight(10);
                            btn->setHeight(40);

                            MediaItem capturedPlaylist = playlist;
                            btn->registerClickAction([this, capturedPlaylist](brls::View* view) {
                                onPlaylistSelected(capturedPlaylist);
                                return true;
                            });

                            m_playlistsContainer->addView(btn);
                        }

                        m_playlistsRow->setVisibility(brls::Visibility::VISIBLE);
                    }
                });
            } else {
                brls::Logger::debug("MusicTab: No audio playlists found");
            }
        }
    });
}

void MusicTab::loadCollections(const std::string& sectionKey) {
    std::string key = sectionKey;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, key, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> collections;

        if (client.fetchCollections(key, collections) && !collections.empty()) {
            brls::Logger::info("MusicTab: Got {} collections for section {}", collections.size(), key);

            brls::sync([this, collections, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

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
            brls::Logger::debug("MusicTab: No collections for section {}", key);
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                if (m_collectionsRow) {
                    m_collectionsRow->setVisibility(brls::Visibility::GONE);
                }
            });
        }
    });
}

void MusicTab::onItemSelected(const MediaItem& item) {
    // For tracks, play directly
    if (item.mediaType == MediaType::MUSIC_TRACK) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
        return;
    }

    // Show media detail view for artists and albums
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void MusicTab::onPlaylistSelected(const MediaItem& playlist) {
    brls::Logger::debug("MusicTab: Selected playlist: {}", playlist.title);

    std::string playlistKey = playlist.ratingKey;
    std::string playlistTitle = playlist.title;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, playlistKey, playlistTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchChildren(playlistKey, items)) {
            brls::Logger::info("MusicTab: Got {} items in playlist", items.size());

            brls::sync([this, items, playlistTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_titleLabel->setText("Playlist - " + playlistTitle);
                m_items = items;
                m_contentGrid->setDataSource(m_items);
            });
        } else {
            brls::Logger::error("MusicTab: Failed to load playlist content");
        }
    });
}

void MusicTab::onCollectionSelected(const MediaItem& collection) {
    brls::Logger::debug("MusicTab: Selected collection: {}", collection.title);

    std::string collectionKey = collection.ratingKey;
    std::string collectionTitle = collection.title;
    std::weak_ptr<bool> aliveWeak = m_alive;

    asyncRun([this, collectionKey, collectionTitle, aliveWeak]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;

        if (client.fetchChildren(collectionKey, items)) {
            brls::Logger::info("MusicTab: Got {} items in collection", items.size());

            brls::sync([this, items, collectionTitle, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_titleLabel->setText("Collection - " + collectionTitle);
                m_items = items;
                m_contentGrid->setDataSource(m_items);
            });
        } else {
            brls::Logger::error("MusicTab: Failed to load collection content");
        }
    });
}

} // namespace vitaplex
