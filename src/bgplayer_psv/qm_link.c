/**
 * The quick menu's Music row, through VitaPlex's own plugin or ElevenMPV-A's
 * (qm_link.h).
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

// ElevenMPV-A's plugin's two pipes, to it and from it, and its messages, as
// its ipc.h has them. One goes to the plugin whole every time, whatever it
// carries, since the plugin takes them a whole one at a time.
#define EMPVA_PIPE_TO_PLUGIN   "ElevenMPVA::ShellIPC_RX"
#define EMPVA_PIPE_FROM_PLUGIN "ElevenMPVA::ShellIPC_TX"
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

_Static_assert(sizeof(QmMessage) == 1544, "qm_protocol.h's message, as the plugin takes it");
_Static_assert(sizeof(QmPress) == 8, "qm_protocol.h's press, as the plugin sends it");

// A message pipe moves a whole message or none of it with this mode, and
// without it as much as fits (Vita3K's kernel/types.h:
// SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
#define MSG_PIPE_MODE_FULL 0x00000001
// A plugin takes each message the moment it comes, so one that cannot go in
// this long finds no plugin taking it, and is dropped.
#define SEND_TIMEOUT_US (200 * 1000)

static SceUID g_lock = -1;
static SceUID g_to = -1, g_from = -1;
static int g_own;   // the pipes are VitaPlex's plugin's
// Under g_lock: 1.5 KB, off the callers' stacks.
static union {
    ToPlugin empva;
    QmMessage own;
} g_message;
static union {
    FromPlugin empva;
    QmPress own;
} g_press;

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

int qmIsOwn(void) {
    lock();
    const int own = g_to >= 0 && g_from >= 0 && g_own;
    unlock();
    return own;
}

// Loads a plugin into SceShell, as ElevenMPV-A does its own.
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

// Under g_lock: opens a plugin's two pipes, loading it from path first when
// they are not there. 0 when open, else the code that stopped it; what
// happened in what, from n on, which it moves on.
static int openPlugin(const char* name, const char* toPipe, const char* fromPipe,
                      const char* path, char* what, size_t size, size_t* n) {
#define SAY(...)                                                                  \
    do {                                                                          \
        if (*n < size) {                                                          \
            const int w = snprintf(what + *n, size - *n, __VA_ARGS__);            \
            if (w > 0) *n = (size_t)w < size - *n ? *n + (size_t)w : size;       \
        }                                                                         \
    } while (0)
    SceUID to = vitaplexOpenMsgPipe(toPipe);
    if (to >= 0) {
        SAY("%s was in SceShell already", name);
    } else {
        SceIoStat st;
        const int stat = sceIoGetstat(path, &st);
        if (stat < 0) {
            SAY("%s is not at %s", name, path);
            return stat;
        }
        const SceUID module = loadIntoShell(path);
        if (module < 0) {
            SAY("loading %s (%s) into SceShell: 0x%08X", name, path, (unsigned)module);
            return module;
        }
        to = vitaplexOpenMsgPipe(toPipe);
        SAY("loaded %s (%s) into SceShell (module 0x%08X)", name, path, (unsigned)module);
    }
    const SceUID from = vitaplexOpenMsgPipe(fromPipe);
    if (to < 0 || from < 0) {
        SAY(", but its pipes would not open: 0x%08X, 0x%08X", (unsigned)to, (unsigned)from);
        if (to >= 0) vitaplexCloseMsgPipe(to);
        if (from >= 0) vitaplexCloseMsgPipe(from);
        return to < 0 ? to : from;
    }
    g_to = to;
    g_from = from;
    return 0;
#undef SAY
}

int qmOpen(char* what, size_t size) {
    lock();
    if (g_to >= 0 && g_from >= 0) {
        snprintf(what, size, "%s plugin is open already", g_own ? "VitaPlex's" : "ElevenMPV-A's");
        unlock();
        return 0;
    }
    size_t n = 0;
    what[0] = 0;
    int rc = openPlugin("VitaPlex's plugin", QM_PIPE_TO_PLUGIN, QM_PIPE_FROM_PLUGIN,
                        QM_OWN_PLUGIN_PATH, what, size, &n);
    if (rc == 0) {
        g_own = 1;
        // ElevenMPV-A's plugin, if VitaPlex had it on earlier this boot, would
        // put its own callbacks on the row's buttons each time the quick menu
        // opens: asked once, without waiting, to give the row back.
        const SceUID empva = vitaplexOpenMsgPipe(EMPVA_PIPE_TO_PLUGIN);
        if (empva >= 0) {
            memset(&g_message, 0, sizeof(g_message));
            g_message.empva.cmd = CMD_DEACTIVATE;
            const int sent = sceKernelTrySendMsgPipe(empva, &g_message, sizeof(g_message.empva),
                                                     MSG_PIPE_MODE_FULL, NULL);
            vitaplexCloseMsgPipe(empva);
            if (n < size)
                snprintf(what + n, size - n, "; ElevenMPV-A's plugin is in SceShell too, "
                         "asked to give the row back: 0x%08X", (unsigned)sent);
        }
        unlock();
        return 0;
    }
    // ElevenMPV-A's, from the first of its copies there is.
    if (n < size) {
        const int w = snprintf(what + n, size - n, "; then ");
        if (w > 0) n = (size_t)w < size - n ? n + (size_t)w : size;
    }
    SceIoStat st;
    const char* path = QM_PLUGIN_PATH;
    const char* name = "ElevenMPV-A's plugin";
    const SceUID probe = vitaplexOpenMsgPipe(EMPVA_PIPE_TO_PLUGIN);
    if (probe >= 0) {
        vitaplexCloseMsgPipe(probe);
    } else if (sceIoGetstat(path, &st) >= 0) {
        name = "VitaPlex BG's copy of ElevenMPV-A's plugin";
    } else {
        path = QM_PLUGIN_PATH_EMPVA;
        name = "ElevenMPV-A's own plugin";
    }
    rc = openPlugin(name, EMPVA_PIPE_TO_PLUGIN, EMPVA_PIPE_FROM_PLUGIN, path, what, size, &n);
    if (rc == 0) g_own = 0;
    unlock();
    return rc;
}

// Under g_lock: g_message, as filled in, to the plugin.
static int sendLocked(void) {
    if (g_to < 0) return -1;
    unsigned int timeout = SEND_TIMEOUT_US;
    const SceSize size = g_own ? sizeof(g_message.own) : sizeof(g_message.empva);
    return sceKernelSendMsgPipe(g_to, &g_message, size, MSG_PIPE_MODE_FULL, NULL, &timeout);
}

// Under g_lock: a command with no text.
static int sendCommandLocked(uint32_t cmd, uint32_t arg) {
    memset(&g_message, 0, sizeof(g_message));
    if (g_own) {
        g_message.own.cmd = cmd;
        g_message.own.arg = arg;
    } else {
        g_message.empva.cmd = cmd;
    }
    return sendLocked();
}

int qmActivate(int owner) {
    lock();
    int rc;
    if (g_own) {
        rc = sendCommandLocked(QM_CMD_ACTIVATE, (uint32_t)owner);
    } else {
        // Its watching thread started (an app that stops sends the plugin a
        // stop), then the row handed to this process.
        rc = sendCommandLocked(CMD_APP_START, 0);
        if (rc >= 0) rc = sendCommandLocked(CMD_ACTIVATE, 0);
    }
    unlock();
    return rc;
}

int qmHeartbeat(int owner) {
    lock();
    int rc = 0;
    if (g_own && g_to >= 0) {
        memset(&g_message.own, 0, sizeof(g_message.own));
        g_message.own.cmd = QM_CMD_ACTIVATE;
        g_message.own.arg = (uint32_t)owner;
        rc = sceKernelTrySendMsgPipe(g_to, &g_message, sizeof(g_message.own), MSG_PIPE_MODE_FULL,
                                     NULL);
    }
    unlock();
    return rc;
}

int qmDeactivate(void) {
    lock();
    const int rc = sendCommandLocked(g_own ? QM_CMD_DEACTIVATE : CMD_DEACTIVATE, 0);
    unlock();
    return rc;
}

int qmSetPlaying(int playing) {
    lock();
    const int rc = g_own ? sendCommandLocked(QM_CMD_PLAYING, playing ? 1 : 0) : 0;
    unlock();
    return rc;
}

int qmSetLook(int look) {
    lock();
    const int rc = g_own ? sendCommandLocked(QM_CMD_LOOK, (uint32_t)look) : 0;
    unlock();
    return rc;
}

int qmSetArt(uint32_t serial) {
    lock();
    const int rc = g_own ? sendCommandLocked(QM_CMD_ART, serial) : 0;
    unlock();
    return rc;
}

// UTF-8 to the UTF-16 the plugins show, ending in 0, cut to fit; what is not
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
    uint16_t *t, *ar, *al;
    if (g_own) {
        g_message.own.cmd = QM_CMD_SONG;
        t = g_message.own.title;
        ar = g_message.own.artist;
        al = g_message.own.album;
    } else {
        g_message.empva.cmd = CMD_INFO;
        g_message.empva.flags = FLAG_REFRESH_TEXT;
        t = g_message.empva.title;
        ar = g_message.empva.artist;
        al = g_message.empva.album;
    }
    toUtf16(title, t, 256);
    toUtf16(artist, ar, 256);
    toUtf16(album, al, 256);
    const int rc = sendLocked();
    unlock();
    return rc;
}

int qmPoll(void) {
    lock();
    int press = QM_NONE;
    if (g_from >= 0) {
        memset(&g_press, 0, sizeof(g_press));
        if (g_own) {
            if (sceKernelTryReceiveMsgPipe(g_from, &g_press, sizeof(g_press.own),
                                           MSG_PIPE_MODE_FULL, NULL) >= 0) {
                press = g_press.own.press == QM_PRESS_PLAY_PAUSE ? QM_PLAY_PAUSE
                      : g_press.own.press == QM_PRESS_NEXT       ? QM_NEXT
                      : g_press.own.press == QM_PRESS_PREVIOUS   ? QM_PREVIOUS
                                                                 : QM_NONE;
            }
        } else if (sceKernelTryReceiveMsgPipe(g_from, &g_press, sizeof(g_press.empva),
                                              MSG_PIPE_MODE_FULL, NULL) >= 0) {
            press = g_press.empva.cmd == CMD_PLAY ? QM_PLAY_PAUSE
                  : g_press.empva.cmd == CMD_FF   ? QM_NEXT
                  : g_press.empva.cmd == CMD_REW  ? QM_PREVIOUS
                                                  : QM_NONE;
        }
    }
    unlock();
    return press;
}
