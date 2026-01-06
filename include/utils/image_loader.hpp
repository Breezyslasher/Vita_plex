/**
 * VitaPlex - Asynchronous Image Loader
 */

#pragma once

#include <borealis.hpp>
#include <string>
#include <functional>
#include <map>
#include <mutex>

namespace vitaplex {

class ImageLoader {
public:
    using LoadCallback = std::function<void(brls::Image*)>;

    // Load image asynchronously from URL
    static void loadAsync(const std::string& url, LoadCallback callback, brls::Image* target);

    // Clear image cache
    static void clearCache();

    // Cancel all pending loads
    static void cancelAll();

private:
    static std::map<std::string, std::vector<uint8_t>> s_cache;
    static std::mutex s_cacheMutex;
};

} // namespace vitaplex
