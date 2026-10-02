/**
 * VitaPlex background test: a tiny app that beeps through the BGM audio port
 * and records whether it keeps running once the user leaves it.
 *
 * VitaPlex is frozen by the system the moment the PS button takes it out of
 * the foreground. One binary, packaged several times with nothing different
 * but the param.sfo (CMakeLists.txt, "Background test"), finds out which kind
 * of app is not. On a console, the first three gave:
 *
 *   VPLXBGT01  a game, set up like VitaPlex: frozen (the control).
 *   VPLXBGT02  a game with ATTRIBUTE_BG_APP (0x04000000): frozen as well.
 *   VPLXBGT03  a non-game application (gdc) with ElevenMPV-A's attributes:
 *              kept running and playing after PS, and it alone was sent
 *              the activate and deactivate events.
 *
 * So a non-game app can play in the background. What decides how VitaPlex
 * can use that is what such an app is given, so each variant now also logs:
 *
 *   - its memory: the system's budget for it and the largest blocks it can
 *     really allocate. ElevenMPV-A plans for as little as 17 MB, and VitaPlex
 *     needs far more than that;
 *   - whether its network works while it is in the background: every 5 s it
 *     asks the Plex server in VitaPlex's settings for /identity, which needs
 *     no sign-in, over plain http;
 *
 * and one more variant is packaged:
 *
 *   VPLXBGT04  the non-game app of VPLXBGT03 with VitaPlex's memory setting
 *              (ATTRIBUTE2=12), to see whether that setting holds for one.
 *
 * Both of those could allocate 233 and 342 MB (logs 9462d9ec, babbea68), the
 * game budget an eboot without a boot param gets, and both were asked to
 * close when another app was opened. ElevenMPV-A's eboot carries what theirs
 * does not, a boot param (SELF control info 6) of attribute 2 and a 16 MB
 * budget, which vita-make-fself calls a system-mode app. Its PAF loader then
 * grows that with sceAppMgrGrowMemory3, to 57 MB or else 32 MB, and nothing
 * in SceAppMgr's exports gives memory back. A second binary, built with
 * BGTEST_SYSTEM_MODE and that boot param, is packaged twice:
 *
 *   VPLXBGT05  ElevenMPV-A's 16 MB as it is.
 *   VPLXBGT06  the same, grown at start as ElevenMPV-A grows.
 *
 * They have no screen, since vita2d's GPU buffers alone come to about 10 MB,
 * and START does not quit them. What they are for is whether they play on
 * while a game runs.
 *
 * VPLXBGT05 played through LittleBigPlanet and on beside VitaShell, but its
 * beeps stopped when VitaPlex was opened, which makes no audio call at
 * start. VitaPlex is packaged with the extended memory vita-mksfoex calls
 * ATTRIBUTE2_MEM109, so two silent games tell whether that is what does it:
 *
 *   VPLXBGT07  a game with ATTRIBUTE2=12, as VitaPlex is.
 *   VPLXBGT08  a game with ATTRIBUTE2=0.
 *
 * Nothing here is VitaPlex code; what it finds goes into VitaPlex (or a
 * helper app) and this goes away.
 */

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#ifndef BGTEST_SYSTEM_MODE
#include <vita2d.h>
#endif

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Small, so the non-game variants fit whatever budget they are given. The
// system-mode build has 16 MB in all and draws nothing, so it takes less.
#ifdef BGTEST_SYSTEM_MODE
int _newlib_heap_size_user = 4 * 1024 * 1024;
#else
int _newlib_heap_size_user = 8 * 1024 * 1024;
#endif

// SceAppMgrUser / SceAppMgr; not declared by this SDK's headers.
int sceAppMgrReceiveEventNum(int* eventNum);
int sceAppMgrReceiveEvent(void* event);
int sceAppMgrAcquireBgmPortWithPriority(int priority);
// SceAudio; this SDK's headers declare sceAudioOutGetAdopt but not this. The
// calling process's private 0..256 gain for one kind of port.
int sceAudioOutGetPortVolume_forUser(SceAudioOutPortType type);
#ifdef BGTEST_SYSTEM_MODE
// Declared as ElevenMPV-A's libScePafPreload declares it: the bytes to add to
// a system-mode app's budget. The second argument is 1 there, unexplained.
int sceAppMgrGrowMemory3(unsigned int size, int unk);
#endif

#define EVENT_ACTIVATE     0x10000001
#define EVENT_DEACTIVATE   0x10000002
#define EVENT_RESUME       0x10000003
#define EVENT_REQUEST_QUIT 0x20000001

#define RATE  48000
#define GRAIN 1024
#define TWO_PI 6.283185307179586
#define MB (1024 * 1024)

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
static volatile int g_away;                 // deactivated and not yet activated
#ifndef BGTEST_SYSTEM_MODE
static char g_memLine[128] = "Memory: measuring";
#endif
static char g_netLine[128] = "Network: starting";

static double secs(void) { return (sceKernelGetSystemTimeWide() - g_t0) / 1e6; }

// A call's result as a number, or as the error code it is when negative.
static const char* result(char* buf, size_t size, int value) {
    if (value < 0)
        snprintf(buf, size, "error 0x%08X", (unsigned)value);
    else
        snprintf(buf, size, "%d", value);
    return buf;
}

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
    if (strcmp(g_title, "VPLXBGT04") == 0) return "a non-game app with VitaPlex's memory setting";
    if (strcmp(g_title, "VPLXBGT05") == 0) return "a system-mode app with ElevenMPV-A's 16 MB";
    if (strcmp(g_title, "VPLXBGT06") == 0) return "a system-mode app grown as ElevenMPV-A grows";
    if (strcmp(g_title, "VPLXBGT07") == 0) return "a silent game with VitaPlex's extended memory";
    if (strcmp(g_title, "VPLXBGT08") == 0) return "a silent game without extended memory";
    return "unknown variant";
}

// The variants that make no sound, for opening while another one beeps.
static int silent(void) {
    return strcmp(g_title, "VPLXBGT07") == 0 || strcmp(g_title, "VPLXBGT08") == 0;
}

// ── Memory ───────────────────────────────────────────────────────────────

// The largest single block of `type` the system gives now, in whole MB, found
// by halving between 0 and `maxMb`. Each block is freed at once.
static int largestBlockMb(SceKernelMemBlockType type, int maxMb) {
    int lo = 0, hi = maxMb;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        const SceUID b = sceKernelAllocMemBlock("BgTestProbe", type, (SceSize)mid * MB, NULL);
        if (b >= 0) {
            sceKernelFreeMemBlock(b);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

static int freeUserMb(void) {
    SceKernelFreeMemorySizeInfo f;
    memset(&f, 0, sizeof(f));
    f.size = sizeof(f);
    return sceKernelGetFreeMemorySize(&f) < 0 ? -1 : f.size_user / MB;
}

// What the system says this app may use. Refused to the game-budget variants
// (0x8080201C on the console); vita2d reads it succeeding as system mode.
static int budget(SceAppMgrBudgetInfo* b) {
    memset(b, 0, sizeof(*b));
    b->size = sizeof(*b);
    return sceAppMgrGetBudgetInfo(b);
}

static void logBudget(const char* when, SceAppMgrBudgetInfo* b) {
    const int rc = budget(b);
    logLine("memory budget%s (0x%08X): mode %d; main %u MB, %u MB free; extra %s, %u MB, %u MB free; "
            "phycont %u MB, %u MB free; cdram %u MB, %u MB free",
            when, (unsigned)rc, b->app_mode, b->total_user_rw_mem / MB, b->free_user_rw / MB,
            b->extra_mem_allowed ? "allowed" : "not allowed", b->total_extra_mem / MB,
            b->free_extra_mem / MB, b->total_phycont_mem / MB, b->free_phycont_mem / MB,
            b->total_cdram_mem / MB, b->free_cdram_mem / MB);
}

#ifdef BGTEST_SYSTEM_MODE
// VPLXBGT06 asks for more, as ElevenMPV-A's libScePafPreload does before
// anything else runs: 41 MB more (57 MB in all), or failing that 16 MB more.
static void growMemory(void) {
    SceAppMgrBudgetInfo b;
    logBudget(" before growing", &b);
    int rc = sceAppMgrGrowMemory3(41 * MB, 1);
    logLine("grow memory by 41 MB: 0x%08X", (unsigned)rc);
    if (rc < 0) {
        rc = sceAppMgrGrowMemory3(16 * MB, 1);
        logLine("grow memory by 16 MB: 0x%08X", (unsigned)rc);
    }
}
#endif

// What the system says this app may use, and what it can really have. Taken
// after the display is up, with the heap and the audio buffers already
// allocated, so "free" is what a running app has left.
static void reportMemory(void) {
    SceAppMgrBudgetInfo b;
    logBudget("", &b);

    SceKernelFreeMemorySizeInfo f;
    memset(&f, 0, sizeof(f));
    f.size = sizeof(f);
    const int rcFree = sceKernelGetFreeMemorySize(&f);
    logLine("free memory (0x%08X): main %d MB, cdram %d MB, phycont %d MB", (unsigned)rcFree,
            f.size_user / MB, f.size_cdram / MB, f.size_phycont / MB);

#ifdef BGTEST_SYSTEM_MODE
    // Whatever this app's budget does not cover may come from memory the
    // shell is using, so the probe asks for no more than a system-mode app
    // can hold (vita-make-fself allows up to 74 MB), and video and contiguous
    // memory, which the budget line above reports, are not probed.
    const int mainMb = largestBlockMb(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 80);
    logLine("largest block it could allocate: main %d MB (probed up to 80)", mainMb);
#else
    const int mainMb = largestBlockMb(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 512);
    const int cdramMb = largestBlockMb(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 512);
    const int phycontMb = largestBlockMb(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 512);
    logLine("largest block it could allocate: main %d MB, cdram %d MB, phycont %d MB", mainMb,
            cdramMb, phycontMb);

    snprintf(g_memLine, sizeof(g_memLine),
             "Memory: budget %u MB, could allocate %d MB more (video %d MB)",
             b.total_user_rw_mem / MB, mainMb, cdramMb);
#endif
}

// ── Network ──────────────────────────────────────────────────────────────

static char g_netMemory[256 * 1024];

// The Plex server in VitaPlex's settings, as an IPv4 address and a port.
// Only "serverUrl" is read from the file. 0 on success.
static int plexServer(char* ip, size_t ipSize, int* port, char* host, size_t hostSize) {
    FILE* f = fopen("ux0:data/VitaPlex/settings.json", "r");
    if (!f) return -1;
    static char buf[32 * 1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char* k = strstr(buf, "\"serverUrl\"");
    if (!k) return -2;
    const char* v = strchr(k + 11, ':');
    if (v) v = strchr(v, '"');
    if (!v) return -2;
    v++;
    const char* end = strchr(v, '"');
    if (!end || end == v) return -2;

    const char* h = strstr(v, "://");
    h = (h && h < end) ? h + 3 : v;
    size_t i = 0;
    while (h + i < end && h[i] != ':' && h[i] != '/' && i + 1 < hostSize) {
        host[i] = h[i];
        i++;
    }
    host[i] = '\0';
    *port = (h + i < end && h[i] == ':') ? atoi(h + i + 1) : 32400;

    // A plex.direct name carries its own address (192-168-1-28.<id>.plex.direct);
    // otherwise the name is looked up.
    int a, b, c, d;
    if (sscanf(host, "%d-%d-%d-%d.", &a, &b, &c, &d) == 4 ||
        sscanf(host, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
        snprintf(ip, ipSize, "%d.%d.%d.%d", a, b, c, d);
        return 0;
    }
    struct hostent* he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return -3;
    inet_ntop(AF_INET, he->h_addr_list[0], ip, ipSize);
    return 0;
}

// One plain http GET of /identity. Returns the status (200 when all is well),
// or a negative step that failed; *bytes gets the size of the reply.
static int getIdentity(const char* ip, int port, const char* host, int* bytes) {
    *bytes = 0;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -2;
    }
    char req[320];
    const int len = snprintf(req, sizeof(req),
                             "GET /identity HTTP/1.0\r\nHost: %s:%d\r\nAccept: */*\r\n\r\n", host,
                             port);
    if (send(fd, req, len, 0) != len) {
        close(fd);
        return -3;
    }
    char head[64];
    int headLen = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 5000) <= 0) break;
        char buf[1024];
        const int r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        if (headLen < (int)sizeof(head) - 1) {
            const int take = r < (int)sizeof(head) - 1 - headLen ? r : (int)sizeof(head) - 1 - headLen;
            memcpy(head + headLen, buf, take);
            headLen += take;
        }
        *bytes += r;
    }
    close(fd);
    head[headLen] = '\0';
    int status = 0;
    if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1) return -4;
    return status;
}

static int netMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p;
    p.memory = g_netMemory;
    p.size = sizeof(g_netMemory);
    p.flags = 0;
    int rc = sceNetInit(&p);
    if (rc >= 0 || rc == (int)0x80410201) rc = sceNetCtlInit();
    if (rc < 0 && rc != (int)0x80412102) {
        logLine("network: could not start (0x%08X)", (unsigned)rc);
        snprintf(g_netLine, sizeof(g_netLine), "Network: could not start (0x%08X)", (unsigned)rc);
        return 0;
    }

    char ip[48], host[160];
    int port = 32400;
    rc = plexServer(ip, sizeof(ip), &port, host, sizeof(host));
    if (rc < 0) {
        const char* why = rc == -1 ? "no VitaPlex settings file"
                        : rc == -2 ? "no server in VitaPlex's settings"
                                   : "the server's name could not be looked up";
        logLine("network: not tested, %s", why);
        snprintf(g_netLine, sizeof(g_netLine), "Network: not tested, %s", why);
        return 0;
    }
    logLine("network: asking %s:%d (%s) for /identity every 5 s, over plain http", ip, port, host);

    for (;;) {
        const SceInt64 t = sceKernelGetSystemTimeWide();
        int bytes = 0;
        const int status = getIdentity(ip, port, host, &bytes);
        const int ms = (int)((sceKernelGetSystemTimeWide() - t) / 1000);
        const char* failed = status == -1 ? "no socket"
                           : status == -2 ? "could not connect"
                           : status == -3 ? "could not send"
                           : status == -4 ? "no reply" : NULL;
        if (failed) {
            logLine("network%s: %s (errno %d) after %d ms", g_away ? ", away" : "", failed, errno, ms);
            snprintf(g_netLine, sizeof(g_netLine), "Network: %s (at %.0fs)", failed, secs());
        } else {
            logLine("network%s: HTTP %d, %d bytes, %d ms", g_away ? ", away" : "", status, bytes, ms);
            snprintf(g_netLine, sizeof(g_netLine), "Network: HTTP %d in %d ms (at %.0fs)", status,
                     ms, secs());
        }
        sceKernelDelayThread(5 * 1000 * 1000);
    }
    return 0;
}

// ── Audio, power and app events ──────────────────────────────────────────

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
    int lastAdopt = 0x7FFFFFFF, lastGain = 0x7FFFFFFF;
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

        // Whether this app's BGM output is adopted, and its private gain: two
        // states audioout.h documents for the calling process. Logged when
        // either changes, to see whether the beeps that go quiet beside
        // another app are muted through one of them.
        const int adopt = sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_BGM);
        const int gain = sceAudioOutGetPortVolume_forUser(SCE_AUDIO_OUT_PORT_TYPE_BGM);
        if (adopt != lastAdopt || gain != lastGain) {
            char a[24], g[24];
            logLine("BGM output%s: adopted %s, gain %s", g_away ? ", away" : "",
                    result(a, sizeof(a), adopt), result(g, sizeof(g), gain));
            lastAdopt = adopt;
            lastGain = gain;
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
                if (id == EVENT_DEACTIVATE) g_away = 1;
                if (id == EVENT_ACTIVATE) g_away = 0;
                if (id == EVENT_REQUEST_QUIT) {
                    // Ended here rather than by the main loop, which could be
                    // stuck: the system starts the next app only once this
                    // process ends, and VitaPlex not ending hung the console
                    // (log 3ea218f5).
                    logLine("quit");
                    sceKernelExitProcess(0);
                }
            }
        }

        // A line every 5 s, so the log shows it running (or not) while away.
        // The budget, where the system gives it, shows any memory it takes
        // back, from a grown VPLXBGT06 when a game starts, say.
        if (++beat % 20 == 0) {
            SceAppMgrBudgetInfo b;
            char mem[64];
            if (budget(&b) >= 0)
                snprintf(mem, sizeof(mem), "budget %u MB, %u MB free", b.total_user_rw_mem / MB,
                         b.free_user_rw / MB);
            else
                snprintf(mem, sizeof(mem), "%d MB free", freeUserMb());
            logLine("alive%s; audio has played %.0fs; %s", g_away ? ", away" : "",
                    (double)g_grains * GRAIN / RATE, mem);
        }
    }
    return 0;
}

#ifndef BGTEST_SYSTEM_MODE
static void drawText(vita2d_pgf* font, int x, int y, unsigned color, float scale,
                     const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    vita2d_pgf_draw_text(font, x, y, color, scale, line);
}

static void drawScreen(vita2d_pgf* font, const char* path) {
    const unsigned white = RGBA8(0xF2, 0xF2, 0xF2, 0xFF);
    const unsigned muted = RGBA8(0x9A, 0x9A, 0x9A, 0xFF);
    const unsigned gold  = RGBA8(0xE5, 0xA0, 0x0D, 0xFF);

    vita2d_start_drawing();
    vita2d_clear_screen();
    drawText(font, 40, 50, gold, 1.3f, "VitaPlex background test");
    drawText(font, 40, 85, white, 1.0f, "%s: %s", g_title, describe());
    if (silent()) {
        drawText(font, 40, 135, white, 1.0f, "This one makes no sound. Open it while BG Test 5 beeps:");
        drawText(font, 60, 165, muted, 1.0f, "do the beeps go on while this is in front?");
        drawText(font, 60, 195, muted, 1.0f, "and once you press PS to leave it?");
        drawText(font, 40, 260, white, 1.0f, "START quits.");
        drawText(font, 40, 310, white, 1.0f, "Running %.0fs", secs());
    } else {
        drawText(font, 40, 135, white, 1.0f, "It beeps once a second. Press PS and listen:");
        drawText(font, 60, 165, muted, 1.0f, "keeps beeping: this kind of app runs in the background");
        drawText(font, 60, 195, muted, 1.0f, "goes quiet: the system froze it");
        drawText(font, 40, 230, white, 1.0f, "Then start VitaPlex: do the beeps go on beside it?");
        drawText(font, 40, 260, white, 1.0f, "Come back here after. START quits.");
        drawText(font, 40, 310, white, 1.0f, "Running %.0fs, audio played %.0fs", secs(),
                 (double)g_grains * GRAIN / RATE);
    }
    if (g_lastGapUs > 0)
        drawText(font, 40, 340, gold, 1.0f, "Last time away it was frozen for %.1fs "
                 "(longest %.1fs)", g_lastGapUs / 1e6, g_longestGapUs / 1e6);
    else
        drawText(font, 40, 340, white, 1.0f, "Never frozen so far");
    drawText(font, 40, 370, white, 1.0f, "%s", g_memLine);
    drawText(font, 40, 400, white, 1.0f, "%s", g_netLine);
    drawText(font, 40, 430, muted, 1.0f, "App events: %d, last 0x%08X", g_events,
             (unsigned)g_lastEvent);
    drawText(font, 40, 510, muted, 0.8f, "Log: %s", path);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    vita2d_wait_rendering_done();
}
#endif

int main(void) {
    g_t0 = sceKernelGetSystemTimeWide();
    sceAppMgrAppParamGetString(SCE_KERNEL_PROCESS_ID_SELF, 12, g_title, sizeof(g_title));  // 12: TITLE_ID

    char path[96];
    snprintf(path, sizeof(path), "ux0:data/VitaPlex/bgtest-%s.log", g_title);
    g_log = fopen(path, "w");
    logLine("start: %s, %s", g_title, describe());
#ifdef BGTEST_SYSTEM_MODE
    if (strcmp(g_title, "VPLXBGT06") == 0) growMemory();
#endif

    SceUID t = sceKernelCreateThread("BgTestWatcher", watcherMain, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    if (silent()) {
        logLine("audio: none, this variant is silent");
    } else {
        t = sceKernelCreateThread("BgTestAudio", audioMain, 0x10000100 - 10, 0x10000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }

#ifdef BGTEST_SYSTEM_MODE
    logLine("display: none (system mode)");
#else
    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    const int video = vita2d_init();
    vita2d_pgf* font = video > 0 ? vita2d_load_default_pgf() : NULL;
    logLine("display: %s", font ? "ok" : "none (running without a screen)");
#endif

    reportMemory();
    t = sceKernelCreateThread("BgTestNet", netMain, 0x10000100 + 10, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);

#ifdef BGTEST_SYSTEM_MODE
    // No START to quit, unlike the others: there is no screen to say so, and
    // this one is meant to run on while a game is in front, where START is
    // pressed all the time. Closed from the LiveArea, it is asked to quit and
    // the watcher ends it.
    for (;;) sceKernelDelayThread(60 * 1000 * 1000);
#else
    SceCtrlData pad;
    for (;;) {
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;
        if (!font) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        drawScreen(font, path);
    }
#endif

    logLine("quit");
    if (g_log) fclose(g_log);
    sceKernelExitProcess(0);
    return 0;
}
