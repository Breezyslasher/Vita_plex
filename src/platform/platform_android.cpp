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
    // Android LANDSCAPE (TV box, tablet landscape, phone rotated):
    // 5 cols × 160px (+14px spacing) fits inside borealis's 1280-wide
    // virtual viewport after the sidebar, and keeps GPU texture pressure
    // low on mobile GPUs. Previous 6-column 200px layout overflowed and
    // the last poster of each row was clipped.
    static const ImageConstraints landscape = {
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
        /* landscapeRowHeight */ 185,
        /* squareRowHeight    */ 215,

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

        /* libraryPageSize          */ 1000,
        /* playlistTrackPageSize    */ 150,
        /* musicCarouselLimit       */ 100,

        /* posterRequestWidth       */ 320,
        /* posterRequestHeight      */ 480,
        /* squareRequestSize        */ 320,
        /* landscapeRequestWidth    */ 440,
        /* landscapeRequestHeight   */ 250,
        /* detailPosterRequestWidth */ 500,
        /* detailPosterRequestHeight*/ 750,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1080,
    };

    // Android PORTRAIT (phone). This is the dominant Android case. The
    // viewport is something like 720×1280 in virtual coords — narrower
    // than landscape so the same poster width that was 12% of the
    // landscape screen would be 22% in portrait. Re-tune from scratch:
    //
    //   - 3 columns of slightly larger covers fills the width without
    //     a giant void on either side
    //   - sidebar shrinks aggressively so the grid has room
    //   - LiveTV guide gets taller (we have the vertical real estate)
    //   - homeRowHeight grows to match the taller posters
    //   - dialogs / lists narrow to fit phone-width comfortably
    //   - text-truncation chars drop because narrower rows fit fewer
    static const ImageConstraints portrait = {
        /* posterWidth        */ 170,
        /* posterHeight       */ 255,
        /* squareCoverSize    */ 170,
        /* landscapeWidth     */ 220,
        /* landscapeHeight    */ 125,
        /* gridColumns        */   3,  // 5 -> 3 for narrow phone width
        /* gridCellSpacing    */  16,
        /* titleFontSize      */  15,
        /* subtitleFontSize   */  12,
        /* descriptionFontSize*/  12,
        /* homeTitleFontSize  */  28,
        /* homeSectionFontSize*/  20,
        /* homeRowHeight      */ 315,
        /* landscapeRowHeight */ 185,
        /* squareRowHeight    */ 230,

        /* listRowHeight            */  64,  // taller for finger taps
        /* livetvChannelCardWidth   */ 150,
        /* livetvChannelRowHeight   */ 130,
        /* livetvGuideHeight        */ 720,  // ~2x landscape — fill the height

        /* maxCellTitleChars        */  16,
        /* maxListTitleChars        */  60,
        /* maxLiveTVProgramChars    */  16,
        /* maxLiveTVChannelChars    */  14,

        /* sidebarMinWidth          */ 180,  // tighten so grid has room
        /* sidebarMaxWidth          */ 240,

        /* dialogWidth              */ 420,

        /* imageCacheSize           */  60,

        /* libraryPageSize          */ 1000,
        /* playlistTrackPageSize    */ 150,
        /* musicCarouselLimit       */ 100,

        /* posterRequestWidth       */ 340,
        /* posterRequestHeight      */ 510,
        /* squareRequestSize        */ 340,
        /* landscapeRequestWidth    */ 440,
        /* landscapeRequestHeight   */ 250,
        /* detailPosterRequestWidth */ 540,
        /* detailPosterRequestHeight*/ 810,
        /* photoRequestWidth        */ 1080,  // phone in portrait
        /* photoRequestHeight       */ 1920,
    };

    return isPortrait() ? portrait : landscape;
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
