/**
 * VitaPlex - Downloads Tab
 * View for managing offline downloads with queue display,
 * start/stop/pause controls, and auto-refresh progress.
 * Groups downloads by playlist/album/artist with cover art.
 *
 * Based on Vita_Suwayomi's downloads tab patterns.
 */

#pragma once

#include <borealis.hpp>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <map>
#include "app/downloads_manager.hpp"

namespace vitaplex {

class DownloadsTab : public brls::Box {
public:
    DownloadsTab();
    ~DownloadsTab() override;

    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;

private:
    void refresh();
    void rebuildList();
    void updateProgressInPlace(const std::vector<DownloadItem>& downloads);
    void startAutoRefresh();
    void stopAutoRefresh();

    // Show the track list for a group
    void showGroupDetail(DownloadGroupType groupType, const std::string& groupKey,
                         const std::string& groupTitle);

    // Show context menu for a group (START button)
    void showGroupContextMenu(DownloadGroupType groupType, const std::string& groupKey,
                              const std::string& groupTitle);

    // Show context menu for a single completed item
    void showItemContextMenu(const DownloadItem& item);

    // Build a row for a grouped entry (playlist/album/artist)
    brls::Box* buildGroupRow(DownloadGroupType groupType, const std::string& groupKey,
                             const std::string& groupTitle, const std::string& groupThumb,
                             int totalItems, int completedItems, int downloadingItems);

    // Build a row for an individual (ungrouped) download item
    brls::Box* buildItemRow(const DownloadItem& item);

    // Action buttons
    brls::Box* m_actionsRow = nullptr;
    brls::Button* m_startStopBtn = nullptr;
    brls::Label* m_startStopLabel = nullptr;
    brls::Button* m_pauseBtn = nullptr;
    brls::Button* m_resumeBtn = nullptr;
    brls::Button* m_clearBtn = nullptr;

    // Download status
    brls::Label* m_statusLabel = nullptr;

    // List container
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_listContainer = nullptr;
    brls::Label* m_emptyLabel = nullptr;

    // State tracking for smart refresh
    struct CachedItem {
        std::string ratingKey;
        int64_t downloadedBytes = 0;
        int64_t totalBytes = 0;
        int state = 0;
        int64_t viewOffset = 0;
        int transcodeElapsedSeconds = 0;
    };
    std::vector<CachedItem> m_lastState;

    // In-place update: maps for updating text without full rebuild
    std::map<std::string, brls::Label*> m_itemStatusLabels;   // ratingKey -> status label
    std::map<std::string, brls::Box*> m_itemRows;             // ratingKey -> row box
    std::map<std::string, brls::Label*> m_groupStatusLabels;  // compositeKey -> status label

    // Auto-refresh
    std::atomic<bool> m_autoRefreshEnabled{false};
    std::chrono::steady_clock::time_point m_lastRefresh;
    std::chrono::steady_clock::time_point m_lastClearTime;
    static constexpr int REFRESH_INTERVAL_MS = 1000;

    // Alive flag for async safety
    std::shared_ptr<bool> m_alive;
    std::shared_ptr<std::atomic<bool>> m_aliveAtomic;
};

} // namespace vitaplex
