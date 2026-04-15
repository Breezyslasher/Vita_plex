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
    // when handheld. 5 cols × 160 + spacing ~= 840px leaves room for the
    // sidebar in a 1280-wide virtual viewport.
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
        /* homeRowHeight      */ 290,  // posterHeight(240) + label + padding
        /* landscapeRowHeight */ 185,  // landscapeHeight(125) + ~60
        /* squareRowHeight    */ 215,  // squareCoverSize(160) + ~55

        /* listRowHeight            */  60,
        /* livetvChannelCardWidth   */ 160,
        /* livetvChannelRowHeight   */ 130,
        /* livetvGuideHeight        */ 430,

        /* maxCellTitleChars        */  20,
        /* maxListTitleChars        */  90,
        /* maxLiveTVProgramChars    */  22,
        /* maxLiveTVChannelChars    */  18,

        /* sidebarMinWidth          */ 240,
        /* sidebarMaxWidth          */ 400,

        /* dialogWidth              */ 520,

        /* imageCacheSize           */  60,

        /* libraryPageSize          */ 200,
        /* playlistTrackPageSize    */ 100,
        /* musicCarouselLimit       */  80,

        /* posterRequestWidth       */ 320,  // ~2x 160px display
        /* posterRequestHeight      */ 480,
        /* squareRequestSize        */ 320,
        /* landscapeRequestWidth    */ 440,
        /* landscapeRequestHeight   */ 250,
        /* detailPosterRequestWidth */ 500,
        /* detailPosterRequestHeight*/ 750,
        /* photoRequestWidth        */ 1280,
        /* photoRequestHeight       */ 720,
    };
    return c;
}

const VideoConstraints& getVideoConstraints() {
    // Switch NVDEC decodes 1080p H.264 up to level 4.2 reliably.
    static const VideoConstraints v = {
        /* plexPlatform     */ "Nintendo Switch",
        /* plexDevice       */ "Switch",
        /* maxVideoWidth    */ 1920,
        /* maxVideoHeight   */ 1080,
        /* maxVideoLevel    */ 42,
        /* defaultBitrate   */ 4000,
        /* defaultResolution*/ "1280x720",
        /* defaultVideoQualityIndex */ 2,  // QUALITY_720P
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
