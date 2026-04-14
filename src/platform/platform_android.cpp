/**
 * VitaPlex platform layer — Android implementation.
 *
 * Selected by CMake when -DPLATFORM_ANDROID=ON. Bundles APK asset
 * extraction and supplies the cover image budget for handheld-class
 * Android devices.
 */

#include "platform/platform.hpp"
#include "platform/android_assets.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <cstdio>
#include <fstream>

// Forward declaration — defined in src/main.cpp. SDL2's Android backend
// dispatches into SDL_main() instead of main(), so we have to provide the
// SDL_main symbol here and forward it to the shared entry point.
extern "C" int VitaPlexMainEntry(int argc, char* argv[]);

extern "C" int SDL_main(int argc, char* argv[]) {
    return VitaPlexMainEntry(argc, argv);
}

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // Android: phone / tablet / TV screens are usually 1080p+. 5 cols
    // × 160px (+14px spacing) fits inside borealis's 1280-wide virtual
    // viewport after the sidebar, and keeps GPU texture pressure low on
    // mobile GPUs. Previous 6-column 200px layout overflowed the viewport
    // and the last poster of each row was clipped.
    static const ImageConstraints c = {
        /* posterWidth        */ 160,
        /* posterHeight       */ 240,
        /* squareCoverSize    */ 160,
        /* landscapeWidth     */ 220,
        /* landscapeHeight    */ 125,
        /* gridColumns        */   5,
        /* gridCellSpacing    */  14,
        /* titleFontSize      */  15,
        /* subtitleFontSize   */  12,
        /* descriptionFontSize*/  11,
        /* homeTitleFontSize  */  28,
        /* homeSectionFontSize*/  20,
        /* homeRowHeight      */ 290,
    };
    return c;
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
