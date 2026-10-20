/**
 * VitaPlex's quick menu plugin: the song, and buttons that work, in the Music
 * row of the quick menu (hold PS), for VitaPlex BG and its background player.
 *
 * The row is SceShell's, and only code inside SceShell can change it. VitaPlex
 * BG loads this module into SceShell with taiHEN (qm_link.c) the first time
 * it wants the row in a boot, and it stays until the Vita restarts. The two
 * talk through the message pipes qm_protocol.h lays out.
 *
 * It does what ElevenMPV-A's shell plugin does (GrapheneCt/ElevenMPV-A,
 * GPL-3.0, third_party/elevenmpva-shell-plugin), in its way: it finds the
 * quick menu through power_manage_plugin, the row's buttons and two lines of
 * text by their id hashes, and while the quick menu is open puts the song on
 * the lines and its own callbacks on the buttons. It differs where that
 * plugin fell short (docs/DESIGN_NOTES.md):
 *
 * - The buttons stayed dimmed with ElevenMPV-A's ButtonBase::Enable(false),
 *   though presses got through. The way they are switched on can be chosen
 *   (QM_LOOK_*), and is logged with the button's state before and after.
 * - A press is sent without waiting, so a process that stopped reading cannot
 *   hold SceShell up, and the row is given back by itself when no process has
 *   asked for it again in QM_ACTIVE_TIMEOUT_S.
 * - Once a boot it logs the row's widgets, for the artwork and the play and
 *   pause icon still to come.
 *
 * Built with vitasdk against the PAF headers scripts/vita/fetch_paf_headers.sh
 * fetches; its PAF imports are in paf_stubs.S. Logs to
 * ux0:data/VitaPlex/qm-plugin.log, one file a boot.
 */

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include <stdarg.h>
#include <stdint.h>

#include <paf.h>

#include "qm_protocol.h"

// ── taiHEN, by NID (taihen_stub.S) ──────────────────────────────────────────
// As taihen.h has them.
typedef uintptr_t TaiHookRef;
typedef struct TaiHookArgs {
    size_t size;
    const char* module;
    uint32_t library_nid;
    uint32_t func_nid;
    const void* hook_func;
} TaiHookArgs;
typedef struct TaiHookUser {
    uintptr_t next;
    void* func;
    void* old;
} TaiHookUser;
extern "C" SceUID vitaplexQmHookFunctionImport(TaiHookRef* hook, TaiHookArgs* args);

// No C runtime here: what the compiler may call for it, from the kernel's.
// new and delete are PAF's (built with -fno-sized-deallocation, so delete is
// always the one PAF exports), as in ElevenMPV-A's plugin.
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    return sceClibMemcpy(dst, src, n);
}
extern "C" void* memset(void* dst, int c, size_t n) {
    return sceClibMemset(dst, c, n);
}
// What a pure virtual function's slot holds in the vtables of PAF's abstract
// classes this module emits; never called.
extern "C" void __cxa_pure_virtual() {
    for (;;) sceKernelDelayThread(1000000);
}

namespace {

// ── The log ─────────────────────────────────────────────────────────────────
const char* const kLogPath = "ux0:data/VitaPlex/qm-plugin.log";
const char* const kLogPrevPath = "ux0:data/VitaPlex/qm-plugin.prev.log";
const int kLogMax = 512 * 1024;

SceUID g_log = -1;
SceUID g_logLock = -1;
int g_logged;

void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char* fmt, ...) {
    if (g_log < 0 || g_logged > kLogMax) return;
    char line[320];
    SceDateTime t;
    sceClibMemset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    int n = sceClibSnprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", t.hour, t.minute,
                            t.second, t.microsecond / 1000);
    va_list ap;
    va_start(ap, fmt);
    n += sceClibVsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    sceKernelLockMutex(g_logLock, 1, NULL);
    sceIoWrite(g_log, line, n);
    sceIoSyncByFd(g_log, 0);  // a SceShell crash keeps what came before it
    g_logged += n;
    sceKernelUnlockMutex(g_logLock, 1);
}

void logOpen() {
    g_logLock = sceKernelCreateMutex("VitaPlexQM::Log", 0, 0, NULL);
    sceIoRemove(kLogPrevPath);
    sceIoRename(kLogPath, kLogPrevPath);
    g_log = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
}

// UTF-16 as ASCII for the log, anything else as '?'.
void toLog(const uint16_t* s, char* out, int cap) {
    int n = 0;
    for (; s && *s && n + 1 < cap; s++) out[n++] = (*s >= 0x20 && *s < 0x7F) ? (char)*s : '?';
    out[n] = 0;
}

// ── State ───────────────────────────────────────────────────────────────────
// The Music row's parts, by the id hashes SceShell's quick menu gives them
// (ElevenMPV-A's main.h and main.cpp).
const uint32_t kHashRew = 0x9F6A44DA;
const uint32_t kHashPlay = 0x56734CD1;
const uint32_t kHashFf = 0x6CA674D6;
const uint32_t kHashTextTop = 0x66FDAFE3;
const uint32_t kHashTextBottom = 0xF099B450;

// SceAppMgr's app state, as ElevenMPV-A's plugin asks for it (its build of
// sceAppMgrGetAppState passes 0x03570000).
const uint32_t kAppStateVersion = 0x03570000;
// Its threads' priority, the SDK's SCE_KERNEL_LOWEST_PRIORITY_USER.
const int kThreadPriority = 191;
// A message pipe moves a whole message or none of it with this mode (Vita3K's
// kernel/types.h, SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
const int kPipeModeFull = 0x00000001;

paf::ui::Widget* g_impose;  // the quick menu's root
SceUID g_toPlugin = -1, g_fromPlugin = -1;
SceUID g_lock = -1;  // everything below, and the widgets

int g_active;
uint32_t g_owner;
SceUInt64 g_activatedUs;  // the last QM_CMD_ACTIVATE
int g_look = QM_LOOK_ENABLE;
int g_playing = -1;
uint16_t g_title[256], g_artist[256], g_album[256];

// While the quick menu is open:
paf::ui::Widget *g_rew, *g_play, *g_ff, *g_textTop, *g_textBottom;
int g_applied;    // the row is ours this time it is open
int g_findTries;  // looks for the buttons this time
// Once a boot:
int g_layoutChecked, g_layoutOk;  // PAF's widget layout is its headers' (checkLayout)
int g_dumped;                     // the row's widgets logged
int g_takesLogged;                // the first takes are logged in full
int g_lookLogged = -1;            // ... and the first with each look
int g_revertsLogged;              // buttons found switched off again, logged

void lock() {
    sceKernelLockMutex(g_lock, 1, NULL);
}
void unlock() {
    sceKernelUnlockMutex(g_lock, 1);
}

const char* ownerName(uint32_t owner) {
    return owner == QM_OWNER_APP ? "VitaPlex BG" : owner == QM_OWNER_PLAYER ? "the background player"
                                                                            : "?";
}
const char* lookName(int look) {
    return look == QM_LOOK_ENABLE_NORMAL ? "Enable(true) and normal state"
         : look == QM_LOOK_AS_ELEVENMPVA ? "Enable(false), as ElevenMPV-A"
                                         : "Enable(true)";
}

// Members PAF's headers keep protected, for the log. Only read, and only
// once the layout has been checked (checkLayout).
struct WidgetPeek : paf::ui::Widget {
    static const paf::math::v4& pos(paf::ui::Widget* w) {
        return static_cast<WidgetPeek*>(w)->m_pos;
    }
    static const paf::math::v4& size(paf::ui::Widget* w) {
        return static_cast<WidgetPeek*>(w)->m_size;
    }
};
struct ButtonPeek : paf::ui::ButtonBase {
    static int state(paf::ui::ButtonBase* b) {
        return (int)static_cast<ButtonPeek*>(b)->m_btn_state;
    }
};

// A vector's x or y, rounded: vitasdk-paf-component's v4 keeps its four floats
// to itself.
int coord(const paf::math::v4& v, int i) {
    return (int)reinterpret_cast<const float*>(&v)[i];
}

bool isA(paf::ui::Widget* w, const char* type) {
    return w && type && w->IsInherit(type);
}

// ── The row ─────────────────────────────────────────────────────────────────
paf::ui::Widget* find(uint32_t hash) {
    return g_impose->FindChild(paf::IDParam(hash), 0);
}

void forgetWidgets() {
    g_rew = g_play = g_ff = g_textTop = g_textBottom = NULL;
    g_applied = 0;
    g_findTries = 0;
}

void onPress(int32_t type, paf::ui::Handler* self, paf::ui::Event* e, void* data) {
    (void)type;
    (void)self;
    (void)e;
    const uint32_t press = (uint32_t)(uintptr_t)data;
    const char* name = press == QM_PRESS_PLAY_PAUSE ? "play/pause"
                     : press == QM_PRESS_NEXT      ? "next"
                                                   : "previous";
    if (!g_active) {
        logLine("press: %s, with the row given back; dropped", name);
        return;
    }
    QmPress msg;
    msg.press = press;
    msg.reserved = 0;
    // On SceShell's own thread: logged only when it goes wrong, since each
    // line waits for the memory card. VitaPlex logs the presses it takes.
    const int rc = sceKernelTrySendMsgPipe(g_fromPlugin, &msg, sizeof(msg),
                                           kPipeModeFull, NULL);
    if (rc < 0) logLine("press: %s, not sent (0x%08X): nobody reading", name, (unsigned)rc);
}

// A button's switched-on bit and state (ButtonBase::ST_BTN_*: 0 normal,
// 1 dimmed), or -1 for both while the layout is not known to be the headers'.
int buttonEnabled(paf::ui::ButtonBase* b) {
    return g_layoutOk ? (int)b->IsEnable() : -1;
}
int buttonState(paf::ui::ButtonBase* b) {
    return g_layoutOk ? ButtonPeek::state(b) : -1;
}

// Switches a button on the way g_look says, logged before and after if asked.
void applyLook(paf::ui::Widget* w, const char* name, int logIt) {
    paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(w);
    const int enabledBefore = buttonEnabled(b);
    const int stateBefore = buttonState(b);
    switch (g_look) {
    case QM_LOOK_AS_ELEVENMPVA:
        b->Enable(false);
        break;
    case QM_LOOK_ENABLE_NORMAL:
        b->Enable(true);
        b->ChangeButtonState(paf::ui::ButtonBase::ST_BTN_NORMAL);
        break;
    default:
        b->Enable(true);
        break;
    }
    if (logIt)
        logLine("  %s: enabled %d, state %d; now enabled %d, state %d", name, enabledBefore,
                stateBefore, buttonEnabled(b), buttonState(b));
}

void applyLookToAll(int logIt) {
    applyLook(g_rew, "previous", logIt);
    applyLook(g_play, "play/pause", logIt);
    applyLook(g_ff, "next", logIt);
}

// While the row is ours and the quick menu open, ten times a second: a button
// SceShell has switched off again since, as its own set-up of the row may do
// after ours, is switched on again, and the first few times logged.
void watchButtons() {
    if (!g_layoutOk || g_look == QM_LOOK_AS_ELEVENMPVA) return;
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    const char* const names[3] = {"previous", "play/pause", "next"};
    for (int i = 0; i < 3; i++) {
        paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(buttons[i]);
        // State 1 is dimmed; 2 to 4 are a press under way, left alone.
        if (buttonEnabled(b) == 1 && (g_look != QM_LOOK_ENABLE_NORMAL || buttonState(b) != 1))
            continue;
        const int logIt = g_revertsLogged < 6;
        if (logIt) {
            g_revertsLogged++;
            logLine("row: %s found switched off again (enabled %d, state %d)", names[i],
                    buttonEnabled(b), buttonState(b));
        }
        applyLook(buttons[i], names[i], logIt);
    }
}

void setTexts() {
    if (!g_textTop || !g_textBottom) return;
    paf::wstring top((const wchar_t*)g_title);
    paf::wstring bottom((const wchar_t*)g_artist);
    if (g_artist[0] && g_album[0]) bottom += L" / ";
    bottom += (const wchar_t*)g_album;
    const paf::math::v4 white(1.0f, 1.0f, 1.0f, 1.0f);
    g_textTop->SetString(top);
    g_textTop->SetColor_core(white);
    g_textBottom->SetString(bottom);
    g_textBottom->SetColor_core(white);
}

void logWidget(const char* where, int depth, paf::ui::Widget* w) {
    char indent[16];
    int n = 0;
    for (; n < depth && n < (int)sizeof(indent) - 1; n++) indent[n] = ' ';
    indent[n] = 0;
    const paf::math::v4& p = WidgetPeek::pos(w);
    const paf::math::v4& s = WidgetPeek::size(w);
    char extra[96];
    extra[0] = 0;
    if (isA(w, paf::ui::ButtonBase::TypeName())) {
        paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(w);
        sceClibSnprintf(extra, sizeof(extra), ", button enabled %d state %d", buttonEnabled(b),
                        buttonState(b));
    } else if (isA(w, paf::ui::Text::TypeName())) {
        paf::wstring str;
        w->GetString(str);
        char text[48];
        toLog((const uint16_t*)str.c_str(), text, sizeof(text));
        sceClibSnprintf(extra, sizeof(extra), ", text \"%s\"", text);
    }
    logLine("%s %s%s 0x%08X at %p: pos %d,%d size %dx%d color %08X%s%s", where, indent,
            w->GetTypeName(), (unsigned)w->GetName().GetIDHash(), (void*)w, coord(p, 0),
            coord(p, 1), coord(s, 0), coord(s, 1), (unsigned)w->GetColor32(),
            w->IsActivated() ? "" : " (not active)", extra);
}

void dumpTree(paf::ui::Widget* w, int depth, int* count) {
    if (!w || *count >= 200) return;
    ++*count;
    logWidget("row", depth, w);
    if (depth >= 7) return;
    const int n = w->GetChildrenNum();
    if (n < 0 || n > 64) {
        logLine("row (%d children at depth %d? not followed)", n, depth + 1);
        return;
    }
    for (int i = 0; i < n; i++) dumpTree(w->GetChild(i), depth + 1, count);
}

// Once a boot: whether PAF's widget layout is the one its headers have, as
// far as the members read here go. The play button must report the id it
// was found by, and be among its parent's children.
void checkLayout() {
    g_layoutChecked = 1;
    const uint32_t id = g_play->GetName().GetIDHash();
    paf::ui::Widget* parent = id == kHashPlay ? g_play->GetParent() : NULL;
    const int siblings = parent ? parent->GetChildrenNum() : -1;
    int found = 0;
    if (siblings > 0 && siblings <= 64)
        for (int i = 0; i < siblings && !found; i++) found = parent->GetChild(i) == g_play;
    g_layoutOk = found;
    if (found)
        logLine("row: widget layout as the headers have it (play button among %d children)",
                siblings);
    else
        logLine("row: the play button reports id 0x%08X and its parent %p has %d children: "
                "the widget layout is not the headers', so nothing is read from it",
                (unsigned)id, (void*)parent, siblings);
}

// Once a boot, the layout checked: the Music row's widgets, for what is still
// to come.
void dumpRow() {
    g_dumped = 1;
    paf::ui::Widget* w = g_play;
    for (int up = 0; w && up < 16; up++, w = w->GetParent()) {
        logWidget("up", 0, w);
        if (w == g_impose) break;
    }
    paf::ui::Widget* top = g_play;
    for (int i = 0; i < 3 && top->GetParent() && top != g_impose; i++) top = top->GetParent();
    int count = 0;
    dumpTree(top, 0, &count);
    logLine("row: %d widgets logged", count);
}

// Takes the row, the quick menu being open: under g_lock.
void takeRow() {
    g_rew = find(kHashRew);
    g_play = find(kHashPlay);
    g_ff = find(kHashFf);
    if (!g_rew || !g_play || !g_ff) {
        // They come a moment after the quick menu (ElevenMPV-A waits for them).
        if (++g_findTries == 30) logLine("row: its buttons not found in 3 s");
        return;
    }
    g_textTop = find(kHashTextTop);
    g_textBottom = find(kHashTextBottom);
    if (!g_layoutChecked) checkLayout();
    const int logIt = g_takesLogged < 3 || g_lookLogged != g_look;
    if (logIt) {
        g_takesLogged++;
        g_lookLogged = g_look;
        logLine("row: taking it after %d ms, buttons switched on with %s", g_findTries * 100,
                lookName(g_look));
    }
    applyLookToAll(logIt);
    g_rew->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                            (void*)(uintptr_t)QM_PRESS_PREVIOUS);
    g_play->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                             (void*)(uintptr_t)QM_PRESS_PLAY_PAUSE);
    g_ff->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                           (void*)(uintptr_t)QM_PRESS_NEXT);
    setTexts();
    g_applied = 1;
    if (logIt)
        logLine("row: taken for %s%s", ownerName(g_owner),
                g_textTop ? "" : " (no text lines found)");
    if (!g_dumped && g_layoutOk) dumpRow();
}

// ── Threads ─────────────────────────────────────────────────────────────────
// Watches the quick menu open and close, as ElevenMPV-A's plugin does, and
// takes the row whenever it is open and wanted.
class WatchThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        int wasOpen = 0;
        while (!IsCanceled()) {
            SceAppMgrAppState st;
            sceClibMemset(&st, 0, sizeof(st));
            _sceAppMgrGetAppState(&st, sizeof(st), kAppStateVersion);
            const int open = st.isSystemUiOverlaid != 0;
            lock();
            if (open != wasOpen) {
                if (!open) forgetWidgets();
                if (g_active) logLine("quick menu %s", open ? "opened" : "closed");
            }
            if (open && g_active && !g_applied) takeRow();
            else if (open && g_active && g_applied) watchButtons();
            if (g_active && sceKernelGetSystemTimeWide() - g_activatedUs >
                                QM_ACTIVE_TIMEOUT_S * 1000000ULL) {
                g_active = 0;
                logLine("nothing from %s for %d s: row given back", ownerName(g_owner),
                        QM_ACTIVE_TIMEOUT_S);
            }
            unlock();
            wasOpen = open;
            paf::thread::Sleep(100);
        }
    }
};

QmMessage g_message;  // the receiving thread's alone

// Takes what VitaPlex sends.
class ReceiveThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            const int rc = sceKernelReceiveMsgPipe(g_toPlugin, &g_message, sizeof(g_message),
                                                   kPipeModeFull, NULL, NULL);
            if (rc < 0) {
                logLine("receive: 0x%08X", (unsigned)rc);
                paf::thread::Sleep(1000);
                continue;
            }
            lock();
            handle(g_message);
            unlock();
        }
    }

private:
    static void handle(const QmMessage& m) {
        switch (m.cmd) {
        case QM_CMD_ACTIVATE:
            if (!g_active || g_owner != m.arg)
                logLine("row wanted by %s", ownerName(m.arg));
            g_active = 1;
            g_owner = m.arg;
            g_activatedUs = sceKernelGetSystemTimeWide();
            break;
        case QM_CMD_DEACTIVATE:
            if (g_active) logLine("row given back by %s", ownerName(g_owner));
            g_active = 0;
            break;
        case QM_CMD_SONG: {
            sceClibMemcpy(g_title, m.title, sizeof(g_title));
            sceClibMemcpy(g_artist, m.artist, sizeof(g_artist));
            sceClibMemcpy(g_album, m.album, sizeof(g_album));
            g_title[255] = g_artist[255] = g_album[255] = 0;
            char title[64], artist[64];
            toLog(g_title, title, sizeof(title));
            toLog(g_artist, artist, sizeof(artist));
            logLine("song: \"%s\" by %s", title, artist);
            if (g_applied && g_active) setTexts();
            break;
        }
        case QM_CMD_PLAYING:
            if (g_playing != (int)m.arg) logLine("%s", m.arg ? "playing" : "paused");
            g_playing = (int)m.arg;
            break;
        case QM_CMD_LOOK:
            if (m.arg < QM_LOOK_COUNT && g_look != (int)m.arg) {
                g_look = (int)m.arg;
                logLine("buttons to be switched on with %s", lookName(g_look));
                if (g_applied && g_active) {
                    g_lookLogged = g_look;
                    applyLookToAll(1);
                }
            }
            break;
        default:
            logLine("unknown command %u", (unsigned)m.cmd);
            break;
        }
    }
};

// ── SceShell's view of the BGM port ─────────────────────────────────────────
// As ElevenMPV-A's plugin does: SceShell reads the port's priority as no more
// than 0x80 (sceAppMgrGetCurrentBgmState2, SceAppMgrUser 0x62BEBD65), what an
// app gets while SceShell decodes for it. VitaPlex's own player takes 0x81.
typedef struct BgmState {
    int ownerId;
    int priority;
    int status1;
    int state;
    int status2;
} BgmState;

TaiHookRef g_bgmHookRef;

int bgmStateCapped(BgmState* state) {
    const TaiHookUser* cur = (const TaiHookUser*)g_bgmHookRef;
    const TaiHookUser* next = (const TaiHookUser*)cur->next;
    typedef int (*Fn)(BgmState*);
    const int ret = next ? ((Fn)next->func)(state) : ((Fn)cur->old)(state);
    if (state->priority > 0x80) state->priority = 0x80;
    return ret;
}

}  // namespace

extern "C" int module_start(SceSize args, const void* argp) {
    (void)args;
    (void)argp;
    logOpen();
    logLine("start: VitaPlex's quick menu plugin");

    // The quick menu's root, as ElevenMPV-A's plugin finds it: power_manage_
    // plugin's interface has a function giving it at 0x54.
    paf::Plugin* power = paf::Plugin::Find("power_manage_plugin");
    void* iface = power ? power->GetInterface(1) : NULL;
    typedef paf::ui::Widget* (*GetImposeRoot)();
    GetImposeRoot getRoot = iface ? *(GetImposeRoot*)((char*)iface + 0x54) : NULL;
    g_impose = getRoot ? getRoot() : NULL;
    if (!g_impose) {
        logLine("the quick menu not found (power_manage_plugin %p, interface %p): not staying",
                (void*)power, iface);
        return SCE_KERNEL_START_NO_RESIDENT;
    }
    logLine("quick menu: %s at %p", g_impose->GetTypeName(), (void*)g_impose);

    g_lock = sceKernelCreateMutex("VitaPlexQM::Lock", 0, 0, NULL);
    // As ElevenMPV-A's pipes are made: 0x40, user main memory; 0x81, whole
    // messages and openable by name.
    g_toPlugin = sceKernelCreateMsgPipe(QM_PIPE_TO_PLUGIN, 0x40, 0x81,
                                        sizeof(QmMessage) * QM_TO_PLUGIN_SLOTS, NULL);
    g_fromPlugin = sceKernelCreateMsgPipe(QM_PIPE_FROM_PLUGIN, 0x40, 0x81,
                                          sizeof(QmPress) * QM_FROM_PLUGIN_SLOTS, NULL);
    if (g_lock < 0 || g_toPlugin < 0 || g_fromPlugin < 0) {
        logLine("pipes: 0x%08X, 0x%08X, lock 0x%08X: not staying", (unsigned)g_toPlugin,
                (unsigned)g_fromPlugin, (unsigned)g_lock);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    TaiHookArgs hook;
    hook.size = sizeof(hook);
    hook.module = "SceShell";
    hook.library_nid = 0xA6605D6F;
    hook.func_nid = 0x62BEBD65;
    hook.hook_func = (const void*)bgmStateCapped;
    const SceUID hooked = vitaplexQmHookFunctionImport(&g_bgmHookRef, &hook);
    logLine("BGM priority SceShell reads capped at 0x80: 0x%08X", (unsigned)hooked);

    (new WatchThread(kThreadPriority, 0x4000, "VitaPlexQM::Watch"))->Start();
    (new ReceiveThread(kThreadPriority, 0x4000, "VitaPlexQM::Receive"))->Start();
    logLine("ready");
    return SCE_KERNEL_START_SUCCESS;
}

// It stays: its threads and SceShell's widgets hold on to it.
extern "C" int module_stop(SceSize args, const void* argp) {
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_CANCEL;
}
