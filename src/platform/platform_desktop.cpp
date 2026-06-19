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
#include <thread>

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // Desktop LANDSCAPE: borealis's virtual coordinate system is
    // 1280x720, so oversized covers (240×360 × 7 cols = 1680px) overflow
    // the viewport and look like "posters cut off at the edge". Use
    // 170×255 covers at 5 columns so an 8-item recently-added row fits
    // comfortably in the content area (sidebar ~230 + 5×180 + padding
    // ≈ 1150 < 1280).
    static const ImageConstraints landscape = {
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

        /* listRowHeight            */  64,
        /* livetvChannelCardWidth   */ 180,
        /* livetvChannelRowHeight   */ 140,
        /* livetvGuideHeight        */ 480,

        /* maxCellTitleChars        */  24,
        /* maxListTitleChars        */ 110,
        /* maxLiveTVProgramChars    */  26,
        /* maxLiveTVChannelChars    */  22,

        /* sidebarMinWidth          */ 260,
        /* sidebarMaxWidth          */ 450,

        /* dialogWidth              */ 560,

        /* imageCacheSize           */ 120,

        /* libraryPageSize          */ 500,
        /* playlistTrackPageSize    */ 200,
        /* musicCarouselLimit       */ 150,

        /* posterRequestWidth       */ 340,  // ~2x 170px display
        /* posterRequestHeight      */ 510,
        /* squareRequestSize        */ 340,
        /* landscapeRequestWidth    */ 480,
        /* landscapeRequestHeight   */ 270,
        /* detailPosterRequestWidth */ 600,
        /* detailPosterRequestHeight*/ 900,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1080,
    };

    // Desktop PORTRAIT (vertical-monitor / rotated window). Narrower
    // content area means fewer columns, slightly larger covers as a
    // fraction of width, tighter sidebar so the grid has room to
    // breathe. List / dialog widths shrink to match. Background-art
    // request sizes stay full-res — the device still has the GPU/RAM
    // budget to handle them, and we want sharp posters when they ARE
    // shown big in detail views.
    static const ImageConstraints portrait = {
        /* posterWidth        */ 180,
        /* posterHeight       */ 270,
        /* squareCoverSize    */ 180,
        /* landscapeWidth     */ 240,
        /* landscapeHeight    */ 135,
        /* gridColumns        */   3,   // 5 -> 3 to fit narrower content
        /* gridCellSpacing    */  12,   // tighter than landscape's 16
        /* titleFontSize      */  16,
        /* subtitleFontSize   */  13,
        /* descriptionFontSize*/  12,
        /* homeTitleFontSize  */  30,
        /* homeSectionFontSize*/  22,
        /* homeRowHeight      */ 330,
        /* landscapeRowHeight */ 195,
        /* squareRowHeight    */ 240,

        /* listRowHeight            */  64,
        /* livetvChannelCardWidth   */ 170,
        /* livetvChannelRowHeight   */ 140,
        /* livetvGuideHeight        */ 620,  // taller window -> taller guide

        /* maxCellTitleChars        */  20,
        /* maxListTitleChars        */  80,
        /* maxLiveTVProgramChars    */  22,
        /* maxLiveTVChannelChars    */  18,

        /* sidebarMinWidth          */ 200,
        /* sidebarMaxWidth          */ 280,

        /* dialogWidth              */ 460,

        /* imageCacheSize           */ 120,

        /* libraryPageSize          */ 500,
        /* playlistTrackPageSize    */ 200,
        /* musicCarouselLimit       */ 150,

        /* posterRequestWidth       */ 360,
        /* posterRequestHeight      */ 540,
        /* squareRequestSize        */ 360,
        /* landscapeRequestWidth    */ 480,
        /* landscapeRequestHeight   */ 270,
        /* detailPosterRequestWidth */ 600,
        /* detailPosterRequestHeight*/ 900,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1920,
    };

    return isPortrait() ? portrait : landscape;
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

void launchThread(std::function<void()> task, std::size_t /*stackSize*/) {
    // Desktop platforms (Linux / macOS / Windows) have huge default
    // thread stacks (1-8 MB) and a well-tested std::thread implementation
    // — bare detach is fine here. stackSize hint ignored.
    std::thread([t = std::move(task)]() { t(); }).detach();
}

std::size_t maxConcurrentNetworkRequests() {
    return 16;
}

bool needsHardExit() {
    return false;
}

[[noreturn]] void hardExit(int code) {
    std::exit(code);
}

}  // namespace platform
}  // namespace vitaplex
