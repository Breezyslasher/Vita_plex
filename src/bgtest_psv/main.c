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
 * It is: VPLXBGT05's beeps went quiet beside VPLXBGT07 and VitaSurf (another
 * homebrew with extended memory) and went on beside VPLXBGT08, and
 * VPLXBGT07 had the BGM output adopted for it without opening a BGM port,
 * where VPLXBGT08 did not (logs f794d228, b25e8414). So:
 *
 *   VPLXBGT09  VPLXBGT07 giving the BGM output up when it is adopted
 *              (sceAudioOutSetAdopt_forUser), to see whether that is enough.
 *
 * The shell's music service plays on beside extended memory, but only from
 * files. A third system-mode build, with BGTEST_PLAYER, adds what a player
 * using it would need:
 *
 *   VPLXBGT10  once sent away, downloads two songs from the Plex server and
 *              hands them to the shell one after the other (player_test.c).
 *
 * The shell played them through a game, so a song VitaPlex is playing could go
 * on the same way:
 *
 *   VPLXBGT11  the same, started by VitaPlex BG from its settings with the song
 *              it was playing, carried on from where VitaPlex paused it, then
 *              the next one in its queue.
 *
 * It did, but the system closed VitaPlex BG to start it (log 0bbfcb94). One
 * app could be both, if the system goes by the program an app is running:
 * sceAppMgrLoadExec switches an app to another SELF in its own package. Two
 * more packages carry this build with a screen as eboot.bin, the player of
 * 10 as player.self, with its system-mode boot param, and a copy of the
 * first as screen.self. X switches to the player; O to the copy and triangle
 * to eboot.bin again, the controls for the switch itself. Opened again after
 * PS, the player switches back. All of them keep one log, carried across by
 * a note.
 *
 *   VPLXBGT12  as a non-game app with VitaPlex's extended memory, as VitaPlex
 *              BG is.
 *   VPLXBGT13  as a game with it, as VitaPlex is.
 *
 * Only the game switched, and its player then ran as the game, with the
 * game's memory, held once PS was pressed (logs 6b8415c2, 5571cc78). The
 * system has a way of its own to give an app a second program: a background
 * application, eboot2.bin with a param2.sfo, which the app starts with
 * sceBgAppUtilStartBgApp and which runs as a process of its own (bgapp.c).
 * BGFTP runs its FTP server like that, through games. The player of 10 is
 * that program in three more packages, built with BGTEST_BGAPP, logging to a
 * file of its own and playing two songs as soon as it starts:
 *
 *   VPLXBGT14  a game with VitaPlex's extended memory, as VitaPlex is, whose
 *              X starts it.
 *   VPLXBGT15  the same game without extended memory.
 *   VPLXBGT16  a small non-game app that starts it and quits, as BGFTP's main
 *              app does (bgstart.c), the control.
 *
 * 16 started it and it played (log 6b8dce16); 14 and 15 gave 0x80101104.
 * Three more with X, each one step from those:
 *
 *   VPLXBGT17  a non-game app on the game budget with extended memory: VitaPlex
 *              BG's param.sfo.
 *   VPLXBGT18  the same without extended memory: VPLXBGT03's, ElevenMPV-A's.
 *   VPLXBGT19  a game with extended memory and the ATTRIBUTE of BGFTP's main
 *              app, to tell its category from its attributes.
 *
 * 19 would not install either, so a game cannot carry one. 18 started it, and
 * it played both songs and on through the power button, but 18 itself could
 * not quit once it ran: asked to as 2048 was started, it called
 * sceKernelExitProcess, which never returned, and 2048 hung (logs d942eac0,
 * e98e760e). BGFTP's main app and 16 quit before their program is up. Two more
 * do that from the app with a screen, and take the music back when opened
 * again, as VitaPlex would. Every exit of 14 to 21 and of the program now has
 * a rescue behind it (quitNow).
 *
 *   VPLXBGT20  the app of 18, whose X starts the player and quits at once, and
 *              which does the same when the system asks it to quit, as when a
 *              game is started over it.
 *   VPLXBGT21  the same with extended memory: VitaPlex BG's param.sfo, as 17.
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

#include "bgtest.h"

// Small, so the non-game variants fit whatever budget they are given. The
// system-mode build has 16 MB in all and draws nothing, so it takes less,
// though the player's curl, TLS and CA list need more than the beeps do.
#if defined(BGTEST_PLAYER)
int _newlib_heap_size_user = 8 * 1024 * 1024;
#elif defined(BGTEST_SYSTEM_MODE)
int _newlib_heap_size_user = 4 * 1024 * 1024;
#else
int _newlib_heap_size_user = 8 * 1024 * 1024;
#endif

// SceAppMgrUser / SceAppMgr; not declared by this SDK's headers.
int sceAppMgrReceiveEventNum(int* eventNum);
int sceAppMgrReceiveEvent(void* event);
int sceAppMgrAcquireBgmPortWithPriority(int priority);
// SceAudio; this SDK's headers declare sceAudioOutGetAdopt but not these. The
// calling process's private 0..256 gain for one kind of port, and its
// adoption of one: SCE_FALSE gives it up, over ramp_length ms (1..4096).
int sceAudioOutGetPortVolume_forUser(SceAudioOutPortType type);
int sceAudioOutSetAdopt_forUser(SceAudioOutPortType type, SceBool adopt, int ramp_length,
                                SceBool wait_for_completion);
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
static SceUID g_logMutex = -1;   // lines from several threads came out mixed
static char g_title[16] = "?";
static SceInt64 g_t0;

// Written by one thread each, read by the UI; a torn read only shows a stale
// number for a frame.
static volatile unsigned g_grains;          // audio buffers played
static volatile SceInt64 g_longestGapUs;    // longest time the watcher did not run
static volatile SceInt64 g_lastGapUs;       // the most recent such gap
static volatile int g_events;               // app events received
static volatile int g_lastEvent;
volatile int g_away;                        // deactivated and not yet activated
volatile int g_netReady;                    // SceNet is up
#ifndef BGTEST_SYSTEM_MODE
static char g_memLine[128] = "Memory: measuring";
#endif
static char g_netLine[128] = "Network: starting";

static double secs(void) { return (sceKernelGetSystemTimeWide() - g_t0) / 1e6; }

const char* bgtestTitle(void) { return g_title; }

const char* result(char* buf, size_t size, int value) {
    if (value < 0)
        snprintf(buf, size, "error 0x%08X", (unsigned)value);
    else
        snprintf(buf, size, "%d", value);
    return buf;
}

// Whole lines, one thread at a time: the file is not safe to write from two.
void logLine(const char* fmt, ...) {
    if (!g_log) return;
    char line[512];
    int n = snprintf(line, sizeof(line), "%9.3f ", secs());
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
    if (g_logMutex >= 0) sceKernelUnlockMutex(g_logMutex, 1);
}

// VPLXBGT12 and 13: one package, two programs (see the header).
static int switchTest(void) {
    return strcmp(g_title, "VPLXBGT12") == 0 || strcmp(g_title, "VPLXBGT13") == 0;
}

// VPLXBGT14 to 21: the player as the app's background program (bgapp.c).
static int bgappTest(void) {
    static const char* const titles[] = {"VPLXBGT14", "VPLXBGT15", "VPLXBGT16", "VPLXBGT17",
                                         "VPLXBGT18", "VPLXBGT19", "VPLXBGT20", "VPLXBGT21"};
    for (unsigned i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        if (strcmp(g_title, titles[i]) == 0) return 1;
    return 0;
}

#ifndef BGTEST_SYSTEM_MODE
// VPLXBGT20 and 21: those of them that quit as soon as the player is started.
static int quitsAfterStart(void) {
    return strcmp(g_title, "VPLXBGT20") == 0 || strcmp(g_title, "VPLXBGT21") == 0;
}
#endif

static const char* describe(void) {
#ifdef BGTEST_BGAPP
    return "its background program, eboot2.bin, started with sceBgAppUtilStartBgApp";
#endif
#ifdef BGTEST_PLAYER
    if (switchTest()) return "its player, player.self, switched to with sceAppMgrLoadExec";
#endif
    if (strcmp(g_title, "VPLXBGT14") == 0) return "a game that starts its player as a background program";
    if (strcmp(g_title, "VPLXBGT15") == 0) return "the game of 14 without extended memory";
    if (strcmp(g_title, "VPLXBGT17") == 0) return "a non-game app set up as VitaPlex BG, starting its player";
    if (strcmp(g_title, "VPLXBGT18") == 0) return "the app of 17 without extended memory";
    if (strcmp(g_title, "VPLXBGT19") == 0) return "the game of 14 with the ATTRIBUTE of BGFTP's main app";
    if (strcmp(g_title, "VPLXBGT20") == 0) return "a non-game app that starts its player and quits at once";
    if (strcmp(g_title, "VPLXBGT21") == 0) return "the app of 20 with extended memory, as VitaPlex BG has";
    if (strcmp(g_title, "VPLXBGT12") == 0) return "a non-game app that switches itself to a player";
    if (strcmp(g_title, "VPLXBGT13") == 0) return "a game that switches itself to a player";
    if (strcmp(g_title, "VPLXBGT01") == 0) return "a game, set up like VitaPlex (the control)";
    if (strcmp(g_title, "VPLXBGT02") == 0) return "a game with the BG_APP flag";
    if (strcmp(g_title, "VPLXBGT03") == 0) return "a non-game app, set up like ElevenMPV-A";
    if (strcmp(g_title, "VPLXBGT04") == 0) return "a non-game app with VitaPlex's memory setting";
    if (strcmp(g_title, "VPLXBGT05") == 0) return "a system-mode app with ElevenMPV-A's 16 MB";
    if (strcmp(g_title, "VPLXBGT06") == 0) return "a system-mode app grown as ElevenMPV-A grows";
    if (strcmp(g_title, "VPLXBGT07") == 0) return "a silent game with VitaPlex's extended memory";
    if (strcmp(g_title, "VPLXBGT08") == 0) return "a silent game without extended memory";
    if (strcmp(g_title, "VPLXBGT09") == 0) return "the silent game of 7, giving up the BGM output";
    if (strcmp(g_title, "VPLXBGT10") == 0) return "a system-mode app playing songs through the shell";
    if (strcmp(g_title, "VPLXBGT11") == 0) return "the player of 10, carrying on a song from VitaPlex";
    return "unknown variant";
}

// The variants that make no sound of their own: the silent games, for opening
// while another one beeps, and the players, whose sound comes from the shell.
static int silent(void) {
#ifdef BGTEST_BGAPP
    return 1;   // whatever title id it ends up with
#endif
    return strcmp(g_title, "VPLXBGT07") == 0 || strcmp(g_title, "VPLXBGT08") == 0 ||
           strcmp(g_title, "VPLXBGT09") == 0 || strcmp(g_title, "VPLXBGT10") == 0 ||
           strcmp(g_title, "VPLXBGT11") == 0 || switchTest() || bgappTest();
}

// ── Switching program (VPLXBGT12 and 13) ─────────────────────────────────

// One path whatever the app, so that a program the system tells no title id
// still finds the note.
#define SWITCH_NOTE "ux0:data/VitaPlex/bgtest-switch.txt"

static char g_program[16] = "eboot.bin";    // which of the app's programs this is
static const char* volatile g_switchingTo;  // the one asked for, for the quit line
#ifdef BGTEST_PLAYER
static volatile int g_switchBack;           // the watcher asks, the main thread switches
#endif
#ifdef BGTEST_BGAPP
static int g_parentKnown;                   // g_title is the app that started it
static double g_startedAt;                  // on the system clock
#endif

typedef struct SwitchNote {
    char title[16], from[16], to[16];
    double t0, asked, since;
} SwitchNote;

// The note a program leaves as it switches (switchTo): the app's title id,
// the program it was, the one it asked for, when the first program started
// and when it asked. The last two are on the system clock, which runs on from
// one program to the next. Read once and removed. 1 when it is fresh and for
// this app: the log then carries on, its clock continuing from the first
// program's, and a program the system told no title id (titleKnown 0) takes
// it from here. The background program of 14 to 16 has a note of its own.
#ifndef BGTEST_BGAPP
static int takeSwitchNote(SwitchNote* n, int titleKnown) {
    memset(n, 0, sizeof(*n));
    FILE* f = fopen(SWITCH_NOTE, "r");
    if (!f) return 0;
    char line[160];
    const int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    remove(SWITCH_NOTE);
    int at = 0;
    if (!got || sscanf(line, "%15s %15s %15s %n", n->title, n->from, n->to, &at) < 3 || !at)
        return 0;
    char* p = line + at;
    n->t0 = strtod(p, &p);
    n->asked = strtod(p, &p);
    n->since = sceKernelGetSystemTimeWide() / 1e6 - n->asked;
    if (n->t0 <= 0 || n->asked <= 0 || n->since < 0 || n->since > 30) return 0;
    if (titleKnown && strcmp(n->title, g_title) != 0) return 0;
    snprintf(g_title, sizeof(g_title), "%s", n->title);
    snprintf(g_program, sizeof(g_program), "%s", n->to);
    g_t0 = (SceInt64)(n->t0 * 1e6);
    return 1;
}
#endif

// Only the two programs that switch need what follows: the build with a
// screen and the player. The system-mode beeper of 5 and 6 never does.
#if !defined(BGTEST_SYSTEM_MODE) || defined(BGTEST_PLAYER)
// Replaces this program with app0:/<program> of the same app, as RetroArch
// switches between its cores (frontend_psp_exec): that path form, no argv, no
// options, and the process ending right after. It had not returned when the
// system asked the app to quit, 69 ms after (log 4a8451a1), so it must not
// run on the watcher, which answers that. Returns only if the system refused.
static void switchTo(const char* program) {
    char app[32], r[24];
    FILE* f = fopen(SWITCH_NOTE, "w");
    if (f) {
        fprintf(f, "%s %s %s %.6f %.6f\n", g_title, g_program, program, g_t0 / 1e6,
                sceKernelGetSystemTimeWide() / 1e6);
        fclose(f);
    }
    snprintf(app, sizeof(app), "app0:/%s", program);
    logLine("switching to %s with sceAppMgrLoadExec%s", app, g_away ? ", away" : "");
    g_switchingTo = program;
    const int rc = sceAppMgrLoadExec(app, NULL, NULL);
    logLine("sceAppMgrLoadExec(%s): %s", app, result(r, sizeof(r), rc));
    if (rc >= 0) {
        logLine("quit, for the switch to %s", program);
        sceKernelExitProcess(0);
    }
    g_switchingTo = NULL;
    logLine("still %s", g_program);
    remove(SWITCH_NOTE);
}
#endif

// ── Background program (VPLXBGT14 to 21) ─────────────────────────────────

#ifndef BGTEST_SYSTEM_MODE
static char g_bgappLine[96] = "Player: not started";
// 20 and 21 quitting after starting their player: its rescue leaves the
// player be, since that is what the app leaves behind.
static volatile int g_handingOver;

// The title id the system gives the app's background program: BGP for BGT
// (CMakeLists.txt, its param2.sfo).
static void playerTitle(char* out, size_t size) {
    snprintf(out, size, "%s", g_title);
    char* p = strstr(out, "BGT");
    if (p) p[2] = 'P';
}

// X in VPLXBGT14, 15 and 17 to 21: the player, as the app's background
// program. 0 or more when the system took the request. A refusal also logs
// whether the system still lists a player: 20 was refused (0x80010013) after
// its player had stopped when asked (log 10c46450).
static int startPlayerApp(void) {
    char r[24];
    const int rc = bgappStart(g_title, g_t0 / 1e6);
    if (rc >= 0) {
        snprintf(g_bgappLine, sizeof(g_bgappLine), "Player: started (at %.0fs)", secs());
    } else {
        snprintf(g_bgappLine, sizeof(g_bgappLine), "Player: refused, %s", result(r, sizeof(r), rc));
        char player[16];
        playerTitle(player, sizeof(player));
        SceUID pid = -1;
        const int listed = sceAppMgrGetIdByName(&pid, player);
        if (listed >= 0)
            logLine("background program: refused, with its player %s listed, process 0x%08X",
                    player, (unsigned)pid);
        else
            logLine("background program: refused, with its player %s not listed (%s)", player,
                    result(r, sizeof(r), listed));
    }
    return rc;
}

// Opened while its player plays, the app takes the music back, as VitaPlex
// would: it asks the player to stop and waits up to 10 s for it to go, since
// an app could not quit while its player ran (VPLXBGT18). The system lists a
// background program by its title id, even one whose start is still to come
// (log 10c46450). The request goes out all the same when it does not, in case
// it runs unlisted; a running player takes it within a second, and one that
// nothing has taken in 2 s is taken back.
static void takeBackPlayer(void) {
    char player[16], r[24];
    playerTitle(player, sizeof(player));
    SceUID pid = -1;
    const int rc = sceAppMgrGetIdByName(&pid, player);
    const int wasListed = rc >= 0;
    if (wasListed)
        logLine("its player %s: running, process 0x%08X; asking it to stop, to take the music back",
                player, (unsigned)pid);
    else
        logLine("its player %s: %s; asking it to stop all the same, in case it runs unlisted",
                player, result(r, sizeof(r), rc));
    bgappAskQuit("its app is open again, and takes the music back");
    const SceInt64 asked = sceKernelGetSystemTimeWide();
    int took = 0;
    for (;;) {
        sceKernelDelayThread(100 * 1000);
        const double waited = (sceKernelGetSystemTimeWide() - asked) / 1e6;
        const int listed = sceAppMgrGetIdByName(&pid, player) >= 0;
        if (!took && !bgappQuitWaiting()) {
            took = 1;
            logLine("its player took the request after %.2f s", waited);
        }
        const char* line = NULL;
        if (took && !listed) {
            logLine("its player has gone%s, %.2f s after it was asked",
                    wasListed ? "" : " (if the system lists it at all)", waited);
            line = "Player: was playing; stopped, taking the music back";
        } else if (!took && !listed && waited >= 2.0) {
            logLine("nothing took the request in 2 s, and no player is listed");
            line = "Player: not started";
        } else if (waited >= 10.0) {
            logLine("its player %s the request, and is still listed 10 s after it was asked",
                    took ? "took" : "did not take");
            line = "Player: still running; it did not stop when asked";
        }
        if (line) {
            snprintf(g_bgappLine, sizeof(g_bgappLine), "%s", line);
            break;
        }
    }
    bgappCancelQuit();
}
#endif

#if !defined(BGTEST_SYSTEM_MODE) || defined(BGTEST_BGAPP)
// _sceKernelExitProcessForUser, the system call that sceKernelExitProcess
// ends with, through this project's own import stub (exit_stub.S).
int bgtestRawExit(int status);

// Started as the process begins to quit (armRescue), it wakes 3 s later only
// if the process is still there. VPLXBGT18's sceKernelExitProcess never
// returned once its player ran: its other threads went on for minutes, and
// the game that had asked it to quit hung, waiting for it. The app asks its
// player to stop, in case the exit waits for that, and gives it 5 s to go;
// then the app, or the player itself, ends through the system call
// underneath, past whatever LibKernel does before it. An app handing the
// music over (g_handingOver) leaves the player be and goes after 2 s.
static int rescueMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
#ifndef BGTEST_BGAPP
    if (g_handingOver) {
        sceKernelDelayThread(2 * 1000 * 1000);
        logLine("still running 2 s after it began to quit, handing the music over; ending "
                "through _sceKernelExitProcessForUser, leaving the player be");
        bgtestRawExit(0);
        sceKernelDelayThread(3 * 1000 * 1000);
        logLine("still running after that too: restart the console by holding the power button");
        return 0;
    }
#endif
    sceKernelDelayThread(3 * 1000 * 1000);
    logLine("still running 3 s after it began to quit");
#ifndef BGTEST_BGAPP
    char player[16], r[24];
    playerTitle(player, sizeof(player));
    SceUID pid = -1;
    const int rc = sceAppMgrGetIdByName(&pid, player);
    logLine("its player %s: %s; asking it to stop, in case the exit waits for it", player,
            rc >= 0 ? "running" : result(r, sizeof(r), rc));
    bgappAskQuit("its app cannot quit while it runs");
    const SceInt64 asked = sceKernelGetSystemTimeWide();
    while (sceKernelGetSystemTimeWide() - asked < 5 * 1000000LL &&
           (bgappQuitWaiting() || sceAppMgrGetIdByName(&pid, player) >= 0))
        sceKernelDelayThread(100 * 1000);
    // A second more for the exit to finish, if the player was what held it.
    sceKernelDelayThread(1000 * 1000);
    logLine("still running %.1f s after asking its player to stop, which %s the request and is %s",
            (sceKernelGetSystemTimeWide() - asked) / 1e6, bgappQuitWaiting() ? "has not taken" : "took",
            sceAppMgrGetIdByName(&pid, player) >= 0 ? "still listed" : "not listed");
    bgappCancelQuit();
#endif
    logLine("ending through _sceKernelExitProcessForUser, the system call underneath");
    bgtestRawExit(0);
    sceKernelDelayThread(3 * 1000 * 1000);
    logLine("still running after that too: restart the console by holding the power button");
    return 0;
}

// Starts the rescue, once, in the apps that start a background program and in
// that program itself.
static int g_rescueArmed;
static void armRescue(void) {
#ifdef BGTEST_BGAPP
    const int rescue = 1;
#else
    const int rescue = bgappTest();
#endif
    if (!rescue || g_rescueArmed) return;
    g_rescueArmed = 1;
    const SceUID t =
        sceKernelCreateThread("BgTestRescue", rescueMain, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

// Ends the process, with the rescue behind it. The log stays open for it.
static void quitNow(void) {
    armRescue();
    logLine("quit");
    sceKernelExitProcess(0);
}
#endif

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

#ifdef BGTEST_PLAYER
static char g_netMemory[1024 * 1024];   // whole songs, over https
#else
static char g_netMemory[256 * 1024];
#endif

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
    g_netReady = 1;

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
#ifdef BGTEST_PLAYER
    playerPowerEvent(powerInfo);
#endif
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
#ifdef BGTEST_BGAPP
    int lastAppRc = 1;   // no call returns that, so the first look is logged
    SceUID lastAppPid = -1;
#endif
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
        const int prevAdopt = lastAdopt;
        int adopt = sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_BGM);
        const int gain = sceAudioOutGetPortVolume_forUser(SCE_AUDIO_OUT_PORT_TYPE_BGM);
        if (adopt != lastAdopt || gain != lastGain) {
            char a[24], g[24];
            logLine("BGM output%s: adopted %s, gain %s", g_away ? ", away" : "",
                    result(a, sizeof(a), adopt), result(g, sizeof(g), gain));
            lastAdopt = adopt;
            lastGain = gain;
        }
        // A game with extended memory has the BGM output adopted for it a
        // moment after it starts, without opening a BGM port, and other
        // apps' beeps go quiet while it is in front (VPLXBGT07). This one
        // gives it up each time it appears, to see whether they come back.
        // Once per appearance, so a refusal is logged once, not every tick.
        if (adopt == 1 && prevAdopt != 1 && strcmp(g_title, "VPLXBGT09") == 0) {
            char r[24], a[24];
            const int rc = sceAudioOutSetAdopt_forUser(SCE_AUDIO_OUT_PORT_TYPE_BGM, SCE_FALSE, 100,
                                                       SCE_FALSE);
            adopt = sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_BGM);
            logLine("give up the BGM output: %s; adopted now %s", result(r, sizeof(r), rc),
                    result(a, sizeof(a), adopt));
            lastAdopt = adopt;
        }

#ifdef BGTEST_BGAPP
        // Whether the app that started it is still there: a game is held once
        // PS takes it out of the front, and closed when another game starts.
        // Logged as that changes.
        if (g_parentKnown) {
            SceUID pid = -1;
            const int rc = sceAppMgrGetIdByName(&pid, g_title);
            if (rc != lastAppRc || (rc >= 0 && pid != lastAppPid)) {
                char r[24];
                if (rc >= 0)
                    logLine("its app %s: running, process 0x%08X", g_title, (unsigned)pid);
                else   // 0x80802012: no such application or process
                    logLine("its app %s: %s%s", g_title, result(r, sizeof(r), rc),
                            rc == (int)0x80802012 ? ", not running" : "");
                lastAppRc = rc;
                lastAppPid = pid;
            }
        }
        // Another app asking for its player, one test after another: this
        // one goes, so that the two do not play over each other. Its own
        // note was read and removed before the watcher started.
        if (bgappNoteWaiting()) {
            logLine("another app is starting its player; quitting, so that the two do not "
                    "play over each other");
            playerQuit("replaced");
            quitNow();
        }
        // Its app asking it to stop: open again and taking the music back,
        // or unable to quit while this runs (rescueMain).
        char why[96];
        const int asked = bgappTakeQuit(g_startedAt, why, sizeof(why));
        if (asked < 0) logLine("a request to stop from before it started, so for another one: dropped");
        if (asked > 0) {
            logLine("its app asks it to stop: %s", why);
            playerStop("asked by its app");
            quitNow();
        }
#endif

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
                if (id == EVENT_ACTIVATE) {
#ifdef BGTEST_PLAYER
                    // Opened again after PS: back to the program with the
                    // screen, which the main thread does (switchTo).
                    if (g_away && switchTest()) g_switchBack = 1;
#endif
                    g_away = 0;
                }
                if (id == EVENT_REQUEST_QUIT) {
                    // Ended here rather than by the main loop, which could be
                    // stuck: the system starts the next app only once this
                    // process ends, and VitaPlex not ending hung the console
                    // (log 3ea218f5). That holds during a switch too: one
                    // build left a switch's request to the system for 5 s,
                    // and closing the app after the error then crashed the
                    // console.
#if defined(BGTEST_BGAPP)
                    // As its app's LiveArea is closed, say: the music goes
                    // with it.
                    playerStop("asked to quit");
                    quitNow();
#elif defined(BGTEST_PLAYER)
                    // Or the shell plays on with nothing to stop it. A switch
                    // back has done this already.
                    if (!g_switchingTo) playerQuit("asked to quit");
#elif !defined(BGTEST_SYSTEM_MODE)
                    // 20 and 21 hand the music to their player as they go, as
                    // VitaPlex would when a game is started over it, and end
                    // through the system call underneath at once. On a quit
                    // request sceKernelExitProcess has not returned in an app
                    // that had used SceBgAppUtil: with its player running
                    // (18), just asked for, and refused (20, log 10c46450).
                    // The system starts the player once the app has gone. The
                    // rescue goes first, in case the request for the player
                    // is held.
                    if (quitsAfterStart()) {
                        g_handingOver = 1;
                        armRescue();
                        startPlayerApp();
                        logLine("quit, through _sceKernelExitProcessForUser at once");
                        bgtestRawExit(0);
                    }
                    if (bgappTest()) quitNow();
#endif
                    if (g_switchingTo)
                        logLine("quit, for the switch to %s", g_switchingTo);
                    else
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
    if (switchTest()) {
        drawText(font, 40, 125, white, 1.0f, "Triangle: the app restarts this screen. O: it switches");
        drawText(font, 40, 150, white, 1.0f, "to a copy of it. This screen coming back means it works.");
        drawText(font, 40, 185, white, 1.0f, "X: it switches to its player; the screen goes black.");
        drawText(font, 40, 215, white, 1.0f, "Press PS and start a game. 15 s after PS the player");
        drawText(font, 60, 240, muted, 1.0f, "plays two songs: do you hear them in the game?");
        drawText(font, 40, 270, white, 1.0f, "Open this app again after: it switches back here.");
        drawText(font, 40, 305, white, 1.0f, "Now %s. START quits. Running %.0fs", g_program, secs());
    } else if (quitsAfterStart()) {
        drawText(font, 40, 120, white, 1.0f, "X starts the player as a background program, and this");
        drawText(font, 40, 145, white, 1.0f, "app quits at once. A song plays soon after.");
        drawText(font, 40, 175, white, 1.0f, "Press PS and start BG Test 8: does the music go on?");
        drawText(font, 40, 205, white, 1.0f, "Open this app again: it stops the player, taking the music back.");
        drawText(font, 40, 235, white, 1.0f, "Press PS and start BG Test 8 again: this app is closed, and");
        drawText(font, 60, 260, muted, 1.0f, "starts its player as it goes. Does the music start in the game?");
        drawText(font, 40, 290, gold, 1.0f, "%s", g_bgappLine);
        drawText(font, 40, 315, white, 1.0f, "START quits without the player. Running %.0fs", secs());
    } else if (bgappTest()) {
        drawText(font, 40, 125, white, 1.0f, "X starts the player as a background program.");
        drawText(font, 40, 155, white, 1.0f, "A song plays through the system player soon after.");
        drawText(font, 40, 185, white, 1.0f, "Then press PS and start a game. Does the music go on,");
        drawText(font, 60, 210, muted, 1.0f, "and does the second song follow the first?");
        drawText(font, 40, 240, white, 1.0f, "After that, close this app's LiveArea (peel it off).");
        drawText(font, 40, 275, gold, 1.0f, "%s", g_bgappLine);
        drawText(font, 40, 305, white, 1.0f, "START quits. Running %.0fs", secs());
    } else if (silent()) {
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
    g_logMutex = sceKernelCreateMutex("BgTestLog", 0, 0, NULL);
    const int rcTitle =
        sceAppMgrAppParamGetString(SCE_KERNEL_PROCESS_ID_SELF, 12, g_title, sizeof(g_title));  // 12: TITLE_ID

    char path[96];
#ifdef BGTEST_BGAPP
    // Started by its app, which leaves a note as it asks (bgapp.c): the log is
    // named after the app, and its clock carries on from the app's start.
    // Appended to, in case the app starts it twice.
    char own[16];
    snprintf(own, sizeof(own), "%s", g_title);
    g_startedAt = g_t0 / 1e6;
    double t0 = 0, since = 0;
    g_parentKnown = bgappTakeNote(g_title, sizeof(g_title), &t0, &since);
    if (g_parentKnown) g_t0 = (SceInt64)(t0 * 1e6);
    snprintf(path, sizeof(path), "ux0:data/VitaPlex/bgtest-%s-bgapp.log",
             g_parentKnown || rcTitle >= 0 ? g_title : "noid");
    g_log = fopen(path, "a");
    logLine("start: %s, %s", g_title, describe());
    if (g_parentKnown)
        logLine("started %.2f s after %s asked for it", since, g_title);
    else
        logLine("no note from its app: started by something else, or over 30 s after it was asked");
    if (rcTitle >= 0) logLine("the system gives it the title id %s", own);
    const int switched = 0;
#else
    // A program VPLXBGT12 or 13 switched to carries the log on, and takes the
    // app's title id from the note when the system tells it none. 14 to 16's
    // are appended to, so that opening one again after the test keeps the run.
    SwitchNote note;
    const int switched = (rcTitle < 0 || switchTest()) && takeSwitchNote(&note, rcTitle >= 0);
    snprintf(path, sizeof(path), "ux0:data/VitaPlex/bgtest-%s.log",
             rcTitle >= 0 || switched ? g_title : "noid");
    g_log = fopen(path, switched || bgappTest() ? "a" : "w");
    if (switchTest())
        logLine("start: %s, %s, as %s", g_title, describe(), g_program);
    else
        logLine("start: %s, %s", g_title, describe());
    if (switched)
        logLine("switched to %s from %s: started %.2f s after it asked", note.to, note.from,
                note.since);
#endif
    if (rcTitle < 0) {
        char r[24];
        logLine("no title id from the system (%s)%s", result(r, sizeof(r), rcTitle),
                switched ? "; took it from the switch note" : "");
    }
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
#if defined(BGTEST_BGAPP)
    playerStart(PLAYER_BACKGROUND);
#elif defined(BGTEST_PLAYER)
    if (strcmp(g_title, "VPLXBGT10") == 0 || switchTest()) playerStart(PLAYER_AWAY);
    if (strcmp(g_title, "VPLXBGT11") == 0) playerStart(PLAYER_HANDOFF);
#endif

#ifdef BGTEST_SYSTEM_MODE
    // No START to quit, unlike the others: there is no screen to say so, and
    // this one is meant to run on while a game is in front, where START is
    // pressed all the time. Closed from the LiveArea, it is asked to quit and
    // the watcher ends it.
    for (;;) {
#ifdef BGTEST_PLAYER
        // The switch back VPLXBGT12 and 13's watcher asks for (switchTo).
        if (g_switchBack) {
            g_switchBack = 0;
            playerQuit("switching back");
            switchTo("eboot.bin");
        }
        sceKernelDelayThread(100 * 1000);
#else
        sceKernelDelayThread(60 * 1000 * 1000);
#endif
    }
#else
    if (bgappTest()) {
        snprintf(g_bgappLine, sizeof(g_bgappLine), "Player: asking one that runs to stop");
        if (font) drawScreen(font, path);
        takeBackPlayer();
    }
    SceCtrlData pad;
    unsigned held = 0;
    for (;;) {
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;
        // Once a press, in VPLXBGT12 and 13: X switches to the player, O to a
        // copy of this program and triangle to this program again, the
        // controls for the switch itself. In 14, 15 and 17 to 21, X starts
        // the player as the app's background program, and 20 and 21 then
        // quit at once, as BGFTP's main app does, which is gone before its
        // program is up: 18 stayed, and could not quit once its player ran.
        const unsigned pressed = pad.buttons & ~held;
        held = pad.buttons;
        if (switchTest() && (pressed & SCE_CTRL_CROSS)) switchTo("player.self");
        if (switchTest() && (pressed & SCE_CTRL_CIRCLE)) switchTo("screen.self");
        if (switchTest() && (pressed & SCE_CTRL_TRIANGLE)) switchTo("eboot.bin");
        if (bgappTest() && (pressed & SCE_CTRL_CROSS) && startPlayerApp() >= 0 &&
            quitsAfterStart()) {
            logLine("quitting at once, as BGFTP's main app does");
            g_handingOver = 1;
            quitNow();
        }
        if (!font) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        drawScreen(font, path);
    }
    if (bgappTest()) quitNow();
#endif

    logLine("quit");
    if (g_log) fclose(g_log);
    sceKernelExitProcess(0);
    return 0;
}
