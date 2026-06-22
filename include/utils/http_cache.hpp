/**
 * Disk-backed HTTP response cache.
 *
 * Used by PlexClient to avoid re-fetching the same "list" endpoints
 * (library sections, Live TV channels, Home hubs, ...) every time the
 * user navigates back to a tab. Each cache entry lives in its own file
 * under <platform data dir>/cache/, named by an FNV-1a hash of the
 * request URL so the on-disk layout is independent of URL length /
 * special characters. The file body is the verbatim HTTP response
 * preceded by an 8-byte little-endian timestamp (epoch seconds);
 * get() returns it only when (now - ts) <= ttlSeconds.
 *
 * Settings UI controls a single global TTL via
 * AppSettings::cacheLifetimeMinutes — zero means "cache disabled".
 * Callers should pass that TTL straight into get(); put() is a no-op
 * when the TTL is zero.
 */

#pragma once

#include <string>
#include <cstddef>

namespace vitaplex {

class HttpCache {
public:
    // True on cache hit and within TTL; populates outBody. False on
    // miss, expired entry, disabled cache (ttlSeconds <= 0), or any
    // I/O / parse error.
    static bool get(const std::string& url, int ttlSeconds, std::string& outBody);

    // Persist a response. No-op when ttlSecondsForGate <= 0 so callers
    // can pass the user's TTL setting directly and avoid the disk
    // write entirely when the cache is off.
    static void put(const std::string& url, const std::string& body,
                    int ttlSecondsForGate);

    // Wipe every cache file. Called from Settings "Clear cache" and
    // from logout. Safe to call when the directory doesn't exist.
    static void clear();

    // Stats for the Settings detail line. Walks the cache directory.
    static size_t entryCount();
    static size_t totalBytes();

private:
    static std::string cacheDir();
    static std::string pathFor(const std::string& url);
};

} // namespace vitaplex
