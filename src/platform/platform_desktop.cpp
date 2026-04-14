/**
 * VitaPlex platform layer — Desktop (Linux / macOS / Windows) implementation.
 *
 * Selected by CMake when -DPLATFORM_DESKTOP=ON. Standard C++ I/O and a
 * generous cover-image budget — desktop screens are usually 1080p or
 * higher and have plenty of memory.
 */

#include "platform/platform.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <cstdio>
#include <fstream>

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // Desktop: borealis's virtual coordinate system is 1280x720, so
    // oversized covers (240×360 × 7 cols = 1680px) overflow the viewport
    // and look like "posters cut off at the edge". Use 170×255 covers at
    // 5 columns so an 8-item recently-added row fits comfortably in the
    // content area (sidebar ~230 + 5×180 + padding ≈ 1150 < 1280).
    static const ImageConstraints c = {
        /* posterWidth        */ 170,
        /* posterHeight       */ 255,
        /* squareCoverSize    */ 170,
        /* landscapeWidth     */ 240,
        /* landscapeHeight    */ 135,
        /* gridColumns        */   5,
        /* gridCellSpacing    */  16,
        /* titleFontSize      */  16,
        /* subtitleFontSize   */  13,
        /* descriptionFontSize*/  11,
        /* homeTitleFontSize  */  30,
        /* homeSectionFontSize*/  22,
        /* homeRowHeight      */ 310,  // posterHeight + label + padding
        /* landscapeRowHeight */ 195,  // landscapeHeight(135) + ~60
        /* squareRowHeight    */ 225,  // squareCoverSize(170) + ~55
    };
    return c;
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
