/**
 * VitaPlex - Downloads Manager Implementation
 * Handles offline media downloads and progress sync
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
#include <thread>

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

    m_initialized = true;
    brls::Logger::info("DownloadsManager: Initialized at {}", m_downloadsPath);
    return true;
}

bool DownloadsManager::queueDownload(const std::string& ratingKey, const std::string& title,
                                      const std::string& partPath, int64_t duration,
                                      const std::string& mediaType,
                                      const std::string& parentTitle,
                                      int seasonNum, int episodeNum) {
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
    item.state = DownloadState::QUEUED;

    // Generate local path with appropriate extension for transcoded format
    std::string extension = (mediaType == "track") ? ".mp3" : ".mp4";
    std::string filename = ratingKey + extension;
    item.localPath = m_downloadsPath + "/" + filename;

    m_downloads.push_back(item);
    saveState();

    brls::Logger::info("DownloadsManager: Queued {} for download", title);
    return true;
}

void DownloadsManager::startDownloads() {
    if (m_downloading) return;
    m_downloading = true;

    brls::Logger::info("DownloadsManager: Starting download queue");

    // Process downloads in background using asyncRun
    asyncRun([this]() {
        brls::Logger::info("DownloadsManager: Download thread started");

        while (m_downloading) {
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

            if (nextItem) {
                brls::Logger::info("DownloadsManager: Starting download of {}", nextItem->title);
                downloadItem(*nextItem);
            } else {
                // No more queued items
                brls::Logger::info("DownloadsManager: No more queued items");
                break;
            }
        }
        m_downloading = false;
        brls::Logger::info("DownloadsManager: Download thread finished");
    });
}

void DownloadsManager::pauseDownloads() {
    m_downloading = false;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_downloads) {
        if (item.state == DownloadState::DOWNLOADING) {
            item.state = DownloadState::PAUSED;
        }
    }
    saveState();
}

bool DownloadsManager::cancelDownload(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (it->ratingKey == ratingKey) {
            // Delete partial file if exists
            if (!it->localPath.empty()) {
#ifdef __vita__
                sceIoRemove(it->localPath.c_str());
#else
                std::remove(it->localPath.c_str());
#endif
            }
            m_downloads.erase(it);
            saveState();
            return true;
        }
    }
    return false;
}

bool DownloadsManager::deleteDownload(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (it->ratingKey == ratingKey) {
            // Delete file
            if (!it->localPath.empty()) {
#ifdef __vita__
                sceIoRemove(it->localPath.c_str());
#else
                std::remove(it->localPath.c_str());
#endif
            }
            m_downloads.erase(it);
            saveState();
            brls::Logger::info("DownloadsManager: Deleted download {}", ratingKey);
            return true;
        }
    }
    return false;
}

std::vector<DownloadItem> DownloadsManager::getDownloads() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_downloads;
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
            item.viewOffset = viewOffset;
            brls::Logger::debug("DownloadsManager: Updated progress for {} to {}ms",
                               item.title, viewOffset);
            break;
        }
    }
    // Don't save on every update - too frequent
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

    // Build transcode URL for Vita-compatible format
    // URL-encode the path
    std::string encodedPath = HttpClient::urlEncode(item.partPath);

    std::string url = serverUrl;
    bool isAudio = (item.mediaType == "track");

    if (isAudio) {
        // Audio transcode - convert to MP3 which the Vita can play
        url += "/music/:/transcode/universal/start.mp3?";
        url += "path=" + encodedPath;
        url += "&mediaIndex=0&partIndex=0";
        url += "&protocol=http";
        url += "&directPlay=0&directStream=0";  // Force transcode
        url += "&audioCodec=mp3&audioBitrate=320";
    } else {
        // Video transcode - convert to H.264/AAC which the Vita can decode
        url += "/video/:/transcode/universal/start.mp4?";
        url += "path=" + encodedPath;
        url += "&mediaIndex=0&partIndex=0";
        url += "&protocol=http";
        url += "&fastSeek=1";
        url += "&directPlay=0&directStream=0";  // Force transcode

        // Get quality settings
        AppSettings& settings = Application::getInstance().getSettings();
        int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : 4000;

        char bitrateStr[64];
        snprintf(bitrateStr, sizeof(bitrateStr), "&videoBitrate=%d", bitrate);
        url += bitrateStr;
        url += "&videoCodec=h264";
        url += "&maxWidth=960&maxHeight=544";  // Vita screen resolution
        url += "&audioCodec=aac&audioChannels=2";
    }

    // Add authentication and client identification
    url += "&X-Plex-Token=" + token;
    url += "&X-Plex-Client-Identifier=VitaPlex";
    url += "&X-Plex-Product=VitaPlex";
    url += "&X-Plex-Version=1.0.0";
    url += "&X-Plex-Platform=PlayStation%20Vita";
    url += "&X-Plex-Device=PS%20Vita";

    // Generate a session ID for this transcode request
    char sessionId[32];
    snprintf(sessionId, sizeof(sessionId), "&session=%lu", (unsigned long)time(nullptr));
    url += sessionId;

    brls::Logger::debug("DownloadsManager: Downloading from {}", url);

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

    // Download with progress tracking
    HttpClient http;
    bool success = http.downloadFile(url,
        [&](const char* data, size_t size) {
            // Write chunk to file
#ifdef __vita__
            sceIoWrite(fd, data, size);
#else
            file.write(data, size);
#endif
            item.downloadedBytes += size;

            // Call progress callback
            if (m_progressCallback) {
                m_progressCallback(item.downloadedBytes, item.totalBytes);
            }

            return m_downloading; // Return false to cancel
        },
        [&](int64_t total) {
            item.totalBytes = total;
            brls::Logger::debug("DownloadsManager: Total size: {} bytes", total);
        }
    );

#ifdef __vita__
    sceIoClose(fd);
#else
    file.close();
#endif

    if (success && m_downloading) {
        item.state = DownloadState::COMPLETED;
        brls::Logger::info("DownloadsManager: Completed download of {}", item.title);
    } else if (!m_downloading) {
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

bool DownloadsManager::reportTimeline(const DownloadItem& item, const std::string& state) {
    PlexClient& client = PlexClient::getInstance();
    std::string serverUrl = client.getServerUrl();
    std::string token = client.getAuthToken();

    if (serverUrl.empty() || token.empty()) {
        return false;
    }

    // Build timeline URL
    // GET /:/timeline?ratingKey={key}&key=/library/metadata/{key}&time={ms}&state={state}&duration={ms}&offline=1
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

void DownloadsManager::saveState() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Simple JSON-like format for state
    std::stringstream ss;
    ss << "{\n\"downloads\":[\n";

    for (size_t i = 0; i < m_downloads.size(); ++i) {
        const auto& item = m_downloads[i];
        ss << "{\n"
           << "\"ratingKey\":\"" << item.ratingKey << "\",\n"
           << "\"title\":\"" << item.title << "\",\n"
           << "\"partPath\":\"" << item.partPath << "\",\n"
           << "\"localPath\":\"" << item.localPath << "\",\n"
           << "\"totalBytes\":" << item.totalBytes << ",\n"
           << "\"downloadedBytes\":" << item.downloadedBytes << ",\n"
           << "\"duration\":" << item.duration << ",\n"
           << "\"viewOffset\":" << item.viewOffset << ",\n"
           << "\"state\":" << static_cast<int>(item.state) << ",\n"
           << "\"mediaType\":\"" << item.mediaType << "\",\n"
           << "\"parentTitle\":\"" << item.parentTitle << "\",\n"
           << "\"seasonNum\":" << item.seasonNum << ",\n"
           << "\"episodeNum\":" << item.episodeNum << ",\n"
           << "\"lastSynced\":" << item.lastSynced << "\n"
           << "}";
        if (i < m_downloads.size() - 1) ss << ",";
        ss << "\n";
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

    // Simple parsing - in production use proper JSON parser
    // For now, just log that we would parse
    brls::Logger::info("DownloadsManager: Loading saved state...");

    // TODO: Implement proper JSON parsing for download items
    // For now, start fresh on each launch
}

void DownloadsManager::setProgressCallback(DownloadProgressCallback callback) {
    m_progressCallback = callback;
}

std::string DownloadsManager::getDownloadsPath() const {
    return m_downloadsPath;
}

} // namespace vitaplex
