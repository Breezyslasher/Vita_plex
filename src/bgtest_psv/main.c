/**
 * VitaPlex background test: a tiny app that beeps through the BGM audio port
 * and records whether it keeps running once the user leaves it.
 *
 * VitaPlex is frozen by the system the moment the PS button takes it out of
 * the foreground: its watcher thread logged no tick for the whole time away,
 * with mpv playing and the BGM port held. ElevenMPV-A keeps running in the
 * same situation. The difference to test is the param.sfo: this one binary is
 * packaged three times, identical except for that (CMakeLists.txt, "Vita
 * background test"), and shows which of the three it is.
 *
 *   VPLXBGT01  a game, set up like VitaPlex. Expected to freeze: the control.
 *   VPLXBGT02  a game with ATTRIBUTE_BG_APP (0x04000000) set.
 *   VPLXBGT03  a non-game application (gdc) with ElevenMPV-A's attributes.
 *
 * Nothing here is VitaPlex code; if one of them keeps beeping, the finding
 * goes into VitaPlex (or a helper app) and this goes away.
 */

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Small, so the non-game variant fits whatever budget it is given.
int _newlib_heap_size_user = 8 * 1024 * 1024;

// SceAppMgrUser / SceAppMgr; not declared by this SDK's headers.
int sceAppMgrReceiveEventNum(int* eventNum);
int sceAppMgrReceiveEvent(void* event);
int sceAppMgrAcquireBgmPortWithPriority(int priority);

#define EVENT_ACTIVATE     0x10000001
#define EVENT_DEACTIVATE   0x10000002
#define EVENT_RESUME       0x10000003
#define EVENT_REQUEST_QUIT 0x20000001

#define RATE  48000
#define GRAIN 1024
#define TWO_PI 6.283185307179586

static FILE* g_log;
static char g_title[16] = "?";
static SceInt64 g_t0;

// Written by one thread each, read by the UI; a torn read only shows a stale
// number for a frame.
static volatile unsigned g_grains;          // audio buffers played
static volatile SceInt64 g_longestGapUs;    // longest time the watcher did not run
static volatile SceInt64 g_lastGapUs;       // the most recent such gap
static volatile int g_events;               // app events received
static volatile int g_lastEvent;
static volatile int g_quit;

static double secs(void) { return (sceKernelGetSystemTimeWide() - g_t0) / 1e6; }

static void logLine(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_log, "%9.3f ", secs());
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

static const char* describe(void) {
    if (strcmp(g_title, "VPLXBGT01") == 0) return "a game, set up like VitaPlex (the control)";
    if (strcmp(g_title, "VPLXBGT02") == 0) return "a game with the BG_APP flag";
    if (strcmp(g_title, "VPLXBGT03") == 0) return "a non-game app, set up like ElevenMPV-A";
    return "unknown variant";
}

// One short beep a second, at a volume that will not startle anyone.
static int audioMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    int rcPort = sceAppMgrAcquireBgmPortWithPriority(0x81);
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, RATE,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    logLine("acquire BGM port 0x81: 0x%08X; open BGM audio port: 0x%08X", (unsigned)rcPort,
         (unsigned)port);
    if (port < 0) return 0;

    static short buf[GRAIN * 2];
    unsigned long long n = 0;
    for (;;) {
        for (int i = 0; i < GRAIN; i++, n++) {
            const double t = (double)n / RATE;
            const double into = t - floor(t);   // seconds into this second
            double amp = 0.0;
            if (into < 0.12) {
                // 5 ms fades each side, so the beep has no click.
                amp = 0.2;
                if (into < 0.005) amp *= into / 0.005;
                if (into > 0.115) amp *= (0.12 - into) / 0.005;
            }
            const short s = (short)(amp * 32767.0 * sin(TWO_PI * 660.0 * t));
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        sceAudioOutOutput(port, buf);
        g_grains++;
    }
    return 0;
}

static int powerCallback(int notifyId, int notifyCount, int powerInfo, void* common) {
    (void)notifyId;
    (void)notifyCount;
    (void)common;
    logLine("power callback 0x%08X", (unsigned)powerInfo);
    return 0;
}

// Ticks four times a second. Nothing in it waits for long, so a tick that is
// seconds late means the whole process was held.
static int watcherMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    SceUID cb = sceKernelCreateCallback("BgTestPower", 0, powerCallback, NULL);
    logLine("power callback registered: 0x%08X",
         (unsigned)(cb >= 0 ? scePowerRegisterCallback(cb) : cb));

    SceInt64 last = sceKernelGetSystemTimeWide();
    unsigned beat = 0;
    for (;;) {
        sceKernelDelayThreadCB(250 * 1000);
        const SceInt64 now = sceKernelGetSystemTimeWide();
        const SceInt64 gap = now - last;
        last = now;
        if (gap > 3 * 1000000LL) {
            g_lastGapUs = gap;
            if (gap > g_longestGapUs) g_longestGapUs = gap;
            logLine("did not run for %.1fs: the process was held", gap / 1e6);
        }

        int count = 0;
        if (sceAppMgrReceiveEventNum(&count) >= 0) {
            for (int i = 0; i < count; i++) {
                unsigned char ev[0x40];
                memset(ev, 0, sizeof(ev));
                if (sceAppMgrReceiveEvent(ev) < 0) break;
                int id;
                memcpy(&id, ev, sizeof(id));
                g_events++;
                g_lastEvent = id;
                logLine("app event 0x%08X%s", (unsigned)id,
                     id == EVENT_DEACTIVATE   ? " (deactivated)"
                     : id == EVENT_ACTIVATE   ? " (activated)"
                     : id == EVENT_RESUME     ? " (resumed)"
                     : id == EVENT_REQUEST_QUIT ? " (asked to quit)" : "");
                if (id == EVENT_REQUEST_QUIT) g_quit = 1;
            }
        }

        // A line every 5 s, so the log shows it running (or not) while away.
        if (++beat % 20 == 0)
            logLine("alive; audio has played %.0fs", (double)g_grains * GRAIN / RATE);
    }
    return 0;
}

static void drawText(vita2d_pgf* font, int x, int y, unsigned color, float scale,
                     const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    vita2d_pgf_draw_text(font, x, y, color, scale, line);
}

int main(void) {
    g_t0 = sceKernelGetSystemTimeWide();
    sceAppMgrAppParamGetString(SCE_KERNEL_PROCESS_ID_SELF, 12, g_title, sizeof(g_title));  // 12: TITLE_ID

    char path[96];
    snprintf(path, sizeof(path), "ux0:data/VitaPlex/bgtest-%s.log", g_title);
    g_log = fopen(path, "w");
    logLine("start: %s, %s", g_title, describe());

    SceUID t = sceKernelCreateThread("BgTestWatcher", watcherMain, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    t = sceKernelCreateThread("BgTestAudio", audioMain, 0x10000100 - 10, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);

    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    const int video = vita2d_init();
    vita2d_pgf* font = video > 0 ? vita2d_load_default_pgf() : NULL;
    logLine("display: %s", font ? "ok" : "none (running without a screen)");

    const unsigned white = RGBA8(0xF2, 0xF2, 0xF2, 0xFF);
    const unsigned muted = RGBA8(0x9A, 0x9A, 0x9A, 0xFF);
    const unsigned gold  = RGBA8(0xE5, 0xA0, 0x0D, 0xFF);

    SceCtrlData pad;
    while (!g_quit) {
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        if (!font) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        vita2d_start_drawing();
        vita2d_clear_screen();
        drawText(font, 40, 60, gold, 1.3f, "VitaPlex background test");
        drawText(font, 40, 100, white, 1.0f, "%s: %s", g_title, describe());
        drawText(font, 40, 160, white, 1.0f, "It beeps once a second. Press PS and listen:");
        drawText(font, 60, 195, muted, 1.0f, "keeps beeping: this kind of app runs in the background");
        drawText(font, 60, 225, muted, 1.0f, "goes quiet: the system froze it");
        drawText(font, 40, 265, white, 1.0f, "Then come back here. START quits.");
        drawText(font, 40, 330, white, 1.0f, "Running %.0fs, audio played %.0fs", secs(),
                 (double)g_grains * GRAIN / RATE);
        if (g_lastGapUs > 0)
            drawText(font, 40, 365, gold, 1.0f, "Last time away it was frozen for %.1fs "
                     "(longest %.1fs)", g_lastGapUs / 1e6, g_longestGapUs / 1e6);
        else
            drawText(font, 40, 365, white, 1.0f, "Never frozen so far");
        drawText(font, 40, 400, muted, 1.0f, "App events: %d, last 0x%08X", g_events,
                 (unsigned)g_lastEvent);
        drawText(font, 40, 500, muted, 0.8f, "Log: %s", path);
        vita2d_end_drawing();
        vita2d_swap_buffers();
        vita2d_wait_rendering_done();
    }

    logLine("quit");
    if (g_log) fclose(g_log);
    sceKernelExitProcess(0);
    return 0;
}
