/**
 * VitaPlex BG's background player: the process.
 *
 * eboot2.bin in VitaPlexBG.vpk, with its own sce_sys/param2.sfo, started by
 * the app with sceBgAppUtilStartBgApp as it leaves (bgplayer.h). It runs as a
 * process of its own with a system-mode budget of 16 MB and no screen, as the
 * background test's player did through games (src/bgtest_psv, BG Tests 14 to
 * 21): this is that program, playing the app's queue instead of two songs
 * from the library.
 *
 * This file is the process around the player (player.c): the log, the network,
 * and a watcher that reads the system's events and the app's requests four
 * times a second. apps.c logs which other apps run beside it.
 */

#include "bgplayer.h"
#include "program.h"

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// curl, mbedtls, the CA list and two songs' worth of buffers, in a 16 MB
// budget: what the background test's player had, with 3 MB to spare.
int _newlib_heap_size_user = 8 * 1024 * 1024;

// SceAppMgrUser; not declared by this SDK's headers.
int sceAppMgrReceiveEventNum(int* eventNum);
int sceAppMgrReceiveEvent(void* event);

// _sceKernelExitProcessForUser, the system call that sceKernelExitProcess
// ends with (raw_exit.S).
int vitaplexRawExit(int status);

#define EVENT_REQUEST_QUIT 0x20000001
#define MB (1024 * 1024)
// Past this the log is moved aside as it starts, so it does not grow forever.
#define LOG_KEEP (512 * 1024)

static FILE* g_log;
static SceUID g_logMutex = -1;   // lines from several threads came out mixed
static int g_syncLog;            // each line forced out to the card
volatile int g_netReady;
double g_startedAt;

double systemSecs(void) { return sceKernelGetSystemTimeWide() / 1e6; }

const char* result(char* buf, size_t size, int value) {
    if (value < 0)
        snprintf(buf, size, "error 0x%08X", (unsigned)value);
    else
        snprintf(buf, size, "%d", value);
    return buf;
}

// Whole lines, one thread at a time, with the time of day in front, as
// vitaplex.log has it, so the two can be read side by side. Each is synced
// to the card: the console crashed twice while the background test's player
// played, and a process's last lines may not reach the card otherwise (logs
// 5d35ef15, cf918ac6).
void logLine(const char* fmt, ...) {
    if (!g_log) return;
    SceDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    char line[512];
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", t.hour, t.minute, t.second,
                     t.microsecond / 1000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    n = (int)strlen(line);
    line[n] = '\n';
    line[n + 1] = '\0';
    if (g_logMutex >= 0) sceKernelLockMutex(g_logMutex, 1, NULL);
    fputs(line, g_log);
    fflush(g_log);
    if (g_syncLog) sceIoSync("ux0:", 0);
    if (g_logMutex >= 0) sceKernelUnlockMutex(g_logMutex, 1);
}

int appRunning(void) {
    SceUID pid = -1;
    return sceAppMgrGetIdByName(&pid, BGPLAYER_APP_TITLEID) >= 0;
}

// Started as the process begins to quit, it wakes 3 s later only if the
// process is still there: sceKernelExitProcess did not return in the
// background test's programs on some of their ways out (BG Test 18), and the
// system call underneath it did.
static int rescueMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    sceKernelDelayThread(3 * 1000 * 1000);
    logLine("still running 3 s after it began to quit; ending through "
            "_sceKernelExitProcessForUser, the system call underneath");
    vitaplexRawExit(0);
    return 0;
}

void quitNow(void) {
    static int armed;
    if (!armed) {
        armed = 1;
        const SceUID t =
            sceKernelCreateThread("BgPlayerRescue", rescueMain, 0x10000100, 0x4000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    logLine("quit");
    sceKernelExitProcess(0);
    for (;;) sceKernelDelayThread(1000 * 1000);
}

// ── The network ──────────────────────────────────────────────────────────

static char g_netMemory[1024 * 1024];   // whole songs, over https

static int netMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    char r[24];
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p;
    p.memory = g_netMemory;
    p.size = sizeof(g_netMemory);
    p.flags = 0;
    int rc = sceNetInit(&p);
    if (rc >= 0 || rc == (int)0x80410201) rc = sceNetCtlInit();   // already up: fine
    if (rc < 0 && rc != (int)0x80412102) {
        logLine("network: could not start (%s)", result(r, sizeof(r), rc));
        return 0;
    }
    g_netReady = 1;
    return 0;
}

// ── The watcher ──────────────────────────────────────────────────────────

static int powerCallback(int notifyId, int notifyCount, int powerInfo, void* common) {
    (void)notifyId;
    (void)notifyCount;
    (void)common;
    logLine("power callback 0x%08X", (unsigned)powerInfo);
    playerPowerEvent(powerInfo);
    return 0;
}

// The request at path: 1 with why, left in place until it has been acted on,
// since the app takes its removal to mean that; -1 when it was made before
// this process started, so for another one, and is removed; 0 when there is
// none. One caught half written, with no end to its line yet, is left for the
// next look.
static int readRequest(const char* path, char* why, size_t size) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[160];
    const int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    char* end = got ? strchr(line, '\n') : NULL;
    double asked = 0;
    int at = 0;
    if (!end || sscanf(line, "%lf %n", &asked, &at) < 1 || !at) return 0;
    if (asked < g_startedAt) {
        remove(path);
        return -1;
    }
    *end = '\0';
    snprintf(why, size, "%s", line + at);
    return 1;
}

static int watcherMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    char r[24];
    const SceUID cb = sceKernelCreateCallback("BgPlayerPower", 0, powerCallback, NULL);
    logLine("power callback registered: %s",
            result(r, sizeof(r), cb >= 0 ? scePowerRegisterCallback(cb) : cb));

    int lastAppRc = 1;   // no call returns that, so the first look is logged
    for (;;) {
        // The CB variant is what runs the power callback on this thread.
        sceKernelDelayThreadCB(250 * 1000);

        // Whether the app is there, logged as that changes: the player waits
        // for it to go before anything it does, and never ends while it is
        // open (bgplayer.h).
        SceUID pid = -1;
        const int rc = sceAppMgrGetIdByName(&pid, BGPLAYER_APP_TITLEID);
        if (rc != lastAppRc && (rc < 0) != (lastAppRc < 0)) {
            if (rc >= 0)
                logLine("its app %s: running, process 0x%08X", BGPLAYER_APP_TITLEID, (unsigned)pid);
            else   // 0x80802012: no such application or process
                logLine("its app %s: %s%s", BGPLAYER_APP_TITLEID, result(r, sizeof(r), rc),
                        rc == (int)0x80802012 ? ", not running" : "");
        }
        lastAppRc = rc;

        // The status says where the music stopped before a pause counts as
        // taken (player.c).
        char why[128];
        const int paused = readRequest(BGPLAYER_ASK_PAUSE, why, sizeof(why));
        if (paused < 0) logLine("a request to pause from before it started: dropped");
        if (paused > 0) {
            logLine("its app asks it to pause: %s", why);
            playerPause(why);
            remove(BGPLAYER_ASK_PAUSE);
        }
        const int play = readRequest(BGPLAYER_ASK_PLAY, why, sizeof(why));
        if (play < 0) logLine("a request to play from before it started: dropped");
        if (play > 0) {
            logLine("its app asks it for the music again: %s", why);
            playerPlay(why);
            remove(BGPLAYER_ASK_PLAY);
        }

        int count = 0;
        if (sceAppMgrReceiveEventNum(&count) < 0) continue;
        for (int i = 0; i < count; i++) {
            unsigned char ev[0x40];
            memset(ev, 0, sizeof(ev));
            if (sceAppMgrReceiveEvent(ev) < 0) break;
            int id, appId;
            memcpy(&id, ev, sizeof(id));
            memcpy(&appId, ev + 4, sizeof(appId));
            // The rest in full, once, for what asks a program to quit: the log
            // of BG Test 10 could not say which app had (DESIGN_NOTES.md).
            char rest[2 * 24 + 1];
            for (int b = 0; b < 24; b++) snprintf(rest + 2 * b, 3, "%02X", ev[8 + b]);
            logLine("app event 0x%08X, app 0x%08X, then %s%s", (unsigned)id, (unsigned)appId, rest,
                    id == EVENT_REQUEST_QUIT ? " (asked to quit)" : "");
            if (id == EVENT_REQUEST_QUIT) {
                // As the app's LiveArea is closed, say: the music goes with
                // it, or the shell would play on with nothing to stop it.
                playerStop("asked to quit by the system");
                quitNow();
            }
        }
    }
    return 0;
}

// ── Start ────────────────────────────────────────────────────────────────

static void openLog(void) {
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(BGPLAYER_LOG, &st) >= 0 && st.st_size > LOG_KEEP) {
        remove(BGPLAYER_DIR "bgplayer-old.log");
        rename(BGPLAYER_LOG, BGPLAYER_DIR "bgplayer-old.log");
    }
    g_log = fopen(BGPLAYER_LOG, "a");
}

int main(void) {
    g_startedAt = systemSecs();
    g_logMutex = sceKernelCreateMutex("BgPlayerLog", 0, 0, NULL);
    sceIoMkdir("ux0:data/VitaPlex", 0777);
    openLog();
    char r[24];
    const int rcSync = sceIoSync("ux0:", 0);
    g_syncLog = rcSync >= 0;

    char title[16] = "?";
    const int rcTitle =
        sceAppMgrAppParamGetString(SCE_KERNEL_PROCESS_ID_SELF, 12, title, sizeof(title));  // 12: TITLE_ID
    logLine("start: VitaPlex BG's background player, %s (each line synced to the card: %s)",
            rcTitle >= 0 ? title : "with no title id from the system",
            result(r, sizeof(r), rcSync));

    SceAppMgrBudgetInfo b;
    memset(&b, 0, sizeof(b));
    b.size = sizeof(b);
    const int rcBudget = sceAppMgrGetBudgetInfo(&b);
    if (rcBudget >= 0)
        logLine("memory budget: main %u MB, %u MB free", b.total_user_rw_mem / MB,
                b.free_user_rw / MB);
    else
        logLine("memory budget: %s", result(r, sizeof(r), rcBudget));

    SceUID t = sceKernelCreateThread("BgPlayerWatcher", watcherMain, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    t = sceKernelCreateThread("BgPlayerNet", netMain, 0x10000100 + 10, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    playerStart();
    appsWatchStart();

    // Nothing more for this thread: the player ends the process once it is
    // done (player.c), the watcher when the system asks.
    for (;;) sceKernelDelayThread(60 * 1000 * 1000);
    return 0;
}
