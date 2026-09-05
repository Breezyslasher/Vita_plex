/*
    VitaPlex — PS4 in-app self-update

    Registers a downloaded PKG with the system's background-file-transfer
    service (BGFT), which installs it exactly like the Debug Settings
    package installer: system progress notification included. This is the
    technique of flatz's Remote Package Installer, which GoldHEN's Direct
    PKG installer builds on; it works on jailbroken firmware where fpkg
    (fake package) support is enabled.

    OpenOrbis ships the libSceBgft stub but no header, so the prototypes and
    struct layouts below are copied verbatim from LightningMods/PS4-Store
    (Store/include/installpkg.h + source/installpkg.cpp) — an open-source
    PS4 homebrew store that self-updates over BGFT. The internal
    ...ByStorageEx register call takes bgft_download_param_ex (the plain
    param nested plus a `slot`), NOT the plain param — passing the plain
    struct is what returned 0x80990004 (INVALID_ARGUMENT).

    With that fixed the register then reports 0x80990088
    (SAME_APPLICATION_ALREADY_INSTALLED): the pkg's title is the running
    VitaPlex itself. PS4-Store clears this by uninstalling the title and
    retrying the register — but that only works when the target is NOT running.
    Uninstalling our OWN title terminates this process before it can register
    the reinstall (confirmed on-device: the app is removed and nothing
    reinstalls it), so we must not do it. There is no in-process BGFT call that
    installs over the running app, so on 0x80990088 we surface a clear message
    and let the caller fall back to a manual install of the downloaded pkg —
    nothing is uninstalled, so the app and its data stay put.
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

// bgft_download_param — field order per PS4-Store's installpkg.h.
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

// bgft_download_param_ex — what ...ByStorageEx actually takes: the plain
// param plus a slot. This wrapper is the fix for 0x80990004.
struct BgftDownloadParamEx {
    BgftDownloadParam param;
    unsigned int      slot;
};

// Only the "Service" family is exported by the OpenOrbis/pacbrew SceBgft
// stub (the plain sceBgftInitialize / sceBgftDownloadRegisterTask do not
// link). ...ByStorageEx registers a task for content already on local
// storage — exactly our /data pkg.
int sceBgftServiceInit(BgftInitParams* params);
int sceBgftServiceIntDownloadRegisterTaskByStorageEx(BgftDownloadParamEx* params, int* taskId);
int sceBgftServiceDownloadStartTask(int taskId);

int sceAppInstUtilInitialize(void);
// Is this title installed? Used to wait for the bundled updater helper to
// finish installing WITHOUT trying to launch it: every failed launch attempt
// pops a system "Cannot start the application" (CE-40841-7) dialog.
int sceAppInstUtilAppExists(const char* titleId, int* exists);
int sceAppInstUtilAppUnInstall(const char* titleId);

// Launch an installed title by id. Layout + call per idc/ps4-experiments-405
// (hostapp_launch): user_id -1 = the foreground user. From libSceSystemService.
struct LaunchAppParam {
    unsigned int  size;
    int           userId;
    int           appAttr;
    int           enableCrashReport;
    unsigned long checkFlag;
};
int sceSystemServiceLaunchApp(const char* titleId, const char** argv, LaunchAppParam* param);
}

namespace {

// BGFT wants a caller-provided service heap.
constexpr size_t kBgftHeapSize = 1 * 1024 * 1024;

// The task option PS4-Store passes for a local install: INVISIBLE (no
// separate download-progress popup; the install shows in the normal
// notification). NOT the CDN flag — that was wrong.
constexpr int kOptInvisible = 0x2;

// BGFT runs in its own system process with a different filesystem root, so it
// cannot resolve our sandbox paths: registering "/data/..." succeeds but the
// transfer then fails (CE-32918-3). Our "/data" is "/user/data" to the system —
// flatz's BGFT writeup installs local pkgs from there.
std::string systemPath(const std::string& sandboxPath) {
    if (sandboxPath.compare(0, 6, "/data/") == 0) return "/user" + sandboxPath;
    return sandboxPath;
}

// Title id of the bundled updater helper (see src/updater_ps4 + CMakeLists).
constexpr const char* kUpdaterTitle = "VPLX00003";

// Load + initialise the installer modules once. EVERY AppInstUtil/BGFT call
// must come after this: calling into a module the process hasn't loaded faults
// instantly (it took VitaPlex down with CE-34878-0 when sceAppInstUtilAppExists
// was called before this ran).
bool ensureInstallerModules() {
    static bool s_ready = false;
    if (s_ready) return true;
    // Returns 0 on success, an 0x8xxxxxxx error code otherwise.
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL) != 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT) != 0) {
        brls::Logger::error("ps4: cannot load the system installer modules");
        return false;
    }
    // Init failures are tolerated: the service reports "already initialized"
    // on repeat calls (e.g. a retry after a failed attempt).
    sceAppInstUtilInitialize();
    s_ready = true;
    return true;
}

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

    if (!ensureInstallerModules()) {
        err = "cannot load the system installer modules";
        return -1;
    }

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

    // Fill matches PS4-Store's installpkg.cpp for a local pkg: slot 0,
    // entitlement 5, INVISIBLE option, empty id, content_url + name, playgo
    // "0". Everything else stays zeroed (memset) — notably user_id,
    // package_type and package_sub_type, which that working self-updater does
    // NOT set.
    const std::string sysPath = systemPath(pkgPath);
    BgftDownloadParamEx paramsEx{};
    paramsEx.slot                   = 0;
    paramsEx.param.entitlementType  = 5;
    paramsEx.param.id               = "";
    paramsEx.param.contentUrl       = sysPath.c_str();
    paramsEx.param.contentName      = contentName.c_str();
    paramsEx.param.option           = kOptInvisible;
    paramsEx.param.playgoScenarioId = "0";

    int taskId = -1;
    int ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&paramsEx, &taskId);

    // 0x80990088 / 0x80990015 = SAME_APPLICATION_ALREADY_INSTALLED: the pkg's
    // title is the running VitaPlex itself. We deliberately do NOT uninstall
    // to clear it — uninstalling our own title terminates this process before
    // the reinstall can be registered (confirmed on-device: the app is removed
    // and nothing reinstalls it). No in-process BGFT call installs over the
    // running app, so report it plainly and let the caller fall back to a
    // manual install of the already-downloaded pkg. Nothing is uninstalled.
    if (static_cast<unsigned>(ret) == 0x80990088u ||
        static_cast<unsigned>(ret) == 0x80990015u) {
        err = "the running app can't replace itself (0x80990088)";
        brls::Logger::error("ps4: {}", err);
        return -1;
    }
    if (ret != 0) {
        char buf[80];
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

int installUpdaterApp(std::string& err) {
    // The helper's pkg ships inside VitaPlex's own pkg (see CMakeLists), so the
    // user only ever installs ONE app — the same trick the Vita build uses to
    // bundle its updater stub. Copy it out to /data (BGFT can read that as
    // /user/data) and hand it to the installer. It's a DIFFERENT title from
    // VitaPlex, so this install is not blocked by 0x80990088.
    const std::string src = "/app0/updater.pkg";
    const std::string dst = "/data/VitaPlex/updater.pkg";

    FILE* in = fopen(src.c_str(), "rb");
    if (!in) {
        err = "the bundled updater package is missing";
        brls::Logger::error("ps4: {} ({})", err, src);
        return -1;
    }
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        err = "cannot write the updater package";
        brls::Logger::error("ps4: {} ({})", err, dst);
        return -1;
    }
    static char buf[64 * 1024];
    size_t n;
    bool wrote = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { wrote = false; break; }
    }
    fclose(in);
    fclose(out);
    if (!wrote) {
        err = "cannot copy the updater package";
        brls::Logger::error("ps4: {}", err);
        return -1;
    }

    brls::Logger::info("ps4: installing the bundled updater app");
    return installPkg(dst, "VitaPlex Updater", err);
}

bool isUpdaterInstalled() {
    // Must not call into AppInstUtil before its module is loaded — doing so faulted the whole app (CE-34878-0).
    if (!ensureInstallerModules()) return false;
    int exists = 0;
    int ret = sceAppInstUtilAppExists(kUpdaterTitle, &exists);
    return ret == 0 && exists != 0;
}

void removeUpdaterApp() {
    if (!ensureInstallerModules()) return;
    int exists = 0;
    if (sceAppInstUtilAppExists(kUpdaterTitle, &exists) != 0 || exists == 0) return;
    int ret = sceAppInstUtilAppUnInstall(kUpdaterTitle);
    brls::Logger::info("ps4: removed leftover updater {} -> 0x{:08X}",
                       kUpdaterTitle, static_cast<unsigned>(ret));
}

int launchUpdater(std::string& err) {
    const char* argv[] = { nullptr };
    LaunchAppParam param{};
    param.size   = sizeof(LaunchAppParam);
    param.userId = -1;  // foreground user

    int ret = sceSystemServiceLaunchApp(kUpdaterTitle, argv, &param);
    // Success returns the launched app's id (>= 0); a negative value is an
    // error — most commonly the helper pkg is not installed.
    if (ret < 0) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "the updater app isn't installed (0x%08X)", (unsigned)ret);
        err = buf;
        brls::Logger::error("ps4: {}", err);
        return -1;
    }
    brls::Logger::info("ps4: launched updater {} (ret {})", kUpdaterTitle, ret);
    return 0;
}

}  // namespace ps4

#endif  // __PS4__
