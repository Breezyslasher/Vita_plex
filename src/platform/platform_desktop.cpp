/**
 * VitaPlex platform layer — Desktop (Linux / macOS / Windows) implementation.
 *
 * Selected by CMake when -DPLATFORM_DESKTOP=ON. Standard C++ I/O and a
 * generous cover-image budget — desktop screens are usually 1080p or
 * higher and have plenty of memory.
 */

#include "platform/platform.hpp"
#include "platform/platform_dynamic.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <cstdio>
#include <fstream>

#include <SDL2/SDL.h>

namespace vitaplex {
namespace platform {

ScreenSize getScreenSize() {
    // Query SDL for the desktop's native resolution. This reflects the
    // monitor the user is actually on — a 24" 1080p panel reports
    // 1920×1080, a 34" ultrawide reports 3440×1440, and a 4K monitor
    // reports 3840×2160. getImageConstraints() scales posters, grid
    // columns, image cache size, and library pagination from this.
    //
    // SDL_GetDesktopDisplayMode is preferred over SDL_GetCurrentDisplayMode
    // because the latter can briefly report stale values while the window
    // is in the middle of a resize. If SDL hasn't been initialized yet
    // (very early startup), we fall back to a conservative 1920×1080.
    SDL_DisplayMode mode{};
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        // Try to init video just enough to read the display mode.
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            return { 1920, 1080 };
        }
    }
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0 || mode.w <= 0 || mode.h <= 0) {
        return { 1920, 1080 };
    }
    return { mode.w, mode.h };
}

const ImageConstraints& getImageConstraints() {
    // Dynamic: borealis layouts are sized from the *actual* monitor
    // resolution so a 24" 1080p and a 34" 1440p ultrawide get different
    // poster sizes, different grid column counts, and different image
    // cache budgets. See include/platform/platform_dynamic.hpp for the
    // full scaling formula.
    return getDynamicImageConstraintsCached();
}

const VideoConstraints& getVideoConstraints() {
    // Desktop boxes are capable of full 1080p H.264 High@L5.1 transcodes.
    static const VideoConstraints v = {
        /* plexPlatform     */ "Desktop",
        /* plexDevice       */ "Desktop",
        /* maxVideoWidth    */ 1920,
        /* maxVideoHeight   */ 1080,
        /* maxVideoLevel    */ 51,
        /* defaultBitrate   */ 10000,
        /* defaultResolution*/ "1920x1080",
        /* defaultVideoQualityIndex */ 1,  // QUALITY_1080P
    };
    return v;
}

bool init() {
    if (!::vitaplex::HttpClient::globalInit()) {
        brls::Logger::error("Failed to initialize curl");
        return false;
    }
    return true;
}

void shutdown() {
    ::vitaplex::HttpClient::globalCleanup();
}

std::string getLogPath() {
    return std::string{};
}

void openLogFile() {}
void closeLogFile() {}

bool readLocalFile(const std::string& path,
                   std::vector<uint8_t>& out,
                   std::size_t maxBytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto size = file.tellg();
    if (size <= 0 || (std::size_t)size > maxBytes) return false;

    file.seekg(0, std::ios::beg);
    out.resize((std::size_t)size);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good() || file.eof();
}

bool needsHardExit() {
    return false;
}

[[noreturn]] void hardExit(int code) {
    std::exit(code);
}

}  // namespace platform
}  // namespace vitaplex
