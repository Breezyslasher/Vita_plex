/*
    VitaPlex — PS4 in-app self-update

    Registers a downloaded PKG with the system's background-file-transfer
    service (BGFT), which installs it exactly like the Debug Settings
    package installer: system progress notification included. This is the
    technique of flatz's Remote Package Installer, which GoldHEN's Direct
    PKG installer builds on; it works on jailbroken firmware where fpkg
    (fake package) support is enabled.

    Neither libSceBgft nor libSceAppInstUtil has OpenOrbis headers — the
    prototypes and struct layouts below are Remote Package Installer's
    (BSD), the de-facto public interface of these modules.
*/

#ifdef __PS4__

#include "utils/ps4_install.hpp"

#include <orbis/Sysmodule.h>

#include <borealis/core/logger.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

struct BgftInitParams {
    void*  heap;
    size_t heapSize;
};

struct BgftDownloadParam {
    int         userId;
    int         entitlementType;
    const char* id;                 // content id, from the pkg header
    const char* contentUrl;         // a plain /data path works
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    int         option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
};

int sceBgftServiceInit(BgftInitParams* params);
int sceBgftServiceIntDownloadRegisterTaskByStorageEx(BgftDownloadParam* params, int* taskId);
int sceBgftServiceDownloadStartTask(int taskId);

int sceAppInstUtilInitialize(void);
}

namespace {

// BGFT wants a caller-provided service heap.
constexpr size_t kBgftHeapSize = 1 * 1024 * 1024;

// Register-task option: skip the CDN query-string handling — this is a
// local file, not a store download.
constexpr int kOptDisableCdnQueryParam = 0x10000;

// The content id sits at offset 0x40 of the PKG header, 36 characters.
bool readContentId(const std::string& pkgPath, char out[40]) {
    FILE* f = fopen(pkgPath.c_str(), "rb");
    if (!f) return false;
    unsigned char magic[4] = {0};
    bool ok = fread(magic, 1, 4, f) == 4 &&
              magic[0] == 0x7F && magic[1] == 'C' && magic[2] == 'N' && magic[3] == 'T';
    if (ok) ok = fseek(f, 0x40, SEEK_SET) == 0 && fread(out, 1, 36, f) == 36;
    fclose(f);
    if (ok) out[36] = '\0';
    return ok;
}

}  // namespace

namespace ps4 {

int installPkg(const std::string& pkgPath, const std::string& contentName, std::string& err) {
    brls::Logger::info("ps4: installing {}", pkgPath);

    char contentId[40] = {0};
    if (!readContentId(pkgPath, contentId)) {
        err = "not a valid PKG (bad header)";
        brls::Logger::error("ps4: {}", err);
        return -1;
    }

    // Returns uint32_t: 0 on success, an 0x8xxxxxxx error code otherwise.
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL) != 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT) != 0) {
        err = "cannot load the system installer modules";
        brls::Logger::error("ps4: {}", err);
        return -1;
    }
    // Init failures are tolerated below: both services report "already
    // initialized" on repeat calls (a retry after a failed attempt).
    sceAppInstUtilInitialize();

    static void* s_bgftHeap = nullptr;
    if (!s_bgftHeap) s_bgftHeap = std::calloc(1, kBgftHeapSize);
    if (!s_bgftHeap) {
        err = "out of memory for the installer service";
        return -1;
    }
    BgftInitParams initParams{};
    initParams.heap = s_bgftHeap;
    initParams.heapSize = kBgftHeapSize;
    sceBgftServiceInit(&initParams);

    BgftDownloadParam params{};
    params.entitlementType  = 5;   // fpkg — what every homebrew installer passes
    params.id               = contentId;
    params.contentUrl       = pkgPath.c_str();
    params.contentExUrl     = "";
    params.contentName      = contentName.c_str();
    params.iconPath         = "";
    params.skuId            = "";
    params.option           = kOptDisableCdnQueryParam;
    params.playgoScenarioId = "0";
    params.releaseDate      = "";
    params.packageType      = "";
    params.packageSubType   = "";
    params.packageSize      = 0;

    int taskId = -1;
    int ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&params, &taskId);
    if (ret != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "cannot register the install (0x%08X)", (unsigned)ret);
        err = buf;
        brls::Logger::error("ps4: {}", err);
        return -1;
    }
    ret = sceBgftServiceDownloadStartTask(taskId);
    if (ret != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "cannot start the install (0x%08X)", (unsigned)ret);
        err = buf;
        brls::Logger::error("ps4: {}", err);
        return -1;
    }

    brls::Logger::info("ps4: install task {} started for {}", taskId, contentId);
    return 0;
}

}  // namespace ps4

#endif  // __PS4__
