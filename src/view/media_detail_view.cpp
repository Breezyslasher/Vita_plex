/**
 * VitaPlex - Media Detail View implementation
 */

#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "view/progress_dialog.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_queue.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include <thread>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace vitaplex {

MediaDetailView::MediaDetailView(const MediaItem& item)
    : m_item(item), m_alive(std::make_shared<std::atomic<bool>>(true)) {

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Register back button (B/Circle) to pop this activity
    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    }, false, false, brls::Sound::SOUND_BACK);

    // Create scrollable content
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_mainContent = new brls::Box();
    m_mainContent->setAxis(brls::Axis::COLUMN);
    m_mainContent->setPadding(30);

    // Check if this is music content
    bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                    m_item.mediaType == MediaType::MUSIC_ALBUM ||
                    m_item.mediaType == MediaType::MUSIC_TRACK);

    // Top row - poster and info
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setJustifyContent(brls::JustifyContent::FLEX_START);
    topRow->setAlignItems(brls::AlignItems::FLEX_START);
    topRow->setMarginBottom(20);

    // Left side - poster
    auto* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::COLUMN);
    leftBox->setWidth(200);
    leftBox->setMarginRight(30);

    // Wrap poster in a container for overlaying the star icon
    auto* posterContainer = new brls::Box();
    posterContainer->setAxis(brls::Axis::COLUMN);
    posterContainer->setWidth(200);

    m_posterImage = new brls::Image();
    if (isMusic) {
        // Square album art
        m_posterImage->setWidth(200);
        m_posterImage->setHeight(200);
        posterContainer->setHeight(200);
    } else {
        // Portrait poster
        m_posterImage->setWidth(200);
        m_posterImage->setHeight(300);
        posterContainer->setHeight(300);
    }
    m_posterImage->setScalingType(brls::ImageScalingType::FIT);
    posterContainer->addView(m_posterImage);

    // Star/rating icon overlay in top-right corner of album art
    if (m_item.rating > 0.0f) {
        auto* starIcon = new brls::Image();
        starIcon->setImageFromRes("icons/star.png");
        starIcon->setWidth(24);
        starIcon->setHeight(24);
        starIcon->setPositionType(brls::PositionType::ABSOLUTE);
        starIcon->setPositionTop(4);
        starIcon->setPositionRight(4);
        posterContainer->addView(starIcon);
    }

    leftBox->addView(posterContainer);

    // Play buttons (not for artists or albums - albums use track list actions)
    if (m_item.mediaType != MediaType::MUSIC_ARTIST &&
        m_item.mediaType != MediaType::MUSIC_ALBUM) {
        m_playButton = new brls::Button();
        m_playButton->setText("Play");
        m_playButton->setWidth(200);
        m_playButton->setHeight(44);
        m_playButton->setMarginTop(20);
        m_playButton->registerClickAction([this](brls::View* view) {
            onPlay(false);
            return true;
        });
        m_playButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_playButton));
        leftBox->addView(m_playButton);

        if (m_item.viewOffset > 0) {
            m_resumeButton = new brls::Button();
            m_resumeButton->setText("Resume");
            m_resumeButton->setWidth(200);
            m_resumeButton->setHeight(44);
            m_resumeButton->setMarginTop(10);
            m_resumeButton->registerClickAction([this](brls::View* view) {
                onPlay(true);
                return true;
            });
            m_resumeButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_resumeButton));
            leftBox->addView(m_resumeButton);
        }

        // Download button (for movies, episodes, and individual tracks only)
        if (m_item.mediaType == MediaType::MOVIE || m_item.mediaType == MediaType::EPISODE ||
            m_item.mediaType == MediaType::MUSIC_TRACK) {
            m_downloadButton = new brls::Button();

            // Check current download state
            {
                DownloadItem dlCheck;
                if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlCheck)) {
                    switch (dlCheck.state) {
                        case DownloadState::COMPLETED:
                            m_downloadButton->setText("Downloaded");
                            break;
                        case DownloadState::TRANSCODING:
                            m_downloadButton->setText("Transcoding...");
                            break;
                        case DownloadState::DOWNLOADING:
                            m_downloadButton->setText("Downloading...");
                            break;
                        case DownloadState::QUEUED:
                            m_downloadButton->setText("Queued");
                            break;
                        case DownloadState::PAUSED:
                            m_downloadButton->setText("Paused");
                            break;
                        case DownloadState::FAILED:
                            m_downloadButton->setText("Retry Download");
                            break;
                        default:
                            m_downloadButton->setText("Download");
                            break;
                    }
                } else {
                    m_downloadButton->setText("Download");
                }
            }

            m_downloadButton->setWidth(200);
            m_downloadButton->setHeight(44);
            m_downloadButton->setMarginTop(10);
            m_downloadButton->registerClickAction([this](brls::View* view) {
                onDownload();
                return true;
            });
            m_downloadButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_downloadButton));
            leftBox->addView(m_downloadButton);
        }
    }

    // Download options for shows and seasons (albums use context menu instead)
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON) {

        m_downloadButton = new brls::Button();
        m_downloadButton->setText("Download...");
        m_downloadButton->setWidth(200);
        m_downloadButton->setHeight(44);
        m_downloadButton->setMarginTop(10);
        m_downloadButton->registerClickAction([this](brls::View* view) {
            showDownloadOptions();
            return true;
        });
        m_downloadButton->addGestureRecognizer(new brls::TapGestureRecognizer(m_downloadButton));
        leftBox->addView(m_downloadButton);
    }

    topRow->addView(leftBox);

    // Right side - details
    auto* rightBox = new brls::Box();
    rightBox->setAxis(brls::Axis::COLUMN);
    rightBox->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(m_item.title);
    m_titleLabel->setFontSize(26);
    m_titleLabel->setMarginBottom(10);
    rightBox->addView(m_titleLabel);

    // Metadata row
    auto* metaBox = new brls::Box();
    metaBox->setAxis(brls::Axis::ROW);
    metaBox->setMarginBottom(15);

    if (m_item.year > 0) {
        m_yearLabel = new brls::Label();
        m_yearLabel->setText(std::to_string(m_item.year));
        m_yearLabel->setFontSize(16);
        m_yearLabel->setMarginRight(15);
        metaBox->addView(m_yearLabel);
    }

    if (!m_item.contentRating.empty()) {
        m_ratingLabel = new brls::Label();
        m_ratingLabel->setText(m_item.contentRating);
        m_ratingLabel->setFontSize(16);
        m_ratingLabel->setMarginRight(15);
        metaBox->addView(m_ratingLabel);
    }

    if (m_item.duration > 0) {
        m_durationLabel = new brls::Label();
        int minutes = m_item.duration / 60000;
        m_durationLabel->setText(std::to_string(minutes) + " min");
        m_durationLabel->setFontSize(16);
        metaBox->addView(m_durationLabel);
    }

    rightBox->addView(metaBox);

    // Summary (collapsible - shows 2 lines by default, L to expand)
    m_summaryLabel = new brls::Label();
    m_summaryLabel->setFontSize(16);
    m_summaryLabel->setMarginBottom(20);

    if (!m_item.summary.empty()) {
        m_fullDescription = m_item.summary;
        m_descriptionExpanded = false;

        // Show first ~80 chars (approximately 2 lines) when collapsed
        std::string truncatedDesc = m_fullDescription;
        if (truncatedDesc.length() > 80) {
            truncatedDesc = truncatedDesc.substr(0, 77) + "... [L]";
        }
        m_summaryLabel->setText(truncatedDesc);
    } else {
        m_fullDescription = "";
        m_summaryLabel->setText("");
    }
    rightBox->addView(m_summaryLabel);

    // Register L trigger for description toggle
    this->registerAction("Summary", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        toggleDescription();
        return true;
    });

    // Make summary tappable for touch (tap to expand/collapse)
    m_summaryLabel->setFocusable(true);
    m_summaryLabel->registerClickAction([this](brls::View* view) {
        toggleDescription();
        return true;
    });
    m_summaryLabel->addGestureRecognizer(new brls::TapGestureRecognizer(m_summaryLabel));

    topRow->addView(rightBox);
    m_mainContent->addView(topRow);

    // Children container (for shows/seasons - horizontal cards)
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON) {

        auto* childrenLabel = new brls::Label();
        if (m_item.mediaType == MediaType::SHOW) {
            childrenLabel->setText("Seasons");
        } else {
            childrenLabel->setText("Episodes");
        }
        childrenLabel->setFontSize(20);
        childrenLabel->setMarginBottom(10);
        m_mainContent->addView(childrenLabel);

        auto* childrenScroll = new brls::HScrollingFrame();
        // Episodes use landscape cells (~150x125), seasons use portrait (~120x200)
        childrenScroll->setHeight(m_item.mediaType == MediaType::SEASON ? 135 : 210);
        childrenScroll->setMarginBottom(20);

        m_childrenBox = new brls::Box();
        m_childrenBox->setAxis(brls::Axis::ROW);
        m_childrenBox->setJustifyContent(brls::JustifyContent::FLEX_START);

        childrenScroll->setContentView(m_childrenBox);
        m_mainContent->addView(childrenScroll);
    }

    // Track list for albums (vertical list with nested scrolling)
    if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
        auto* tracksLabel = new brls::Label();
        tracksLabel->setText("Tracks");
        tracksLabel->setFontSize(20);
        tracksLabel->setMarginBottom(10);
        m_mainContent->addView(tracksLabel);

        // Wrap track list in its own ScrollingFrame so only tracks scroll
        auto* trackScroll = new brls::ScrollingFrame();
        trackScroll->setGrow(1.0f);
        trackScroll->setMinHeight(200);

        m_trackListBox = new brls::Box();
        m_trackListBox->setAxis(brls::Axis::COLUMN);
        m_trackListBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        m_trackListBox->setAlignItems(brls::AlignItems::STRETCH);

        trackScroll->setContentView(m_trackListBox);
        m_mainContent->addView(trackScroll);
    }

    // Music categories container for artists - scrollable below fixed header
    if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        auto* categoriesScroll = new brls::ScrollingFrame();
        categoriesScroll->setGrow(1.0f);

        m_musicCategoriesBox = new brls::Box();
        m_musicCategoriesBox->setAxis(brls::Axis::COLUMN);

        categoriesScroll->setContentView(m_musicCategoriesBox);
        m_mainContent->addView(categoriesScroll);
    }

    if (m_item.mediaType == MediaType::MUSIC_ALBUM ||
        m_item.mediaType == MediaType::MUSIC_ARTIST) {
        // For albums and artists: top info is fixed, only content below scrolls
        m_mainContent->setGrow(1.0f);
        this->addView(m_mainContent);
    } else {
        m_scrollView->setContentView(m_mainContent);
        this->addView(m_scrollView);
    }

    // Load full details
    loadDetails();
}

MediaDetailView::~MediaDetailView() {
    if (m_alive) {
        m_alive->store(false);
    }
    brls::Logger::debug("MediaDetailView: Destroyed");
}

brls::HScrollingFrame* MediaDetailView::createMediaRow(const std::string& title, brls::Box** contentOut) {
    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(20);
    label->setMarginBottom(10);
    label->setMarginTop(15);
    m_musicCategoriesBox->addView(label);

    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(150);
    scrollFrame->setMarginBottom(10);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setJustifyContent(brls::JustifyContent::FLEX_START);

    scrollFrame->setContentView(content);
    m_musicCategoriesBox->addView(scrollFrame);

    if (contentOut) {
        *contentOut = content;
    }

    return scrollFrame;
}

brls::View* MediaDetailView::create() {
    return nullptr; // Factory not used
}

void MediaDetailView::loadDetails() {
    std::string ratingKey = m_item.ratingKey;
    std::string thumbPath = m_item.thumb;
    MediaType mediaType = m_item.mediaType;

    // Load full details asynchronously to avoid blocking the UI thread
    asyncRun([this, ratingKey, thumbPath, mediaType]() {
        PlexClient& client = PlexClient::getInstance();

        // Fetch full details on background thread
        MediaItem fullItem;
        bool detailsLoaded = client.fetchMediaDetails(ratingKey, fullItem);

        // Update UI on main thread
        brls::sync([this, detailsLoaded, fullItem, thumbPath, mediaType]() {
            if (detailsLoaded) {
                m_item = fullItem;

                // Update UI with full details
                if (m_titleLabel && !m_item.title.empty()) {
                    m_titleLabel->setText(m_item.title);
                }

                if (m_summaryLabel && !m_item.summary.empty()) {
                    m_summaryLabel->setText(m_item.summary);
                }

                // Update download button state now that we have the part path
                if (m_downloadButton && !m_item.partPath.empty()) {
                    DownloadItem dlCheck;
                    if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlCheck)) {
                        switch (dlCheck.state) {
                            case DownloadState::COMPLETED:
                                m_downloadButton->setText("Downloaded");
                                break;
                            case DownloadState::DOWNLOADING:
                                m_downloadButton->setText("Downloading...");
                                break;
                            case DownloadState::QUEUED:
                                m_downloadButton->setText("Queued");
                                break;
                            case DownloadState::PAUSED:
                                m_downloadButton->setText("Paused");
                                break;
                            case DownloadState::FAILED:
                                m_downloadButton->setText("Retry Download");
                                break;
                            default:
                                m_downloadButton->setText("Download");
                                break;
                        }
                    } else {
                        m_downloadButton->setText("Download");
                    }
                    brls::Logger::debug("loadDetails: partPath available, download enabled");
                }
            }

            // Load thumbnail with appropriate aspect ratio
            std::string thumb = detailsLoaded ? m_item.thumb : thumbPath;
            if (m_posterImage && !thumb.empty()) {
                bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                                m_item.mediaType == MediaType::MUSIC_ALBUM ||
                                m_item.mediaType == MediaType::MUSIC_TRACK);

                int width = 400;
                int height = isMusic ? 400 : 600;

                PlexClient& client = PlexClient::getInstance();
                std::string url = client.getThumbnailUrl(thumb, width, height);
                ImageLoader::loadAsync(url, [](brls::Image* image) {
                    // Image loaded
                }, m_posterImage, m_alive);
            }

            // Update description if full details loaded
            if (!m_item.summary.empty() && m_summaryLabel) {
                m_fullDescription = m_item.summary;
                m_descriptionExpanded = false;
                std::string truncatedDesc = m_fullDescription;
                if (truncatedDesc.length() > 80) {
                    truncatedDesc = truncatedDesc.substr(0, 77) + "... [L]";
                }
                m_summaryLabel->setText(truncatedDesc);
            }

            // Load children if applicable
            if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
                loadMusicCategories();
            } else if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
                loadTrackList();
            } else {
                loadChildren();
            }
        });
    });
}

void MediaDetailView::loadChildren() {
    if (!m_childrenBox) return;

    PlexClient& client = PlexClient::getInstance();

    if (client.fetchChildren(m_item.ratingKey, m_children)) {
        m_childrenBox->clearViews();

        for (const auto& child : m_children) {
            auto* cell = new MediaItemCell();
            cell->setItem(child);
            cell->setMarginRight(10);

            cell->registerClickAction([this, child](brls::View* view) {
                // Navigate to child detail
                auto* detailView = new MediaDetailView(child);
                brls::Application::pushActivity(new brls::Activity(detailView));
                return true;
            });
            cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

            // Register START button context menu for child items
            MediaItem capturedChild = child;
            if (child.mediaType == MediaType::SEASON) {
                cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                    [capturedChild](brls::View* view) {
                        showSeasonContextMenuStatic(capturedChild);
                        return true;
                    });
            }

            m_childrenBox->addView(cell);
        }
    }
}

void MediaDetailView::loadMusicCategories() {
    if (!m_musicCategoriesBox) return;

    asyncRun([this]() {
        PlexClient& client = PlexClient::getInstance();

        // Use the hubs API which returns albums pre-grouped by type
        // (Albums, Singles & EPs, Compilations, Appears On, etc.)
        std::vector<Hub> hubs;
        bool useHubs = client.fetchArtistHubs(m_item.ratingKey, hubs);

        if (useHubs && !hubs.empty()) {
            brls::Logger::info("Artist hubs: {} categories", hubs.size());

            // Collect all album items from hubs, then group by subtype
            std::vector<MediaItem> allAlbumItems;
            for (const auto& hub : hubs) {
                for (const auto& item : hub.items) {
                    if (item.mediaType == MediaType::MUSIC_ALBUM) {
                        allAlbumItems.push_back(item);
                    }
                }
            }

            brls::Logger::info("Artist hubs: {} total album items to group by subtype", allAlbumItems.size());

            // Group items by subtype
            std::vector<MediaItem> albums;
            std::vector<MediaItem> singles;
            std::vector<MediaItem> eps;
            std::vector<MediaItem> compilations;
            std::vector<MediaItem> soundtracks;
            std::vector<MediaItem> live;
            std::vector<MediaItem> other;

            for (const auto& item : allAlbumItems) {
                std::string subtype = item.subtype;
                for (char& c : subtype) c = tolower(c);

                if (subtype == "single") {
                    singles.push_back(item);
                } else if (subtype == "ep") {
                    eps.push_back(item);
                } else if (subtype == "compilation") {
                    compilations.push_back(item);
                } else if (subtype == "soundtrack") {
                    soundtracks.push_back(item);
                } else if (subtype == "live") {
                    live.push_back(item);
                } else if (subtype == "album" || subtype.empty()) {
                    albums.push_back(item);
                } else {
                    other.push_back(item);
                }
            }

            brls::Logger::info("Grouped: {} albums, {} singles, {} EPs, {} compilations, {} soundtracks, {} live, {} other",
                albums.size(), singles.size(), eps.size(), compilations.size(), soundtracks.size(), live.size(), other.size());

            brls::sync([this, albums, singles, eps, compilations, soundtracks, live, other]() {
                m_musicCategoriesBox->clearViews();

                auto addCategory = [this](const std::string& title, const std::vector<MediaItem>& items) {
                    if (items.empty()) return;

                    brls::Box* content = nullptr;
                    createMediaRow(title + " (" + std::to_string(items.size()) + ")", &content);

                    for (const auto& item : items) {
                        auto* cell = new MediaItemCell();
                        cell->setItem(item);
                        cell->setMarginRight(10);

                        MediaItem capturedItem = item;
                        cell->registerClickAction([this, capturedItem](brls::View* view) {
                            auto* detailView = new MediaDetailView(capturedItem);
                            brls::Application::pushActivity(new brls::Activity(detailView));
                            return true;
                        });
                        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                        cell->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedItem](brls::View* view) {
                            showAlbumContextMenu(capturedItem);
                            return true;
                        });

                        content->addView(cell);
                    }
                };

                addCategory("Albums", albums);
                addCategory("Singles", singles);
                addCategory("EPs", eps);
                addCategory("Compilations", compilations);
                addCategory("Soundtracks", soundtracks);
                addCategory("Live", live);
                addCategory("Other", other);
            });
            return;
        }

        // Fallback: use children endpoint and group by subtype
        brls::Logger::info("Artist hubs unavailable, falling back to children grouping");

        std::vector<MediaItem> allAlbums;
        if (!client.fetchChildren(m_item.ratingKey, allAlbums)) {
            brls::Logger::error("Failed to fetch albums for artist");
            return;
        }

        std::vector<MediaItem> albums;
        std::vector<MediaItem> singles;
        std::vector<MediaItem> eps;
        std::vector<MediaItem> compilations;
        std::vector<MediaItem> soundtracks;
        std::vector<MediaItem> other;

        for (const auto& album : allAlbums) {
            std::string subtype = album.subtype;
            for (char& c : subtype) c = tolower(c);

            if (subtype == "single") {
                singles.push_back(album);
            } else if (subtype == "ep") {
                eps.push_back(album);
            } else if (subtype == "compilation") {
                compilations.push_back(album);
            } else if (subtype == "soundtrack") {
                soundtracks.push_back(album);
            } else if (subtype == "album" || subtype.empty()) {
                albums.push_back(album);
            } else {
                other.push_back(album);
            }
        }

        brls::sync([this, albums, singles, eps, compilations, soundtracks, other]() {
            m_musicCategoriesBox->clearViews();

            auto addCategory = [this](const std::string& title, const std::vector<MediaItem>& items) {
                if (items.empty()) return;

                brls::Box* content = nullptr;
                createMediaRow(title + " (" + std::to_string(items.size()) + ")", &content);

                for (const auto& item : items) {
                    auto* cell = new MediaItemCell();
                    cell->setItem(item);
                    cell->setMarginRight(10);

                    MediaItem capturedItem = item;
                    cell->registerClickAction([this, capturedItem](brls::View* view) {
                        auto* detailView = new MediaDetailView(capturedItem);
                        brls::Application::pushActivity(new brls::Activity(detailView));
                        return true;
                    });
                    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                    cell->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedItem](brls::View* view) {
                        showAlbumContextMenu(capturedItem);
                        return true;
                    });

                    content->addView(cell);
                }
            };

            addCategory("Albums", albums);
            addCategory("Singles", singles);
            addCategory("EPs", eps);
            addCategory("Compilations", compilations);
            addCategory("Soundtracks", soundtracks);
            addCategory("Other", other);
        });
    });
}

void MediaDetailView::onPlay(bool resume) {
    // Start playback
    if (m_item.mediaType == MediaType::MOVIE ||
        m_item.mediaType == MediaType::EPISODE ||
        m_item.mediaType == MediaType::MUSIC_TRACK) {

        // Check if this item is downloaded locally - play from local file if available
        DownloadItem dlItem;
        bool isLocal = DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, dlItem)
                       && dlItem.state == DownloadState::COMPLETED;
        Application::getInstance().pushPlayerActivity(m_item.ratingKey, isLocal);
    }
    // For shows/seasons/albums, play the first child item
    else if (m_item.mediaType == MediaType::SHOW ||
             m_item.mediaType == MediaType::SEASON ||
             m_item.mediaType == MediaType::MUSIC_ALBUM) {

        // If we already have children loaded, play the first one
        if (!m_children.empty()) {
            // For shows, the first child is a season - need to get its first episode
            if (m_item.mediaType == MediaType::SHOW) {
                PlexClient& client = PlexClient::getInstance();
                std::vector<MediaItem> episodes;
                if (client.fetchChildren(m_children[0].ratingKey, episodes) && !episodes.empty()) {
                    Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
                }
            } else {
                // For seasons/albums, first child is directly playable
                Application::getInstance().pushPlayerActivity(m_children[0].ratingKey);
            }
        } else {
            // Fetch children and play first one
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> children;
            if (client.fetchChildren(m_item.ratingKey, children) && !children.empty()) {
                if (m_item.mediaType == MediaType::SHOW) {
                    // First child is a season, get its episodes
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(children[0].ratingKey, episodes) && !episodes.empty()) {
                        Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
                    }
                } else {
                    Application::getInstance().pushPlayerActivity(children[0].ratingKey);
                }
            }
        }
    }
}

void MediaDetailView::onDownload() {
    // Check if already in download queue
    DownloadItem existingDl;
    if (DownloadsManager::getInstance().getDownloadCopy(m_item.ratingKey, existingDl)) {
        switch (existingDl.state) {
            case DownloadState::COMPLETED:
                brls::Application::notify("Already downloaded");
                return;
            case DownloadState::DOWNLOADING:
                brls::Application::notify("Already downloading");
                return;
            case DownloadState::QUEUED:
                brls::Application::notify("Already queued for download");
                return;
            case DownloadState::PAUSED:
                // Resume paused downloads
                brls::Application::notify("Resuming download...");
                DownloadsManager::getInstance().resumeIncompleteDownloads();
                DownloadsManager::getInstance().startDownloads();
                if (m_downloadButton) m_downloadButton->setText("Downloading...");
                return;
            case DownloadState::FAILED:
                // Allow retry - fall through to re-queue
                brls::Logger::info("Retrying failed download: {}", m_item.title);
                DownloadsManager::getInstance().deleteDownload(m_item.ratingKey);
                break;
            default:
                break;
        }
    }

    // Check if we have the part path (need to fetch full details first)
    if (m_item.partPath.empty()) {
        brls::Application::notify("Loading media info...");

        // Try to load details now
        PlexClient& client = PlexClient::getInstance();
        MediaItem fullItem;
        if (client.fetchMediaDetails(m_item.ratingKey, fullItem) && !fullItem.partPath.empty()) {
            m_item = fullItem;
            brls::Logger::debug("onDownload: Loaded partPath={}", m_item.partPath);
        } else {
            brls::Logger::debug("onDownload: partPath is still empty, cannot download");
            brls::Application::notify("Unable to download - media info not available");
            return;
        }
    }

    // Determine media type and parent info
    std::string mediaType;
    if (m_item.mediaType == MediaType::MOVIE) {
        mediaType = "movie";
    } else if (m_item.mediaType == MediaType::MUSIC_TRACK) {
        mediaType = "track";
    } else {
        mediaType = "episode";
    }

    std::string parentTitle = "";
    int seasonNum = 0;
    int episodeNum = 0;

    if (m_item.mediaType == MediaType::EPISODE) {
        parentTitle = m_item.grandparentTitle;  // Show name
        seasonNum = m_item.parentIndex;  // Season number
        episodeNum = m_item.index;       // Episode number
    } else if (m_item.mediaType == MediaType::MUSIC_TRACK) {
        parentTitle = m_item.grandparentTitle;  // Artist name
    }

    // Queue the download (pass thumb for cover art on music tracks)
    bool queued = DownloadsManager::getInstance().queueDownload(
        m_item.ratingKey,
        m_item.title,
        m_item.partPath,
        m_item.duration,
        mediaType,
        parentTitle,
        seasonNum,
        episodeNum,
        m_item.thumb
    );

    if (queued) {
        if (m_downloadButton) {
            m_downloadButton->setText("Queued");
        }

        // Just notify the user and add to queue - no progress dialog
        brls::Application::notify("Added to download queue: " + m_item.title);

        // Start downloading in background
        DownloadsManager::getInstance().startDownloads();
    } else {
        brls::Application::notify("Failed to queue download");
    }
}

void MediaDetailView::showDownloadOptions() {
    // Create dialog with download options
    auto* dialog = new brls::Dialog("Download Options");

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    // Helper to create touch-friendly dialog buttons
    auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    // Different options based on media type
    if (m_item.mediaType == MediaType::SHOW) {
        addDialogButton("Download All Episodes", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
        addDialogButton("Download Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(); return true;
        });
        addDialogButton("Download Next 5 Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(5); return true;
        });
    } else if (m_item.mediaType == MediaType::SEASON) {
        addDialogButton("Download All Episodes", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
        addDialogButton("Download Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(); return true;
        });
        addDialogButton("Download Next 3 Unwatched", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadUnwatched(3); return true;
        });
    } else if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
        addDialogButton("Download Album", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
    } else if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        addDialogButton("Download All Albums", [this, dialog](brls::View*) {
            dialog->dismiss(); downloadAll(); return true;
        });
    }

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::downloadAll() {
    // Show progress dialog
    auto* progressDialog = new ProgressDialog("Preparing Downloads");
    progressDialog->setStatus("Fetching content list...");
    progressDialog->show();

    std::string ratingKey = m_item.ratingKey;
    MediaType mediaType = m_item.mediaType;
    std::string parentTitle = m_item.title;

    asyncRun([this, progressDialog, ratingKey, mediaType, parentTitle]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> items;
        int queued = 0;

        if (mediaType == MediaType::SHOW) {
            // Get all seasons first, then all episodes
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (auto& ep : episodes) {
                            items.push_back(ep);
                        }
                    }
                }
            }
        } else if (mediaType == MediaType::SEASON || mediaType == MediaType::MUSIC_ALBUM) {
            // Get direct children (episodes or tracks)
            client.fetchChildren(ratingKey, items);
        } else if (mediaType == MediaType::MUSIC_ARTIST) {
            // Get all albums, then all tracks
            std::vector<MediaItem> albums;
            if (client.fetchChildren(ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (auto& track : tracks) {
                            items.push_back(track);
                        }
                    }
                }
            }
        }

        size_t itemCount = items.size();
        brls::sync([progressDialog, itemCount]() {
            progressDialog->setStatus("Found " + std::to_string(itemCount) + " items");
            progressDialog->setProgress(0.1f);
        });

        // If no items found, dismiss dialog
        if (items.empty()) {
            brls::sync([progressDialog]() {
                progressDialog->setStatus("No items found to download");
                brls::delay(1500, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Queue each item for download
        for (size_t i = 0; i < items.size(); i++) {
            const auto& item = items[i];

            // Get full details to get the part path
            MediaItem fullItem;
            if (client.fetchMediaDetails(item.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                std::string itemMediaType = "episode";
                if (fullItem.mediaType == MediaType::MOVIE) itemMediaType = "movie";
                else if (fullItem.mediaType == MediaType::MUSIC_TRACK) itemMediaType = "track";

                if (DownloadsManager::getInstance().queueDownload(
                    fullItem.ratingKey,
                    fullItem.title,
                    fullItem.partPath,
                    fullItem.duration,
                    itemMediaType,
                    parentTitle,
                    fullItem.parentIndex,
                    fullItem.index
                )) {
                    queued++;
                }
            }

            size_t currentIndex = i;
            brls::sync([progressDialog, currentIndex, itemCount, queued]() {
                progressDialog->setStatus("Queued " + std::to_string(queued) + " of " +
                                         std::to_string(itemCount));
                progressDialog->setProgress(0.1f + 0.9f * static_cast<float>(currentIndex + 1) / itemCount);
            });
        }

        // Start downloads
        DownloadsManager::getInstance().startDownloads();

        brls::sync([progressDialog, queued]() {
            progressDialog->setStatus("Queued " + std::to_string(queued) + " downloads");
            brls::delay(1500, [progressDialog]() {
                progressDialog->dismiss();
            });
        });
    });
}

void MediaDetailView::downloadUnwatched(int maxCount) {
    // Show progress dialog
    auto* progressDialog = new ProgressDialog("Preparing Downloads");
    progressDialog->setStatus("Fetching unwatched content...");
    progressDialog->show();

    std::string ratingKey = m_item.ratingKey;
    MediaType mediaType = m_item.mediaType;
    std::string parentTitle = m_item.title;

    asyncRun([this, progressDialog, ratingKey, mediaType, parentTitle, maxCount]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> unwatchedItems;
        int queued = 0;

        if (mediaType == MediaType::SHOW) {
            // Get all seasons first, then unwatched episodes
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (auto& ep : episodes) {
                            if (!ep.watched && ep.viewOffset == 0) {
                                unwatchedItems.push_back(ep);
                                if (maxCount > 0 && (int)unwatchedItems.size() >= maxCount) {
                                    break;
                                }
                            }
                        }
                    }
                    if (maxCount > 0 && (int)unwatchedItems.size() >= maxCount) {
                        break;
                    }
                }
            }
        } else if (mediaType == MediaType::SEASON) {
            // Get unwatched episodes in this season
            std::vector<MediaItem> episodes;
            if (client.fetchChildren(ratingKey, episodes)) {
                for (auto& ep : episodes) {
                    if (!ep.watched && ep.viewOffset == 0) {
                        unwatchedItems.push_back(ep);
                        if (maxCount > 0 && (int)unwatchedItems.size() >= maxCount) {
                            break;
                        }
                    }
                }
            }
        }

        size_t itemCount = unwatchedItems.size();
        brls::sync([progressDialog, itemCount]() {
            progressDialog->setStatus("Found " + std::to_string(itemCount) + " unwatched");
            progressDialog->setProgress(0.1f);
        });

        // If no items found, dismiss dialog
        if (unwatchedItems.empty()) {
            brls::sync([progressDialog]() {
                progressDialog->setStatus("No unwatched items found");
                brls::delay(1500, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Queue each unwatched item for download
        for (size_t i = 0; i < unwatchedItems.size(); i++) {
            const auto& item = unwatchedItems[i];

            // Get full details to get the part path
            MediaItem fullItem;
            if (client.fetchMediaDetails(item.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                std::string itemMediaType = "episode";

                if (DownloadsManager::getInstance().queueDownload(
                    fullItem.ratingKey,
                    fullItem.title,
                    fullItem.partPath,
                    fullItem.duration,
                    itemMediaType,
                    parentTitle,
                    fullItem.parentIndex,
                    fullItem.index
                )) {
                    queued++;
                }
            }

            size_t currentIndex = i;
            brls::sync([progressDialog, currentIndex, itemCount, queued]() {
                progressDialog->setStatus("Queued " + std::to_string(queued) + " of " +
                                         std::to_string(itemCount));
                progressDialog->setProgress(0.1f + 0.9f * static_cast<float>(currentIndex + 1) / itemCount);
            });
        }

        // Start downloads
        DownloadsManager::getInstance().startDownloads();

        brls::sync([progressDialog, queued]() {
            progressDialog->setStatus("Queued " + std::to_string(queued) + " downloads");
            brls::delay(1500, [progressDialog]() {
                progressDialog->dismiss();
            });
        });
    });
}

void MediaDetailView::loadTrackList() {
    if (!m_trackListBox) return;

    asyncRun([this]() {
        PlexClient& client = PlexClient::getInstance();
        std::vector<MediaItem> tracks;

        if (!client.fetchChildren(m_item.ratingKey, tracks)) {
            brls::Logger::error("Failed to fetch tracks for album");
            return;
        }

        std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;

        brls::sync([this, tracks, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            m_trackListBox->clearViews();
            m_children = tracks;

            for (size_t i = 0; i < tracks.size(); i++) {
                const auto& track = tracks[i];

                // Create a row for each track (like Suwayomi ChapterCell)
                auto* row = new brls::Box();
                row->setAxis(brls::Axis::ROW);
                row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
                row->setAlignItems(brls::AlignItems::CENTER);
                row->setHeight(56);
                row->setPadding(10, 16, 10, 16);
                row->setMarginBottom(4);
                row->setCornerRadius(8);
                row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
                row->setFocusable(true);

                // Left side: track number + title
                auto* leftBox = new brls::Box();
                leftBox->setAxis(brls::Axis::ROW);
                leftBox->setAlignItems(brls::AlignItems::CENTER);
                leftBox->setGrow(1.0f);

                auto* trackNum = new brls::Label();
                trackNum->setFontSize(14);
                trackNum->setMarginRight(12);
                trackNum->setTextColor(nvgRGBA(150, 150, 150, 255));
                if (track.index > 0) {
                    trackNum->setText(std::to_string(track.index));
                } else {
                    trackNum->setText(std::to_string(i + 1));
                }
                leftBox->addView(trackNum);

                auto* titleLabel = new brls::Label();
                titleLabel->setFontSize(14);
                titleLabel->setText(track.title);
                leftBox->addView(titleLabel);

                row->addView(leftBox);

                // Right side: button hint + duration
                auto* rightSide = new brls::Box();
                rightSide->setAxis(brls::Axis::ROW);
                rightSide->setAlignItems(brls::AlignItems::CENTER);

                auto* hintIcon = new brls::Image();
                hintIcon->setImageFromRes("images/square_button.png");
                hintIcon->setWidth(16);
                hintIcon->setHeight(16);
                hintIcon->setMarginRight(2);
                hintIcon->setVisibility(brls::Visibility::INVISIBLE);
                rightSide->addView(hintIcon);

                auto* hintLabel = new brls::Label();
                hintLabel->setFontSize(10);
                hintLabel->setTextColor(nvgRGBA(150, 150, 180, 180));
                hintLabel->setText("DL");
                hintLabel->setMarginRight(10);
                hintLabel->setVisibility(brls::Visibility::INVISIBLE);
                rightSide->addView(hintLabel);

                // Show hint on focus, hide previous (like Suwayomi chapter icon pattern)
                brls::Image* capturedHintIcon = hintIcon;
                brls::Label* capturedHintLabel = hintLabel;
                row->getFocusEvent()->subscribe([this, capturedHintIcon, capturedHintLabel](brls::View*) {
                    // Hide previously focused hint
                    if (m_currentFocusedHint && m_currentFocusedHint != capturedHintIcon) {
                        m_currentFocusedHint->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    if (m_currentFocusedHintLabel && m_currentFocusedHintLabel != capturedHintLabel) {
                        m_currentFocusedHintLabel->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    // Show current hint
                    capturedHintIcon->setVisibility(brls::Visibility::VISIBLE);
                    capturedHintLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_currentFocusedHint = capturedHintIcon;
                    m_currentFocusedHintLabel = capturedHintLabel;
                });

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
                    rightSide->addView(durLabel);
                }

                row->addView(rightSide);

                // Click to perform default track action
                MediaItem capturedTrack = track;
                row->registerClickAction([this, capturedTrack, i](brls::View* view) {
                    performTrackAction(capturedTrack, i);
                    return true;
                });
                row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

                // Square button (X on PS Vita) adds to download queue
                row->registerAction("Download", brls::ControllerButton::BUTTON_X, [this, capturedTrack](brls::View* view) {
                    // Create a temporary copy to download
                    MediaItem dlItem = capturedTrack;
                    if (DownloadsManager::getInstance().isDownloaded(dlItem.ratingKey)) {
                        brls::Application::notify("Already downloaded");
                    } else {
                        // Fetch full details for partPath
                        asyncRun([this, dlItem]() {
                            PlexClient& client = PlexClient::getInstance();
                            MediaItem fullItem;
                            if (client.fetchMediaDetails(dlItem.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                bool queued = DownloadsManager::getInstance().queueDownload(
                                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                    fullItem.duration, "track",
                                    fullItem.grandparentTitle, 0, fullItem.index,
                                    fullItem.thumb);
                                brls::sync([queued, fullItem]() {
                                    if (queued) {
                                        DownloadsManager::getInstance().startDownloads();
                                        brls::Application::notify("Downloading: " + fullItem.title);
                                    } else {
                                        brls::Application::notify("Failed to queue download");
                                    }
                                });
                            } else {
                                brls::sync([]() {
                                    brls::Application::notify("Could not get download info");
                                });
                            }
                        });
                    }
                    return true;
                });

                m_trackListBox->addView(row);
            }
        });
    });
}

void MediaDetailView::performTrackAction(const MediaItem& track, size_t trackIndex) {
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        showTrackActionDialog(track, trackIndex);
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();

    switch (action) {
        case TrackDefaultAction::PLAY_NEXT:
            // Add after current track in queue
            if (queue.isEmpty()) {
                // No queue yet - start with just this track
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                brls::Application::notify("Playing next: " + track.title);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_REPLACE:
            // Replace current track and play
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                if (queue.playNext()) {
                    brls::Application::notify("Now playing: " + track.title);
                }
            }
            break;

        case TrackDefaultAction::ADD_TO_BOTTOM:
            // Add to end of queue
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(track);
                brls::Application::notify("Added to queue: " + track.title);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_CLEAR:
        default:
            // Clear queue and play just this track
            {
                std::vector<MediaItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            }
            break;
    }
}

void MediaDetailView::showTrackActionDialog(const MediaItem& track, size_t trackIndex) {
    auto* dialog = new brls::Dialog("Choose Action");

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

    MediaItem capturedTrack = track;
    size_t capturedIndex = trackIndex;

    addDialogButton("Play Now (Clear Queue)", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        // Play only this single track
        std::vector<MediaItem> single = {capturedTrack};
        auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
        brls::Application::pushActivity(playerActivity);
        return true;
    });

    addDialogButton("Play Next", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            // Start new queue with just this track
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.insertTrackAfterCurrent(capturedTrack);
            brls::Application::notify("Playing next: " + capturedTrack.title);
        }
        return true;
    });

    addDialogButton("Add to Bottom of Queue", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            std::vector<MediaItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.addTrack(capturedTrack);
            brls::Application::notify("Added to queue: " + capturedTrack.title);
        }
        return true;
    });

    addDialogButton("Add to Playlist", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([this, capturedTrack]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<Playlist> playlists;
            client.fetchMusicPlaylists(playlists);

            brls::sync([this, playlists, capturedTrack]() {
                auto alive = m_alive;
                if (!alive || !alive->load()) return;

                auto* plDialog = new brls::Dialog("Add to Playlist");
                auto* plBox = new brls::Box();
                plBox->setAxis(brls::Axis::COLUMN);
                plBox->setPadding(20);

                auto addBtn = [&plBox](const std::string& text, std::function<bool(brls::View*)> action) {
                    auto* btn = new brls::Button();
                    btn->setText(text);
                    btn->setHeight(44);
                    btn->setMarginBottom(10);
                    btn->registerClickAction(action);
                    btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
                    plBox->addView(btn);
                };

                addBtn("+ New Playlist", [plDialog, capturedTrack](brls::View*) {
                    plDialog->dismiss();
                    brls::Application::getImeManager()->openForText([capturedTrack](std::string name) {
                        if (name.empty()) return;
                        asyncRun([name, capturedTrack]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys = {capturedTrack.ratingKey};
                            Playlist result;
                            if (client.createPlaylistWithItems(name, keys, result)) {
                                brls::sync([name]() {
                                    brls::Application::notify("Created playlist: " + name);
                                });
                            }
                        });
                    }, "New Playlist", "Enter playlist name", 128, "");
                    return true;
                });

                for (const auto& pl : playlists) {
                    if (pl.smart) continue;
                    Playlist capturedPl = pl;
                    addBtn(pl.title, [plDialog, capturedPl, capturedTrack](brls::View*) {
                        plDialog->dismiss();
                        asyncRun([capturedPl, capturedTrack]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys = {capturedTrack.ratingKey};
                            if (client.addToPlaylist(capturedPl.ratingKey, keys)) {
                                brls::sync([capturedPl]() {
                                    brls::Application::notify("Added to " + capturedPl.title);
                                });
                            }
                        });
                        return true;
                    });
                }

                addBtn("Cancel", [plDialog](brls::View*) {
                    plDialog->dismiss();
                    return true;
                });

                plDialog->addView(plBox);
                brls::Application::pushActivity(new brls::Activity(plDialog));
            });
        });
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::showAlbumContextMenu(const MediaItem& album) {
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

    addDialogButton("Play Now (Clear Queue)", [this, capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        // Fetch album tracks and play
        asyncRun([this, capturedAlbum]() {
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

    addDialogButton("Play Next", [this, capturedAlbum, dialog](brls::View*) {
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
                        // Insert all tracks after current
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

    addDialogButton("Add to Bottom of Queue", [this, capturedAlbum, dialog](brls::View*) {
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

    addDialogButton("Add to Playlist", [this, capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        // Fetch audio playlists and let user pick one
        asyncRun([this, capturedAlbum]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<Playlist> playlists;
            client.fetchMusicPlaylists(playlists);

            // Also fetch album tracks to get their ratingKeys
            std::vector<MediaItem> tracks;
            client.fetchChildren(capturedAlbum.ratingKey, tracks);

            brls::sync([this, playlists, tracks, capturedAlbum]() {
                auto alive = m_alive;
                if (!alive || !alive->load()) return;

                if (tracks.empty()) {
                    brls::Application::notify("No tracks found");
                    return;
                }

                auto* plDialog = new brls::Dialog("Add to Playlist");
                auto* plBox = new brls::Box();
                plBox->setAxis(brls::Axis::COLUMN);
                plBox->setPadding(20);

                auto addBtn = [&plBox](const std::string& text, std::function<bool(brls::View*)> action) {
                    auto* btn = new brls::Button();
                    btn->setText(text);
                    btn->setHeight(44);
                    btn->setMarginBottom(10);
                    btn->registerClickAction(action);
                    btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
                    plBox->addView(btn);
                };

                // Option to create new playlist with this album
                addBtn("+ New Playlist", [plDialog, tracks](brls::View*) {
                    plDialog->dismiss();
                    brls::Application::getImeManager()->openForText([tracks](std::string name) {
                        if (name.empty()) return;
                        asyncRun([name, tracks]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys;
                            for (const auto& t : tracks) {
                                keys.push_back(t.ratingKey);
                            }
                            Playlist result;
                            if (client.createPlaylistWithItems(name, keys, result)) {
                                brls::sync([name]() {
                                    brls::Application::notify("Created playlist: " + name);
                                });
                            } else {
                                brls::sync([]() {
                                    brls::Application::notify("Failed to create playlist");
                                });
                            }
                        });
                    }, "New Playlist", "Enter playlist name", 128, "");
                    return true;
                });

                // Existing playlists
                for (const auto& pl : playlists) {
                    if (pl.smart) continue;  // Can't add to smart playlists
                    Playlist capturedPl = pl;
                    addBtn(pl.title, [plDialog, capturedPl, tracks](brls::View*) {
                        plDialog->dismiss();
                        asyncRun([capturedPl, tracks]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<std::string> keys;
                            for (const auto& t : tracks) {
                                keys.push_back(t.ratingKey);
                            }
                            if (client.addToPlaylist(capturedPl.ratingKey, keys)) {
                                brls::sync([capturedPl]() {
                                    brls::Application::notify("Added to " + capturedPl.title);
                                });
                            } else {
                                brls::sync([]() {
                                    brls::Application::notify("Failed to add to playlist");
                                });
                            }
                        });
                        return true;
                    });
                }

                addBtn("Cancel", [plDialog](brls::View*) {
                    plDialog->dismiss();
                    return true;
                });

                plDialog->addView(plBox);
                brls::Application::pushActivity(new brls::Activity(plDialog));
            });
        });
        return true;
    });

    addDialogButton("Download Album", [this, capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        // Re-use existing download logic
        m_item = capturedAlbum;
        downloadAll();
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::showMovieContextMenu(const MediaItem& movie) {
    showMovieContextMenuStatic(movie);
}

void MediaDetailView::showShowContextMenu(const MediaItem& show) {
    showShowContextMenuStatic(show);
}

void MediaDetailView::showMovieContextMenuStatic(const MediaItem& movie) {
    auto* dialog = new brls::Dialog(movie.title);

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

    MediaItem capturedMovie = movie;

    // Restart button
    addDialogButton("Restart", [capturedMovie, dialog](brls::View*) {
        dialog->dismiss();
        // Mark as unwatched first to reset progress, then play
        PlexClient::getInstance().markAsUnwatched(capturedMovie.ratingKey);
        Application::getInstance().pushPlayerActivity(capturedMovie.ratingKey);
        return true;
    });

    // Resume button (only if there's a view offset)
    if (movie.viewOffset > 0) {
        int totalSec = movie.viewOffset / 1000;
        int hours = totalSec / 3600;
        int minutes = (totalSec % 3600) / 60;
        char resumeStr[64];
        if (hours > 0) {
            snprintf(resumeStr, sizeof(resumeStr), "Resume from %dh %dm", hours, minutes);
        } else {
            snprintf(resumeStr, sizeof(resumeStr), "Resume from %dm", minutes);
        }
        addDialogButton(resumeStr, [capturedMovie, dialog](brls::View*) {
            dialog->dismiss();
            Application::getInstance().pushPlayerActivity(capturedMovie.ratingKey);
            return true;
        });
    }

    // Download
    addDialogButton("Download", [capturedMovie, dialog](brls::View*) {
        dialog->dismiss();
        if (DownloadsManager::getInstance().isDownloaded(capturedMovie.ratingKey)) {
            brls::Application::notify("Already downloaded");
            return true;
        }
        // Fetch full details and queue download
        asyncRun([capturedMovie]() {
            PlexClient& client = PlexClient::getInstance();
            MediaItem fullItem;
            if (client.fetchMediaDetails(capturedMovie.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                bool queued = DownloadsManager::getInstance().queueDownload(
                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                    fullItem.duration, "movie", "", 0, 0);
                brls::sync([queued, fullItem]() {
                    if (queued) {
                        DownloadsManager::getInstance().startDownloads();
                        brls::Application::notify("Downloading: " + fullItem.title);
                    } else {
                        brls::Application::notify("Failed to queue download");
                    }
                });
            } else {
                brls::sync([]() {
                    brls::Application::notify("Could not get download info");
                });
            }
        });
        return true;
    });

    // Mark as watched/unwatched
    if (movie.watched) {
        addDialogButton("Mark as Unwatched", [capturedMovie, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedMovie]() {
                PlexClient::getInstance().markAsUnwatched(capturedMovie.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        });
    } else {
        addDialogButton("Mark as Watched", [capturedMovie, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedMovie]() {
                PlexClient::getInstance().markAsWatched(capturedMovie.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        });
    }

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::showShowContextMenuStatic(const MediaItem& show) {
    auto* dialog = new brls::Dialog(show.title);

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

    MediaItem capturedShow = show;

    // Restart (play first episode of first season)
    addDialogButton("Restart (S01E01)", [capturedShow, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> seasons;
            if (client.fetchChildren(capturedShow.ratingKey, seasons) && !seasons.empty()) {
                std::vector<MediaItem> episodes;
                if (client.fetchChildren(seasons[0].ratingKey, episodes) && !episodes.empty()) {
                    brls::sync([episodes]() {
                        Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
                    });
                }
            }
        });
        return true;
    });

    // Resume (find the next unwatched/in-progress episode)
    if (show.viewOffset > 0 || show.viewedLeafCount > 0) {
        addDialogButton("Resume", [capturedShow, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedShow]() {
                PlexClient& client = PlexClient::getInstance();
                std::vector<MediaItem> seasons;
                if (!client.fetchChildren(capturedShow.ratingKey, seasons) || seasons.empty()) return;

                // Find the first in-progress or unwatched episode
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (!client.fetchChildren(season.ratingKey, episodes)) continue;
                    for (const auto& ep : episodes) {
                        // Episode with viewOffset = in-progress, play it
                        if (ep.viewOffset > 0) {
                            int totalSec = ep.viewOffset / 1000;
                            int minLeft = ((ep.duration - ep.viewOffset) / 1000) / 60;
                            char info[64];
                            snprintf(info, sizeof(info), "S%02dE%02d - %dm left",
                                     ep.parentIndex, ep.index, minLeft);
                            std::string infoStr = info;
                            brls::sync([ep, infoStr]() {
                                brls::Application::notify("Resuming " + infoStr);
                                Application::getInstance().pushPlayerActivity(ep.ratingKey);
                            });
                            return;
                        }
                        // First unwatched episode
                        if (!ep.watched) {
                            char info[64];
                            snprintf(info, sizeof(info), "S%02dE%02d",
                                     ep.parentIndex, ep.index);
                            std::string infoStr = info;
                            brls::sync([ep, infoStr]() {
                                brls::Application::notify("Playing " + infoStr);
                                Application::getInstance().pushPlayerActivity(ep.ratingKey);
                            });
                            return;
                        }
                    }
                }
                // All watched - play first episode
                brls::sync([]() {
                    brls::Application::notify("All episodes watched, restarting");
                });
                std::vector<MediaItem> firstEps;
                if (client.fetchChildren(seasons[0].ratingKey, firstEps) && !firstEps.empty()) {
                    brls::sync([firstEps]() {
                        Application::getInstance().pushPlayerActivity(firstEps[0].ratingKey);
                    });
                }
            });
            return true;
        });
    }

    // Download options
    addDialogButton("Download Entire Show", [capturedShow, dialog](brls::View*) {
        dialog->dismiss();
        // Use the downloadAll pattern from existing code
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> seasons;
            int queued = 0;
            if (client.fetchChildren(capturedShow.ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (const auto& ep : episodes) {
                            MediaItem fullItem;
                            if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                if (DownloadsManager::getInstance().queueDownload(
                                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                    fullItem.duration, "episode", capturedShow.title,
                                    fullItem.parentIndex, fullItem.index)) {
                                    queued++;
                                }
                            }
                        }
                    }
                }
            }
            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued]() {
                brls::Application::notify("Queued " + std::to_string(queued) + " episodes");
            });
        });
        return true;
    });

    addDialogButton("Download All Unwatched", [capturedShow, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> seasons;
            int queued = 0;
            if (client.fetchChildren(capturedShow.ratingKey, seasons)) {
                for (const auto& season : seasons) {
                    std::vector<MediaItem> episodes;
                    if (client.fetchChildren(season.ratingKey, episodes)) {
                        for (const auto& ep : episodes) {
                            if (!ep.watched && ep.viewOffset == 0) {
                                MediaItem fullItem;
                                if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                    if (DownloadsManager::getInstance().queueDownload(
                                        fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                        fullItem.duration, "episode", capturedShow.title,
                                        fullItem.parentIndex, fullItem.index)) {
                                        queued++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued]() {
                brls::Application::notify("Queued " + std::to_string(queued) + " unwatched episodes");
            });
        });
        return true;
    });

    // Per-season download submenu
    addDialogButton("Download by Season...", [capturedShow, dialog](brls::View*) {
        dialog->dismiss();
        // Show a second dialog with season options
        asyncRun([capturedShow]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> seasons;
            if (!client.fetchChildren(capturedShow.ratingKey, seasons) || seasons.empty()) {
                brls::sync([]() {
                    brls::Application::notify("No seasons found");
                });
                return;
            }

            brls::sync([capturedShow, seasons]() {
                auto* seasonDialog = new brls::Dialog("Download Season");
                auto* box = new brls::Box();
                box->setAxis(brls::Axis::COLUMN);
                box->setPadding(20);

                for (const auto& season : seasons) {
                    MediaItem capturedSeason = season;
                    std::string showTitle = capturedShow.title;
                    auto* btn = new brls::Button();
                    btn->setText(season.title);
                    btn->setHeight(44);
                    btn->setMarginBottom(10);
                    btn->registerClickAction([capturedSeason, showTitle, seasonDialog](brls::View*) {
                        seasonDialog->dismiss();
                        asyncRun([capturedSeason, showTitle]() {
                            PlexClient& client = PlexClient::getInstance();
                            std::vector<MediaItem> episodes;
                            int queued = 0;
                            if (client.fetchChildren(capturedSeason.ratingKey, episodes)) {
                                for (const auto& ep : episodes) {
                                    MediaItem fullItem;
                                    if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                        if (DownloadsManager::getInstance().queueDownload(
                                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                            fullItem.duration, "episode", showTitle,
                                            fullItem.parentIndex, fullItem.index)) {
                                            queued++;
                                        }
                                    }
                                }
                            }
                            DownloadsManager::getInstance().startDownloads();
                            brls::sync([queued, capturedSeason]() {
                                brls::Application::notify("Queued " + std::to_string(queued) +
                                    " episodes from " + capturedSeason.title);
                            });
                        });
                        return true;
                    });
                    btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
                    box->addView(btn);
                }

                auto* cancelBtn = new brls::Button();
                cancelBtn->setText("Cancel");
                cancelBtn->setHeight(44);
                cancelBtn->setMarginBottom(10);
                cancelBtn->registerClickAction([seasonDialog](brls::View*) {
                    seasonDialog->dismiss(); return true;
                });
                cancelBtn->addGestureRecognizer(new brls::TapGestureRecognizer(cancelBtn));
                box->addView(cancelBtn);

                seasonDialog->addView(box);
                brls::Application::pushActivity(new brls::Activity(seasonDialog));
            });
        });
        return true;
    });

    // Mark as watched/unwatched
    if (show.watched || (show.leafCount > 0 && show.viewedLeafCount == show.leafCount)) {
        addDialogButton("Mark as Unwatched", [capturedShow, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedShow]() {
                PlexClient::getInstance().markAsUnwatched(capturedShow.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        });
    } else {
        addDialogButton("Mark as Watched", [capturedShow, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedShow]() {
                PlexClient::getInstance().markAsWatched(capturedShow.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        });
    }

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::showSeasonContextMenuStatic(const MediaItem& season) {
    auto* dialog = new brls::Dialog(season.title);

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

    MediaItem capturedSeason = season;

    // Resume (find next in-progress or unwatched episode)
    addDialogButton("Resume", [capturedSeason, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedSeason]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> episodes;
            if (!client.fetchChildren(capturedSeason.ratingKey, episodes) || episodes.empty()) {
                brls::sync([]() { brls::Application::notify("No episodes found"); });
                return;
            }
            // Find in-progress episode first
            for (const auto& ep : episodes) {
                if (ep.viewOffset > 0) {
                    int minLeft = ((ep.duration - ep.viewOffset) / 1000) / 60;
                    char info[64];
                    snprintf(info, sizeof(info), "S%02dE%02d - %dm left",
                             ep.parentIndex, ep.index, minLeft);
                    std::string infoStr = info;
                    brls::sync([ep, infoStr]() {
                        brls::Application::notify("Resuming " + infoStr);
                        Application::getInstance().pushPlayerActivity(ep.ratingKey);
                    });
                    return;
                }
            }
            // Find first unwatched
            for (const auto& ep : episodes) {
                if (!ep.watched) {
                    char info[64];
                    snprintf(info, sizeof(info), "S%02dE%02d", ep.parentIndex, ep.index);
                    std::string infoStr = info;
                    brls::sync([ep, infoStr]() {
                        brls::Application::notify("Playing " + infoStr);
                        Application::getInstance().pushPlayerActivity(ep.ratingKey);
                    });
                    return;
                }
            }
            // All watched - play first
            brls::sync([episodes]() {
                brls::Application::notify("All watched, restarting season");
                Application::getInstance().pushPlayerActivity(episodes[0].ratingKey);
            });
        });
        return true;
    });

    // Download whole season
    addDialogButton("Download Whole Season", [capturedSeason, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedSeason]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> episodes;
            int queued = 0;
            if (client.fetchChildren(capturedSeason.ratingKey, episodes)) {
                for (const auto& ep : episodes) {
                    MediaItem fullItem;
                    if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                        if (DownloadsManager::getInstance().queueDownload(
                            fullItem.ratingKey, fullItem.title, fullItem.partPath,
                            fullItem.duration, "episode", capturedSeason.parentTitle.empty() ? capturedSeason.title : capturedSeason.parentTitle,
                            fullItem.parentIndex, fullItem.index)) {
                            queued++;
                        }
                    }
                }
            }
            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued]() {
                brls::Application::notify("Queued " + std::to_string(queued) + " episodes");
            });
        });
        return true;
    });

    // Download all unwatched
    addDialogButton("Download All Unwatched", [capturedSeason, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedSeason]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> episodes;
            int queued = 0;
            if (client.fetchChildren(capturedSeason.ratingKey, episodes)) {
                for (const auto& ep : episodes) {
                    if (!ep.watched && ep.viewOffset == 0) {
                        MediaItem fullItem;
                        if (client.fetchMediaDetails(ep.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                            if (DownloadsManager::getInstance().queueDownload(
                                fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                fullItem.duration, "episode", capturedSeason.parentTitle.empty() ? capturedSeason.title : capturedSeason.parentTitle,
                                fullItem.parentIndex, fullItem.index)) {
                                queued++;
                            }
                        }
                    }
                }
            }
            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued]() {
                brls::Application::notify("Queued " + std::to_string(queued) + " unwatched episodes");
            });
        });
        return true;
    });

    // Mark as watched/unwatched
    if (season.watched || (season.leafCount > 0 && season.viewedLeafCount == season.leafCount)) {
        addDialogButton("Mark as Unwatched", [capturedSeason, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedSeason]() {
                PlexClient::getInstance().markAsUnwatched(capturedSeason.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as unwatched");
                });
            });
            return true;
        });
    } else {
        addDialogButton("Mark as Watched", [capturedSeason, dialog](brls::View*) {
            dialog->dismiss();
            asyncRun([capturedSeason]() {
                PlexClient::getInstance().markAsWatched(capturedSeason.ratingKey);
                brls::sync([]() {
                    brls::Application::notify("Marked as watched");
                });
            });
            return true;
        });
    }

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::showArtistContextMenuStatic(const MediaItem& artist) {
    auto* dialog = new brls::Dialog(artist.title);

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

    MediaItem capturedArtist = artist;

    // Shuffle Artist - add all tracks to queue in random order
    addDialogButton("Shuffle Artist", [capturedArtist, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedArtist]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> albums;
            std::vector<MediaItem> allTracks;

            if (client.fetchChildren(capturedArtist.ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (auto& track : tracks) {
                            allTracks.push_back(track);
                        }
                    }
                }
            }

            if (allTracks.empty()) {
                brls::sync([]() { brls::Application::notify("No tracks found"); });
                return;
            }

            // Shuffle
            srand((unsigned)time(nullptr));
            for (size_t i = allTracks.size() - 1; i > 0; i--) {
                size_t j = rand() % (i + 1);
                std::swap(allTracks[i], allTracks[j]);
            }

            brls::sync([allTracks]() {
                auto* playerActivity = PlayerActivity::createWithQueue(allTracks, 0);
                brls::Application::pushActivity(playerActivity);
            });
        });
        return true;
    });

    // Play All (in order)
    addDialogButton("Play All", [capturedArtist, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedArtist]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> albums;
            std::vector<MediaItem> allTracks;

            if (client.fetchChildren(capturedArtist.ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (auto& track : tracks) {
                            allTracks.push_back(track);
                        }
                    }
                }
            }

            if (allTracks.empty()) {
                brls::sync([]() { brls::Application::notify("No tracks found"); });
                return;
            }

            brls::sync([allTracks]() {
                auto* playerActivity = PlayerActivity::createWithQueue(allTracks, 0);
                brls::Application::pushActivity(playerActivity);
            });
        });
        return true;
    });

    // Download Artist
    addDialogButton("Download Artist", [capturedArtist, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedArtist]() {
            PlexClient& client = PlexClient::getInstance();
            std::vector<MediaItem> albums;
            int queued = 0;

            if (client.fetchChildren(capturedArtist.ratingKey, albums)) {
                for (const auto& album : albums) {
                    std::vector<MediaItem> tracks;
                    if (client.fetchChildren(album.ratingKey, tracks)) {
                        for (const auto& track : tracks) {
                            MediaItem fullItem;
                            if (client.fetchMediaDetails(track.ratingKey, fullItem) && !fullItem.partPath.empty()) {
                                if (DownloadsManager::getInstance().queueDownload(
                                    fullItem.ratingKey, fullItem.title, fullItem.partPath,
                                    fullItem.duration, "track",
                                    capturedArtist.title, 0, fullItem.index,
                                    fullItem.thumb)) {
                                    queued++;
                                }
                            }
                        }
                    }
                }
            }

            DownloadsManager::getInstance().startDownloads();
            brls::sync([queued]() {
                brls::Application::notify("Queued " + std::to_string(queued) + " tracks");
            });
        });
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::toggleDescription() {
    if (!m_summaryLabel || m_fullDescription.empty()) return;

    m_descriptionExpanded = !m_descriptionExpanded;

    if (m_descriptionExpanded) {
        // Expand inline: show full text, set max height so it scrolls within the page
        m_summaryLabel->setText(m_fullDescription + "\n\n[L] Collapse");
        // Allow the label to grow tall - the parent ScrollingFrame handles scrolling
        m_summaryLabel->setMarginBottom(20);
    } else {
        // Collapse back to truncated preview
        std::string truncatedDesc = m_fullDescription;
        if (truncatedDesc.length() > 80) {
            truncatedDesc = truncatedDesc.substr(0, 77) + "... [L]";
        }
        m_summaryLabel->setText(truncatedDesc);
        m_summaryLabel->setMarginBottom(20);
    }
}

} // namespace vitaplex
