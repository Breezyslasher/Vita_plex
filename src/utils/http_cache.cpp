/**
 * Disk-backed HTTP response cache implementation.
 */

#include "utils/http_cache.hpp"
#include "platform/paths.hpp"

#include <borealis.hpp>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace vitaplex {

// FNV-1a 64-bit. Plenty wide for the few hundred URLs we'll ever
// cache; collision probability is astronomical. Hex-encoded so the
// filename is filesystem-safe on every platform we ship.
static std::string fnv1aHex(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

std::string HttpCache::cacheDir() {
    return platformPath("cache");
}

std::string HttpCache::pathFor(const std::string& url) {
    return cacheDir() + "/" + fnv1aHex(url);
}

bool HttpCache::get(const std::string& url, int ttlSeconds, std::string& outBody) {
    if (ttlSeconds <= 0) return false;

    const std::string path = pathFor(url);
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    int64_t ts = 0;
    f.read(reinterpret_cast<char*>(&ts), sizeof(ts));
    if (!f) return false;

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (now - ts > ttlSeconds) {
        // Expired — leave the file in place; the next put() for this
        // URL will overwrite it. Cleaning up here would require a
        // second filesystem call for every miss.
        return false;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    outBody = ss.str();
    brls::Logger::debug("HttpCache HIT ({} bytes, age={}s): {}",
                        outBody.size(), now - ts, url);
    return true;
}

void HttpCache::put(const std::string& url, const std::string& body,
                    int ttlSecondsForGate) {
    if (ttlSecondsForGate <= 0) return;
    if (body.empty()) return;

    const std::string dir = cacheDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        brls::Logger::warning("HttpCache: failed to create {}: {}",
                              dir, ec.message());
        return;
    }

    const std::string path = pathFor(url);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        brls::Logger::warning("HttpCache: failed to open {} for write", path);
        return;
    }

    const int64_t ts = static_cast<int64_t>(std::time(nullptr));
    f.write(reinterpret_cast<const char*>(&ts), sizeof(ts));
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    brls::Logger::debug("HttpCache STORE ({} bytes): {}", body.size(), url);
}

void HttpCache::clear() {
    std::error_code ec;
    std::filesystem::remove_all(cacheDir(), ec);
    if (ec) {
        brls::Logger::warning("HttpCache: clear() failed: {}", ec.message());
    } else {
        brls::Logger::info("HttpCache: cleared");
    }
}

size_t HttpCache::entryCount() {
    std::error_code ec;
    size_t count = 0;
    auto it = std::filesystem::directory_iterator(cacheDir(), ec);
    if (ec) return 0;
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec)) count++;
    }
    return count;
}

size_t HttpCache::totalBytes() {
    std::error_code ec;
    size_t total = 0;
    auto it = std::filesystem::directory_iterator(cacheDir(), ec);
    if (ec) return 0;
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec)) {
            total += static_cast<size_t>(entry.file_size(ec));
        }
    }
    return total;
}

} // namespace vitaplex
