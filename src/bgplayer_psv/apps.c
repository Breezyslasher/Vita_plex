/**
 * Which other apps run beside the player, and which of them have extended
 * memory.
 *
 * Opening BG Test 21, whose param.sfo is VitaPlex BG's, ended its own player
 * every time while the shell played on (build 2012). Whether every app with
 * extended memory ends a background program is not known. The test's player
 * outlived VitaShell, but VitaShell behaved as an app without it in the beep
 * tests: the beeps went on beside it and went quiet beside BG Test 7 and
 * VitaSurf, which have it (build 1971). Extended memory is ATTRIBUTE2 in an
 * app's param.sfo (vita-mksfoex's MEM29, MEM77 and MEM109, 4, 8 and 12), so the
 * player logs every installed app's once, and then every 2 s which of them
 * run. An app that ends it is the last one logged as starting.
 */

#include "bgplayer.h"
#include "program.h"

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_APPS 400

typedef struct App {
    char id[12];
    char category[8];
    int attribute2;   // -1 when its param.sfo does not give one
    int running;
} App;

static App* g_apps;
static int g_appCount;

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }

// ATTRIBUTE2 and CATEGORY from a param.sfo: a header, an index of keys, then
// the keys' names and their values.
static void readSfo(const char* path, App* a) {
    a->attribute2 = -1;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    static unsigned char buf[16 * 1024];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 20 || memcmp(buf, "\0PSF", 4) != 0) return;
    const uint32_t keys = le32(buf + 8), data = le32(buf + 12), count = le32(buf + 16);
    for (uint32_t i = 0; i < count && 20 + 16 * (i + 1) <= n; i++) {
        const unsigned char* e = buf + 20 + 16 * i;
        const uint32_t name = keys + le16(e), len = le32(e + 4), at = data + le32(e + 12);
        if (name >= n || at + len > n) continue;
        const char* key = (const char*)buf + name;
        if (strncmp(key, "ATTRIBUTE2", n - name) == 0 && len == 4)
            a->attribute2 = (int)le32(buf + at);
        else if (strncmp(key, "CATEGORY", n - name) == 0 && len > 0)
            snprintf(a->category, sizeof(a->category), "%.*s", (int)len, (const char*)buf + at);
    }
}

static void readApps(void) {
    g_apps = (App*)calloc(MAX_APPS, sizeof(App));
    if (!g_apps) return;
    const SceUID d = sceIoDopen("ux0:app");
    if (d < 0) return;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0 || g_appCount >= MAX_APPS) break;
        if (e.d_name[0] == '.' || strlen(e.d_name) != 9) continue;
        App* a = &g_apps[g_appCount++];
        snprintf(a->id, sizeof(a->id), "%s", e.d_name);
        char path[64];
        snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/param.sfo", a->id);
        readSfo(path, a);
    }
    sceIoDclose(d);
}

static void describe(const App* a, char* out, size_t size) {
    if (a->attribute2 < 0)
        snprintf(out, size, "%s (%s, no ATTRIBUTE2)", a->id, a->category);
    else
        snprintf(out, size, "%s (%s, ATTRIBUTE2 %d)", a->id, a->category, a->attribute2);
}

// The installed apps with extended memory, a few to a line.
static void logApps(void) {
    int extended = 0;
    char line[400] = "";
    for (int i = 0; i < g_appCount; i++) {
        const App* a = &g_apps[i];
        if (a->attribute2 <= 0) continue;
        extended++;
        char one[48];
        describe(a, one, sizeof(one));
        if (strlen(line) + strlen(one) + 2 >= sizeof(line)) {
            logLine("apps with extended memory: %s", line);
            line[0] = '\0';
        }
        if (line[0]) strcat(line, ", ");
        strcat(line, one);
    }
    if (line[0]) logLine("apps with extended memory: %s", line);
    logLine("%d app(s) installed, %d of them with extended memory", g_appCount, extended);
}

static int appsMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    // After the first song is out: reading a few hundred files holds the
    // memory card up.
    sceKernelDelayThread(20 * 1000 * 1000);
    readApps();
    logApps();
    for (int first = 1;; first = 0) {
        for (int i = 0; i < g_appCount; i++) {
            App* a = &g_apps[i];
            // Its own app has its own lines (main.c).
            if (strcmp(a->id, BGPLAYER_APP_TITLEID) == 0) continue;
            SceUID pid = -1;
            const int running = sceAppMgrGetIdByName(&pid, a->id) >= 0;
            if (running == a->running && !(first && running)) continue;
            a->running = running;
            char one[48];
            describe(a, one, sizeof(one));
            logLine("%s %s", one, running ? (first ? "is running beside it" : "started beside it")
                                          : "has gone");
        }
        sceKernelDelayThread(2 * 1000 * 1000);
    }
    return 0;
}

void appsWatchStart(void) {
    const SceUID t = sceKernelCreateThread("BgPlayerApps", appsMain, 0x10000100 + 30, 0x8000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
