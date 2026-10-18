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
std::vector<ImageLoader::DeferredUpload> ImageLoader::s_deferred;
std::map<brls::Image*, ImageLoader::PendingImage> ImageLoader::s_pendingImages;
uint64_t ImageLoader::s_lastRequest = 0;

// ASYNC_RETAIN's bookkeeping, which as a macro only works inside a View.
ImageLoader::ViewHold::ViewHold(brls::View* view) : m_view(view) {
    if (!view) return;
    if (!view->deletionToken && !view->deletionTokenCounter) {
        view->deletionToken = new bool(false);
        view->deletionTokenCounter = new int(0);
    }
    ++*view->deletionTokenCounter;
    m_deleted = view->deletionToken;
    m_count = view->deletionTokenCounter;
}

// ASYNC_RELEASE's: the last holder frees the token, and detaches it from a
// view that still exists, so that view's next hold starts a fresh one.
void ImageLoader::ViewHold::reset() {
    if (!m_count) return;
    const bool deleted = *m_deleted;
    if (*m_count > 0 && --*m_count == 0) {
        delete m_deleted;
        delete m_count;
        if (!deleted) {
            m_view->deletionToken = nullptr;
            m_view->deletionTokenCounter = nullptr;
        }
    }
    m_view = nullptr;
    m_deleted = nullptr;
    m_count = nullptr;
}

ImageLoader::PendingImage* ImageLoader::currentRequest(brls::Image* target, uint64_t request) {
    auto it = s_pendingImages.find(target);
    if (it == s_pendingImages.end() || it->second.request != request) return nullptr;
    PendingImage& p = it->second;
    if (!p.alive || !p.alive->load() || p.gen != s_generation.load() || !p.view.viewAlive()) {
        s_pendingImages.erase(it);
        return nullptr;
    }
    return &p;
}

void ImageLoader::settle(brls::Image* target, uint64_t request) {
    // Looked up again: the caller's callback may have asked anew for this
    // target, or deleted it.
    PendingImage* p = currentRequest(target, request);
    if (!p) return;
    if (target->getTexture() != 0) {
        s_pendingImages.erase(target);
        return;
    }
    // Decoded to nothing; treat it like a failed request.
    p->waiting = false;
    p->nextRetryAt = brls::getCPUTimeUsec() + 900 * 1000;
}

void ImageLoader::requestFailed(brls::Image* target, uint64_t request) {
    PendingImage* p = currentRequest(target, request);
    if (!p) return;
    p->waiting = false;
    p->nextRetryAt = brls::getCPUTimeUsec() + 900 * 1000;
}

// Defined below; the deferred-upload drain replays cover loads through it.
static void dispatchCoverFromBytes(const std::vector<uint8_t>& bytes,
                                   ImageLoader::CoverCallback callback,
                                   std::shared_ptr<std::atomic<bool>> alive,
                                   uint64_t gen,
                                   std::atomic<uint64_t>& generationRef);

// Diagnostics. A dropped load is invisible to the user except as a permanently
// blank cell: MediaItemCell::setItem() asks for its cover exactly once and never
// retries, so anything that returns early here leaves that cell empty until it
// is recycled and re-bound by scrolling. Every skip path therefore names itself
// in the log — `adb logcat | grep ImageLoader` identifies which gate fired.
// UI-thread only, like the rest of these paths.
static void noteSkip(const char* reason) {
    static std::map<std::string, uint64_t> counts;
    const uint64_t n = ++counts[reason];
    if (n == 1 || n % 25 == 0)
        brls::Logger::warning("ImageLoader: {} load(s) skipped — {}", n, reason);
}

bool ImageLoader::uploadsAreSafe() {
    // Mobile tears the GL drawing surface down while the app is backgrounded,
    // and the main loop deliberately keeps running there (background audio,
    // timers) — so image loads still complete and still land in performSyncTasks
    // with no surface bound. nvgCreateImageMem would then issue glGenTextures /
    // glTexImage2D into the void: the calls are silently discarded and the
    // texture stays empty, which is why thumbnails came back black after
    // backgrounding the app mid-load.
    //
    // canUploadTextures(), not isWindowForeground(): the latter also waits out
    // the post-resume warm-up frames, which gate *drawing*, not upload validity.
    // Always true on desktop/console, where the context survives.
    return brls::Application::canUploadTextures();
}

// One run-loop subscriber serving both recovery paths: replaying uploads that
// were parked with no GL surface, and re-issuing image requests that failed.
// It fires every iteration, right after the sync tasks that would otherwise have
// done the work, so a recovered image appears on the very next frame.
void ImageLoader::ensureRunLoopHook() {
    static bool hooked = false;
    if (hooked) return;
    hooked = true;

    brls::Application::getRunLoopEvent()->subscribe([]() {
        // ── Deferred uploads: parked because there was no drawing surface ──
        if (!s_deferred.empty() && uploadsAreSafe()) {
            std::vector<DeferredUpload> pendingNow;
            pendingNow.swap(s_deferred);
            brls::Logger::info("ImageLoader: replaying {} deferred texture upload(s) after resume",
                               pendingNow.size());
            for (auto& p : pendingNow) {
                // Same liveness rules as the original dispatch: the view may
                // have been destroyed, or cancelAll() may have moved on, while
                // we were backgrounded.
                if (!p.alive || !p.alive->load()) { noteSkip("deferred: target destroyed"); continue; }
                if (p.gen != s_generation.load()) { noteSkip("deferred: cancelAll() raced"); continue; }

                if (p.coverCallback) {
                    dispatchCoverFromBytes(p.data, p.coverCallback, p.alive, p.gen, s_generation);
                } else if (p.target) {
                    if (!currentRequest(p.target, p.request)) {
                        noteSkip("deferred: target destroyed");
                        continue;
                    }
                    p.target->setImageFromMem(p.data.data(), p.data.size());
                    if (p.imageCallback) p.imageCallback(p.target);
                    settle(p.target, p.request);
                }
            }
        }

        // ── Image requests that failed ───────────────────────────────────
        if (s_pendingImages.empty()) return;
        const int64_t now = brls::getCPUTimeUsec();
        constexpr int kMaxRetries = 3;

        for (auto it = s_pendingImages.begin(); it != s_pendingImages.end(); ) {
            PendingImage& p = it->second;
            brls::Image* target = it->first;

            // Gone, superseded, or the target destroyed: stop tracking, without
            // touching the pointer.
            if (!p.alive || !p.alive->load() || p.gen != s_generation.load() ||
                !p.view.viewAlive()) {
                it = s_pendingImages.erase(it);
                continue;
            }
            // Its request is still out, or its upload is parked for a surface;
            // the reply settles it. Re-issuing here used to send every slow
            // thumbnail again after 0.9 s, up to three times: on one search,
            // 550 duplicate requests queued behind the single loader thread in
            // three seconds.
            if (p.waiting) { ++it; continue; }
            if (p.retries >= kMaxRetries) {           // genuinely missing artwork
                it = s_pendingImages.erase(it);
                continue;
            }
            // Hold, don't drop, while a retry could not succeed: loadAsync()
            // returns early when paused, which would erase the entry and leave
            // the view blank for good once playback ends.
            if (s_paused.load() || !uploadsAreSafe() || now < p.nextRetryAt) { ++it; continue; }

            // Re-issue. loadAsync() replaces this entry for the same target, so
            // the iterator is finished with before that happens.
            const std::string url = p.url;
            const LoadCallback cb = p.callback;
            auto alive            = p.alive;
            const int retries     = p.retries + 1;
            it = s_pendingImages.erase(it);

            noteSkip("image request failed — retrying");
            loadAsync(url, cb, target, alive);
            auto again = s_pendingImages.find(target);
            if (again != s_pendingImages.end()) again->second.retries = retries;
        }
    });
}

void ImageLoader::deferUpload(DeferredUpload&& pending) {
    // Not a drop — these are replayed — but knowing whether the surface gate
    // fired at all is the first thing to check when images fail to appear.
    noteSkip("no GL surface — upload deferred for replay");

    ensureRunLoopHook();

    // Bound the queue so a long background stint with a busy grid can't grow it
    // without limit. Evicting is a permanent blank cell, so the cap is generous
    // enough to hold several screenfuls and an eviction is reported.
    constexpr size_t kMaxDeferred = 256;
    if (s_deferred.size() >= kMaxDeferred) {
        noteSkip("deferred queue full, oldest evicted");
        s_deferred.erase(s_deferred.begin());
    }
    s_deferred.push_back(std::move(pending));
}

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

    // Skip new loads while paused (playback in progress). Note this is a
    // permanent drop for the requesting cell, not a delay — see noteSkip().
    if (s_paused.load()) { noteSkip("loader paused (playback active)"); return; }

    // Capture the current generation so stale callbacks are skipped after cancelAll()
    uint64_t gen = s_generation.load();

    // Remember the request. Almost every caller asks once and never retries, so
    // without this a failed load leaves the view blank for as long as it
    // lives. Keyed by target, so a recycled view's newer request replaces this
    // one, and the reply to the older one is then dropped.
    const uint64_t request = ++s_lastRequest;
    {
        PendingImage pending;
        pending.url      = url;
        pending.callback = callback;
        pending.alive    = alive;
        pending.gen      = gen;
        pending.request  = request;
        pending.view     = ViewHold(target);
        s_pendingImages[target] = std::move(pending);
        ensureRunLoopHook();
    }

    // Check cache first. The bytes are copied out: the callback below runs
    // without the lock held, and the decoder may write to its input.
    std::vector<uint8_t> cached;
    bool hit = false;
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_cache.find(url);
        if (it != s_cache.end()) {
            // Promote to front of LRU list (most recently used)
            s_lruOrder.erase(it->second.lruIt);
            s_lruOrder.push_front(url);
            it->second.lruIt = s_lruOrder.begin();
            cached = it->second.data;
            hit = true;
        }
    }
    if (hit) {
        // With no drawing surface the upload would be discarded and the image
        // left blank forever, so park it for replay instead.
        if (!uploadsAreSafe()) {
            DeferredUpload pending;
            pending.data          = std::move(cached);
            pending.imageCallback = callback;
            pending.target        = target;
            pending.request       = request;
            pending.alive         = alive;
            pending.gen           = gen;
            deferUpload(std::move(pending));
            return;
        }
        target->setImageFromMem(cached.data(), cached.size());
        if (callback) callback(target);
        settle(target, request);
        return;
    }

    // Load asynchronously. The worker never touches `target`: it is only the key
    // the reply is filed under, looked up again on the UI thread.
    brls::async([url, callback, target, alive, gen, request]() {
        // Check if cancelled before making the HTTP request. The run-loop hook
        // drops the entry for the same reasons.
        if (!alive->load() || gen != s_generation.load()) return;

        HttpClient client;
        HttpResponse resp = client.get(url);

        if (!resp.success || resp.body.empty()) {
            brls::sync([target, request]() { requestFailed(target, request); });
            return;
        }

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

        // Update UI on main thread, only if this is still the target's request,
        // the caller's alive flag and generation hold, and the image itself
        // still exists.
        brls::sync([imageData, callback, target, alive, gen, request]() {
            if (!currentRequest(target, request)) return;
            // Sync tasks keep running while backgrounded (by design, so audio
            // and timers survive), but the GL surface is gone then — uploading
            // now would silently produce a blank texture.
            if (!uploadsAreSafe()) {
                DeferredUpload pending;
                pending.data          = imageData;
                pending.imageCallback = callback;
                pending.target        = target;
                pending.request       = request;
                pending.alive         = alive;
                pending.gen           = gen;
                deferUpload(std::move(pending));
                return;
            }
            target->setImageFromMem(imageData.data(), imageData.size());
            if (callback) callback(target);
            settle(target, request);
        });
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
    if (s_paused.load()) { noteSkip("loader paused (playback active)"); return; }

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
        // nvgCreateImageMem needs a live drawing surface — park it if we have none.
        if (!uploadsAreSafe()) {
            DeferredUpload pending;
            pending.data          = cachedBytes;
            pending.coverCallback = callback;
            pending.alive         = alive;
            pending.gen           = gen;
            deferUpload(std::move(pending));
            return;
        }
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
            if (!alive->load() || gen != s_generation.load()) return;
            if (!uploadsAreSafe()) {
                DeferredUpload pending;
                pending.data          = imageData;
                pending.coverCallback = callback;
                pending.alive         = alive;
                pending.gen           = gen;
                deferUpload(std::move(pending));
                return;
            }
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
