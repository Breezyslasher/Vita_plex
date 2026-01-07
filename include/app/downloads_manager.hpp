/**
 * VitaPlex - Downloads Manager
 * Handles offline media downloads and progress sync
 *
 * API References:
 * - Download: GET /{part_path}?download=1&X-Plex-Token={token}
 * - Timeline: GET /:/timeline?ratingKey={key}&time={ms}&state={state}&duration={ms}&offline=1
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>

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
    std::string mediaType;      // "movie", "episode"
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

    std::vector<DownloadItem> m_downloads;
    mutable std::mutex m_mutex;
    bool m_downloading = false;
    bool m_initialized = false;
    DownloadProgressCallback m_progressCallback;
    std::string m_downloadsPath;
};

} // namespace vitaplex
