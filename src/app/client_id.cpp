/**
 * VitaPlex - this install's X-Plex-Client-Identifier
 *
 * openapi.json requires X-Plex-Client-Identifier on many calls and describes
 * it as "Unique per client". VitaPlex sent one fixed string from every
 * device, so a server (and plex.tv) took every Vita, phone and TV for the
 * same client: one device's playback state, transcode sessions and "now
 * playing" could be taken for another's.
 *
 * Made at first use, a random 128-bit value, and kept in the data folder
 * (client_id.txt) so it stays the same across launches and updates. A
 * missing or damaged file just gets a new one; the server then sees a new
 * device, which is all that changes.
 */

#include "app/application.hpp"
#include "platform/paths.hpp"

#include <borealis.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace vitaplex {

namespace {

bool looksValid(const std::string& id) {
    if (id.size() < 16 || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-')) return false;
    return true;
}

std::string makeId() {
    uint64_t a = 0, b = 0;
    try {
        std::random_device rd;
        a = ((uint64_t)rd() << 32) ^ rd();
        b = ((uint64_t)rd() << 32) ^ rd();
    } catch (...) {
    }
    // Mixed with the clock and an address, in case the random device is a
    // fixed sequence on a port.
    const uint64_t t = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 mix(t ^ a ^ (uint64_t)(uintptr_t)&a);
    a ^= mix();
    b ^= mix();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "vitaplex-%08x-%04x-%04x-%04x-%012llx",
                  (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xFFFF), (unsigned)(a & 0xFFFF),
                  (unsigned)(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string loadOrCreate() {
    const std::string path = platformPath("client_id.txt");
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[80] = {0};
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        std::string id(buf, n);
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
        if (looksValid(id)) return id;
    }
    const std::string id = makeId();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs(id.c_str(), f);
        std::fclose(f);
        brls::Logger::info("Plex client identifier made for this install");
    } else {
        brls::Logger::warning("Could not save the client identifier to {}", path);
    }
    return id;
}

}  // namespace

const std::string& plexClientId() {
    static const std::string id = loadOrCreate();   // once, thread-safe
    return id;
}

}  // namespace vitaplex
