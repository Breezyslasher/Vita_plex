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
    // Android: phone / tablet screens are usually 1080p+. Slightly smaller
    // covers than desktop to keep texture upload pressure manageable on
    // mobile GPUs but still large enough that posters remain sharp.
    static const ImageConstraints c = {
        /* posterWidth        */ 200,
        /* posterHeight       */ 300,
        /* squareCoverSize    */ 200,
        /* landscapeWidth     */ 260,
        /* landscapeHeight    */ 150,
        /* gridColumns        */   6,
        /* gridCellSpacing    */  16,
        /* titleFontSize      */  16,
        /* subtitleFontSize   */  13,
        /* descriptionFontSize*/  11,
    };
    return c;
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
