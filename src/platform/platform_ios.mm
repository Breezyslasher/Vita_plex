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
#import <UIKit/UIKit.h>
#import <AVKit/AVKit.h>
#import <CoreMedia/CoreMedia.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_hints.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

// SDL's iOS main() (SDL2main) starts UIKit and then calls SDL_main, so the
// app's entry point lives here, as on Android, and src/main.cpp defines no
// main() on this port.
extern "C" int VitaPlexMainEntry(int argc, char* argv[]);

extern "C" int SDL_main(int argc, char* argv[]) {
    return VitaPlexMainEntry(argc, argv);
}

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
        /* supportsHevc     */ true,  // Apple TV decodes HEVC in hardware
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
        /* supportsHevc     */ true,  // iOS devices decode HEVC in hardware
    };
#endif
    return v;
}

bool supports4KDecode() {
    // Left off deliberately. Most current iPhones and the Apple TV 4K do decode
    // 2160p, but the 4th-gen Apple TV does not and this port already caps its
    // constraints at 1080p pending HEVC decoder negotiation (see above). Turn
    // this on together with that work, on a device it can be tested against.
    return false;
}

// The frame rate alone: the Apple TV takes it with the dynamic range through
// setPreferredVideoMode; an iPhone's display picks its own rate.
void setPreferredRefreshRate(float fps) {
    setPreferredVideoMode(fps, VideoDynamicRange::SDR);
}
bool displaySupportsHdr() { return false; }
// No way to ask what the audio sink accepts, so everything is decoded to PCM
// exactly as before.
int passthroughCodecs() { return 0; }
// No platform caption preferences here, so subtitles keep the app's styling.
const CaptionStyle& getSystemCaptionStyle() {
    static const CaptionStyle none;
    return none;
}
// vitaplex:// links (Info.plist CFBundleURLTypes). SDL hands a URL the app
// is opened with over as an SDL_DROPFILE event, which the watch in init()
// passes here; MainActivity opens the item, as on desktop and Android.
namespace {
std::string g_pendingDeepLink;
std::function<void()> g_deepLinkHandler;
}  // namespace

void offerDeepLink(const std::string& url) {
    if (url.empty()) return;
    brls::Logger::info("Deep link: {}", url);
    g_pendingDeepLink = url;
    if (g_deepLinkHandler) g_deepLinkHandler();
}

std::string takePendingDeepLink() {
    std::string out;
    out.swap(g_pendingDeepLink);
    return out;
}

void setDeepLinkHandler(std::function<void()> onLinkArrived) {
    g_deepLinkHandler = std::move(onLinkArrived);
    // A link that launched the app is already waiting by the time
    // MainActivity registers.
    if (g_deepLinkHandler && !g_pendingDeepLink.empty()) g_deepLinkHandler();
}

static UIWindow* keyWindow();

// Apple TV: ask the TV for the video's frame rate and dynamic range (it
// switches only with Settings > Video and Audio > Match Content on).
// Needs tvOS 17's AVDisplayCriteria initializer; older systems keep their
// mode. iPhone: nothing to do.
void setPreferredVideoMode(float fps, VideoDynamicRange range) {
#if TARGET_OS_TV
    if (@available(tvOS 17.0, *)) {
        @autoreleasepool {
            UIWindow* window = keyWindow();
            if (!window) return;
            AVDisplayManager* dm = window.avDisplayManager;
            if (!dm.displayCriteriaMatchingEnabled) return;  // Match Content is off
            if (fps <= 0.0f) {
                dm.preferredDisplayCriteria = nil;
                return;
            }
            NSDictionary* ext = nil;
            if (range != VideoDynamicRange::SDR) {
                ext = @{
                    (id)kCMFormatDescriptionExtension_TransferFunction:
                        (range == VideoDynamicRange::HLG
                             ? (id)kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG
                             : (id)kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ),
                    (id)kCMFormatDescriptionExtension_ColorPrimaries:
                        (id)kCMFormatDescriptionColorPrimaries_ITU_R_2020,
                    (id)kCMFormatDescriptionExtension_YCbCrMatrix:
                        (id)kCMFormatDescriptionYCbCrMatrix_ITU_R_2020,
                };
            }
            CMVideoFormatDescriptionRef fd = NULL;
            if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_HEVC, 3840,
                                               2160, (__bridge CFDictionaryRef)ext,
                                               &fd) != noErr || !fd)
                return;
            dm.preferredDisplayCriteria =
                [[AVDisplayCriteria alloc] initWithRefreshRate:fps formatDescription:fd];
            CFRelease(fd);
            brls::Logger::info("Display: asked for {} fps, {}", fps,
                               range == VideoDynamicRange::SDR   ? "SDR"
                               : range == VideoDynamicRange::HLG ? "HLG"
                                                                 : "HDR10");
        }
    }
#else
    (void)fps;
    (void)range;
#endif
}

static UIWindow* keyWindow() {
    for (UIWindow* w in [UIApplication sharedApplication].windows) {
        if (w.isKeyWindow) return w;
    }
    return [UIApplication sharedApplication].windows.firstObject;
}

bool canSetScreenBrightness() {
#if TARGET_OS_TV
    return false;
#else
    return true;
#endif
}

float getScreenBrightness() {
#if TARGET_OS_TV
    return 0.0f;
#else
    return (float)[UIScreen mainScreen].brightness;
#endif
}

void setScreenBrightness(float value) {
#if !TARGET_OS_TV
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    [UIScreen mainScreen].brightness = value;
#else
    (void)value;
#endif
}

bool canShowAudioRoutePicker() {
#if TARGET_OS_TV
    return false;
#else
    return true;
#endif
}

// iOS has no call that opens the AirPlay picker by itself, only the
// AVRoutePickerView button. Keep a nearly invisible one in the window and
// press it.
void showAudioRoutePicker() {
#if !TARGET_OS_TV
    @autoreleasepool {
        static AVRoutePickerView* picker = nil;
        UIWindow* window = keyWindow();
        if (!window) return;
        if (!picker) {
            picker = [[AVRoutePickerView alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
            picker.alpha = 0.01;
        }
        if (picker.superview != window) [window addSubview:picker];
        for (UIView* v in picker.subviews) {
            if ([v isKindOfClass:[UIButton class]]) {
                [(UIButton*)v sendActionsForControlEvents:UIControlEventTouchUpInside];
                return;
            }
        }
        brls::Logger::warning("AirPlay picker: no button to press");
    }
#endif
}

// The window edges iOS reserves (the notch or Dynamic Island, the home
// indicator, rounded corners), converted to the units borealis's window size
// is in. Without it the sidebar ran under the Dynamic Island in landscape.
// Called on the main thread from Application::setWindowSize, so after every
// rotation too.
static void iosSafeAreaInsets(int width, int height, float* left, float* top, float* right,
                              float* bottom) {
    @autoreleasepool {
        UIWindow* window = nil;
        window = keyWindow();
        if (!window || window.bounds.size.width <= 0 || window.bounds.size.height <= 0) return;
        const UIEdgeInsets in = window.safeAreaInsets;
        const CGFloat sx = width / window.bounds.size.width;
        const CGFloat sy = height / window.bounds.size.height;
        *left   = (float)(in.left * sx);
        *right  = (float)(in.right * sx);
        *top    = (float)(in.top * sy);
        *bottom = (float)(in.bottom * sy);
        brls::Logger::info("Safe area: left {} top {} right {} bottom {} (of {}x{})", *left, *top,
                           *right, *bottom, width, height);
    }
}

bool init() {
    openLogFile();
    // A vitaplex:// URL the app is opened with arrives as SDL_DROPFILE.
    SDL_AddEventWatch(
        [](void*, SDL_Event* e) -> int {
            if (e->type == SDL_DROPFILE && e->drop.file) {
                const std::string url = e->drop.file;
                if (url.rfind("vitaplex://", 0) == 0 || url.rfind("plex://", 0) == 0)
                    offerDeepLink(url);
            }
            return 0;
        },
        nullptr);
#if TARGET_OS_TV
    // The Siri Remote as a remote, not a gamepad. Opened as a joystick (SDL's
    // default) its touch surface is an analog stick: a resting thumb keeps
    // the focus moving and swipes overshoot. Off, SDL sends one arrow key
    // per swipe, Enter for a click and Menu for the Back button, which is
    // how Apple TV apps move. Must be set before SDL starts.
    SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
    // Nor a mouse: SDL makes a pointer out of the touch surface by default.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    // What the remote sends, in the log: key presses only (the touch surface
    // reports every movement and would flood it).
    SDL_AddEventWatch(
        [](void*, SDL_Event* e) -> int {
            if (e->type == SDL_KEYDOWN && !e->key.repeat)
                brls::Logger::info("Remote: key {} pressed",
                                   SDL_GetScancodeName(e->key.keysym.scancode));
            return 0;
        },
        nullptr);

    // Back (Menu) on the main screen leaves the app, as on every Apple TV
    // app: while the highlight is on the sidebar of the only screen open,
    // SDL lets the Menu press through to the system; anywhere else the app
    // gets it as Back. SDL watches this hint and applies it at once.
    brls::Application::getRunLoopEvent()->subscribe([]() {
        static bool exitOnMenu = false;
        bool want = false;
        if (brls::Application::getActivitiesStack().size() == 1)
            want = dynamic_cast<brls::SidebarItem*>(brls::Application::getCurrentFocus()) != nullptr;
        if (want != exitOnMenu) {
            exitOnMenu = want;
            SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, want ? "1" : "0");
        }
    });
#endif
    brls::Application::safeAreaInsetsProvider = iosSafeAreaInsets;
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

// The log goes to Documents/vitaplex.log, which the Files app shows under
// On My iPhone > VitaPlex (UIFileSharingEnabled in Info.plist), so it can be
// sent in without a Mac. The previous run's is kept as vitaplex.prev.log.
// Written and flushed line by line: what matters most on a new port is the
// last line before a crash.
namespace {
FILE* g_logFile = nullptr;
std::mutex g_logMutex;
}  // namespace

void openLogFile() {
    if (g_logFile) return;
    const std::string path = getLogPath();
    const std::string prev = getIosDataDir() + "/vitaplex.prev.log";
    std::remove(prev.c_str());
    std::rename(path.c_str(), prev.c_str());
    g_logFile = std::fopen(path.c_str(), "w");
    if (!g_logFile) return;

    brls::Logger::getLogEvent()->subscribe(
        [](brls::Logger::TimePoint time, brls::LogLevel level, std::string log) {
            const char* levelStr = "UNKNOWN";
            switch (level) {
                case brls::LogLevel::LOG_ERROR:   levelStr = "ERROR";   break;
                case brls::LogLevel::LOG_WARNING: levelStr = "WARNING"; break;
                case brls::LogLevel::LOG_INFO:    levelStr = "INFO";    break;
                case brls::LogLevel::LOG_DEBUG:   levelStr = "DEBUG";   break;
                case brls::LogLevel::LOG_VERBOSE: levelStr = "VERBOSE"; break;
            }
            std::time_t tt = std::chrono::system_clock::to_time_t(time);
            std::tm tm = *std::localtime(&tt);
            const long ms = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       time.time_since_epoch()).count() % 1000);
            // Redacted here, as on the other ports: logs get pasted into bug
            // reports, and a token is a key to the whole server.
            const std::string line = ::vitaplex::redactTokensInUrl(log);
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (!g_logFile) return;
            std::fprintf(g_logFile, "%02d:%02d:%02d.%03ld [%s] %s\n", tm.tm_hour, tm.tm_min,
                         tm.tm_sec, ms, levelStr, line.c_str());
            std::fflush(g_logFile);
        });
    brls::Logger::info("Log file: {}", path);
}

void closeLogFile() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

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
#if TARGET_OS_TV
            // tvOS gives apps no persistent Documents folder: local storage
            // is Caches only (the system may clear it when space runs low).
            // Without this the settings, the sign-in and the log could not be
            // written on an Apple TV at all.
            NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
                NSCachesDirectory, NSUserDomainMask, YES);
#else
            NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
                NSDocumentDirectory, NSUserDomainMask, YES);
#endif
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
