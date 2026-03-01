/**
 * VitaPlex - Asynchronous Image Loader
 */

#pragma once

#include <borealis.hpp>
#include <string>
#include <functional>
#include <map>
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

    // Clear image cache
    static void clearCache();

    // Cancel all pending loads (invalidates in-flight callbacks via generation counter)
    static void cancelAll();

    // Pause/resume image loading. While paused, new loadAsync calls are no-ops
    // and in-flight async loads skip the HTTP request. Use when entering playback
    // to stop background thumbnail fetches from competing with media streaming.
    static void setPaused(bool paused);
    static bool isPaused();

private:
    static std::map<std::string, std::vector<uint8_t>> s_cache;
    static std::mutex s_cacheMutex;
    static std::atomic<uint64_t> s_generation;
    static std::atomic<bool> s_paused;
};

} // namespace vitaplex
