/**
 * VitaPlex platform layer — iOS / tvOS implementation (.mm Objective-C++).
 *
 * Selected by CMake when -DPLATFORM_IOS=ON or -DPLATFORM_TVOS=ON. Branches
 * internally on TARGET_OS_TV so the same TU compiles for both. We use
 * Objective-C++ specifically for the data-path lookup (NSFileManager) and
 * to read the bundle resource path for the log file.
 *
 * Resources are bundled via libromfs (RESOURCE_PREFIX="romfs:/", same as
 * Switch) — borealis embeds them at build time, so there is no runtime
 * resource copy step.
 */

#include "platform/platform.hpp"
#include "platform/paths.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <TargetConditionals.h>
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
#if TARGET_OS_TV
    // Apple TV runs at 1920x1080 (4K downsampled by tvOS for UI). It's a
    // living-room device viewed from couch distance, so use the desktop
    // landscape numbers verbatim — large covers, big fonts.
    static const ImageConstraints landscape = {
        /* posterWidth        */ 200,
        /* posterHeight       */ 300,
        /* squareCoverSize    */ 200,
        /* landscapeWidth     */ 280,
        /* landscapeHeight    */ 158,
        /* gridColumns        */   6,
        /* gridCellSpacing    */  20,
        /* titleFontSize      */  20,
        /* subtitleFontSize   */  16,
        /* descriptionFontSize*/  14,
        /* homeTitleFontSize  */  36,
        /* homeSectionFontSize*/  26,
        /* homeRowHeight      */ 360,
        /* landscapeRowHeight */ 220,
        /* squareRowHeight    */ 260,

        /* listRowHeight            */  72,
        /* livetvChannelCardWidth   */ 210,
        /* livetvChannelRowHeight   */ 160,
        /* livetvGuideHeight        */ 520,

        /* maxCellTitleChars        */  28,
        /* maxListTitleChars        */ 120,
        /* maxLiveTVProgramChars    */  30,
        /* maxLiveTVChannelChars    */  24,

        /* sidebarMinWidth          */ 300,
        /* sidebarMaxWidth          */ 500,

        /* dialogWidth              */ 640,

        /* imageCacheSize           */ 150,

        /* libraryPageSize          */ 500,
        /* playlistTrackPageSize    */ 200,
        /* musicCarouselLimit       */ 150,

        /* posterRequestWidth       */ 400,
        /* posterRequestHeight      */ 600,
        /* squareRequestSize        */ 400,
        /* landscapeRequestWidth    */ 560,
        /* landscapeRequestHeight   */ 315,
        /* detailPosterRequestWidth */ 700,
        /* detailPosterRequestHeight*/1050,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1080,
    };
    // tvOS has no portrait — but borealis still queries this getter twice.
    static const ImageConstraints portrait = landscape;
    return isPortrait() ? portrait : landscape;
#else
    // iPhone / iPad. Phones are usually portrait; iPads can rotate.
    // Numbers tuned for a 390-414pt iPhone width and ~1024pt iPad.
    static const ImageConstraints landscape = {
        /* posterWidth        */ 160,
        /* posterHeight       */ 240,
        /* squareCoverSize    */ 160,
        /* landscapeWidth     */ 220,
        /* landscapeHeight    */ 124,
        /* gridColumns        */   5,
        /* gridCellSpacing    */  14,
        /* titleFontSize      */  15,
        /* subtitleFontSize   */  12,
        /* descriptionFontSize*/  11,
        /* homeTitleFontSize  */  26,
        /* homeSectionFontSize*/  20,
        /* homeRowHeight      */ 290,
        /* landscapeRowHeight */ 180,
        /* squareRowHeight    */ 215,

        /* listRowHeight            */  60,
        /* livetvChannelCardWidth   */ 170,
        /* livetvChannelRowHeight   */ 130,
        /* livetvGuideHeight        */ 440,

        /* maxCellTitleChars        */  22,
        /* maxListTitleChars        */ 100,
        /* maxLiveTVProgramChars    */  24,
        /* maxLiveTVChannelChars    */  20,

        /* sidebarMinWidth          */ 230,
        /* sidebarMaxWidth          */ 400,

        /* dialogWidth              */ 500,

        /* imageCacheSize           */ 100,

        /* libraryPageSize          */ 300,
        /* playlistTrackPageSize    */ 150,
        /* musicCarouselLimit       */ 120,

        /* posterRequestWidth       */ 320,
        /* posterRequestHeight      */ 480,
        /* squareRequestSize        */ 320,
        /* landscapeRequestWidth    */ 440,
        /* landscapeRequestHeight   */ 248,
        /* detailPosterRequestWidth */ 600,
        /* detailPosterRequestHeight*/ 900,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1080,
    };

    static const ImageConstraints portrait = {
        /* posterWidth        */ 150,
        /* posterHeight       */ 225,
        /* squareCoverSize    */ 150,
        /* landscapeWidth     */ 200,
        /* landscapeHeight    */ 112,
        /* gridColumns        */   3,
        /* gridCellSpacing    */  10,
        /* titleFontSize      */  15,
        /* subtitleFontSize   */  12,
        /* descriptionFontSize*/  11,
        /* homeTitleFontSize  */  26,
        /* homeSectionFontSize*/  20,
        /* homeRowHeight      */ 285,
        /* landscapeRowHeight */ 175,
        /* squareRowHeight    */ 215,

        /* listRowHeight            */  60,
        /* livetvChannelCardWidth   */ 160,
        /* livetvChannelRowHeight   */ 125,
        /* livetvGuideHeight        */ 560,

        /* maxCellTitleChars        */  18,
        /* maxListTitleChars        */  70,
        /* maxLiveTVProgramChars    */  20,
        /* maxLiveTVChannelChars    */  16,

        /* sidebarMinWidth          */ 180,
        /* sidebarMaxWidth          */ 260,

        /* dialogWidth              */ 380,

        /* imageCacheSize           */ 100,

        /* libraryPageSize          */ 300,
        /* playlistTrackPageSize    */ 150,
        /* musicCarouselLimit       */ 120,

        /* posterRequestWidth       */ 300,
        /* posterRequestHeight      */ 450,
        /* squareRequestSize        */ 300,
        /* landscapeRequestWidth    */ 400,
        /* landscapeRequestHeight   */ 224,
        /* detailPosterRequestWidth */ 600,
        /* detailPosterRequestHeight*/ 900,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1920,
    };
    return isPortrait() ? portrait : landscape;
#endif
}

const VideoConstraints& getVideoConstraints() {
#if TARGET_OS_TV
    // Apple TV 4K can H.264/HEVC 4K, but conservatively cap at 1080p
    // until we wire HEVC decoder negotiation through Plex's transcode
    // params. tvos plexDevice string per Plex's recommended X-Plex-Device.
    static const VideoConstraints v = {
        /* plexPlatform     */ "tvOS",
        /* plexDevice       */ "Apple TV",
        /* maxVideoWidth    */ 1920,
        /* maxVideoHeight   */ 1080,
        /* maxVideoLevel    */ 51,
        /* defaultBitrate   */ 20000,
        /* defaultResolution*/ "1920x1080",
        /* defaultVideoQualityIndex */ 1,  // QUALITY_1080P
    };
#else
    static const VideoConstraints v = {
        /* plexPlatform     */ "iOS",
        /* plexDevice       */ "iPhone",
        /* maxVideoWidth    */ 1920,
        /* maxVideoHeight   */ 1080,
        /* maxVideoLevel    */ 51,
        /* defaultBitrate   */ 8000,
        /* defaultResolution*/ "1920x1080",
        /* defaultVideoQualityIndex */ 2,  // QUALITY_720P
    };
#endif
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
    return getIosDataDir() + "/vitaplex.log";
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
    // iOS / tvOS std::thread is libc++ on Apple's pthread — generous
    // default stack, kernel-managed; bare detach is fine.
    std::thread([t = std::move(task)]() { t(); }).detach();
}

std::size_t maxConcurrentNetworkRequests() {
    return 16;
}

bool needsHardExit() { return false; }
[[noreturn]] void hardExit(int code) { std::exit(code); }

}  // namespace platform
}  // namespace vitaplex

// Lives at the top level (matches the declaration in paths.hpp).
// iOS apps are sandboxed; the writable area is the per-app Documents
// directory under the container. NSFileManager resolves the absolute
// path on first call; cache it for the process lifetime.
const std::string& getIosDataDir() {
    static std::string s_dir;
    if (s_dir.empty()) {
        @autoreleasepool {
            NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
                NSDocumentDirectory, NSUserDomainMask, YES);
            if (paths.count > 0) {
                s_dir = [paths[0] UTF8String];
            } else {
                // Should never happen on a real device, but guard anyway.
                s_dir = "/tmp";
            }
        }
    }
    return s_dir;
}
