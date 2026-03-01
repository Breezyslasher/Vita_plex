# Plan: Port 7 Performance/Robustness Features from Vita_Suwayomi to VitaPlex

## Context

VitaPlex is a Plex client for PS Vita. Compared to Vita_Suwayomi (a manga reader for the same hardware), VitaPlex is missing several performance optimizations critical for the Vita's constrained environment (256MB RAM, limited VRAM, slow network). These improvements will make library browsing faster, reduce crashes, and improve offline reliability.

---

## Feature 1: LRU Memory Cache + Disk Thumbnail Cache

**Problem**: `ImageLoader` (`src/utils/image_loader.cpp`) uses a simple `std::map` that accumulates entries until it hits 30, then **clears everything** (`s_cache.clear()` at line 66). No disk cache — every thumbnail is re-downloaded every session.

**Changes**:

### `include/utils/image_loader.hpp`
- Replace `std::map<std::string, std::vector<uint8_t>> s_cache` with an LRU cache:
  - `std::list<std::pair<std::string, std::vector<uint8_t>>> s_lruList` — ordered by access time
  - `std::unordered_map<std::string, std::list<...>::iterator> s_lruMap` — O(1) lookup
- Add constants: `MAX_CACHE_ENTRIES = 30`, `MAX_CACHE_BYTES = 25 * 1024 * 1024` (25MB)
- Add `static size_t s_cacheBytes` to track total memory usage
- Add disk cache methods: `static bool loadFromDisk(const std::string& key, std::vector<uint8_t>& data)` and `static void saveToDisk(const std::string& key, const std::vector<uint8_t>& data)`
- Add `static std::string getCachePath(const std::string& url)` — hashes URL to filename

### `src/utils/image_loader.cpp`
- **LRU eviction**: On insert, if `s_cacheBytes > MAX_CACHE_BYTES` or `s_lruMap.size() >= MAX_CACHE_ENTRIES`, evict least-recently-used entries from the back of `s_lruList` until under budget
- **On cache hit**: Move entry to front of `s_lruList` (most recently used)
- **Disk cache path**: `ux0:data/VitaPlex/cache/thumbs/` — use a hash of the URL as filename, store as raw JPEG/PNG bytes (no format conversion needed since borealis handles decoding)
- **Load order**: Check memory LRU → check disk cache → HTTP fetch → store to both disk + memory
- **clearCache()**: Also optionally clear disk cache (add parameter `bool clearDisk = false`)

---

## Feature 2: Texture/VRAM Safety

**Problem**: No guardrails on image dimensions or memory. A large poster image could blow VRAM on the Vita without warning.

**Changes**:

### `include/utils/image_loader.hpp`
- Add constants: `MAX_TEXTURE_DIM = 2048`, `MAX_IMAGE_BYTES = 16 * 1024 * 1024` (16MB VRAM budget per image)

### `src/utils/image_loader.cpp`
- Before calling `target->setImageFromMem()`, validate the decoded image data:
  - Check raw data size against `MAX_IMAGE_BYTES`
  - After decode (or by inspecting JPEG/PNG headers), check dimensions against `MAX_TEXTURE_DIM`
  - If oversized, skip loading and log a warning: `"ImageLoader: Skipping oversized image (%dx%d) for URL: %s"`
- In the HTTP fetch callback, reject responses larger than `MAX_IMAGE_BYTES` before caching
- Add a helper `static bool validateImageSize(const std::vector<uint8_t>& data)` that reads JPEG/PNG headers to extract dimensions without full decode

---

## Feature 3: Texture Upload Batching

**Problem**: When scrolling through a grid of media items, multiple thumbnails can finish downloading simultaneously. Each calls `target->setImageFromMem()` on the main thread, causing GPU texture uploads that compete for frame time, resulting in frame drops.

**Changes**:

### `include/utils/image_loader.hpp`
- Add `static const int MAX_TEXTURES_PER_FRAME = 2`
- Add a pending upload queue: `static std::vector<PendingUpload> s_pendingUploads` where `PendingUpload` holds `{imageData, callback, target, alive, generation}`
- Add `static int s_uploadsThisFrame`
- Add `static void processPendingUploads()` — called once per frame from the main loop
- Add `static void resetFrameCounter()` — called at start of each frame

### `src/utils/image_loader.cpp`
- In the `brls::sync` callback (line 73), instead of calling `setImageFromMem` directly:
  - If `s_uploadsThisFrame < MAX_TEXTURES_PER_FRAME`, do the upload immediately and increment counter
  - Otherwise, push to `s_pendingUploads` queue for next frame
- `processPendingUploads()`: Process up to `MAX_TEXTURES_PER_FRAME` entries from the queue, checking `alive` and `generation` flags before each upload
- `resetFrameCounter()`: Set `s_uploadsThisFrame = 0`

### `src/main.cpp` or `src/app/application.cpp`
- In the main loop / frame callback, call `ImageLoader::resetFrameCounter()` at frame start and `ImageLoader::processPendingUploads()` after UI update

---

## Feature 4: Local Metadata Cache

**Problem**: `HomeTab::loadContent()` (`src/view/home_tab.cpp:146`) makes multiple HTTP calls every launch: `fetchContinueWatching()`, `fetchLibrarySections()`, then `fetchSectionRecentlyAdded()` for each section. This means 5-15+ HTTP calls just to render the home screen.

**Changes**:

### New file: `include/app/metadata_cache.hpp`
```
class MetadataCache {
public:
    static MetadataCache& getInstance();

    // Library sections
    bool getCachedSections(std::vector<LibrarySection>& sections);
    void cacheSections(const std::vector<LibrarySection>& sections);

    // Home screen data
    bool getCachedContinueWatching(std::vector<MediaItem>& items);
    void cacheContinueWatching(const std::vector<MediaItem>& items);

    bool getCachedRecentlyAdded(const std::string& sectionKey, std::vector<MediaItem>& items);
    void cacheRecentlyAdded(const std::string& sectionKey, const std::vector<MediaItem>& items);

    // Check staleness (cache valid for N minutes)
    bool isFresh(const std::string& cacheKey, int maxAgeSeconds = 300);

    // Persistence
    void loadFromDisk();
    void saveToDisk();
    void clearAll();

private:
    struct CacheEntry { std::vector<MediaItem> items; time_t timestamp; };
    std::map<std::string, CacheEntry> m_cache;
    std::vector<LibrarySection> m_sections;
    time_t m_sectionsTimestamp = 0;
};
```

### New file: `src/app/metadata_cache.cpp`
- **Disk path**: `ux0:data/VitaPlex/cache/metadata.json`
- Serialize/deserialize `MediaItem` fields to JSON using the existing `extractJsonValue` pattern from PlexClient
- On `loadFromDisk()`, populate the in-memory cache maps
- `isFresh()` checks `time(nullptr) - entry.timestamp < maxAgeSeconds`

### Modify: `src/view/home_tab.cpp`
- In `loadContent()`:
  1. First try `MetadataCache::getInstance().getCachedContinueWatching()` — if fresh, populate UI immediately
  2. Still kick off async fetch in background to refresh cache
  3. When async fetch completes, update cache via `cacheContinueWatching()` and refresh UI only if data changed
- Same pattern for recently added sections

### Modify: `src/app/plex_client.cpp`
- After successful `fetchLibrarySections()`, also call `MetadataCache::getInstance().cacheSections()`
- Add cache integration to other fetch methods as needed

---

## Feature 5: Network Failover (LAN/WAN Dual URL)

**Problem**: `PlexClient` stores a single `m_serverUrl`. If the user leaves their home network, the local URL becomes unreachable with no fallback. The `PlexServer` struct already has `std::vector<ServerConnection> connections` (sorted local-first), but `connectToServer()` only tries one URL.

**Changes**:

### `include/app/plex_client.hpp`
- Add to PlexClient private members:
  - `std::vector<std::string> m_serverUrls` — all available URLs for current server (local, remote, relay), ordered by preference
  - `int m_activeUrlIndex = 0` — index of currently working URL
  - `std::mutex m_tokenRefreshMutex` — prevent thundering herd on 401
  - `std::atomic<bool> m_refreshingToken{false}` — dedup flag

### `src/app/plex_client.cpp`
- **`connectToServer()`**: Accept a `PlexServer` overload that stores all connection URIs in `m_serverUrls`, tries them in order (local → remote → relay), sets `m_activeUrlIndex` to first that responds
- **`buildApiUrl()`**: Use `m_serverUrls[m_activeUrlIndex]` instead of `m_serverUrl`
- **Add `tryNextUrl()` method**: On connection failure or timeout, increment `m_activeUrlIndex` (wrapping around), log the failover, and retry the request once
- **401 handling**: In `request()` wrapper, if HTTP 401 and not already refreshing:
  - Set `m_refreshingToken = true`
  - Attempt `refreshToken()`
  - On success, retry original request
  - On failure, trigger re-login flow
  - Use mutex to prevent concurrent refresh attempts
- **Implement `refreshToken()`**: Currently returns false with a TODO. Implement using `POST https://plex.tv/api/v2/pins` JWT refresh flow

---

## Feature 6: Download Queue Robustness

**Problem**: `DownloadsManager` has several gaps:
- `loadState()` (line 491) has a `TODO: Implement proper JSON parsing` — downloads are lost on every restart
- No integrity validation — partial downloads from crashes aren't detected
- No retry on failure
- No concurrent download support despite `maxConcurrentDownloads` setting

**Changes**:

### `src/app/downloads_manager.cpp`

**6a. Implement `loadState()` properly:**
- Parse the JSON written by `saveState()` using the existing `extractJsonValue` pattern from PlexClient
- Walk through the `"downloads"` array, extract each field, reconstruct `DownloadItem` structs
- Map `"state"` integer back to `DownloadState` enum

**6b. File integrity validation on startup:**
- After `loadState()`, validate each COMPLETED download:
  - Check file exists (`sceIoOpen` with `SCE_O_RDONLY`)
  - Check file size matches `totalBytes` (if > 0)
  - If file missing or size mismatch → set state to `QUEUED` for re-download
- For DOWNLOADING state items found on load → set to `QUEUED` (interrupted by crash)

**6c. Retry on failure:**
- Add `int retryCount = 0` and `int maxRetries = 3` to `DownloadItem`
- In `downloadItem()`, on failure: if `retryCount < maxRetries`, set state back to `QUEUED` and increment `retryCount` instead of leaving as `FAILED`
- Add exponential backoff: sleep `2^retryCount` seconds before retry

**6d. Concurrent downloads:**
- In `startDownloads()`, instead of processing one item at a time, launch up to `maxConcurrentDownloads` async workers
- Each worker pulls the next QUEUED item from the list
- Use an atomic counter `m_activeDownloads` to track running downloads

**6e. Proper `saveState()` string escaping:**
- Escape special characters in title/path strings (quotes, backslashes) when writing JSON to prevent parse failures on load

---

## Feature 7: Custom JSON Parsing (Lightweight)

**Problem**: VitaPlex already has `extractJsonValue/extractJsonInt/extractJsonFloat/extractJsonBool` in `PlexClient` (lines 55-103), but they're private to `PlexClient`. Other code (like `DownloadsManager::loadState`) can't reuse them, and they lack array iteration.

**Changes**:

### New file: `include/utils/json_helpers.hpp`
- Extract the JSON helper functions from `PlexClient` into a standalone utility namespace:
```cpp
namespace vitaplex { namespace json {
    std::string getValue(const std::string& json, const std::string& key);
    int getInt(const std::string& json, const std::string& key);
    float getFloat(const std::string& json, const std::string& key);
    bool getBool(const std::string& json, const std::string& key);

    // New: Array iteration — calls callback for each object in a JSON array
    // Returns the number of items processed
    int forEachInArray(const std::string& json, const std::string& arrayKey,
                       std::function<void(const std::string& objectJson)> callback);

    // New: Escape a string for JSON output (handles quotes, backslashes, newlines)
    std::string escape(const std::string& str);
}}
```

### New file: `src/utils/json_helpers.cpp`
- Move `extractJsonValue`, `extractJsonInt`, `extractJsonFloat`, `extractJsonBool` implementations here
- **`forEachInArray()`**: Find `"arrayKey":[`, then iterate through top-level objects by tracking brace depth. Call callback with each `{...}` substring.
- **`escape()`**: Replace `\` → `\\`, `"` → `\"`, newlines → `\n`, tabs → `\t`

### Modify: `include/app/plex_client.hpp`
- Remove private `extractJsonValue/Int/Float/Bool` declarations
- Add `#include "utils/json_helpers.hpp"`

### Modify: `src/app/plex_client.cpp`
- Replace all `extractJsonValue(...)` calls with `json::getValue(...)`
- Replace all `extractJsonInt(...)` calls with `json::getInt(...)`
- Same for Float/Bool
- Use `json::forEachInArray()` for server/connection parsing instead of manual brace counting (lines 278-369)

### Modify: `src/app/downloads_manager.cpp`
- Use `json::getValue/getInt/forEachInArray` in `loadState()` to properly parse the saved state
- Use `json::escape()` in `saveState()` for title/path strings

---

## Implementation Order

1. **Feature 7** (JSON helpers) — No dependencies, enables Features 4 and 6
2. **Feature 1** (LRU + disk cache) — Core image improvement
3. **Feature 2** (VRAM safety) — Small addition to Feature 1's code
4. **Feature 3** (Texture batching) — Builds on Feature 1's upload path
5. **Feature 6** (Download robustness) — Uses Feature 7's JSON helpers
6. **Feature 4** (Metadata cache) — Uses Feature 7's JSON helpers
7. **Feature 5** (Network failover) — Most complex, changes request flow

## Files Modified

| File | Features |
|------|----------|
| `include/utils/json_helpers.hpp` | **NEW** — Feature 7 |
| `src/utils/json_helpers.cpp` | **NEW** — Feature 7 |
| `include/app/metadata_cache.hpp` | **NEW** — Feature 4 |
| `src/app/metadata_cache.cpp` | **NEW** — Feature 4 |
| `include/utils/image_loader.hpp` | Features 1, 2, 3 |
| `src/utils/image_loader.cpp` | Features 1, 2, 3 |
| `include/app/plex_client.hpp` | Features 5, 7 |
| `src/app/plex_client.cpp` | Features 5, 7 |
| `include/app/downloads_manager.hpp` | Feature 6 |
| `src/app/downloads_manager.cpp` | Features 6, 7 |
| `src/view/home_tab.cpp` | Feature 4 |
| `src/app/application.cpp` or `src/main.cpp` | Feature 3 (frame hook) |
| `CMakeLists.txt` | Add new source files |

## Verification

1. **Build**: `mkdir -p build && cd build && cmake .. && make` — must compile without errors
2. **Feature 7**: Verify `json::forEachInArray` works by checking PlexClient server parsing still produces correct results
3. **Feature 1**: After browsing a library, thumbnails should appear instantly when navigating back (memory cache). After restart, thumbnails should load from disk cache without network
4. **Feature 2**: Log messages should appear for any oversized images; no crashes from large posters
5. **Feature 3**: Scrolling through a full library grid should not drop frames — verify texture uploads are batched (debug log: "ImageLoader: Deferred N uploads to next frame")
6. **Feature 6**: Queue downloads, force-kill the app, relaunch → downloads should still be in the list with correct states
7. **Feature 4**: Second app launch should show home screen content immediately from cache, then refresh in background
8. **Feature 5**: Connect to server, then disconnect from LAN → app should automatically try remote/relay URLs
