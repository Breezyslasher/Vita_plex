/**
 * The quick menu's Music row, through ElevenMPV-A's shell plugin (qm_link.h).
 */

#include "qm_link.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Not declared by this SDK's headers.
int sceAppMgrGetIdByName(SceUID* pid, const char* name);

// By NID (qm_link_stub.S): SceThreadmgr's pipe open and close, which the
// SDK's headers and stubs leave out, and taiHEN's loader into another
// process, with its arguments as taihen.h's tai_module_args_t has them.
SceUID vitaplexOpenMsgPipe(const char* name);
int vitaplexCloseMsgPipe(SceUID uid);
typedef struct TaiModuleArgs {
    size_t size;
    SceUID pid;
    size_t args;
    void* argp;
    int flags;
} TaiModuleArgs;
SceUID vitaplexTaiLoadStartModuleForPidForUser(const char* path, TaiModuleArgs* args);

// The plugin's two pipes: to it, and from it. It makes them as it starts.
#define PIPE_TO_PLUGIN   "ElevenMPVA::ShellIPC_RX"
#define PIPE_FROM_PLUGIN "ElevenMPVA::ShellIPC_TX"
#define SHELL_TITLEID    "NPXS19999"

// The messages, as ipc.h has them. One goes to the plugin whole every time,
// whatever it carries, since the plugin takes them a whole one at a time.
enum {
    CMD_TERMINATE_RX,
    CMD_ACTIVATE,
    CMD_DEACTIVATE,
    CMD_PLAY,
    CMD_FF,
    CMD_REW,
    CMD_INFO,
    CMD_APP_STOP,
    CMD_APP_START
};
enum { FLAG_REFRESH_TEXT = 2 };

typedef struct ToPlugin {
    uint32_t cmd;
    uint32_t flags;
    uint32_t playButton;   // ignored by the plugin as it stands
    uint16_t title[256];
    uint16_t artist[256];
    uint16_t album[256];
} ToPlugin;
_Static_assert(sizeof(ToPlugin) == 1548, "ipc.h's IpcDataRX is 1548 bytes");

typedef struct FromPlugin {
    uint32_t cmd;
    char data[0x100];
} FromPlugin;
_Static_assert(sizeof(FromPlugin) == 260, "ipc.h's IpcDataTX is 260 bytes");

// A message pipe moves a whole message or none of it with this mode, and
// without it as much as fits (Vita3K's kernel/types.h:
// SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
#define MSG_PIPE_MODE_FULL 0x00000001
// The plugin takes each message the moment it comes, so one that cannot go
// in this long finds no plugin taking it, and is dropped.
#define SEND_TIMEOUT_US (200 * 1000)

static SceUID g_lock = -1;
static SceUID g_to = -1, g_from = -1;
static ToPlugin g_message;   // under g_lock: 1.5 KB, off the callers' stacks
static FromPlugin g_press;

static void lock(void) {
    if (g_lock >= 0) sceKernelLockMutex(g_lock, 1, NULL);
}
static void unlock(void) {
    if (g_lock >= 0) sceKernelUnlockMutex(g_lock, 1);
}

void qmInit(void) {
    if (g_lock < 0) g_lock = sceKernelCreateMutex("VitaPlexQuickMenu", 0, 0, NULL);
}

int qmIsOpen(void) {
    lock();
    const int open = g_to >= 0 && g_from >= 0;
    unlock();
    return open;
}

int qmOpen(char* what, size_t size) {
    lock();
    int rc = 0;
    if (g_to >= 0 && g_from >= 0) {
        snprintf(what, size, "ElevenMPV-A's plugin is open already");
        unlock();
        return 0;
    }
    SceUID to = vitaplexOpenMsgPipe(PIPE_TO_PLUGIN);
    int n;
    if (to >= 0) {
        n = snprintf(what, size, "ElevenMPV-A's plugin was in SceShell already");
    } else {
        SceIoStat st;
        if (sceIoGetstat(QM_PLUGIN_PATH, &st) < 0) {
            snprintf(what, size, "no ElevenMPV-A plugin at %s: is ElevenMPV-A installed?",
                     QM_PLUGIN_PATH);
            unlock();
            return to;
        }
        SceUID shell = -1;
        rc = sceAppMgrGetIdByName(&shell, SHELL_TITLEID);
        if (rc < 0) {
            snprintf(what, size, "SceShell not found: 0x%08X", (unsigned)rc);
            unlock();
            return rc;
        }
        TaiModuleArgs args;
        memset(&args, 0, sizeof(args));
        args.size = sizeof(args);
        args.pid = shell;
        const SceUID module = vitaplexTaiLoadStartModuleForPidForUser(QM_PLUGIN_PATH, &args);
        if (module < 0) {
            snprintf(what, size, "loading ElevenMPV-A's plugin into SceShell: 0x%08X",
                     (unsigned)module);
            unlock();
            return module;
        }
        to = vitaplexOpenMsgPipe(PIPE_TO_PLUGIN);
        n = snprintf(what, size, "loaded ElevenMPV-A's plugin into SceShell (module 0x%08X)",
                     (unsigned)module);
    }
    const SceUID from = vitaplexOpenMsgPipe(PIPE_FROM_PLUGIN);
    if (to < 0 || from < 0) {
        if (n > 0 && (size_t)n < size)
            snprintf(what + n, size - (size_t)n, ", but its pipes would not open: 0x%08X, 0x%08X",
                     (unsigned)to, (unsigned)from);
        if (to >= 0) vitaplexCloseMsgPipe(to);
        if (from >= 0) vitaplexCloseMsgPipe(from);
        unlock();
        return to < 0 ? to : from;
    }
    g_to = to;
    g_from = from;
    unlock();
    return 0;
}

// Under g_lock: g_message, as filled in, to the plugin.
static int sendLocked(void) {
    if (g_to < 0) return -1;
    unsigned int timeout = SEND_TIMEOUT_US;
    return sceKernelSendMsgPipe(g_to, &g_message, sizeof(g_message), MSG_PIPE_MODE_FULL, NULL,
                                &timeout);
}

static int sendCommandLocked(uint32_t cmd) {
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = cmd;
    return sendLocked();
}

int qmActivate(void) {
    lock();
    int rc = sendCommandLocked(CMD_APP_START);
    if (rc >= 0) rc = sendCommandLocked(CMD_ACTIVATE);
    unlock();
    return rc;
}

int qmDeactivate(void) {
    lock();
    const int rc = sendCommandLocked(CMD_DEACTIVATE);
    unlock();
    return rc;
}

// UTF-8 to the UTF-16 the plugin shows, ending in 0, cut to fit; what is not
// UTF-8 becomes U+FFFD.
static void toUtf16(const char* s, uint16_t* out, size_t cap) {
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    size_t n = 0;
    while (*p && n + 1 < cap) {
        unsigned c = *p++;
        int more = 0;
        if (c >= 0xF0 && c <= 0xF4) {
            c &= 0x07;
            more = 3;
        } else if (c >= 0xE0 && c <= 0xEF) {
            c &= 0x0F;
            more = 2;
        } else if (c >= 0xC2 && c <= 0xDF) {
            c &= 0x1F;
            more = 1;
        } else if (c >= 0x80) {
            c = 0xFFFD;
        }
        for (; more > 0; more--) {
            if ((*p & 0xC0) != 0x80) {
                c = 0xFFFD;
                break;
            }
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (c >> 10));
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    out[n] = 0;
}

int qmSetSong(const char* title, const char* artist, const char* album) {
    lock();
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = CMD_INFO;
    g_message.flags = FLAG_REFRESH_TEXT;
    toUtf16(title, g_message.title, sizeof(g_message.title) / sizeof(g_message.title[0]));
    toUtf16(artist, g_message.artist, sizeof(g_message.artist) / sizeof(g_message.artist[0]));
    toUtf16(album, g_message.album, sizeof(g_message.album) / sizeof(g_message.album[0]));
    const int rc = sendLocked();
    unlock();
    return rc;
}

int qmPoll(void) {
    lock();
    int press = QM_NONE;
    if (g_from >= 0) {
        memset(&g_press, 0, sizeof(g_press));
        if (sceKernelTryReceiveMsgPipe(g_from, &g_press, sizeof(g_press), MSG_PIPE_MODE_FULL,
                                       NULL) >= 0) {
            press = g_press.cmd == CMD_PLAY ? QM_PLAY_PAUSE
                  : g_press.cmd == CMD_FF   ? QM_NEXT
                  : g_press.cmd == CMD_REW  ? QM_PREVIOUS
                                            : QM_NONE;
        }
    }
    unlock();
    return press;
}
