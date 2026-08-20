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
    borealis / fmt dependency so this stub links it without the UI stack.
*/

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstring>
#include <string>

#include "utils/vita_install.hpp"

namespace {

const char* kUpdateVpk = "ux0:data/VitaPlex/update.vpk";
const char* kWorkDir   = "ux0:data/VitaPlex/updater_work";
const char* kTargetId  = "VPLEX0001";

void log(const char* msg) {
    SceUID fd = sceIoOpen("ux0:data/VitaPlex/updater.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, std::strlen(msg));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

void relaunchVitaPlex() {
    std::string uri = std::string("psgm:play?titleid=") + kTargetId;
    // 0xFFFFF is the flag homebrew launchers pass to sceAppMgrLaunchAppByUri.
    sceAppMgrLaunchAppByUri(0xFFFFF, const_cast<char*>(uri.c_str()));
    sceKernelDelayThread(200 * 1000);
}

}  // namespace

int main(int, char*[]) {
    log("stub: started");

    // Give the shell a moment to finish tearing down VitaPlex so the promoter
    // no longer sees the title as in use.
    sceKernelDelayThread(3 * 1000 * 1000);

    SceIoStat st;
    if (sceIoGetstat(kUpdateVpk, &st) < 0) {
        log("stub: no update.vpk, relaunching");
        relaunchVitaPlex();
        sceKernelExitProcess(0);
        return 0;
    }

    std::string err;
    int rc = vita::installVpk(kUpdateVpk, kWorkDir, err);
    if (rc == 0) {
        log("stub: install ok");
        sceIoRemove(kUpdateVpk);
    } else {
        // Leave update.vpk in place so the user can still install it from
        // VitaShell; the shared log already recorded the reason.
        log(("stub: install failed: " + err).c_str());
    }

    // Relaunch VitaPlex either way — the updated build on success, the
    // existing one on failure, so the user is never left staring at the
    // LiveArea wondering what happened.
    relaunchVitaPlex();
    sceKernelExitProcess(0);
    return 0;
}
