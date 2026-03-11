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
#include <borealis.hpp>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <cctype>
#include <thread>
#include <chrono>

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
#else
static const char* DOWNLOADS_DIR = "./downloads";
static const char* STATE_FILE = "./downloads/state.json";
#endif

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

    m_downloadsPath = DOWNLOADS_DIR;

#ifdef __vita__
    // Create downloads directory if it doesn't exist
    sceIoMkdir("ux0:data/VitaPlex", 0777);
    sceIoMkdir(DOWNLOADS_DIR, 0777);
#else
    // Create directory on other platforms
    std::system("mkdir -p ./downloads");
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
                                      const std::string& thumbUrl) {
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

    // Generate local path - extract extension from the original file's part path
    // (e.g., /library/parts/19779/1760220985/file.mp4 -> .mp4)
    std::string extension;
    if (!partPath.empty()) {
        size_t dotPos = partPath.rfind('.');
        if (dotPos != std::string::npos) {
            extension = partPath.substr(dotPos);
        }
    }
    if (extension.empty()) {
        extension = (mediaType == "track") ? ".mp3" : ".mp4";
    }
    std::string filename = ratingKey + extension;
    item.localPath = m_downloadsPath + "/" + filename;

    // Generate cover art path for music tracks
    if (mediaType == "track" && !thumbUrl.empty()) {
        item.thumbPath = m_downloadsPath + "/" + ratingKey + "_cover.jpg";
    }

    m_downloads.push_back(item);
    saveStateUnlocked();

    brls::Logger::info("DownloadsManager: Queued {} for download", title);
    return true;
}

void DownloadsManager::startDownloads() {
    if (m_downloading.load()) return;
    m_downloading.store(true);

    brls::Logger::info("DownloadsManager: Starting download queue");

    // Process downloads in background using asyncRun
    asyncRun([this]() {
        m_downloadThreadActive.store(true);
        brls::Logger::info("DownloadsManager: Download thread started");

        while (m_downloading.load()) {
            DownloadItem* nextItem = nullptr;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& item : m_downloads) {
                    if (item.state == DownloadState::QUEUED) {
                        item.state = DownloadState::DOWNLOADING;
                        nextItem = &item;
                        brls::Logger::info("DownloadsManager: Found queued item: {}", item.title);
                        break;
                    }
                }
            }

            if (nextItem && nextItem->state != DownloadState::CANCELLED) {
                brls::Logger::info("DownloadsManager: Starting download of {}", nextItem->title);
                downloadItem(*nextItem);

                // Retry failed downloads up to 5 times with exponential backoff
                // Video transcodes especially need retries as the server may need time to prepare
                int retries = 0;
                int maxRetries = (nextItem->mediaType == "track") ? 3 : 5;
                while (nextItem->state == DownloadState::FAILED && retries < maxRetries && m_downloading.load()) {
                    retries++;
                    int waitSec = retries * 5;  // 5s, 10s, 15s, 20s, 25s
                    brls::Logger::info("DownloadsManager: Retry {}/{} for {} in {}s",
                                      retries, maxRetries, nextItem->title, waitSec);
#ifdef __vita__
                    sceKernelDelayThread(waitSec * 1000 * 1000);
#else
                    std::this_thread::sleep_for(std::chrono::seconds(waitSec));
#endif
                    nextItem->state = DownloadState::DOWNLOADING;
                    nextItem->downloadedBytes = 0;
                    downloadItem(*nextItem);
                }
            } else if (!nextItem) {
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
                item.downloadedBytes = 0;  // Re-download from scratch (transcoded streams aren't resumable)
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
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = "VitaPlex";
    req.headers["X-Plex-Product"] = "VitaPlex";
    req.headers["X-Plex-Version"] = "1.0.0";
    req.headers["X-Plex-Platform"] = "PlayStation Vita";
    req.headers["X-Plex-Device"] = "PS Vita";
    req.headers["X-Plex-Device-Name"] = "PS Vita";
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
// This transcodes video to Vita-compatible resolution (960x544) before downloading.
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

    // Step 2: Add item to the download queue
    std::string addUrl = baseUrl + "/downloadQueue/" + queueId + "/add"
        + "?keys=" + HttpClient::urlEncode("/library/metadata/" + ratingKey)
        + "&X-Plex-Token=" + token;

    HttpRequest addReq;
    addReq.url = addUrl;
    addReq.method = "POST";
    addPlexHeaders(addReq, token);
    addReq.timeout = 30;

    HttpClient addHttp;
    HttpResponse addResp = addHttp.request(addReq);

    if (addResp.statusCode != 200) {
        brls::Logger::warning("DownloadsManager: Failed to add to download queue (HTTP {})", addResp.statusCode);
        return false;
    }

    brls::Logger::info("DownloadsManager: Added to download queue, response: {}",
                      addResp.body.substr(0, 500));

    // Parse item ID from the add response
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

    // Step 3: Get the decision for this download queue item (triggers transcoding)
    HttpClient decHttp;
    HttpRequest decReq;
    decReq.url = baseUrl + "/downloadQueue/" + queueId + "/item/" + itemId
        + "/decision?X-Plex-Token=" + token;
    decReq.method = "GET";
    addPlexHeaders(decReq, token);
    decReq.timeout = 60;
    HttpResponse decResp = decHttp.request(decReq);

    brls::Logger::info("DownloadsManager: Queue decision response: {} ({})",
                      decResp.statusCode, decResp.body.substr(0, 500));

    // Step 4: Poll the individual item status until transcoding is complete.
    // The /media endpoint returns 503 if not ready, so we poll the item status.
    // Poll /downloadQueue/{queueId}/items/{itemId} for item-level status every 3 seconds.
    item.state = DownloadState::TRANSCODING;
    const int maxPollAttempts = 100;  // 100 * 3s = 5 minutes max
    const int pollIntervalMs = 3000;
    const int maxErrorRestarts = 5;   // Max consecutive error restarts before giving up
    int errorRestarts = 0;
    bool mediaReady = false;

    for (int attempt = 0; attempt < maxPollAttempts && downloading.load(); attempt++) {
        // Poll individual item status, not the overall queue
        HttpClient statusHttp;
        HttpRequest statusReq;
        statusReq.url = baseUrl + "/downloadQueue/" + queueId + "/items/" + itemId
            + "?X-Plex-Token=" + token;
        statusReq.method = "GET";
        addPlexHeaders(statusReq, token);
        statusReq.timeout = 15;
        HttpResponse statusResp = statusHttp.request(statusReq);

        if (statusResp.statusCode != 200) {
            brls::Logger::warning("DownloadsManager: Item status check failed (HTTP {})", statusResp.statusCode);
            break;
        }

        std::string status = extractJsonString(statusResp.body, "status");
        brls::Logger::debug("DownloadsManager: Item {} status: {} (attempt {}/{})", itemId, status, attempt + 1, maxPollAttempts);

        if (status == "done" || status == "complete" || status == "ready") {
            mediaReady = true;
            break;
        }

        if (status == "error" || status == "failed") {
            errorRestarts++;
            brls::Logger::error("DownloadsManager: Item transcoding failed with status: {} (error {}/{})",
                               status, errorRestarts, maxErrorRestarts);

            // If the very first poll returns error, the Download Queue API likely
            // doesn't work on this server. Fail fast to fall back to streaming transcode.
            if (attempt == 0) {
                brls::Logger::warning("DownloadsManager: Immediate error on first poll, Download Queue API not supported");
                break;
            }

            if (errorRestarts >= maxErrorRestarts) {
                brls::Logger::error("DownloadsManager: Giving up after {} consecutive transcode errors", errorRestarts);
                break;
            }

            // Try restarting the item processing with exponential backoff
            HttpClient restartHttp;
            HttpRequest restartReq;
            restartReq.url = baseUrl + "/downloadQueue/" + queueId + "/items/" + itemId
                + "/restart?X-Plex-Token=" + token;
            restartReq.method = "POST";
            addPlexHeaders(restartReq, token);
            restartReq.timeout = 30;
            HttpResponse restartResp = restartHttp.request(restartReq);
            brls::Logger::info("DownloadsManager: Restart response: {}", restartResp.statusCode);

            // Exponential backoff: 3s, 6s, 12s, 24s, 48s
            int backoffMs = pollIntervalMs * (1 << (errorRestarts - 1));
            brls::Logger::info("DownloadsManager: Waiting {}ms before next poll", backoffMs);
#ifdef __vita__
            sceKernelDelayThread(backoffMs * 1000);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
#endif
            continue;
        }

        // Status is not an error - reset error counter
        errorRestarts = 0;

        // Still processing ("deciding", "transcoding", etc.) - wait and poll again
#ifdef __vita__
        sceKernelDelayThread(pollIntervalMs * 1000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
#endif

        // Every 30 attempts (~90s), try probing the media endpoint directly
        // Some servers may not update item status but have the media ready
        if (attempt > 0 && attempt % 30 == 0) {
            HttpClient probeHttp;
            HttpRequest probeReq;
            probeReq.url = baseUrl + "/downloadQueue/" + queueId + "/item/" + itemId
                + "/media?X-Plex-Token=" + token;
            probeReq.method = "GET";
            addPlexHeaders(probeReq, token);
            probeReq.timeout = 10;
            // Use a HEAD-like check: just see if we get 200 vs 503
            HttpResponse probeResp = probeHttp.request(probeReq);
            if (probeResp.statusCode == 200 && probeResp.body.size() > 1000) {
                brls::Logger::info("DownloadsManager: Media probe returned 200 with {} bytes, media is ready",
                                  probeResp.body.size());
                mediaReady = true;
                break;
            }
        }
    }

    if (!downloading.load()) {
        brls::Logger::info("DownloadsManager: Download cancelled during transcode wait");
        return false;
    }

    if (!mediaReady) {
        brls::Logger::warning("DownloadsManager: Download Queue API failed, will fall back to streaming transcode");
        return false;
    }

    // Step 5: Build the media download URL
    outUrl = baseUrl + "/downloadQueue/" + queueId + "/item/" + itemId
        + "/media?X-Plex-Token=" + token;

    brls::Logger::info("DownloadsManager: Download Queue media URL ready");
    return true;
}

void DownloadsManager::downloadItem(DownloadItem& item) {
    brls::Logger::info("DownloadsManager: Starting download of {}", item.title);

    PlexClient& client = PlexClient::getInstance();
    std::string serverUrl = client.getServerUrl();
    std::string token = client.getAuthToken();

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

    if (isAudio) {
        // Audio: download the original file directly (small files, Vita-compatible formats)
        url = buildDirectDownloadUrl(serverUrl, token, item.partPath);
        if (!url.empty()) {
            urlReady = true;
            brls::Logger::info("DownloadsManager: Using direct file download for audio: {}", item.title);
        }
    } else {
        // Video: use Download Queue API to transcode to Vita resolution (960x544)
        // This polls until the server finishes transcoding before downloading
        urlReady = tryDownloadQueueApi(serverUrl, token, item.ratingKey, url, m_downloading, item);
        if (urlReady) {
            brls::Logger::info("DownloadsManager: Using Download Queue API for video: {}", item.title);
        } else {
            brls::Logger::info("DownloadsManager: Download Queue API failed, falling back to transcode stream");
        }
    }

    // Fall back to streaming transcode if no URL ready yet
    if (!urlReady) {
        // Build transcode URL for Vita-compatible format
        std::string metadataPath = "/library/metadata/" + item.ratingKey;
        std::string encodedPath = HttpClient::urlEncode(metadataPath);

        // Build query parameters (shared between decision and start endpoints)
        std::string queryParams;
        queryParams += "path=" + encodedPath;
        queryParams += "&mediaIndex=0&partIndex=0";
        queryParams += "&protocol=http";
        queryParams += "&offset=0";
        queryParams += "&directPlay=0&directStream=0";
        queryParams += "&directStreamAudio=1";
        queryParams += "&hasMDE=1";
        queryParams += "&location=lan";
        queryParams += "&audioBoost=100";
        queryParams += "&audioChannelCount=2";

        if (isAudio) {
            queryParams += "&musicBitrate=320";
            profileExtra = "add-transcode-target(type=musicProfile"
                           "&context=streaming&protocol=http"
                           "&container=mp3&audioCodec=mp3)";
        } else {
            AppSettings& settings = Application::getInstance().getSettings();
            int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : 2000;

            char bitrateStr[64];
            snprintf(bitrateStr, sizeof(bitrateStr), "&videoBitrate=%d", bitrate);
            queryParams += bitrateStr;
            queryParams += "&videoResolution=960x544";
            queryParams += "&subtitles=none";

            profileExtra = "add-transcode-target(type=videoProfile"
                           "&context=streaming&protocol=http"
                           "&container=mp4&videoCodec=h264"
                           "&audioCodec=aac)"
                           "+add-limitation(scope=videoCodec&scopeName=h264"
                           "&type=upperBound&name=video.level&value=40)"
                           "+add-limitation(scope=videoCodec&scopeName=h264"
                           "&type=upperBound&name=video.width&value=960)"
                           "+add-limitation(scope=videoCodec&scopeName=h264"
                           "&type=upperBound&name=video.height&value=544)";
        }

        // Generate a session ID for this transcode request
        char sessionBuf[32];
        snprintf(sessionBuf, sizeof(sessionBuf), "%lu", (unsigned long)time(nullptr));
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
            decisionReq.timeout = 30;
            HttpResponse decisionResp = decisionClient.request(decisionReq);

            brls::Logger::info("DownloadsManager: Decision response: {} ({})",
                              decisionResp.statusCode, decisionResp.body.substr(0, 300));

            if (decisionResp.statusCode != 200) {
                brls::Logger::warning("DownloadsManager: Decision returned {}, trying start anyway",
                                     decisionResp.statusCode);
            }
        }

        // Build the /start URL for downloading (transcode params as query, Plex identity as headers per API spec)
        const char* container = isAudio ? "mp3" : "mp4";
        char startPathBuf[128];
        snprintf(startPathBuf, sizeof(startPathBuf), "/%s/:/transcode/universal/start.%s?", transcodeType, container);
        url = convertToHttpForDownload(serverUrl + startPathBuf + queryParams);

        // Give the server time to start the transcode session before we request data.
        // Without this delay, the server may return an empty/partial response because
        // the transcode hasn't begun buffering yet (causes CURLE_PARTIAL_FILE).
        if (!isAudio) {
            brls::Logger::info("DownloadsManager: Waiting for transcode to buffer...");
#ifdef __vita__
            sceKernelDelayThread(3 * 1000 * 1000);  // 3 seconds
#else
            std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
        }
    }

    brls::Logger::debug("DownloadsManager: Downloading from {}", url);

    // Transcoding done (or skipped for audio), now downloading the file
    item.state = DownloadState::DOWNLOADING;

    // Open local file for writing
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

    // Build Plex identification headers required by the transcode API.
    // The API spec requires X-Plex-Client-Identifier, X-Plex-Client-Profile-Extra,
    // and X-Plex-Client-Profile-Name as HTTP headers, not URL query parameters.
    std::map<std::string, std::string> dlHeaders;
    dlHeaders["X-Plex-Client-Identifier"] = "VitaPlex";
    dlHeaders["X-Plex-Product"] = "VitaPlex";
    dlHeaders["X-Plex-Version"] = "1.0.0";
    dlHeaders["X-Plex-Platform"] = "PlayStation Vita";
    dlHeaders["X-Plex-Device"] = "PS Vita";
    dlHeaders["X-Plex-Device-Name"] = "PS Vita";
    dlHeaders["X-Plex-Token"] = token;
    if (!profileExtra.empty()) {
        dlHeaders["X-Plex-Client-Profile-Name"] = "Generic";
        dlHeaders["X-Plex-Client-Profile-Extra"] = profileExtra;
    }

    // Download with progress tracking
    HttpClient http;
    bool success = http.downloadFile(url,
        [&](const char* data, size_t size) {
            // Write chunk to file
#ifdef __vita__
            int written = sceIoWrite(fd, data, size);
            if (written < 0 || (size_t)written != size) {
                brls::Logger::error("DownloadsManager: Write failed (disk full?), wrote {}/{}", written, size);
                return false;  // Cancel download on write error
            }
#else
            file.write(data, size);
            if (!file.good()) {
                brls::Logger::error("DownloadsManager: Write failed (disk full?)");
                return false;  // Cancel download on write error
            }
#endif
            item.downloadedBytes += size;

            // Call progress callback (copy to local to avoid race if cleared by UI thread)
            auto cb = m_progressCallback;
            if (cb) {
                cb(item.downloadedBytes, item.totalBytes);
            }

            // Return false to cancel download
            return m_downloading.load() && item.state != DownloadState::CANCELLED;
        },
        [&](int64_t total) {
            item.totalBytes = total;
            brls::Logger::debug("DownloadsManager: Total size: {} bytes", total);
        },
        dlHeaders
    );

#ifdef __vita__
    sceIoClose(fd);
#else
    file.close();
#endif

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
        brls::Logger::error("DownloadsManager: Failed to download {}", item.title);
        // Delete partial file
#ifdef __vita__
        sceIoRemove(item.localPath.c_str());
#else
        std::remove(item.localPath.c_str());
#endif
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

        // Convert DOWNLOADING/TRANSCODING to QUEUED on startup (app was interrupted)
        if (item.state == DownloadState::DOWNLOADING || item.state == DownloadState::TRANSCODING) {
            item.state = DownloadState::QUEUED;
            item.downloadedBytes = 0;
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
           << "\"lastSynced\":" << item.lastSynced << "\n"
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
    std::ofstream file(STATE_FILE);
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
    std::ifstream file(STATE_FILE);
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
