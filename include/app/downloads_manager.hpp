/**
 * VitaPlex - Downloads Manager
 * Handles offline media downloads and progress sync
 *
 * API References:
 * - Download: GET /{part_path}?download=1&X-Plex-Token={token}
 * - Timeline: GET /:/timeline?ratingKey={key}&time={ms}&state={state}&duration={ms}&offline=1
 *
 * Design based on Vita_Suwayomi's download system:
 * - std::deque for thread-safe push_back without pointer invalidation
 * - Atomic flags for download state signaling
 * - State persistence with proper JSON parsing
 * - Resume support for interrupted downloads
 * - File validation on startup
 */

#pragma once

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <atomic>

namespace vitaplex {

// Download state
enum class DownloadState {
    QUEUED,
    DOWNLOADING,
    PAUSED,
    COMPLETED,
    FAILED
};

// Download item information
struct DownloadItem {
    std::string ratingKey;      // Plex rating key
    std::string title;          // Display title
    std::string partPath;       // Path to media file on server
    std::string localPath;      // Local storage path
    std::string thumbUrl;       // Thumbnail URL
    int64_t totalBytes = 0;     // Total file size
    int64_t downloadedBytes = 0; // Downloaded so far
    int64_t duration = 0;       // Media duration in ms
    int64_t viewOffset = 0;     // Watch progress in ms
    DownloadState state = DownloadState::QUEUED;
    std::string mediaType;      // "movie", "episode", "track"
    std::string parentTitle;    // Show name for episodes
    int seasonNum = 0;          // Season number for episodes
    int episodeNum = 0;         // Episode number for episodes
    time_t lastSynced = 0;      // Last time progress was synced to server
};

// Progress callback: (downloadedBytes, totalBytes)
using DownloadProgressCallback = std::function<void(int64_t, int64_t)>;

class DownloadsManager {
public:
    static DownloadsManager& getInstance();

    // Initialize downloads directory and load saved state
    bool init();

    // Queue a media item for download
    bool queueDownload(const std::string& ratingKey, const std::string& title,
                       const std::string& partPath, int64_t duration,
                       const std::string& mediaType = "movie",
                       const std::string& parentTitle = "",
                       int seasonNum = 0, int episodeNum = 0);

    // Start downloading queued items
    void startDownloads();

    // Pause all downloads
    void pauseDownloads();

    // Cancel a specific download
    bool cancelDownload(const std::string& ratingKey);

    // Delete a downloaded item
    bool deleteDownload(const std::string& ratingKey);

    // Get all download items
    std::vector<DownloadItem> getDownloads() const;

    // Get a specific download by rating key
    DownloadItem* getDownload(const std::string& ratingKey);

    // Check if media is downloaded
    bool isDownloaded(const std::string& ratingKey) const;

    // Get local playback path for downloaded media
    std::string getLocalPath(const std::string& ratingKey) const;

    // Update watch progress for downloaded media
    void updateProgress(const std::string& ratingKey, int64_t viewOffset);

    // Sync all offline progress to server (call when online)
    void syncProgressToServer();

    // Pull server progress for downloaded items (call when online)
    void syncProgressFromServer();

    // Bidirectional sync: push local then pull server progress
    void syncProgressBidirectional();

    // Resume incomplete downloads (queues PAUSED/FAILED items)
    void resumeIncompleteDownloads();

    // Check if there are any incomplete downloads
    bool hasIncompleteDownloads() const;

    // Count incomplete downloads
    int countIncompleteDownloads() const;

    // Wait for the download thread to fully exit (call after pauseDownloads)
    void waitForDownloadThread(int timeoutMs = 2000);

    // Check if downloads are currently running
    bool isDownloading() const { return m_downloading.load(); }

    // Clear completed downloads from list
    void clearCompleted();

    // Save/load state to persistent storage
    void saveState();
    void loadState();

    // Set progress callback for UI updates
    void setProgressCallback(DownloadProgressCallback callback);

    // Get downloads directory path
    std::string getDownloadsPath() const;

private:
    DownloadsManager() = default;
    ~DownloadsManager() = default;
    DownloadsManager(const DownloadsManager&) = delete;
    DownloadsManager& operator=(const DownloadsManager&) = delete;

    // Download a single item (runs in background)
    void downloadItem(DownloadItem& item);

    // Report timeline to server
    bool reportTimeline(const DownloadItem& item, const std::string& state);

    // Validate that downloaded files actually exist on disk
    void validateDownloadedFiles();

    // Internal save without locking (caller must hold m_mutex)
    void saveStateUnlocked();

    // Use deque so push_back never invalidates references/pointers to
    // existing elements. The download thread holds a DownloadItem&
    // reference while other threads may queue new items via push_back.
    std::deque<DownloadItem> m_downloads;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_downloading{false};
    std::atomic<bool> m_downloadThreadActive{false};
    bool m_initialized = false;
    DownloadProgressCallback m_progressCallback;
    std::string m_downloadsPath;
};

} // namespace vitaplex
