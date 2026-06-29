/**
 * VitaPlex - Downloads Manager Implementation
 * Handles offline media downloads and progress sync
 *
 * Based on Vita_Suwayomi's download system patterns:
 * - Proper state persistence with JSON parsing
 * - Atomic flags for thread-safe download control
 * - File validation on startup
 * - Resume support for interrupted downloads
 */

#include "app/downloads_manager.hpp"
#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "utils/http_client.hpp"
#include "utils/async.hpp"
#include "platform/paths.hpp"
#include "platform/platform.hpp"
#include <borealis.hpp>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <cctype>
#include <thread>
#include <chrono>
#include <filesystem>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace vitaplex {

// Downloads directory on Vita
#ifdef __vita__
static const char* DOWNLOADS_DIR = "ux0:data/VitaPlex/downloads";
static const char* STATE_FILE = "ux0:data/VitaPlex/downloads/state.json";
#endif

// On-disk size of a file in bytes, or 0 if it doesn't exist / can't be read.
// This is the source of truth for a resume offset: a hard crash can leave the
// in-memory byte counter ahead of what was actually flushed to disk.
static int64_t partFileSize(const std::string& path) {
    if (path.empty()) return 0;
#ifdef __vita__
    SceIoStat st;
    if (sceIoGetstat(path.c_str(), &st) >= 0 && st.st_size > 0) return (int64_t)st.st_size;
    return 0;
#else
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return 0;
    return (int64_t)sz;
#endif
}

// Sequential file writer that can either truncate (fresh download) or append
// (resume), hiding the Vita / desktop file-API split from the download loop.
struct PartFileWriter {
#ifdef __vita__
    SceUID fd = -1;
    bool open(const std::string& p, bool append) {
        int flags = SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC);
        fd = sceIoOpen(p.c_str(), flags, 0777);
        return fd >= 0;
    }
    bool write(const char* d, size_t n) {
        int w = sceIoWrite(fd, d, n);
        return w >= 0 && (size_t)w == n;
    }
    void close() { if (fd >= 0) { sceIoClose(fd); fd = -1; } }
    bool isOpen() const { return fd >= 0; }
#else
    std::ofstream f;
    bool open(const std::string& p, bool append) {
        f.open(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        return f.is_open();
    }
    bool write(const char* d, size_t n) {
        f.write(d, (std::streamsize)n);
        return f.good();
    }
    void close() { if (f.is_open()) f.close(); }
    bool isOpen() { return f.is_open(); }
#endif
};

// Helper: extract a JSON string value by key from a simple JSON object string
static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos += searchKey.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";

    // Unescape basic sequences
    std::string result;
    for (size_t i = pos; i < end; i++) {
        if (json[i] == '\\' && i + 1 < end) {
            i++;
            switch (json[i]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                default: result += json[i]; break;
            }
        } else {
            result += json[i];
        }
    }
    return result;
}

// Helper: extract a JSON integer value by key
static int64_t extractJsonInt(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return 0;
    pos += searchKey.length();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0;

    std::string numStr;
    bool negative = false;
    if (json[pos] == '-') { negative = true; pos++; }
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        numStr += json[pos++];
    }
    if (numStr.empty()) return 0;
    int64_t val = std::strtoll(numStr.c_str(), nullptr, 10);
    return negative ? -val : val;
}

// Helper: escape a string for JSON output
static std::string escapeJson(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

DownloadsManager& DownloadsManager::getInstance() {
    static DownloadsManager instance;
    return instance;
}

bool DownloadsManager::init() {
    if (m_initialized) return true;

#ifdef __vita__
    m_downloadsPath = DOWNLOADS_DIR;
#else
    m_downloadsPath = platformPath("downloads");
#endif

#ifdef __vita__
    // Create downloads directory if it doesn't exist
    sceIoMkdir("ux0:data/VitaPlex", 0777);
    sceIoMkdir(DOWNLOADS_DIR, 0777);
#else
    // Create directory on other platforms
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_downloadsPath), ec);
#endif

    // Load saved state
    loadState();

    // Validate that completed files actually exist
    validateDownloadedFiles();

    m_initialized = true;
    brls::Logger::info("DownloadsManager: Initialized at {} ({} items loaded)",
                       m_downloadsPath, m_downloads.size());
    return true;
}

bool DownloadsManager::queueDownload(const std::string& ratingKey, const std::string& title,
                                      const std::string& partPath, int64_t duration,
                                      const std::string& mediaType,
                                      const std::string& parentTitle,
                                      int seasonNum, int episodeNum,
                                      const std::string& thumbUrl,
                                      DownloadGroupType groupType,
                                      const std::string& groupKey,
                                      const std::string& groupTitle,
                                      const std::string& groupThumb,
                                      const std::string& albumTitle) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Check if already in queue
    for (const auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            brls::Logger::warning("DownloadsManager: {} already in queue", title);
            return false;
        }
    }

    DownloadItem item;
    item.ratingKey = ratingKey;
    item.title = title;
    item.partPath = partPath;
    item.duration = duration;
    item.mediaType = mediaType;
    item.parentTitle = parentTitle;
    item.seasonNum = seasonNum;
    item.episodeNum = episodeNum;
    item.thumbUrl = thumbUrl;
    item.state = DownloadState::QUEUED;
    item.groupType = groupType;
    item.groupKey = groupKey;
    item.groupTitle = groupTitle;
    item.groupThumb = groupThumb;
    item.albumTitle = albumTitle;

    // ratingKey and partPath come from the Plex server's JSON response. A
    // malicious or compromised server can return arbitrary strings, so we
    // must never splice them into filesystem paths unsanitised: e.g. a
    // ratingKey of "../../config/settings" would let the server overwrite
    // other files on the device (CWE-22 path traversal). Restrict each
    // component to a conservative allowlist before concatenation.
    auto sanitizeForFilename = [](const std::string& in, size_t maxLen) {
        std::string out;
        out.reserve(std::min(in.size(), maxLen));
        for (char c : in) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') {
                out.push_back(c);
                if (out.size() >= maxLen) break;
            }
        }
        return out;
    };
    // Extensions are taken from server-supplied part paths. Only accept
    // 1–5 alphanumerics after the final dot (covers mp3/mp4/mkv/webm/flac/
    // m4a/...). Anything else falls back to the media-type default.
    auto sanitizeExtension = [](const std::string& part, const std::string& mt) {
        std::string ext;
        if (!part.empty()) {
            size_t dotPos = part.rfind('.');
            if (dotPos != std::string::npos) {
                std::string raw = part.substr(dotPos + 1);
                if (!raw.empty() && raw.size() <= 5) {
                    bool allAlnum = true;
                    for (char c : raw) {
                        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9'))) { allAlnum = false; break; }
                    }
                    if (allAlnum) ext = "." + raw;
                }
            }
        }
        if (ext.empty()) ext = (mt == "track") ? ".mp3" : ".mp4";
        return ext;
    };

    std::string safeKey = sanitizeForFilename(ratingKey, 64);
    if (safeKey.empty()) {
        brls::Logger::error("DownloadsManager: rejecting download with unsafe ratingKey");
        return false;
    }
    std::string extension = sanitizeExtension(partPath, mediaType);
    std::string filename = safeKey + extension;
    item.localPath = m_downloadsPath + "/" + filename;

    // Generate cover art path for all media types with thumbnails
    if (!thumbUrl.empty()) {
        item.thumbPath = m_downloadsPath + "/" + safeKey + "_cover.jpg";
    }

    m_downloads.push_back(item);

    // Update groupTotalItems on all items in this group so Y stays stable
    if (groupType != DownloadGroupType::NONE && !groupKey.empty()) {
        int groupCount = 0;
        for (const auto& d : m_downloads) {
            if (d.groupType == groupType && d.groupKey == groupKey) {
                groupCount++;
            }
        }
        for (auto& d : m_downloads) {
            if (d.groupType == groupType && d.groupKey == groupKey) {
                d.groupTotalItems = groupCount;
            }
        }
    }

    saveStateUnlocked();

    brls::Logger::info("DownloadsManager: Queued {} for download", title);
    return true;
}

void DownloadsManager::startDownloads() {
    if (m_downloading.load()) return;
    m_downloading.store(true);

    brls::Logger::info("DownloadsManager: Starting download queue");

    // Process downloads in background with larger stack size.
    // downloadItem() has deep call stacks (HTTP, HLS parsing, file I/O)
    // that can overflow the Vita's default 256KB thread stack.
    asyncRunLargeStack([this]() {
        m_downloadThreadActive.store(true);
        brls::Logger::info("DownloadsManager: Download thread started");

        while (m_downloading.load()) {
            std::string nextRatingKey;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& item : m_downloads) {
                    if (item.state == DownloadState::QUEUED) {
                        item.state = DownloadState::DOWNLOADING;
                        nextRatingKey = item.ratingKey;
                        brls::Logger::info("DownloadsManager: Found queued item: {}", item.title);
                        break;
                    }
                }
            }

            if (!nextRatingKey.empty()) {
                // Access the item safely through the mutex for each operation.
                // We hold the ratingKey (stable identifier) instead of a raw
                // pointer to the deque element, which would be invalidated if
                // the deque is modified (e.g. by clearCompleted/purge).
                DownloadItem* nextItem = findItemByKey(nextRatingKey);
                if (nextItem && nextItem->state != DownloadState::CANCELLED) {
                    brls::Logger::info("DownloadsManager: Starting download of {}", nextItem->title);
                    downloadItem(*nextItem);

                    // Retry failed downloads with exponential backoff
                    int retries = 0;
                    int maxRetries = (nextItem->mediaType == "track") ? 3 : 5;
                    while (m_downloading.load()) {
                        // Re-lookup item each retry iteration — deque may have changed
                        nextItem = findItemByKey(nextRatingKey);
                        if (!nextItem || nextItem->state != DownloadState::FAILED || retries >= maxRetries) break;

                        retries++;
                        int waitSec = retries * 5;  // 5s, 10s, 15s, 20s, 25s
                        brls::Logger::info("DownloadsManager: Retry {}/{} for {} in {}s",
                                          retries, maxRetries, nextRatingKey, waitSec);
#ifdef __vita__
                        sceKernelDelayThread(waitSec * 1000 * 1000);
#else
                        std::this_thread::sleep_for(std::chrono::seconds(waitSec));
#endif
                        // Re-lookup again after sleep — item may have been cancelled/removed
                        nextItem = findItemByKey(nextRatingKey);
                        if (!nextItem || nextItem->state == DownloadState::CANCELLED) break;

                        nextItem->state = DownloadState::DOWNLOADING;
                        nextItem->downloadedBytes = 0;
                        downloadItem(*nextItem);
                    }
                }
            } else {
                // No more queued items
                brls::Logger::info("DownloadsManager: No more queued items");
                break;
            }
        }
        m_downloading.store(false);
        m_downloadThreadActive.store(false);

        // Now safe to purge cancelled items since no references are held
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            purgeCancelledUnlocked();
            saveStateUnlocked();
        }

        brls::Logger::info("DownloadsManager: Download thread finished");
    });
}

void DownloadsManager::pauseDownloads() {
    m_downloading.store(false);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_downloads) {
        if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) {
            item.state = DownloadState::PAUSED;
        }
    }
    saveStateUnlocked();
}

bool DownloadsManager::cancelDownload(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            // Delete partial file if exists
            if (!item.localPath.empty()) {
#ifdef __vita__
                sceIoRemove(item.localPath.c_str());
#else
                std::remove(item.localPath.c_str());
#endif
            }
            // Mark as cancelled instead of erasing - safe while download thread runs
            item.state = DownloadState::CANCELLED;
            // Only purge if download thread is not active to avoid invalidating references
            if (!m_downloadThreadActive.load()) {
                purgeCancelledUnlocked();
            }
            saveStateUnlocked();
            return true;
        }
    }
    return false;
}

bool DownloadsManager::deleteDownload(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            // Delete media file
            if (!item.localPath.empty()) {
#ifdef __vita__
                sceIoRemove(item.localPath.c_str());
#else
                std::remove(item.localPath.c_str());
#endif
            }
            // Delete cover art file if exists
            if (!item.thumbPath.empty()) {
#ifdef __vita__
                sceIoRemove(item.thumbPath.c_str());
#else
                std::remove(item.thumbPath.c_str());
#endif
            }
            // Mark as cancelled instead of erasing - safe while download thread runs
            item.state = DownloadState::CANCELLED;
            // Only purge if download thread is not active to avoid invalidating references
            if (!m_downloadThreadActive.load()) {
                purgeCancelledUnlocked();
            }
            saveStateUnlocked();
            brls::Logger::info("DownloadsManager: Deleted download {}", ratingKey);
            return true;
        }
    }
    return false;
}

std::vector<DownloadItem> DownloadsManager::getDownloads() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DownloadItem> result;
    for (const auto& item : m_downloads) {
        if (item.state != DownloadState::CANCELLED) {
            result.push_back(item);
        }
    }
    return result;
}

DownloadItem* DownloadsManager::getDownload(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            return &item;
        }
    }
    return nullptr;
}

bool DownloadsManager::getDownloadCopy(const std::string& ratingKey, DownloadItem& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            out = item;
            return true;
        }
    }
    return false;
}

std::vector<DownloadItem> DownloadsManager::getDownloadsByGroup(DownloadGroupType type, const std::string& groupKey) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DownloadItem> result;
    for (const auto& item : m_downloads) {
        if (item.state != DownloadState::CANCELLED &&
            item.groupType == type && item.groupKey == groupKey) {
            result.push_back(item);
        }
    }
    return result;
}

bool DownloadsManager::isDownloaded(const std::string& ratingKey) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_downloads) {
        if (item.ratingKey == ratingKey && item.state == DownloadState::COMPLETED) {
            return true;
        }
    }
    return false;
}

std::string DownloadsManager::getLocalPath(const std::string& ratingKey) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_downloads) {
        if (item.ratingKey == ratingKey && item.state == DownloadState::COMPLETED) {
            return item.localPath;
        }
    }
    return "";
}

void DownloadsManager::updateProgress(const std::string& ratingKey, int64_t viewOffset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            int64_t oldOffset = item.viewOffset;
            item.viewOffset = viewOffset;
            brls::Logger::debug("DownloadsManager: Updated progress for {} to {}ms",
                               item.title, viewOffset);

            // Save periodically - every 30 seconds of progress to avoid data loss on crash
            int64_t delta = (viewOffset > oldOffset) ? (viewOffset - oldOffset) : (oldOffset - viewOffset);
            if (delta >= 30000) {
                saveStateUnlocked();
            }
            break;
        }
    }
}

void DownloadsManager::syncProgressToServer() {
    std::vector<DownloadItem> itemsToSync;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_downloads) {
            if (item.state == DownloadState::COMPLETED && item.viewOffset > 0) {
                itemsToSync.push_back(item);
            }
        }
    }

    brls::Logger::info("DownloadsManager: Syncing {} items to server", itemsToSync.size());

    for (auto& item : itemsToSync) {
        if (reportTimeline(item, "stopped")) {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Update last synced time
            for (auto& d : m_downloads) {
                if (d.ratingKey == item.ratingKey) {
                    d.lastSynced = std::time(nullptr);
                    break;
                }
            }
        }
    }

    saveState();
}

void DownloadsManager::syncProgressFromServer() {
    std::vector<std::string> ratingKeys;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_downloads) {
            if (item.state == DownloadState::COMPLETED) {
                ratingKeys.push_back(item.ratingKey);
            }
        }
    }

    if (ratingKeys.empty()) return;

    brls::Logger::info("DownloadsManager: Pulling server progress for {} items", ratingKeys.size());

    PlexClient& client = PlexClient::getInstance();
    for (const auto& key : ratingKeys) {
        MediaItem serverItem;
        if (client.fetchMediaDetails(key, serverItem) && serverItem.viewOffset > 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& d : m_downloads) {
                if (d.ratingKey == key) {
                    // Use whichever progress is further ahead
                    if (serverItem.viewOffset > d.viewOffset) {
                        brls::Logger::info("DownloadsManager: Updated local progress for {} from {}ms to {}ms (from server)",
                                          d.title, d.viewOffset, serverItem.viewOffset);
                        d.viewOffset = serverItem.viewOffset;
                    }
                    break;
                }
            }
        }
    }

    saveState();
}

void DownloadsManager::syncProgressBidirectional() {
    brls::Logger::info("DownloadsManager: Starting bidirectional progress sync");
    syncProgressToServer();
    syncProgressFromServer();
    brls::Logger::info("DownloadsManager: Bidirectional sync complete");
}

void DownloadsManager::resumeIncompleteDownloads() {
    int resumed = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_downloads) {
            if (item.state == DownloadState::PAUSED || item.state == DownloadState::FAILED) {
                item.state = DownloadState::QUEUED;
                // Keep the partial file; resume from whatever is on disk. The
                // download continues via HTTP Range (the Download Queue /media
                // file and direct files both support it), and cleanly falls
                // back to a full re-download if the server answers 200.
                item.downloadedBytes = partFileSize(item.localPath);
                resumed++;
            }
        }
        if (resumed > 0) {
            saveStateUnlocked();
        }
    }

    if (resumed > 0) {
        brls::Logger::info("DownloadsManager: Resumed {} incomplete downloads", resumed);
        startDownloads();
    }
}

bool DownloadsManager::hasIncompleteDownloads() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_downloads) {
        if (item.state == DownloadState::PAUSED || item.state == DownloadState::FAILED ||
            item.state == DownloadState::QUEUED) {
            return true;
        }
    }
    return false;
}

int DownloadsManager::countIncompleteDownloads() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    int count = 0;
    for (const auto& item : m_downloads) {
        if (item.state == DownloadState::PAUSED || item.state == DownloadState::FAILED ||
            item.state == DownloadState::QUEUED) {
            count++;
        }
    }
    return count;
}

void DownloadsManager::waitForDownloadThread(int timeoutMs) {
    int waited = 0;
    while (m_downloadThreadActive.load() && waited < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
    }
    if (m_downloadThreadActive.load()) {
        brls::Logger::warning("DownloadsManager: Download thread did not exit within {}ms", timeoutMs);
    }
}

void DownloadsManager::clearCompleted() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& item : m_downloads) {
        if (item.state == DownloadState::COMPLETED) {
            // Delete media file
            if (!item.localPath.empty()) {
#ifdef __vita__
                sceIoRemove(item.localPath.c_str());
#else
                std::remove(item.localPath.c_str());
#endif
            }
            // Delete cover art file
            if (!item.thumbPath.empty()) {
#ifdef __vita__
                sceIoRemove(item.thumbPath.c_str());
#else
                std::remove(item.thumbPath.c_str());
#endif
            }
            item.state = DownloadState::CANCELLED;
        }
    }
    purgeCancelledUnlocked();
    saveStateUnlocked();
    brls::Logger::info("DownloadsManager: Cleared completed downloads");
}

void DownloadsManager::purgeCancelledUnlocked() {
    // Only erase from deque when download thread is not active
    // to avoid invalidating references held by the download thread
    if (m_downloadThreadActive.load()) return;

    auto it = m_downloads.begin();
    while (it != m_downloads.end()) {
        if (it->state == DownloadState::CANCELLED) {
            it = m_downloads.erase(it);
        } else {
            ++it;
        }
    }
}

DownloadItem* DownloadsManager::findItemByKey(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_downloads) {
        if (item.ratingKey == ratingKey) {
            return &item;
        }
    }
    return nullptr;
}

// Convert .plex.direct HTTPS URL to plain HTTP with real IP.
// The .plex.direct hostnames embed the LAN IP (e.g., 192-168-1-28.xxx.plex.direct)
// and exist solely to provide SSL certs for local servers. On the Vita's limited
// SSL stack, HTTPS streaming transcode connections frequently fail with
// CURLE_RECV_ERROR. Using HTTP on the LAN avoids this entirely.
static std::string convertToHttpForDownload(const std::string& url) {
    // Only convert .plex.direct HTTPS URLs
    size_t plexDirect = url.find(".plex.direct");
    if (plexDirect == std::string::npos) return url;
    if (url.substr(0, 8) != "https://") return url;

    // Extract the IP from hostname: "192-168-1-28.HASH.plex.direct"
    size_t hostStart = 8;  // after "https://"
    size_t firstDot = url.find('.', hostStart);
    if (firstDot == std::string::npos || firstDot >= plexDirect) return url;

    std::string ipDashed = url.substr(hostStart, firstDot - hostStart);

    // Convert dashes to dots: "192-168-1-28" -> "192.168.1.28"
    std::string ip;
    for (char c : ipDashed) {
        ip += (c == '-') ? '.' : c;
    }

    // Validate it looks like an IP (4 dot-separated numbers)
    int dots = 0;
    for (char c : ip) {
        if (c == '.') dots++;
        else if (c < '0' || c > '9') return url;  // Not an IP
    }
    if (dots != 3) return url;

    // Find the port (after .plex.direct:PORT)
    size_t portStart = url.find(':', plexDirect);
    std::string port = "32400";
    std::string pathAndQuery;
    if (portStart != std::string::npos) {
        size_t pathStart = url.find('/', portStart);
        if (pathStart != std::string::npos) {
            port = url.substr(portStart + 1, pathStart - portStart - 1);
            pathAndQuery = url.substr(pathStart);
        } else {
            port = url.substr(portStart + 1);
        }
    } else {
        size_t pathStart = url.find('/', plexDirect);
        if (pathStart != std::string::npos) {
            pathAndQuery = url.substr(pathStart);
        }
    }

    std::string httpUrl = "http://" + ip + ":" + port + pathAndQuery;
    brls::Logger::info("DownloadsManager: Converted to HTTP: {}:{}...", ip, port);
    return httpUrl;
}

// Helper: Build standard Plex client headers for requests
static void addPlexHeaders(HttpRequest& req, const std::string& token) {
    const auto& vc = platform::getVideoConstraints();
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_NAME;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    req.headers["X-Plex-Platform"] = vc.plexPlatform;
    req.headers["X-Plex-Device"] = vc.plexDevice;
    req.headers["X-Plex-Device-Name"] = vc.plexDevice;
    req.headers["X-Plex-Token"] = token;
}

// Build a direct file download URL using the part path.
// Downloads the original file without transcoding - suitable for audio.
static std::string buildDirectDownloadUrl(const std::string& serverUrl, const std::string& token,
                                           const std::string& partPath) {
    if (partPath.empty()) return "";

    std::string baseUrl = convertToHttpForDownload(serverUrl);
    // The part path is like /library/parts/19779/1760220985/file.m4a
    // We add ?download=1 to signal this is a download (lower priority than streaming)
    std::string url = baseUrl + partPath + "?download=1&X-Plex-Token=" + token;
    return url;
}

// Try downloading via the Plex Download Queue API (server-side transcode + file download).
// This transcodes video to platform-compatible resolution before downloading.
// Polls the queue status until transcoding is complete before returning the media URL.
// Returns true if the download URL was obtained, with the URL stored in outUrl.
static bool tryDownloadQueueApi(const std::string& serverUrl, const std::string& token,
                                 const std::string& ratingKey, std::string& outUrl,
                                 std::atomic<bool>& downloading, DownloadItem& item) {
    brls::Logger::info("DownloadsManager: Trying Download Queue API for {}", ratingKey);

    std::string baseUrl = convertToHttpForDownload(serverUrl);

    // Step 1: Create/get a download queue
    HttpClient http;
    HttpRequest req;
    req.url = baseUrl + "/downloadQueue?X-Plex-Token=" + token;
    req.method = "POST";
    addPlexHeaders(req, token);
    req.timeout = 30;
    HttpResponse resp = http.request(req);

    if (resp.statusCode != 200 || resp.body.empty()) {
        brls::Logger::warning("DownloadsManager: Download Queue API not available (HTTP {})", resp.statusCode);
        return false;
    }

    brls::Logger::debug("DownloadsManager: downloadQueue response: {}", resp.body.substr(0, 500));

    // Parse queue ID from response
    std::string queueId;
    int64_t numId = extractJsonInt(resp.body, "id");
    if (numId > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)numId);
        queueId = buf;
    }

    if (queueId.empty()) {
        brls::Logger::warning("DownloadsManager: Could not parse queue ID from response");
        return false;
    }

    brls::Logger::info("DownloadsManager: Got download queue ID: {}", queueId);

    // Step 1b: Clean up any existing items in the queue that may be stuck in "deciding"
    // Stale items from previous attempts can prevent the queue from progressing.
    {
        HttpClient cleanHttp;
        HttpRequest cleanReq;
        cleanReq.url = baseUrl + "/downloadQueue/" + queueId + "/items?X-Plex-Token=" + token;
        cleanReq.method = "GET";
        addPlexHeaders(cleanReq, token);
        cleanReq.timeout = 15;
        HttpResponse cleanResp = cleanHttp.request(cleanReq);

        if (cleanResp.statusCode == 200 && !cleanResp.body.empty()) {
            // Find all item IDs and delete them
            size_t searchPos = 0;
            while (searchPos < cleanResp.body.size()) {
                size_t idPos = cleanResp.body.find("\"id\":", searchPos);
                if (idPos == std::string::npos) break;
                int64_t oldItemId = extractJsonInt(cleanResp.body.substr(idPos), "id");
                if (oldItemId > 0) {
                    char oldIdBuf[32];
                    snprintf(oldIdBuf, sizeof(oldIdBuf), "%lld", (long long)oldItemId);
                    HttpClient delHttp;
                    HttpRequest delReq;
                    delReq.url = baseUrl + "/downloadQueue/" + queueId + "/items/" + oldIdBuf
                        + "?X-Plex-Token=" + token;
                    delReq.method = "DELETE";
                    addPlexHeaders(delReq, token);
                    delReq.timeout = 10;
                    delHttp.request(delReq);
                    brls::Logger::debug("DownloadsManager: Cleaned up old queue item {}", oldIdBuf);
                }
                searchPos = idPos + 5;
            }
        }
    }

    // Step 2: Add item to the download queue with transcode parameters.
    // Per API spec (openapi.json), the /add endpoint accepts transcode query params
    // (parameters #2-#27) and profile headers (#28-#35).
    bool isAudio = (item.mediaType == "track");
    std::string addUrl = baseUrl + "/downloadQueue/" + queueId + "/add"
        + "?keys=/library/metadata/" + ratingKey;

    // protocol=http tells the server to produce a single downloadable file
    // (as opposed to hls/dash streaming segments).
    //
    // directPlay=1 and directStream=1: tell the server the client can
    // play / direct-stream the source if it fits the profile-extra
    // constraints below. The server uses these as *capabilities*, not
    // demands — it'll still transcode when the source exceeds our
    // limits (e.g. 1080p source for the Vita's 544p ceiling), but it
    // will skip transcoding entirely when the source already matches
    // (Switch/Android/desktop with an h264 source within their codec
    // limits). The previous directPlay=0&directStream=0 forced a
    // server-side transcode on every video download, which is why a
    // 3.6 GB movie sat in /media 503-loop for minutes even when the
    // source was already client-compatible.
    addUrl += "&protocol=http";
    addUrl += "&directPlay=1&directStream=1&directStreamAudio=1";
    addUrl += "&location=lan";
    addUrl += "&audioBoost=100";
    // Keep the source's surround layout when the user asked for it; otherwise
    // cap at 2.0 stereo (the default — smaller, universally playable).
    AppSettings& settings = Application::getInstance().getSettings();
    if (!settings.downloadKeepOriginalAudio)
        addUrl += "&audioChannelCount=2";
    addUrl += "&mediaIndex=0&partIndex=0";

    std::string profileExtra;
    if (isAudio) {
        addUrl += "&musicBitrate=320";
        // Direct play profiles cover the common audio containers — server
        // ships the file untouched when it matches one of these. Transcode
        // target stays as the fallback for everything else.
        profileExtra =
            "add-direct-play-profile(type=musicProfile"
                "&container=mp3&audioCodec=mp3)"
            "+add-direct-play-profile(type=musicProfile"
                "&container=mp4&audioCodec=aac)"
            "+add-direct-play-profile(type=musicProfile"
                "&container=mp4&audioCodec=alac)"
            "+add-direct-play-profile(type=musicProfile"
                "&container=flac&audioCodec=flac)"
            "+add-direct-play-profile(type=musicProfile"
                "&container=ogg&audioCodec=vorbis)"
            "+add-direct-play-profile(type=musicProfile"
                "&container=ogg&audioCodec=opus)"
            // replace=true (override the Generic profile's built-in target) and
            // both streaming + static contexts, matching the video targets so a
            // download decision finds a usable target whichever context it uses.
            "+add-transcode-target(type=musicProfile"
                "&context=streaming&protocol=http"
                "&container=mp3&audioCodec=mp3&replace=true)"
            "+add-transcode-target(type=musicProfile"
                "&context=static&protocol=http"
                "&container=mp3&audioCodec=mp3&replace=true)";
    } else {
        const auto& vc = platform::getVideoConstraints();
        // Resolution + video bitrate from the download-quality setting. ORIGINAL
        // keeps the platform's ceiling (and, with directPlay above, ships an
        // already-compatible source untouched); a specific tier yields a smaller
        // file that also encodes faster.
        std::string resolution = vc.defaultResolution;
        int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : vc.defaultBitrate;
        switch (settings.downloadQuality) {
            case VideoQuality::QUALITY_1080P: resolution = "1920x1080"; bitrate = 20000; break;
            case VideoQuality::QUALITY_720P:  resolution = "1280x720";  bitrate = 4000;  break;
            case VideoQuality::QUALITY_480P:  resolution = "854x480";   bitrate = 2000;  break;
            case VideoQuality::QUALITY_360P:  resolution = "640x360";   bitrate = 1000;  break;
            case VideoQuality::QUALITY_240P:  resolution = "426x240";   bitrate = 500;   break;
            case VideoQuality::ORIGINAL: default: break;  // keep the platform default
        }
        char bitrateStr[64];
        snprintf(bitrateStr, sizeof(bitrateStr), "&videoBitrate=%d", bitrate);
        addUrl += bitrateStr;
        addUrl += "&videoResolution=" + resolution;
        // subtitles=embedded muxes soft subs into the file (mpv can toggle them);
        // none strips them (the default). Never =burn — that hardcodes subs and
        // adds a re-encode pass. subCodec is only advertised on the transcode
        // target when subs are wanted, so the default (no-subs) profile stays
        // exactly the one already known to work.
        const bool wantSubs = settings.downloadIncludeSubtitles;
        addUrl += wantSubs ? "&subtitles=embedded" : "&subtitles=none";
        const char* subCodec = wantSubs ? "&subtitleCodec=mov_text" : "";
        char dlProfileBuf[1700];
        snprintf(dlProfileBuf, sizeof(dlProfileBuf),
            // Direct-play profiles say "ship this file untouched" — keep them
            // for H.264 (mp4/mkv) so an already-compatible source downloads
            // as-is. We deliberately do NOT list HEVC: with an HEVC direct-play
            // profile the server tries to KEEP HEVC for the download (remux),
            // which it can't resolve for http + multichannel audio (the "Cannot
            // make a decision" error) and which wouldn't play on the Vita/Switch
            // anyway. Dropping it forces HEVC down the H.264 transcode path.
            "add-direct-play-profile(type=videoProfile"
                "&container=mp4&videoCodec=h264&audioCodec=aac)"
            "+add-direct-play-profile(type=videoProfile"
                "&container=mp4&videoCodec=h264&audioCodec=ac3)"
            "+add-direct-play-profile(type=videoProfile"
                "&container=mp4&videoCodec=h264&audioCodec=mp3)"
            "+add-direct-play-profile(type=videoProfile"
                "&container=mkv&videoCodec=h264&audioCodec=aac)"
            "+add-direct-play-profile(type=videoProfile"
                "&container=mkv&videoCodec=h264&audioCodec=ac3)"
            "+add-direct-play-profile(type=videoProfile"
                "&container=mkv&videoCodec=h264&audioCodec=eac3)"
            // Transcode fallback (H.264/AAC, mp4 to match our stored file).
            // replace=true so it overrides the Generic profile's built-in
            // target instead of being ignored. We register BOTH a streaming
            // AND a static target: a download decision can resolve in either
            // context, and registering only "streaming" left the server
            // reporting "no transcode profile" for the download decision.
            "+add-transcode-target(type=videoProfile"
                "&context=streaming&protocol=http"
                "&container=mp4&videoCodec=h264"
                "&audioCodec=aac%s&replace=true)"
            "+add-transcode-target(type=videoProfile"
                "&context=static&protocol=http"
                "&container=mp4&videoCodec=h264"
                "&audioCodec=aac%s&replace=true)"
            "+add-limitation(scope=videoCodec&scopeName=h264"
                "&type=upperBound&name=video.level&value=%d)"
            "+add-limitation(scope=videoCodec&scopeName=h264"
                "&type=upperBound&name=video.width&value=%d)"
            "+add-limitation(scope=videoCodec&scopeName=h264"
                "&type=upperBound&name=video.height&value=%d)",
            subCodec, subCodec, vc.maxVideoLevel, vc.maxVideoWidth, vc.maxVideoHeight);
        profileExtra = dlProfileBuf;
    }

    addUrl += "&X-Plex-Token=" + token;

    HttpRequest addReq;
    addReq.url = addUrl;
    addReq.method = "POST";
    addPlexHeaders(addReq, token);
    addReq.headers["X-Plex-Client-Profile-Name"] = "Generic";
    addReq.headers["X-Plex-Client-Profile-Extra"] = profileExtra;
    addReq.timeout = 30;

    HttpClient addHttp;
    HttpResponse addResp = addHttp.request(addReq);

    if (addResp.statusCode != 200) {
        brls::Logger::warning("DownloadsManager: Failed to add to download queue (HTTP {})", addResp.statusCode);
        return false;
    }

    brls::Logger::info("DownloadsManager: Added to download queue, response: {}",
                      addResp.body.substr(0, 500));

    // Parse item ID from the add response (AddedQueueItems[].id)
    std::string itemId;
    int64_t numItemId = extractJsonInt(addResp.body, "id");
    if (numItemId > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)numItemId);
        itemId = buf;
    }

    if (itemId.empty()) {
        brls::Logger::warning("DownloadsManager: Could not determine item ID in download queue");
        return false;
    }

    brls::Logger::info("DownloadsManager: Download queue item ID: {}", itemId);

    // /decision intentionally not called. The openapi spec defines
    // GET /downloadQueue/{q}/item/{i}/decision as a read-only "grab
    // the decision" query — transcoding is already triggered by /add
    // above (Plex's server log shows the "Downloading document
    // /library/metadata/N" trace fire as part of the /add request).
    // When we did call it the server returned 400 Bad Request because
    // the spec only allows path params there but we were attaching
    // the X-Plex-Client-Profile-Extra header; the 400 didn't break
    // the download but the failure log was noise. The /media poll
    // below does all the "is it ready yet" work via 200 / 503 +
    // Retry-After per the spec.

    // Step 4: Wait for the queue item to finish transcoding.
    //
    // Switched from polling /media (HEAD) to polling /items/{itemId}
    // (GET) — same liveness signal, but /items returns JSON with the
    // queue item's status enum (deciding/waiting/processing/available
    // /error/expired) AND a TranscodeSession.progress (0-100) percent,
    // so we get a real number for the UI and don't repeatedly ask the
    // server to spin up a media stream just to check whether it's done.
    //
    // Polling cadence stretches over time so a long transcode doesn't
    // turn into thousands of log lines: 5s base, capped at 30s after a
    // few minutes. Plex's Retry-After is almost always -1 (unknown),
    // so a fixed schedule is more predictable than honoring it. Status
    // changes log at INFO; the per-poll details are DEBUG.
    item.state = DownloadState::TRANSCODING;
    item.transcodeElapsedSeconds = 0;
    item.transcodePollAttempt = 0;
    item.transcodeProgressPercent = 0;
    std::string itemUrl = baseUrl + "/downloadQueue/" + queueId
        + "/items/" + itemId + "?X-Plex-Token=" + token;
    std::string mediaUrl = baseUrl + "/downloadQueue/" + queueId
        + "/item/" + itemId + "/media?X-Plex-Token=" + token;

    auto pollInterval = [](int elapsedSec) {
        if (elapsedSec < 30)  return 5;   // First 30s: 5s
        if (elapsedSec < 120) return 10;  // 30s–2m: 10s
        if (elapsedSec < 300) return 20;  // 2–5m: 20s
        return 30;                        // 5m+: 30s
    };

    const int maxElapsedSeconds = 60 * 60;  // 1 hour absolute cap
    bool mediaReady = false;
    std::string lastStatus;
    int consecutiveErrors = 0;
    auto transcodeStart = std::chrono::steady_clock::now();

    for (int attempt = 0; downloading.load(); attempt++) {
        auto now = std::chrono::steady_clock::now();
        int elapsedSec = (int)std::chrono::duration_cast<std::chrono::seconds>(
            now - transcodeStart).count();
        item.transcodeElapsedSeconds = elapsedSec;
        item.transcodePollAttempt = attempt;

        if (elapsedSec > maxElapsedSeconds) {
            brls::Logger::error(
                "DownloadsManager: Transcode timeout after {}s, giving up",
                elapsedSec);
            break;
        }

        HttpClient itemHttp;
        HttpRequest itemReq;
        itemReq.url = itemUrl;
        itemReq.method = "GET";
        addPlexHeaders(itemReq, token);
        itemReq.timeout = 15;
        HttpResponse itemResp = itemHttp.request(itemReq);

        if (itemResp.statusCode != 200) {
            brls::Logger::warning(
                "DownloadsManager: /items poll HTTP {} (attempt {}, elapsed {}s)",
                itemResp.statusCode, attempt + 1, elapsedSec);
            if (++consecutiveErrors >= 5) {
                brls::Logger::error(
                    "DownloadsManager: 5 consecutive /items errors, giving up");
                break;
            }
            int wait = pollInterval(elapsedSec);
#ifdef __vita__
            sceKernelDelayThread(wait * 1000 * 1000);
#else
            std::this_thread::sleep_for(std::chrono::seconds(wait));
#endif
            continue;
        }
        consecutiveErrors = 0;

        std::string status = extractJsonString(itemResp.body, "status");
        int progress = (int)extractJsonInt(itemResp.body, "progress");
        if (progress >= 0 && progress <= 100) {
            item.transcodeProgressPercent = progress;
        }

        if (status != lastStatus) {
            brls::Logger::info(
                "DownloadsManager: Queue item {} status={} progress={}% elapsed={}s",
                itemId, status, item.transcodeProgressPercent, elapsedSec);
            lastStatus = status;
        } else {
            brls::Logger::debug(
                "DownloadsManager: status={} progress={}% elapsed={}s",
                status, item.transcodeProgressPercent, elapsedSec);
        }

        if (status == "available") {
            mediaReady = true;
            brls::Logger::info(
                "DownloadsManager: Media ready after {}s ({} polls)",
                elapsedSec, attempt + 1);
            break;
        }
        if (status == "error" || status == "expired") {
            brls::Logger::error(
                "DownloadsManager: Queue item ended in status='{}', giving up",
                status);
            break;
        }
        // deciding / waiting / processing — keep polling.

        int wait = pollInterval(elapsedSec);
#ifdef __vita__
        sceKernelDelayThread(wait * 1000 * 1000);
#else
        std::this_thread::sleep_for(std::chrono::seconds(wait));
#endif
    }

    if (!downloading.load()) {
        brls::Logger::info("DownloadsManager: Download cancelled during transcode wait");
        return false;
    }

    if (!mediaReady) {
        brls::Logger::warning("DownloadsManager: Download Queue API failed, will fall back to streaming transcode");
        return false;
    }

    // Step 5: The media URL is ready - return it for the caller to download via GET
    outUrl = mediaUrl;

    brls::Logger::info("DownloadsManager: Download Queue media URL ready");
    return true;
}

void DownloadsManager::downloadItem(DownloadItem& item) {
    brls::Logger::info("DownloadsManager: Starting download of {}", item.title);

    PlexClient& client = PlexClient::getInstance();
    std::string serverUrl = client.getServerUrl();
    std::string token = client.getAuthToken();

    // Fetch exact file size from Plex metadata API if not already known
    if (item.totalBytes <= 0) {
        MediaItem mediaInfo;
        if (client.fetchMediaDetails(item.ratingKey, mediaInfo) && mediaInfo.partSize > 0) {
            item.totalBytes = mediaInfo.partSize;
            brls::Logger::info("DownloadsManager: Got file size from API: {} bytes", item.totalBytes);
        }
    }

    if (serverUrl.empty() || token.empty()) {
        brls::Logger::error("DownloadsManager: Not connected to server");
        item.state = DownloadState::FAILED;
        saveState();
        return;
    }

    bool isAudio = (item.mediaType == "track");
    std::string url;
    std::string profileExtra;
    bool urlReady = false;

    // Layered URL strategy — try each option from "most Plex-native" to
    // "last resort", taking the first that works for this item + server:
    //
    // 1. Plex Download Queue API (/downloadQueue/...). The canonical way
    //    Plex clients request offline media. Server enqueues the item,
    //    transcodes server-side to a client-compatible container, and
    //    serves a single file via /downloadQueue/{q}/item/{i}/media with
    //    503 + Retry-After while it works. Sets item.state to TRANSCODING
    //    during the poll so the UI shows progress.
    //
    // 2. ?download=1 on the part URL. Plain HTTP GET that returns the
    //    source file untouched — fastest path, but only useful when the
    //    file's native container is already client-compatible.
    //
    // 3. HLS streaming transcode (/video/:/transcode/universal/...).
    //    Last resort, captures a streaming session to disk in HLS
    //    segments. Awkward to play back outside the app but exists for
    //    servers that don't expose the Download Queue API.
    // Platform gate (platform::getVideoConstraints().supportsHevc). A platform
    // whose decoder plays the source directly grabs the raw file and SKIPS the
    // (often very slow, CPU-bound) server transcode — seconds instead of the
    // many minutes a transcode takes. The Vita, which is H.264-only, leaves
    // supportsHevc=false and routes through the Download Queue for a server-side
    // transcode to H.264 (directPlay=1 still ships an already-compatible source
    // untouched there).
    // Direct (raw) download only when the platform plays HEVC AND the user
    // hasn't asked for a smaller transcoded copy (downloadQuality == ORIGINAL).
    // A specific download quality forces the queue so the server produces the
    // chosen-resolution transcode even on a platform that could play the raw.
    const bool preferDirect =
        platform::getVideoConstraints().supportsHevc &&
        Application::getInstance().getSettings().downloadQuality == VideoQuality::ORIGINAL &&
        !item.partPath.empty();

    if (!preferDirect &&
        tryDownloadQueueApi(serverUrl, token, item.ratingKey, url, m_downloading, item)) {
        urlReady = true;
        brls::Logger::info("DownloadsManager: Download Queue API ready for {}", item.title);
    } else if (!item.partPath.empty()) {
        url = buildDirectDownloadUrl(serverUrl, token, item.partPath);
        if (!url.empty()) {
            urlReady = true;
            brls::Logger::info("DownloadsManager: Direct file download for {} ({})",
                               item.title, preferDirect ? "raw, no transcode" : "fallback");
        }
    } else {
        brls::Logger::warning(
            "DownloadsManager: No Download Queue + no partPath for {} — HLS transcode",
            item.title);
    }

    // Fall back to streaming transcode if no URL ready yet
    if (!urlReady) {
        // Build transcode URL for Vita-compatible format
        std::string metadataPath = "/library/metadata/" + item.ratingKey;
        std::string encodedPath = HttpClient::urlEncode(metadataPath);

        // Build query parameters (shared between decision and start endpoints)
        // Video uses HLS protocol (same as the working player) since HTTP progressive
        // download (start.mp4 with protocol=http) returns HTTP 400 on many Plex servers.
        std::string queryParams;
        queryParams += "path=" + encodedPath;
        queryParams += "&mediaIndex=0&partIndex=0";
        queryParams += "&offset=0";
        queryParams += "&directPlay=0&directStream=0";
        queryParams += "&directStreamAudio=1";
        queryParams += "&hasMDE=1";
        queryParams += "&location=lan";
        queryParams += "&audioBoost=100";
        queryParams += "&audioChannelCount=2";

        if (isAudio) {
            queryParams += "&protocol=http";
            queryParams += "&musicBitrate=320";
            profileExtra = "add-transcode-target(type=musicProfile"
                           "&context=streaming&protocol=http"
                           "&container=mp3&audioCodec=mp3)";
        } else {
            // Use HLS protocol - matches the working player configuration
            queryParams += "&protocol=hls";

            const auto& vc = platform::getVideoConstraints();
            AppSettings& settings = Application::getInstance().getSettings();
            int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : vc.defaultBitrate;

            char bitrateStr[64];
            snprintf(bitrateStr, sizeof(bitrateStr), "&videoBitrate=%d", bitrate);
            queryParams += bitrateStr;
            queryParams += "&videoResolution=";
            queryParams += vc.defaultResolution;
            queryParams += "&videoQuality=100";
            queryParams += "&subtitles=none";

            // HLS with MPEG-TS segments, h264+aac - platform-specific limits
            char streamProfileBuf[512];
            snprintf(streamProfileBuf, sizeof(streamProfileBuf),
                "add-transcode-target(type=videoProfile"
                "&context=streaming&protocol=hls"
                "&container=mpegts&videoCodec=h264"
                "&audioCodec=aac)"
                "+add-limitation(scope=videoCodec&scopeName=h264"
                "&type=upperBound&name=video.level&value=%d)"
                "+add-limitation(scope=videoCodec&scopeName=h264"
                "&type=upperBound&name=video.width&value=%d)"
                "+add-limitation(scope=videoCodec&scopeName=h264"
                "&type=upperBound&name=video.height&value=%d)",
                vc.maxVideoLevel, vc.maxVideoWidth, vc.maxVideoHeight);
            profileExtra = streamProfileBuf;
        }

        // Generate a unique session ID for this transcode request
        char sessionBuf[64];
        snprintf(sessionBuf, sizeof(sessionBuf), "vitaplex-%lu-%d",
                 (unsigned long)time(nullptr), (int)(rand() % 10000));
        std::string sessionId = sessionBuf;
        queryParams += "&session=" + sessionId;
        queryParams += "&X-Plex-Token=" + token;

        // Call the /decision endpoint first (required by Plex to set up transcode session)
        const char* transcodeType = isAudio ? "music" : "video";
        std::string decisionUrl = convertToHttpForDownload(
            serverUrl + "/" + transcodeType + "/:/transcode/universal/decision?" + queryParams);

        brls::Logger::info("DownloadsManager: Calling decision endpoint for {}", item.title);

        {
            HttpClient decisionClient;
            HttpRequest decisionReq;
            decisionReq.url = decisionUrl;
            decisionReq.method = "GET";
            addPlexHeaders(decisionReq, token);
            decisionReq.headers["X-Plex-Client-Profile-Name"] = "Generic";
            decisionReq.headers["X-Plex-Client-Profile-Extra"] = profileExtra;
            decisionReq.headers["X-Plex-Session-Identifier"] = sessionId;
            decisionReq.timeout = 30;
            HttpResponse decisionResp = decisionClient.request(decisionReq);

            brls::Logger::info("DownloadsManager: Decision response: {} ({})",
                              decisionResp.statusCode, decisionResp.body.substr(0, 300));

            if (decisionResp.statusCode != 200) {
                brls::Logger::warning("DownloadsManager: Decision returned {}", decisionResp.statusCode);
            }
        }

        // Build the start URL
        if (isAudio) {
            // Audio: HTTP progressive download of mp3
            char startPathBuf[128];
            snprintf(startPathBuf, sizeof(startPathBuf), "/%s/:/transcode/universal/start.mp3?",
                     transcodeType);
            url = convertToHttpForDownload(serverUrl + startPathBuf + queryParams);
        } else {
            // Video: HLS m3u8 playlist - we'll download segments and concatenate
            char startPathBuf[128];
            snprintf(startPathBuf, sizeof(startPathBuf), "/%s/:/transcode/universal/start.m3u8?",
                     transcodeType);
            url = convertToHttpForDownload(serverUrl + startPathBuf + queryParams);
        }
        brls::Logger::info("DownloadsManager: Using transcode URL for download");

        // Give the server time to start the transcode session
        if (!isAudio) {
            brls::Logger::info("DownloadsManager: Waiting for transcode to start...");
#ifdef __vita__
            sceKernelDelayThread(3 * 1000 * 1000);  // 3 seconds
#else
            std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
        }
    }

    // Transcoding done (or skipped for audio), now downloading the file
    item.state = DownloadState::DOWNLOADING;

    // Build Plex identification headers
    const auto& vc = platform::getVideoConstraints();
    std::map<std::string, std::string> dlHeaders;
    dlHeaders["X-Plex-Client-Identifier"] = PLEX_CLIENT_NAME;
    dlHeaders["X-Plex-Product"] = PLEX_CLIENT_NAME;
    dlHeaders["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    dlHeaders["X-Plex-Platform"] = vc.plexPlatform;
    dlHeaders["X-Plex-Device"] = vc.plexDevice;
    dlHeaders["X-Plex-Device-Name"] = vc.plexDevice;
    dlHeaders["X-Plex-Token"] = token;
    if (!profileExtra.empty()) {
        dlHeaders["X-Plex-Client-Profile-Name"] = "Generic";
        dlHeaders["X-Plex-Client-Profile-Extra"] = profileExtra;
    }

    bool success = false;

    // Check if this is an HLS download (video transcode) - need to download segments
    bool isHlsDownload = !isAudio && !urlReady &&
                         url.find("start.m3u8") != std::string::npos;

    if (isHlsDownload) {
        // HLS download: fetch m3u8 playlist, then download each TS segment
        brls::Logger::info("DownloadsManager: Starting HLS segment download for {}", item.title);

        // The output file will be .ts (MPEG-TS container) since we concatenate TS segments
        // Update localPath extension to .ts if it was .mp4
        if (item.localPath.size() > 4 && item.localPath.substr(item.localPath.size() - 4) == ".mp4") {
            item.localPath = item.localPath.substr(0, item.localPath.size() - 4) + ".ts";
        }

        // Fetch the m3u8 master playlist
        HttpClient playlistHttp;
        std::string playlistBody;
        bool gotPlaylist = playlistHttp.get(url, playlistBody, dlHeaders);

        if (!gotPlaylist || playlistBody.empty()) {
            brls::Logger::error("DownloadsManager: Failed to fetch m3u8 playlist");
            item.state = DownloadState::FAILED;
            saveState();
            return;
        }

        brls::Logger::debug("DownloadsManager: m3u8 playlist ({} bytes): {}",
                           playlistBody.size(), playlistBody.substr(0, 500));

        // Parse segment URLs from the m3u8 playlist.
        // The playlist may be a master playlist (contains stream variants) or a media
        // playlist (contains segment URLs directly). We handle both cases.
        std::vector<std::string> segmentUrls;

        // Extract base URL for resolving relative segment paths
        std::string baseUrl = url;
        size_t lastSlash = baseUrl.rfind('/');
        if (lastSlash != std::string::npos) {
            baseUrl = baseUrl.substr(0, lastSlash + 1);
        }

        // Extract server base URL for absolute paths
        std::string serverBaseUrl;
        {
            // Find the third slash (after http://host:port/)
            size_t schemeEnd = url.find("://");
            if (schemeEnd != std::string::npos) {
                size_t hostEnd = url.find('/', schemeEnd + 3);
                if (hostEnd != std::string::npos) {
                    serverBaseUrl = url.substr(0, hostEnd);
                }
            }
        }

        // Check if this is a master playlist (contains #EXT-X-STREAM-INF)
        if (playlistBody.find("#EXT-X-STREAM-INF") != std::string::npos) {
            // Master playlist - find the first variant stream URL
            std::string variantUrl;
            std::istringstream masterStream(playlistBody);
            std::string line;
            bool nextLineIsUrl = false;
            while (std::getline(masterStream, line)) {
                // Trim carriage return
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                if (nextLineIsUrl && line[0] != '#') {
                    variantUrl = line;
                    break;
                }
                if (line.find("#EXT-X-STREAM-INF") != std::string::npos) {
                    nextLineIsUrl = true;
                }
            }

            if (variantUrl.empty()) {
                brls::Logger::error("DownloadsManager: No variant stream found in master playlist");
                item.state = DownloadState::FAILED;
                saveState();
                return;
            }

            // Resolve variant URL
            if (variantUrl[0] == '/') {
                variantUrl = serverBaseUrl + variantUrl;
            } else if (variantUrl.find("://") == std::string::npos) {
                variantUrl = baseUrl + variantUrl;
            }

            // Add token if not present
            if (variantUrl.find("X-Plex-Token") == std::string::npos) {
                variantUrl += (variantUrl.find('?') != std::string::npos ? "&" : "?");
                variantUrl += "X-Plex-Token=" + token;
            }

            brls::Logger::info("DownloadsManager: Fetching variant playlist: {}",
                              variantUrl.substr(0, 120));

            // Fetch the variant/media playlist
            HttpClient variantHttp;
            std::string variantBody;
            bool gotVariant = variantHttp.get(variantUrl, variantBody, dlHeaders);

            if (!gotVariant || variantBody.empty()) {
                brls::Logger::error("DownloadsManager: Failed to fetch variant playlist");
                item.state = DownloadState::FAILED;
                saveState();
                return;
            }

            // Update base URL for segment resolution
            lastSlash = variantUrl.rfind('/');
            if (lastSlash != std::string::npos) {
                baseUrl = variantUrl.substr(0, lastSlash + 1);
            }
            playlistBody = variantBody;
        }

        // Parse media playlist for segment URLs
        {
            std::istringstream segStream(playlistBody);
            std::string line;
            while (std::getline(segStream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;

                // This is a segment URL
                std::string segUrl = line;
                if (!segUrl.empty() && segUrl[0] == '/') {
                    segUrl = serverBaseUrl + segUrl;
                } else if (segUrl.find("://") == std::string::npos) {
                    segUrl = baseUrl + segUrl;
                }
                // Add token if not present
                if (segUrl.find("X-Plex-Token") == std::string::npos) {
                    segUrl += (segUrl.find('?') != std::string::npos ? "&" : "?");
                    segUrl += "X-Plex-Token=" + token;
                }
                segmentUrls.push_back(segUrl);
            }
        }

        if (segmentUrls.empty()) {
            // Playlist may not have all segments yet (live-style HLS).
            // Wait and re-fetch until we get #EXT-X-ENDLIST or segments appear.
            brls::Logger::warning("DownloadsManager: No segments in initial playlist, will poll for segments");
        }

        brls::Logger::info("DownloadsManager: Found {} initial segments to download", segmentUrls.size());

        // Open output file
#ifdef __vita__
        SceUID fd = sceIoOpen(item.localPath.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd < 0) {
            brls::Logger::error("DownloadsManager: Failed to create file {}", item.localPath);
            item.state = DownloadState::FAILED;
            saveState();
            return;
        }
#else
        std::ofstream file(item.localPath, std::ios::binary);
        if (!file.is_open()) {
            brls::Logger::error("DownloadsManager: Failed to create file {}", item.localPath);
            item.state = DownloadState::FAILED;
            saveState();
            return;
        }
#endif

        // HLS concatenates fresh TS segments into the truncated file above — it
        // can't resume a partial, so reset the byte counter (validateDownloaded-
        // Files / resumeIncompleteDownloads may have seeded it from a leftover
        // .ts on disk for the resumable paths).
        item.downloadedBytes = 0;

        // Download segments. For live-style HLS playlists (no #EXT-X-ENDLIST),
        // we poll for new segments until the playlist is finalized.
        int totalSegments = 0;
        int downloadedSegments = 0;
        bool playlistFinished = (playlistBody.find("#EXT-X-ENDLIST") != std::string::npos);
        size_t nextSegmentIndex = 0;
        std::string lastSegmentUrl;  // Track last downloaded to avoid duplicates
        success = true;

        while (m_downloading.load() && item.state != DownloadState::CANCELLED) {
            // Download any pending segments
            while (nextSegmentIndex < segmentUrls.size() && m_downloading.load() &&
                   item.state != DownloadState::CANCELLED) {
                const std::string& segUrl = segmentUrls[nextSegmentIndex];

                brls::Logger::debug("DownloadsManager: Downloading segment {}", nextSegmentIndex + 1);

                HttpClient segHttp;
                bool segSuccess = segHttp.downloadFile(segUrl,
                    [&](const char* data, size_t size) {
#ifdef __vita__
                        int written = sceIoWrite(fd, data, size);
                        if (written < 0 || (size_t)written != size) {
                            brls::Logger::error("DownloadsManager: Write failed, wrote {}/{}", written, size);
                            return false;
                        }
#else
                        file.write(data, size);
                        if (!file.good()) {
                            brls::Logger::error("DownloadsManager: Write failed (disk full?)");
                            return false;
                        }
#endif
                        item.downloadedBytes += size;

                        auto cb = m_progressCallback;
                        if (cb) {
                            cb(item.downloadedBytes, item.totalBytes);
                        }
                        return m_downloading.load() && item.state != DownloadState::CANCELLED;
                    },
                    [&](int64_t total) {
                        // Estimate total from segment sizes - only set once to keep Y stable
                        if (item.totalBytes <= 0 && total > 0 && segmentUrls.size() > 0) {
                            item.totalBytes = total * (int64_t)segmentUrls.size();
                        }
                    },
                    dlHeaders
                );

                if (!segSuccess) {
                    brls::Logger::error("DownloadsManager: Failed to download segment {}",
                                       nextSegmentIndex + 1);
                    success = false;
                    break;
                }

                lastSegmentUrl = segUrl;
                downloadedSegments++;
                nextSegmentIndex++;

                brls::Logger::debug("DownloadsManager: Segment {}/{} downloaded ({} bytes total)",
                                   downloadedSegments,
                                   playlistFinished ? (int)segmentUrls.size() : -1,
                                   item.downloadedBytes);
            }

            if (!success || !m_downloading.load() || item.state == DownloadState::CANCELLED) {
                break;
            }

            // If playlist is finished and all segments downloaded, we're done
            if (playlistFinished && nextSegmentIndex >= segmentUrls.size()) {
                brls::Logger::info("DownloadsManager: All {} segments downloaded", downloadedSegments);
                break;
            }

            // Playlist not finished yet - wait and re-fetch for new segments
#ifdef __vita__
            sceKernelDelayThread(2 * 1000 * 1000);  // 2 seconds
#else
            std::this_thread::sleep_for(std::chrono::seconds(2));
#endif

            // Re-fetch the media playlist (use the variant URL if we had a master playlist)
            HttpClient refreshHttp;
            std::string refreshBody;
            bool gotRefresh = refreshHttp.get(url, refreshBody, dlHeaders);

            if (!gotRefresh || refreshBody.empty()) {
                brls::Logger::warning("DownloadsManager: Failed to refresh playlist, retrying...");
                continue;
            }

            // Re-check if this is a master playlist and get the media playlist
            if (refreshBody.find("#EXT-X-STREAM-INF") != std::string::npos) {
                // Master playlist - re-extract variant URL (same logic as above)
                std::istringstream masterStream(refreshBody);
                std::string variantUrl2;
                std::string line;
                bool nextLineIsUrl = false;
                while (std::getline(masterStream, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    if (nextLineIsUrl && line[0] != '#') {
                        variantUrl2 = line;
                        break;
                    }
                    if (line.find("#EXT-X-STREAM-INF") != std::string::npos) {
                        nextLineIsUrl = true;
                    }
                }
                if (!variantUrl2.empty()) {
                    if (variantUrl2[0] == '/') variantUrl2 = serverBaseUrl + variantUrl2;
                    else if (variantUrl2.find("://") == std::string::npos)
                        variantUrl2 = baseUrl + variantUrl2;
                    if (variantUrl2.find("X-Plex-Token") == std::string::npos) {
                        variantUrl2 += (variantUrl2.find('?') != std::string::npos ? "&" : "?");
                        variantUrl2 += "X-Plex-Token=" + token;
                    }
                    HttpClient vHttp;
                    std::string vBody;
                    if (vHttp.get(variantUrl2, vBody, dlHeaders)) {
                        refreshBody = vBody;
                    }
                }
            }

            // Parse new segments
            playlistFinished = (refreshBody.find("#EXT-X-ENDLIST") != std::string::npos);
            std::vector<std::string> newSegUrls;
            {
                std::string refreshBase = baseUrl;
                std::istringstream segStream(refreshBody);
                std::string line;
                while (std::getline(segStream, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty() || line[0] == '#') continue;

                    std::string segUrl = line;
                    if (!segUrl.empty() && segUrl[0] == '/') segUrl = serverBaseUrl + segUrl;
                    else if (segUrl.find("://") == std::string::npos) segUrl = refreshBase + segUrl;
                    if (segUrl.find("X-Plex-Token") == std::string::npos) {
                        segUrl += (segUrl.find('?') != std::string::npos ? "&" : "?");
                        segUrl += "X-Plex-Token=" + token;
                    }
                    newSegUrls.push_back(segUrl);
                }
            }

            // Add only truly new segments (not already in our list)
            for (const auto& newSeg : newSegUrls) {
                bool found = false;
                for (const auto& existing : segmentUrls) {
                    if (existing == newSeg) { found = true; break; }
                }
                if (!found) {
                    segmentUrls.push_back(newSeg);
                }
            }

            brls::Logger::debug("DownloadsManager: Playlist refresh: {} total segments, finished={}",
                               segmentUrls.size(), playlistFinished);
        }

#ifdef __vita__
        sceIoClose(fd);
#else
        file.close();
#endif

    } else {
        // Non-HLS download (audio direct download, or Download Queue API /media
        // URL). Resume support: if a partial file already exists, ask the server
        // to continue from that byte via HTTP Range. The Download Queue /media
        // file and direct ?download=1 files both honour it (206 Partial Content);
        // a server that doesn't answers 200 and we transparently restart from
        // zero — we never splice mismatched bytes into a half-file.
        const int maxDownloadAttempts = 3;
        for (int attempt = 0; attempt < maxDownloadAttempts && m_downloading.load(); attempt++) {
            if (attempt > 0) {
                int waitSec = attempt * 5;
                brls::Logger::info("DownloadsManager: Download attempt {}/{}, waiting {}s...",
                                  attempt + 1, maxDownloadAttempts, waitSec);
#ifdef __vita__
                sceKernelDelayThread(waitSec * 1000 * 1000);
#else
                std::this_thread::sleep_for(std::chrono::seconds(waitSec));
#endif
            }

            // Trust the bytes actually on disk as the resume point each attempt.
            int64_t resumeOffset = partFileSize(item.localPath);

            PartFileWriter out;
            bool alreadyComplete = false;   // server said 416 → file is whole
            bool openFailed = false;

            // Decide append-vs-truncate the instant the final status is known,
            // before any body byte is delivered.
            auto onStart = [&](int statusCode, int64_t fullSize) {
                if (statusCode == 416) {
                    // Range past end of file — it's already fully downloaded.
                    alreadyComplete = true;
                    if (fullSize > 0) item.totalBytes = fullSize;
                    item.downloadedBytes = resumeOffset;
                    return;
                }
                bool resume = (statusCode == 206) && resumeOffset > 0;
                // Guard: a 206 whose full size disagrees with the size we recorded
                // earlier means the server's file changed — restart clean rather
                // than append onto stale bytes.
                if (resume && fullSize > 0 && item.totalBytes > 0 && fullSize != item.totalBytes) {
                    brls::Logger::warning("DownloadsManager: size changed ({} vs {}), restarting {}",
                                          fullSize, item.totalBytes, item.title);
                    resume = false;
                }
                if (fullSize > 0) item.totalBytes = fullSize;
                item.downloadedBytes = resume ? resumeOffset : 0;
                if (!out.open(item.localPath, /*append=*/resume)) {
                    openFailed = true;
                    brls::Logger::error("DownloadsManager: Failed to open file {}", item.localPath);
                } else if (resume) {
                    brls::Logger::info("DownloadsManager: Resuming {} from {} bytes",
                                       item.title, resumeOffset);
                }
            };

            brls::Logger::debug("DownloadsManager: Downloading from {} (resume offset {})",
                                url, resumeOffset);

            HttpClient http;
            success = http.downloadFile(url,
                [&](const char* data, size_t size) {
                    if (alreadyComplete) return false;   // 416: ignore any error body
                    if (openFailed) return false;
                    // Safety net: if the status path never opened a file, start fresh.
                    if (!out.isOpen()) {
                        item.downloadedBytes = 0;
                        if (!out.open(item.localPath, /*append=*/false)) { openFailed = true; return false; }
                    }
                    if (!out.write(data, size)) {
                        brls::Logger::error("DownloadsManager: Write failed (disk full?)");
                        return false;
                    }
                    item.downloadedBytes += size;
                    auto cb = m_progressCallback;
                    if (cb) cb(item.downloadedBytes, item.totalBytes);
                    return m_downloading.load() && item.state != DownloadState::CANCELLED;
                },
                /*sizeCallback*/ nullptr,   // full size comes from startCallback (correct for 206 + 200)
                dlHeaders,
                resumeOffset,
                onStart
            );
            out.close();

            if (alreadyComplete) { success = true; break; }
            if (openFailed) { item.state = DownloadState::FAILED; saveState(); return; }

            if (success || !m_downloading.load() || item.state == DownloadState::CANCELLED) {
                break;
            }

            // Made progress this attempt? Keep the partial and resume next time
            // (next attempt, or a later launch) rather than throwing it away.
            int64_t now = partFileSize(item.localPath);
            if (now > resumeOffset) {
                brls::Logger::info("DownloadsManager: Got {} bytes before failing, will resume", now);
                break;
            }

            brls::Logger::warning("DownloadsManager: Download made no progress (attempt {}/{})",
                                 attempt + 1, maxDownloadAttempts);
        }
    }

    // Don't overwrite CANCELLED state - it was already handled
    if (item.state == DownloadState::CANCELLED) {
        brls::Logger::info("DownloadsManager: Download of {} was cancelled", item.title);
    } else if (success && m_downloading.load()) {
        item.state = DownloadState::COMPLETED;
        brls::Logger::info("DownloadsManager: Completed download of {}", item.title);

        // Download cover art for music tracks
        if (!item.thumbUrl.empty() && !item.thumbPath.empty()) {
            downloadCoverArt(item);
        }
    } else if (!m_downloading.load()) {
        item.state = DownloadState::PAUSED;
        brls::Logger::info("DownloadsManager: Paused download of {}", item.title);
    } else {
        item.state = DownloadState::FAILED;
        // Keep whatever bytes reached disk so a retry / next launch resumes via
        // Range instead of starting over. Only a 0-byte stub is removed so it's
        // never mistaken for real progress.
        int64_t onDisk = partFileSize(item.localPath);
        if (onDisk == 0) {
#ifdef __vita__
            sceIoRemove(item.localPath.c_str());
#else
            std::remove(item.localPath.c_str());
#endif
        }
        item.downloadedBytes = onDisk;
        brls::Logger::error("DownloadsManager: Failed to download {} (kept {} partial bytes)",
                            item.title, onDisk);
    }

    saveState();
}

void DownloadsManager::downloadCoverArt(DownloadItem& item) {
    PlexClient& client = PlexClient::getInstance();
    std::string serverUrl = client.getServerUrl();
    std::string token = client.getAuthToken();

    if (serverUrl.empty() || token.empty() || item.thumbUrl.empty()) return;

    // Build the thumbnail URL using Plex's photo transcoder for a reasonable size
    std::string thumbDownloadUrl = convertToHttpForDownload(
        serverUrl + "/photo/:/transcode?url="
        + HttpClient::urlEncode(item.thumbUrl)
        + "&width=400&height=400&minSize=1"
        + "&X-Plex-Token=" + token);

    brls::Logger::info("DownloadsManager: Downloading cover art for {}", item.title);

    // Open file for writing
#ifdef __vita__
    SceUID fd = sceIoOpen(item.thumbPath.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        brls::Logger::warning("DownloadsManager: Failed to create cover art file");
        return;
    }
#else
    std::ofstream file(item.thumbPath, std::ios::binary);
    if (!file.is_open()) {
        brls::Logger::warning("DownloadsManager: Failed to create cover art file");
        return;
    }
#endif

    HttpClient http;
    bool success = http.downloadFile(thumbDownloadUrl,
        [&](const char* data, size_t size) {
#ifdef __vita__
            sceIoWrite(fd, data, size);
#else
            file.write(data, size);
#endif
            return true;
        },
        [](int64_t) {}
    );

#ifdef __vita__
    sceIoClose(fd);
#else
    file.close();
#endif

    if (success) {
        brls::Logger::info("DownloadsManager: Cover art saved for {}", item.title);
    } else {
        brls::Logger::warning("DownloadsManager: Failed to download cover art for {}", item.title);
        // Clean up failed file
#ifdef __vita__
        sceIoRemove(item.thumbPath.c_str());
#else
        std::remove(item.thumbPath.c_str());
#endif
        item.thumbPath.clear();
    }
}

bool DownloadsManager::reportTimeline(const DownloadItem& item, const std::string& state) {
    PlexClient& client = PlexClient::getInstance();
    std::string serverUrl = client.getServerUrl();
    std::string token = client.getAuthToken();

    if (serverUrl.empty() || token.empty()) {
        return false;
    }

    // Build timeline URL
    std::stringstream url;
    url << serverUrl << "/:/timeline"
        << "?ratingKey=" << item.ratingKey
        << "&key=/library/metadata/" << item.ratingKey
        << "&identifier=com.plexapp.plugins.library"
        << "&time=" << item.viewOffset
        << "&state=" << state
        << "&duration=" << item.duration
        << "&offline=1"
        << "&X-Plex-Token=" << token;

    HttpClient http;
    std::string response;
    if (http.get(url.str(), response)) {
        brls::Logger::debug("DownloadsManager: Reported timeline for {} ({}ms)",
                           item.title, item.viewOffset);
        return true;
    }

    brls::Logger::error("DownloadsManager: Failed to report timeline for {}", item.title);
    return false;
}

void DownloadsManager::validateDownloadedFiles() {
    // Check that COMPLETED items actually have their files on disk
    for (auto& item : m_downloads) {
        if (item.state == DownloadState::COMPLETED && !item.localPath.empty()) {
            bool exists = false;
#ifdef __vita__
            SceUID fd = sceIoOpen(item.localPath.c_str(), SCE_O_RDONLY, 0);
            if (fd >= 0) {
                exists = true;
                sceIoClose(fd);
            }
#else
            std::ifstream f(item.localPath);
            exists = f.good();
#endif
            if (!exists) {
                brls::Logger::warning("DownloadsManager: File missing for {}, marking as failed",
                                     item.title);
                item.state = DownloadState::FAILED;
                item.downloadedBytes = 0;
            }
        }

        // App was interrupted mid-download. Re-queue, but KEEP the partial file
        // and set the resume point to whatever actually reached disk (a hard
        // crash can leave the counter ahead of the flushed bytes) so the next
        // run continues via HTTP Range instead of starting over.
        if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) {
            item.state = DownloadState::QUEUED;
            item.downloadedBytes = partFileSize(item.localPath);
        }
    }
}

void DownloadsManager::saveState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    saveStateUnlocked();
}

void DownloadsManager::saveStateUnlocked() {
    std::stringstream ss;
    ss << "{\n\"downloads\":[\n";

    bool first = true;
    for (size_t i = 0; i < m_downloads.size(); ++i) {
        const auto& item = m_downloads[i];
        // Skip cancelled items - don't persist them
        if (item.state == DownloadState::CANCELLED) continue;
        if (!first) ss << ",\n";
        first = false;
        ss << "{\n"
           << "\"ratingKey\":\"" << escapeJson(item.ratingKey) << "\",\n"
           << "\"title\":\"" << escapeJson(item.title) << "\",\n"
           << "\"partPath\":\"" << escapeJson(item.partPath) << "\",\n"
           << "\"localPath\":\"" << escapeJson(item.localPath) << "\",\n"
           << "\"totalBytes\":" << item.totalBytes << ",\n"
           << "\"downloadedBytes\":" << item.downloadedBytes << ",\n"
           << "\"duration\":" << item.duration << ",\n"
           << "\"viewOffset\":" << item.viewOffset << ",\n"
           << "\"state\":" << static_cast<int>(item.state) << ",\n"
           << "\"mediaType\":\"" << escapeJson(item.mediaType) << "\",\n"
           << "\"parentTitle\":\"" << escapeJson(item.parentTitle) << "\",\n"
           << "\"seasonNum\":" << item.seasonNum << ",\n"
           << "\"episodeNum\":" << item.episodeNum << ",\n"
           << "\"thumbUrl\":\"" << escapeJson(item.thumbUrl) << "\",\n"
           << "\"thumbPath\":\"" << escapeJson(item.thumbPath) << "\",\n"
           << "\"lastSynced\":" << item.lastSynced << ",\n"
           << "\"groupType\":" << static_cast<int>(item.groupType) << ",\n"
           << "\"groupKey\":\"" << escapeJson(item.groupKey) << "\",\n"
           << "\"groupTitle\":\"" << escapeJson(item.groupTitle) << "\",\n"
           << "\"groupThumb\":\"" << escapeJson(item.groupThumb) << "\",\n"
           << "\"albumTitle\":\"" << escapeJson(item.albumTitle) << "\",\n"
           << "\"groupTotalItems\":" << item.groupTotalItems << "\n"
           << "}";
    }

    ss << "]\n}";

#ifdef __vita__
    SceUID fd = sceIoOpen(STATE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        std::string data = ss.str();
        sceIoWrite(fd, data.c_str(), data.size());
        sceIoClose(fd);
    }
#else
    const std::string statePath = m_downloadsPath + "/state.json";
    std::ofstream file(statePath);
    if (file.is_open()) {
        file << ss.str();
        file.close();
    }
#endif

    brls::Logger::debug("DownloadsManager: Saved state ({} items)", m_downloads.size());
}

void DownloadsManager::loadState() {
    std::string content;

#ifdef __vita__
    SceUID fd = sceIoOpen(STATE_FILE, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buffer[4096];
        int read;
        while ((read = sceIoRead(fd, buffer, sizeof(buffer))) > 0) {
            content.append(buffer, read);
        }
        sceIoClose(fd);
    }
#else
    const std::string statePath = m_downloadsPath + "/state.json";
    std::ifstream file(statePath);
    if (file.is_open()) {
        std::stringstream ss;
        ss << file.rdbuf();
        content = ss.str();
        file.close();
    }
#endif

    if (content.empty()) {
        brls::Logger::debug("DownloadsManager: No saved state found");
        return;
    }

    brls::Logger::info("DownloadsManager: Loading saved state...");

    // Parse the JSON array of download items
    // Find the "downloads" array
    size_t arrStart = content.find("[");
    if (arrStart == std::string::npos) {
        brls::Logger::error("DownloadsManager: Invalid state file format");
        return;
    }

    // Find each object in the array
    size_t pos = arrStart;
    while (pos < content.size()) {
        size_t objStart = content.find("{", pos + 1);
        if (objStart == std::string::npos) break;

        // Find matching closing brace (handle nested braces)
        int depth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < content.size() && depth > 0) {
            if (content[objEnd] == '{') depth++;
            else if (content[objEnd] == '}') depth--;
            if (depth > 0) objEnd++;
        }

        if (depth != 0) break;

        std::string objStr = content.substr(objStart, objEnd - objStart + 1);

        DownloadItem item;
        item.ratingKey = extractJsonString(objStr, "ratingKey");
        item.title = extractJsonString(objStr, "title");
        item.partPath = extractJsonString(objStr, "partPath");
        item.localPath = extractJsonString(objStr, "localPath");
        item.totalBytes = extractJsonInt(objStr, "totalBytes");
        item.downloadedBytes = extractJsonInt(objStr, "downloadedBytes");
        item.duration = extractJsonInt(objStr, "duration");
        item.viewOffset = extractJsonInt(objStr, "viewOffset");
        item.state = static_cast<DownloadState>(extractJsonInt(objStr, "state"));
        item.mediaType = extractJsonString(objStr, "mediaType");
        item.parentTitle = extractJsonString(objStr, "parentTitle");
        item.seasonNum = static_cast<int>(extractJsonInt(objStr, "seasonNum"));
        item.episodeNum = static_cast<int>(extractJsonInt(objStr, "episodeNum"));
        item.thumbUrl = extractJsonString(objStr, "thumbUrl");
        item.thumbPath = extractJsonString(objStr, "thumbPath");
        item.lastSynced = static_cast<time_t>(extractJsonInt(objStr, "lastSynced"));
        item.groupType = static_cast<DownloadGroupType>(extractJsonInt(objStr, "groupType"));
        item.groupKey = extractJsonString(objStr, "groupKey");
        item.groupTitle = extractJsonString(objStr, "groupTitle");
        item.groupThumb = extractJsonString(objStr, "groupThumb");
        item.albumTitle = extractJsonString(objStr, "albumTitle");
        item.groupTotalItems = static_cast<int>(extractJsonInt(objStr, "groupTotalItems"));

        if (!item.ratingKey.empty()) {
            m_downloads.push_back(item);
            brls::Logger::debug("DownloadsManager: Loaded item: {} (state={})",
                               item.title, static_cast<int>(item.state));
        }

        pos = objEnd;
    }

    brls::Logger::info("DownloadsManager: Loaded {} items from state file", m_downloads.size());
}

void DownloadsManager::setProgressCallback(DownloadProgressCallback callback) {
    m_progressCallback = callback;
}

std::string DownloadsManager::getDownloadsPath() const {
    return m_downloadsPath;
}

} // namespace vitaplex
