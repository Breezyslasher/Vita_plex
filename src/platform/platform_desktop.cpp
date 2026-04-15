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

// Per-OS display queries. Desktop borealis uses GLFW internally, not SDL2, so
// we can't reuse SDL2 here like platform_android.cpp does — linking would
// fail. Instead use the native windowing APIs that are ALREADY linked into
// the app for each OS:
//   - macOS: CoreGraphics is linked via "-framework CoreGraphics" in
//            CMakeLists.txt for the CoreWLAN/AppKit stack.
//   - Windows: user32.dll's GetSystemMetrics ships with every MSVC toolchain.
//   - Linux: no guaranteed display server lib is linked into the binary
//            (could be X11, Wayland, or headless), so fall back to a fixed
//            1920×1080 default. This still gives good layouts on the most
//            common Linux monitor; users with other sizes just land in the
//            1080p constraint tier.
#if defined(__APPLE__)
  #include <CoreGraphics/CoreGraphics.h>
#elif defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace vitaplex {
namespace platform {

ScreenSize getScreenSize() {
    // Returns the main-monitor resolution so getImageConstraints() can size
    // posters, grid columns, cache budget, and thumbnail requests to match
    // the user's actual display — a 24" 1080p panel, a 34" 1440p ultrawide,
    // and a 4K monitor all get appropriately different layouts.
#if defined(__APPLE__)
    CGDirectDisplayID display = CGMainDisplayID();
    size_t w = CGDisplayPixelsWide(display);
    size_t h = CGDisplayPixelsHigh(display);
    if (w == 0 || h == 0) return { 1920, 1080 };
    return { static_cast<int>(w), static_cast<int>(h) };
#elif defined(_WIN32)
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return { 1920, 1080 };
    return { w, h };
#else
    // Linux: no standardized display query without pulling in X11 / Wayland
    // libs that aren't guaranteed to be linked. Fall back to the 1080p tier.
    return { 1920, 1080 };
#endif
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
