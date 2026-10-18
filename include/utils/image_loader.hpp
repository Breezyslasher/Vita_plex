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

    // Load image synchronously from a local file path into a brls::Image. Returns true on success.
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
        uint64_t request = 0;         // the target's PendingImage it belongs to
        std::shared_ptr<std::atomic<bool>> alive;
        uint64_t gen = 0;
    };
    static std::vector<DeferredUpload> s_deferred;

    // A hold on a view's own deletion token (borealis' ASYNC_RETAIN): the view
    // sets it when destroyed and the last holder frees it. The caller's alive
    // flag usually covers a whole screen, so it stays true while a list on
    // that screen deletes rows — the player's queue does — and a reply for a
    // deleted row's image would then write into freed memory. This covers the
    // one image. UI thread only, like everything that touches a target.
    class ViewHold {
    public:
        ViewHold() = default;
        explicit ViewHold(brls::View* view);
        ViewHold(ViewHold&& other) noexcept { take(other); }
        ViewHold& operator=(ViewHold&& other) noexcept {
            if (this != &other) { reset(); take(other); }
            return *this;
        }
        ViewHold(const ViewHold&) = delete;
        ViewHold& operator=(const ViewHold&) = delete;
        ~ViewHold() { reset(); }

        bool viewAlive() const { return m_deleted && !*m_deleted; }
        void reset();

    private:
        void take(ViewHold& other) {
            m_view = other.m_view;
            m_deleted = other.m_deleted;
            m_count = other.m_count;
            other.m_view = nullptr;
            other.m_deleted = nullptr;
            other.m_count = nullptr;
        }
        brls::View* m_view = nullptr;
        bool* m_deleted = nullptr;
        int* m_count = nullptr;
    };

    // Outstanding brls::Image loads, one per target, so one that fails can be
    // re-issued: nearly every caller requests its image exactly once, so a
    // failed request would otherwise leave the view blank for good. Keyed by
    // target so a recycled view's newer request replaces the older one; a
    // reply carries its request number and is dropped unless it is still the
    // target's current one, and unless the target still exists. MediaItemCell
    // and the detail-view poster do their own retry from draw(); this covers
    // everything else.
    struct PendingImage {
        std::string url;
        LoadCallback callback;
        std::shared_ptr<std::atomic<bool>> alive;
        uint64_t gen = 0;
        uint64_t request = 0;
        ViewHold view;              // the target's own deletion token
        bool     waiting = true;    // request out, or its upload parked
        int64_t  nextRetryAt = 0;   // CPU usec; set when a request fails
        int      retries = 0;
    };
    static std::map<brls::Image*, PendingImage> s_pendingImages;
    static uint64_t s_lastRequest;

    // A reply for `target`'s request number `request` arrived (UI thread):
    // the entry if it is still current and its target alive, else nullptr
    // (and a dead entry is dropped).
    static PendingImage* currentRequest(brls::Image* target, uint64_t request);
    // After setting the image: done if it took, else retry later.
    static void settle(brls::Image* target, uint64_t request);
    static void requestFailed(brls::Image* target, uint64_t request);

    // Subscribe the run-loop worker that drains deferrals and retries images.
    static void ensureRunLoopHook();

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

    // Max cached images. Platform-driven: ~20 on Vita (tight RAM), ~60 on Switch/Android, ~120 on desktop/PS4.
    static size_t getMaxCacheSize();
};

} // namespace vitaplex
