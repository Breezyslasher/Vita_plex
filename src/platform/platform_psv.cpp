/**
 * VitaPlex platform layer — PlayStation Vita (PSV) implementation.
 *
 * Selected by CMake when -DPLATFORM_PSV=ON. Implements every function
 * declared in include/platform/platform.hpp using PSP2 native APIs.
 *
 * All Vita-specific bootstrap code (sysmodule loading, network init, clock
 * tuning, log file routing) lives here so the rest of the codebase contains
 * no #ifdef __vita__ blocks.
 */

#include "platform/platform.hpp"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <psp2/apputil.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <chrono>
#include <ctime>
#include <cstring>

// Memory configuration — Vita-specific globals required by the SDK.
// Reduced from 192 MB to 172 MB - leaves more room for GPU VRAM and system.
// With optimized view recycling and image caching, the app uses significantly
// less heap, so this headroom is no longer needed.
extern "C" {
int _newlib_heap_size_user = 172 * 1024 * 1024;          // 172 MB heap
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024; // 2 MB stack
}

namespace {

// Reduced network buffer from 4 MB to 2 MB - sufficient for API calls + thumbnails
constexpr int NET_MEMORY_SIZE  = 2 * 1024 * 1024;
constexpr int SSL_MEMORY_SIZE  = 512 * 1024;
constexpr int HTTP_MEMORY_SIZE = 1 * 1024 * 1024;

char __attribute__((aligned(64))) g_netMemory[NET_MEMORY_SIZE];

FILE* g_logFile = nullptr;

bool loadShaderCompiler() {
    SceUID mod = sceKernelLoadStartModule("ur0:data/libshacccg.suprx", 0, NULL, 0, NULL, NULL);
    if (mod >= 0) return true;

    mod = sceKernelLoadStartModule("vs0:sys/external/libshacccg.suprx", 0, NULL, 0, NULL, NULL);
    if (mod >= 0) return true;

    brls::Logger::warning("Could not load libshacccg.suprx - using fallback shaders");
    return true;
}

bool initVitaSystem() {
    brls::Logger::info("Initializing PS Vita system modules...");

    // App utilities
    SceAppUtilInitParam initParam;
    SceAppUtilBootParam bootParam;
    memset(&initParam, 0, sizeof(initParam));
    memset(&bootParam, 0, sizeof(bootParam));
    sceAppUtilInit(&initParam, &bootParam);

    // Start with reduced clock speeds for browsing (saves battery).
    // PlayerActivity raises clocks to max when playback starts,
    // and lowers them again when playback ends.
    scePowerSetArmClockFrequency(333);
    scePowerSetBusClockFrequency(166);
    scePowerSetGpuClockFrequency(166);
    scePowerSetGpuXbarClockFrequency(111);

    loadShaderCompiler();

    // Load system modules
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    // SceAvPlayer intentionally NOT loaded - we use MPV/libmpv for playback.
    // Loading SceAvPlayer pulls in SceMp4 as a dependency, and "Mp4Demux Critical"
    // appears in crash dumps during HLS playback, suggesting a conflict with
    // MPV's FFmpeg-based demuxer using the same SceAvcodec/SceVideodec resources.
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);

    brls::Logger::info("System modules loaded");
    return true;
}

bool initVitaNetwork() {
    brls::Logger::info("Initializing networking...");

    SceNetInitParam netInitParam;
    netInitParam.memory = g_netMemory;
    netInitParam.size   = NET_MEMORY_SIZE;
    netInitParam.flags  = 0;

    int ret = sceNetInit(&netInitParam);
    if (ret < 0 && ret != (int)0x80410201) {
        brls::Logger::error("sceNetInit failed: {:#x}", ret);
        return false;
    }

    ret = sceNetCtlInit();
    if (ret < 0 && ret != (int)0x80412102) {
        brls::Logger::error("sceNetCtlInit failed: {:#x}", ret);
        return false;
    }

    ret = sceSslInit(SSL_MEMORY_SIZE);
    if (ret < 0 && ret != (int)0x80435001) {
        brls::Logger::error("sceSslInit failed: {:#x}", ret);
        return false;
    }

    ret = sceHttpInit(HTTP_MEMORY_SIZE);
    if (ret < 0 && ret != (int)0x80431002) {
        brls::Logger::error("sceHttpInit failed: {:#x}", ret);
        return false;
    }

    ::vitaplex::HttpClient::globalInit();

    brls::Logger::info("Networking initialized");
    return true;
}

void cleanupVitaNetwork() {
    ::vitaplex::HttpClient::globalCleanup();
    sceHttpTerm();
    sceSslTerm();
    sceNetCtlTerm();
    sceNetTerm();
}

}  // namespace

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // PSV: 960x544 screen, ~115 MB usable heap. Keep covers small so that
    // the in-RAM thumbnail cache (~20 entries) fits comfortably and so that
    // the GXM renderer doesn't allocate huge textures.
    static const ImageConstraints c = {
        /* posterWidth        */ 110,
        /* posterHeight       */ 165,
        /* squareCoverSize    */ 110,
        /* landscapeWidth     */ 140,
        /* landscapeHeight    */  80,
        /* gridColumns        */   6,
        /* gridCellSpacing    */  10,
        /* titleFontSize      */  12,
        /* subtitleFontSize   */  10,
        /* descriptionFontSize*/   9,
        /* homeTitleFontSize  */  28,
        /* homeSectionFontSize*/  22,
        /* homeRowHeight      */ 210,  // 165 poster + ~45 for label/margins
    };
    return c;
}

const VideoConstraints& getVideoConstraints() {
    // Vita's SceAvcodec peaks at 960x544 H.264 High@L4.0 — anything above
    // that falls back to software decode and is unplayable. Keep the
    // bitrate modest so Plex transcodes efficiently on older servers.
    static const VideoConstraints v = {
        /* plexPlatform     */ "PlayStation Vita",
        /* plexDevice       */ "PS Vita",
        /* maxVideoWidth    */ 960,
        /* maxVideoHeight   */ 544,
        /* maxVideoLevel    */ 40,
        /* defaultBitrate   */ 2000,
        /* defaultResolution*/ "960x544",
        /* defaultVideoQualityIndex */ 3,  // QUALITY_480P
    };
    return v;
}

bool init() {
    if (!initVitaSystem()) {
        return false;
    }
    if (!initVitaNetwork()) {
        return false;
    }
    sceIoMkdir("ux0:data/VitaPlex", 0777);
    openLogFile();
    return true;
}

void shutdown() {
    closeLogFile();
    cleanupVitaNetwork();
}

std::string getLogPath() {
    return "ux0:data/VitaPlex/vitaplex.log";
}

void openLogFile() {
    if (g_logFile) return;
    g_logFile = std::fopen(getLogPath().c_str(), "w");
    if (!g_logFile) return;
    setvbuf(g_logFile, NULL, _IOLBF, 0);

    // Subscribe to log events to write to file (since brls::Logger::setLogOutput
    // doesn't work on Vita — it writes to sceClibPrintf which is invisible).
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
            std::tm time_tm = *std::localtime(&tt);
            uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              time.time_since_epoch())
                              .count() %
                          1000;

            fprintf(g_logFile, "%02d:%02d:%02d.%03d [%s] %s\n",
                    time_tm.tm_hour, time_tm.tm_min, time_tm.tm_sec,
                    (int)ms, levelStr, log.c_str());
        });
    brls::Logger::info("Log file initialized");
}

void closeLogFile() {
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

bool readLocalFile(const std::string& path,
                   std::vector<uint8_t>& out,
                   std::size_t maxBytes) {
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;

    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    if (size <= 0 || (std::size_t)size > maxBytes) {
        sceIoClose(fd);
        return false;
    }

    out.resize((std::size_t)size);
    int read = sceIoRead(fd, out.data(), size);
    sceIoClose(fd);
    return read == (int)size;
}

bool needsHardExit() {
    return true;
}

[[noreturn]] void hardExit(int code) {
    sceKernelExitProcess(code);
    // sceKernelExitProcess is [[noreturn]] but the toolchain doesn't know.
    while (true) {}
}

}  // namespace platform
}  // namespace vitaplex
