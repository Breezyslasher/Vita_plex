/**
 * VitaPlex - Media Detail View implementation
 */

#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "view/progress_dialog.hpp"
#include "app/application.hpp"
#include "app/downloads_manager.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include <thread>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace vitaplex {

MediaDetailView::MediaDetailView(const MediaItem& item)
    : m_item(item) {

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

    m_posterImage = new brls::Image();
    if (isMusic) {
        // Square album art
        m_posterImage->setWidth(200);
        m_posterImage->setHeight(200);
    } else {
        // Portrait poster
        m_posterImage->setWidth(200);
        m_posterImage->setHeight(300);
    }
    m_posterImage->setScalingType(brls::ImageScalingType::FIT);
    leftBox->addView(m_posterImage);

    // Play buttons (not for artists)
    if (m_item.mediaType != MediaType::MUSIC_ARTIST) {
        m_playButton = new brls::Button();
        m_playButton->setText("Play");
        m_playButton->setWidth(200);
        m_playButton->setMarginTop(20);
        m_playButton->registerClickAction([this](brls::View* view) {
            onPlay(false);
            return true;
        });
        leftBox->addView(m_playButton);

        if (m_item.viewOffset > 0) {
            m_resumeButton = new brls::Button();
            m_resumeButton->setText("Resume");
            m_resumeButton->setWidth(200);
            m_resumeButton->setMarginTop(10);
            m_resumeButton->registerClickAction([this](brls::View* view) {
                onPlay(true);
                return true;
            });
            leftBox->addView(m_resumeButton);
        }

        // Download button (for movies, episodes, and tracks)
        if (m_item.mediaType == MediaType::MOVIE || m_item.mediaType == MediaType::EPISODE ||
            m_item.mediaType == MediaType::MUSIC_TRACK) {
            m_downloadButton = new brls::Button();

            // Check if already downloaded
            if (DownloadsManager::getInstance().isDownloaded(m_item.ratingKey)) {
                m_downloadButton->setText("Downloaded");
            } else {
                m_downloadButton->setText("Download");
            }

            m_downloadButton->setWidth(200);
            m_downloadButton->setMarginTop(10);
            m_downloadButton->registerClickAction([this](brls::View* view) {
                onDownload();
                return true;
            });
            leftBox->addView(m_downloadButton);
        }
    }

    // Download options for shows, seasons, albums
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON ||
        m_item.mediaType == MediaType::MUSIC_ALBUM ||
        m_item.mediaType == MediaType::MUSIC_ARTIST) {

        m_downloadButton = new brls::Button();
        m_downloadButton->setText("Download...");
        m_downloadButton->setWidth(200);
        m_downloadButton->setMarginTop(10);
        m_downloadButton->registerClickAction([this](brls::View* view) {
            showDownloadOptions();
            return true;
        });
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

    // Summary
    if (!m_item.summary.empty()) {
        m_summaryLabel = new brls::Label();
        m_summaryLabel->setText(m_item.summary);
        m_summaryLabel->setFontSize(16);
        m_summaryLabel->setMarginBottom(20);
        rightBox->addView(m_summaryLabel);
    }

    topRow->addView(rightBox);
    m_mainContent->addView(topRow);

    // Children container (for shows/seasons/albums - but NOT artists)
    if (m_item.mediaType == MediaType::SHOW ||
        m_item.mediaType == MediaType::SEASON ||
        m_item.mediaType == MediaType::MUSIC_ALBUM) {

        auto* childrenLabel = new brls::Label();
        if (m_item.mediaType == MediaType::SHOW) {
            childrenLabel->setText("Seasons");
        } else if (m_item.mediaType == MediaType::SEASON) {
            childrenLabel->setText("Episodes");
        } else {
            childrenLabel->setText("Tracks");
        }
        childrenLabel->setFontSize(20);
        childrenLabel->setMarginBottom(10);
        m_mainContent->addView(childrenLabel);

        auto* childrenScroll = new brls::HScrollingFrame();
        childrenScroll->setHeight(180);
        childrenScroll->setMarginBottom(20);

        m_childrenBox = new brls::Box();
        m_childrenBox->setAxis(brls::Axis::ROW);
        m_childrenBox->setJustifyContent(brls::JustifyContent::FLEX_START);

        childrenScroll->setContentView(m_childrenBox);
        m_mainContent->addView(childrenScroll);
    }

    // Music categories container for artists
    if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        m_musicCategoriesBox = new brls::Box();
        m_musicCategoriesBox->setAxis(brls::Axis::COLUMN);
        m_mainContent->addView(m_musicCategoriesBox);
    }

    m_scrollView->setContentView(m_mainContent);
    this->addView(m_scrollView);

    // Load full details
    loadDetails();
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
    PlexClient& client = PlexClient::getInstance();

    // Load full details
    MediaItem fullItem;
    if (client.fetchMediaDetails(m_item.ratingKey, fullItem)) {
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
            if (DownloadsManager::getInstance().isDownloaded(m_item.ratingKey)) {
                m_downloadButton->setText("Downloaded");
            } else {
                m_downloadButton->setText("Download");
            }
            brls::Logger::debug("loadDetails: partPath available, download enabled");
        }
    }

    // Load thumbnail with appropriate aspect ratio
    if (m_posterImage && !m_item.thumb.empty()) {
        bool isMusic = (m_item.mediaType == MediaType::MUSIC_ARTIST ||
                        m_item.mediaType == MediaType::MUSIC_ALBUM ||
                        m_item.mediaType == MediaType::MUSIC_TRACK);

        int width = isMusic ? 400 : 400;
        int height = isMusic ? 400 : 600;

        std::string url = client.getThumbnailUrl(m_item.thumb, width, height);
        ImageLoader::loadAsync(url, [this](brls::Image* image) {
            // Image loaded
        }, m_posterImage);
    }

    // Load children if applicable
    if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        loadMusicCategories();
    } else {
        loadChildren();
    }
}

void MediaDetailView::loadChildren() {
    if (!m_childrenBox) return;

    PlexClient& client = PlexClient::getInstance();

    if (client.fetchChildren(m_item.ratingKey, m_children)) {
        m_childrenBox->clearViews();

        for (const auto& child : m_children) {
            auto* cell = new MediaItemCell();
            cell->setItem(child);
            cell->setWidth(120);
            cell->setHeight(150);
            cell->setMarginRight(10);

            cell->registerClickAction([this, child](brls::View* view) {
                // Navigate to child detail
                auto* detailView = new MediaDetailView(child);
                brls::Application::pushActivity(new brls::Activity(detailView));
                return true;
            });

            m_childrenBox->addView(cell);
        }
    }
}

void MediaDetailView::loadMusicCategories() {
    if (!m_musicCategoriesBox) return;

    asyncRun([this]() {
        PlexClient& client = PlexClient::getInstance();

        std::vector<MediaItem> allAlbums;
        if (!client.fetchChildren(m_item.ratingKey, allAlbums)) {
            brls::Logger::error("Failed to fetch albums for artist");
            return;
        }

        // Categorize albums by subtype
        std::vector<MediaItem> albums;
        std::vector<MediaItem> singles;
        std::vector<MediaItem> eps;
        std::vector<MediaItem> compilations;
        std::vector<MediaItem> soundtracks;
        std::vector<MediaItem> other;

        for (const auto& album : allAlbums) {
            std::string subtype = album.subtype;
            // Convert to lowercase for comparison
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

        brls::Logger::info("Music categories: {} albums, {} singles, {} EPs, {} compilations, {} soundtracks, {} other",
                           albums.size(), singles.size(), eps.size(),
                           compilations.size(), soundtracks.size(), other.size());

        // Update UI on main thread
        brls::sync([this, albums, singles, eps, compilations, soundtracks, other]() {
            m_musicCategoriesBox->clearViews();

            auto addCategory = [this](const std::string& title, const std::vector<MediaItem>& items) {
                if (items.empty()) return;

                brls::Box* content = nullptr;
                createMediaRow(title, &content);

                for (const auto& item : items) {
                    auto* cell = new MediaItemCell();
                    cell->setItem(item);
                    cell->setWidth(120);
                    cell->setHeight(150);
                    cell->setMarginRight(10);

                    MediaItem capturedItem = item;
                    cell->registerClickAction([this, capturedItem](brls::View* view) {
                        auto* detailView = new MediaDetailView(capturedItem);
                        brls::Application::pushActivity(new brls::Activity(detailView));
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

        Application::getInstance().pushPlayerActivity(m_item.ratingKey);
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
    // Check if already downloaded
    if (DownloadsManager::getInstance().isDownloaded(m_item.ratingKey)) {
        brls::Application::notify("Already downloaded");
        return;
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

    // Queue the download
    bool queued = DownloadsManager::getInstance().queueDownload(
        m_item.ratingKey,
        m_item.title,
        m_item.partPath,
        m_item.duration,
        mediaType,
        parentTitle,
        seasonNum,
        episodeNum
    );

    if (queued) {
        if (m_downloadButton) {
            m_downloadButton->setText("Downloading...");
        }

        // Show progress dialog
        auto* progressDialog = new ProgressDialog("Downloading");
        progressDialog->setStatus(m_item.title);
        progressDialog->setProgress(0);
        progressDialog->show();

        // Track the rating key to update button when done
        std::string ratingKey = m_item.ratingKey;
        brls::Button* downloadBtn = m_downloadButton;

        // Set progress callback with speed display
        DownloadsManager::getInstance().setProgressCallback(
            [progressDialog](int64_t downloaded, int64_t total) {
                brls::sync([progressDialog, downloaded, total]() {
                    progressDialog->updateDownloadProgress(downloaded, total);
                });
            }
        );

        // Allow dismissing dialog - download continues in background
        progressDialog->setCancelCallback([progressDialog, downloadBtn]() {
            brls::Application::notify("Download continues in background");
            // Clear callback to avoid updating dismissed dialog
            DownloadsManager::getInstance().setProgressCallback(nullptr);
        });

        // Start downloading
        DownloadsManager::getInstance().startDownloads();

        // Monitor for completion
        asyncRun([progressDialog, downloadBtn, ratingKey]() {
            while (true) {
                auto* item = DownloadsManager::getInstance().getDownload(ratingKey);
                if (!item) break;

                if (item->state == DownloadState::COMPLETED) {
                    brls::sync([progressDialog, downloadBtn]() {
                        progressDialog->setStatus("Download complete!");
                        progressDialog->setProgress(1.0f);
                        if (downloadBtn) {
                            downloadBtn->setText("Downloaded");
                        }
                        brls::delay(1500, [progressDialog]() {
                            progressDialog->dismiss();
                        });
                    });
                    break;
                } else if (item->state == DownloadState::FAILED) {
                    brls::sync([progressDialog, downloadBtn]() {
                        progressDialog->setStatus("Download failed");
                        if (downloadBtn) {
                            downloadBtn->setText("Download");
                        }
                        brls::delay(2000, [progressDialog]() {
                            progressDialog->dismiss();
                        });
                    });
                    break;
                }

                // Sleep briefly before checking again
#ifdef __vita__
                sceKernelDelayThread(500 * 1000);  // 500ms
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
#endif
            }

            // Clear progress callback
            DownloadsManager::getInstance().setProgressCallback(nullptr);
        });
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

    // Different options based on media type
    if (m_item.mediaType == MediaType::SHOW) {
        // Show options: Download all seasons, download unwatched
        auto* downloadAllBtn = new brls::Button();
        downloadAllBtn->setText("Download All Episodes");
        downloadAllBtn->setMarginBottom(10);
        downloadAllBtn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadAll();
            return true;
        });
        optionsBox->addView(downloadAllBtn);

        auto* downloadUnwatchedBtn = new brls::Button();
        downloadUnwatchedBtn->setText("Download Unwatched");
        downloadUnwatchedBtn->setMarginBottom(10);
        downloadUnwatchedBtn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadUnwatched();
            return true;
        });
        optionsBox->addView(downloadUnwatchedBtn);

        auto* downloadNext5Btn = new brls::Button();
        downloadNext5Btn->setText("Download Next 5 Unwatched");
        downloadNext5Btn->setMarginBottom(10);
        downloadNext5Btn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadUnwatched(5);
            return true;
        });
        optionsBox->addView(downloadNext5Btn);

    } else if (m_item.mediaType == MediaType::SEASON) {
        // Season options: Download all episodes, download unwatched
        auto* downloadAllBtn = new brls::Button();
        downloadAllBtn->setText("Download All Episodes");
        downloadAllBtn->setMarginBottom(10);
        downloadAllBtn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadAll();
            return true;
        });
        optionsBox->addView(downloadAllBtn);

        auto* downloadUnwatchedBtn = new brls::Button();
        downloadUnwatchedBtn->setText("Download Unwatched");
        downloadUnwatchedBtn->setMarginBottom(10);
        downloadUnwatchedBtn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadUnwatched();
            return true;
        });
        optionsBox->addView(downloadUnwatchedBtn);

        auto* downloadNext3Btn = new brls::Button();
        downloadNext3Btn->setText("Download Next 3 Unwatched");
        downloadNext3Btn->setMarginBottom(10);
        downloadNext3Btn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadUnwatched(3);
            return true;
        });
        optionsBox->addView(downloadNext3Btn);

    } else if (m_item.mediaType == MediaType::MUSIC_ALBUM) {
        // Album options: Download all tracks
        auto* downloadAlbumBtn = new brls::Button();
        downloadAlbumBtn->setText("Download Album");
        downloadAlbumBtn->setMarginBottom(10);
        downloadAlbumBtn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadAll();
            return true;
        });
        optionsBox->addView(downloadAlbumBtn);

    } else if (m_item.mediaType == MediaType::MUSIC_ARTIST) {
        // Artist options: Download all albums
        auto* downloadAllBtn = new brls::Button();
        downloadAllBtn->setText("Download All Albums");
        downloadAllBtn->setMarginBottom(10);
        downloadAllBtn->registerClickAction([this, dialog](brls::View*) {
            dialog->dismiss();
            downloadAll();
            return true;
        });
        optionsBox->addView(downloadAllBtn);
    }

    // Cancel button
    auto* cancelBtn = new brls::Button();
    cancelBtn->setText("Cancel");
    cancelBtn->registerClickAction([dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    optionsBox->addView(cancelBtn);

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

} // namespace vitaplex
