/**
 * VitaPlex platform layer — Nintendo Switch (libnx) implementation.
 *
 * Selected by CMake when -DPLATFORM_SWITCH=ON. The Switch wrapper in
 * borealis already handles libnx subsystem init via switch_wrapper.c, so
 * this file mostly delegates to portable curl init and supplies tuned
 * image-sizing constraints for the 720p handheld / 1080p docked screen.
 */

#include "platform/platform.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <cstdio>
#include <fstream>

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // Switch: 1280x720 handheld, 1920x1080 docked. Mid-size covers strike
    // a balance between sharpness when docked and texture memory pressure
    // when handheld.
    static const ImageConstraints c = {
        /* posterWidth        */ 180,
        /* posterHeight       */ 270,
        /* squareCoverSize    */ 180,
        /* landscapeWidth     */ 230,
        /* landscapeHeight    */ 130,
        /* gridColumns        */   6,
        /* gridCellSpacing    */  16,
        /* titleFontSize      */  16,
        /* subtitleFontSize   */  13,
        /* descriptionFontSize*/  11,
    };
    return c;
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
