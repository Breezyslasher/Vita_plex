/**
 * VPLXBGT14 to 16: the player as its app's background application.
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
 */

#include "bgtest.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <string.h>

// SceBgAppUtil (psp2/bgapputil.h): starts the package's eboot2.bin. vitasdk's
// header says the mode must be 1; BGFTP passes 0.
int sceBgAppUtilStartBgApp(int mode);

// One path whatever the app, as for the switch tests' note.
#define BGAPP_NOTE "ux0:data/VitaPlex/bgtest-bgapp.txt"

static double systemSecs(void) { return sceKernelGetSystemTimeWide() / 1e6; }

int bgappStart(const char* title, double t0) {
    char r[24];
    FILE* f = fopen(BGAPP_NOTE, "w");
    if (f) {
        fprintf(f, "%s %.6f %.6f\n", title, t0, systemSecs());
        fclose(f);
    }
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
