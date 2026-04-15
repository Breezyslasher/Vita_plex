/**
 * VitaPlex platform layer — Android implementation.
 *
 * Selected by CMake when -DPLATFORM_ANDROID=ON. Bundles APK asset
 * extraction and supplies the cover image budget for handheld-class
 * Android devices.
 */

#include "platform/platform.hpp"
#include "platform/platform_dynamic.hpp"
#include "platform/android_assets.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <cstdio>
#include <fstream>

#include <SDL2/SDL.h>

// Forward declaration — defined in src/main.cpp. SDL2's Android backend
// dispatches into SDL_main() instead of main(), so we have to provide the
// SDL_main symbol here and forward it to the shared entry point.
extern "C" int VitaPlexMainEntry(int argc, char* argv[]);

extern "C" int SDL_main(int argc, char* argv[]) {
    return VitaPlexMainEntry(argc, argv);
}

namespace vitaplex {
namespace platform {

ScreenSize getScreenSize() {
    // Query SDL for the current display dimensions. On Android this
    // returns the logical pixel size of the active display — which
    // adapts automatically when a foldable unfolds (the OS swaps the
    // active display from the cover screen to the main screen and SDL
    // reports the new dimensions), or when the user rotates between
    // portrait and landscape. A Galaxy Z Fold outer cover (6.2",
    // ~2316×904) and its main screen (7.6", ~2176×1812) produce
    // different column counts and poster sizes via the dynamic formula.
    SDL_DisplayMode mode{};
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            return { 1920, 1080 };  // conservative tablet fallback
        }
    }
    if (SDL_GetCurrentDisplayMode(0, &mode) != 0 || mode.w <= 0 || mode.h <= 0) {
        return { 1920, 1080 };
    }
    return { mode.w, mode.h };
}

const ImageConstraints& getImageConstraints() {
    // Dynamic: Android devices range from 720p budget phones to 4K TV
    // boxes and foldables whose logical resolution changes when the
    // cover/main display swaps. The shared dynamic formula derives all
    // layout metrics from the live screen size so unfolding a phone
    // automatically widens the grid and enlarges posters on the next
    // layout pass. See include/platform/platform_dynamic.hpp.
    return getDynamicImageConstraintsCached();
}

const VideoConstraints& getVideoConstraints() {
    // Most Android devices (phones, tablets, TV boxes) decode 1080p H.264
    // High@L5.1 in hardware via MediaCodec. Bitrate default is kept
    // conservative for mobile networks.
    static const VideoConstraints v = {
        /* plexPlatform     */ "Android",
        /* plexDevice       */ "Android TV",
        /* maxVideoWidth    */ 1920,
        /* maxVideoHeight   */ 1080,
        /* maxVideoLevel    */ 51,
        /* defaultBitrate   */ 8000,
        /* defaultResolution*/ "1920x1080",
        /* defaultVideoQualityIndex */ 1,  // QUALITY_1080P
    };
    return v;
}

bool init() {
    // Borealis on Android loads resources via fopen("resources/...") which
    // can't read APK assets directly, so extract them to internal storage
    // first. Must run BEFORE brls::Application::init().
    extractAndroidAssets();

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
