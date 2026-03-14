/**
 * VitaPlex - Downloads Tab Implementation
 * Queue display with start/stop/pause controls and auto-refresh progress.
 * Groups downloads by playlist/album/artist with cover art and context menus.
 *
 * Based on Vita_Suwayomi's downloads tab patterns:
 * - Start/Stop/Pause/Resume/Clear action buttons
 * - Auto-refresh with smart rebuild detection
 * - Per-item cancel/delete actions
 * - Color-coded status display
 * - Grouped view for playlists, albums, artists
 * - Cover art / poster thumbnails
 * - START button context menu (Play Now, Add to Queue, Clear Queue)
 */

#include "view/downloads_tab.hpp"
#include "app/downloads_manager.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/async.hpp"
#include "utils/image_loader.hpp"

#include <memory>
#include <thread>
#include <chrono>
#include <set>
#include <map>

namespace vitaplex {

// Format elapsed seconds as a human-readable string like "1m 23s" or "45s"
static std::string formatElapsedTime(int totalSeconds) {
    if (totalSeconds < 60) {
        return std::to_string(totalSeconds) + "s";
    }
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
}

// Build a transcoding status string with elapsed time and animated dots
static std::string buildTranscodeStatus(int elapsedSeconds) {
    int dotCount = (elapsedSeconds % 3) + 1;
    std::string dots(dotCount, '.');

    std::string status = "Transcoding on server" + dots;
    if (elapsedSeconds > 0) {
        status += " (" + formatElapsedTime(elapsedSeconds) + " elapsed)";
    }
    return status;
}

static std::string buildItemStatusText(const DownloadItem& item) {
    switch (item.state) {
        case DownloadState::QUEUED:
            return "Queued";
        case DownloadState::TRANSCODING:
            return buildTranscodeStatus(item.transcodeElapsedSeconds);
        case DownloadState::DOWNLOADING:
            if (item.totalBytes > 0) {
                int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                int64_t totalMB = item.totalBytes / (1024 * 1024);
                return "Downloading... " + std::to_string(percent) + "% (" +
                       std::to_string(dlMB) + "/" + std::to_string(totalMB) + " MB)";
            } else {
                int64_t dlMB = item.downloadedBytes / (1024 * 1024);
                return "Downloading... " + std::to_string(dlMB) + " MB";
            }
        case DownloadState::PAUSED:
            if (item.totalBytes > 0) {
                int percent = (int)((item.downloadedBytes * 100) / item.totalBytes);
                return "Paused (" + std::to_string(percent) + "%)";
            }
            return "Paused";
        case DownloadState::COMPLETED:
            if (item.viewOffset > 0) {
                int minutes = (int)(item.viewOffset / 60000);
                return "Ready to play (" + std::to_string(minutes) + " min watched)";
            }
            return "Ready to play";
        case DownloadState::FAILED:
            return "Failed";
        default:
            return "";
    }
}

static NVGcolor getStateColor(DownloadState state) {
    switch (state) {
        case DownloadState::TRANSCODING: return nvgRGBA(50, 30, 60, 200);
        case DownloadState::DOWNLOADING: return nvgRGBA(20, 60, 20, 200);
        case DownloadState::PAUSED:      return nvgRGBA(60, 50, 10, 200);
        case DownloadState::FAILED:      return nvgRGBA(60, 20, 20, 200);
        case DownloadState::COMPLETED:   return nvgRGBA(20, 40, 60, 200);
        default:                         return nvgRGBA(40, 40, 40, 200);
    }
}

DownloadsTab::DownloadsTab()
    : m_alive(std::make_shared<bool>(true))
    , m_aliveAtomic(std::make_shared<std::atomic<bool>>(true))
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

    // Resume button
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
    m_syncBtn = new brls::Button();
    auto syncBtn = m_syncBtn;
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
        // Debounce: ignore rapid double-taps (< 500ms apart)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastClearTime).count();
        if (elapsed < 500) return true;
        m_lastClearTime = now;

        DownloadsManager::getInstance().clearCompleted();
        brls::Application::notify("Cleared completed downloads");
        m_lastState.clear();
        rebuildList();
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
    m_aliveAtomic->store(false);
    stopAutoRefresh();
}

void DownloadsTab::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);

    if (DownloadsManager::getInstance().isDownloading()) {
        m_startStopLabel->setText("Stop");
    } else {
        m_startStopLabel->setText("Start");
    }

    m_lastState.clear();
    rebuildList();
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
    int queued = 0, downloading = 0, transcoding = 0, completed = 0, paused = 0, failed = 0;
    for (const auto& d : downloads) {
        switch (d.state) {
            case DownloadState::QUEUED: queued++; break;
            case DownloadState::TRANSCODING: transcoding++; break;
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
        if (transcoding > 0) status += " | " + std::to_string(transcoding) + " transcoding";
        if (downloading > 0) status += " | " + std::to_string(downloading) + " downloading";
        if (queued > 0) status += " | " + std::to_string(queued) + " queued";
        if (paused > 0) status += " | " + std::to_string(paused) + " paused";
        if (failed > 0) status += " | " + std::to_string(failed) + " failed";
        if (completed > 0) status += " | " + std::to_string(completed) + " ready";
    }
    m_statusLabel->setText(status);

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
        ci.transcodeElapsedSeconds = d.transcodeElapsedSeconds;
        currentState.push_back(ci);
    }

    // Check if anything changed at all
    bool anyChange = (currentState.size() != m_lastState.size());
    if (!anyChange) {
        for (size_t i = 0; i < currentState.size(); i++) {
            if (currentState[i].ratingKey != m_lastState[i].ratingKey ||
                currentState[i].state != m_lastState[i].state ||
                currentState[i].downloadedBytes != m_lastState[i].downloadedBytes ||
                currentState[i].transcodeElapsedSeconds != m_lastState[i].transcodeElapsedSeconds) {
                anyChange = true;
                break;
            }
        }
    }

    if (!anyChange) return;

    // Check if only progress changed (same items, same states, just bytes/transcode time differ)
    // If so, update labels in-place without rebuilding the whole UI
    bool structureChanged = (currentState.size() != m_lastState.size());
    if (!structureChanged) {
        for (size_t i = 0; i < currentState.size(); i++) {
            if (currentState[i].ratingKey != m_lastState[i].ratingKey ||
                currentState[i].state != m_lastState[i].state) {
                structureChanged = true;
                break;
            }
        }
    }

    m_lastState = currentState;

    if (structureChanged) {
        // Full rebuild needed: items added/removed or state type changed
        rebuildList();
    } else {
        // Progress-only update: just update status text labels in-place
        updateProgressInPlace(downloads);
    }
}

void DownloadsTab::rebuildList() {
    auto downloads = DownloadsManager::getInstance().getDownloads();

    // Clear in-place update maps (old pointers are about to be destroyed)
    m_itemStatusLabels.clear();
    m_itemRows.clear();
    m_groupStatusLabels.clear();

    // Invalidate all in-flight async image loads from previous rebuild cycle.
    // This prevents use-after-free when old brls::Image* targets are destroyed
    // below but their async callbacks haven't fired yet.
    m_aliveAtomic->store(false);
    m_aliveAtomic = std::make_shared<std::atomic<bool>>(true);

    // Move focus away from list items before removing them to prevent
    // focus-related crashes when the currently-focused view is destroyed
    if (m_clearBtn) {
        brls::Application::giveFocus(m_clearBtn);
    }

    // Clear existing items (except empty label which is always last)
    while (m_listContainer->getChildren().size() > 1) {
        m_listContainer->removeView(m_listContainer->getChildren()[0]);
    }

    if (downloads.empty()) {
        m_emptyLabel->setVisibility(brls::Visibility::VISIBLE);
        return;
    }

    m_emptyLabel->setVisibility(brls::Visibility::GONE);

    // Group information
    struct GroupInfo {
        DownloadGroupType type;
        std::string key;
        std::string title;
        std::string thumb;
        int total = 0;
        int contentTotal = 0;  // Stable total from groupTotalItems
        int completed = 0;
        int downloading = 0;
    };

    // Collect groups and ungrouped items in order of first appearance
    std::vector<std::string> groupOrder;
    std::map<std::string, GroupInfo> groups;
    std::vector<DownloadItem> ungrouped;

    for (const auto& item : downloads) {
        if (item.groupType != DownloadGroupType::NONE && !item.groupKey.empty()) {
            std::string compositeKey = std::to_string(static_cast<int>(item.groupType)) + ":" + item.groupKey;
            auto it = groups.find(compositeKey);
            if (it == groups.end()) {
                GroupInfo gi;
                gi.type = item.groupType;
                gi.key = item.groupKey;
                gi.title = item.groupTitle;
                gi.thumb = item.groupThumb;
                gi.total = 1;
                gi.contentTotal = item.groupTotalItems;
                gi.completed = (item.state == DownloadState::COMPLETED) ? 1 : 0;
                gi.downloading = (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) ? 1 : 0;
                groups[compositeKey] = gi;
                groupOrder.push_back(compositeKey);
            } else {
                it->second.total++;
                if (item.groupTotalItems > it->second.contentTotal) it->second.contentTotal = item.groupTotalItems;
                if (item.state == DownloadState::COMPLETED) it->second.completed++;
                if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) it->second.downloading++;
                // Use first non-empty thumb
                if (it->second.thumb.empty() && !item.groupThumb.empty()) {
                    it->second.thumb = item.groupThumb;
                }
            }
        } else {
            ungrouped.push_back(item);
        }
    }

    // Add grouped entries
    for (const auto& compositeKey : groupOrder) {
        const auto& gi = groups[compositeKey];
        auto* row = buildGroupRow(gi.type, gi.key, gi.title, gi.thumb,
                                   gi.total, gi.completed, gi.downloading,
                                   gi.contentTotal);
        m_listContainer->addView(row, m_listContainer->getChildren().size() - 1);
    }

    // Add ungrouped items
    for (const auto& item : ungrouped) {
        auto* row = buildItemRow(item);
        m_listContainer->addView(row, m_listContainer->getChildren().size() - 1);
    }

    // Set up focus navigation between action buttons and list items
    auto& children = m_listContainer->getChildren();
    brls::View* firstListItem = nullptr;
    for (auto* child : children) {
        if (child != m_emptyLabel && child->isFocusable()) {
            firstListItem = child;
            break;
        }
    }

    if (firstListItem) {
        // DOWN from each action button -> first list item
        if (m_startStopBtn) m_startStopBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);
        if (m_resumeBtn) m_resumeBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);
        if (m_syncBtn) m_syncBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);
        if (m_clearBtn) m_clearBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstListItem);

        // UP from each list item -> first action button (Start/Stop)
        for (auto* child : children) {
            if (child != m_emptyLabel && child->isFocusable()) {
                child->setCustomNavigationRoute(brls::FocusDirection::UP, m_startStopBtn);
            }
        }
    }
}

void DownloadsTab::updateProgressInPlace(const std::vector<DownloadItem>& downloads) {
    // Update individual item status labels
    for (const auto& item : downloads) {
        auto it = m_itemStatusLabels.find(item.ratingKey);
        if (it != m_itemStatusLabels.end()) {
            it->second->setText(buildItemStatusText(item));
        }
    }

    // Update group status labels
    struct GroupProgress {
        std::string typePrefix;
        int total = 0;
        int contentTotal = 0;  // Stable total from groupTotalItems
        int completed = 0;
        int downloading = 0;
    };
    std::map<std::string, GroupProgress> groupStats;

    for (const auto& item : downloads) {
        if (item.groupType != DownloadGroupType::NONE && !item.groupKey.empty()) {
            std::string compositeKey = std::to_string(static_cast<int>(item.groupType)) + ":" + item.groupKey;
            auto& gp = groupStats[compositeKey];
            if (gp.total == 0) {
                switch (item.groupType) {
                    case DownloadGroupType::PLAYLIST: gp.typePrefix = "Playlist"; break;
                    case DownloadGroupType::ALBUM:    gp.typePrefix = "Album"; break;
                    case DownloadGroupType::ARTIST:   gp.typePrefix = "Artist"; break;
                    case DownloadGroupType::SHOW:     gp.typePrefix = "Show"; break;
                    default: break;
                }
            }
            gp.total++;
            if (item.groupTotalItems > gp.contentTotal) gp.contentTotal = item.groupTotalItems;
            if (item.state == DownloadState::COMPLETED) gp.completed++;
            if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) gp.downloading++;
        }
    }

    for (const auto& pair : groupStats) {
        auto it = m_groupStatusLabels.find(pair.first);
        if (it != m_groupStatusLabels.end()) {
            const auto& gp = pair.second;
            int displayTotal = (gp.contentTotal > 0) ? gp.contentTotal : gp.total;
            std::string statusText = gp.typePrefix + " - " + std::to_string(gp.completed) + "/" +
                                     std::to_string(displayTotal) + " ready";
            if (gp.downloading > 0) {
                statusText += " (" + std::to_string(gp.downloading) + " downloading)";
            }
            it->second->setText(statusText);
        }
    }
}

brls::Box* DownloadsTab::buildGroupRow(DownloadGroupType groupType, const std::string& groupKey,
                                        const std::string& groupTitle, const std::string& groupThumb,
                                        int totalItems, int completedItems, int downloadingItems,
                                        int contentTotal) {
    // Use contentTotal (stable) for Y if available, otherwise fall back to current count
    int displayTotal = (contentTotal > 0) ? contentTotal : totalItems;
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(8);
    row->setMargins(0, 0, 8, 0);
    row->setCornerRadius(8);
    row->setFocusable(true);

    // Background color based on completion status
    if (completedItems == displayTotal) {
        row->setBackgroundColor(nvgRGBA(20, 40, 60, 200));  // Blue - all done
    } else if (downloadingItems > 0) {
        row->setBackgroundColor(nvgRGBA(20, 60, 20, 200));  // Green - downloading
    } else {
        row->setBackgroundColor(nvgRGBA(40, 40, 40, 200));  // Gray - queued
    }

    // Cover art thumbnail
    auto* thumbImage = new brls::Image();
    bool isMusic = (groupType == DownloadGroupType::PLAYLIST ||
                    groupType == DownloadGroupType::ALBUM ||
                    groupType == DownloadGroupType::ARTIST);
    int thumbW = isMusic ? 60 : 45;
    int thumbH = isMusic ? 60 : 67;
    thumbImage->setSize(brls::Size(thumbW, thumbH));
    thumbImage->setScalingType(brls::ImageScalingType::FIT);
    thumbImage->setMargins(0, 10, 0, 0);
    thumbImage->setCornerRadius(4);

    // Try to load cover from first completed item in this group, or use async URL
    // Hide thumbnail initially to prevent null texture rendering crash on Vita
    thumbImage->setVisibility(brls::Visibility::GONE);
    if (!groupThumb.empty()) {
        // Check if any item in the group has a downloaded cover
        auto groupItems = DownloadsManager::getInstance().getDownloadsByGroup(groupType, groupKey);
        bool loaded = false;
        for (const auto& gi : groupItems) {
            if (!gi.thumbPath.empty() && gi.state == DownloadState::COMPLETED) {
                if (ImageLoader::loadFromFile(gi.thumbPath, thumbImage)) {
                    thumbImage->setVisibility(brls::Visibility::VISIBLE);
                    loaded = true;
                    break;
                }
            }
        }
        if (!loaded) {
            // Load from server URL - show only when texture loads successfully
            std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(groupThumb, thumbW * 2, thumbH * 2);
            if (!thumbUrl.empty()) {
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                    img->setVisibility(brls::Visibility::VISIBLE);
                }, thumbImage, m_aliveAtomic);
            }
        }
    }
    row->addView(thumbImage);

    // Info column
    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setGrow(1.0f);

    // Group type label
    std::string typePrefix;
    switch (groupType) {
        case DownloadGroupType::PLAYLIST: typePrefix = "Playlist"; break;
        case DownloadGroupType::ALBUM:    typePrefix = "Album"; break;
        case DownloadGroupType::ARTIST:   typePrefix = "Artist"; break;
        case DownloadGroupType::SHOW:     typePrefix = "Show"; break;
        default: break;
    }

    auto* titleLabel = new brls::Label();
    titleLabel->setText(groupTitle);
    titleLabel->setFontSize(18);
    infoBox->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setFontSize(14);
    statusLabel->setTextColor(nvgRGBA(200, 200, 200, 255));
    std::string statusText = typePrefix + " - " + std::to_string(completedItems) + "/" +
                             std::to_string(displayTotal) + " ready";
    if (downloadingItems > 0) {
        statusText += " (" + std::to_string(downloadingItems) + " downloading)";
    }
    statusLabel->setText(statusText);
    infoBox->addView(statusLabel);

    // Track for in-place progress updates
    std::string compositeKey = std::to_string(static_cast<int>(groupType)) + ":" + groupKey;
    m_groupStatusLabels[compositeKey] = statusLabel;

    row->addView(infoBox);

    // Click to view tracks in this group
    DownloadGroupType capturedType = groupType;
    std::string capturedKey = groupKey;
    std::string capturedTitle = groupTitle;
    row->registerClickAction([this, capturedType, capturedKey, capturedTitle](brls::View*) {
        showGroupDetail(capturedType, capturedKey, capturedTitle);
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // START button context menu
    row->registerAction("Options", brls::ControllerButton::BUTTON_START,
        [this, capturedType, capturedKey, capturedTitle](brls::View*) {
            showGroupContextMenu(capturedType, capturedKey, capturedTitle);
            return true;
        });

    return row;
}

brls::Box* DownloadsTab::buildItemRow(const DownloadItem& item) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(8);
    row->setMargins(0, 0, 8, 0);
    row->setCornerRadius(8);
    row->setFocusable(true);
    row->setBackgroundColor(getStateColor(item.state));

    // Cover art / poster thumbnail
    auto* thumbImage = new brls::Image();
    bool isMusic = (item.mediaType == "track");
    bool isMovie = (item.mediaType == "movie");
    int thumbW = isMusic ? 60 : (isMovie ? 45 : 50);
    int thumbH = isMusic ? 60 : (isMovie ? 67 : 38);
    thumbImage->setSize(brls::Size(thumbW, thumbH));
    thumbImage->setScalingType(brls::ImageScalingType::FIT);
    thumbImage->setMargins(0, 10, 0, 0);
    thumbImage->setCornerRadius(4);

    // Load thumbnail - hide initially to prevent null texture rendering crash on Vita
    thumbImage->setVisibility(brls::Visibility::GONE);
    if (!item.thumbPath.empty() && item.state == DownloadState::COMPLETED) {
        if (ImageLoader::loadFromFile(item.thumbPath, thumbImage)) {
            thumbImage->setVisibility(brls::Visibility::VISIBLE);
        }
    } else if (!item.thumbUrl.empty()) {
        std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(item.thumbUrl, thumbW * 2, thumbH * 2);
        if (!thumbUrl.empty()) {
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, thumbImage, m_aliveAtomic);
        }
    }
    row->addView(thumbImage);

    // Info column
    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setGrow(1.0f);

    auto* titleLabel = new brls::Label();
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

    auto* statusLabel = new brls::Label();
    statusLabel->setFontSize(14);
    statusLabel->setText(buildItemStatusText(item));
    statusLabel->setTextColor(nvgRGBA(200, 200, 200, 255));
    infoBox->addView(statusLabel);

    // Track for in-place progress updates
    m_itemStatusLabels[item.ratingKey] = statusLabel;
    m_itemRows[item.ratingKey] = row;

    row->addView(infoBox);

    // Action buttons
    auto* buttonsBox = new brls::Box();
    buttonsBox->setAxis(brls::Axis::ROW);

    if (item.state == DownloadState::COMPLETED) {
        // START button for context menu on completed items
        DownloadItem capturedItem = item;
        row->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [this, capturedItem](brls::View*) {
                showItemContextMenu(capturedItem);
                return true;
            });

        auto* playBtn = new brls::Button();
        auto* playLabel = new brls::Label();
        playLabel->setText("Play");
        playLabel->setFontSize(14);
        playBtn->addView(playLabel);
        playBtn->setMargins(0, 0, 0, 5);

        std::string ratingKey = item.ratingKey;
        bool isTrack = (item.mediaType == "track");
        DownloadItem capturedForPlay = item;
        playBtn->registerClickAction([ratingKey, isTrack, capturedForPlay](brls::View*) {
            if (isTrack) {
                MediaItem mi;
                mi.ratingKey = capturedForPlay.ratingKey;
                mi.title = capturedForPlay.title;
                mi.grandparentTitle = capturedForPlay.parentTitle;
                mi.parentTitle = capturedForPlay.parentTitle;
                mi.duration = capturedForPlay.duration;
                mi.thumb = capturedForPlay.thumbUrl;

                std::vector<MediaItem> single = {mi};
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(single, 0));
            } else {
                brls::Application::pushActivity(new PlayerActivity(ratingKey, true));
            }
            return true;
        });
        buttonsBox->addView(playBtn);

        auto* deleteBtn = new brls::Button();
        auto* delLabel = new brls::Label();
        delLabel->setText("Delete");
        delLabel->setFontSize(14);
        deleteBtn->addView(delLabel);

        std::string key = item.ratingKey;
        deleteBtn->registerClickAction([this, key](brls::View*) {
            DownloadsManager::getInstance().deleteDownload(key);
            brls::Application::notify("Download deleted");
            m_lastState.clear();
            return true;
        });
        buttonsBox->addView(deleteBtn);
    } else if (item.state == DownloadState::DOWNLOADING ||
               item.state == DownloadState::TRANSCODING ||
               item.state == DownloadState::QUEUED) {
        auto* cancelBtn = new brls::Button();
        auto* cancelLabel = new brls::Label();
        cancelLabel->setText("Cancel");
        cancelLabel->setFontSize(14);
        cancelBtn->addView(cancelLabel);

        std::string key = item.ratingKey;
        cancelBtn->registerClickAction([this, key](brls::View*) {
            DownloadsManager::getInstance().cancelDownload(key);
            brls::Application::notify("Download cancelled");
            m_lastState.clear();
            return true;
        });
        buttonsBox->addView(cancelBtn);
    } else if (item.state == DownloadState::PAUSED ||
               item.state == DownloadState::FAILED) {
        auto* cancelBtn = new brls::Button();
        auto* cancelLabel = new brls::Label();
        cancelLabel->setText("Remove");
        cancelLabel->setFontSize(14);
        cancelBtn->addView(cancelLabel);

        std::string key = item.ratingKey;
        cancelBtn->registerClickAction([this, key](brls::View*) {
            DownloadsManager::getInstance().cancelDownload(key);
            brls::Application::notify("Download removed");
            m_lastState.clear();
            return true;
        });
        buttonsBox->addView(cancelBtn);
    }

    row->addView(buttonsBox);
    return row;
}

void DownloadsTab::showGroupDetail(DownloadGroupType groupType, const std::string& groupKey,
                                    const std::string& groupTitle) {
    auto items = DownloadsManager::getInstance().getDownloadsByGroup(groupType, groupKey);
    if (items.empty()) return;

    // Alive flag for async image loads - invalidated when activity is destroyed
    auto viewAlive = std::make_shared<std::atomic<bool>>(true);

    // Build a full-screen view mirroring the online MediaDetailView layout
    auto* mainBox = new brls::Box();
    mainBox->setAxis(brls::Axis::COLUMN);
    mainBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    mainBox->setAlignItems(brls::AlignItems::STRETCH);
    mainBox->setGrow(1.0f);

    // Register back button (B/Circle) to pop this activity
    mainBox->registerAction("Back", brls::ControllerButton::BUTTON_B, [viewAlive](brls::View* view) {
        viewAlive->store(false);
        brls::Application::popActivity();
        return true;
    }, false, false, brls::Sound::SOUND_BACK);

    // Top section: cover art + info (fixed, non-scrolling)
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setJustifyContent(brls::JustifyContent::FLEX_START);
    topRow->setAlignItems(brls::AlignItems::FLEX_START);
    topRow->setPadding(30, 30, 15, 30);

    // Cover art
    bool isMusic = (groupType == DownloadGroupType::PLAYLIST ||
                    groupType == DownloadGroupType::ALBUM ||
                    groupType == DownloadGroupType::ARTIST);
    int artW = isMusic ? 150 : 120;
    int artH = isMusic ? 150 : 180;

    auto* coverImage = new brls::Image();
    coverImage->setSize(brls::Size(artW, artH));
    coverImage->setScalingType(brls::ImageScalingType::FIT);
    coverImage->setMarginRight(20);
    coverImage->setCornerRadius(8);
    coverImage->setVisibility(brls::Visibility::GONE);

    // Try local cover first, then server URL
    bool coverLoaded = false;
    for (const auto& item : items) {
        if (!item.thumbPath.empty() && item.state == DownloadState::COMPLETED) {
            if (ImageLoader::loadFromFile(item.thumbPath, coverImage)) {
                coverImage->setVisibility(brls::Visibility::VISIBLE);
                coverLoaded = true;
                break;
            }
        }
    }
    if (!coverLoaded && !items.empty() && !items[0].groupThumb.empty()) {
        std::string thumbUrl = PlexClient::getInstance().getThumbnailUrl(items[0].groupThumb, artW * 2, artH * 2);
        if (!thumbUrl.empty()) {
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                img->setVisibility(brls::Visibility::VISIBLE);
            }, coverImage, viewAlive);
        }
    }
    topRow->addView(coverImage);

    // Info column
    auto* infoBox = new brls::Box();
    infoBox->setAxis(brls::Axis::COLUMN);
    infoBox->setGrow(1.0f);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(groupTitle);
    titleLabel->setFontSize(26);
    titleLabel->setMarginBottom(8);
    infoBox->addView(titleLabel);

    // Type label
    std::string typeStr;
    switch (groupType) {
        case DownloadGroupType::PLAYLIST: typeStr = "Playlist"; break;
        case DownloadGroupType::ALBUM:    typeStr = "Album"; break;
        case DownloadGroupType::ARTIST:   typeStr = "Artist"; break;
        case DownloadGroupType::SHOW:     typeStr = "TV Show"; break;
        default: break;
    }

    int completed = 0, total = (int)items.size();
    int contentTotal = 0;
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED) completed++;
        if (item.groupTotalItems > contentTotal) contentTotal = item.groupTotalItems;
    }
    int stableTotal = (contentTotal > 0) ? contentTotal : total;

    auto* typeLabel = new brls::Label();
    typeLabel->setFontSize(16);
    typeLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    std::string itemWord = isMusic ? "tracks" : "items";
    typeLabel->setText(typeStr + " - " + std::to_string(completed) + "/" +
                       std::to_string(stableTotal) + " " + itemWord + " ready");
    typeLabel->setMarginBottom(15);
    infoBox->addView(typeLabel);

    // Action buttons
    auto addActionBtn = [&infoBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setWidth(180);
        btn->setHeight(40);
        btn->setMarginBottom(8);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        infoBox->addView(btn);
    };

    // Build completed MediaItem list for queue operations
    auto completedItems = std::make_shared<std::vector<MediaItem>>();
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED) {
            MediaItem mi;
            mi.ratingKey = item.ratingKey;
            mi.title = item.title;
            mi.grandparentTitle = item.parentTitle;
            mi.parentTitle = item.parentTitle;
            mi.duration = item.duration;
            mi.thumb = item.thumbUrl;
            completedItems->push_back(mi);
        }
    }

    if (isMusic) {
        addActionBtn("Play All", [completedItems](brls::View*) {
            if (!completedItems->empty()) {
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(*completedItems, 0));
            } else {
                brls::Application::notify("No completed tracks to play");
            }
            return true;
        });

        addActionBtn("Add to Queue", [completedItems](brls::View*) {
            if (completedItems->empty()) {
                brls::Application::notify("No completed tracks to add");
                return true;
            }
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(*completedItems, 0));
            } else {
                queue.addTracks(*completedItems);
                brls::Application::notify("Added " + std::to_string(completedItems->size()) + " tracks to queue");
            }
            return true;
        });
    } else {
        // TV Show / Movie - just "Play All" which plays first item
        addActionBtn("Play", [completedItems](brls::View*) {
            if (!completedItems->empty()) {
                brls::Application::pushActivity(
                    new PlayerActivity(completedItems->front().ratingKey, true));
            } else {
                brls::Application::notify("No completed items to play");
            }
            return true;
        });
    }

    topRow->addView(infoBox);
    mainBox->addView(topRow);

    // Track / Episode list header
    auto* listHeader = new brls::Label();
    if (groupType == DownloadGroupType::SHOW) {
        listHeader->setText("Episodes");
    } else {
        listHeader->setText("Tracks");
    }
    listHeader->setFontSize(20);
    listHeader->setMargins(0, 30, 10, 30);
    mainBox->addView(listHeader);

    // Scrollable track / episode list
    auto* trackScroll = new brls::ScrollingFrame();
    trackScroll->setGrow(1.0f);

    auto* trackListBox = new brls::Box();
    trackListBox->setAxis(brls::Axis::COLUMN);
    trackListBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    trackListBox->setAlignItems(brls::AlignItems::STRETCH);
    trackListBox->setPadding(0, 30, 20, 30);

    for (size_t i = 0; i < items.size(); i++) {
        const auto& item = items[i];

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(56);
        row->setPadding(10, 16, 10, 16);
        row->setMarginBottom(4);
        row->setCornerRadius(8);
        row->setFocusable(true);

        // Color-code by download state
        if (item.state == DownloadState::COMPLETED) {
            row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
        } else {
            row->setBackgroundColor(getStateColor(item.state));
        }

        // Left side: number + title
        auto* leftBox = new brls::Box();
        leftBox->setAxis(brls::Axis::ROW);
        leftBox->setAlignItems(brls::AlignItems::CENTER);
        leftBox->setGrow(1.0f);

        auto* numLabel = new brls::Label();
        numLabel->setFontSize(14);
        numLabel->setMarginRight(12);
        numLabel->setTextColor(nvgRGBA(150, 150, 150, 255));

        if (groupType == DownloadGroupType::SHOW && item.seasonNum > 0 && item.episodeNum > 0) {
            char epStr[16];
            snprintf(epStr, sizeof(epStr), "S%02dE%02d", item.seasonNum, item.episodeNum);
            numLabel->setText(epStr);
        } else {
            numLabel->setText(std::to_string(i + 1));
        }
        leftBox->addView(numLabel);

        auto* titleLabel2 = new brls::Label();
        titleLabel2->setFontSize(14);
        std::string displayTitle = item.title;
        if (!item.albumTitle.empty() && groupType == DownloadGroupType::ARTIST) {
            displayTitle += " (" + item.albumTitle + ")";
        }
        // Truncate for Vita screen
        if (displayTitle.length() > 50) {
            displayTitle = displayTitle.substr(0, 47) + "...";
        }
        titleLabel2->setText(displayTitle);
        leftBox->addView(titleLabel2);

        row->addView(leftBox);

        // Right side: status + duration
        auto* rightSide = new brls::Box();
        rightSide->setAxis(brls::Axis::ROW);
        rightSide->setAlignItems(brls::AlignItems::CENTER);

        if (item.state != DownloadState::COMPLETED) {
            auto* statusLabel = new brls::Label();
            statusLabel->setFontSize(11);
            statusLabel->setTextColor(nvgRGBA(200, 180, 100, 255));
            statusLabel->setText(buildItemStatusText(item));
            statusLabel->setMarginRight(10);
            rightSide->addView(statusLabel);
        }

        if (item.duration > 0) {
            auto* durLabel = new brls::Label();
            durLabel->setFontSize(12);
            durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
            int totalSec = (int)(item.duration / 1000);
            int min = totalSec / 60;
            int sec = totalSec % 60;
            char durStr[16];
            snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
            durLabel->setText(durStr);
            rightSide->addView(durLabel);
        }

        row->addView(rightSide);

        // Click action - play from this track/episode
        if (item.state == DownloadState::COMPLETED) {
            DownloadItem capturedItem = item;
            bool capturedIsMusic = isMusic;
            auto capturedCompleted = completedItems;
            size_t capturedIdx = i;

            row->registerClickAction([capturedItem, capturedIsMusic, capturedCompleted, capturedIdx](brls::View*) {
                if (capturedIsMusic) {
                    // Use track default action setting
                    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;
                    MusicQueue& queue = MusicQueue::getInstance();

                    // Find index in completed list
                    size_t completedIdx = 0;
                    for (size_t j = 0; j < capturedCompleted->size(); j++) {
                        if ((*capturedCompleted)[j].ratingKey == capturedItem.ratingKey) {
                            completedIdx = j;
                            break;
                        }
                    }

                    switch (action) {
                        case TrackDefaultAction::PLAY_NOW_CLEAR:
                        default:
                            brls::Application::pushActivity(
                                PlayerActivity::createWithQueue(*capturedCompleted, completedIdx));
                            break;
                        case TrackDefaultAction::PLAY_NEXT:
                            if (queue.isEmpty()) {
                                brls::Application::pushActivity(
                                    PlayerActivity::createWithQueue(*capturedCompleted, completedIdx));
                            } else {
                                MediaItem mi;
                                mi.ratingKey = capturedItem.ratingKey;
                                mi.title = capturedItem.title;
                                mi.grandparentTitle = capturedItem.parentTitle;
                                mi.parentTitle = capturedItem.parentTitle;
                                mi.duration = capturedItem.duration;
                                mi.thumb = capturedItem.thumbUrl;
                                queue.insertTrackAfterCurrent(mi);
                                brls::Application::notify("Playing next: " + mi.title);
                            }
                            break;
                        case TrackDefaultAction::ADD_TO_BOTTOM:
                            if (queue.isEmpty()) {
                                brls::Application::pushActivity(
                                    PlayerActivity::createWithQueue(*capturedCompleted, completedIdx));
                            } else {
                                MediaItem mi;
                                mi.ratingKey = capturedItem.ratingKey;
                                mi.title = capturedItem.title;
                                mi.grandparentTitle = capturedItem.parentTitle;
                                mi.parentTitle = capturedItem.parentTitle;
                                mi.duration = capturedItem.duration;
                                mi.thumb = capturedItem.thumbUrl;
                                queue.addTrack(mi);
                                brls::Application::notify("Added to queue: " + mi.title);
                            }
                            break;
                        case TrackDefaultAction::PLAY_NOW_REPLACE:
                            if (queue.isEmpty()) {
                                brls::Application::pushActivity(
                                    PlayerActivity::createWithQueue(*capturedCompleted, completedIdx));
                            } else {
                                MediaItem mi;
                                mi.ratingKey = capturedItem.ratingKey;
                                mi.title = capturedItem.title;
                                mi.grandparentTitle = capturedItem.parentTitle;
                                mi.parentTitle = capturedItem.parentTitle;
                                mi.duration = capturedItem.duration;
                                mi.thumb = capturedItem.thumbUrl;
                                queue.insertTrackAfterCurrent(mi);
                                if (queue.playNext()) {
                                    brls::Application::notify("Now playing: " + mi.title);
                                }
                            }
                            break;
                        case TrackDefaultAction::ASK_EACH_TIME: {
                            // Show action dialog
                            auto* dlg = new brls::Dialog("Choose Action");
                            auto* opts = new brls::Box();
                            opts->setAxis(brls::Axis::COLUMN);
                            opts->setPadding(20);

                            auto addBtn = [&opts](const std::string& text, std::function<bool(brls::View*)> act) {
                                auto* btn = new brls::Button();
                                btn->setText(text);
                                btn->setHeight(44);
                                btn->setMarginBottom(10);
                                btn->registerClickAction(act);
                                btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
                                opts->addView(btn);
                            };

                            addBtn("Play from Here", [dlg, capturedCompleted, completedIdx](brls::View*) {
                                dlg->dismiss();
                                brls::Application::pushActivity(
                                    PlayerActivity::createWithQueue(*capturedCompleted, completedIdx));
                                return true;
                            });
                            addBtn("Play Next", [dlg, capturedItem](brls::View*) {
                                dlg->dismiss();
                                MusicQueue& q = MusicQueue::getInstance();
                                MediaItem mi;
                                mi.ratingKey = capturedItem.ratingKey;
                                mi.title = capturedItem.title;
                                mi.grandparentTitle = capturedItem.parentTitle;
                                mi.parentTitle = capturedItem.parentTitle;
                                mi.duration = capturedItem.duration;
                                mi.thumb = capturedItem.thumbUrl;
                                if (q.isEmpty()) {
                                    std::vector<MediaItem> single = {mi};
                                    brls::Application::pushActivity(
                                        PlayerActivity::createWithQueue(single, 0));
                                } else {
                                    q.insertTrackAfterCurrent(mi);
                                    brls::Application::notify("Playing next: " + mi.title);
                                }
                                return true;
                            });
                            addBtn("Add to Queue", [dlg, capturedItem](brls::View*) {
                                dlg->dismiss();
                                MusicQueue& q = MusicQueue::getInstance();
                                MediaItem mi;
                                mi.ratingKey = capturedItem.ratingKey;
                                mi.title = capturedItem.title;
                                mi.grandparentTitle = capturedItem.parentTitle;
                                mi.parentTitle = capturedItem.parentTitle;
                                mi.duration = capturedItem.duration;
                                mi.thumb = capturedItem.thumbUrl;
                                if (q.isEmpty()) {
                                    std::vector<MediaItem> single = {mi};
                                    brls::Application::pushActivity(
                                        PlayerActivity::createWithQueue(single, 0));
                                } else {
                                    q.addTrack(mi);
                                    brls::Application::notify("Added to queue: " + mi.title);
                                }
                                return true;
                            });
                            addBtn("Cancel", [dlg](brls::View*) {
                                dlg->dismiss();
                                return true;
                            });

                            dlg->addView(opts);
                            dlg->open();
                            break;
                        }
                    }
                } else {
                    // Video content - play locally
                    brls::Application::pushActivity(
                        new PlayerActivity(capturedItem.ratingKey, true));
                }
                return true;
            });
            row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
        }

        trackListBox->addView(row);
    }

    trackScroll->setContentView(trackListBox);
    mainBox->addView(trackScroll);

    brls::Application::pushActivity(new brls::Activity(mainBox));
}

void DownloadsTab::showGroupContextMenu(DownloadGroupType groupType, const std::string& groupKey,
                                         const std::string& groupTitle) {
    auto items = DownloadsManager::getInstance().getDownloadsByGroup(groupType, groupKey);
    if (items.empty()) return;

    auto* dialog = new brls::Dialog(groupTitle);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    // Track count info
    int completed = 0;
    int contentTotal = 0;
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED) completed++;
        if (item.groupTotalItems > contentTotal) contentTotal = item.groupTotalItems;
    }
    int stableTotal = (contentTotal > 0) ? contentTotal : (int)items.size();
    auto* infoLabel = new brls::Label();
    infoLabel->setText(std::to_string(completed) + "/" + std::to_string(stableTotal) + " tracks ready");
    infoLabel->setFontSize(14);
    infoLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    infoLabel->setMarginBottom(10);
    optionsBox->addView(infoLabel);

    auto addBtn = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    // Build a list of completed tracks as MediaItems
    std::vector<MediaItem> completedTracks;
    for (const auto& item : items) {
        if (item.state == DownloadState::COMPLETED && item.mediaType == "track") {
            MediaItem mi;
            mi.ratingKey = item.ratingKey;
            mi.title = item.title;
            mi.grandparentTitle = item.parentTitle;
            mi.parentTitle = item.parentTitle;
            mi.duration = item.duration;
            mi.thumb = item.thumbUrl;
            completedTracks.push_back(mi);
        }
    }

    // Play Now (Clear Queue)
    addBtn("Play Now (Clear Queue)", [dialog, completedTracks](brls::View*) {
        dialog->dismiss();
        if (!completedTracks.empty()) {
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(completedTracks, 0));
        } else {
            brls::Application::notify("No completed tracks to play");
        }
        return true;
    });

    // Add to Queue
    addBtn("Add to Queue", [dialog, completedTracks](brls::View*) {
        dialog->dismiss();
        if (completedTracks.empty()) {
            brls::Application::notify("No completed tracks to add");
            return true;
        }
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            brls::Application::pushActivity(
                PlayerActivity::createWithQueue(completedTracks, 0));
        } else {
            queue.addTracks(completedTracks);
            brls::Application::notify("Added " + std::to_string(completedTracks.size()) + " tracks to queue");
        }
        return true;
    });

    // Delete All
    DownloadGroupType capturedType = groupType;
    std::string capturedKey = groupKey;
    addBtn("Delete All", [this, dialog, capturedType, capturedKey](brls::View*) {
        dialog->dismiss();
        auto groupItems = DownloadsManager::getInstance().getDownloadsByGroup(capturedType, capturedKey);
        for (const auto& item : groupItems) {
            DownloadsManager::getInstance().deleteDownload(item.ratingKey);
        }
        m_lastState.clear();
        brls::Application::notify("Deleted all items in group");
        return true;
    });

    addBtn("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    dialog->open();
}

void DownloadsTab::showItemContextMenu(const DownloadItem& item) {
    auto* dialog = new brls::Dialog(item.title);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto addBtn = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    DownloadItem capturedItem = item;

    if (item.mediaType == "track") {
        MediaItem mi;
        mi.ratingKey = item.ratingKey;
        mi.title = item.title;
        mi.grandparentTitle = item.parentTitle;
        mi.parentTitle = item.parentTitle;
        mi.duration = item.duration;
        mi.thumb = item.thumbUrl;

        addBtn("Play Now (Clear Queue)", [dialog, mi](brls::View*) {
            dialog->dismiss();
            std::vector<MediaItem> single = {mi};
            brls::Application::pushActivity(PlayerActivity::createWithQueue(single, 0));
            return true;
        });

        addBtn("Add to Queue", [dialog, mi](brls::View*) {
            dialog->dismiss();
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                std::vector<MediaItem> single = {mi};
                brls::Application::pushActivity(PlayerActivity::createWithQueue(single, 0));
            } else {
                queue.addTrack(mi);
                brls::Application::notify("Added to queue: " + mi.title);
            }
            return true;
        });
    } else {
        // Video content
        std::string ratingKey = item.ratingKey;
        addBtn("Play", [dialog, ratingKey](brls::View*) {
            dialog->dismiss();
            brls::Application::pushActivity(new PlayerActivity(ratingKey, true));
            return true;
        });
    }

    std::string key = item.ratingKey;
    addBtn("Delete", [this, dialog, key](brls::View*) {
        dialog->dismiss();
        DownloadsManager::getInstance().deleteDownload(key);
        m_lastState.clear();
        brls::Application::notify("Download deleted");
        return true;
    });

    addBtn("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    dialog->open();
}

} // namespace vitaplex
