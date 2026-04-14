/**
 * VitaPlex platform layer — PlayStation 4 (PS4 / Orbis) implementation.
 *
 * Selected by CMake when -DPLATFORM_PS4=ON. Contains all the Orbis-specific
 * sysmodule loading, log file routing, and image-sizing constraints so the
 * rest of the codebase contains no #ifdef __PS4__ blocks.
 */

#include "platform/platform.hpp"

#include <orbis/Sysmodule.h>
#include <sys/stat.h>

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <chrono>
#include <ctime>
#include <thread>

namespace {

FILE* g_logFile = nullptr;

bool initPs4SysModules() {
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0) {
        brls::Logger::error("Cannot load PS4 net module");
    }
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0) {
        brls::Logger::error("Cannot load PS4 SSL module");
    }
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0) {
        brls::Logger::error("Cannot load PS4 HTTP module");
    }
    // Give PS4 network stack time to become ready.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return true;
}

}  // namespace

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // PS4 runs at 1080p with abundant VRAM. Use full-resolution covers so
    // posters look crisp on a TV. Grid columns scale with the wider screen.
    static const ImageConstraints c = {
        /* posterWidth        */ 220,
        /* posterHeight       */ 330,
        /* squareCoverSize    */ 220,
        /* landscapeWidth     */ 280,
        /* landscapeHeight    */ 160,
        /* gridColumns        */   7,
        /* gridCellSpacing    */  20,
        /* titleFontSize      */  18,
        /* subtitleFontSize   */  14,
        /* descriptionFontSize*/  12,
    };
    return c;
}

bool init() {
    initPs4SysModules();

    if (!::vitaplex::HttpClient::globalInit()) {
        brls::Logger::error("Failed to initialize curl");
        return false;
    }

    mkdir("/data/VitaPlex", 0777);
    openLogFile();
    return true;
}

void shutdown() {
    closeLogFile();
    ::vitaplex::HttpClient::globalCleanup();
}

std::string getLogPath() {
    return "/data/VitaPlex/vitaplex.log";
}

void openLogFile() {
    if (g_logFile) return;
    g_logFile = std::fopen(getLogPath().c_str(), "w");
    if (!g_logFile) return;
    setvbuf(g_logFile, NULL, _IOLBF, 0);

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
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0 || (std::size_t)size > maxBytes) {
        std::fclose(fp);
        return false;
    }

    out.resize((std::size_t)size);
    std::size_t read = std::fread(out.data(), 1, (std::size_t)size, fp);
    std::fclose(fp);
    return read == (std::size_t)size;
}

bool needsHardExit() {
    return false;
}

[[noreturn]] void hardExit(int code) {
    std::exit(code);
}

}  // namespace platform
}  // namespace vitaplex
