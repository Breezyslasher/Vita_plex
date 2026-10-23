/**
 * The quick menu's Music row, and the music after VitaPlex has gone, through
 * VitaPlex's quick menu plugin (qm_link.h).
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

#define SHELL_TITLEID "NPXS19999"

_Static_assert(sizeof(QmMessage) == 1544, "qm_protocol.h's message, as the plugin takes it");
_Static_assert(sizeof(QmPress) == 8, "qm_protocol.h's press, as the plugin sends it");

// A message pipe moves a whole message or none of it with this mode, and
// without it as much as fits (Vita3K's kernel/types.h:
// SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
#define MSG_PIPE_MODE_FULL 0x00000001
// The plugin takes each message the moment it comes, so one that cannot go in
// this long finds no plugin taking it, and is dropped.
#define SEND_TIMEOUT_US (200 * 1000)

static SceUID g_lock = -1;
static SceUID g_to = -1, g_from = -1;
// Under g_lock: 1.5 KB, off the callers' stacks.
static QmMessage g_message;
static QmPress g_press;

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

// Loads the plugin into SceShell, as ElevenMPV-A does its own.
static SceUID loadIntoShell(const char* path) {
    SceUID shell = -1;
    const int rc = sceAppMgrGetIdByName(&shell, SHELL_TITLEID);
    if (rc < 0) return rc;
    TaiModuleArgs args;
    memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.pid = shell;
    return vitaplexTaiLoadStartModuleForPidForUser(path, &args);
}

int qmOpen(char* what, size_t size) {
    lock();
    if (g_to >= 0 && g_from >= 0) {
        snprintf(what, size, "VitaPlex's plugin is open already");
        unlock();
        return 0;
    }
    SceUID to = vitaplexOpenMsgPipe(QM_PIPE_TO_PLUGIN);
    int n = 0;
    if (to >= 0) {
        n = snprintf(what, size, "VitaPlex's plugin was in SceShell already");
    } else {
        SceIoStat st;
        const int stat = sceIoGetstat(QM_OWN_PLUGIN_PATH, &st);
        if (stat < 0) {
            snprintf(what, size, "VitaPlex's plugin is not at %s", QM_OWN_PLUGIN_PATH);
            unlock();
            return stat;
        }
        const SceUID module = loadIntoShell(QM_OWN_PLUGIN_PATH);
        if (module < 0) {
            snprintf(what, size, "loading VitaPlex's plugin (%s) into SceShell: 0x%08X",
                     QM_OWN_PLUGIN_PATH, (unsigned)module);
            unlock();
            return module;
        }
        to = vitaplexOpenMsgPipe(QM_PIPE_TO_PLUGIN);
        n = snprintf(what, size, "loaded VitaPlex's plugin (%s) into SceShell (module 0x%08X)",
                     QM_OWN_PLUGIN_PATH, (unsigned)module);
    }
    const SceUID from = vitaplexOpenMsgPipe(QM_PIPE_FROM_PLUGIN);
    if (to < 0 || from < 0) {
        if (n >= 0 && (size_t)n < size)
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

// Under g_lock: a command with no text.
static int sendCommandLocked(uint32_t cmd, uint32_t arg) {
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = cmd;
    g_message.arg = arg;
    return sendLocked();
}

int qmActivate(int owner) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_ACTIVATE, (uint32_t)owner);
    unlock();
    return rc;
}

int qmHeartbeat(int owner) {
    lock();
    int rc = 0;
    if (g_to >= 0) {
        memset(&g_message, 0, sizeof(g_message));
        g_message.cmd = QM_CMD_ACTIVATE;
        g_message.arg = (uint32_t)owner;
        rc = sceKernelTrySendMsgPipe(g_to, &g_message, sizeof(g_message), MSG_PIPE_MODE_FULL,
                                     NULL);
    }
    unlock();
    return rc;
}

int qmDeactivate(void) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_DEACTIVATE, 0);
    unlock();
    return rc;
}

int qmSetPlaying(int playing) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_PLAYING, playing ? 1 : 0);
    unlock();
    return rc;
}

int qmSetArt(uint32_t serial) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_ART, serial);
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
    g_message.cmd = QM_CMD_SONG;
    toUtf16(title, g_message.title, 256);
    toUtf16(artist, g_message.artist, 256);
    toUtf16(album, g_message.album, 256);
    const int rc = sendLocked();
    unlock();
    return rc;
}

int qmPlayQueue(void) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_PLAY_QUEUE, 0);
    unlock();
    return rc;
}

// Under g_lock: g_message, as filled in, to the plugin, whether or not this
// process has opened it. Only to a plugin that is there: one this would load
// has carried nothing on.
static int sendToPresentLocked(void) {
    if (g_to >= 0) return sendLocked();
    const SceUID to = vitaplexOpenMsgPipe(QM_PIPE_TO_PLUGIN);
    int rc = to;
    if (to >= 0) {
        unsigned int timeout = SEND_TIMEOUT_US;
        rc = sceKernelSendMsgPipe(to, &g_message, sizeof(g_message), MSG_PIPE_MODE_FULL, NULL,
                                  &timeout);
        vitaplexCloseMsgPipe(to);
    }
    return rc;
}

int qmTakeBack(void) {
    lock();
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = QM_CMD_TAKE_BACK;
    const int rc = sendToPresentLocked();
    unlock();
    return rc;
}

int qmInstallUpdate(void) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_INSTALL_UPDATE, 0);
    unlock();
    return rc;
}

int qmCarryDownloads(void) {
    lock();
    const int rc = sendCommandLocked(QM_CMD_DOWNLOADS, 0);
    unlock();
    return rc;
}

int qmTakeDownloads(void) {
    lock();
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = QM_CMD_TAKE_DOWNLOADS;
    const int rc = sendToPresentLocked();
    unlock();
    return rc;
}

// Under g_lock: a command with two lines of text.
static int sendLinesLocked(uint32_t cmd, uint32_t arg, const char* first, const char* second) {
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = cmd;
    g_message.arg = arg;
    toUtf16(first, g_message.title, 256);
    toUtf16(second, g_message.artist, 256);
    return sendLocked();
}

int qmDownloadProgress(const char* title, const char* detail, uint32_t permille) {
    lock();
    const int rc = sendLinesLocked(QM_CMD_DL_PROGRESS, permille, title, detail);
    unlock();
    return rc;
}

int qmDownloadProgressEnd(const char* title, const char* detail) {
    lock();
    const int rc = sendLinesLocked(QM_CMD_DL_PROGRESS_END, 0, title, detail);
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
            press = g_press.press == QM_PRESS_PLAY_PAUSE ? QM_PLAY_PAUSE
                  : g_press.press == QM_PRESS_NEXT       ? QM_NEXT
                  : g_press.press == QM_PRESS_PREVIOUS   ? QM_PREVIOUS
                                                         : QM_NONE;
        }
    }
    unlock();
    return press;
}
