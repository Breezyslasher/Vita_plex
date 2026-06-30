/*
 * VitaPlex PS Vita background helper — Phase-0 heartbeat proof.
 *
 * This is a SECOND, tiny executable (eboot2.bin) bundled inside the VitaPlex
 * VPK and launched by the main (vitasdk) app via sceBgAppUtilStartBgApp().
 * Built with DolceSDK because it must be a "system mode" background app
 * (boot param attribute 0x03 + CATEGORY=gdd), which vitasdk can't produce.
 *
 * Phase 0 does NOT play audio yet. Its only job is to answer the one
 * make-or-break question for the whole background-music design:
 *
 *     Does a Vita background app, launched from VitaPlex, keep RUNNING
 *     after the user presses PS to send VitaPlex to LiveArea?
 *
 * It proves that by writing a once-per-second heartbeat to
 * ux0:data/VitaPlex/bgapp.log and popping a system notification every 30s.
 * If the heartbeat ticks keep incrementing in the log while VitaPlex is
 * backgrounded, the resident-background-process foundation works and audio
 * (SceShell music service) can be layered on next. If the ticks stop the
 * instant VitaPlex is backgrounded, the architecture is dead — cheaply.
 *
 * BG-notification init + send are taken (by NID) from GrapheneCt's
 * BG-App-PSV / BGFTP samples; the resident while(1)+PowerTick loop is the
 * BGFTP background-service pattern.
 */

#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/iofilemgr.h>
#include <stdio.h>
#include <string.h>

/* DolceSDK uses the older psp2 header layout: file I/O lives in
   <psp2/kernel/iofilemgr.h> (current vitasdk's <psp2/io/fcntl.h> isn't present
   here). Define the SCE_O_* open flags defensively in case this SDK's
   iofilemgr.h doesn't expose them. */
#ifndef SCE_O_WRONLY
#define SCE_O_WRONLY 0x0002
#endif
#ifndef SCE_O_APPEND
#define SCE_O_APPEND 0x0100
#endif
#ifndef SCE_O_CREAT
#define SCE_O_CREAT  0x0200
#endif

/* Not declared in DolceSDK headers — resolved via SceNotificationUtil.yml. */
extern int SceNotificationUtilBgApp_CBE814C1(void); /* sceNotificationUtilBgAppInitialize */
extern int SceNotificationUtil_DE6F33F4(const char *utf16le); /* send notification */

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

/* Widen an ASCII string to UTF-16LE in-place into out (needs (len+1)*2 bytes).
   The notification API takes a UTF-16LE buffer; ASCII widens trivially. */
static void ascii_to_utf16le(const char *in, char *out, int out_bytes)
{
    int i = 0;
    int max = (out_bytes / 2) - 1;
    for (; in[i] && i < max; i++)
    {
        out[i * 2] = in[i];
        out[i * 2 + 1] = 0;
    }
    out[i * 2] = 0;
    out[i * 2 + 1] = 0;
}

static void notify(const char *ascii)
{
    char u16[256] = { 0 };
    ascii_to_utf16le(ascii, u16, sizeof(u16));
    SceNotificationUtil_DE6F33F4(u16);
}

int main(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaPlex", 0777);

    sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    SceNotificationUtilBgApp_CBE814C1();

    log_append("[bgapp] ===== started (phase-0 heartbeat) =====\n");
    notify("VitaPlex background helper: started");

    char buf[160];
    int tick = 0;
    for (;;)
    {
        /* Keep the system from auto-suspending us while we're the BG owner. */
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        snprintf(buf, sizeof(buf), "[bgapp] heartbeat tick=%d\n", tick);
        log_append(buf);

        /* Visible proof every 30s that we're still alive (even backgrounded). */
        if (tick > 0 && (tick % 30) == 0)
        {
            snprintf(buf, sizeof(buf), "VitaPlex helper alive: %ds", tick);
            notify(buf);
        }

        tick++;
        sceKernelDelayThread(1000000); /* 1 s */
    }

    return 0;
}
