/**
 * VPLXBGT14 to 21: the player as its app's background application.
 *
 * A package can carry a second program, eboot2.bin with its own
 * sce_sys/param2.sfo, which the app starts with sceBgAppUtilStartBgApp and
 * which then runs as a process of its own. GrapheneCt's BGFTP runs its FTP
 * server that way, through games, from a main app that does nothing but start
 * it and quit (docs/DESIGN_NOTES.md). Here that program is BG Test 10's
 * player, which plays two songs through the shell's music service.
 *
 * The app leaves a note as it asks: its title id, when it started and when it
 * asked, on the system clock, which runs on from one process to the next. The
 * background program reads it as it starts, so that its log names the app
 * that started it and how long that took, and its clock carries on from the
 * app's.
 *
 * The app asks things of a running program with notes of its own, which say
 * when and why: to pause as it opens again and takes the music back, and to
 * play again as it goes (VPLXBGT20 and 21); or to stop for good when it
 * cannot quit while the program runs, as VPLXBGT18 could not. With one note
 * for every app, 21's program took 20's request.
 *
 * As it asks, the app also copies its CA bundle to ux0:data for the program.
 * A program started before its app had gone lost app0: when the app was ended
 * under it, and every https request then failed.
 */

#include "bgtest.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <string.h>

// SceBgAppUtil (psp2/bgapputil.h): starts the package's eboot2.bin. vitasdk's
// header says the mode must be 1; BGFTP passes 0.
int sceBgAppUtilStartBgApp(int mode);

// One path whatever the app, as for the switch tests' note: a program that
// sees another app's note appear gives way to that app's program.
#define BGAPP_NOTE "ux0:data/VitaPlex/bgtest-bgapp.txt"

static double systemSecs(void) { return sceKernelGetSystemTimeWide() / 1e6; }

static void notePath(char* out, size_t size, const char* app, int what) {
    static const char* const kinds[] = {"quit", "pause", "play"};
    snprintf(out, size, "ux0:data/VitaPlex/bgtest-%s-%s.txt", app, kinds[what]);
}

static long fileSize(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    const long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    fclose(f);
    return size;
}

// The app's CA bundle to BGAPP_CA_BUNDLE, unless a copy of its size is there
// already: the bytes copied, 0 for none needed, or a negative step that
// failed. Written beside it and then moved in, so that a program never finds
// half of one.
static int copyCaBundle(void) {
    static const char part[] = BGAPP_CA_BUNDLE ".part";
    const long size = fileSize("app0:cacert.pem");
    if (size < 0) return -1;
    if (fileSize(BGAPP_CA_BUNDLE) == size) return 0;
    FILE* in = fopen("app0:cacert.pem", "rb");
    if (!in) return -1;
    FILE* out = fopen(part, "wb");
    if (!out) {
        fclose(in);
        return -2;
    }
    static char buf[16 * 1024];
    size_t n;
    int total = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            total = -3;
            break;
        }
        total += (int)n;
    }
    fclose(in);
    if (fclose(out) != 0 && total >= 0) total = -3;
    // rename does not replace a file on every target (platform_common.cpp).
    if (total >= 0) {
        remove(BGAPP_CA_BUNDLE);
        if (rename(part, BGAPP_CA_BUNDLE) != 0) total = -4;
    }
    if (total < 0) remove(part);
    return total;
}

int bgappStart(const char* title, double t0) {
    char r[24];
    FILE* f = fopen(BGAPP_NOTE, "w");
    if (f) {
        fprintf(f, "%s %.6f %.6f\n", title, t0, systemSecs());
        fclose(f);
    }
    static const char* const failed[] = {"no app0:cacert.pem", "cannot write beside it",
                                         "the copy failed", "cannot move it in"};
    const double copyAt = systemSecs();
    const int copied = copyCaBundle();
    if (copied > 0)
        logLine("background program: CA bundle copied to %s, %d bytes in %.2f s", BGAPP_CA_BUNDLE,
                copied, systemSecs() - copyAt);
    else if (copied == 0)
        logLine("background program: CA bundle already at %s", BGAPP_CA_BUNDLE);
    else
        logLine("background program: CA bundle not copied to %s: %s", BGAPP_CA_BUNDLE,
                failed[-copied - 1]);
    logLine("background program: load the module: %s",
            result(r, sizeof(r), sceSysmoduleLoadModule(SCE_SYSMODULE_BG_APP_UTIL)));
    // As BGFTP asks, then as vitasdk documents it if that is refused.
    int rc = -1;
    for (int mode = 0; mode <= 1 && rc < 0; mode++) {
        const double asked = systemSecs();
        rc = sceBgAppUtilStartBgApp(mode);
        logLine("background program: start it with %d: %s, in %.2f s", mode,
                result(r, sizeof(r), rc), systemSecs() - asked);
    }
    if (rc < 0) remove(BGAPP_NOTE);
    return rc;
}

int bgappNoteWaiting(void) {
    FILE* f = fopen(BGAPP_NOTE, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int bgappTakeNote(char* title, size_t size, double* t0, double* since) {
    FILE* f = fopen(BGAPP_NOTE, "r");
    if (!f) return 0;
    char line[96];
    const int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    remove(BGAPP_NOTE);
    char app[16];
    double start = 0, asked = 0;
    if (!got || sscanf(line, "%15s %lf %lf", app, &start, &asked) != 3) return 0;
    *since = systemSecs() - asked;
    if (start <= 0 || asked <= 0 || *since < 0 || *since > 30) return 0;
    snprintf(title, size, "%s", app);
    *t0 = start;
    return 1;
}

void bgappAsk(const char* app, int what, const char* why) {
    char path[64];
    notePath(path, sizeof(path), app, what);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%.6f %s\n", systemSecs(), why);
    fclose(f);
}

void bgappCancel(const char* app, int what) {
    char path[64];
    notePath(path, sizeof(path), app, what);
    remove(path);
}

int bgappWaiting(const char* app, int what) {
    char path[64];
    notePath(path, sizeof(path), app, what);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

// A note it cannot read yet, caught half written, is left for the next look.
int bgappTake(const char* app, int what, double startedAt, char* why, size_t size) {
    char path[64];
    notePath(path, sizeof(path), app, what);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[160];
    const int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    char* end = got ? strchr(line, '\n') : NULL;
    double asked = 0;
    int at = 0;
    if (!end || sscanf(line, "%lf %n", &asked, &at) < 1 || !at) return 0;
    remove(path);
    if (asked < startedAt) return -1;
    *end = '\0';
    snprintf(why, size, "%s", line + at);
    return 1;
}
