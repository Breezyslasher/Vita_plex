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
#include "platform/paths.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

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
        /* supportsHevc     */ true,  // desktop players decode HEVC natively
    };
    return v;
}

bool supports4KDecode() {
    // Software decode on a desktop CPU handles 2160p; mpv falls back to it when no hardware path exists.
    return true;
}

// The display mode is the OS/console's business on this port; VitaPlex neither
// switches refresh rates nor sees HDR capabilities here, so both stay no-ops.
void setPreferredRefreshRate(float) {}
bool displaySupportsHdr() { return false; }
// No way to ask what the audio sink accepts, so everything is decoded to PCM exactly as before.
int passthroughCodecs() { return 0; }
// No platform caption preferences here, so subtitles keep the app's styling.
const CaptionStyle& getSystemCaptionStyle() {
    static const CaptionStyle none;
    return none;
}
// Deep links arrive as argv[1] and are collected once the UI is up.
namespace {
std::string g_pendingDeepLink;
std::function<void()> g_deepLinkHandler;
}

void offerDeepLink(const std::string& url) {
    if (url.empty()) return;
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
    // A link that arrived on the command line is already waiting by the time
    // MainActivity registers, so tell it straight away.
    if (g_deepLinkHandler && !g_pendingDeepLink.empty()) g_deepLinkHandler();
}

#ifdef _WIN32
// The Windows build links the GUI subsystem, so double-clicking it no longer
// opens a console. That also means a build started FROM a console has nowhere
// to write: the standard handles are closed and every log line is discarded.
//
// AttachConsole borrows the parent's console when there is one, which is
// exactly the terminal the user typed into. It fails harmlessly when there
// isn't (double-clicked, or launched from Explorer or the Start Menu), and
// that failure is the normal case — hence no logging on the way out.
//
// Appending rather than truncating matters: two runs from the same console
// would otherwise overwrite each other's output.
void attachParentConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    std::freopen("CONOUT$", "a", stdout);
    std::freopen("CONOUT$", "a", stderr);
    std::freopen("CONIN$", "r", stdin);

    // borealis logs UTF-8; without this the console renders it as mojibake.
    SetConsoleOutputCP(CP_UTF8);

    // The shell printed its prompt the moment it launched a GUI-subsystem
    // process, so our first line would land on the same row as that prompt.
    std::fputc('\n', stdout);
}
#endif

bool init() {
#ifdef _WIN32
    attachParentConsole();
    openLogFile();
#endif
    if (!::vitaplex::HttpClient::globalInit()) {
        brls::Logger::error("Failed to initialize curl");
        return false;
    }
    return true;
}

void shutdown() {
    ::vitaplex::HttpClient::globalCleanup();
    closeLogFile();
}

#ifdef _WIN32
FILE* g_logFile = nullptr;
#endif

// Linux and macOS keep their log on stdout, where a terminal or the journal
// picks it up. Windows cannot: the app links the GUI subsystem, so a
// double-clicked build has no stdout to write to, and the console window that
// used to carry the log is the thing that was removed. Without a file there is
// no way for anyone to send a log at all.
std::string getLogPath() {
#ifdef _WIN32
    return platformPath("vitaplex.log");
#else
    return std::string{};
#endif
}

void openLogFile() {
#ifdef _WIN32
    if (g_logFile) return;
    std::error_code ec;
    std::filesystem::create_directories(getDesktopDataDir(), ec);

    // Truncated per run rather than appended: this is for "it just did the
    // wrong thing, send me the log", and an unbounded file that nobody ever
    // rotates is its own problem.
    g_logFile = std::fopen(getLogPath().c_str(), "w");
    if (!g_logFile) return;
    // Line-buffered, so a crash still leaves everything up to the last line.
    setvbuf(g_logFile, nullptr, _IOLBF, 0);

    brls::Logger::getLogEvent()->subscribe(
        [](brls::Logger::TimePoint time, brls::LogLevel level, std::string log) {
            if (!g_logFile) return;
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
            const uint64_t ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch())
                    .count() % 1000;
            std::fprintf(g_logFile, "%02d:%02d:%02d.%03d [%s] %s\n", tm.tm_hour, tm.tm_min,
                         tm.tm_sec, (int)ms, levelStr, log.c_str());
        });
    brls::Logger::info("Log file: {}", getLogPath());
#endif
}

void closeLogFile() {
#ifdef _WIN32
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
#endif
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
