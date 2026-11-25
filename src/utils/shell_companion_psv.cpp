/**
 * VitaPlex and vita-shell-companion's SceShell plugin: utils/
 * shell_companion_psv.hpp.
 */

#include "utils/shell_companion_psv.hpp"

#include <borealis.hpp>

#include <cstdio>
#include <mutex>

#include "shell_companion.h"

namespace vitaplex {
namespace shellcomp {

namespace {

// VitaPlex's data folder, where the plugin's files for it go, and its copy
// of the plugin in its package.
const char* const kDataDir = "ux0:data/VitaPlex";
const char* const kBundledPlugin = "app0:module/shell_companion.suprx";

std::once_flag g_initOnce;
// open's: one at a time, so that two parts of VitaPlex opening it at once
// load it once, and the logged answer is the one this process got.
std::mutex g_openMutex;
bool g_infoLogged = false;

}  // namespace

void init() {
    std::call_once(g_initOnce, []() {
        const int rc = scInit(VITAPLEX_TITLEID, kDataDir, "VitaPlex",
                              SC_FEAT_ROW | SC_FEAT_MUSIC | SC_FEAT_DOWNLOADS | SC_FEAT_UPDATE,
                              kBundledPlugin);
        if (rc < 0) brls::Logger::error("shell companion: scInit failed ({})", rc);
    });
}

bool open(const char* who) {
    init();
    std::lock_guard<std::mutex> lock(g_openMutex);
    if (scIsOpen()) return true;
    char what[320];
    const int rc = scOpen(what, sizeof(what));
    brls::Logger::info("{}: shell companion: {} ({:#x})", who, what, (unsigned)rc);
    if (rc < 0) return false;
    if (!g_infoLogged) {
        g_infoLogged = true;
        ScPluginInfo info;
        if (scPluginInfo(&info) == 0)
            brls::Logger::info("{}: shell companion: plugin {} (protocol {}), features {:#x}, "
                               "granted {:#x}, caps {}",
                               who, info.version, info.protocol, info.features, info.granted,
                               info.caps);
        else
            brls::Logger::info("{}: shell companion: the plugin did not say its version (older "
                               "than 0.2.0)",
                               who);
    }
    return true;
}

bool has(const char* cap) {
    ScPluginInfo info;
    return scIsOpen() && scPluginInfo(&info) == 0 && scPluginHas(&info, cap);
}

std::string path(const char* name) {
    init();
    char out[256];
    if (scPath(name, out, sizeof(out)) < 0) return std::string(kDataDir) + "/" + name;
    return out;
}

}  // namespace shellcomp
}  // namespace vitaplex
