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
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

namespace vitaplex {

class ImageLoader {
public:
    using LoadCallback = std::function<void(brls::Image*)>;

    // Callback for raw-NVG cover loads. Receives ownership of the NVG
    // image handle — the consumer is responsible for nvgDeleteImage on
    // destruction. If the alive flag has flipped false by the time the
    // callback fires, the loader deletes the handle for you.
    using CoverCallback = std::function<void(int nvgImg, int w, int h)>;

    // Load image asynchronously from URL, using an alive flag to prevent use-after-free.
    // The caller must hold a shared_ptr<std::atomic<bool>> that is set to false when
    // the target brls::Image* is destroyed (e.g. in the cell's destructor).
    static void loadAsync(const std::string& url, LoadCallback callback,
                          brls::Image* target, std::shared_ptr<std::atomic<bool>> alive);

    // Same lifecycle and cache as loadAsync, but returns a raw NVG image
    // handle instead of populating a brls::Image. Used by RecyclingGrid's
    // batched cover-draw pass — skipping brls::Image avoids one View per
    // cell in the borealis tree, the per-cell frame()/drawBackground()
    // calls, and the indirection through the view layout system. Caller
    // owns the returned handle and must nvgDeleteImage in its destructor.
    static void loadCoverAsync(const std::string& url, CoverCallback callback,
                               std::shared_ptr<std::atomic<bool>> alive);

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
    // Texture uploads parked because the app had no GL drawing surface at the
    // moment they completed (mobile background). Creating a texture then is not
    // merely wasted work: the GL calls are silently dropped and the image is
    // left permanently blank, so the upload is replayed instead once a surface
    // is back. Touched only from the UI thread (brls::sync callbacks, cache
    // hits, and the run-loop drain), matching the rest of this class.
    struct DeferredUpload {
        std::vector<uint8_t> data;
        LoadCallback imageCallback;   // set when the target is a brls::Image
        CoverCallback coverCallback;  // set for raw-NVG cover loads
        brls::Image* target = nullptr;
        std::shared_ptr<std::atomic<bool>> alive;
        uint64_t gen = 0;
    };
    static std::vector<DeferredUpload> s_deferred;

    // True when a GL upload issued right now would actually reach the GPU.
    static bool uploadsAreSafe();

    // Park an upload for replay, wiring the run-loop drain on first use.
    static void deferUpload(DeferredUpload&& pending);

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

    // Max cached images. Platform-driven: ~20 on Vita (tight RAM),
    // ~60 on Switch/Android, ~120 on desktop/PS4.
    static size_t getMaxCacheSize();
};

} // namespace vitaplex
