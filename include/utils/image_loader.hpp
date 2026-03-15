/**
 * VitaPlex - Asynchronous Image Loader
 * Memory-optimized with LRU cache eviction and reduced cache size.
 */

#pragma once

#include <borealis.hpp>
#include <string>
#include <functional>
#include <map>
#include <list>
#include <mutex>
#include <atomic>
#include <memory>

namespace vitaplex {

class ImageLoader {
public:
    using LoadCallback = std::function<void(brls::Image*)>;

    // Load image asynchronously from URL, using an alive flag to prevent use-after-free.
    // The caller must hold a shared_ptr<std::atomic<bool>> that is set to false when
    // the target brls::Image* is destroyed (e.g. in the cell's destructor).
    static void loadAsync(const std::string& url, LoadCallback callback,
                          brls::Image* target, std::shared_ptr<std::atomic<bool>> alive);

    // Load image synchronously from a local file path into a brls::Image.
    // Returns true on success.
    static bool loadFromFile(const std::string& path, brls::Image* target);

    // Clear image cache
    static void clearCache();

    // Cancel all pending loads (invalidates in-flight callbacks via generation counter)
    static void cancelAll();

    // Pause/resume image loading. While paused, new loadAsync calls are no-ops
    // and in-flight async loads skip the HTTP request. Use when entering playback
    // to stop background thumbnail fetches from competing with media streaming.
    static void setPaused(bool paused);
    static bool isPaused();

    // Get current cache size (for debug display)
    static size_t getCacheSize();

private:
    // LRU cache: list stores URL keys in order of recent use (front = most recent)
    // map stores the data + iterator into the list for O(1) promotion
    struct CacheEntry {
        std::vector<uint8_t> data;
        std::list<std::string>::iterator lruIt;
    };

    static std::map<std::string, CacheEntry> s_cache;
    static std::list<std::string> s_lruOrder;
    static std::mutex s_cacheMutex;
    static std::atomic<uint64_t> s_generation;
    static std::atomic<bool> s_paused;

    // Max cached images - reduced from 30 to 20 to save ~2-4 MB on Vita
    static constexpr size_t MAX_CACHE_SIZE = 20;
};

} // namespace vitaplex
