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
    // Desktop: 1080p+ screens. Use the largest cover budget so posters
    // remain crisp at typical viewing distances.
    static const ImageConstraints c = {
        /* posterWidth        */ 240,
        /* posterHeight       */ 360,
        /* squareCoverSize    */ 240,
        /* landscapeWidth     */ 320,
        /* landscapeHeight    */ 180,
        /* gridColumns        */   7,
        /* gridCellSpacing    */  20,
        /* titleFontSize      */  18,
        /* subtitleFontSize   */  14,
        /* descriptionFontSize*/  12,
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
