/**
 * VitaPlex - Asynchronous Image Loader implementation
 * Uses LRU eviction to keep memory bounded instead of clearing entire cache.
 */

#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "platform/platform.hpp"
#include <vector>

namespace vitaplex {

std::map<std::string, ImageLoader::CacheEntry> ImageLoader::s_cache;
std::list<std::string> ImageLoader::s_lruOrder;
std::mutex ImageLoader::s_cacheMutex;
std::atomic<uint64_t> ImageLoader::s_generation{0};
std::atomic<bool> ImageLoader::s_paused{false};

size_t ImageLoader::getMaxCacheSize() {
    int v = platform::getImageConstraints().imageCacheSize;
    return v > 0 ? static_cast<size_t>(v) : 20;
}

void ImageLoader::setPaused(bool paused) {
    s_paused.store(paused);
    if (paused) {
        brls::Logger::info("ImageLoader: Paused - new thumbnail loads disabled");
    } else {
        brls::Logger::info("ImageLoader: Resumed - thumbnail loads re-enabled");
    }
}

bool ImageLoader::isPaused() {
    return s_paused.load();
}

size_t ImageLoader::getCacheSize() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    return s_cache.size();
}

void ImageLoader::loadAsync(const std::string& url, LoadCallback callback,
                            brls::Image* target, std::shared_ptr<std::atomic<bool>> alive) {
    if (url.empty() || !target || !alive) return;

    // Skip new loads while paused (playback in progress)
    if (s_paused.load()) return;

    // Capture the current generation so stale callbacks are skipped after cancelAll()
    uint64_t gen = s_generation.load();

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_cache.find(url);
        if (it != s_cache.end()) {
            // Promote to front of LRU list (most recently used)
            s_lruOrder.erase(it->second.lruIt);
            s_lruOrder.push_front(url);
            it->second.lruIt = s_lruOrder.begin();

            // Load from cache (we're on the main thread, target is valid right now)
            target->setImageFromMem(it->second.data.data(), it->second.data.size());
            if (callback) callback(target);
            return;
        }
    }

    // Load asynchronously
    brls::async([url, callback, target, alive, gen]() {
        // Check if cancelled before making the HTTP request.
        if (!alive->load() || gen != s_generation.load()) return;

        HttpClient client;
        HttpResponse resp = client.get(url);

        if (resp.success && !resp.body.empty()) {
            // Cache the image data
            std::vector<uint8_t> imageData(resp.body.begin(), resp.body.end());

            {
                std::lock_guard<std::mutex> lock(s_cacheMutex);

                // LRU eviction: remove oldest entries until we're under the limit
                while (s_cache.size() >= getMaxCacheSize() && !s_lruOrder.empty()) {
                    const std::string& oldest = s_lruOrder.back();
                    s_cache.erase(oldest);
                    s_lruOrder.pop_back();
                }

                // Insert new entry at front of LRU
                s_lruOrder.push_front(url);
                CacheEntry entry;
                entry.data = imageData;
                entry.lruIt = s_lruOrder.begin();
                s_cache[url] = std::move(entry);
            }

            // Update UI on main thread - check alive flag AND generation to prevent
            // use-after-free when the target view has been destroyed
            brls::sync([imageData, callback, target, alive, gen]() {
                if (!alive->load()) return;        // Target was destroyed
                if (gen != s_generation.load()) return;  // cancelAll() was called
                target->setImageFromMem(imageData.data(), imageData.size());
                if (callback) callback(target);
            });
        }
    });
}

// Decode cached bytes into a fresh NVG image and hand it off through the
// caller's CoverCallback. Runs on the UI thread because nvgCreateImageMem
// touches the shared NVG context. If `alive` has flipped false by now the
// caller is gone — discard the would-be handle so we don't leak it.
static void dispatchCoverFromBytes(const std::vector<uint8_t>& bytes,
                                   ImageLoader::CoverCallback callback,
                                   std::shared_ptr<std::atomic<bool>> alive,
                                   uint64_t gen,
                                   std::atomic<uint64_t>& generationRef) {
    if (!alive->load() || gen != generationRef.load()) return;

    NVGcontext* vg = brls::Application::getNVGContext();
    if (!vg) return;

    // nanovg's stb_image decoder mutates the input buffer in place, so we
    // can't hand it the cached `data.data()` directly without risking
    // corrupting the shared cache entry. Copy into a scratch buffer.
    std::vector<unsigned char> scratch(bytes.begin(), bytes.end());
    int nvgImg = nvgCreateImageMem(vg, 0,
                                   scratch.data(),
                                   static_cast<int>(scratch.size()));
    if (nvgImg == 0) return;

    int w = 0, h = 0;
    nvgImageSize(vg, nvgImg, &w, &h);

    if (!alive->load() || gen != generationRef.load()) {
        // Cell died between decode and dispatch — clean up the handle.
        nvgDeleteImage(vg, nvgImg);
        return;
    }
    if (callback) callback(nvgImg, w, h);
}

void ImageLoader::loadCoverAsync(const std::string& url, CoverCallback callback,
                                  std::shared_ptr<std::atomic<bool>> alive) {
    if (url.empty() || !alive) return;
    if (s_paused.load()) return;

    uint64_t gen = s_generation.load();

    // Cache hit: decode synchronously on the calling thread (we're already
    // on the UI thread when cells call this from setItem()). This matches
    // the timing of the loadAsync cache path which calls setImageFromMem
    // inline rather than scheduling another sync. We copy out the cached
    // bytes under the lock, drop the lock, and only then call the
    // user-supplied callback — that way a re-entrant load from inside the
    // callback can still hit the cache without deadlocking.
    std::vector<uint8_t> cachedBytes;
    bool hit = false;
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_cache.find(url);
        if (it != s_cache.end()) {
            s_lruOrder.erase(it->second.lruIt);
            s_lruOrder.push_front(url);
            it->second.lruIt = s_lruOrder.begin();
            cachedBytes = it->second.data;
            hit = true;
        }
    }
    if (hit) {
        dispatchCoverFromBytes(cachedBytes, callback, alive, gen, s_generation);
        return;
    }

    brls::async([url, callback, alive, gen]() {
        if (!alive->load() || gen != s_generation.load()) return;

        HttpClient client;
        HttpResponse resp = client.get(url);
        if (!resp.success || resp.body.empty()) return;

        std::vector<uint8_t> imageData(resp.body.begin(), resp.body.end());
        {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            while (s_cache.size() >= getMaxCacheSize() && !s_lruOrder.empty()) {
                const std::string& oldest = s_lruOrder.back();
                s_cache.erase(oldest);
                s_lruOrder.pop_back();
            }
            s_lruOrder.push_front(url);
            CacheEntry entry;
            entry.data = imageData;
            entry.lruIt = s_lruOrder.begin();
            s_cache[url] = std::move(entry);
        }

        brls::sync([imageData, callback, alive, gen]() {
            dispatchCoverFromBytes(imageData, callback, alive, gen, s_generation);
        });
    });
}

bool ImageLoader::loadFromFile(const std::string& path, brls::Image* target) {
    if (path.empty() || !target) return false;

    // Cap cover art at 4 MB. Backed by sceIoOpen on Vita and std::ifstream
    // elsewhere — the platform layer hides the difference.
    std::vector<uint8_t> data;
    if (!platform::readLocalFile(path, data, 4 * 1024 * 1024)) return false;

    target->setImageFromMem(data.data(), data.size());
    return true;
}

void ImageLoader::clearCache() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cache.clear();
    s_lruOrder.clear();
}

void ImageLoader::cancelAll() {
    // Increment generation counter - all in-flight downloads will see a stale
    // generation and skip their brls::sync callbacks, preventing use-after-free
    s_generation.fetch_add(1);
}

} // namespace vitaplex
