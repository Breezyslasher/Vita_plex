/**
 * VitaPlex - Asynchronous Image Loader implementation
 */

#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"

namespace vitaplex {

std::map<std::string, std::vector<uint8_t>> ImageLoader::s_cache;
std::mutex ImageLoader::s_cacheMutex;

void ImageLoader::loadAsync(const std::string& url, LoadCallback callback, brls::Image* target) {
    if (url.empty() || !target) return;

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_cache.find(url);
        if (it != s_cache.end()) {
            // Load from cache
            target->setImageFromMem(it->second.data(), it->second.size());
            if (callback) callback(target);
            return;
        }
    }

    // Load asynchronously
    brls::async([url, callback, target]() {
        HttpClient client;
        HttpResponse resp = client.get(url);

        if (resp.success && !resp.body.empty()) {
            // Cache the image data
            std::vector<uint8_t> imageData(resp.body.begin(), resp.body.end());

            {
                std::lock_guard<std::mutex> lock(s_cacheMutex);
                // Limit cache size
                if (s_cache.size() > 100) {
                    s_cache.clear();
                }
                s_cache[url] = imageData;
            }

            // Update UI on main thread
            brls::sync([imageData, callback, target]() {
                target->setImageFromMem(imageData.data(), imageData.size());
                if (callback) callback(target);
            });
        }
    });
}

void ImageLoader::clearCache() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cache.clear();
}

void ImageLoader::cancelAll() {
    // Borealis handles thread cancellation
}

} // namespace vitaplex
