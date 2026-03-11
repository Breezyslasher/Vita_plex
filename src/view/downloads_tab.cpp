/**
 * VitaPlex - Downloads Tab Implementation
 * Queue display with start/stop/pause controls and auto-refresh progress.
 *
 * Based on Vita_Suwayomi's downloads tab patterns:
 * - Start/Stop/Pause/Resume/Clear action buttons
 * - Auto-refresh with incremental UI updates
 * - Per-item cancel/delete actions
 * - Color-coded status display
 */

#include "view/downloads_tab.hpp"
#include "app/downloads_manager.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/async.hpp"

#include <memory>
#include <thread>
#include <chrono>
#include <set>

namespace vitaplex {

DownloadsTab::DownloadsTab()
    : m_alive(std::make_shared<bool>(true))
{
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Header
    auto header = new brls::Label();
    header->setText("Downloads");
    header->setFontSize(24);
    header->setMargins(0, 0, 10, 0);
    this->addView(header);

    // Status label
    m_statusLabel = new brls::Label();
    m_statusLabel->setFontSize(14);
    m_statusLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    m_statusLabel->setMargins(0, 0, 10, 0);
    this->addView(m_statusLabel);

    // Action buttons row
    m_actionsRow = new brls::Box();
    m_actionsRow->setAxis(brls::Axis::ROW);
    m_actionsRow->setMargins(0, 0, 15, 0);

    // Start/Stop button
    m_startStopBtn = new brls::Button();
    m_startStopLabel = new brls::Label();
    m_startStopLabel->setText("Start");
    m_startStopLabel->setFontSize(16);
    m_startStopBtn->addView(m_startStopLabel);
    m_startStopBtn->setMargins(0, 10, 0, 0);
    m_startStopBtn->registerClickAction([this](brls::View*) {
        auto& mgr = DownloadsManager::getInstance();
        if (mgr.isDownloading()) {
            mgr.pauseDownloads();
            m_startStopLabel->setText("Start");
            brls::Application::notify("Downloads paused");
        } else {
            mgr.startDownloads();
            m_startStopLabel->setText("Stop");
            brls::Application::notify("Downloads started");
        }
        return true;
    });
    m_actionsRow->addView(m_startStopBtn);

    // Resume button (for paused/failed items)
    m_resumeBtn = new brls::Button();
    auto resumeLabel = new brls::Label();
    resumeLabel->setText("Resume All");
    resumeLabel->setFontSize(16);
    m_resumeBtn->addView(resumeLabel);
    m_resumeBtn->setMargins(0, 10, 0, 0);
    m_resumeBtn->registerClickAction([this](brls::View*) {
        auto& mgr = DownloadsManager::getInstance();
        int count = mgr.countIncompleteDownloads();
        if (count > 0) {
            mgr.resumeIncompleteDownloads();
            m_startStopLabel->setText("Stop");
            brls::Application::notify("Resuming " + std::to_string(count) + " downloads");
        } else {
            brls::Application::notify("No incomplete downloads");
        }
        return true;
    });
    m_actionsRow->addView(m_resumeBtn);

    // Sync button
    auto syncBtn = new brls::Button();
    auto syncLabel = new brls::Label();
    syncLabel->setText("Sync");
    syncLabel->setFontSize(16);
    syncBtn->addView(syncLabel);
    syncBtn->setMargins(0, 10, 0, 0);
    syncBtn->registerClickAction([](brls::View*) {
        DownloadsManager::getInstance().syncProgressBidirectional();
        brls::Application::notify("Progress synced");
        return true;
    });
    m_actionsRow->addView(syncBtn);

    // Clear completed button
    m_clearBtn = new brls::Button();
    auto clearLabel = new brls::Label();
    clearLabel->setText("Clear Done");
    clearLabel->setFontSize(16);
    m_clearBtn->addView(clearLabel);
    m_clearBtn->setMargins(0, 10, 0, 0);
    m_clearBtn->registerClickAction([this](brls::View*) {
        DownloadsManager::getInstance().clearCompleted();
        brls::Application::notify("Cleared completed downloads");
        refresh();
        return true;
    });
    m_actionsRow->addView(m_clearBtn);

    this->addView(m_actionsRow);

    // Scrollable list container
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_listContainer = new brls::Box();
    m_listContainer->setAxis(brls::Axis::COLUMN);
    m_listContainer->setGrow(1.0f);

    // Empty label
    m_emptyLabel = new brls::Label();
    m_emptyLabel->setText("No downloads yet.\nUse the download button on media details to save for offline viewing.");
    m_emptyLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_emptyLabel->setVerticalAlign(brls::VerticalAlign::CENTER);
    m_emptyLabel->setGrow(1.0f);
    m_emptyLabel->setVisibility(brls::Visibility::GONE);
    m_listContainer->addView(m_emptyLabel);

    m_scrollView->setContentView(m_listContainer);
    this->addView(m_scrollView);
}

DownloadsTab::~DownloadsTab() {
    *m_alive = false;
    stopAutoRefresh();
}

void DownloadsTab::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);

    // Update start/stop button label
    if (DownloadsManager::getInstance().isDownloading()) {
        m_startStopLabel->setText("Stop");
    } else {
        m_startStopLabel->setText("Start");
    }

    refresh();
    startAutoRefresh();
}

void DownloadsTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    stopAutoRefresh();
}

void DownloadsTab::startAutoRefresh() {
    if (m_autoRefreshEnabled.load()) return;
    m_autoRefreshEnabled.store(true);

    auto aliveWeak = std::weak_ptr<bool>(m_alive);

    asyncRun([this, aliveWeak]() {
        while (m_autoRefreshEnabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_INTERVAL_MS));

            auto alive = aliveWeak.lock();
            if (!alive || !*alive || !m_autoRefreshEnabled.load()) break;

            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                refresh();
            });
        }
    });
}

void DownloadsTab::stopAutoRefresh() {
    m_autoRefreshEnabled.store(false);
}

void DownloadsTab::refresh() {
    auto downloads = DownloadsManager::getInstance().getDownloads();

    // Update status label
    int queued = 0, downloading = 0, completed = 0, paused = 0, failed = 0;
    for (const auto& d : downloads) {
        switch (d.state) {
            case DownloadState::QUEUED: queued++; break;
            case DownloadState::DOWNLOADING: downloading++; break;
            case DownloadState::COMPLETED: completed++; break;
            case DownloadState::PAUSED: paused++; break;
            case DownloadState::FAILED: failed++; break;
            default: break;
        }
    }

    std::string status;
    if (downloads.empty()) {
        status = "No downloads";
    } else {
        status = std::to_string(downloads.size()) + " items";
        if (downloading > 0) status += " | " + std::to_string(downloading) + " downloading";
        if (queued > 0) status += " | " + std::to_string(queued) + " queued";
        if (paused > 0) status += " | " + std::to_string(paused) + " paused";
        if (failed > 0) status += " | " + std::to_string(failed) + " failed";
        if (completed > 0) status += " | " + std::to_string(completed) + " ready";
    }
    m_statusLabel->setText(status);

    // Update start/stop button
    if (DownloadsManager::getInstance().isDownloading()) {
        m_startStopLabel->setText("Stop");
    } else {
        m_startStopLabel->setText("Start");
    }

    // Build current state for comparison
    std::vector<CachedItem> currentState;
    currentState.reserve(downloads.size());
    for (const auto& d : downloads) {
        CachedItem ci;
        ci.ratingKey = d.ratingKey;
        ci.downloadedBytes = d.downloadedBytes;
        ci.totalBytes = d.totalBytes;
        ci.state = static_cast<int>(d.state);
        ci.viewOffset = d.viewOffset;
        currentState.push_back(ci);
    }

    // Check if state changed
    bool stateChanged = (currentState.size() != m_lastState.size());
    if (!stateChanged) {
        for (size_t i = 0; i < currentState.size(); i++) {
            if (currentState[i].ratingKey != m_lastState[i].ratingKey ||
                currentState[i].state != m_lastState[i].state ||
                currentState[i].downloadedBytes != m_lastState[i].downloadedBytes) {
                stateChanged = true;
                break;
            }
        }
    }

    if (!stateChanged) return;  // Nothing changed, skip UI rebuild
    m_lastState = currentState;

    // Try incremental update: remove rows for items that no longer exist,
    // then update status on remaining items that match
    if (!m_rowElements.empty()) {
        // Build set of current ratingKeys for quick lookup
        std::set<std::string> currentKeys;
        for (const auto& d : downloads) {
            currentKeys.insert(d.ratingKey);
        }

        // Remove rows for items that were deleted/cancelled
        // Walk backwards to avoid index shifting issues
        for (int i = (int)m_rowElements.size() - 1; i >= 0; i--) {
            if (currentKeys.find(m_rowElements[i].ratingKey) == currentKeys.end()) {
                if (m_rowElements[i].row) {
                    m_listContainer->removeView(m_rowElements[i].row);
                }
                m_rowElements.erase(m_rowElements.begin() + i);
            }
        }

        // Check if remaining items match the download list (same order, same keys)
        bool canUpdate = (downloads.size() == m_rowElements.size());
        if (canUpdate) {
            for (size_t i = 0; i < downloads.size(); i++) {
                if (downloads[i].ratingKey != m_rowElements[i].ratingKey) {
                    canUpdate = false;
                    break;
                }
            }
        }

        if (canUpdate) {
            // Update status labels and colors in-place
            for (size_t i = 0; i < downloads.size(); i++) {
                const auto& item = downloads[i];
                auto& row = m_rowElements[i];

                std::string statusText;
                switch (item.state) {
                    case DownloadState::QUEUED:
                        statusText = "Queued";
                        break;
                    case DownloadState::DOWNLOADING:
                        if (item.totalBytes > 0) {
                            int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                            int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                            int64_t totalMB = item.totalBytes / (1024 * 1024);
                            statusText = "Downloading... " + std::to_string(percent) + "% (" +
                                        std::to_string(dlMB) + "/" + std::to_string(totalMB) + " MB)";
                        } else {
                            int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                            statusText = "Downloading... " + std::to_string(dlMB) + " MB";
                        }
                        break;
                    case DownloadState::PAUSED:
                        statusText = "Paused";
                        if (item.totalBytes > 0) {
                            int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                            statusText += " (" + std::to_string(percent) + "%)";
                        }
                        break;
                    case DownloadState::COMPLETED:
                        statusText = "Ready to play";
                        if (item.viewOffset > 0) {
                            int minutes = (int)(item.viewOffset / 60000);
                            statusText += " (" + std::to_string(minutes) + " min watched)";
                        }
                        break;
                    case DownloadState::FAILED:
                        statusText = "Failed";
                        break;
                    default:
                        break;
                }

                if (row.statusLabel) {
                    row.statusLabel->setText(statusText);
                }

                // Update row background color based on state
                if (row.row) {
                    NVGcolor bgColor;
                    switch (item.state) {
                        case DownloadState::DOWNLOADING:
                            bgColor = nvgRGBA(20, 60, 20, 200);  // Green tint
                            break;
                        case DownloadState::PAUSED:
                            bgColor = nvgRGBA(60, 50, 10, 200);  // Amber tint
                            break;
                        case DownloadState::FAILED:
                            bgColor = nvgRGBA(60, 20, 20, 200);  // Red tint
                            break;
                        default:
                            bgColor = nvgRGBA(40, 40, 40, 200);
                            break;
                    }
                    row.row->setBackgroundColor(bgColor);
                }
            }

            // Show/hide empty label
            if (downloads.empty()) {
                m_emptyLabel->setVisibility(brls::Visibility::VISIBLE);
            } else {
                m_emptyLabel->setVisibility(brls::Visibility::GONE);
            }
            return;
        }
    }

    // Full rebuild needed (new items added or order changed)
    // Clear existing items (except empty label which is always last)
    while (m_listContainer->getChildren().size() > 1) {
        m_listContainer->removeView(m_listContainer->getChildren()[0]);
    }
    m_rowElements.clear();

    if (downloads.empty()) {
        m_emptyLabel->setVisibility(brls::Visibility::VISIBLE);
        return;
    }

    m_emptyLabel->setVisibility(brls::Visibility::GONE);

    for (const auto& item : downloads) {
        auto row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPadding(10);
        row->setMargins(0, 0, 8, 0);
        row->setCornerRadius(8);

        // Color-coded background based on state
        NVGcolor bgColor;
        switch (item.state) {
            case DownloadState::DOWNLOADING:
                bgColor = nvgRGBA(20, 60, 20, 200);  // Green tint
                break;
            case DownloadState::PAUSED:
                bgColor = nvgRGBA(60, 50, 10, 200);  // Amber tint
                break;
            case DownloadState::FAILED:
                bgColor = nvgRGBA(60, 20, 20, 200);  // Red tint
                break;
            case DownloadState::COMPLETED:
                bgColor = nvgRGBA(20, 40, 60, 200);  // Blue tint
                break;
            default:
                bgColor = nvgRGBA(40, 40, 40, 200);
                break;
        }
        row->setBackgroundColor(bgColor);

        // Title and info column
        auto infoBox = new brls::Box();
        infoBox->setAxis(brls::Axis::COLUMN);
        infoBox->setGrow(1.0f);

        auto titleLabel = new brls::Label();
        std::string displayTitle = item.title;
        if (!item.parentTitle.empty()) {
            displayTitle = item.parentTitle + " - " + item.title;
        }
        if (item.seasonNum > 0 && item.episodeNum > 0) {
            char epStr[32];
            snprintf(epStr, sizeof(epStr), " (S%02dE%02d)", item.seasonNum, item.episodeNum);
            displayTitle += epStr;
        }
        titleLabel->setText(displayTitle);
        titleLabel->setFontSize(18);
        infoBox->addView(titleLabel);

        // Status/progress label
        auto statusLabel = new brls::Label();
        statusLabel->setFontSize(14);

        std::string statusText;
        switch (item.state) {
            case DownloadState::QUEUED:
                statusText = "Queued";
                break;
            case DownloadState::DOWNLOADING:
                if (item.totalBytes > 0) {
                    int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                    int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                    int64_t totalMB = item.totalBytes / (1024 * 1024);
                    statusText = "Downloading... " + std::to_string(percent) + "% (" +
                                std::to_string(dlMB) + "/" + std::to_string(totalMB) + " MB)";
                } else {
                    int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                    statusText = "Downloading... " + std::to_string(dlMB) + " MB";
                }
                break;
            case DownloadState::PAUSED:
                statusText = "Paused";
                if (item.totalBytes > 0) {
                    int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                    statusText += " (" + std::to_string(percent) + "%)";
                }
                break;
            case DownloadState::COMPLETED:
                statusText = "Ready to play";
                if (item.viewOffset > 0) {
                    int minutes = (int)(item.viewOffset / 60000);
                    statusText += " (" + std::to_string(minutes) + " min watched)";
                }
                break;
            case DownloadState::FAILED:
                statusText = "Failed";
                break;
            default:
                break;
        }
        statusLabel->setText(statusText);
        statusLabel->setTextColor(nvgRGBA(200, 200, 200, 255));
        infoBox->addView(statusLabel);

        row->addView(infoBox);

        // Action buttons based on state
        auto buttonsBox = new brls::Box();
        buttonsBox->setAxis(brls::Axis::ROW);

        if (item.state == DownloadState::COMPLETED) {
            auto playBtn = new brls::Button();
            auto playLabel = new brls::Label();
            playLabel->setText("Play");
            playLabel->setFontSize(14);
            playBtn->addView(playLabel);
            playBtn->setMargins(0, 0, 0, 5);

            std::string ratingKey = item.ratingKey;
            bool isMusic = (item.mediaType == "track");
            DownloadItem capturedItem = item;
            playBtn->registerClickAction([ratingKey, isMusic, capturedItem](brls::View*) {
                if (isMusic) {
                    // Build a MediaItem from the download for queue integration
                    MediaItem mi;
                    mi.ratingKey = capturedItem.ratingKey;
                    mi.title = capturedItem.title;
                    mi.grandparentTitle = capturedItem.parentTitle;  // artist
                    mi.parentTitle = capturedItem.parentTitle;       // album/artist
                    mi.duration = capturedItem.duration;
                    mi.thumb = capturedItem.thumbUrl;

                    // Helper lambda to execute a track action with queue integration
                    auto executeAction = [](const MediaItem& track, TrackDefaultAction act) {
                        MusicQueue& queue = MusicQueue::getInstance();
                        switch (act) {
                            case TrackDefaultAction::PLAY_NEXT:
                                if (queue.isEmpty()) {
                                    std::vector<MediaItem> single = {track};
                                    brls::Application::pushActivity(
                                        PlayerActivity::createWithQueue(single, 0));
                                } else {
                                    queue.insertTrackAfterCurrent(track);
                                    brls::Application::notify("Playing next: " + track.title);
                                }
                                break;

                            case TrackDefaultAction::PLAY_NOW_REPLACE:
                                if (queue.isEmpty()) {
                                    std::vector<MediaItem> single = {track};
                                    brls::Application::pushActivity(
                                        PlayerActivity::createWithQueue(single, 0));
                                } else {
                                    queue.insertTrackAfterCurrent(track);
                                    if (queue.playNext()) {
                                        brls::Application::notify("Now playing: " + track.title);
                                    }
                                }
                                break;

                            case TrackDefaultAction::ADD_TO_BOTTOM:
                                if (queue.isEmpty()) {
                                    std::vector<MediaItem> single = {track};
                                    brls::Application::pushActivity(
                                        PlayerActivity::createWithQueue(single, 0));
                                } else {
                                    queue.addTrack(track);
                                    brls::Application::notify("Added to queue: " + track.title);
                                }
                                break;

                            case TrackDefaultAction::PLAY_NOW_CLEAR:
                            default: {
                                std::vector<MediaItem> single = {track};
                                brls::Application::pushActivity(
                                    PlayerActivity::createWithQueue(single, 0));
                                break;
                            }
                        }
                    };

                    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;

                    if (action == TrackDefaultAction::ASK_EACH_TIME) {
                        // Show action dialog
                        auto* dialog = new brls::Dialog("Choose Action");
                        auto* optionsBox = new brls::Box();
                        optionsBox->setAxis(brls::Axis::COLUMN);
                        optionsBox->setPadding(20);

                        auto addBtn = [&optionsBox, &mi, dialog, executeAction](
                                const std::string& text, TrackDefaultAction act) {
                            auto* btn = new brls::Button();
                            btn->setText(text);
                            btn->setHeight(44);
                            btn->setMarginBottom(10);
                            MediaItem captured = mi;
                            btn->registerClickAction([dialog, captured, executeAction, act](brls::View*) {
                                dialog->dismiss();
                                executeAction(captured, act);
                                return true;
                            });
                            btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
                            optionsBox->addView(btn);
                        };

                        addBtn("Play Now (Clear Queue)", TrackDefaultAction::PLAY_NOW_CLEAR);
                        addBtn("Play Next", TrackDefaultAction::PLAY_NEXT);
                        addBtn("Add to Bottom of Queue", TrackDefaultAction::ADD_TO_BOTTOM);
                        addBtn("Play Now (Replace Current)", TrackDefaultAction::PLAY_NOW_REPLACE);

                        dialog->addView(optionsBox);
                        dialog->open();
                    } else {
                        executeAction(mi, action);
                    }
                } else {
                    // Video: play directly as local file
                    brls::Application::pushActivity(new PlayerActivity(ratingKey, true));
                }
                return true;
            });
            buttonsBox->addView(playBtn);

            auto deleteBtn = new brls::Button();
            auto delLabel = new brls::Label();
            delLabel->setText("Delete");
            delLabel->setFontSize(14);
            deleteBtn->addView(delLabel);

            std::string key = item.ratingKey;
            deleteBtn->registerClickAction([this, key](brls::View*) {
                DownloadsManager::getInstance().deleteDownload(key);
                brls::Application::notify("Download deleted");
                m_lastState.clear();  // Force refresh on next auto-refresh cycle
                return true;
            });
            buttonsBox->addView(deleteBtn);
        } else if (item.state == DownloadState::DOWNLOADING ||
                   item.state == DownloadState::QUEUED) {
            auto cancelBtn = new brls::Button();
            auto cancelLabel = new brls::Label();
            cancelLabel->setText("Cancel");
            cancelLabel->setFontSize(14);
            cancelBtn->addView(cancelLabel);

            std::string key = item.ratingKey;
            cancelBtn->registerClickAction([this, key](brls::View*) {
                DownloadsManager::getInstance().cancelDownload(key);
                brls::Application::notify("Download cancelled");
                m_lastState.clear();  // Force refresh on next auto-refresh cycle
                return true;
            });
            buttonsBox->addView(cancelBtn);
        } else if (item.state == DownloadState::PAUSED ||
                   item.state == DownloadState::FAILED) {
            // Show cancel to remove from queue
            auto cancelBtn = new brls::Button();
            auto cancelLabel = new brls::Label();
            cancelLabel->setText("Remove");
            cancelLabel->setFontSize(14);
            cancelBtn->addView(cancelLabel);

            std::string key = item.ratingKey;
            cancelBtn->registerClickAction([this, key](brls::View*) {
                DownloadsManager::getInstance().cancelDownload(key);
                brls::Application::notify("Download removed");
                m_lastState.clear();  // Force refresh on next auto-refresh cycle
                return true;
            });
            buttonsBox->addView(cancelBtn);
        }

        row->addView(buttonsBox);

        // Track elements for incremental updates
        RowElements re;
        re.row = row;
        re.statusLabel = statusLabel;
        re.ratingKey = item.ratingKey;
        m_rowElements.push_back(re);

        // Add row before the empty label
        m_listContainer->addView(row, m_listContainer->getChildren().size() - 1);
    }
}

} // namespace vitaplex
