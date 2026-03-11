/**
 * VitaPlex - Asynchronous Image Loader implementation
 */

#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include <fstream>
#include <vector>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace vitaplex {

std::map<std::string, std::vector<uint8_t>> ImageLoader::s_cache;
std::mutex ImageLoader::s_cacheMutex;
std::atomic<uint64_t> ImageLoader::s_generation{0};
std::atomic<bool> ImageLoader::s_paused{false};

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
            // Load from cache (we're on the main thread, target is valid right now)
            target->setImageFromMem(it->second.data(), it->second.size());
            if (callback) callback(target);
            return;
        }
    }

    // Load asynchronously
    brls::async([url, callback, target, alive, gen]() {
        // Check if cancelled or paused before making the HTTP request
        if (!alive->load() || gen != s_generation.load() || s_paused.load()) return;

        HttpClient client;
        HttpResponse resp = client.get(url);

        if (resp.success && !resp.body.empty()) {
            // Cache the image data
            std::vector<uint8_t> imageData(resp.body.begin(), resp.body.end());

            {
                std::lock_guard<std::mutex> lock(s_cacheMutex);
                // Limit cache size - keep low to preserve memory on Vita (256MB total)
                if (s_cache.size() > 30) {
                    s_cache.clear();
                }
                s_cache[url] = imageData;
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

bool ImageLoader::loadFromFile(const std::string& path, brls::Image* target) {
    if (path.empty() || !target) return false;

#ifdef __vita__
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;

    // Get file size
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    if (size <= 0 || size > 4 * 1024 * 1024) {  // Max 4MB for cover art
        sceIoClose(fd);
        return false;
    }

    std::vector<uint8_t> data(size);
    int read = sceIoRead(fd, data.data(), size);
    sceIoClose(fd);

    if (read != size) return false;
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto size = file.tellg();
    if (size <= 0 || size > 4 * 1024 * 1024) return false;

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    file.close();
#endif

    target->setImageFromMem(data.data(), data.size());
    return true;
}

void ImageLoader::clearCache() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cache.clear();
}

void ImageLoader::cancelAll() {
    // Increment generation counter - all in-flight downloads will see a stale
    // generation and skip their brls::sync callbacks, preventing use-after-free
    s_generation.fetch_add(1);
}

} // namespace vitaplex
