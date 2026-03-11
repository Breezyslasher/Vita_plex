/**
 * VitaPlex - Downloads Tab
 * View for managing offline downloads with queue display,
 * start/stop/pause controls, and auto-refresh progress.
 *
 * Based on Vita_Suwayomi's downloads tab patterns.
 */

#pragma once

#include <borealis.hpp>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>

namespace vitaplex {

class DownloadsTab : public brls::Box {
public:
    DownloadsTab();
    ~DownloadsTab() override;

    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;

private:
    void refresh();
    void startAutoRefresh();
    void stopAutoRefresh();

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

    // Cached state for smart refresh
    struct CachedItem {
        std::string ratingKey;
        int64_t downloadedBytes = 0;
        int64_t totalBytes = 0;
        int state = 0;
        int64_t viewOffset = 0;
        int transcodeElapsedSeconds = 0;
    };
    std::vector<CachedItem> m_lastState;

    // UI element tracking for incremental updates
    struct RowElements {
        brls::Box* row = nullptr;
        brls::Label* statusLabel = nullptr;
        std::string ratingKey;
    };
    std::vector<RowElements> m_rowElements;

    // Auto-refresh
    std::atomic<bool> m_autoRefreshEnabled{false};
    std::chrono::steady_clock::time_point m_lastRefresh;
    static constexpr int REFRESH_INTERVAL_MS = 1000;

    // Alive flag for async safety
    std::shared_ptr<bool> m_alive;
};

} // namespace vitaplex
