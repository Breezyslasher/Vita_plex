/*
    VitaPlex — updater stub

    A tiny standalone Vita app whose ONLY job is to install a VitaPlex update
    while VitaPlex itself is closed, then relaunch it. This is the AutoPlugin2
    technique: a title cannot promote over itself (the Vita installer returns
    0x80101114 "in use" for the running app), so VitaPlex installs and launches
    THIS stub, which — now that VitaPlex is no longer the running title — can
    promote VitaPlex's downloaded VPK cleanly, then boots VitaPlex.

    Everything is fixed by convention so no arguments need to cross the app
    boundary:
      - the VPK to install : ux0:data/VitaPlex/update.vpk
      - the app to relaunch : VPLEX0001 (VitaPlex's title id)

    The heavy lifting (extract + head.bin forge + promote) is shared verbatim
    with VitaPlex via utils/vita_install, which is deliberately free of any
    borealis / fmt dependency so this stub links it without the UI stack. A
    minimal vita2d screen shows progress so the user isn't staring at black.
*/

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vita2d.h>

#include <cstring>
#include <string>

#include "utils/vita_install.hpp"

namespace {

const char* kUpdateVpk = "ux0:data/VitaPlex/update.vpk";
const char* kWorkDir   = "ux0:data/VitaPlex/updater_work";
const char* kTargetId  = "VPLEX0001";

vita2d_pgf* g_font = nullptr;

void log(const char* msg) {
    SceUID fd = sceIoOpen("ux0:data/VitaPlex/updater.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, std::strlen(msg));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

// One full draw cycle: dark background, gold title, status line, optional
// sub-line (used for the percentage). Centered-ish for the 960x544 screen.
void draw(const std::string& status, const std::string& sub) {
    if (!g_font) return;
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(g_font, 330, 250, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), 1.3f,
                         "VitaPlex Updater");
    vita2d_pgf_draw_text(g_font, 330, 292, RGBA8(0xE5, 0xA0, 0x0D, 0xFF), 1.0f,
                         status.c_str());
    if (!sub.empty())
        vita2d_pgf_draw_text(g_font, 330, 322, RGBA8(0xB4, 0xB4, 0xBA, 0xFF), 0.9f,
                             sub.c_str());
    vita2d_pgf_draw_text(g_font, 330, 372, RGBA8(0x6A, 0x6A, 0x70, 0xFF), 0.8f,
                         "Keep the system on - this only takes a moment.");
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void relaunchVitaPlex() {
    draw("Reopening VitaPlex\xE2\x80\xA6", "");
    vita::launchTitle(kTargetId);
}

}  // namespace

int main(int, char*[]) {
    log("stub: started");

    vita2d_init();
    vita2d_set_clear_color(RGBA8(0x14, 0x14, 0x18, 0xFF));
    g_font = vita2d_load_default_pgf();

    draw("Preparing\xE2\x80\xA6", "");

    // Give the shell a moment to finish tearing down VitaPlex so the promoter
    // no longer sees the title as in use.
    sceKernelDelayThread(3 * 1000 * 1000);

    SceIoStat st;
    if (sceIoGetstat(kUpdateVpk, &st) < 0) {
        log("stub: no update.vpk, relaunching");
        relaunchVitaPlex();
        sceKernelDelayThread(1000 * 1000);
        if (g_font) vita2d_free_pgf(g_font);
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }

    draw("Installing update\xE2\x80\xA6", "");
    std::string err;
    int rc = vita::installVpk(kUpdateVpk, kWorkDir, err,
        [](int done, int total) {
            int pct = total > 0 ? done * 100 / total : 0;
            draw("Installing update\xE2\x80\xA6", std::to_string(pct) + "%");
        });
    if (rc == 0) {
        log("stub: install ok");
        sceIoRemove(kUpdateVpk);
    } else {
        // Leave update.vpk in place so the user can still install it from
        // VitaShell; the shared log already recorded the reason.
        log(("stub: install failed: " + err).c_str());
        draw("Update failed", err);
        sceKernelDelayThread(4 * 1000 * 1000);
    }

    // Relaunch VitaPlex either way — the updated build on success, the
    // existing one on failure, so the user is never left staring at the
    // LiveArea wondering what happened.
    relaunchVitaPlex();
    sceKernelDelayThread(1000 * 1000);

    if (g_font) vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
