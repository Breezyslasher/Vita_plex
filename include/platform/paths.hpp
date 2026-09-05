#pragma once

/**
 * Platform-specific data directory abstraction.
 *
 * All persistent data (settings, cache, downloads, logs) is rooted under
 * PLATFORM_DATA_DIR. Use the helper macros / inline functions below instead
 * of hard-coding "ux0:" paths anywhere in the codebase.
 *
 * Supported platforms:
 *   PS Vita  – ux0:data/VitaPlex
 *   Switch   – sdmc:/VitaPlex
 *   Android  – <SDL internal storage>/VitaPlex  (resolved at runtime)
 *   Desktop  – ./VitaPlex  (next to the executable)
 */

#include <string>
#include <cstdlib>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
// Only the Windows branch of getDesktopDataDir() needs it, and the console
// toolchains are happier not seeing it at all.
#include <filesystem>
#include <system_error>
#endif

#if defined(__vita__)
    static constexpr const char* PLATFORM_DATA_DIR = "ux0:data/VitaPlex";
#elif defined(__SWITCH__)
    static constexpr const char* PLATFORM_DATA_DIR = "sdmc:/VitaPlex";
#elif defined(__ANDROID__)
    // On Android the writable path is determined at runtime via SDL.
    // PLATFORM_DATA_DIR is left empty here — use getAndroidDataDir() instead
    // of PLATFORM_DATA_DIR directly. platformPath() calls getAndroidDataDir()
    // automatically on Android.
    static constexpr const char* PLATFORM_DATA_DIR = "";
#elif defined(__PS4__)
    static constexpr const char* PLATFORM_DATA_DIR = "/data/VitaPlex";
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
    // iOS / tvOS apps are sandboxed. The writable path is the per-app
    // Documents directory, resolved by NSFileManager. Look up once and
    // cache — same pattern as Android.
    const std::string& getIosDataDir();
    static constexpr const char* PLATFORM_DATA_DIR = "";
#else
    // Desktop: $XDG_DATA_HOME/VitaPlex, per the XDG Base Directory spec, which
    // defines $HOME/.local/share as the default when the variable is unset or
    // not an absolute path. That default is byte-for-byte the path this used to
    // hardcode, so nothing moves for anyone who has not set the variable —
    // while a user who has relocated it, or a sandbox that sets it (Flatpak
    // points it inside ~/.var/app), is now honoured instead of ignored.
    //
    // Config and cache deliberately stay in here rather than splitting to
    // XDG_CONFIG_HOME and XDG_CACHE_HOME: correct, but it would move the
    // settings of every existing install.
    inline const std::string& getDesktopDataDir() {
        static std::string s_dir;
        if (s_dir.empty()) {
#if defined(_WIN32)
            // Windows has neither $HOME nor XDG_DATA_HOME, so the branch below
            // fell all the way through to "./VitaPlex" — a path relative to the
            // working directory. That is wrong twice over: an install under
            // Program Files cannot write next to its own exe, and the working
            // directory depends on how the app was launched, so the same
            // install could read a different data directory from one start to
            // the next. %LOCALAPPDATA% is where this belongs.
            //
            // An existing install already has its settings, downloads and cache
            // in the old spot, and moving them out from under it would read as
            // losing them. So a "VitaPlex" directory that is already there wins,
            // and only fresh installs get the correct location.
            std::error_code ec;
            const char* localApp = std::getenv("LOCALAPPDATA");
            if (std::filesystem::is_directory("VitaPlex", ec)) {
                s_dir = "./VitaPlex";
            } else if (localApp && *localApp) {
                s_dir = std::string(localApp) + "\\VitaPlex";
            } else {
                s_dir = "./VitaPlex";
            }
#else
            const char* xdgData = std::getenv("XDG_DATA_HOME");
            const char* home    = std::getenv("HOME");
            if (xdgData && xdgData[0] == '/') {
                s_dir = std::string(xdgData) + "/VitaPlex";
            } else if (home && *home) {
                s_dir = std::string(home) + "/.local/share/VitaPlex";
            } else {
                s_dir = "./VitaPlex";
            }
#endif
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
 * Example: /data/user/0/org.VitaPlex.app/files/VitaPlex
 */
inline std::string& androidDataDirStorage() {
    static std::string s_dataDir;
    return s_dataDir;
}

/**
 * Seed the data directory from a Java-supplied path (Context.getFilesDir()).
 *
 * SDL_AndroidGetInternalStoragePath() only answers once SDL's JNI setup has run,
 * which happens when SDLActivity starts. A process launched *without* the
 * activity — the media-browser service being bound cold by Android Auto or a
 * watch companion — has no SDL context, so the lookup below would fall through
 * to the /sdcard guess and never find the saved config. Java knows the real
 * path from its Context, so it hands it over before any path is resolved.
 * No-op once the directory is known.
 */
inline void setAndroidDataDir(const std::string& filesDir) {
    if (filesDir.empty()) return;
    std::string& dir = androidDataDirStorage();
    if (dir.empty()) dir = filesDir + "/VitaPlex";
}

/**
 * Returns the Android-specific writable data directory (internal storage).
 * Result is cached after first call. Thread-safe after SDL init.
 * Example: /data/user/0/org.VitaPlex.app/files/VitaPlex
 */
inline const std::string& getAndroidDataDir() {
    std::string& s_dataDir = androidDataDirStorage();
    if (s_dataDir.empty()) {
        const char* internalPath = SDL_AndroidGetInternalStoragePath();
        if (internalPath && internalPath[0] != '\0') {
            s_dataDir = std::string(internalPath) + "/VitaPlex";
        } else {
            // Absolute fallback — should never happen in practice
            s_dataDir = "/sdcard/VitaPlex";
        }
    }
    return s_dataDir;
}
#endif

/**
 * Build a full path rooted at the platform data directory.
 * Example: platformPath("downloads") -> "ux0:data/VitaPlex/downloads"
 *
 * On Android the base directory is resolved via SDL_AndroidGetInternalStoragePath()
 * so that the path is always writable without requiring external storage permission.
 */
inline std::string platformPath(const char* relative) {
#if defined(__ANDROID__)
    return getAndroidDataDir() + "/" + relative;
#elif defined(__vita__) || defined(__SWITCH__) || defined(__PS4__)
    return std::string(PLATFORM_DATA_DIR) + "/" + relative;
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
    return getIosDataDir() + "/" + relative;
#else
    return getDesktopDataDir() + "/" + relative;
#endif
}

inline std::string platformPath(const std::string& relative) {
#if defined(__ANDROID__)
    return getAndroidDataDir() + "/" + relative;
#elif defined(__vita__) || defined(__SWITCH__) || defined(__PS4__)
    return std::string(PLATFORM_DATA_DIR) + "/" + relative;
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
    return getIosDataDir() + "/" + relative;
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
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
    return url[0] == '/' ||
           url.find(getIosDataDir()) == 0;
#else
    // Desktop: absolute path or anything under our data dir
    return url[0] == '/' ||
           url.find(getDesktopDataDir()) == 0;
#endif
}
