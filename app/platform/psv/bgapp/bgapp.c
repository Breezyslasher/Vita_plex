/*
 * VitaPlex PS Vita background helper — Phase-0 heartbeat proof.
 *
 * A SECOND, tiny executable (eboot2.bin) bundled inside the VitaPlex VPK and
 * launched by the main app via sceBgAppUtilStartBgApp(). It's marked as a Vita
 * "background application" purely through its self boot param — Control Info
 * section 6 ATTRIBUTE = 0x03 + a small MEMSIZE — which vitasdk's vita-make-fself
 * writes directly (vita_create_self ATTRIBUTE/MEMSIZE). No DolceSDK needed:
 * everything here uses the same vitasdk toolchain as the main app.
 *
 * Phase 0 plays NO audio. Its only job is the one make-or-break question for
 * the whole background-music design:
 *
 *     Does a Vita background app, launched from VitaPlex, keep RUNNING
 *     after the user presses PS to send VitaPlex to LiveArea?
 *
 * It answers that by writing a once-per-second heartbeat to
 * ux0:data/VitaPlex/bgapp.log. If the tick count keeps climbing in that log
 * while VitaPlex is backgrounded, the resident-background-process foundation
 * works and SceShell audio can be layered on next. If the ticks stop the
 * instant VitaPlex is backgrounded, the architecture is dead — cheaply.
 *
 * Resident while(1)+PowerTick loop follows GrapheneCt's BGFTP background
 * service pattern; the BG-notification init matches BGFTP/BG-App-PSV.
 */

#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

/* In SceNotificationUtil's stub (which vitasdk ships); declared here so we
   don't depend on the exact header path — the stub resolves it at link time. */
extern int sceNotificationUtilBgAppInitialize(void);

#define LOG_PATH "ux0:data/VitaPlex/bgapp.log"

static void log_append(const char *s)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0)
    {
        sceIoWrite(fd, s, strlen(s));
        sceIoClose(fd);
    }
}

int main(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaPlex", 0777);

    /* Register as a background app (matches BGFTP / BG-App-PSV). */
    sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    sceNotificationUtilBgAppInitialize();

    log_append("[bgapp] ===== started (phase-0 heartbeat, vitasdk) =====\n");

    char buf[160];
    int tick = 0;
    for (;;)
    {
        /* Keep the system from auto-suspending us while we're the BG owner. */
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        snprintf(buf, sizeof(buf), "[bgapp] heartbeat tick=%d\n", tick++);
        log_append(buf);

        sceKernelDelayThread(1000000); /* 1 s */
    }

    return 0;
}
