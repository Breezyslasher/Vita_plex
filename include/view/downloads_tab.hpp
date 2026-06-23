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

    // Media-type sub-tab filter (D8). ALL must stay 0 so the per-type
    // count array can index MOVIES/SHOWS/MUSIC directly.
    enum class Type { ALL = 0, MOVIES = 1, SHOWS = 2, MUSIC = 3 };

    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;

private:
    void refresh();
    void rebuildList();
    void updateProgressInPlace(const std::vector<DownloadItem>& downloads);
    void startAutoRefresh();
    void stopAutoRefresh();

    // D8 sub-tab bar + type filtering (layout/filter only — no change to
    // the download engine, item model, or per-row actions).
    brls::Box* buildTypeTabs();
    void setTypeFilter(Type type);
    void applyTabVisuals();
    void updateTypeCounts(const std::vector<DownloadItem>& all);
    void updateStorageReadout(const std::vector<DownloadItem>& all);
    void applyResponsiveLayout();

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
                             int totalItems, int completedItems, int downloadingItems,
                             int contentTotal = 0);

    // Build a row for an individual (ungrouped) download item
    brls::Box* buildItemRow(const DownloadItem& item);

    // Action buttons
    brls::Box* m_actionsRow = nullptr;
    brls::Button* m_startStopBtn = nullptr;
    brls::Label* m_startStopLabel = nullptr;
    brls::Button* m_pauseBtn = nullptr;
    brls::Button* m_resumeBtn = nullptr;
    brls::Button* m_syncBtn = nullptr;
    brls::Button* m_clearBtn = nullptr;

    // Header: page title + offline-storage readout (D8)
    brls::Box*   m_headerRow = nullptr;
    brls::Label* m_titleLabel = nullptr;
    brls::Box*   m_storageBox = nullptr;
    brls::Label* m_storageUsedLabel = nullptr;
    brls::Label* m_storageTotalLabel = nullptr;
    brls::Box*   m_storageMeterFill = nullptr;

    // Media-type sub-tab bar (D8)
    Type m_activeType = Type::ALL;
    struct TypeTab {
        brls::Box*   tab = nullptr;
        brls::Image* icon = nullptr;
        brls::Label* label = nullptr;
        brls::Box*   badge = nullptr;
        brls::Label* count = nullptr;
        brls::Box*   underline = nullptr;
    };
    TypeTab m_typeTabs[4];
    brls::Box* m_tabBar = nullptr;

    // List container
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_listContainer = nullptr;
    brls::Box* m_emptyView = nullptr;     // grow-filling placeholder shown when the list is empty
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
    std::map<std::string, brls::Box*> m_itemRows;             // ratingKey -> row box (for color updates)
    std::map<std::string, brls::Label*> m_groupStatusLabels;  // compositeKey -> status label
    std::map<std::string, brls::Box*> m_groupRows;            // compositeKey -> row box (for color updates)
    std::map<std::string, brls::Box*> m_itemStrips;           // ratingKey -> left state-accent strip
    std::map<std::string, brls::Box*> m_groupStrips;          // compositeKey -> left state-accent strip

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
