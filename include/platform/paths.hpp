#pragma once

/**
 * Platform-specific data directory abstraction.
 *
 * All persistent data (settings, cache, downloads, logs) is rooted under
 * PLATFORM_DATA_DIR. Use the helper macros / inline functions below instead
 * of hard-coding "ux0:" paths anywhere in the codebase.
 *
 * Supported platforms:
 *   PS Vita  – ux0:data/VitaSuwayomi
 *   Switch   – sdmc:/VitaSuwayomi
 *   Android  – <SDL internal storage>/VitaSuwayomi  (resolved at runtime)
 *   Desktop  – ./VitaSuwayomi   (next to the executable)
 */

#include <string>
#include <cstdlib>

#if defined(__vita__)
    static constexpr const char* PLATFORM_DATA_DIR = "ux0:data/VitaSuwayomi";
#elif defined(__SWITCH__)
    static constexpr const char* PLATFORM_DATA_DIR = "sdmc:/VitaSuwayomi";
#elif defined(__ANDROID__)
    // On Android the writable path is determined at runtime via SDL.
    // PLATFORM_DATA_DIR is left empty here — use getAndroidDataDir() instead
    // of PLATFORM_DATA_DIR directly. platformPath() calls getAndroidDataDir()
    // automatically on Android.
    static constexpr const char* PLATFORM_DATA_DIR = "";
#elif defined(__PS4__)
    static constexpr const char* PLATFORM_DATA_DIR = "/data/VitaSuwayomi";
#else
    // Desktop: resolved at runtime via $HOME
    inline const std::string& getDesktopDataDir() {
        static std::string s_dir;
        if (s_dir.empty()) {
            const char* home = std::getenv("HOME");
            if (home && *home) {
                s_dir = std::string(home) + "/.local/share/VitaSuwayomi";
            } else {
                s_dir = "./VitaSuwayomi";
            }
        }
        return s_dir;
    }
#endif

#if defined(__ANDROID__)
#include <SDL2/SDL.h>
#include <string>

/**
 * Returns the Android-specific writable data directory (internal storage).
 * Result is cached after first call. Thread-safe after SDL init.
 * Example: /data/user/0/org.vitasuwayomi.app/files/VitaSuwayomi
 */
inline const std::string& getAndroidDataDir() {
    static std::string s_dataDir;
    if (s_dataDir.empty()) {
        const char* internalPath = SDL_AndroidGetInternalStoragePath();
        if (internalPath && internalPath[0] != '\0') {
            s_dataDir = std::string(internalPath) + "/VitaSuwayomi";
        } else {
            // Absolute fallback — should never happen in practice
            s_dataDir = "/sdcard/VitaSuwayomi";
        }
    }
    return s_dataDir;
}
#endif

/**
 * Build a full path rooted at the platform data directory.
 * Example: platformPath("downloads") -> "ux0:data/VitaSuwayomi/downloads"
 *
 * On Android the base directory is resolved via SDL_AndroidGetInternalStoragePath()
 * so that the path is always writable without requiring external storage permission.
 */
inline std::string platformPath(const char* relative) {
#if defined(__ANDROID__)
    return getAndroidDataDir() + "/" + relative;
#elif defined(__vita__) || defined(__SWITCH__) || defined(__PS4__)
    return std::string(PLATFORM_DATA_DIR) + "/" + relative;
#else
    return getDesktopDataDir() + "/" + relative;
#endif
}

inline std::string platformPath(const std::string& relative) {
#if defined(__ANDROID__)
    return getAndroidDataDir() + "/" + relative;
#elif defined(__vita__) || defined(__SWITCH__) || defined(__PS4__)
    return std::string(PLATFORM_DATA_DIR) + "/" + relative;
#else
    return getDesktopDataDir() + "/" + relative;
#endif
}

/**
 * Returns true when a URL / path string looks like a local file on the
 * current platform, rather than an HTTP(S) URL.
 *
 * Vita  : paths start with ux0:, ur0:, uma0:, imc0:, or absolute /
 * Switch: paths start with sdmc:/ or absolute /
 * Android: absolute paths (always /)
 * Desktop: absolute paths (/) or paths under the data dir
 */
inline bool isPlatformLocalPath(const std::string& url) {
    if (url.empty()) return false;
#if defined(__vita__)
    return url.find("ux0:") == 0 ||
           url.find("ur0:") == 0 ||
           url.find("uma0:") == 0 ||
           url.find("imc0:") == 0 ||
           url[0] == '/';
#elif defined(__SWITCH__)
    return url.find("sdmc:/") == 0 || url[0] == '/';
#elif defined(__ANDROID__)
    return url[0] == '/';
#elif defined(__PS4__)
    return url[0] == '/' ||
           url.find(PLATFORM_DATA_DIR) == 0;
#else
    // Desktop: absolute path or anything under our data dir
    return url[0] == '/' ||
           url.find(getDesktopDataDir()) == 0;
#endif
}
