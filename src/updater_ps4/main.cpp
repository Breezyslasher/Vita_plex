/*
    VitaPlex — PS4 updater helper

    A tiny standalone PS4 app whose ONLY job is to install a VitaPlex update
    while VitaPlex itself is closed. This is the PS4 analogue of the Vita
    updater stub: a running title cannot be replaced in place — BGFT installs
    into /user/app/<titleid>, which is locked while the app runs, and the
    system reports 0x80990088 (SAME_APPLICATION_ALREADY_INSTALLED) if you try
    to register over it, and uninstalling your OWN running title just kills the
    process. So VitaPlex launches THIS helper and quits; with VitaPlex no
    longer running, the helper uninstalls VPLX00002 (its data under
    /data/VitaPlex is on a different partition and is untouched) and hands the
    downloaded pkg to the system installer (BGFT).

    It then shows a small progress screen while the install runs and relaunches
    VitaPlex once it completes, so an update never dumps the user back to the
    home screen (the Vita stub behaves the same way). The screen is drawn with
    plain SDL2 plus the app icon decoded via stb_image — no font stack, since
    the helper deliberately links none of the UI code.

    Everything is fixed by convention so no arguments cross the app boundary:
      - the pkg to install : /data/VitaPlex/update.pkg
      - the app to replace : VPLX00002 (VitaPlex's title id)

    The BGFT / AppInstUtil calls mirror utils/ps4_install.cpp but are kept
    self-contained here (file logging instead of borealis) so the helper links
    without the UI stack — the same split the Vita stub uses with vita_install.
*/

#ifdef __PS4__

#include <SDL2/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

extern "C" {

struct BgftInitParams {
    void*  heap;
    size_t heapSize;
};

// bgft_download_param / _ex — field order per LightningMods/PS4-Store's
// installpkg.h (see utils/ps4_install.cpp for the derivation).
struct BgftDownloadParam {
    int         userId;
    int         entitlementType;
    const char* id;
    const char* contentUrl;
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
struct BgftDownloadParamEx {
    BgftDownloadParam param;
    unsigned int      slot;
};

// SceBgftTaskProgress, field-for-field from flatz's bgft.h. Polled to drive a
// real progress bar AND to know when the install has actually finished —
// inferring completion from "the title exists" was wrong: BGFT creates
// /user/app/<titleid> immediately, so that reported done after 3 seconds.
struct BgftTaskProgress {
    unsigned int  bits;
    int           errorResult;
    unsigned long length;
    unsigned long transferred;
    unsigned long lengthTotal;
    unsigned long transferredTotal;
    unsigned int  numIndex;
    unsigned int  numTotal;
    unsigned int  restSec;
    unsigned int  restSecTotal;
    int           preparingPercent;
    int           localCopyPercent;
};

// Runtime module loading, exactly as borealis's Ps4Platform does for a
// SANDBOXED app (loadStartModuleFromSandbox + moduleDlsym). We do NOT call the
// linked BGFT/AppInstUtil stubs directly: linking resolves the symbol but the
// PRX is never actually loaded into this bare process, so the first call faults
// ("call to unpatched function" — what killed every build so far). Loading the
// PRX and resolving each function by name sidesteps that entirely.
int   sceKernelLoadStartModule(const char* path, size_t argc, const void* argv,
                               unsigned int flags, void* opt, int* res);
int   sceKernelDlsym(int handle, const char* name, void** func);
char* sceKernelGetFsSandboxRandomWord(void);

// Relaunching VitaPlex when the install finishes. Layout per idc's
// ps4-experiments-405; user_id -1 is the foreground user.
struct LaunchAppParam {
    unsigned int  size;
    int           userId;
    int           appAttr;
    int           enableCrashReport;
    unsigned long checkFlag;
};
}

namespace {

// Resolved at runtime from the loaded PRXs (see loadSysModule/resolve below).
int (*p_sceBgftServiceInit)(BgftInitParams*)                                 = nullptr;
int (*p_sceBgftServiceIntDownloadRegisterTaskByStorageEx)(BgftDownloadParamEx*, int*) = nullptr;
int (*p_sceBgftServiceDownloadStartTask)(int)                                = nullptr;
int (*p_sceAppInstUtilInitialize)(void)                                      = nullptr;
int (*p_sceAppInstUtilAppUnInstall)(const char*)                             = nullptr;
// Used to tell when the install has actually finished, so we can relaunch.
int  (*p_sceAppInstUtilAppExists)(const char*, int*)                         = nullptr;
bool (*p_sceAppInstUtilAppIsInInstalling)(const char*)                       = nullptr;
int  (*p_sceSystemServiceLaunchApp)(const char*, const char**, LaunchAppParam*) = nullptr;
int  (*p_sceBgftServiceDownloadGetProgress)(int, BgftTaskProgress*)          = nullptr;

// Our own (sandboxed) view of the pkg — used to stat it.
const char* kPkgPath  = "/data/VitaPlex/update.pkg";
// The SAME file as the system sees it. BGFT runs in a different process with a
// different filesystem root, so it cannot resolve our sandbox path: passing
// "/data/..." registered fine but then failed the transfer with CE-32918-3.
// flatz's BGFT writeup installs local pkgs from "/user/data/".
const char* kPkgPathSystem = "/user/data/VitaPlex/update.pkg";
const char* kLogPath  = "/data/VitaPlex/updater_ps4.log";
const char* kTargetId = "VPLX00002";

constexpr size_t kBgftHeapSize  = 1 * 1024 * 1024;
// SCE_BGFT_TASK_OPT_FORCE_UPDATE, per flatz's bgft.h. VitaPlex is closed while
// we run, so its /user/app dir isn't locked — ask the installer to replace the
// installed title in place. If it still refuses we fall back to uninstalling.
constexpr int    kOptForceUpdate = 0x8;

FILE* g_log = nullptr;

void vlog(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// ── Progress screen ──────────────────────────────────────────────────────
// Deliberately text-free: the helper links no font stack, so it shows the app
// icon over an indeterminate gold bar. Indeterminate is honest — we can tell
// when the install finishes, but not how far along it is.
SDL_Window*   g_win  = nullptr;
SDL_Renderer* g_ren  = nullptr;
SDL_Texture*  g_icon = nullptr;
int g_iconW = 0, g_iconH = 0;
int g_scrW = 1920, g_scrH = 1080;

void uiInit() {
    // The window MUST be created GL-capable, with the GLES2 attributes set
    // first — this is what borealis does before its own SDL_CreateWindow, and
    // borealis demonstrably renders on this hardware (VitaPlex logs
    // "OpenGL ES 2.0 Piglet"). Without SDL_WINDOW_OPENGL, SDL_CreateRenderer
    // still selects its 'opengles2' backend and every draw call returns
    // success, but nothing reaches the display: the previous build logged
    // setcolor=0 clear=0, rendercopy 0, present with no error — onto a blank
    // screen.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    g_win = SDL_CreateWindow("VitaPlex Updater", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, g_scrW, g_scrH,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!g_win) { vlog("ps4 updater: no window (%s)", SDL_GetError()); return; }
    SDL_GetWindowSize(g_win, &g_scrW, &g_scrH);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) { vlog("ps4 updater: no renderer (%s)", SDL_GetError()); return; }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    // Ships as a PLAIN file in our pkg, deliberately not sce_sys/icon0.png:
    // sce_sys is the package METADATA directory, so that icon is consumed by
    // the pkg tool and can't be opened at runtime — the fopen failed and this
    // function returned early, which is why the screen came up empty.
    FILE* f = std::fopen("/app0/updater_icon.png", "rb");
    if (!f) {
        vlog("ps4 updater: icon not found at /app0/updater_icon.png");
    } else {
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len > 0) {
            unsigned char* raw = (unsigned char*)std::malloc((size_t)len);
            if (raw && std::fread(raw, 1, (size_t)len, f) == (size_t)len) {
                int comp = 0;
                unsigned char* px =
                    stbi_load_from_memory(raw, (int)len, &g_iconW, &g_iconH, &comp, 4);
                if (px) {
                    // Go through a surface rather than CreateTexture +
                    // UpdateTexture: SDL then converts to whatever format the
                    // renderer actually wants. The manual path produced a
                    // texture that drew as a solid white block — the classic
                    // symptom of a texture that was created but never
                    // successfully filled (and neither call was error-checked).
                    // SDL_PIXELFORMAT_RGBA32 is the byte-order-correct alias
                    // for what stb_image hands back.
                    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
                        px, g_iconW, g_iconH, 32, g_iconW * 4, SDL_PIXELFORMAT_RGBA32);
                    if (!surf) {
                        vlog("ps4 updater: icon surface failed (%s)", SDL_GetError());
                    } else {
                        g_icon = SDL_CreateTextureFromSurface(g_ren, surf);
                        if (!g_icon)
                            vlog("ps4 updater: icon texture failed (%s)", SDL_GetError());
                        else
                            SDL_SetTextureBlendMode(g_icon, SDL_BLENDMODE_BLEND);
                        SDL_FreeSurface(surf);
                    }
                    stbi_image_free(px);
                } else {
                    vlog("ps4 updater: icon decode failed (%s)", stbi_failure_reason());
                }
            }
            std::free(raw);
        }
        std::fclose(f);
    }
    // Always logged, whatever happened above — a silent return here is exactly
    // what hid the previous failure.
    vlog("ps4 updater: ui ready %dx%d (icon %s)", g_scrW, g_scrH,
         g_icon ? "loaded" : "MISSING");
}

// Set once by the first fillRect/RenderCopy of the first frame so a failing
// draw call reports itself instead of silently producing a blank screen.
bool g_logDraw = false;

void fillRect(int x, int y, int w, int h, int r, int g, int b, int a) {
    SDL_Rect rc{ x, y, w, h };
    int e1 = SDL_SetRenderDrawColor(g_ren, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a);
    int e2 = SDL_RenderFillRect(g_ren, &rc);
    if (g_logDraw && (e1 != 0 || e2 != 0))
        vlog("ps4 updater: fillRect(%d,%d,%d,%d) setcolor=%d fill=%d (%s)",
             x, y, w, h, e1, e2, SDL_GetError());
}

// One frame. `t` is seconds since start (drives the indeterminate sweep);
// `fraction` is real install progress in 0..1, or <0 while BGFT hasn't
// reported a length yet.
void uiFrame(float t, float fraction = -1.0f) {
    if (!g_ren) return;

    // NOTE: an earlier revision wrapped the clear in
    // SDL_SetRenderDrawBlendMode(NONE) / (BLEND) to stop the system background
    // showing through. That is the only per-frame change between the build that
    // rendered (1570) and the one that came up black (1572), so it is reverted
    // here rather than kept on a hunch.
    int e1 = SDL_SetRenderDrawColor(g_ren, 0x14, 0x14, 0x16, 0xFF);
    int e2 = SDL_RenderClear(g_ren);

    // Report what the renderer actually does, once, instead of inferring it
    // from a photo of the TV.
    static bool s_firstFrame = true;
    if (s_firstFrame) {
        s_firstFrame = false;
        g_logDraw = true;   // let this frame's draw calls report failures
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(g_ren, &info) == 0)
            vlog("ps4 updater: renderer '%s' flags=0x%X maxtex=%dx%d",
                 info.name ? info.name : "?", (unsigned)info.flags,
                 info.max_texture_width, info.max_texture_height);
        else
            vlog("ps4 updater: SDL_GetRendererInfo failed (%s)", SDL_GetError());
        vlog("ps4 updater: frame1 setcolor=%d clear=%d (%s)", e1, e2, SDL_GetError());
    }

    const int cx = g_scrW / 2;

    // Panel behind the content. Deliberately well above the background in
    // brightness with a visible border — the first version used #1E1E21 on
    // #141416, two near-blacks that were indistinguishable on a TV and read as
    // a blank screen.
    const int panelW = g_scrW / 2;
    const int panelH = g_scrH / 2;
    const int panelX = cx - panelW / 2;
    const int panelY = g_scrH / 2 - panelH / 2;
    fillRect(panelX, panelY, panelW, panelH, 0x2A, 0x2A, 0x30, 0xFF);
    // 1px border, then a thick gold header so the screen is unmistakable.
    fillRect(panelX, panelY, panelW, 1, 0x3E, 0x3E, 0x46, 0xFF);
    fillRect(panelX, panelY + panelH - 1, panelW, 1, 0x3E, 0x3E, 0x46, 0xFF);
    fillRect(panelX, panelY, 1, panelH, 0x3E, 0x3E, 0x46, 0xFF);
    fillRect(panelX + panelW - 1, panelY, 1, panelH, 0x3E, 0x3E, 0x46, 0xFF);
    fillRect(panelX, panelY, panelW, 8, 0xE5, 0xA0, 0x0D, 0xFF);

    // A play mark drawn from rectangles rather than the app icon texture.
    // RenderCopy reports success on this GLES2/Piglet renderer but samples to a
    // solid white block, while plain filled rects draw correctly — so the mark
    // is built from rows of rects, which is guaranteed to show up.
    {
        const int markH = g_scrH / 6;
        const int markW = markH * 7 / 8;
        const int markX = cx - markW / 2;
        const int markY = g_scrH / 2 - markH - g_scrH / 40;
        const int rows  = markH / 2;               // 2px per row
        for (int i = 0; i < rows; ++i) {
            // Triangle: full width at the top row, tapering to a point at the
            // vertical centre, then mirrored for the bottom half.
            int fromCentre = (i < rows / 2) ? (rows / 2 - i) : (i - rows / 2);
            int w = markW - (markW * fromCentre) / (rows / 2);
            if (w > 0) fillRect(markX, markY + i * 2, w, 2, 0xE5, 0xA0, 0x0D, 0xFF);
        }
    }

    const int barW = g_scrW / 3;
    const int barH = 16;   // was 8: too thin to notice on a TV
    const int barX = cx - barW / 2;
    const int barY = g_scrH / 2 + g_scrH / 12;
    fillRect(barX, barY, barW, barH, 0xFF, 0xFF, 0xFF, 26);

    if (fraction >= 0.0f) {
        // Real progress from BGFT.
        int w = (int)(fraction * (float)barW);
        if (w < 2) w = 2;
        if (w > barW) w = barW;
        fillRect(barX, barY, w, barH, 0xE5, 0xA0, 0x0D, 0xFF);
    } else {
        // No length reported yet: a gold segment sweeping back and forth, so
        // the wait reads as busy rather than as a fake percentage.
        const int segW = barW / 4;
        float p = t * 0.45f;
        p = p - (float)((int)p);
        float pos = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
        int segX = barX + (int)(pos * (float)(barW - segW));
        fillRect(segX, barY, segW, barH, 0xE5, 0xA0, 0x0D, 0xFF);
    }

    SDL_RenderPresent(g_ren);
    if (g_logDraw) {
        vlog("ps4 updater: frame1 presented (panel %dx%d at %d,%d) err='%s'",
             panelW, panelH, panelX, panelY, SDL_GetError());
        g_logDraw = false;
    }
}

// The content id sits at offset 0x40 of the PKG header, 36 characters. Needed
// to ask whether that content is still installing.
bool readContentId(const char* pkgPath, char out[40]) {
    FILE* f = std::fopen(pkgPath, "rb");
    if (!f) return false;
    unsigned char magic[4] = {0};
    bool ok = std::fread(magic, 1, 4, f) == 4 &&
              magic[0] == 0x7F && magic[1] == 'C' && magic[2] == 'N' && magic[3] == 'T';
    if (ok) ok = std::fseek(f, 0x40, SEEK_SET) == 0 && std::fread(out, 1, 36, f) == 36;
    std::fclose(f);
    if (ok) out[36] = '\0';
    return ok;
}

// Load a system PRX the way a sandboxed app must: through the sandbox path
// /<random word>/common/lib/<name>, per borealis's loadStartModuleFromSandbox.
int loadSysModule(const char* name) {
    const char* word = sceKernelGetFsSandboxRandomWord();
    char path[256];
    std::snprintf(path, sizeof(path), "/%s/common/lib/%s", word ? word : "system", name);
    int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    vlog("ps4 updater: load %s -> handle %d", path, handle);
    return handle;
}

// Resolve one export; logs and reports failure so a missing symbol is visible
// instead of faulting on a null call.
bool resolve(int handle, const char* name, void** out) {
    int ret = sceKernelDlsym(handle, name, out);
    vlog("ps4 updater:   dlsym %s -> %d (%p)", name, ret, *out);
    return ret == 0 && *out != nullptr;
}

}  // namespace

int main(int, char*[]) {
    g_log = std::fopen(kLogPath, "w");
    vlog("ps4 updater: start");

    // MUST come before any system service call. borealis's Ps4Platform does
    // exactly this as its very first step, with the comment "Initialize here
    // for loading the system modules" — SDL_Init is what makes the process
    // able to reach the system libraries at all. Without it every privileged
    // call faults instantly: builds 1552/1553 died in
    // sceSysmoduleLoadModuleInternal and 1555 died in sceAppInstUtilInitialize,
    // and 1554 proved that LINKING SDL2 without calling SDL_Init changes
    // nothing. VitaPlex makes these same calls successfully because borealis
    // has already run this.
    int sdl = SDL_Init(SDL_INIT_VIDEO);
    vlog("ps4 updater: SDL_Init -> %d (%s)", sdl, sdl == 0 ? "ok" : SDL_GetError());

    // Give VitaPlex a moment to fully exit before we uninstall its title.
    sleep(3);

    struct stat st{};
    if (stat(kPkgPath, &st) != 0 || st.st_size <= 0) {
        vlog("ps4 updater: no pkg at %s (size=%lld) — nothing to do",
             kPkgPath, (long long)st.st_size);
        return 0;
    }
    vlog("ps4 updater: pkg %s size=%lld", kPkgPath, (long long)st.st_size);

    // NOTE: the window is deliberately NOT created here. Launched from the home
    // screen the screen drew correctly, but launched by VitaPlex it stayed
    // blank — the helper was creating its window before VitaPlex had finished
    // releasing the display, getting a context that reports every draw as a
    // success while presenting nothing. uiInit() is therefore called further
    // down, after the module loading, uninstall and register work, by which
    // point VitaPlex is long gone. See below.

    char contentId[40] = {0};
    if (!readContentId(kPkgPath, contentId))
        vlog("ps4 updater: could not read content id (install-finished check "
             "will rely on the title appearing)");
    else
        vlog("ps4 updater: content id %s", contentId);

    // Load the PRXs and resolve every function at runtime. Calling the LINKED
    // stubs is what killed builds 1552-1556: the symbol resolves at link time
    // but the PRX is never loaded into this bare process, so the first call
    // faults ("call to unpatched function"). borealis loads its system modules
    // the same way for a sandboxed app, and VitaPlex only gets away with direct
    // calls because borealis has already loaded these modules in-process.
    int hAppInst = loadSysModule("libSceAppInstUtil.sprx");
    int hBgft    = loadSysModule("libSceBgft.sprx");
    if (hAppInst <= 0 || hBgft <= 0) {
        vlog("ps4 updater: cannot load the installer modules — aborting "
             "(pkg kept at %s for a manual install)", kPkgPath);
        return 1;
    }

    // Optional extras: used to wait for the install and relaunch VitaPlex. If
    // any of these don't resolve we still install, just without the relaunch.
    int hSysSvc = loadSysModule("libSceSystemService.sprx");
    resolve(hAppInst, "sceAppInstUtilAppExists", (void**)&p_sceAppInstUtilAppExists);
    resolve(hAppInst, "sceAppInstUtilAppIsInInstalling",
            (void**)&p_sceAppInstUtilAppIsInInstalling);
    if (hSysSvc > 0)
        resolve(hSysSvc, "sceSystemServiceLaunchApp", (void**)&p_sceSystemServiceLaunchApp);
    resolve(hBgft, "sceBgftServiceDownloadGetProgress",
            (void**)&p_sceBgftServiceDownloadGetProgress);

    bool ok =
        resolve(hAppInst, "sceAppInstUtilInitialize",  (void**)&p_sceAppInstUtilInitialize) &
        resolve(hAppInst, "sceAppInstUtilAppUnInstall", (void**)&p_sceAppInstUtilAppUnInstall) &
        resolve(hBgft,    "sceBgftServiceInit",         (void**)&p_sceBgftServiceInit) &
        resolve(hBgft,    "sceBgftServiceIntDownloadRegisterTaskByStorageEx",
                (void**)&p_sceBgftServiceIntDownloadRegisterTaskByStorageEx) &
        resolve(hBgft,    "sceBgftServiceDownloadStartTask",
                (void**)&p_sceBgftServiceDownloadStartTask);
    if (!ok) {
        vlog("ps4 updater: some installer functions did not resolve — aborting "
             "(pkg kept at %s for a manual install)", kPkgPath);
        return 1;
    }

    vlog("ps4 updater: AppInstUtilInitialize...");
    int ai = p_sceAppInstUtilInitialize();
    vlog("ps4 updater: AppInstUtilInitialize -> 0x%08X", (unsigned)ai);

    void* heap = std::calloc(1, kBgftHeapSize);
    vlog("ps4 updater: heap=%p", heap);
    if (!heap) { vlog("ps4 updater: out of memory"); return 1; }
    BgftInitParams initParams{};
    initParams.heap     = heap;
    initParams.heapSize = kBgftHeapSize;
    int bi = p_sceBgftServiceInit(&initParams);
    vlog("ps4 updater: BgftServiceInit -> 0x%08X", (unsigned)bi);

    BgftDownloadParamEx paramsEx{};
    paramsEx.slot                   = 0;
    paramsEx.param.entitlementType  = 5;
    paramsEx.param.id               = "";
    paramsEx.param.contentUrl       = kPkgPathSystem;
    paramsEx.param.contentName      = "VitaPlex";
    paramsEx.param.option           = kOptForceUpdate;
    paramsEx.param.playgoScenarioId = "0";

    // Try to replace the installed title IN PLACE first, without uninstalling.
    // Only if the installer refuses with SAME_APPLICATION_ALREADY_INSTALLED do
    // we uninstall and retry. That way a failed transfer can't leave the
    // console with no VitaPlex at all — which is exactly what happened when the
    // uninstall ran first and the install then failed (CE-32918-3).
    vlog("ps4 updater: register (force-update, no uninstall) url=%s", kPkgPathSystem);
    int taskId = -1;
    int ret = p_sceBgftServiceIntDownloadRegisterTaskByStorageEx(&paramsEx, &taskId);
    vlog("ps4 updater: register -> 0x%08X (task %d)", (unsigned)ret, taskId);

    if (static_cast<unsigned>(ret) == 0x80990088u ||
        static_cast<unsigned>(ret) == 0x80990015u) {
        vlog("ps4 updater: installer wants the old title gone; uninstalling %s...", kTargetId);
        int un = p_sceAppInstUtilAppUnInstall(kTargetId);
        vlog("ps4 updater: uninstall %s -> 0x%08X", kTargetId, (unsigned)un);
        if (un == 0) {
            ret = p_sceBgftServiceIntDownloadRegisterTaskByStorageEx(&paramsEx, &taskId);
            vlog("ps4 updater: register (after uninstall) -> 0x%08X (task %d)",
                 (unsigned)ret, taskId);
        }
    }

    if (ret == 0) {
        ret = p_sceBgftServiceDownloadStartTask(taskId);
        vlog("ps4 updater: start -> 0x%08X", (unsigned)ret);
    } else {
        vlog("ps4 updater: install did not start — pkg kept at %s for a manual install",
             kPkgPath);
    }

    // Show the progress screen while BGFT installs, then relaunch VitaPlex so
    // an update doesn't dump the user back to the home screen. We deliberately
    // do NOT uninstall ourselves: a title that removes its own running self is
    // killed mid-call and the system reports it as a crash (CE-36329-3).
    // VitaPlex removes this helper on its next start (ps4::removeUpdaterApp),
    // where it's just another title and goes quietly.
    if (ret == 0) {
        // Create the window HERE, not at startup. Launched from the home screen
        // the screen drew fine, but launched by VitaPlex it was blank: the
        // helper was building its window while VitaPlex still held the display,
        // producing a context that reports every draw as a success and presents
        // nothing. By this point the initial sleep, the module loads, the
        // uninstall and two register calls have all run, so VitaPlex is long
        // gone and the display is free.
        uiInit();

        const Uint32 t0 = SDL_GetTicks();
        const Uint32 kTimeoutMs = 15 * 60 * 1000;  // generous; big pkg, slow HDD
        bool installed = false;
        bool sawTask   = false;   // the task was observed running at least once
        bool taskGone  = false;   // ...and has since disappeared = transfer done
        float fraction = -1.0f;   // <0 until BGFT reports a real length
        Uint32 lastPoll = 0;

        while (SDL_GetTicks() - t0 < kTimeoutMs) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) { /* drain; no input is acted on */ }
            uiFrame((float)(SDL_GetTicks() - t0) / 1000.0f, fraction);

            // Poll about once a second, not every frame.
            Uint32 now = SDL_GetTicks();
            if (now - lastPoll >= 1000) {
                lastPoll = now;

                // Track the actual BGFT task. Do NOT infer completion from
                // "the title exists": BGFT creates /user/app/<titleid> up front,
                // so that reported success 3s into a 99MB install and the
                // relaunch then failed with 0x80A40012.
                if (p_sceBgftServiceDownloadGetProgress) {
                    BgftTaskProgress prog{};
                    int pr = p_sceBgftServiceDownloadGetProgress(taskId, &prog);
                    if (pr == 0) {
                        sawTask = true;
                        if (prog.errorResult != 0) {
                            vlog("ps4 updater: install failed (0x%08X)",
                                 (unsigned)prog.errorResult);
                            break;
                        }
                        if (prog.length > 0) {
                            fraction = (float)((double)prog.transferred /
                                               (double)prog.length);
                            if (fraction > 1.0f) fraction = 1.0f;
                        }
                    } else if (sawTask) {
                        // The task is finished and has been reaped.
                        taskGone = true;
                    }
                }

                // Only once the transfer is done, confirm the system has
                // finished installing the content before relaunching.
                if (taskGone || !p_sceBgftServiceDownloadGetProgress) {
                    int exists = 0;
                    bool present = p_sceAppInstUtilAppExists &&
                                   p_sceAppInstUtilAppExists(kTargetId, &exists) == 0 &&
                                   exists != 0;
                    bool busy = p_sceAppInstUtilAppIsInInstalling &&
                                p_sceAppInstUtilAppIsInInstalling(contentId);
                    if (present && !busy) { installed = true; break; }
                }
            }
            SDL_Delay(16);
        }
        vlog("ps4 updater: install %s after %us (sawTask=%d taskGone=%d)",
             installed ? "finished" : "did not finish",
             (unsigned)((SDL_GetTicks() - t0) / 1000), (int)sawTask, (int)taskGone);

        if (installed && p_sceSystemServiceLaunchApp) {
            // Let the system finish registering the freshly installed title
            // before launching it; relaunching the instant the transfer ends
            // is what produced CE-36329-3 / a failed launch.
            for (int i = 0; i < 5 * 60; ++i) {   // ~5s, still drawing
                uiFrame((float)(SDL_GetTicks() - t0) / 1000.0f, 1.0f);
                SDL_Delay(16);
            }
            const char* argv[] = { nullptr };
            LaunchAppParam param{};
            param.size   = sizeof(LaunchAppParam);
            param.userId = -1;
            int lr = p_sceSystemServiceLaunchApp(kTargetId, argv, &param);
            vlog("ps4 updater: relaunch %s -> %d", kTargetId, lr);
        }
    } else {
        sleep(2);
    }

    vlog("ps4 updater: done");
    return 0;
}

#endif  // __PS4__
