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
    TRANSCODING,  // Server is transcoding video before download begins
    PAUSED,
    COMPLETED,
    FAILED,
    CANCELLED   // Marked for removal - will be purged when safe
};

// Group type for organizing downloads in the UI
enum class DownloadGroupType {
    NONE = 0,       // Standalone item (single track, movie)
    PLAYLIST = 1,   // Part of a downloaded playlist
    ALBUM = 2,      // Part of a downloaded album
    ARTIST = 3,     // Part of a downloaded artist
    SHOW = 4        // Part of a downloaded TV show
};

// Download item information
struct DownloadItem {
    std::string ratingKey;      // Plex rating key
    std::string title;          // Display title
    std::string partPath;       // Path to media file on server
    std::string localPath;      // Local storage path
    std::string thumbUrl;       // Thumbnail URL (Plex path, e.g. /library/metadata/.../thumb)
    std::string thumbPath;      // Local path to downloaded cover art
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

    // Transcoding progress tracking
    int transcodeElapsedSeconds = 0;  // How long transcoding has been running
    int transcodePollAttempt = 0;     // Current poll attempt number

    // Grouping fields for organized display in downloads tab
    DownloadGroupType groupType = DownloadGroupType::NONE;
    std::string groupKey;       // ratingKey of the parent (playlist/album/artist/show)
    std::string groupTitle;     // Display title of the group
    std::string groupThumb;     // Thumbnail URL of the group (for cover art)
    std::string albumTitle;     // Album title (for tracks in an artist group)
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
                       int seasonNum = 0, int episodeNum = 0,
                       const std::string& thumbUrl = "",
                       DownloadGroupType groupType = DownloadGroupType::NONE,
                       const std::string& groupKey = "",
                       const std::string& groupTitle = "",
                       const std::string& groupThumb = "",
                       const std::string& albumTitle = "");

    // Get all downloads belonging to a specific group
    std::vector<DownloadItem> getDownloadsByGroup(DownloadGroupType type, const std::string& groupKey) const;

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

    // Get a specific download by rating key (returns pointer - caller must hold no assumption about lifetime)
    DownloadItem* getDownload(const std::string& ratingKey);

    // Get a copy of a specific download by rating key (thread-safe)
    bool getDownloadCopy(const std::string& ratingKey, DownloadItem& out) const;

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

    // Download cover art for a music track
    void downloadCoverArt(DownloadItem& item);

    // Report timeline to server
    bool reportTimeline(const DownloadItem& item, const std::string& state);

    // Validate that downloaded files actually exist on disk
    void validateDownloadedFiles();

    // Remove items marked as CANCELLED from the deque (only safe when download thread is idle)
    void purgeCancelledUnlocked();

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
