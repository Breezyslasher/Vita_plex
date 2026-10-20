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
 *   though presses got through, and with Enable(true) as well (build 2022).
 *   Their icons are planes of their own beside them. The way they are
 *   switched on can be chosen (QM_LOOK_*); the default also undims the icons.
 * - It takes the row 200 ms after the quick menu opens, as ElevenMPV-A's
 *   plugin about does: the song this one put on the lines at once did not
 *   show (build 2022). While the quick menu stays open it puts back what
 *   SceShell changes: buttons switched off, other text, dimmed icons.
 * - A press is sent without waiting, so a process that stopped reading cannot
 *   hold SceShell up, and the row is given back by itself when no process has
 *   asked for it again in QM_ACTIVE_TIMEOUT_S.
 * - For the first opens of a boot it logs the row's widgets as the quick menu
 *   opens, then each change to them for 3 s, whether the row is wanted or
 *   not: what SceShell does to the row for its own Music app, beside what it
 *   does with VitaPlex playing.
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

#include <limits>  // before PAF's headers: their vector uses it without including it
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

// A line put together a piece at a time, as far as it fits.
struct Line {
    char buf[224];
    int n;
    Line() : n(0) {
        buf[0] = 0;
    }
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
void Line::add(const char* fmt, ...) {
    if (n >= (int)sizeof(buf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = sceClibVsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (w > 0) n = n + w < (int)sizeof(buf) - 1 ? n + w : (int)sizeof(buf) - 1;
}

// UTF-16 as ASCII for the log, anything else as '?'.
void toLog(const uint16_t* s, char* out, int cap) {
    int n = 0;
    for (; s && *s && n + 1 < cap; s++) out[n++] = (*s >= 0x20 && *s < 0x7F) ? (char)*s : '?';
    out[n] = 0;
}

// FNV-1a over UTF-16, to tell texts apart.
uint32_t hashText(const uint16_t* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ *s) * 16777619u;
    return h;
}

// An alpha as a whole per cent, for the log (its printf has no %f).
int percent(float f) {
    return (int)(f * 100.0f + (f < 0.0f ? -0.5f : 0.5f));
}

bool startsWith(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (*s != *prefix) return false;
    return true;
}

// ── State ───────────────────────────────────────────────────────────────────
// The Music row's parts, by the id hashes SceShell's quick menu gives them:
// the buttons and lines as ElevenMPV-A's main.h and main.cpp have them, the
// rest as qm-plugin.log listed the row (build 2022).
const uint32_t kHashRew = 0x9F6A44DA;
const uint32_t kHashPlay = 0x56734CD1;
const uint32_t kHashFf = 0x6CA674D6;
const uint32_t kHashTextTop = 0x66FDAFE3;
const uint32_t kHashTextBottom = 0xF099B450;
// 64x64 planes in the row where the buttons are, listed just before each:
// their icons, by all appearances.
const uint32_t kHashIconRew = 0xD639AB27;
const uint32_t kHashIconPlay = 0xE057A7D0;
const uint32_t kHashIconFf = 0x6FA104CC;
// The row's heading, "Music", in a group of its own above it.
const uint32_t kHashHeading = 0x90E6251E;

// SceAppMgr's app state, as ElevenMPV-A's plugin asks for it (its build of
// sceAppMgrGetAppState passes 0x03570000).
const uint32_t kAppStateVersion = 0x03570000;
// Its threads' priority, the SDK's SCE_KERNEL_LOWEST_PRIORITY_USER.
const int kThreadPriority = 191;
// A message pipe moves a whole message or none of it with this mode (Vita3K's
// kernel/types.h, SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
const int kPipeModeFull = 0x00000001;
// How long after the quick menu is seen open the row is taken. ElevenMPV-A's
// plugin waits 100 ms and then looks for the buttons, and its song showed;
// the song this one put on the lines at once did not (build 2022).
const SceUInt64 kTakeAfterUs = 200 * 1000;

const paf::math::v4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);

paf::ui::Widget* g_impose;  // the quick menu's root
SceUID g_toPlugin = -1, g_fromPlugin = -1;
SceUID g_lock = -1;  // everything below, and the widgets

int g_active;
uint32_t g_owner;
SceUInt64 g_activatedUs;  // the last QM_CMD_ACTIVATE
int g_look = QM_LOOK_LIT;
int g_playing = -1;
uint16_t g_title[256], g_artist[256], g_album[256];

// While the quick menu is open:
SceUInt64 g_openedUs;  // when it was seen open
paf::ui::Widget *g_rew, *g_play, *g_ff, *g_textTop, *g_textBottom;
paf::ui::Widget *g_iconRew, *g_iconPlay, *g_iconFf;
uint32_t g_topHash, g_bottomHash;  // the lines as this plugin set them
int g_applied;                     // the row is ours this time it is open
int g_findTries;                   // looks for the buttons this time
// Once a boot:
int g_layoutChecked, g_layoutOk;  // PAF's widget layout is its headers' (checkLayout)
int g_opens;                      // times the quick menu opened
int g_takesLogged;                // the first takes are logged in full
int g_lookLogged = -1;            // ... and the first with each look
int g_revertsLogged;              // buttons found switched off again, logged
int g_textsLogged;                // lines found changed, logged
int g_dimsLogged;                 // icons and buttons found dimmed, logged

void lock() {
    sceKernelLockMutex(g_lock, 1, NULL);
}
void unlock() {
    sceKernelUnlockMutex(g_lock, 1);
}

// Milliseconds since the quick menu was seen open, for the log. In 32 bits:
// a 64-bit division would bring in libgcc's, built for another wchar_t.
int msOpen(SceUInt64 now) {
    return (int)((uint32_t)(now - g_openedUs) / 1000u);
}

const char* ownerName(uint32_t owner) {
    return owner == QM_OWNER_APP ? "VitaPlex BG" : owner == QM_OWNER_PLAYER ? "the background player"
                                                                            : "?";
}
const char* lookName(int look) {
    return look == QM_LOOK_ENABLE_NORMAL ? "Enable(true) and normal state"
         : look == QM_LOOK_AS_ELEVENMPVA ? "Enable(false), as ElevenMPV-A"
         : look == QM_LOOK_LIT           ? "Enable(true), icons undimmed"
                                         : "Enable(true)";
}

// Members PAF's headers keep protected: read for the log, and a dimmed icon's
// meta alpha written, only once the layout has been checked (checkLayout).
struct WidgetPeek : paf::ui::Widget {
    static WidgetPeek* of(paf::ui::Widget* w) {
        return static_cast<WidgetPeek*>(w);
    }
    static const paf::math::v4& size(paf::ui::Widget* w) {
        return of(w)->m_size;
    }
    static uint32_t state(paf::ui::Widget* w) {
        return of(w)->m_state;
    }
    // Its alphas times its parents': what it is drawn with.
    static float showSum(paf::ui::Widget* w) {
        return of(w)->m_show_a_sum;
    }
    static float metaSum(paf::ui::Widget* w) {
        return of(w)->m_meta_a_sum;
    }
    static int activated(paf::ui::Widget* w) {
        return of(w)->m_activated;
    }
    // Its meta alpha, and the product with its parents' that it is drawn
    // with, as Widget::SetMetaAlpha_core would set them. That export is not
    // one to count on: ScePaf-RE has it from a prototype's debug data, and a
    // NID missing on the console would keep the whole plugin from loading.
    static void setMetaAlpha(paf::ui::Widget* w, float alpha) {
        paf::ui::Widget* parent = w->GetParent();
        float parentSum = parent ? of(parent)->m_meta_a_sum : 1.0f;
        if (!(parentSum >= 0.0f && parentSum <= 1.0f)) parentSum = 1.0f;
        of(w)->m_meta_alpha = alpha;
        of(w)->m_meta_a_sum = parentSum * alpha;
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

// A colour as the RGBA bytes GetColor32 gives.
uint32_t rgba(const paf::math::v4& v) {
    const float* f = reinterpret_cast<const float*>(&v);
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) {
        const float x = f[i] < 0.0f ? 0.0f : f[i] > 1.0f ? 1.0f : f[i];
        c = (c << 8) | (uint32_t)(x * 255.0f + 0.5f);
    }
    return c;
}

bool isA(paf::ui::Widget* w, const char* type) {
    return w && type && w->IsInherit(type);
}

// A widget's texture, for the log: where it is and its size. The reference
// GetTexture adds is dropped again without ever freeing the texture from
// this thread, should SceShell have let go of it meanwhile; ref's own
// destructor, which would, is never run.
void textureOf(paf::ui::Widget* w, const void** tex, int* width, int* height) {
    union Ref {
        paf::intrusive_ptr<paf::graph::Surface> ptr;
        Ref() : ptr() {}
        ~Ref() {}
    } ref;
    w->GetTexture(ref.ptr);
    paf::graph::Surface* s = ref.ptr.get();
    *tex = s;
    *width = s ? s->GetWidth() : 0;
    *height = s ? s->GetHeight() : 0;
    if (s) s->UnsafeRelease();
}

// ── The row ─────────────────────────────────────────────────────────────────
paf::ui::Widget* find(uint32_t hash) {
    return g_impose->FindChild(paf::IDParam(hash), 0);
}

paf::ui::Widget* g_rowGroup;      // the play button's parent
paf::ui::Widget* g_headingGroup;  // the heading's
int g_timeline;                   // this open's changes are being logged
int g_snapCount;

void forgetWidgets() {
    g_rew = g_play = g_ff = g_textTop = g_textBottom = NULL;
    g_iconRew = g_iconPlay = g_iconFf = NULL;
    g_rowGroup = g_headingGroup = NULL;
    g_applied = 0;
    g_findTries = 0;
    g_timeline = 0;
    g_snapCount = 0;
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

// A button or icon at full colour, should SceShell have dimmed it by its
// colour or its meta alpha (which way, if either, is for the log to tell).
// One hidden that way is left so. true when it changed anything.
bool undim(paf::ui::Widget* w, const char* name, int logIt) {
    if (!w || !g_layoutOk) return false;
    const uint32_t color = w->GetColor32();
    const float meta = w->GetMetaAlpha();
    const bool dimColor = color != 0xFFFFFFFF && (color & 0xFF) >= 0x10;
    const bool dimMeta = meta >= 0.05f && meta < 0.99f;
    if (!dimColor && !dimMeta) return false;
    if (dimMeta) WidgetPeek::setMetaAlpha(w, 1.0f);
    // Even with its colour white already: setting it has the widget take up
    // its alphas as they now are.
    w->SetColor_core(kWhite);
    if (logIt)
        logLine("row: %s dimmed (color %08X, meta alpha %d%%): now color %08X, meta alpha %d%%",
                name, (unsigned)color, percent(meta), (unsigned)w->GetColor32(),
                percent(w->GetMetaAlpha()));
    return true;
}

bool undimAll(int logIt) {
    bool changed = undim(g_iconRew, "previous's icon", logIt);
    changed |= undim(g_iconPlay, "play/pause's icon", logIt);
    changed |= undim(g_iconFf, "next's icon", logIt);
    changed |= undim(g_rew, "previous", logIt);
    changed |= undim(g_play, "play/pause", logIt);
    changed |= undim(g_ff, "next", logIt);
    return changed;
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
    if (g_look == QM_LOOK_LIT) undimAll(logIt);
}

// While the row is ours and the quick menu open, ten times a second: a button
// SceShell has switched off again since, as its own set-up of the row may do
// after ours, is switched on again, and the first few times logged. true
// when it changed anything.
bool watchButtons() {
    if (!g_layoutOk || g_look == QM_LOOK_AS_ELEVENMPVA) return false;
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    const char* const names[3] = {"previous", "play/pause", "next"};
    bool changed = false;
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
        changed = true;
    }
    return changed;
}

void setTexts() {
    if (!g_textTop || !g_textBottom) return;
    paf::wstring top((const wchar_t*)g_title);
    paf::wstring bottom((const wchar_t*)g_artist);
    if (g_artist[0] && g_album[0]) bottom += L" / ";
    bottom += (const wchar_t*)g_album;
    g_topHash = hashText((const uint16_t*)top.c_str());
    g_bottomHash = hashText((const uint16_t*)bottom.c_str());
    g_textTop->SetString(top);
    g_textTop->SetColor_core(kWhite);
    g_textBottom->SetString(bottom);
    g_textBottom->SetColor_core(kWhite);
}

// While the row is ours and the quick menu open: the lines as set, should
// SceShell have put other text or another colour on them. true when it
// changed anything.
bool keepTexts() {
    if (!g_textTop || !g_textBottom || !g_layoutOk) return false;
    paf::wstring top, bottom;
    g_textTop->GetString(top);
    g_textBottom->GetString(bottom);
    const uint32_t topColor = g_textTop->GetColor32(), bottomColor = g_textBottom->GetColor32();
    if (hashText((const uint16_t*)top.c_str()) == g_topHash &&
        hashText((const uint16_t*)bottom.c_str()) == g_bottomHash && topColor == 0xFFFFFFFF &&
        bottomColor == 0xFFFFFFFF)
        return false;
    if (g_textsLogged < 6) {
        g_textsLogged++;
        char a[48], b[48];
        toLog((const uint16_t*)top.c_str(), a, sizeof(a));
        toLog((const uint16_t*)bottom.c_str(), b, sizeof(b));
        logLine("row: lines found changed (\"%s\", color %08X; \"%s\", color %08X): set again", a,
                (unsigned)topColor, b, (unsigned)bottomColor);
    }
    setTexts();
    return true;
}

// Once a boot: whether PAF's widget layout is the one its headers have, as
// far as the members read here go. The play button must report the id it
// was found by, and be among its parent's children.
void checkLayout(paf::ui::Widget* play) {
    g_layoutChecked = 1;
    const uint32_t id = play->GetName().GetIDHash();
    paf::ui::Widget* parent = id == kHashPlay ? play->GetParent() : NULL;
    const int siblings = parent ? parent->GetChildrenNum() : -1;
    int found = 0;
    if (siblings > 0 && siblings <= 64)
        for (int i = 0; i < siblings && !found; i++) found = parent->GetChild(i) == play;
    g_layoutOk = found;
    if (found)
        logLine("row: widget layout as the headers have it (play button among %d children)",
                siblings);
    else
        logLine("row: the play button reports id 0x%08X and its parent %p has %d children: "
                "the widget layout is not the headers', so nothing is read from it",
                (unsigned)id, (void*)parent, siblings);
}

// ── The row over time, for the log ──────────────────────────────────────────
// For the first kTimelineOpens opens of a boot: the widgets of the Music row
// and of its heading as the quick menu opens, then whatever changes in them,
// ten times a second for kTimelineUs. What this plugin changes is logged as
// it does it and not again here, so these are SceShell's changes: its set-up
// of the row, and what it puts back of what this plugin did. With the row
// SceShell's, they are the look it gives the row itself.
const int kTimelineOpens = 8;
const SceUInt64 kTimelineUs = 3000000;
const int kSnapMax = 24;

struct Snap {
    paf::ui::Widget* w;
    const char* type;
    uint32_t hash;
    uint32_t state;  // Widget::STATE_*
    uint32_t color;
    int show, showSum, meta, metaSum;  // per cent: its own, then times its parents'
    int activated;
    int width, height;
    int enabled, btnState;  // -1 if not a button
    uint32_t disableColor;  // a button's colour for dimmed, 0 for none
    const void* tex;
    int texWidth, texHeight;
    int isText;
    uint32_t textHash;
    char text[40];
};
Snap g_snaps[kSnapMax], g_next[kSnapMax];

void snapWidget(paf::ui::Widget* w, Snap* s) {
    sceClibMemset(s, 0, sizeof(*s));
    s->w = w;
    s->type = w->GetTypeName();
    s->hash = w->GetName().GetIDHash();
    s->state = WidgetPeek::state(w);
    s->color = w->GetColor32();
    s->show = percent(w->GetShowAlpha());
    s->showSum = percent(WidgetPeek::showSum(w));
    s->meta = percent(w->GetMetaAlpha());
    s->metaSum = percent(WidgetPeek::metaSum(w));
    s->activated = WidgetPeek::activated(w);
    s->width = coord(WidgetPeek::size(w), 0);
    s->height = coord(WidgetPeek::size(w), 1);
    s->enabled = s->btnState = -1;
    if (isA(w, paf::ui::Text::TypeName())) {
        paf::wstring str;
        w->GetString(str);
        s->isText = 1;
        s->textHash = hashText((const uint16_t*)str.c_str());
        toLog((const uint16_t*)str.c_str(), s->text, sizeof(s->text));
        return;
    }
    const bool button = isA(w, paf::ui::ButtonBase::TypeName());
    if (button) {
        paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(w);
        s->enabled = b->IsEnable();
        s->btnState = ButtonPeek::state(b);
        const paf::math::v4* dimmed = b->GetDisableColor();
        s->disableColor = dimmed ? rgba(*dimmed) : 0;
    }
    // Planes and app icons by their type's name: Plane::TypeName, which
    // IsInherit would want, is an import the plugin does without.
    if (button || startsWith(s->type, "plane") || startsWith(s->type, "app_icon"))
        textureOf(w, &s->tex, &s->texWidth, &s->texHeight);
}

// The children of the heading's group and of the row's, as they are now.
int snapAll(Snap* out) {
    int n = 0;
    paf::ui::Widget* const groups[2] = {g_headingGroup, g_rowGroup};
    for (int g = 0; g < 2; g++) {
        paf::ui::Widget* group = groups[g];
        const int children = group ? group->GetChildrenNum() : 0;
        for (int i = 0; i < children && i < 16 && n < kSnapMax; i++) {
            paf::ui::Widget* w = group->GetChild(i);
            if (w) snapWidget(w, &out[n++]);
        }
    }
    return n;
}

void logSnap(const char* when, const Snap& s) {
    Line l;
    if (s.enabled >= 0) {
        l.add(", button enabled %d state %d", s.enabled, s.btnState);
        if (s.disableColor) l.add(", dimmed color %08X", (unsigned)s.disableColor);
    }
    if (s.tex) l.add(", texture %p %dx%d", s.tex, s.texWidth, s.texHeight);
    if (s.isText) l.add(", text \"%s\"", s.text);
    logLine("%s %s 0x%08X: state %u, color %08X, show %d%%/%d%%, meta %d%%/%d%%%s, size %dx%d%s",
            when, s.type, (unsigned)s.hash, (unsigned)s.state, (unsigned)s.color, s.show,
            s.showSum, s.meta, s.metaSum, s.activated ? "" : ", off", s.width, s.height, l.buf);
}

void logChange(int ms, const Snap& was, const Snap& now) {
    Line l;
    if (was.state != now.state) l.add(", state %u", (unsigned)now.state);
    if (was.color != now.color) l.add(", color %08X", (unsigned)now.color);
    if (was.show != now.show || was.showSum != now.showSum)
        l.add(", show %d%%/%d%%", now.show, now.showSum);
    if (was.meta != now.meta || was.metaSum != now.metaSum)
        l.add(", meta %d%%/%d%%", now.meta, now.metaSum);
    if (was.activated != now.activated) l.add(", %s", now.activated ? "on" : "off");
    if (was.width != now.width || was.height != now.height)
        l.add(", size %dx%d", now.width, now.height);
    if (was.enabled != now.enabled || was.btnState != now.btnState)
        l.add(", button enabled %d state %d", now.enabled, now.btnState);
    if (was.disableColor != now.disableColor)
        l.add(", dimmed color %08X", (unsigned)now.disableColor);
    if (was.tex != now.tex || was.texWidth != now.texWidth || was.texHeight != now.texHeight)
        l.add(", texture %p %dx%d", now.tex, now.texWidth, now.texHeight);
    if (was.textHash != now.textHash) l.add(", text \"%s\"", now.text);
    if (l.n) logLine("+%d ms %s 0x%08X%s", ms, now.type, (unsigned)now.hash, l.buf);
}

// Ten times a second while the quick menu is open: under g_lock.
void timelineTick(SceUInt64 now) {
    if (!g_timeline) return;
    const int ms = msOpen(now);
    if (now - g_openedUs > kTimelineUs) {
        logLine("+%d ms row: changes no longer logged this time", ms);
        g_timeline = 0;
        return;
    }
    if (!g_rowGroup) {
        // The row's widgets come a moment after the quick menu.
        paf::ui::Widget* play = find(kHashPlay);
        if (!play) return;
        if (!g_layoutChecked) checkLayout(play);
        if (!g_layoutOk) {
            g_timeline = 0;
            return;
        }
        g_rowGroup = play->GetParent();
        paf::ui::Widget* heading = find(kHashHeading);
        g_headingGroup = heading ? heading->GetParent() : NULL;
        g_snapCount = snapAll(g_snaps);
        logLine("+%d ms row: %d widgets, %s", ms, g_snapCount,
                g_active ? (g_applied ? "the row ours" : "the row to be ours")
                         : "the row SceShell's");
        for (int i = 0; i < g_snapCount; i++) logSnap(" ", g_snaps[i]);
        return;
    }
    const int n = snapAll(g_next);
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < g_snapCount && g_snaps[j].w != g_next[i].w) j++;
        if (j < g_snapCount) {
            logChange(ms, g_snaps[j], g_next[i]);
        } else {
            char when[24];
            sceClibSnprintf(when, sizeof(when), "+%d ms new:", ms);
            logSnap(when, g_next[i]);
        }
    }
    for (int j = 0; j < g_snapCount; j++) {
        int i = 0;
        while (i < n && g_next[i].w != g_snaps[j].w) i++;
        if (i == n) logLine("+%d ms gone: %s 0x%08X", ms, g_snaps[j].type, (unsigned)g_snaps[j].hash);
    }
    sceClibMemcpy(g_snaps, g_next, sizeof(Snap) * n);
    g_snapCount = n;
}

// What this plugin has just changed, taken in without logging it again.
void timelineQuiet() {
    if (g_timeline && g_rowGroup) g_snapCount = snapAll(g_snaps);
}

// Takes the row, the quick menu having been open kTakeAfterUs: under g_lock.
// true when taken.
bool takeRow(SceUInt64 now) {
    g_rew = find(kHashRew);
    g_play = find(kHashPlay);
    g_ff = find(kHashFf);
    if (!g_rew || !g_play || !g_ff) {
        if (++g_findTries == 30) logLine("row: its buttons not found in 3 s");
        return false;
    }
    g_textTop = find(kHashTextTop);
    g_textBottom = find(kHashTextBottom);
    g_iconRew = find(kHashIconRew);
    g_iconPlay = find(kHashIconPlay);
    g_iconFf = find(kHashIconFf);
    if (!g_layoutChecked) checkLayout(g_play);
    const int logIt = g_takesLogged < 3 || g_lookLogged != g_look;
    if (logIt) {
        g_takesLogged++;
        g_lookLogged = g_look;
        logLine("row: taking it %d ms after the quick menu opened, buttons switched on with %s",
                msOpen(now), lookName(g_look));
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
        logLine("row: taken for %s%s%s", ownerName(g_owner),
                g_textTop ? "" : " (no text lines found)",
                g_iconRew && g_iconPlay && g_iconFf ? "" : " (not all icons found)");
    return true;
}

// While the row is ours and the quick menu open, ten times a second: puts
// back what SceShell has changed since. true when it changed anything.
bool keepRow() {
    bool changed = watchButtons();
    changed |= keepTexts();
    if (g_look == QM_LOOK_LIT) {
        const int logIt = g_dimsLogged < 6;
        if (undimAll(logIt)) {
            changed = true;
            if (logIt) g_dimsLogged++;
        }
    }
    return changed;
}

int g_openLogged;  // this open's opening was logged

void quickMenuOpened(SceUInt64 now) {
    forgetWidgets();
    g_openedUs = now;
    g_opens++;
    g_timeline = g_opens <= kTimelineOpens;
    g_openLogged = g_active || g_timeline;
    if (g_openLogged) logLine("quick menu opened%s", g_active ? "" : ", the row SceShell's");
}

void quickMenuClosed(SceUInt64 now) {
    if (g_active || g_openLogged) logLine("quick menu closed after %d ms", msOpen(now));
    forgetWidgets();
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
            const SceUInt64 now = sceKernelGetSystemTimeWide();
            lock();
            if (open && !wasOpen) quickMenuOpened(now);
            else if (!open && wasOpen) quickMenuClosed(now);
            if (open) {
                timelineTick(now);
                bool changed = false;
                if (g_active && !g_applied && now - g_openedUs >= kTakeAfterUs)
                    changed = takeRow(now);
                else if (g_active && g_applied)
                    changed = keepRow();
                if (changed) timelineQuiet();
            }
            if (g_active && now - g_activatedUs > QM_ACTIVE_TIMEOUT_S * 1000000ULL) {
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
            if (g_applied && g_active) {
                setTexts();
                timelineQuiet();
            }
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
                    timelineQuiet();
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
// What SceShell read last, the first changes of a boot logged, beside the
// row's look: on SceShell's threads, under nothing, so a change two of them
// read at once may be logged twice.
BgmState g_bgmSeen;
int g_bgmRet = 1, g_bgmPriority, g_bgmLogged;

int bgmStateCapped(BgmState* state) {
    const TaiHookUser* cur = (const TaiHookUser*)g_bgmHookRef;
    const TaiHookUser* next = (const TaiHookUser*)cur->next;
    typedef int (*Fn)(BgmState*);
    const int ret = next ? ((Fn)next->func)(state) : ((Fn)cur->old)(state);
    const int priority = state->priority;
    if (state->priority > 0x80) state->priority = 0x80;
    if (g_bgmLogged < 40 &&
        (ret != g_bgmRet || priority != g_bgmPriority || state->ownerId != g_bgmSeen.ownerId ||
         state->status1 != g_bgmSeen.status1 || state->state != g_bgmSeen.state ||
         state->status2 != g_bgmSeen.status2)) {
        g_bgmLogged++;
        g_bgmRet = ret;
        g_bgmPriority = priority;
        g_bgmSeen = *state;
        logLine("BGM port as SceShell reads it: 0x%08X; owner 0x%08X, priority 0x%X%s, "
                "0x%X, state %d, 0x%X",
                (unsigned)ret, (unsigned)state->ownerId, (unsigned)priority,
                priority > 0x80 ? " (read as 0x80)" : "", (unsigned)state->status1, state->state,
                (unsigned)state->status2);
    }
    return ret;
}

}  // namespace

extern "C" int module_start(SceSize args, const void* argp) {
    (void)args;
    (void)argp;
    logOpen();
    logLine("start: VitaPlex's quick menu plugin, pipes %s", QM_PIPE_TO_PLUGIN);

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
