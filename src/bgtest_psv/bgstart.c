/**
 * VPLXBGT16's eboot.bin: what BGFTP's main app does, the control for BG Tests
 * 14 and 15. It starts the background program its package carries, the player
 * (bgapp.c), and quits. Like that app it is a small non-game app in system
 * mode with no screen, though with 16 MB where BGFTP's has 4: the system loads
 * its own modules into every vitasdk program (libc, fios2 and more), which
 * BGFTP's main app turns off to fit in that.
 */

#include "bgtest.h"

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int _newlib_heap_size_user = 1 * 1024 * 1024;

static FILE* g_log;
static SceInt64 g_t0;

const char* result(char* buf, size_t size, int value) {
    if (value < 0)
        snprintf(buf, size, "error 0x%08X", (unsigned)value);
    else
        snprintf(buf, size, "%d", value);
    return buf;
}

// One thread, so no lock, unlike main.c's.
void logLine(const char* fmt, ...) {
    if (!g_log) return;
    fprintf(g_log, "%9.3f ", (sceKernelGetSystemTimeWide() - g_t0) / 1e6);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

int main(void) {
    g_t0 = sceKernelGetSystemTimeWide();
    char title[16] = "?";
    const int rcTitle =
        sceAppMgrAppParamGetString(SCE_KERNEL_PROCESS_ID_SELF, 12, title, sizeof(title));  // 12: TITLE_ID
    char path[96];
    snprintf(path, sizeof(path), "ux0:data/VitaPlex/bgtest-%s.log", rcTitle >= 0 ? title : "noid");
    // Appended to, as for 14 and 15, so that opening it again keeps the first run.
    g_log = fopen(path, "a");
    logLine("start: %s, a small non-game app that starts its background program and quits, as "
            "BGFTP's main app does",
            title);
    if (rcTitle < 0) {
        char r[24];
        logLine("no title id from the system (%s)", result(r, sizeof(r), rcTitle));
    }
    bgappStart(title, g_t0 / 1e6);
    logLine("quit");
    if (g_log) fclose(g_log);
    sceKernelExitProcess(0);
    return 0;
}
