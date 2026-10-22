/**
 * VitaPlex's quick menu plugin: the song, its cover, and buttons that work,
 * in the Music row of the quick menu (hold PS), for VitaPlex BG and its
 * background player.
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
 * - It changes SceShell's widgets on SceShell's own UI thread, through PAF's
 *   main thread call list, as ElevenMPV-A's app does its own. Its own threads
 *   watch the quick menu, take VitaPlex's messages and write the log.
 * - The buttons, switched on, kept their off look, with ElevenMPV-A's plugin
 *   and with this one (builds 2019 to 2026), though nothing the timeline read
 *   of them differed from SceShell's lit row. Build 2026's byte dumps found
 *   the icons' own alpha at 0.3, a field the timeline did not read. The
 *   plugin sets it to 1, which lights them (build 2028), and puts it back
 *   when the row is SceShell's again.
 * - It takes the row 200 ms after the quick menu opens, as ElevenMPV-A's
 *   plugin about does: the song this one put on the lines at once did not
 *   show (build 2022). While the quick menu stays open it puts back what
 *   SceShell changes: buttons switched off, other text.
 * - While VitaPlex plays, the play icon shows ⏸; and the Music heading's icon
 *   shows the song's cover, which VitaPlex writes to a file (QM_ART_PATH_FMT).
 * - A press is sent without waiting, so a process that stopped reading cannot
 *   hold SceShell up, and the row is given back by itself when no process has
 *   asked for it again in QM_ACTIVE_TIMEOUT_S.
 * - It logs the row's widgets as the quick menu opens and their changes for
 *   3 s, for the first opens of a boot, whoever has the row.
 * - When the system ends the background player, as an app with extended
 *   memory starts, SceShell carries its queue on: this plays the queue's
 *   songs that are on the memory card through SceShell's music service, from
 *   inside SceShell, and keeps the row (qm_protocol.h, build 2033).
 * - For a test, it fetches the start of a song from the server through Sony's
 *   HTTP library as SceShell has it (QM_CMD_HTTP_TEST, build 2040): whether
 *   SceShell can fetch the queue's songs that are not on the memory card.
 * - As it carries the queue on, it fetches those songs that way, each while
 *   the one before plays, when the shell plays their format (build 2042).
 * - VitaPlex BG can hand its queue to it straight away, with no background
 *   player (QM_CMD_PLAY_QUEUE, build 2043): it plays from the queue's song
 *   and place, fetches covers and marks songs played, as the player does.
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

#include "bgplayer.h"
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
extern "C" int vitaplexQmGetModuleExportFunc(const char* module, uint32_t libnid,
                                             uint32_t funcnid, uintptr_t* func);

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
// Lines are put together where they happen, SceShell's UI thread included,
// and kept in a ring; the watch thread writes them to the card, so that no
// line waits for the card on a thread SceShell draws with.
const char* const kLogPath = QM_PLUGIN_LOG;
const char* const kLogPrevPath = "ux0:data/VitaPlex/qm-plugin.prev.log";
const int kLogMax = 512 * 1024;
const int kRingLines = 128;
const int kLineMax = 320;

struct RingLine {
    int n;
    char text[kLineMax];
};
RingLine g_ring[kRingLines];
int g_ringHead, g_ringCount, g_ringDropped;
SceUID g_ringLock = -1;
SceUID g_log = -1;
int g_logged;

void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char* fmt, ...) {
    if (g_ringLock < 0) return;
    char line[kLineMax];
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
    sceKernelLockMutex(g_ringLock, 1, NULL);
    if (g_ringCount < kRingLines) {
        RingLine& r = g_ring[(g_ringHead + g_ringCount) % kRingLines];
        sceClibMemcpy(r.text, line, n);
        r.n = n;
        g_ringCount++;
    } else {
        g_ringDropped++;
    }
    sceKernelUnlockMutex(g_ringLock, 1);
}

// The watch thread's: the lines kept so far, to the card.
void logDrain() {
    if (g_log < 0) return;
    int wrote = 0;
    for (;;) {
        RingLine line;
        int dropped = 0;
        sceKernelLockMutex(g_ringLock, 1, NULL);
        const bool any = g_ringCount > 0;
        if (any) {
            line = g_ring[g_ringHead];
            g_ringHead = (g_ringHead + 1) % kRingLines;
            g_ringCount--;
        } else {
            dropped = g_ringDropped;
            g_ringDropped = 0;
        }
        sceKernelUnlockMutex(g_ringLock, 1);
        if (!any) {
            if (dropped) {
                char note[64];
                const int n = sceClibSnprintf(note, sizeof(note), "(%d lines not kept)\n", dropped);
                sceIoWrite(g_log, note, n);
                wrote++;
            }
            break;
        }
        if (g_logged <= kLogMax) {
            sceIoWrite(g_log, line.text, line.n);
            g_logged += line.n;
            wrote++;
        }
    }
    if (wrote) sceIoSyncByFd(g_log, 0);  // a SceShell crash keeps what came before it
}

void logOpen() {
    g_ringLock = sceKernelCreateMutex("VitaPlexQM::Log", 0, 0, NULL);
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
// One more there, at play's place and sized 0x0: the pause icon. SceShell's
// own row shows it in play's stead, by size, while its Music app plays
// (build 2025).
const uint32_t kHashIconPause = 0xB26E4562;
// The row's heading, "Music", in a group of its own above it, with its icon:
// a plane at the group's left, 0x0 and drawn at its texture's 80x80, as the
// other rows' icons are.
const uint32_t kHashHeading = 0x90E6251E;
const uint32_t kHashHeadingIcon = 0xE219E0DE;

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
// The row is looked at ten times a second while the quick menu is open.
const SceUInt64 kTickUs = 100 * 1000;
// PAF's Plugin keeps the pool its textures come from here (ScePaf-RE's
// plugin.h, surface_pool; private there).
const int kPluginSurfacePool = 0xD4;

// No object here may need constructing as the module loads: it has no C
// runtime to run constructors, so such an object stays all zeros. Build
// 2023's white was, and set the song's lines transparent. CMake fails the
// build if the module has any (.init_array).
paf::math::v4 white() {
    return paf::math::v4(1.0f, 1.0f, 1.0f, 1.0f);
}

paf::ui::Widget* g_impose;               // the quick menu's root
paf::graph::SurfacePool* g_surfacePool;  // power_manage_plugin's, for the cover
SceUID g_toPlugin = -1, g_fromPlugin = -1;
SceUID g_lock = -1;  // everything below, and the widgets

// The row's owner while SceShell itself carries the background player's queue
// on (qm_protocol.h), beside QM_OWNER_APP and QM_OWNER_PLAYER.
const uint32_t kOwnerShell = 3;
// The serials of the covers SceShell shows as it carries the queue on: from
// a file named by path (g_artPath), not by QM_ART_PATH_FMT. VitaPlex's
// serials are milliseconds since boot, which never reach this bit.
const uint32_t kShellArt = 0x80000000u;

// From VitaPlex, by the receiving thread, and as it carries the queue on,
// by the music thread:
int g_active;
uint32_t g_owner;
SceUInt64 g_activatedUs;  // the last QM_CMD_ACTIVATE
int g_look = QM_LOOK_LIT;
int g_playing = -1;
uint16_t g_title[256], g_artist[256], g_album[256];
uint32_t g_artSerial;  // the cover at QM_ART_PATH, 0 for none
char g_artPath[160];   // the file a kShellArt serial names
int g_releaseDue;      // the row given back, to be left as SceShell had it
// The music thread's (startMusicThread), woken for a test, for a press while
// SceShell carries the queue on, and to stop that.
SceUID g_musicSema = -1;
// From SceShell's UI thread (onPress), atomic: a press on the row for the
// music thread while SceShell carries the queue on, QM_PRESS_*.
int g_carryPress;

// The quick menu, by the watch thread:
int g_open;            // it is open
SceUInt64 g_openedUs;  // when it was seen open
int g_openSerial;      // opens so far; the row's tick sees a new one by it
int g_timelineDue;     // this open's changes are to be logged

// The row, by its tick (rowTick), on SceShell's UI thread: found anew each
// tick, so that nothing is kept of a row SceShell has since done away with.
paf::ui::Widget *g_rew, *g_play, *g_ff, *g_textTop, *g_textBottom;
paf::ui::Widget *g_iconRew, *g_iconPlay, *g_iconFf, *g_iconPause, *g_headingIcon;
uint32_t g_topHash, g_bottomHash;  // the lines as this plugin set them
int g_tickOpenSerial;              // the open this tick's state is for
SceUInt64 g_lastTickUs;
int g_applied;    // the row is ours this time it is open
int g_findTries;  // looks for the buttons this time
int g_mainTicks;  // times SceShell's UI thread has called mainTick
int g_fallback;   // it never has: the watch thread ticks the row itself
// Once a boot:
int g_layoutChecked, g_layoutOk;  // PAF's widget layout is its headers' (checkLayout)
int g_opens;                      // times the quick menu opened
int g_takesLogged;                // the first takes are logged in full
int g_lookLogged = -1;            // ... and the first with each look
int g_revertsLogged;              // buttons found switched off again, logged
int g_textsLogged;                // lines found changed, logged
int g_dimsLogged;                 // icons and buttons found dimmed, logged
int g_sawLitRow;                  // SceShell's own row seen lit, its music in it

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
    return owner == QM_OWNER_APP      ? "VitaPlex BG"
         : owner == QM_OWNER_PLAYER   ? "the background player"
         : owner == kOwnerShell       ? "SceShell"
                                      : "?";
}
const char* lookName(int look) {
    return look == QM_LOOK_ENABLE_NORMAL ? "Enable(true) and normal state"
         : look == QM_LOOK_AS_ELEVENMPVA ? "Enable(false), as ElevenMPV-A"
         : look == QM_LOOK_LIT           ? "Enable(true), normal state, buttons and icons lit"
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
    // Its own alpha, between its meta alpha's product and its colour (0x1AC):
    // an int ScePaf-RE's headers leave unnamed (m_unk_1AC), a float its older
    // ones call alpha. The row's icons had 0.3 there in build 2026, with all
    // their other alphas at 1.0, and the buttons and their group 1.0.
    // ElevenMPV-A's app dims a page behind another with SetAlpha(0.3f).
    static uint32_t ownAlphaRaw(paf::ui::Widget* w) {
        return (uint32_t)of(w)->m_unk_1AC;
    }
    // As a float, or -1 if it is not one from 0 to 1.
    static float ownAlpha(paf::ui::Widget* w) {
        const uint32_t raw = ownAlphaRaw(w);
        float f;
        sceClibMemcpy(&f, &raw, sizeof(f));
        return f >= 0.0f && f <= 1.0f ? f : -1.0f;
    }
    static void setOwnAlpha(paf::ui::Widget* w, float f) {
        uint32_t raw;
        sceClibMemcpy(&raw, &f, sizeof(raw));
        of(w)->m_unk_1AC = (int32_t)raw;
    }
};
struct ButtonPeek : paf::ui::ButtonBase {
    static int state(paf::ui::ButtonBase* b) {
        return (int)static_cast<ButtonPeek*>(b)->m_btn_state;
    }
};
// What a widget is drawn with (Widget::GetDrawObj, a virtual call): PAF's
// graph::DrawObj has a colour of its own besides the widget's. Its layout
// is the headers' if its size reads as its widget's (drawLayoutOk), as every
// one did in build 2024. Its visible bit read 0 for all of them, the row's
// heading too, so it is not read.
struct DrawPeek : paf::graph::DrawObj {
    static DrawPeek* of(paf::graph::DrawObj* d) {
        return static_cast<DrawPeek*>(d);
    }
    static uint32_t color(paf::graph::DrawObj* d) {
        return of(d)->m_color;
    }
    static void setColor(paf::graph::DrawObj* d, uint32_t color) {
        of(d)->m_color = color;
    }
    static int size(paf::graph::DrawObj* d, int i) {
        return (int)reinterpret_cast<const float*>(&of(d)->m_size)[i];
    }
    // Its class (its vtable) and its shader: a SharedPtr's first word.
    static const void* vtable(paf::graph::DrawObj* d) {
        return *reinterpret_cast<const void* const*>(d);
    }
    static const void* shader(paf::graph::DrawObj* d) {
        return *reinterpret_cast<const void* const*>(&of(d)->m_shader);
    }
};

// A vector's x or y, rounded: vitasdk-paf-component's v4 keeps its four floats
// to itself.
int coord(const paf::math::v4& v, int i) {
    return (int)reinterpret_cast<const float*>(&v)[i];
}

// A colour (red, green, blue, alpha) as the console keeps one in 32 bits,
// alpha first (kLitColor).
uint32_t argb(const paf::math::v4& v) {
    const float* f = reinterpret_cast<const float*>(&v);
    const int order[4] = {3, 0, 1, 2};
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) {
        const float x = f[order[i]] < 0.0f ? 0.0f : f[order[i]] > 1.0f ? 1.0f : f[order[i]];
        c = (c << 8) | (uint32_t)(x * 255.0f + 0.5f);
    }
    return c;
}

bool isA(paf::ui::Widget* w, const char* type) {
    return w && type && w->IsInherit(type);
}

// ── Textures ────────────────────────────────────────────────────────────────
// An intrusive_ptr to a texture that the plugin fills itself and never
// destroys: setting it adds no reference and nothing takes one away, so it
// calls neither Surface::AddRef nor Release, imports the plugin does without.
struct TexPtr : paf::intrusive_ptr<paf::graph::Surface> {
    void set(paf::graph::Surface* s) {
        m_ptr = s;
    }
};
union TexRef {
    TexPtr ptr;
    explicit TexRef(paf::graph::Surface* s) : ptr() {
        ptr.set(s);
    }
    ~TexRef() {}
};

// A widget's texture in slot `index` of its first draw object, for the log:
// where it is and its size. The reference GetTexture adds is dropped again
// without ever freeing the texture from this thread, should SceShell have let
// go of it meanwhile. Slot 0 by the call the console has run since build 2023.
void textureOf(paf::ui::Widget* w, int index, const void** tex, int* width, int* height) {
    TexRef ref(NULL);
    if (index == 0) w->GetTexture(ref.ptr);
    else w->GetTexture(ref.ptr, 0, (uint32_t)index);
    paf::graph::Surface* s = ref.ptr.get();
    *tex = s;
    *width = s ? s->GetWidth() : 0;
    *height = s ? s->GetHeight() : 0;
    if (s) s->UnsafeRelease();
}

// A widget's texture, with the reference GetTexture adds kept for good: the
// plugin's own hold on it, so it outlives any row SceShell makes.
paf::graph::Surface* holdTexture(paf::ui::Widget* w) {
    TexRef ref(NULL);
    w->GetTexture(ref.ptr);
    return ref.ptr.get();
}

paf::graph::Surface* currentTexture(paf::ui::Widget* w) {
    const void* tex;
    int width, height;
    textureOf(w, 0, &tex, &width, &height);
    return (paf::graph::Surface*)tex;
}

// Gives a widget a texture; the widget adds a reference of its own.
void giveTexture(paf::ui::Widget* w, paf::graph::Surface* s) {
    TexRef ref(s);
    w->SetTexture(ref.ptr);
}

// ── The row ─────────────────────────────────────────────────────────────────
paf::ui::Widget* find(uint32_t hash) {
    return g_impose->FindChild(paf::IDParam(hash), 0);
}

paf::ui::Widget* g_rowGroup;      // the play button's parent
paf::ui::Widget* g_headingGroup;  // the heading's
int g_timeline;                   // this open's changes are being logged
int g_snapCount;

// The row's widgets, by their ids: on its tick. true when its buttons are
// there; they come a moment after the quick menu.
bool findRow() {
    g_rew = find(kHashRew);
    g_play = find(kHashPlay);
    g_ff = find(kHashFf);
    g_textTop = find(kHashTextTop);
    g_textBottom = find(kHashTextBottom);
    g_iconRew = find(kHashIconRew);
    g_iconPlay = find(kHashIconPlay);
    g_iconFf = find(kHashIconFf);
    g_iconPause = find(kHashIconPause);
    g_headingIcon = find(kHashHeadingIcon);
    return g_rew && g_play && g_ff;
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
    // SceShell's own music: for the music thread, which is woken for it.
    if (__atomic_load_n(&g_owner, __ATOMIC_SEQ_CST) == kOwnerShell) {
        __atomic_store_n(&g_carryPress, (int)press, __ATOMIC_SEQ_CST);
        if (g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
        return;
    }
    QmPress msg;
    msg.press = press;
    msg.reserved = 0;
    // On SceShell's own thread: logged only when it goes wrong. VitaPlex logs
    // the presses it takes.
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

// ── Lighting the buttons and icons ──────────────────────────────────────────
// SceShell draws the row's buttons and their icons faded with no music of its
// own, and opaque and white with its Music app playing (screenshots, build
// 2023), with every colour, alpha, texture and shader the timeline reads alike
// either way (builds 2023 to 2025). Build 2026's byte dumps found the icons'
// own alpha (WidgetPeek::ownAlpha) at 0.3. A button or icon whose own alpha,
// meta alpha or colour is faded has its colour set (SetColor_core) and those
// alphas set to 1; a faded draw object's colour is set to kLitColor. What was
// changed is put back when the row is SceShell's again, where it is still
// the plugin's. Colours have their alpha in the top byte on the console,
// whatever the headers' GetColor says: the quick menu's background reads
// FF282828, the volume slider 4CCCCCCC as it fades in, the song's lines draw
// with D8FFFFFF and CCFFFFFF (build 2026). kLitColor is white with a last
// byte of 0xFE nobody would see: kept apart from a white SceShell sets.
const uint32_t kLitColor = 0xFFFFFFFE;
const int kLitMax = 6;

struct Lit {
    paf::ui::Widget* w;
    paf::graph::DrawObj* d;  // its draw object, if its colour was set
    uint32_t was;            // that colour as SceShell had it
    float alphaWas;          // its own alpha as SceShell had it, -1 if left
};
Lit g_lit[kLitMax];  // what the plugin lit, while it is to be put back
int g_litCount;
int g_drawLayoutLogged;

// Whether a draw object's layout is the headers' as far as its colour goes:
// its size reads as its widget's, which sits just before it.
bool drawLayoutOk(paf::ui::Widget* w, paf::graph::DrawObj* d) {
    const paf::math::v4& size = WidgetPeek::size(w);
    const int dw = DrawPeek::size(d, 0) - coord(size, 0);
    const int dh = DrawPeek::size(d, 1) - coord(size, 1);
    return dw >= -1 && dw <= 1 && dh >= -1 && dh <= 1 && coord(size, 0) > 0;
}

// A faded white or grey: its three colours alike, not opaque white, not
// nearly transparent, and not the plugin's own.
bool faded(uint32_t c) {
    const uint32_t a = c >> 24, x = (c >> 16) & 0xFF, y = (c >> 8) & 0xFF, z = c & 0xFF;
    return x == y && y == z && a >= 0x10 && c != 0xFFFFFFFF && c != kLitColor;
}

// The record of a widget the plugin lit, made the first time; NULL if full.
Lit* litEntry(paf::ui::Widget* w) {
    for (int i = 0; i < g_litCount; i++)
        if (g_lit[i].w == w) return &g_lit[i];
    if (g_litCount == kLitMax) return NULL;
    Lit& l = g_lit[g_litCount++];
    l.w = w;
    l.d = NULL;
    l.was = 0;
    l.alphaWas = -1.0f;
    return &l;
}

// One button or icon lit, logged if asked; true when it changed anything.
// A widget hidden by its colour or an alpha is left so.
bool light(paf::ui::Widget* w, const char* name, int logIt) {
    if (!w || !g_layoutOk) return false;
    const uint32_t color = w->GetColor32();
    const float meta = w->GetMetaAlpha();
    const float own = WidgetPeek::ownAlpha(w);
    if ((color >> 24) < 0x10 || meta < 0.05f || (own >= 0.0f && own < 0.05f)) return false;
    paf::graph::DrawObj* d = w->GetDrawObj(0);
    const uint32_t drawn = d ? DrawPeek::color(d) : 0;
    const bool dimWidget = color != 0xFFFFFFFF || meta < 0.99f;
    const bool dimOwn = own >= 0.0f && own < 0.99f;
    const bool dimDrawn = d && faded(drawn);
    if (!dimWidget && !dimOwn && !dimDrawn) return false;
    if (meta < 0.99f) WidgetPeek::setMetaAlpha(w, 1.0f);
    const paf::math::v4 c = white();
    w->SetColor_core(c);
    // Its own alpha after SetColor_core, should that call set it too.
    if (dimOwn) {
        Lit* l = litEntry(w);
        if (l && l->alphaWas < 0.0f) l->alphaWas = own;
        WidgetPeek::setOwnAlpha(w, 1.0f);
    }
    const uint32_t passed = d ? DrawPeek::color(d) : 0;
    // A draw object that was faded is set by hand even if SetColor_core lit
    // it: kLitColor marks it as the plugin's, to be put back.
    bool direct = false;
    if (dimDrawn) {
        Lit* l = drawLayoutOk(w, d) ? litEntry(w) : NULL;
        if (l) {
            if (!l->d) {
                l->d = d;
                l->was = drawn;
            }
            DrawPeek::setColor(d, kLitColor);
            direct = true;
        } else if (g_drawLayoutLogged < 3) {
            g_drawLayoutLogged++;
            logLine("row: %s's draw object reads %dx%d against its %dx%d: left faded", name,
                    DrawPeek::size(d, 0), DrawPeek::size(d, 1), coord(WidgetPeek::size(w), 0),
                    coord(WidgetPeek::size(w), 1));
        }
    }
    if (logIt)
        logLine("row: %s lit: color %08X, meta alpha %d%%, own alpha %d%%, drawn %08X; after "
                "SetColor_core drawn %08X%s%s",
                name, (unsigned)color, percent(meta), percent(own), (unsigned)drawn,
                (unsigned)passed, dimOwn ? ", own alpha set to 100%" : "",
                direct ? ", drawn set to FFFFFFFE" : "");
    return true;
}

bool lightAll(int logIt) {
    bool changed = light(g_iconRew, "previous's icon", logIt);
    changed |= light(g_iconPlay, "play/pause's icon", logIt);
    changed |= light(g_iconFf, "next's icon", logIt);
    changed |= light(g_rew, "previous", logIt);
    changed |= light(g_play, "play/pause", logIt);
    changed |= light(g_ff, "next", logIt);
    return changed;
}

// Puts back the own alphas and draw objects' colours the plugin changed,
// where they are still its: with the row's widgets found this tick
// (findRow). They must be the widgets lit, so a row SceShell made anew is
// left alone.
void unlightAll(const char* why) {
    if (!g_litCount) return;
    int back = 0;
    for (int i = 0; i < g_litCount; i++) {
        const Lit& l = g_lit[i];
        paf::ui::Widget* const found[6] = {g_iconRew, g_iconPlay, g_iconFf, g_rew, g_play, g_ff};
        bool same = false;
        for (int j = 0; j < 6 && !same; j++) same = found[j] == l.w;
        if (!same) continue;
        bool put = false;
        if (l.alphaWas >= 0.0f && WidgetPeek::ownAlpha(l.w) >= 0.99f) {
            WidgetPeek::setOwnAlpha(l.w, l.alphaWas);
            put = true;
        }
        if (l.d && l.w->GetDrawObj(0) == l.d && DrawPeek::color(l.d) == kLitColor) {
            DrawPeek::setColor(l.d, l.was);
            put = true;
        }
        if (put) back++;
    }
    logLine("row: %d of %d faded again as SceShell had them (%s)", back, g_litCount, why);
    g_litCount = 0;
}

// ── The play/pause icon ─────────────────────────────────────────────────────
// While VitaPlex plays, the play icon is given the pause icon's texture, and
// its own back when paused or when the row is SceShell's again (build 2025).
paf::graph::Surface *g_playTex, *g_pauseTex;  // held for good (holdTexture)
int g_pauseShown;  // the play icon has the pause icon's texture
int g_pauseLogged;

// Holds the two icons' textures, the first time the row is taken in a boot:
// SceShell's own then, before the plugin has changed either.
void holdPlayTextures() {
    if (g_playTex || !g_iconPlay || !g_iconPause) return;
    g_playTex = holdTexture(g_iconPlay);
    g_pauseTex = holdTexture(g_iconPause);
    logLine("row: play icon %p, pause icon %p, held", (void*)g_playTex, (void*)g_pauseTex);
}

// The play icon as VitaPlex's music is, or as SceShell's own row had it:
// true when it changed it.
bool showPlaying(bool pause) {
    if (!g_iconPlay || !g_playTex || !g_pauseTex) return false;
    paf::graph::Surface* want = pause ? g_pauseTex : g_playTex;
    if (currentTexture(g_iconPlay) == want) return false;
    giveTexture(g_iconPlay, want);
    g_pauseShown = pause;
    if (g_pauseLogged < 6) {
        g_pauseLogged++;
        logLine("row: play icon now shows %s", pause ? "pause" : "play");
    }
    return true;
}

// ── The cover ───────────────────────────────────────────────────────────────
// The song's cover, in place of the Music heading's icon while the row is
// VitaPlex's. VitaPlex writes it as a PNG (QM_ART_PATH_FMT, QM_ART_SIZE
// square) and names it by a serial (QM_CMD_ART). It is loaded with PAF's own
// decoder (Surface::Load, from a LocalFile), the calls ElevenMPV-A's app
// loads its covers with, into power_manage_plugin's texture pool. The icon
// keeps the only reference to it, so it goes when the icon gets its own back
// or SceShell does away with the row. The icon shows either its own texture,
// which the plugin holds, or the cover the plugin gave it last.
paf::graph::Surface* g_headingTex;  // the heading's own icon, held for good
uint32_t g_artShown;                // the cover given to the icon last, 0 for none
uint32_t g_artFailed;               // a cover that would not load, not tried again
int g_artLogged;

// The heading icon's own texture, held the first time it is seen.
void holdHeadingTexture() {
    if (g_headingTex || !g_headingIcon) return;
    g_headingTex = holdTexture(g_headingIcon);
    logLine("art: the heading's icon %p, held", (void*)g_headingTex);
}

// A cover's file as a texture, with one reference: the caller's.
paf::graph::Surface* loadArt(uint32_t serial) {
    if (!g_surfacePool) return NULL;
    char path[sizeof(g_artPath)];
    if (serial & kShellArt)
        sceClibSnprintf(path, sizeof(path), "%s", g_artPath);
    else
        sceClibSnprintf(path, sizeof(path), QM_ART_PATH_FMT, (unsigned)(serial & 1));
    int32_t err = 0;
    paf::common::SharedPtr<paf::LocalFile> file =
        paf::LocalFile::Open(path, SCE_O_RDONLY, 0, &err);
    if (err < 0 || !file.get()) {
        if (g_artLogged < 6) {
            g_artLogged++;
            logLine("art: %s would not open: 0x%08X", path, (unsigned)err);
        }
        return NULL;
    }
    // Surface::Load takes a SharedPtr<File>, which PAF's SharedPtr will not
    // make of a SharedPtr<LocalFile>: the same two pointers are passed as
    // one, as ElevenMPV-A's app does, copied into a SharedPtr<File> that is
    // never destroyed, so the file's count is the one `file` holds.
    union AsFile {
        paf::common::SharedPtr<paf::File> ptr;
        AsFile() {}
        ~AsFile() {}
    } asFile;
    static_assert(sizeof(asFile) == sizeof(file), "a SharedPtr is a SharedPtr");
    sceClibMemcpy(&asFile, &file, sizeof(asFile));
    // The texture it returns is built where it is wanted, so that nothing
    // takes its reference away.
    union Loaded {
        paf::intrusive_ptr<paf::graph::Surface> ptr;
        Loaded(paf::graph::SurfacePool* pool, paf::common::SharedPtr<paf::File>& f)
            : ptr(paf::graph::Surface::Load(pool, f, NULL)) {}
        ~Loaded() {}
    } loaded(g_surfacePool, asFile.ptr);
    return loaded.ptr.get();
}

// The heading's own icon on it again, if it has a cover. true when it
// changed it.
bool showOwnIcon(const char* why) {
    g_artShown = 0;
    if (!g_headingIcon || !g_headingTex || currentTexture(g_headingIcon) == g_headingTex)
        return false;
    giveTexture(g_headingIcon, g_headingTex);
    logLine("art: the heading's own icon back (%s)", why);
    return true;
}

// The heading's icon as the row's owner wants it: the cover named last, or
// its own. A cover is loaded again for an icon SceShell has made anew, which
// shows its own. true when it changed it.
bool showArt() {
    if (!g_headingIcon) return false;
    holdHeadingTexture();
    if (!g_headingTex) return false;
    const uint32_t want = g_artSerial;
    if (want == 0) return showOwnIcon("no cover");
    const bool own = currentTexture(g_headingIcon) == g_headingTex;
    if (!own && g_artShown == want) return false;
    if (g_artFailed == want) return own ? false : showOwnIcon("the cover would not load");
    const SceUInt64 start = sceKernelGetSystemTimeWide();
    paf::graph::Surface* art = loadArt(want);
    if (!art) {
        g_artFailed = want;
        logLine("art: %u not loaded", (unsigned)want);
        return own ? false : showOwnIcon("the cover would not load");
    }
    giveTexture(g_headingIcon, art);
    art->UnsafeRelease();  // the icon's reference is the one left
    g_artShown = want;
    logLine("art: %u, %dx%d, shown in %u ms", (unsigned)want, art->GetWidth(), art->GetHeight(),
            (unsigned)((uint32_t)(sceKernelGetSystemTimeWide() - start) / 1000u));
    return true;
}

// ── Switching the buttons on ────────────────────────────────────────────────
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
    case QM_LOOK_LIT:
        // PlaneButton has a ChangeButtonState of its own, which may also put
        // the button's normal look on it.
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
    if (g_look == QM_LOOK_LIT) lightAll(logIt);
}

// While the row is ours: a button SceShell has switched off again since, as
// its own set-up of the row may do after ours, is switched on again, and the
// first few times logged. true when it changed anything.
bool watchButtons() {
    if (!g_layoutOk || g_look == QM_LOOK_AS_ELEVENMPVA) return false;
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    const char* const names[3] = {"previous", "play/pause", "next"};
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(buttons[i]);
        // State 1 is dimmed; 2 to 4 are a press under way, left alone.
        const bool normalToo = g_look == QM_LOOK_ENABLE_NORMAL || g_look == QM_LOOK_LIT;
        if (buttonEnabled(b) == 1 && (!normalToo || buttonState(b) != 1)) continue;
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

// ── The lines ───────────────────────────────────────────────────────────────
void setTexts() {
    if (!g_textTop || !g_textBottom) return;
    paf::wstring top((const wchar_t*)g_title);
    paf::wstring bottom((const wchar_t*)g_artist);
    if (g_artist[0] && g_album[0]) bottom += L" / ";
    bottom += (const wchar_t*)g_album;
    g_topHash = hashText((const uint16_t*)top.c_str());
    g_bottomHash = hashText((const uint16_t*)bottom.c_str());
    const paf::math::v4 c = white();
    g_textTop->SetString(top);
    g_textTop->SetColor_core(c);
    g_textBottom->SetString(bottom);
    g_textBottom->SetColor_core(c);
}

// While the row is ours: the lines as the song is, should SceShell have put
// other text or another colour on them, or VitaPlex sent another song. true
// when it changed anything.
bool keepTexts() {
    if (!g_textTop || !g_textBottom || !g_layoutOk) return false;
    paf::wstring top, bottom;
    g_textTop->GetString(top);
    g_textBottom->GetString(bottom);
    const uint32_t topColor = g_textTop->GetColor32(), bottomColor = g_textBottom->GetColor32();
    // What the lines should hold, the song as it now is.
    paf::wstring wantTop((const wchar_t*)g_title);
    paf::wstring wantBottom((const wchar_t*)g_artist);
    if (g_artist[0] && g_album[0]) wantBottom += L" / ";
    wantBottom += (const wchar_t*)g_album;
    const bool newSong = hashText((const uint16_t*)wantTop.c_str()) != g_topHash ||
                         hashText((const uint16_t*)wantBottom.c_str()) != g_bottomHash;
    if (!newSong && hashText((const uint16_t*)top.c_str()) == g_topHash &&
        hashText((const uint16_t*)bottom.c_str()) == g_bottomHash && topColor == 0xFFFFFFFF &&
        bottomColor == 0xFFFFFFFF)
        return false;
    if (!newSong && g_textsLogged < 6) {
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
// SceShell's, they are the look it gives the row itself: faded with no music
// of its own, lit with its Music app playing.
const int kTimelineOpens = 12;
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
    uint32_t ownAlpha;      // its own alpha's bits (WidgetPeek::ownAlpha)
    const void* tex;
    int texWidth, texHeight;
    int isText;
    uint32_t textHash;
    char text[40];
    const void* draw;  // its first draw object, and that one's colour, size, class and shader
    uint32_t drawColor;
    int drawWidth, drawHeight;
    const void* drawClass;
    const void* shader;
    const void* moreTex[3];  // a plane's or plane button's textures in slots 1 to 3
    int moreWidth[3], moreHeight[3];
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
    s->ownAlpha = WidgetPeek::ownAlphaRaw(w);
    s->width = coord(WidgetPeek::size(w), 0);
    s->height = coord(WidgetPeek::size(w), 1);
    s->enabled = s->btnState = -1;
    paf::graph::DrawObj* d = w->GetDrawObj(0);
    if (d) {
        s->draw = d;
        s->drawColor = DrawPeek::color(d);
        s->drawWidth = DrawPeek::size(d, 0);
        s->drawHeight = DrawPeek::size(d, 1);
        s->drawClass = DrawPeek::vtable(d);
        s->shader = DrawPeek::shader(d);
    }
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
        s->disableColor = dimmed ? argb(*dimmed) : 0;
    }
    // Planes and app icons by their type's name: Plane::TypeName, which
    // IsInherit would want, is an import the plugin does without.
    const bool plane = startsWith(s->type, "plane");
    if (button || plane || startsWith(s->type, "app_icon"))
        textureOf(w, 0, &s->tex, &s->texWidth, &s->texHeight);
    // A plane's draw object has four texture slots (PlaneObj); none of the
    // row's had a second one in build 2025.
    if (plane)
        for (int i = 0; i < 3; i++)
            textureOf(w, i + 1, &s->moreTex[i], &s->moreWidth[i], &s->moreHeight[i]);
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

// A widget's own alpha for the log, if it is not 1: as a per cent, or its bits
// if it is not a float from 0 to 1.
void addOwnAlpha(Line& l, uint32_t raw) {
    float f;
    sceClibMemcpy(&f, &raw, sizeof(f));
    if (f >= 0.0f && f <= 1.0f) l.add(", own alpha %d%%", percent(f));
    else l.add(", own alpha field %08X", (unsigned)raw);
}

void logSnap(const char* when, const Snap& s) {
    Line l;
    if (s.ownAlpha != 0x3F800000) addOwnAlpha(l, s.ownAlpha);
    if (s.enabled >= 0) {
        l.add(", button enabled %d state %d", s.enabled, s.btnState);
        if (s.disableColor) l.add(", dimmed color %08X", (unsigned)s.disableColor);
    }
    if (s.tex) l.add(", texture %p %dx%d", s.tex, s.texWidth, s.texHeight);
    for (int i = 0; i < 3; i++)
        if (s.moreTex[i])
            l.add(", slot %d %p %dx%d", i + 1, s.moreTex[i], s.moreWidth[i], s.moreHeight[i]);
    if (s.draw)
        l.add(", drawn %08X %dx%d by %p, shader %p", (unsigned)s.drawColor, s.drawWidth,
              s.drawHeight, s.drawClass, s.shader);
    if (s.isText) l.add(", text \"%s\"", s.text);
    logLine("%s %s 0x%08X at %p: state %u, color %08X, show %d%%/%d%%, meta %d%%/%d%%%s, "
            "size %dx%d%s",
            when, s.type, (unsigned)s.hash, (void*)s.w, (unsigned)s.state, (unsigned)s.color,
            s.show, s.showSum, s.meta, s.metaSum, s.activated ? "" : ", off", s.width, s.height,
            l.buf);
}

void logChange(int ms, const Snap& was, const Snap& now) {
    Line l;
    if (was.state != now.state) l.add(", state %u", (unsigned)now.state);
    if (was.color != now.color) l.add(", color %08X", (unsigned)now.color);
    if (was.show != now.show || was.showSum != now.showSum)
        l.add(", show %d%%/%d%%", now.show, now.showSum);
    if (was.meta != now.meta || was.metaSum != now.metaSum)
        l.add(", meta %d%%/%d%%", now.meta, now.metaSum);
    if (was.ownAlpha != now.ownAlpha) addOwnAlpha(l, now.ownAlpha);
    if (was.activated != now.activated) l.add(", %s", now.activated ? "on" : "off");
    if (was.width != now.width || was.height != now.height)
        l.add(", size %dx%d", now.width, now.height);
    if (was.enabled != now.enabled || was.btnState != now.btnState)
        l.add(", button enabled %d state %d", now.enabled, now.btnState);
    if (was.disableColor != now.disableColor)
        l.add(", dimmed color %08X", (unsigned)now.disableColor);
    if (was.tex != now.tex || was.texWidth != now.texWidth || was.texHeight != now.texHeight)
        l.add(", texture %p %dx%d", now.tex, now.texWidth, now.texHeight);
    for (int i = 0; i < 3; i++)
        if (was.moreTex[i] != now.moreTex[i])
            l.add(", slot %d %p %dx%d", i + 1, now.moreTex[i], now.moreWidth[i],
                  now.moreHeight[i]);
    if (was.draw != now.draw || was.drawColor != now.drawColor ||
        was.drawWidth != now.drawWidth || was.drawHeight != now.drawHeight ||
        was.drawClass != now.drawClass || was.shader != now.shader)
        l.add(", drawn %08X %dx%d by %p, shader %p", (unsigned)now.drawColor, now.drawWidth,
              now.drawHeight, now.drawClass, now.shader);
    if (was.textHash != now.textHash) l.add(", text \"%s\"", now.text);
    if (l.n) logLine("+%d ms %s 0x%08X%s", ms, now.type, (unsigned)now.hash, l.buf);
}

// Ten times a second while the quick menu is open, the row found this tick.
void timelineTick(SceUInt64 now, bool found) {
    if (!g_timeline || !found) return;
    const int ms = msOpen(now);
    if (now - g_openedUs > kTimelineUs) {
        logLine("+%d ms row: changes no longer logged this time", ms);
        g_timeline = 0;
        return;
    }
    if (!g_layoutChecked) checkLayout(g_play);
    if (!g_layoutOk) {
        g_timeline = 0;
        return;
    }
    paf::ui::Widget* heading = find(kHashHeading);
    paf::ui::Widget* rowGroup = g_play->GetParent();
    paf::ui::Widget* headingGroup = heading ? heading->GetParent() : NULL;
    if (!g_snapCount || rowGroup != g_rowGroup || headingGroup != g_headingGroup) {
        g_rowGroup = rowGroup;
        g_headingGroup = headingGroup;
        g_snapCount = snapAll(g_snaps);
        // SceShell's own row with its play button on has its own music in it:
        // the lit look the plugin is after.
        if (!g_active && static_cast<paf::ui::ButtonBase*>(g_play)->IsEnable()) {
            g_sawLitRow = 1;
            logLine("+%d ms row: SceShell's own, lit: its own music is in it", ms);
        }
        // The groups' places tell whether SceShell keeps the row from one
        // open to the next or makes it anew.
        logLine("+%d ms row: %d widgets, %s (groups at %p and %p)", ms, g_snapCount,
                g_active ? (g_applied ? "the row ours" : "the row to be ours")
                         : "the row SceShell's",
                (void*)g_headingGroup, (void*)g_rowGroup);
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

// ── Taking, keeping and giving back the row ─────────────────────────────────
// Takes the row, the quick menu having been open kTakeAfterUs. true when
// taken.
bool takeRow(SceUInt64 now, bool found) {
    if (!found) {
        // Another of SceShell's overlays, such as the LiveArea's, reads as the
        // quick menu open too.
        if (++g_findTries == 30) logLine("row: its buttons not found in 3 s: not the quick menu");
        return false;
    }
    if (!g_layoutChecked) checkLayout(g_play);
    const int logIt = g_takesLogged < 3 || g_lookLogged != g_look;
    if (logIt) {
        g_takesLogged++;
        g_lookLogged = g_look;
        logLine("row: taking it %d ms after the quick menu opened, on %s's thread, buttons "
                "switched on with %s",
                msOpen(now), g_fallback ? "the plugin" : "SceShell's UI", lookName(g_look));
    }
    applyLookToAll(logIt);
    holdPlayTextures();
    showPlaying(g_playing == 1);
    g_rew->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                            (void*)(uintptr_t)QM_PRESS_PREVIOUS);
    g_play->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                             (void*)(uintptr_t)QM_PRESS_PLAY_PAUSE);
    g_ff->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                           (void*)(uintptr_t)QM_PRESS_NEXT);
    setTexts();
    showArt();
    g_applied = 1;
    if (logIt)
        logLine("row: taken for %s%s%s", ownerName(g_owner),
                g_textTop ? "" : " (no text lines found)",
                g_iconRew && g_iconPlay && g_iconFf ? "" : " (not all icons found)");
    return true;
}

// While the row is ours and the quick menu open, ten times a second: puts
// back what SceShell has changed since, and shows what VitaPlex has sent
// since. true when it changed anything.
bool keepRow() {
    bool changed = watchButtons();
    changed |= keepTexts();
    changed |= showPlaying(g_playing == 1);
    changed |= showArt();
    if (g_look == QM_LOOK_LIT) {
        const int logIt = g_dimsLogged < 6;
        if (lightAll(logIt)) {
            changed = true;
            if (logIt) g_dimsLogged++;
        }
    }
    return changed;
}

// The row given back with the quick menu open: it is left as SceShell's own
// set-up had it with no music of its own (build 2023's log): the lines empty,
// the buttons off and faded, play showing ▶ and the heading its own icon.
void releaseRow(const char* why) {
    unlightAll(why);
    if (g_pauseShown) showPlaying(false);
    showOwnIcon(why);
    const paf::wstring empty;
    if (g_textTop) g_textTop->SetString(empty);
    if (g_textBottom) g_textBottom->SetString(empty);
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    for (int i = 0; i < 3; i++) static_cast<paf::ui::ButtonBase*>(buttons[i])->Enable(false);
    g_applied = 0;
    logLine("row: left as SceShell had it: lines empty, buttons off");
}

// The row SceShell's, with something of the plugin's left on it since it was
// last ours. true when it changed anything.
bool restoreRow() {
    if (!g_litCount && !g_pauseShown && !g_artShown) return false;
    unlightAll("the row SceShell's");
    // Its play icon is put back only with the button off: with it on,
    // SceShell's own music is in the row, and ⏸ may be SceShell's.
    if (g_pauseShown && !static_cast<paf::ui::ButtonBase*>(g_play)->IsEnable()) showPlaying(false);
    g_pauseShown = 0;
    if (g_artShown) showOwnIcon("the row SceShell's");
    return true;
}

// ── The row's tick ──────────────────────────────────────────────────────────
// Each time round SceShell's UI loop (mainTick), under g_lock; the row itself
// is looked at ten times a second while the quick menu is open.
void rowTick(SceUInt64 now) {
    if (g_openSerial != g_tickOpenSerial) {
        // Opened since the last tick: what was kept of the last open goes.
        g_tickOpenSerial = g_openSerial;
        g_applied = 0;
        g_findTries = 0;
        g_rowGroup = g_headingGroup = NULL;
        g_snapCount = 0;
        g_timeline = g_timelineDue;
        g_lastTickUs = 0;
    }
    if (!g_open) {
        g_applied = 0;
        return;
    }
    if (now - g_lastTickUs < kTickUs) return;
    g_lastTickUs = now;
    const bool found = findRow();
    if (!found) g_applied = 0;  // a row SceShell has done away with is not ours
    timelineTick(now, found);
    bool changed = false;
    if (g_releaseDue) {
        g_releaseDue = 0;
        if (g_applied) {
            releaseRow("given back");
            changed = true;
        }
    }
    if (g_active && !g_applied && now - g_openedUs >= kTakeAfterUs)
        changed |= takeRow(now, found);
    else if (g_active && g_applied)
        changed |= keepRow();
    else if (!g_active && found)
        changed |= restoreRow();
    if (changed) timelineQuiet();
}

// PAF's main thread call list's: SceShell's UI thread, each time round its
// loop. It never waits for the plugin's lock, so SceShell is never held up.
void mainTick(void* data) {
    (void)data;
    if (!g_mainTicks) logLine("SceShell's UI thread ticks the row (thread 0x%08X)",
                              (unsigned)sceKernelGetThreadId());
    g_mainTicks++;
    if (g_fallback) return;
    if (sceKernelTryLockMutex(g_lock, 1) < 0) return;
    rowTick(sceKernelGetSystemTimeWide());
    unlock();
}

// ── SceShell's music service ────────────────────────────────────────────────
// The background player plays through SceShell's music service from a process
// of its own, which the system ends as an app with extended memory starts.
// Code in SceShell drives the same service (build 2032's test,
// docs/DESIGN_NOTES.md), so the queue is carried on from here once the player
// is gone (qm_protocol.h). The service's client object (sceShellSvcGetSvcObj)
// is found with taiHEN, so that the plugin does not import it and takes
// whatever SceShell has: in build 2032 SceShell's own module SceShellSvc had
// it. It is spoken to with libShellAudio's calls (lib/libshellaudio), as the
// background player makes them. The BGM port is left to the service, which
// took it for SceShell itself while it played for SceShell (build 2032): the
// libraries that take it (SceAppMgr) and that name a module (SceModulemgr)
// are ones no plugin known to load in SceShell imports, and one SceShell
// refused would keep the whole plugin out. All of it on a thread of its own,
// never SceShell's UI thread, which may be the one to answer the calls.
const uint32_t kShellSvcLibrary = 0x773242C7;  // SceShellSvc (src/utils's shellsvc_stub.S)
const uint32_t kGetSvcObj = 0xB31E7F1C;        // sceShellSvcGetSvcObj
const char* const kShellSvcPath = "vs0:sys/external/libshellsvc.suprx";
// The events libShellAudio sends (ShellAudio.h's SCE_MUSIC_EVENTID_*).
const int kEventPlay = 1, kEventStop = 2, kEventSeek = 0x11;

typedef void* (*GetSvcObj)(void);
// A method's arguments, in and out (libShellAudio's SceShellSvcAudioCustomParams).
struct SvcParams {
    const void* p[4][2];  // pointer, size
};
// The client's invokeSyncMethod, at 0x14 in its vtable.
typedef int (*SvcInvoke)(void* obj, int method, SvcParams* in, int inCount, int* res,
                         SvcParams* out, int outCount);
// Who the service is talking to: for the music player service of type 0,
// as the background player starts it, 1 and 8.
struct SvcTracking {
    int unk;
    int t1, t2;
};

GetSvcObj g_getSvcObj;
char g_musicPath[256];   // the file the test plays; under g_lock
int g_musicTestDue;      // a test asked for, not yet begun; under g_lock
int g_musicBusy;         // a test runs, or is asked for
int g_musicThread;       // its thread started

void setArg(SvcParams* a, int i, const void* p, uint32_t size) {
    a->p[i][0] = p;
    a->p[i][1] = (const void*)(uintptr_t)size;
}

// A method of SceShell's music service, as libShellAudio calls it. The call's
// code if it failed, else the service's answer.
int svcCall(int method, SvcParams* in, int inCount, SvcParams* out, int outCount) {
    void* obj = g_getSvcObj ? g_getSvcObj() : NULL;
    if (!obj) return -1;
    SvcInvoke invoke = (*(SvcInvoke* const*)obj)[5];
    int res = 0;
    const int ret = invoke(obj, method, in, inCount, &res, out, outCount);
    return ret != 0 ? ret : res;
}

const SvcTracking kMusicPlayer = {0, 1, 8};

int musicEvent(int event) {
    const int command[2] = {event, 0};
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, command, sizeof(command));
    setArg(&in, 1, &kMusicPlayer, sizeof(kMusicPlayer));
    return svcCall(0x30006, &in, 2, NULL, 0);
}

int musicOpen(const char* path) {
    char opt[16];
    sceClibMemset(opt, 0, sizeof(opt));
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    setArg(&in, 1, path, (uint32_t)sceClibStrnlen(path, 0x401));
    setArg(&in, 2, opt, sizeof(opt));
    return svcCall(0x30004, &in, 3, NULL, 0);
}

// Its tracking alone: the service's start (0x30000) and end (0x30001).
int musicTracked(int method) {
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    return svcCall(method, &in, 1, NULL, 0);
}

// The play status (0x30007): the state at 0x10, the clock in ms at 0x2C.
int musicStatus(int* state, uint32_t* timeMs) {
    uint32_t buf[0x70 / 4];
    sceClibMemset(buf, 0, sizeof(buf));
    SvcParams in, out;
    sceClibMemset(&in, 0, sizeof(in));
    sceClibMemset(&out, 0, sizeof(out));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    setArg(&out, 0, buf, sizeof(buf));
    const int rc = svcCall(0x30007, &in, 1, &out, 1);
    *state = (int)buf[0x10 / 4];
    *timeMs = buf[0x2C / 4];
    return rc;
}

// Where the next seek event takes the song (0x3000B), as libShellAudio's
// sceMusicPlayerServiceSetSeekTime sets it.
int musicSeekTime(uint32_t ms) {
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, &ms, sizeof(ms));
    setArg(&in, 1, &kMusicPlayer, sizeof(kMusicPlayer));
    return svcCall(0x3000B, &in, 2, NULL, 0);
}

// The track info (0x3001B): the length the shell gives its song, in ms at 8,
// of a buffer libShellAudio sizes at 0x1810.
uint32_t g_trackInfo[0x1810 / 4];  // the music thread's
int musicLength(uint32_t* ms) {
    sceClibMemset(g_trackInfo, 0, sizeof(g_trackInfo));
    SvcParams in, out;
    sceClibMemset(&in, 0, sizeof(in));
    sceClibMemset(&out, 0, sizeof(out));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    setArg(&out, 0, g_trackInfo, sizeof(g_trackInfo));
    const int rc = svcCall(0x3001B, &in, 1, &out, 1);
    const int len = (int)g_trackInfo[8 / 4];
    *ms = rc >= 0 && len > 0 ? (uint32_t)len : 0;
    return rc;
}

// sceShellSvcGetSvcObj in a module of SceShell's by the name of either library
// libshellsvc.suprx exports. 0 when found.
int findGetSvcObj(uintptr_t* fn, const char** found) {
    const char* const names[2] = {"SceShellSvc", "SceShellUtil"};
    int rc = -1;
    for (int i = 0; i < 2 && rc < 0; i++) {
        rc = vitaplexQmGetModuleExportFunc(names[i], kShellSvcLibrary, kGetSvcObj, fn);
        *found = names[i];
    }
    return rc;
}

// The client object's maker, from SceShell's own SceShellSvc, else from the
// library loaded into SceShell now. `who` begins each line it logs. true when
// found.
bool findSvcObj(const char* who) {
    if (g_getSvcObj) return true;
    uintptr_t fn = 0;
    const char* name = "";
    int rc = findGetSvcObj(&fn, &name);
    logLine("%s: sceShellSvcGetSvcObj in SceShell already: 0x%08X%s%s", who, (unsigned)rc,
            rc >= 0 ? ", in module " : "", rc >= 0 ? name : "");
    if (rc < 0) {
        int status = 0;
        const SceUID mod = sceKernelLoadStartModule(kShellSvcPath, 0, NULL, 0, NULL, &status);
        logLine("%s: %s loaded into SceShell: 0x%08X, its start 0x%08X", who, kShellSvcPath,
                (unsigned)mod, (unsigned)status);
        if (mod < 0) return false;
        rc = findGetSvcObj(&fn, &name);
        logLine("%s: sceShellSvcGetSvcObj in it: 0x%08X%s%s", who, (unsigned)rc,
                rc >= 0 ? ", in module " : " (not under either library's name)",
                rc >= 0 ? name : "");
        if (rc < 0) return false;
    }
    g_getSvcObj = (GetSvcObj)fn;
    return true;
}

// ── Playing a file from inside SceShell (a test) ────────────────────────────
// QM_CMD_MUSIC_TEST: the first thing the queue needed here, tried on its own.
// Every code logged, each line with "music test:" in it.

// Watches the service's clock for `seconds`, logging what changes. true when
// it ran on by 2 s or more, counting only the small steps between readings,
// not a jump to where an earlier song left it; with `untilMoving`, it stops
// watching then.
bool watchMusic(int seconds, bool untilMoving) {
    int lastState = -1000, havePrev = 0;
    uint32_t prev = 0, ran = 0, lastSecond = 0xFFFFFFFF;
    bool moved = false;
    for (int i = 0; i < seconds * 4; i++) {
        int state = 0;
        uint32_t timeMs = 0;
        const int rc = musicStatus(&state, &timeMs);
        if (rc < 0) {
            logLine("music test: status: 0x%08X", (unsigned)rc);
        } else {
            if (havePrev && timeMs > prev && timeMs - prev < 1000) ran += timeMs - prev;
            prev = timeMs;
            havePrev = 1;
            if (state != lastState || timeMs / 1000 != lastSecond)
                logLine("music test: state %d, at %u.%u s", state, (unsigned)(timeMs / 1000),
                        (unsigned)((timeMs % 1000) / 100));
            lastState = state;
            lastSecond = timeMs / 1000;
            if (ran >= 2000) moved = true;
        }
        if (untilMoving && moved) break;
        sceKernelDelayThread(250 * 1000);
    }
    return moved;
}

void musicTest(const char* path) {
    logLine("music test: %s, from inside SceShell", path);
    if (!findSvcObj("music test")) {
        logLine("music test: done: no client for SceShell's music service");
        return;
    }
    void* obj = g_getSvcObj();
    logLine("music test: client object %p, its methods at %p", obj,
            obj ? *(void* const*)obj : NULL);
    if (!obj) {
        logLine("music test: done: no client object");
        return;
    }
    const int rcStart = musicTracked(0x30000);
    logLine("music test: start the music player service (type 0): 0x%08X", (unsigned)rcStart);
    if (rcStart < 0) {
        logLine("music test: done: the service would not start");
        return;
    }
    const int rcStop = musicEvent(kEventStop), rcOpen = musicOpen(path),
              rcPlay = musicEvent(kEventPlay);
    const SceUInt64 playAt = sceKernelGetSystemTimeWide();
    logLine("music test: stop 0x%08X, open 0x%08X, play 0x%08X", (unsigned)rcStop,
            (unsigned)rcOpen, (unsigned)rcPlay);
    const bool played = watchMusic(8, true);
    if (played) {
        // In 32 bits, as msOpen: the test is over in well under 71 minutes.
        const int left = QM_MUSIC_TEST_S -
                         (int)((uint32_t)(sceKernelGetSystemTimeWide() - playAt) / 1000000u);
        logLine("music test: playing; on for %d s more", left);
        watchMusic(left, false);
    }
    const int rcEnd = musicEvent(kEventStop), rcFinish = musicTracked(0x30001);
    logLine("music test: stop 0x%08X, end the service 0x%08X", (unsigned)rcEnd,
            (unsigned)rcFinish);
    logLine("music test: done: %s", played ? "SceShell played it" : "its clock never moved");
}

// ── Fetching from inside SceShell (a test) ──────────────────────────────────
// QM_CMD_HTTP_TEST: what SceShell needs to fetch the queue's songs that are
// not on the memory card, tried on its own. Sony's SSL and HTTP libraries, as
// SceShell has them, are found with taiHEN as the music service is: imported,
// a library SceShell did not have would keep the whole plugin out. What holds
// for all of SceShell is left as it is: the TLS options and certificates, and
// the libraries' start, ended only when this test began them. On a thread of
// its own with a 64 KB stack, which it measures, so that the fetching to come
// knows whether the music thread's 32 KB would do. Every code logged, each
// line with "http test:" in it; the token goes as a header, never logged.
struct LibFn {
    const char* name;
    uint32_t nid;
    uintptr_t fn;  // found, or 0
};
const uint32_t kSslLibrary = 0xCAF5F2E9;   // SceSsl, in module SceLibSsl
const uint32_t kHttpLibrary = 0xE8F15CDE;  // SceHttp, in module SceLibHttp
enum { kSslInit, kSslTerm, kSslFns };
LibFn g_ssl[kSslFns] = {{"sceSslInit", 0x3C733316, 0}, {"sceSslTerm", 0x03CE6E3A, 0}};
enum {
    kHttpInit,
    kHttpTerm,
    kHttpCreateTemplate,
    kHttpDeleteTemplate,
    kHttpCreateConnection,
    kHttpDeleteConnection,
    kHttpCreateRequest,
    kHttpDeleteRequest,
    kHttpAddHeader,
    kHttpSend,
    kHttpStatus,
    kHttpLength,
    kHttpRead,
    kHttpResolveTimeOut,
    kHttpConnectTimeOut,
    kHttpSendTimeOut,
    kHttpRecvTimeOut,
    kHttpsSslError,
    kHttpFns
};
LibFn g_http[kHttpFns] = {
    {"sceHttpInit", 0x214926D9, 0},
    {"sceHttpTerm", 0xC9076666, 0},
    {"sceHttpCreateTemplate", 0x62241DAB, 0},
    {"sceHttpDeleteTemplate", 0xEC85ECFB, 0},
    {"sceHttpCreateConnectionWithURL", 0xC616C200, 0},
    {"sceHttpDeleteConnection", 0xF0F65C15, 0},
    {"sceHttpCreateRequestWithURL", 0xBD5DA1D0, 0},
    {"sceHttpDeleteRequest", 0x3D3D29AD, 0},
    {"sceHttpAddRequestHeader", 0x7B51B122, 0},
    {"sceHttpSendRequest", 0x9CA58B99, 0},
    {"sceHttpGetStatusCode", 0x27071691, 0},
    {"sceHttpGetResponseContentLength", 0xF580D304, 0},
    {"sceHttpReadData", 0x7EDE3979, 0},
    {"sceHttpSetResolveTimeOut", 0x8455B5B3, 0},
    {"sceHttpSetConnectTimeOut", 0x237CA86E, 0},
    {"sceHttpSetSendTimeOut", 0x8AE3F008, 0},
    {"sceHttpSetRecvTimeOut", 0x94BF196E, 0},
    {"sceHttpsGetSslError", 0x2B79BDE0, 0},
};
// As psp2/net/http.h has them.
typedef int (*FnUnsigned)(unsigned);
typedef int (*FnVoid)(void);
typedef int (*FnId)(int);
typedef int (*FnIdUnsigned)(int, unsigned);
typedef int (*FnTemplate)(const char*, int, int);
typedef int (*FnConnection)(int, const char*, int);
typedef int (*FnRequest)(int, int, const char*, unsigned long long);
typedef int (*FnHeader)(int, const char*, const char*, unsigned);
typedef int (*FnSend)(int, const void*, unsigned);
typedef int (*FnStatus)(int, int*);
typedef int (*FnLength)(int, unsigned long long*);
typedef int (*FnRead)(int, void*, unsigned);
typedef int (*FnSslError)(int, int*, unsigned*);
template <typename F>
F httpFn(int i) {
    return (F)g_http[i].fn;
}
const int kHttpVersion11 = 2;                    // SCE_HTTP_VERSION_1_1
const int kHttpGet = 0;                          // SCE_HTTP_METHOD_GET
const unsigned kHeaderOverwrite = 0;             // SCE_HTTP_HEADER_OVERWRITE
const int kHttpAlreadyInited = (int)0x80431020;  // SCE_HTTP_ERROR_ALREADY_INITED
const int kSslAlreadyInited = (int)0x80435020;   // what SceShell's gave (build 2040)
// The pools, should this test be what starts the libraries.
const unsigned kSslPool = 256 * 1024, kHttpPool = 64 * 1024;
const char* const kHttpTestFile = "ux0:data/VitaPlex/qm-http-test.bin";
const int kHttpStack = 0x10000;
const uint32_t kPaintBytes = 40 * 1024;  // of it painted, below where the thread begins
const uint32_t kPaint = 0x56504C58;

// From QM_CMD_HTTP_TEST: set under g_lock while g_httpBusy is 0, then the
// test thread's until it sets it back.
char g_httpBase[256], g_httpPath[256], g_httpToken[256];
uint32_t g_httpBytes;
int g_httpBusy;
uint8_t g_httpBuf[8192];  // the test thread's

// A library's functions, from SceShell's module of it, else from its file
// loaded into SceShell now. `who` begins each line it logs. true when every
// one is found.
bool findLibrary(const char* who, const char* module, uint32_t libnid, const char* path,
                 LibFn* fns, int count) {
    uintptr_t fn = 0;
    int rc = vitaplexQmGetModuleExportFunc(module, libnid, fns[0].nid, &fn);
    logLine("%s: %s in SceShell already: 0x%08X", who, module, (unsigned)rc);
    if (rc < 0) {
        int status = 0;
        const SceUID mod = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
        logLine("%s: %s loaded into SceShell: 0x%08X, its start 0x%08X", who, path,
                (unsigned)mod, (unsigned)status);
        if (mod < 0) return false;
    }
    bool all = true;
    for (int i = 0; i < count; i++) {
        fn = 0;
        rc = vitaplexQmGetModuleExportFunc(module, libnid, fns[i].nid, &fn);
        fns[i].fn = rc >= 0 ? fn : 0;
        if (rc < 0) {
            all = false;
            logLine("%s: %s not in %s: 0x%08X", who, fns[i].name, module, (unsigned)rc);
        }
    }
    return all;
}

// Both libraries' functions, for `who`.
bool findLibraries(const char* who) {
    return findLibrary(who, "SceLibSsl", kSslLibrary, "vs0:sys/external/libssl.suprx", g_ssl,
                       kSslFns) &&
           findLibrary(who, "SceLibHttp", kHttpLibrary, "vs0:sys/external/libhttp.suprx", g_http,
                       kHttpFns);
}

// Both libraries started, unless SceShell has started them: their starts'
// codes, which say what is to be ended after (httpEnd). Logged always, or
// only when SceShell had not started them. true when the HTTP library can be
// used.
bool httpStart(const char* who, const char* what, bool always, int* rcSsl, int* rcHttp) {
    *rcSsl = ((FnUnsigned)g_ssl[kSslInit].fn)(kSslPool);
    if (always || *rcSsl != kSslAlreadyInited)
        logLine("%s: SSL library started: 0x%08X%s%s", who, (unsigned)*rcSsl,
                *rcSsl >= 0 ? ", here, for this " : "", *rcSsl >= 0 ? what : "");
    *rcHttp = httpFn<FnUnsigned>(kHttpInit)(kHttpPool);
    if (always || *rcHttp != kHttpAlreadyInited)
        logLine("%s: HTTP library started: 0x%08X%s%s", who, (unsigned)*rcHttp,
                *rcHttp >= 0                    ? ", here, for this "
                : *rcHttp == kHttpAlreadyInited ? ", SceShell's already"
                                                : "",
                *rcHttp >= 0 ? what : "");
    return *rcHttp >= 0 || *rcHttp == kHttpAlreadyInited;
}

// What httpStart started, ended.
void httpEnd(const char* who, int rcSsl, int rcHttp) {
    if (rcHttp >= 0)
        logLine("%s: HTTP library ended: 0x%08X", who, (unsigned)httpFn<FnVoid>(kHttpTerm)());
    if (rcSsl >= 0)
        logLine("%s: SSL library ended: 0x%08X", who, (unsigned)((FnVoid)g_ssl[kSslTerm].fn)());
}

// A template with the time-outs the test found to do: 5 s to resolve and to
// connect, 10 s to send and to receive. Its id, or the code it failed with.
int httpTemplate(const char* who) {
    const int tmpl = httpFn<FnTemplate>(kHttpCreateTemplate)("VitaPlex (SceShell)", kHttpVersion11, 1);
    if (tmpl < 0) {
        logLine("%s: template 0x%08X", who, (unsigned)tmpl);
        return tmpl;
    }
    const int rcResolve = httpFn<FnIdUnsigned>(kHttpResolveTimeOut)(tmpl, 5000000u),
              rcConnect = httpFn<FnIdUnsigned>(kHttpConnectTimeOut)(tmpl, 5000000u),
              rcSend = httpFn<FnIdUnsigned>(kHttpSendTimeOut)(tmpl, 10000000u),
              rcRecv = httpFn<FnIdUnsigned>(kHttpRecvTimeOut)(tmpl, 10000000u);
    if (rcResolve < 0 || rcConnect < 0 || rcSend < 0 || rcRecv < 0)
        logLine("%s: time-outs of 5, 5, 10 and 10 s: 0x%08X 0x%08X 0x%08X 0x%08X", who,
                (unsigned)rcResolve, (unsigned)rcConnect, (unsigned)rcSend, (unsigned)rcRecv);
    return tmpl;
}

// What sceHttpsGetSslError's detail says (SCE_HTTPS_ERROR_SSL_*), for the log.
void sslDetail(unsigned detail, char* out, int cap) {
    static const char* const kWhy[6] = {"internal",      "invalid certificate",
                                        "not the server's name", "expired",
                                        "not yet valid", "unknown CA"};
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < 6 && n < cap; i++)
        if (detail & (1u << i)) n += sceClibSnprintf(out + n, cap - n, "%s%s", n ? ", " : ": ", kWhy[i]);
}

// A request's connection and the request, deleted (either may be -1).
void httpClose(int conn, int req) {
    if (req >= 0) httpFn<FnId>(kHttpDeleteRequest)(req);
    if (conn >= 0) httpFn<FnId>(kHttpDeleteConnection)(conn);
}

// A GET of url sent with the template tmpl: VitaPlex's names for itself as
// headers, as the player sends them, the token too when given, and a range
// when given. `who` and `what` name it in the log, which has the call that
// failed, and why TLS refused it. 0, with *conn and *req to read from and
// then close (httpClose); else the code of the call that failed, with
// nothing left to close.
int httpOpen(int tmpl, const char* who, const char* what, const char* url, const char* token,
             const char* range, int* conn, int* req) {
    *req = -1;
    *conn = httpFn<FnConnection>(kHttpCreateConnection)(tmpl, url, 1);
    if (*conn < 0) {
        logLine("%s: %s: connection 0x%08X", who, what, (unsigned)*conn);
        return *conn;
    }
    *req = httpFn<FnRequest>(kHttpCreateRequest)(*conn, kHttpGet, url, 0);
    if (*req < 0) {
        const int rc = *req;
        logLine("%s: %s: request 0x%08X", who, what, (unsigned)rc);
        httpClose(*conn, -1);
        return rc;
    }
    int rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Client-Identifier", "vitaplex-sceshell",
                                              kHeaderOverwrite);
    if (rc >= 0)
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Product", "VitaPlex", kHeaderOverwrite);
    if (rc >= 0 && token && token[0])
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Token", token, kHeaderOverwrite);
    if (rc >= 0 && range) rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "Range", range, kHeaderOverwrite);
    if (rc >= 0) rc = httpFn<FnSend>(kHttpSend)(*req, NULL, 0);
    if (rc < 0) {
        int err = 0;
        unsigned detail = 0;
        const int rcSsl = httpFn<FnSslError>(kHttpsSslError)(*req, &err, &detail);
        char why[128];
        sslDetail(detail, why, sizeof(why));
        logLine("%s: %s: failed 0x%08X; TLS error 0x%08X, detail 0x%X%s (0x%08X)", who, what,
                (unsigned)rc, (unsigned)err, detail, why, (unsigned)rcSsl);
        httpClose(*conn, *req);
        return rc;
    }
    return 0;
}

// One GET of url with the template tmpl, `what` naming it in the log: the
// token as a header when given, and only the first `bytes` asked for and read
// when non-0, the body written to kHttpTestFile when `save`. The status code,
// or the code of the call that failed (< 0); the body's size in *got.
int httpGet(int tmpl, const char* what, const char* url, const char* token, uint32_t bytes,
            bool save, uint32_t* got) {
    *got = 0;
    char range[32];
    if (bytes) sceClibSnprintf(range, sizeof(range), "bytes=0-%u", (unsigned)(bytes - 1));
    const SceUInt64 t0 = sceKernelGetSystemTimeWide();
    int conn, req;
    const int rc = httpOpen(tmpl, "http test", what, url, token, bytes ? range : NULL, &conn, &req);
    if (rc < 0) return rc;
    int status = 0;
    unsigned long long length = 0;
    const int rcStatus = httpFn<FnStatus>(kHttpStatus)(req, &status);
    const int rcLength = httpFn<FnLength>(kHttpLength)(req, &length);
    const SceUID fd =
        save ? sceIoOpen(kHttpTestFile, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666) : -1;
    uint8_t first[8];
    sceClibMemset(first, 0, sizeof(first));
    int rcRead = 0, rcWrite = 0;
    for (;;) {
        uint32_t want = sizeof(g_httpBuf);
        if (bytes && bytes - *got < want) want = bytes - *got;
        if (want == 0) break;
        rcRead = httpFn<FnRead>(kHttpRead)(req, g_httpBuf, want);
        if (rcRead <= 0) break;
        if (*got == 0) sceClibMemcpy(first, g_httpBuf, rcRead < 8 ? rcRead : 8);
        if (fd >= 0 && rcWrite >= 0) {
            const int w = sceIoWrite(fd, g_httpBuf, (SceSize)rcRead);
            if (w != rcRead) rcWrite = w < 0 ? w : -1;
        }
        *got += (uint32_t)rcRead;
    }
    if (fd >= 0) sceIoClose(fd);
    httpClose(conn, req);
    // In 32 bits, as msOpen: a test is over in well under 71 minutes.
    const uint32_t ms = (uint32_t)(sceKernelGetSystemTimeWide() - t0) / 1000u;
    char len[24];
    if (rcLength < 0)
        sceClibSnprintf(len, sizeof(len), "none (0x%08X)", (unsigned)rcLength);
    else if (length >> 32)
        sceClibSnprintf(len, sizeof(len), "4 GB or more");
    else
        sceClibSnprintf(len, sizeof(len), "%u", (unsigned)length);
    logLine("http test: %s: status %d (0x%08X), length %s; %u bytes in %u ms, %u KB/s, "
            "the last read 0x%08X",
            what, status, (unsigned)rcStatus, len, (unsigned)*got, (unsigned)ms,
            (unsigned)(*got / (ms ? ms : 1)), (unsigned)rcRead);
    if (save)
        logLine("http test: %s: written to %s: %s (0x%08X), its first bytes %02X %02X %02X "
                "%02X %02X %02X %02X %02X",
                what, kHttpTestFile, fd < 0 || rcWrite < 0 ? "no" : "yes",
                (unsigned)(fd < 0 ? fd : rcWrite), first[0], first[1], first[2], first[3],
                first[4], first[5], first[6], first[7]);
    return rcStatus < 0 ? rcStatus : status;
}

// The test: base's /identity with no token, and over plain http too, then
// the first `bytes` of the file at path with the token, only over the first
// if its TLS held. What it came to.
const char* httpTest(const char* base, const char* path, const char* token, uint32_t bytes) {
    logLine("http test: %s, its /identity, then the first %u KB of %s, from inside SceShell",
            base, (unsigned)(bytes / 1024), path);
    if (!findLibraries("http test"))
        return "Sony's SSL and HTTP libraries are not all there in SceShell";
    // Begun here only if SceShell has not begun them, and then ended here.
    int rcSsl, rcHttp;
    const char* outcome = "the HTTP library would not start";
    if (httpStart("http test", "test", true, &rcSsl, &rcHttp)) {
        const int tmpl = httpTemplate("http test");
        logLine("http test: template 0x%08X", (unsigned)tmpl);
        outcome = "no template";
        if (tmpl >= 0) {
            static char url[600];
            uint32_t got = 0;
            sceClibSnprintf(url, sizeof(url), "%s/identity", base);
            const int identity = httpGet(tmpl, "/identity", url, NULL, 0, false, &got);
            if (startsWith(base, "https://")) {
                sceClibSnprintf(url, sizeof(url), "http://%s/identity", base + 8);
                httpGet(tmpl, "/identity over plain http", url, NULL, 0, false, &got);
            }
            if (identity != 200) {
                outcome = "the server's /identity did not answer 200, so the token was not sent";
            } else {
                sceClibSnprintf(url, sizeof(url), "%s%s", base, path);
                const int song = httpGet(tmpl, "the song", url, token, bytes, true, &got);
                logLine("http test: %s removed: 0x%08X", kHttpTestFile,
                        (unsigned)sceIoRemove(kHttpTestFile));
                outcome = (song == 206 || song == 200) && got > 0 ? "SceShell fetched the song's start"
                                                                  : "the song's start did not come";
            }
            logLine("http test: template deleted: 0x%08X",
                    (unsigned)httpFn<FnId>(kHttpDeleteTemplate)(tmpl));
        }
    }
    httpEnd("http test", rcSsl, rcHttp);
    return outcome;
}

// The test's thread. The stack below where it begins is painted, and read
// back after the test for how far down the test reached; nothing is called
// as it is painted, so the frames painted over are none in use.
#if defined(__arm__)
#define READ_SP(sp) asm volatile("mov %0, sp" : "=r"(sp))
#else  // the host test
#define READ_SP(sp) asm volatile("mov %%rsp, %0" : "=r"(sp))
#endif
__attribute__((no_sanitize_address)) int httpThread(SceSize, void*) {
    uintptr_t sp;
    READ_SP(sp);
    volatile uint32_t* const low = (volatile uint32_t*)((sp - 256 - kPaintBytes) & ~(uintptr_t)3);
    volatile uint32_t* const high = (volatile uint32_t*)((sp - 256) & ~(uintptr_t)3);
    for (volatile uint32_t* p = low; p < high; p++) *p = kPaint;
    const char* outcome = httpTest(g_httpBase, g_httpPath, g_httpToken, g_httpBytes);
    volatile uint32_t* p = low;
    while (p < high && *p == kPaint) p++;
    logLine("http test: its thread used %u KB of its %u KB stack%s",
            (unsigned)((sp - (uintptr_t)p) / 1024), (unsigned)(kHttpStack / 1024),
            p == low ? " or more, all that was measured" : "");
    logLine("http test: done: %s", outcome);
    lock();
    sceClibMemset(g_httpToken, 0, sizeof(g_httpToken));
    g_httpBusy = 0;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// ── SceShell carries the queue on ───────────────────────────────────────────
// (qm_protocol.h) The background player stopped checking in without giving
// the row back: the system has ended it, and its song plays on in the shell.
// From that song's end the queue's next song on the memory card is played
// from here, and the one after, much as the player plays them (player.c's
// watchSong). Everything comes from the files the player and VitaPlex BG keep
// (bgplayer.h): the player's status for the song playing, the queue for the
// songs and their downloads, and the player's own files of them as
// QM_CMD_SONG_FILE named them. The status is written as the player writes it,
// with "by=SceShell", so that VitaPlex BG takes the music back where SceShell
// has got to. The row stays the player's as it was: the song's lines, its
// cover if the player made one (QM_SONG_COVER_FMT), and its buttons, which act
// on the music here. Its lines in the log begin "carry on:".
const int kLookAhead = 20;        // songs looked at for the next on the card
const int kSongFiles = 4;         // the player's song files remembered
// Stopping: the place written and the music left, for VitaPlex BG to take
// back; the music left to a process that took the row, with nothing written;
// something else in the shell, the status saying stopped; the queue played
// out, or nothing more on the card: the service stopped and ended.
enum { kEndTakeBack = 1, kEndYield, kEndOther, kEndOver, kEndStopped };

// Under g_lock:
QmSongFile g_songFiles[kSongFiles];  // the last QM_CMD_SONG_FILEs
int g_songFilesNext;
int g_carryAllowed;      // the player has had the row since VitaPlex BG took the music back
int g_carry;             // 1 to begin (the watch thread's ask), 2 carrying on
const char* g_carryStop; // why it is to stop, or NULL ...
int g_carryStopHow;      // ... and how (kEnd*)
int g_playQueueDue;      // VitaPlex BG has handed its queue over (QM_CMD_PLAY_QUEUE)
uint32_t g_shellArts;    // kShellArt serials so far

// The music thread's alone.
struct Track {
    int index;          // its place in the queue, from 0
    char key[16];       // its ratingKey
    uint32_t durationMs;  // the server's length, 0 if unknown
    char local[128];    // a download of it, or ""
    char thumb[128];    // its cover on the server (a path), or ""
    char title[192], artist[128], album[128];  // UTF-8, for the row
    char path[128];     // the file the shell is given
    int own;            // the player's (BGPLAYER_SONGS): removed once played
};
// A few songs remembered by their place in the queue: the last 8.
struct Ring {
    int v[8];
    int n;  // added so far
};

bool ringHas(const Ring* r, int i) {
    const int n = r->n < 8 ? r->n : 8;
    for (int k = 0; k < n; k++)
        if (r->v[k] == i) return true;
    return false;
}

void ringAdd(Ring* r, int i) {
    if (!ringHas(r, i)) r->v[r->n++ & 7] = i;
}

struct Carry {
    char queue[32];     // the queue's "written=", as it has it
    int repeat, count;
    Track cur;          // the song with the shell
    int started;        // seen playing, with its length (newSong)
    int menuPaused;     // the row's play/pause stopped it
    int resuming;       // the row's play pressed, the shell not yet playing
    int failures;       // songs in a row that would not play
    uint32_t maxTime, prevTime, lenMs, lastLenMs;
    uint32_t playMs;    // the clock as it last played, where it goes on from after a stop
    int havePrev, replays, lastState;
    int otherLogged;    // the shell seen playing something else in its place, logged
    int waitTicks;      // looks so far for it to play: ticks, not the clock, which runs on
                        // through a sleep
    SceUInt64 songUs;      // when it was handed to the shell
    SceUInt64 stoppedUs;   // stopped before its end since, 0 if not
    SceUInt64 lastPlayUs, statusUs, checkUs;
    // A song not on the card, fetched to play next with the shell stopped
    // meanwhile: its place, -1 when none; the song; since when; the fetches
    // begun for it; why it is next (carryPlay's why); and looks in a row that
    // found the shell playing something meanwhile.
    int waitIndex;
    Track wait;
    SceUInt64 waitUs;
    int waitTries;
    const char* waitWhy;
    int waitPlays;
    // A song's file fetched here and not yet played: its place (-1 for none),
    // key and file. Kept apart from g_fetch, which goes on to other jobs.
    int fetchedIndex;
    char fetchedKey[16];
    char fetchedPath[128];
    int fetchWant;   // a song to fetch once the fetch running has stopped, -1 if none
    int fetchEnds;   // g_fetch.ends as last seen
    int aheadDue;    // what to fetch next to be looked at (fetchAhead)
    Ring lost;         // songs the server has no file of for the shell
    Ring failedAhead;  // songs whose fetch failed ahead: fetched again only when next
    Ring coverTried;   // songs whose cover was asked for
    // VitaPlex BG's song as it handed the queue over (-1 for none), and where
    // in it: the shell is taken there once it plays (seekTo), and a seek to
    // there under way (0 for none), since when, and how many times asked.
    int startIndex;
    uint32_t startAtMs;
    uint32_t seekWantMs;
    SceUInt64 seekUs;
    int seekTries;
};
Carry g_c;
Track g_found;  // a song looked at for what comes next
int g_statusErrors, g_statusReadErrors;

// No 64-bit division here: it would bring in libgcc's, built for another
// wchar_t. Microseconds as seconds and the rest, a byte at a time, each step
// a 32-bit division (the remainder stays below 1e6 * 256).
void splitUs(SceUInt64 us, uint32_t* secs, uint32_t* micros) {
    uint32_t q = 0, r = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
        const uint32_t cur = (r << 8) | (uint32_t)((us >> shift) & 0xFF);
        q = (q << 8) | (cur / 1000000u);
        r = cur % 1000000u;
    }
    *secs = q;
    *micros = r;
}

// Milliseconds as "m:ss", for the log.
struct Mmss {
    char s[16];
};
Mmss mmss(uint32_t ms) {
    Mmss m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u:%02u", (unsigned)(ms / 60000),
                    (unsigned)(ms / 1000 % 60));
    return m;
}

bool same(const char* a, const char* b) {
    for (; *a && *a == *b; a++, b++) {
    }
    return *a == *b;
}

uint32_t toUint(const char* s) {
    uint32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint32_t)(*s - '0');
    return v;
}

int toInt(const char* s) {
    return *s == '-' ? -(int)toUint(s + 1) : (int)toUint(s);
}

// A time on the system clock as a note has it ("%.6f" seconds): the same two
// times as these whatever their last digits.
void toSecs(const char* s, uint32_t* secs, uint32_t* micros) {
    *secs = toUint(s);
    *micros = 0;
    while (*s >= '0' && *s <= '9') s++;
    if (*s != '.') return;
    s++;
    uint32_t m = 0;
    int digits = 0;
    for (; digits < 6 && *s >= '0' && *s <= '9'; s++, digits++) m = m * 10 + (uint32_t)(*s - '0');
    for (; digits < 6; digits++) m *= 10;
    *micros = m;
}

bool sameTime(const char* a, const char* b) {
    uint32_t as, am, bs, bm;
    toSecs(a, &as, &am);
    toSecs(b, &bs, &bm);
    return as == bs && am == bm;
}

// A file read whole into buf, as much as fits, ended with 0. Its length, or
// the error.
int readFile(const char* path, char* buf, int cap) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    int n = 0;
    while (n < cap - 1) {
        const int got = sceIoRead(fd, buf + n, (SceSize)(cap - 1 - n));
        if (got <= 0) break;
        n += got;
    }
    sceIoClose(fd);
    buf[n] = 0;
    return n;
}

bool exists(const char* path) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    sceIoClose(fd);
    return true;
}

// The value of key in a note (bgplayer.h's "key=value" lines) into out, cut
// to fit. true when it is there.
bool noteValue(const char* text, const char* key, char* out, int cap) {
    const int klen = (int)sceClibStrnlen(key, 64);
    for (const char* p = text; *p;) {
        if (startsWith(p, key) && p[klen] == '=') {
            p += klen + 1;
            int n = 0;
            for (; *p && *p != '\n' && *p != '\r'; p++)
                if (n < cap - 1) out[n++] = *p;
            out[n] = 0;
            return true;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    out[0] = 0;
    return false;
}

// The queue a line at a time, without its end; a line longer than the room
// for it is cut. POD, so that nothing needs constructing as the module loads.
struct LineReader {
    SceUID fd;
    char buf[2048];
    int len, pos;
};
LineReader g_reader;
char g_line[1024];  // the line read last

bool readerOpen(const char* path) {
    g_reader.fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    g_reader.len = g_reader.pos = 0;
    return g_reader.fd >= 0;
}

void readerClose() {
    if (g_reader.fd >= 0) sceIoClose(g_reader.fd);
    g_reader.fd = -1;
}

// The next line in g_line. false at the file's end.
bool readerNext() {
    int n = 0;
    bool any = false;
    for (;;) {
        if (g_reader.pos == g_reader.len) {
            const int got = sceIoRead(g_reader.fd, g_reader.buf, sizeof(g_reader.buf));
            g_reader.len = got > 0 ? got : 0;
            g_reader.pos = 0;
            if (g_reader.len == 0) break;
        }
        any = true;
        const char c = g_reader.buf[g_reader.pos++];
        if (c == '\n') break;
        if (c != '\r' && n < (int)sizeof(g_line) - 1) g_line[n++] = c;
    }
    g_line[n] = 0;
    return any;
}

// The queue read through: its head lines, which come before its songs, and
// each song in turn.
struct QueueScan {
    int index;          // the song read last, -1 before the first
    bool whole;         // its "end" read
    char written[32];
    int repeat;
    int start;          // the song to start with (index=), and where in it
    uint32_t startMs;
    char file[128];     // that song as a file VitaPlex BG made ready (file=), or ""
};

bool scanOpen(QueueScan* s) {
    s->index = -1;
    s->whole = false;
    s->written[0] = 0;
    s->repeat = BGPLAYER_REPEAT_OFF;
    s->start = 0;
    s->startMs = 0;
    s->file[0] = 0;
    return readerOpen(BGPLAYER_QUEUE);
}

// The next song's fields, after "track=", in *fields. false at the end.
bool scanNext(QueueScan* s, const char** fields) {
    while (readerNext()) {
        if (startsWith(g_line, "track=")) {
            s->index++;
            *fields = g_line + 6;
            return true;
        }
        if (startsWith(g_line, "written=")) {
            sceClibSnprintf(s->written, sizeof(s->written), "%s", g_line + 8);
        } else if (startsWith(g_line, "repeat=")) {
            s->repeat = (int)toUint(g_line + 7);
        } else if (startsWith(g_line, "index=")) {
            s->start = toInt(g_line + 6);
        } else if (startsWith(g_line, "positionMs=")) {
            s->startMs = toUint(g_line + 11);
        } else if (startsWith(g_line, "file=")) {
            // A path is never cut: one too long is left out.
            const bool fits = sceClibStrnlen(g_line + 5, sizeof(s->file)) < sizeof(s->file);
            sceClibSnprintf(s->file, sizeof(s->file), "%s", fits ? g_line + 5 : "");
        } else if (same(g_line, "end")) {
            s->whole = true;
        }
    }
    return false;
}

// Up to the next '|' of *p into out, and past it: cut to fit, but never in
// the middle of a UTF-8 character.
void field(const char** p, char* out, int cap) {
    int n = 0;
    bool cut = false;
    for (; **p && **p != '|'; (*p)++) {
        if (n < cap - 1) out[n++] = **p;
        else cut = true;
    }
    if (**p == '|') (*p)++;
    if (cut) {
        while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0 && ((unsigned char)out[n - 1] & 0x80)) n--;
    }
    out[n] = 0;
}

// The same, but left empty if it does not fit: a path is never cut.
void wholeField(const char** p, char* out, int cap) {
    int n = 0;
    bool cut = false;
    for (; **p && **p != '|'; (*p)++) {
        if (n < cap - 1) out[n++] = **p;
        else cut = true;
    }
    if (**p == '|') (*p)++;
    out[cut ? 0 : n] = 0;
}

// A song's fields as bgplayer.h lays them out (BGPLAYER_TRACK_*).
void parseTrack(const char* p, int index, Track* t) {
    sceClibMemset(t, 0, sizeof(*t));
    t->index = index;
    char num[16], skip[4];
    field(&p, t->key, sizeof(t->key));
    field(&p, num, sizeof(num));
    t->durationMs = toUint(num);
    wholeField(&p, t->local, sizeof(t->local));
    field(&p, skip, sizeof(skip));  // the rating
    wholeField(&p, t->thumb, sizeof(t->thumb));
    field(&p, t->title, sizeof(t->title));
    field(&p, t->artist, sizeof(t->artist));
    field(&p, t->album, sizeof(t->album));
}

// The queue's song at index into *t. true when it is there.
bool trackAt(int index, Track* t) {
    QueueScan s;
    if (index < 0 || !scanOpen(&s)) return false;
    const char* fields;
    bool found = false;
    while (!found && scanNext(&s, &fields)) {
        if (s.index == index) {
            parseTrack(fields, index, t);
            found = true;
        }
    }
    readerClose();
    return found;
}

// The extension of a path's last part, in lower case, with mp4 as m4a
// (player.c's extOf); "" with none, and cut to fit.
void extOf(const char* path, char* out, int cap) {
    const char* dot = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '.') dot = p;
        if (*p == '/') dot = NULL;
    }
    int n = 0;
    for (const char* p = dot ? dot + 1 : ""; *p && n < cap - 1; p++)
        out[n++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
    out[n] = 0;
    if (same(out, "mp4")) sceClibSnprintf(out, cap, "m4a");
}

// A format the shell plays, by its extension (player.c's shellDecodes).
bool shellExt(const char* ext) {
    return same(ext, "mp3") || same(ext, "m4a") || same(ext, "aac") || same(ext, "wav");
}

bool shellPlays(const char* path) {
    char ext[8];
    extOf(path, ext, sizeof(ext));
    return shellExt(ext);
}

// Where t is on the card, into t->path: its download if the shell plays its
// format, else the player's file of it. false when it is on neither.
bool onCard(Track* t) {
    if (t->local[0] && shellPlays(t->local) && exists(t->local)) {
        sceClibSnprintf(t->path, sizeof(t->path), "%s", t->local);
        t->own = 0;
        return true;
    }
    char path[128] = "";
    lock();
    for (int i = 0; i < kSongFiles; i++)
        if (g_songFiles[i].key[0] && same(g_songFiles[i].key, t->key))
            sceClibSnprintf(path, sizeof(path), "%s", g_songFiles[i].path);
    unlock();
    if (!path[0] || !exists(path)) return false;
    sceClibSnprintf(t->path, sizeof(t->path), "%s", path);
    t->own = startsWith(path, BGPLAYER_SONGS "/");
    return true;
}

// The song `steps` on from `from` in the queue's order, back for a negative
// count, repeat all going round; -1 past either end.
int stepFrom(int from, int steps) {
    const int i = from + steps;
    if (i >= 0 && i < g_c.count) return i;
    if (g_c.repeat != BGPLAYER_REPEAT_ALL || g_c.count <= 0) return -1;
    return ((i % g_c.count) + g_c.count) % g_c.count;
}

// Whether the shell has a song of the length len (its own figure) where it
// should have t: the server's length to 3 s, or with none from the server,
// any length but the song before's. As player.c's shellHasNewSong.
bool hasSong(uint32_t len, const Track* t, uint32_t lenBefore) {
    if (len == 0) return false;
    if (t->durationMs > 0) {
        const int d = (int)len - (int)t->durationMs;
        return d > -3000 && d < 3000;
    }
    const int e = (int)len - (int)lenBefore;
    return lenBefore == 0 || e <= -3000 || e >= 3000;
}

// UTF-8 to the UTF-16 the row shows, ending in 0, cut to fit; what is not
// UTF-8 becomes U+FFFD. As qm_link.c's toUtf16.
void fromUtf8(const char* s, uint16_t* out, int cap) {
    const unsigned char* p = (const unsigned char*)s;
    int n = 0;
    while (*p && n + 1 < cap) {
        uint32_t c = *p++;
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

// The status as the player writes it (player.c's writeStatus), with
// "by=SceShell" on it, for t: the song with the shell, or one the shell waits
// for as it is fetched ("fetching=1").
void carryStatusOf(const Track* t, const char* state, uint32_t positionMs, bool fetching) {
    static char text[448];
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    uint32_t secs, micros;
    splitUs(now, &secs, &micros);
    const int n = sceClibSnprintf(
        text, sizeof(text),
        "written=%u.%06u\nqueue=%s\nindex=%d\nratingKey=%s\npositionMs=%u\ndurationMs=%u\n"
        "state=%s\nwasPlaying=0\nby=SceShell\n%send\n",
        (unsigned)secs, (unsigned)micros, g_c.queue, t->index, t->key, (unsigned)positionMs,
        (unsigned)t->durationMs, state, fetching ? "fetching=1\n" : "");
    const char* const part = BGPLAYER_STATUS ".part";
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int rc = fd;
    if (fd >= 0) {
        rc = sceIoWrite(fd, text, (SceSize)n);
        sceIoClose(fd);
    }
    if (rc == n) {
        // rename does not replace a file on every target (player.c's moveIn).
        sceIoRemove(BGPLAYER_STATUS);
        rc = sceIoRename(part, BGPLAYER_STATUS);
    }
    if (rc < 0 && g_statusErrors < 3) {
        g_statusErrors++;
        logLine("carry on: status not written: 0x%08X", (unsigned)rc);
    }
    g_c.statusUs = now;
}

void carryStatus(const char* state, uint32_t positionMs) {
    carryStatusOf(&g_c.cur, state, positionMs, false);
}

// The player's file of a song played, and its cover, once the shell has
// another: as player.c removes its own.
void removeOwn(const Track* t) {
    if (!t->own || !t->path[0]) return;
    const int rc = sceIoRemove(t->path);
    char cover[160];
    sceClibSnprintf(cover, sizeof(cover), QM_SONG_COVER_FMT, t->key);
    sceIoRemove(cover);
    logLine("carry on: song %d's file removed: 0x%08X", t->index + 1, (unsigned)rc);
}

// The row as t is: its lines, ⏸, and its cover if the player made one, or a
// fetch here.
void showTrack(const Track* t) {
    char cover[160];
    sceClibSnprintf(cover, sizeof(cover), QM_SONG_COVER_FMT, t->key);
    const bool art = exists(cover);
    lock();
    fromUtf8(t->title, g_title, 256);
    fromUtf8(t->artist, g_artist, 256);
    fromUtf8(t->album, g_album, 256);
    g_playing = 1;
    if (art) {
        sceClibSnprintf(g_artPath, sizeof(g_artPath), "%s", cover);
        g_artSerial = kShellArt | (++g_shellArts & ~kShellArt);
    } else {
        g_artSerial = 0;
    }
    unlock();
}

// The row's cover, once t's has come, its lines and ⏸ left as they are.
void showCover(const Track* t) {
    char cover[160];
    sceClibSnprintf(cover, sizeof(cover), QM_SONG_COVER_FMT, t->key);
    if (!exists(cover)) return;
    lock();
    sceClibSnprintf(g_artPath, sizeof(g_artPath), "%s", cover);
    g_artSerial = kShellArt | (++g_shellArts & ~kShellArt);
    unlock();
}

// Where in t the shell is to begin: VitaPlex BG's place in the song it handed
// over, until the shell has been taken there; 0 for any other.
uint32_t startAt(const Track* t) {
    return t->index == g_c.startIndex ? g_c.startAtMs : 0;
}

// ── Fetching the queue's songs ──────────────────────────────────────────────
// A song the queue goes on to that is not on the memory card is fetched from
// VitaPlex's server, as the background player fetches one (player.c's
// prepareSong): its file as the server has it, when the shell plays its
// format, into the player's folder of songs (BGPLAYER_SONGS), then its cover
// for the row (QM_SONG_COVER_FMT). Each goes once played, as the player's own
// files do. The song after the one with the shell is fetched while that one
// plays; one wanted at once (that one ended first, or next was pressed) is
// waited for, the shell stopped meanwhile. With nothing else to fetch, the
// covers the row is to show come next, for the songs on the card too: this
// song's, then the next one's. One fetch at a time, on a thread of its own,
// through Sony's libraries as the test above finds them. The server comes
// from VitaPlex's settings, and the token too, read for each fetch and wiped
// after it. Songs in other formats, of which the player has the server make
// MP3s, are passed over.
enum { kFetchIdle, kFetchRunning, kFetchDone, kFetchFailed };
enum { kJobSong, kJobCover };  // a song's file and its cover, or its cover alone
const int kFetchStack = 0x8000;
const int kFetchStopped = 1;  // fetchBody's: stopped by a cancel

// Under g_lock. t is the fetch thread's as it runs, but for its place and key.
struct Fetch {
    int state;    // kFetch*
    int kind;     // kJob*
    int cancel;   // set to stop it
    int urgent;   // the song is waited for: its cover is left for later
    int coverDone;  // a song's job fetched its cover, or found it would not come
    int ends;     // fetches ended so far
    int forGood;  // failed in a way another try would not mend
    int stopped;  // failed as it was stopped
    int count;    // the queue's songs, for the log
    Track t;      // the song; its file in t.path once fetched
};
Fetch g_fetch;
char g_server[256];  // the music thread's: VitaPlex's server, "" with none
Track g_ahead;       // the music thread's: a song looked at to fetch
// The fetch thread's: the libraries' functions found; what it reads (the
// settings, a song's details) or passes on to the card; a request's URL and
// the file beside the one it is for.
int g_netFound;
uint8_t g_fetchBuf[16384];
char g_fetchUrl[1024], g_fetchPart[168];

// The string value of "key" in VitaPlex's settings, a flat object with \ and
// " escaped (player.c's jsonString). Its length, or -1 with out "".
int jsonString(const char* json, const char* key, char* out, int cap) {
    char pat[40];
    const int plen = sceClibSnprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = 0;
    const char* v = json;
    while (*v && !startsWith(v, pat)) v++;
    if (!*v) return -1;
    v += plen;
    while (*v && *v != ':') v++;
    while (*v && *v != '"') v++;
    if (!*v) return -1;
    v++;
    int n = 0;
    while (*v && *v != '"' && n < cap - 1) {
        if (*v == '\\' && v[1]) v++;
        out[n++] = *v++;
    }
    out[n] = 0;
    if (*v == '"') return n;
    out[0] = 0;
    return -1;
}

// VitaPlex's server, with no '/' at its end, and its token, from the start
// of its settings, where VitaPlex writes both, read into buf and wiped from
// it after. true when there is a server; the token may be "".
bool readServer(char* buf, int cap, char* server, int serverCap, char* token, int tokenCap) {
    const int n = readFile(BGPLAYER_DIR "settings.json", buf, cap);
    if (n <= 0 || jsonString(buf, "serverUrl", server, serverCap) <= 0) server[0] = 0;
    if (n <= 0 || jsonString(buf, "authToken", token, tokenCap) < 0) token[0] = 0;
    sceClibMemset(buf, 0, cap);
    int len = (int)sceClibStrnlen(server, serverCap);
    while (len > 0 && server[len - 1] == '/') server[--len] = 0;
    return server[0] != 0;
}

// Where `what` first is in s, or NULL.
const char* findText(const char* s, const char* what) {
    for (; *s; s++)
        if (startsWith(s, what)) return s;
    return NULL;
}

// The value of attribute name in the XML element at el, which ends at its
// '>', with the predefined entities decoded (player.c's xmlAttr, but for
// character references, which a part's key and size do not have). true when
// it is there, whole.
bool xmlAttr(const char* el, const char* name, char* out, int cap) {
    static const char* const kEntities[5][2] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    char pat[40];
    const int plen = sceClibSnprintf(pat, sizeof(pat), " %s=\"", name);
    out[0] = 0;
    const char* p = el;
    while (*p && *p != '>' && !startsWith(p, pat)) p++;
    if (!*p || *p == '>') return false;
    p += plen;
    int n = 0;
    while (*p && *p != '"' && *p != '>' && n < cap - 1) {
        int skip = 0;
        for (int e = 0; *p == '&' && e < 5 && !skip; e++)
            if (startsWith(p, kEntities[e][0])) {
                out[n++] = kEntities[e][1][0];
                skip = (int)sceClibStrnlen(kEntities[e][0], 8);
            }
        if (skip)
            p += skip;
        else
            out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"';
}

// s with all but letters, digits and -_.~ as %XX, into out, cut to fit, as
// curl_easy_escape makes the cover's path in player.c's fetchCover.
void urlEncode(const char* s, char* out, int cap) {
    static const char kHex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (n + (plain ? 1 : 3) > cap - 1) break;
        if (plain) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = kHex[c >> 4];
            out[n++] = kHex[c & 15];
        }
    }
    out[n] = 0;
}

// Bytes as megabytes, "x.yy", and milliseconds as seconds, "x.y", for the
// log: in 32 bits, with no division but by constants.
struct Num {
    char s[16];
};
Num mbOf(uint32_t bytes) {
    Num m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u.%02u", (unsigned)(bytes >> 20),
                    (unsigned)(((bytes & 0xFFFFF) * 100u) >> 20));
    return m;
}
Num secsOf(uint32_t ms) {
    Num m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u.%u", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    return m;
}

// The body of req: into fd a chunk at a time through g_fetchBuf, or with fd
// < 0 into g_fetchBuf, as much as fits, ended with 0. Its first 8 bytes into
// first, if given; a file's progress logged every 10 s under label, if
// given. 0 at the body's end (or with g_fetchBuf full), kFetchStopped at a
// cancel, or the code of the read or write that failed; the bytes in *got.
int fetchBody(int req, SceUID fd, const char* label, uint8_t* first, uint32_t* got) {
    *got = 0;
    SceUInt64 logged = sceKernelGetSystemTimeWide();
    int rc = 0;
    for (;;) {
        if (__atomic_load_n(&g_fetch.cancel, __ATOMIC_SEQ_CST)) {
            rc = kFetchStopped;
            break;
        }
        uint8_t* const to = fd >= 0 ? g_fetchBuf : g_fetchBuf + *got;
        const uint32_t want = fd >= 0 ? sizeof(g_fetchBuf) : sizeof(g_fetchBuf) - 1 - *got;
        if (want == 0) break;
        const int n = httpFn<FnRead>(kHttpRead)(req, to, want);
        if (n <= 0) {
            rc = n;
            break;
        }
        for (uint32_t i = 0; first && i < (uint32_t)n && *got + i < 8; i++) first[*got + i] = to[i];
        if (fd >= 0) {
            const int w = sceIoWrite(fd, to, (SceSize)n);
            if (w != n) {
                rc = w < 0 ? w : -1;
                break;
            }
        }
        *got += (uint32_t)n;
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        if (label && (uint32_t)(now - logged) >= 10000000u) {
            logged = now;
            logLine("carry on: %s: %s MB so far", label, mbOf(*got).s);
        }
    }
    if (fd < 0) g_fetchBuf[*got] = 0;
    return rc;
}

// The song's cover for the row, from the server's photo transcoder as
// player.c's fetchCover asks for it, but as a PNG, which the row can show:
// nothing here could make one of another format. Left out when anything is
// amiss, the row showing its own icon then. 1 when the cover is on the card,
// 0 when it is not, -1 when stopped.
int fetchCover(int tmpl, const char* server, const char* token, const char* label) {
    const Track* const t = &g_fetch.t;
    static char path[160], thumb[400];
    char* const part = g_fetchPart;
    sceClibSnprintf(path, sizeof(path), QM_SONG_COVER_FMT, t->key);
    if (exists(path)) return 1;
    if (!t->thumb[0]) return 0;
    urlEncode(t->thumb, thumb, sizeof(thumb));
    sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl),
                    "%s/photo/:/transcode?width=%d&height=%d&minSize=1&upscale=1&format=png&url=%s",
                    server, QM_ART_SIZE, QM_ART_SIZE, thumb);
    sceClibSnprintf(part, sizeof(g_fetchPart), "%s.part", path);
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        logLine("carry on: %s: no cover: %s not written (0x%08X)", label, part, (unsigned)fd);
        return 0;
    }
    int conn, req, status = 0;
    uint8_t first[8];
    sceClibMemset(first, 0, sizeof(first));
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", "its cover", g_fetchUrl, token, NULL, &conn, &req);
    if (rc >= 0) {
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        if (status == 200) rc = fetchBody(req, fd, NULL, first, &got);
        httpClose(conn, req);
    }
    const int rcClose = sceIoClose(fd);
    static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    bool png = got >= 8;
    for (int i = 0; i < 8; i++) png = png && first[i] == kPng[i];
    if (rc == 0 && status == 200 && png && rcClose >= 0) {
        sceIoRemove(path);
        rc = sceIoRename(part, path);
        if (rc >= 0) {
            logLine("carry on: %s: its cover fetched, %u bytes", label, (unsigned)got);
            return 1;
        }
    }
    sceIoRemove(part);
    if (rc == kFetchStopped) {
        logLine("carry on: %s: no cover: stopped", label);
        return -1;
    } else if (rc < 0 || rcClose < 0) {
        logLine("carry on: %s: no cover: 0x%08X", label, (unsigned)(rc < 0 ? rc : rcClose));
    } else if (status != 200) {
        logLine("carry on: %s: no cover: HTTP %d", label, status);
    } else {
        logLine("carry on: %s: no cover: not a PNG, its first bytes %02X %02X %02X %02X", label,
                first[0], first[1], first[2], first[3]);
    }
    return 0;
}

// The song's file whole from the server's partKey into t's path, through a
// file beside it, moved in once all of it has come. kFetchDone, or
// kFetchFailed with why, and *forGood when another try would fail the same
// way.
int fetchFile(int tmpl, const char* server, const char* token, const char* label,
              const char* partKey, char* why, int cap, int* forGood) {
    Track* const t = &g_fetch.t;
    char* const part = g_fetchPart;
    sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl), "%s%s?download=1", server, partKey);
    sceClibSnprintf(part, sizeof(g_fetchPart), "%s.part", t->path);
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        sceClibSnprintf(why, cap, "%s not written (0x%08X)", part, (unsigned)fd);
        return kFetchFailed;
    }
    const SceUInt64 t0 = sceKernelGetSystemTimeWide();
    int conn, req, status = 0, rcLength = -1;
    unsigned long long length = 0;
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", label, g_fetchUrl, token, NULL, &conn, &req);
    if (rc >= 0) {
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        rcLength = httpFn<FnLength>(kHttpLength)(req, &length);
        if (status == 200) rc = fetchBody(req, fd, label, NULL, &got);
        httpClose(conn, req);
    }
    const int rcClose = sceIoClose(fd);
    // In 32 bits, as the test's: a song comes in well under 71 minutes.
    const uint32_t ms = (uint32_t)(sceKernelGetSystemTimeWide() - t0) / 1000u;
    // The length the server gave, unless it gave none or 4 GB or more.
    const bool sized = rcLength >= 0 && (length >> 32) == 0;
    if (rc == 0 && status == 200 && got > 0 && rcClose >= 0 && (!sized || (uint32_t)length == got)) {
        logLine("carry on: %s: fetched, %s MB in %s s (%u KB/s)", label, mbOf(got).s, secsOf(ms).s,
                (unsigned)((got >> 10) * 1000u / (ms ? ms : 1)));
        sceIoRemove(t->path);
        rc = sceIoRename(part, t->path);
        if (rc >= 0) return kFetchDone;
        sceClibSnprintf(why, cap, "%s not moved in (0x%08X)", part, (unsigned)rc);
    } else if (rc == kFetchStopped) {
        sceClibSnprintf(why, cap, "stopped after %s MB", mbOf(got).s);
    } else if (rc < 0) {
        sceClibSnprintf(why, cap, "its file: 0x%08X after %s MB", (unsigned)rc, mbOf(got).s);
    } else if (status != 200) {
        sceClibSnprintf(why, cap, "its file: HTTP %d", status);
        *forGood = status == 404;
    } else if (rcClose < 0) {
        sceClibSnprintf(why, cap, "%s not written (0x%08X)", part, (unsigned)rcClose);
    } else {
        sceClibSnprintf(why, cap, "%u bytes of its %u came", (unsigned)got,
                        (unsigned)(sized ? length : 0));
    }
    sceIoRemove(part);
    return kFetchFailed;
}

// The fetch thread's work on g_fetch.t, under label: its file found on the
// server as player.c's fetchDetails finds it, the first Part of the Track
// under its key, fetched if the shell plays its format, and then its cover.
// kFetchDone with the file at t.path; else kFetchFailed, as fetchFile.
int fetchSongNow(int tmpl, const char* server, const char* token, const char* label, char* why,
                 int cap, int* forGood) {
    Track* const t = &g_fetch.t;
    static char partKey[256];
    sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl), "%s/library/metadata/%s", server, t->key);
    int conn, req, status = 0;
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", label, g_fetchUrl, token, NULL, &conn, &req);
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the server did not answer (0x%08X)", (unsigned)rc);
        return kFetchFailed;
    }
    httpFn<FnStatus>(kHttpStatus)(req, &status);
    if (status == 200) rc = fetchBody(req, -1, NULL, NULL, &got);
    httpClose(conn, req);
    if (rc == kFetchStopped) {
        sceClibSnprintf(why, cap, "stopped");
        return kFetchFailed;
    }
    if (status != 200 || rc < 0) {
        if (status != 200)
            sceClibSnprintf(why, cap, "its details: HTTP %d", status);
        else
            sceClibSnprintf(why, cap, "its details: 0x%08X", (unsigned)rc);
        *forGood = status == 404;
        return kFetchFailed;
    }
    const char* const track = findText((const char*)g_fetchBuf, "<Track ");
    const char* const next = track ? findText(track + 1, "<Track ") : NULL;
    const char* const p = track ? findText(track, "<Part ") : NULL;
    if (!p || (next && p > next) || !xmlAttr(p, "key", partKey, sizeof(partKey)) || !partKey[0]) {
        sceClibSnprintf(why, cap, "the server has no file of it in its details (%u bytes)",
                        (unsigned)got);
        *forGood = 1;
        return kFetchFailed;
    }
    char num[24], ext[8];
    xmlAttr(p, "size", num, sizeof(num));
    extOf(partKey, ext, sizeof(ext));
    if (!shellExt(ext)) {
        sceClibSnprintf(why, cap, "its file is %s, which the shell does not play",
                        ext[0] ? ext : "of no known format");
        *forGood = 1;
        return kFetchFailed;
    }
    sceClibSnprintf(t->path, sizeof(t->path), "%s/%s.%s", BGPLAYER_SONGS, t->key, ext);
    if (exists(t->path)) {
        logLine("carry on: %s: already on the memory card, at %s", label, t->path);
    } else {
        logLine("carry on: %s: a %s file of %s MB on the server; fetching it to %s", label, ext,
                mbOf(toUint(num)).s, t->path);
        if (fetchFile(tmpl, server, token, label, partKey, why, cap, forGood) != kFetchDone) {
            t->path[0] = 0;
            return kFetchFailed;
        }
    }
    // A song waited for plays as soon as its file is in: its cover, which
    // held one up 0.4 s (log c6d126f7), is a job of its own after it.
    if (__atomic_load_n(&g_fetch.urgent, __ATOMIC_SEQ_CST)) return kFetchDone;
    fetchCover(tmpl, server, token, label);
    g_fetch.coverDone = 1;
    return kFetchDone;
}

// The fetch thread: g_fetch.t fetched, or its cover alone, the outcome left
// in g_fetch for the music thread (fetchPoll).
int fetchThread(SceSize, void*) {
    static char server[256], token[160], label[32], why[96];
    sceClibSnprintf(label, sizeof(label), "song %d of %d", g_fetch.t.index + 1, g_fetch.count);
    why[0] = 0;
    int forGood = 0, result = kFetchFailed;
    bool said = false;  // what came of it logged already
    if (!readServer((char*)g_fetchBuf, sizeof(g_fetchBuf), server, sizeof(server), token,
                    sizeof(token))) {
        sceClibSnprintf(why, sizeof(why), "no server in VitaPlex's settings");
    } else {
        if (!g_netFound) g_netFound = findLibraries("carry on");
        int rcSsl = -1, rcHttp = -1;
        if (!g_netFound) {
            sceClibSnprintf(why, sizeof(why),
                            "Sony's SSL and HTTP libraries are not all there in SceShell");
        } else if (!httpStart("carry on", "fetch", false, &rcSsl, &rcHttp)) {
            sceClibSnprintf(why, sizeof(why), "the HTTP library would not start");
        } else {
            const int tmpl = httpTemplate("carry on");
            if (tmpl < 0) {
                sceClibSnprintf(why, sizeof(why), "no template (0x%08X)", (unsigned)tmpl);
            } else if (g_fetch.kind == kJobCover) {
                const int got = fetchCover(tmpl, server, token, label);
                result = got > 0 ? kFetchDone : kFetchFailed;
                if (got < 0) sceClibSnprintf(why, sizeof(why), "stopped");
                said = true;
                httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
            } else {
                result = fetchSongNow(tmpl, server, token, label, why, sizeof(why), &forGood);
                httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
            }
        }
        httpEnd("carry on", rcSsl, rcHttp);
    }
    sceClibMemset(token, 0, sizeof(token));
    if (result != kFetchDone && !said)
        logLine("carry on: %s: %snot fetched: %s", label,
                g_fetch.kind == kJobCover ? "its cover " : "", why);
    lock();
    g_fetch.state = result;
    g_fetch.forGood = forGood;
    g_fetch.stopped = result != kFetchDone && startsWith(why, "stopped");
    g_fetch.ends++;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// Whether g_fetch is a job of this kind for t. Under g_lock.
bool fetchIsFor(const Track* t, int kind) {
    return g_fetch.state != kFetchIdle && g_fetch.kind == kind && g_fetch.t.index == t->index &&
           same(g_fetch.t.key, t->key);
}

// The song fetched here and not played, removed with its cover, unless the
// shell has it, whose file goes once another has it; forgotten either way.
void dropFetched() {
    if (g_c.fetchedIndex < 0) return;
    if (!same(g_c.fetchedPath, g_c.cur.path)) {
        const int rc = sceIoRemove(g_c.fetchedPath);
        char cover[160];
        sceClibSnprintf(cover, sizeof(cover), QM_SONG_COVER_FMT, g_c.fetchedKey);
        sceIoRemove(cover);
        logLine("carry on: song %d, fetched and not played, removed: 0x%08X",
                g_c.fetchedIndex + 1, (unsigned)rc);
    }
    g_c.fetchedIndex = -1;
    g_c.fetchedPath[0] = 0;
}

// A song's file that g_fetch has fetched, taken into g_c.fetched*, and g_fetch
// left free; one fetched before and not played goes, if it is another file.
void takeFetched() {
    Track t;
    lock();
    const bool took = g_fetch.state == kFetchDone && g_fetch.kind == kJobSong;
    if (took) {
        t = g_fetch.t;
        g_fetch.state = kFetchIdle;
    }
    unlock();
    if (!took) return;
    if (g_c.fetchedIndex >= 0 && !same(g_c.fetchedPath, t.path)) dropFetched();
    g_c.fetchedIndex = t.index;
    sceClibSnprintf(g_c.fetchedKey, sizeof(g_c.fetchedKey), "%s", t.key);
    sceClibSnprintf(g_c.fetchedPath, sizeof(g_c.fetchedPath), "%s", t.path);
}

// t's file as fetched here, into t->path: true when it is on the card. One
// gone from the card since is forgotten.
bool fetchedFor(Track* t) {
    takeFetched();
    if (g_c.fetchedIndex != t->index || !same(g_c.fetchedKey, t->key)) return false;
    if (!exists(g_c.fetchedPath)) {
        g_c.fetchedIndex = -1;
        g_c.fetchedPath[0] = 0;
        return false;
    }
    sceClibSnprintf(t->path, sizeof(t->path), "%s", g_c.fetchedPath);
    t->own = 1;
    return true;
}

// A job for t (kJob*): now, or once the one running has stopped, which is
// stopped if it is another's. A song's file is waited for that way
// (fetchWant); a cover only begins with nothing running (fetchAhead). A song
// fetched before and not played is removed.
void fetchStart(const Track* t, int kind) {
    lock();
    const bool running = g_fetch.state == kFetchRunning;
    const bool forIt = fetchIsFor(t, kind);
    int stopping = -1;
    if (running && !forIt && !g_fetch.cancel) {
        __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
        stopping = g_fetch.t.index;
    }
    const int stoppingKind = g_fetch.kind;
    const bool later = running || g_httpBusy;
    if (later && kind == kJobSong && !(running && forIt && !g_fetch.cancel))
        g_c.fetchWant = t->index;
    unlock();
    if (stopping >= 0)
        logLine("carry on: song %d's %s stopped: song %d of %d is wanted first", stopping + 1,
                stoppingKind == kJobCover ? "cover's fetch" : "fetch", t->index + 1, g_c.count);
    if (later) return;
    takeFetched();
    if (kind == kJobSong && g_c.fetchedIndex >= 0 && g_c.fetchedIndex != t->index) dropFetched();
    lock();
    g_fetch.state = kFetchRunning;
    g_fetch.kind = kind;
    g_fetch.cancel = 0;
    g_fetch.urgent = 0;
    g_fetch.coverDone = 0;
    g_fetch.forGood = 0;
    g_fetch.stopped = 0;
    g_fetch.count = g_c.count;
    g_fetch.t = *t;
    g_fetch.t.path[0] = 0;
    unlock();
    if (kind == kJobSong) {
        g_c.fetchWant = -1;
        logLine("carry on: song %d of %d, \"%s\", is not on the memory card: fetching it",
                t->index + 1, g_c.count, t->title);
    } else {
        logLine("carry on: song %d of %d: fetching its cover for the row", t->index + 1,
                g_c.count);
    }
    const SceUID thread = sceKernelCreateThread("VitaPlexQM::Fetch", fetchThread, kThreadPriority,
                                                kFetchStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("carry on: song %d of %d: no thread to fetch it (0x%08X)", t->index + 1, g_c.count,
                (unsigned)rc);
        lock();
        g_fetch.state = kFetchFailed;
        g_fetch.ends++;
        unlock();
    }
}

// The song the queue goes on to after the one with the shell, into out: the
// nearest within kLookAhead on the card, or that the server may have a file
// of for the shell. Its place, with *card saying which; -1 with none.
int nextSong(Track* out, bool* card) {
    for (int d = 1; d <= kLookAhead; d++) {
        const int i = stepFrom(g_c.cur.index, d);
        if (i < 0 || i == g_c.cur.index || !trackAt(i, out)) return -1;
        *card = onCard(out);
        if (*card || !ringHas(&g_c.lost, i)) return i;
    }
    return -1;
}

// Whether t's cover for the row is yet to be fetched: on the server, not on
// the card, and not asked for before.
bool wantsCover(const Track* t) {
    if (!t->thumb[0] || !t->key[0] || ringHas(&g_c.coverTried, t->index)) return false;
    char cover[160];
    sceClibSnprintf(cover, sizeof(cover), QM_SONG_COVER_FMT, t->key);
    return !exists(cover);
}

// What to fetch while the song with the shell plays, if anything: the next
// song's file when it is not on the card, unless it failed ahead already
// (it is fetched again, waited for, when it is next); else, with nothing
// running, the covers the row is to show, this song's and then the next
// one's. Nothing while waiting, nor a song ahead for repeat one.
void fetchAhead() {
    g_c.aheadDue = 0;
    if (!g_server[0] || g_c.waitIndex >= 0) return;
    lock();
    const bool busy = g_httpBusy != 0;
    unlock();
    if (busy) {
        g_c.aheadDue = 1;  // once the test is over
        return;
    }
    bool card = false;
    const int next = g_c.repeat == BGPLAYER_REPEAT_ONE ? -1 : nextSong(&g_ahead, &card);
    takeFetched();
    lock();
    const bool running = g_fetch.state == kFetchRunning;
    const bool inHand =
        next >= 0 && ((g_c.fetchedIndex == next && same(g_c.fetchedKey, g_ahead.key)) ||
                      (running && fetchIsFor(&g_ahead, kJobSong)));
    unlock();
    if (next >= 0 && !card && !inHand && !ringHas(&g_c.failedAhead, next)) {
        fetchStart(&g_ahead, kJobSong);
        return;
    }
    if (running) return;
    if (wantsCover(&g_c.cur))
        fetchStart(&g_c.cur, kJobCover);
    else if (next >= 0 && card && wantsCover(&g_ahead))
        fetchStart(&g_ahead, kJobCover);
}

// Each look: a fetch that has ended seen to, the fetch wanted next begun, and
// the next song fetched ahead when it is due.
void fetchPoll() {
    lock();
    const int state = g_fetch.state, ends = g_fetch.ends, index = g_fetch.t.index;
    const int kind = g_fetch.kind, forGood = g_fetch.forGood, stopped = g_fetch.stopped;
    const int coverDone = g_fetch.coverDone;
    unlock();
    if (ends != g_c.fetchEnds) {
        g_c.fetchEnds = ends;
        if (kind == kJobSong && state == kFetchFailed) {
            if (forGood)
                ringAdd(&g_c.lost, index);
            else if (!stopped)
                ringAdd(&g_c.failedAhead, index);
        }
        // A song's file fetched comes with its cover, or with its cover's
        // failure, unless it was waited for: not asked for again.
        if (kind == kJobSong && state == kFetchDone) {
            if (coverDone) ringAdd(&g_c.coverTried, index);
            takeFetched();
        }
        if (kind == kJobCover && !stopped) {
            ringAdd(&g_c.coverTried, index);
            if (state == kFetchDone && index == g_c.cur.index && g_c.waitIndex < 0)
                showCover(&g_c.cur);
        }
        g_c.aheadDue = 1;  // on to what is to be fetched next
    }
    if (state != kFetchRunning && g_c.fetchWant >= 0) {
        const int want = g_c.fetchWant;
        g_c.fetchWant = -1;
        if (trackAt(want, &g_ahead)) fetchStart(&g_ahead, kJobSong);
    }
    if (g_c.aheadDue) fetchAhead();
}

// t, not on the card, fetched and then played (carryWait), the shell stopped
// meanwhile; why is why it is next. The row shows it, and the status names it
// with "fetching=1", so that VitaPlex BG takes the music back from its start.
void waitFor(const Track* t, const char* why) {
    const int rcStop = musicEvent(kEventStop);
    g_c.wait = *t;
    g_c.waitIndex = t->index;
    g_c.waitUs = sceKernelGetSystemTimeWide();
    g_c.waitTries = 0;
    g_c.waitWhy = why;
    g_c.waitPlays = 0;
    g_c.menuPaused = 0;
    g_c.resuming = 0;
    logLine("carry on: song %d of %d (%s), \"%s\", is not on the memory card: the shell stopped "
            "(0x%08X) until it is fetched",
            t->index + 1, g_c.count, why, t->title, (unsigned)rcStop);
    showTrack(t);
    carryStatusOf(t, BGPLAYER_STATE_PLAYING, startAt(t), true);
    lock();
    const bool under = g_fetch.state == kFetchRunning && fetchIsFor(t, kJobSong) && !g_fetch.cancel;
    unlock();
    if (!under) {
        g_c.waitTries = 1;
        fetchStart(t, kJobSong);
    }
}

// ── Marking songs played ────────────────────────────────────────────────────
// A song that played to its end is marked played on the server, as the
// background player marks it (player.c's scrobble). On a thread of its own,
// which sends what is waiting and ends, so that a slow server holds nothing
// up: not the music, nor the next song's fetch. Through the libraries the
// fetch finds, with the token read for each run and wiped after it.
const int kScrobbles = 8;
struct Scrobble {
    char key[16];  // the song's ratingKey
    int index;     // its place, for the log
};
// Under g_lock: the songs waiting to be marked, from g_scrobbleFirst on, and
// whether a thread is marking them.
Scrobble g_scrobbles[kScrobbles];
int g_scrobbleFirst, g_scrobbleCount, g_scrobbling;
char g_scrobbleSettings[2048];  // the scrobble thread's

// The scrobble thread's: each song waiting marked, until none is; each said
// not to be when the server or the libraries are not there.
void scrobbleWaiting() {
    static char server[256], token[160], url[320];
    int rcSsl = -1, rcHttp = -1, tmpl = -1;
    if (readServer(g_scrobbleSettings, sizeof(g_scrobbleSettings), server, sizeof(server), token,
                   sizeof(token))) {
        if (!g_netFound) g_netFound = findLibraries("carry on");
        if (g_netFound && httpStart("carry on", "marking songs played", false, &rcSsl, &rcHttp))
            tmpl = httpTemplate("carry on");
    }
    for (;;) {
        Scrobble one;
        lock();
        const bool any = g_scrobbleCount > 0;
        if (any) {
            one = g_scrobbles[g_scrobbleFirst];
            g_scrobbleFirst = (g_scrobbleFirst + 1) % kScrobbles;
            g_scrobbleCount--;
        }
        unlock();
        if (!any) break;
        if (tmpl < 0) {
            logLine("carry on: song %d not marked played: %s", one.index + 1,
                    server[0] ? "no HTTP library" : "no server in VitaPlex's settings");
            continue;
        }
        sceClibSnprintf(url, sizeof(url),
                        "%s/:/scrobble?key=%s&identifier=com.plexapp.plugins.library", server,
                        one.key);
        int conn, req, status = 0;
        const int rc = httpOpen(tmpl, "carry on", "marking a song played", url, token, NULL,
                                &conn, &req);
        if (rc >= 0) {
            httpFn<FnStatus>(kHttpStatus)(req, &status);
            httpClose(conn, req);
        }
        if (status == 200)
            logLine("carry on: song %d marked played", one.index + 1);
        else
            logLine("carry on: song %d not marked played: HTTP %d (0x%08X)", one.index + 1, status,
                    (unsigned)rc);
    }
    if (tmpl >= 0) httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    httpEnd("carry on", rcSsl, rcHttp);
    sceClibMemset(token, 0, sizeof(token));
}

int scrobbleThread(SceSize, void*) {
    for (;;) {
        scrobbleWaiting();
        // Ended only with none left waiting, under the lock that a song
        // added since would have been put in under.
        lock();
        const bool more = g_scrobbleCount > 0;
        if (!more) g_scrobbling = 0;
        unlock();
        if (!more) break;
    }
    return sceKernelExitDeleteThread(0);
}

// t marked played on the server, now or after those waiting. The music
// thread's.
void scrobbleSong(const Track* t) {
    if (!g_server[0] || !t->key[0]) return;
    lock();
    const bool full = g_scrobbleCount == kScrobbles;
    bool start = false;
    if (!full) {
        Scrobble* const one = &g_scrobbles[(g_scrobbleFirst + g_scrobbleCount) % kScrobbles];
        sceClibSnprintf(one->key, sizeof(one->key), "%s", t->key);
        one->index = t->index;
        g_scrobbleCount++;
        start = !g_scrobbling;
        if (start) g_scrobbling = 1;
    }
    unlock();
    if (full) {
        logLine("carry on: song %d not marked played: %d songs are waiting to be already",
                t->index + 1, kScrobbles);
        return;
    }
    if (!start) return;
    const SceUID thread = sceKernelCreateThread("VitaPlexQM::Played", scrobbleThread,
                                                kThreadPriority, kFetchStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        lock();
        const int dropped = g_scrobbleCount;
        g_scrobbleCount = 0;
        g_scrobbling = 0;
        unlock();
        logLine("carry on: %d song(s) not marked played: no thread for it (0x%08X)", dropped,
                (unsigned)rc);
    }
}

// Stops carrying the queue on (kEnd*). The row goes back to SceShell unless a
// process has taken it. A fetch under way is stopped, and a song fetched and
// not played removed.
void carryEnd(const char* why, int how) {
    int state = 0;
    uint32_t time = 0;
    musicStatus(&state, &time);
    // The queue has gone on to a song being fetched: the music plays, as far
    // as VitaPlex BG is to know, from that song's start.
    const bool waiting = g_c.waitIndex >= 0;
    const char* written = "nothing";
    if (how == kEndTakeBack) {
        written = state == 1 || waiting ? BGPLAYER_STATE_PLAYING : BGPLAYER_STATE_STOPPED;
    } else if (how == kEndOther || how == kEndStopped) {
        written = BGPLAYER_STATE_STOPPED;
    } else if (how == kEndOver) {
        written = BGPLAYER_STATE_OVER;
    }
    if (how != kEndYield) {
        if (waiting)
            carryStatusOf(&g_c.wait, written, startAt(&g_c.wait), true);
        else
            carryStatus(written, time);
    }
    lock();
    const bool fetching = g_fetch.state == kFetchRunning;
    if (fetching) __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
    unlock();
    dropFetched();
    int rcStop = 0, rcEnd = 0;
    if (how == kEndOver || how == kEndStopped) {
        rcStop = musicEvent(kEventStop);
        rcEnd = musicTracked(0x30001);
        removeOwn(&g_c.cur);
    }
    char also[64] = "";
    if (waiting)
        sceClibSnprintf(also, sizeof(also), ", song %d to come once fetched", g_c.waitIndex + 1);
    logLine("carry on: stopped at song %d of %d, %s, state %d%s (%s): status %s; stop 0x%08X, "
            "end 0x%08X%s",
            g_c.cur.index + 1, g_c.count, mmss(time).s, state, also, why, written,
            (unsigned)rcStop, (unsigned)rcEnd, fetching ? "; its fetch stopped" : "");
    g_c.waitIndex = -1;
    lock();
    g_carry = 0;
    g_carryStop = NULL;
    if (g_owner == kOwnerShell) {
        g_active = 0;
        g_releaseDue = 1;
        g_artSerial = 0;
        g_playing = -1;
        logLine("carry on: row given back");
    }
    unlock();
}

// Nothing carried on after all: the row goes back, as it would have without
// SceShell carrying anything on.
void carryNone() {
    lock();
    g_carry = 0;
    g_carryStop = NULL;
    if (g_owner == kOwnerShell) {
        g_active = 0;
        g_releaseDue = 1;
        g_artSerial = 0;
        g_playing = -1;
        logLine("carry on: nothing carried on; row given back");
    }
    unlock();
}

// The carry-on's state as one begins: nothing waited for, wanted or
// remembered, and a fetch from a queue carried on before forgotten, or left
// to stop.
void carryReset() {
    sceClibMemset(&g_c, 0, sizeof(g_c));
    g_c.waitIndex = -1;
    g_c.fetchedIndex = -1;
    g_c.fetchWant = -1;
    g_c.startIndex = -1;
    lock();
    if (g_fetch.state != kFetchRunning) g_fetch.state = kFetchIdle;
    g_c.fetchEnds = g_fetch.ends;
    unlock();
}

// Songs not on the card are fetched from VitaPlex's server, if its settings
// have one; the token is only looked for here.
void carryServer() {
    static char settings[2048];
    char token[160];
    readServer(settings, sizeof(settings), g_server, sizeof(g_server), token, sizeof(token));
    const bool signedIn = token[0] != 0;
    sceClibMemset(token, 0, sizeof(token));
    if (g_server[0])
        logLine("carry on: songs not on the memory card are fetched from %s, %s", g_server,
                signedIn ? "signed in" : "with no token");
    else
        logLine("carry on: no server in VitaPlex's settings: songs not on the memory card are "
                "passed over");
}

// Begins carrying on, from the player's status and the queue. true when it
// does; else nothing has changed.
bool carryStart() {
    static char text[1024];
    const int n = readFile(BGPLAYER_STATUS, text, sizeof(text));
    if (n <= 0) {
        logLine("carry on: no status from the player (0x%08X)", (unsigned)n);
        return false;
    }
    char state[16], statusQueue[32], key[16], num[16];
    noteValue(text, "state", state, sizeof(state));
    noteValue(text, "queue", statusQueue, sizeof(statusQueue));
    noteValue(text, "ratingKey", key, sizeof(key));
    noteValue(text, "index", num, sizeof(num));
    const int index = toInt(num);
    if (!same(state, BGPLAYER_STATE_PLAYING)) {
        logLine("carry on: the player's status says \"%s\": nothing to carry on", state);
        return false;
    }
    QueueScan s;
    if (!scanOpen(&s)) {
        logLine("carry on: no queue at %s", BGPLAYER_QUEUE);
        return false;
    }
    carryReset();
    const char* fields;
    bool have = false;
    while (scanNext(&s, &fields)) {
        if (s.index == index) {
            parseTrack(fields, index, &g_c.cur);
            have = true;
        }
    }
    readerClose();
    g_c.count = s.index + 1;
    g_c.repeat = s.repeat;
    sceClibSnprintf(g_c.queue, sizeof(g_c.queue), "%s", s.written);
    if (!s.whole || !sameTime(s.written, statusQueue)) {
        logLine("carry on: the queue (%s, %s) is not the one the player plays (%s)", s.written,
                s.whole ? "whole" : "not whole", statusQueue);
        return false;
    }
    if (!have || !same(g_c.cur.key, key)) {
        logLine("carry on: song %d of the queue is not the player's song %s", index + 1, key);
        return false;
    }
    // The song plays on from the player's file of it; removed once it has
    // played, as the player would.
    if (!onCard(&g_c.cur)) g_c.cur.path[0] = 0;
    if (!findSvcObj("carry on") || !g_getSvcObj()) {
        logLine("carry on: no client for SceShell's music service");
        return false;
    }
    const int rcStart = musicTracked(0x30000);
    int shellState = 0;
    uint32_t time = 0, len = 0;
    const int rcStatus = musicStatus(&shellState, &time);
    const int rcLen = musicLength(&len);
    logLine("carry on: song %d of %d, \"%s\" by %s, %s, from %s; music service started "
            "(0x%08X): at %s, state %d, a song %s long (0x%08X, 0x%08X); repeat %d",
            index + 1, g_c.count, g_c.cur.title, g_c.cur.artist, mmss(g_c.cur.durationMs).s,
            g_c.cur.path[0] ? g_c.cur.path : "a file not known here", (unsigned)rcStart,
            mmss(time).s, shellState, mmss(len).s, (unsigned)rcStatus, (unsigned)rcLen,
            g_c.repeat);
    if (rcStart < 0 || rcStatus < 0) return false;
    if (!hasSong(len, &g_c.cur, 0)) {
        // Ended between handing the shell its next song and saying so: the
        // status still names the song before (player.c writes it once the
        // new one plays).
        const int next = stepFrom(index, 1);
        if (next >= 0 && trackAt(next, &g_found) && hasSong(len, &g_found, 0)) {
            logLine("carry on: the shell has song %d of %d, \"%s\", the one after the status's",
                    next + 1, g_c.count, g_found.title);
            g_c.cur = g_found;
            if (!onCard(&g_c.cur)) g_c.cur.path[0] = 0;
            showTrack(&g_c.cur);
        } else {
            logLine("carry on: the shell has another song: nothing carried on");
            return false;
        }
    }
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    g_c.started = 1;
    g_c.maxTime = time;
    g_c.playMs = time;
    g_c.lenMs = len;
    g_c.songUs = now;
    g_c.checkUs = now;
    g_c.lastState = shellState;
    lock();
    // Paused from the row as the player went, it stays paused.
    g_c.menuPaused = g_playing == 0;
    unlock();
    carryStatus(BGPLAYER_STATE_PLAYING, time);
    carryServer();
    g_c.aheadDue = 1;
    return true;
}

// Hands t to the shell: stop, open and play, as the player does. The song
// before's file goes once the shell has this one. true when it took the file.
bool carryPlay(const Track* t, const char* why) {
    const int rcStop = musicEvent(kEventStop), rcOpen = musicOpen(t->path),
              rcPlay = musicEvent(kEventPlay);
    logLine("carry on: song %d of %d (%s), \"%s\" by %s, %s, %s: stop 0x%08X, open 0x%08X, "
            "play 0x%08X",
            t->index + 1, g_c.count, why, t->title, t->artist, mmss(t->durationMs).s, t->path,
            (unsigned)rcStop, (unsigned)rcOpen, (unsigned)rcPlay);
    if (rcOpen < 0) return false;
    if (!same(g_c.cur.path, t->path)) removeOwn(&g_c.cur);
    g_c.lastLenMs = g_c.lenMs;
    g_c.cur = *t;
    g_c.started = 0;
    g_c.menuPaused = 0;
    g_c.resuming = 0;
    g_c.havePrev = 0;
    g_c.maxTime = 0;
    g_c.playMs = 0;
    g_c.lenMs = 0;
    g_c.replays = 0;
    g_c.otherLogged = 0;
    g_c.waitTicks = 0;
    g_c.stoppedUs = 0;
    g_c.songUs = sceKernelGetSystemTimeWide();
    g_c.waitIndex = -1;
    g_c.aheadDue = 1;  // the song after it fetched, if it is not on the card
    g_c.seekWantMs = 0;
    g_c.seekTries = 0;
    // VitaPlex BG's place is for its own song alone.
    if (t->index != g_c.startIndex) g_c.startAtMs = 0;
    // A fetched song, played: its file is the shell's now, removed as the
    // player's own are.
    takeFetched();
    if (g_c.fetchedIndex >= 0 && same(g_c.fetchedPath, t->path)) {
        g_c.fetchedIndex = -1;
        g_c.fetchedPath[0] = 0;
    }
    showTrack(t);
    carryStatus(BGPLAYER_STATE_PLAYING, startAt(t));
    return true;
}

// What carryNext goes on to (nextFrom).
enum { kGoNone, kGoPlay, kGoFetch };

// The song the queue goes on to after `from`, into g_found, within
// kLookAhead songs: the nearest on the card or fetched here (kGoPlay), else
// the nearest to fetch and wait for (kGoFetch). Passed over: songs not on the
// card with no server to fetch them from, and songs the server has no file
// of for the shell.
int nextFrom(int from) {
    for (int d = 1; d <= kLookAhead; d++) {
        const int i = stepFrom(from, d);
        if (i < 0 || i == from || !trackAt(i, &g_found)) return kGoNone;
        if (onCard(&g_found) || fetchedFor(&g_found)) return kGoPlay;
        if (g_server[0] && !ringHas(&g_c.lost, i)) return kGoFetch;
        logLine("carry on: song %d of %d, \"%s\", is not on the memory card%s: passed over", i + 1,
                g_c.count, g_found.title,
                g_server[0] ? ", and the server has no file of it the shell plays" : "");
    }
    return kGoNone;
}

// The song after `from` as the queue goes on: the nearest on the card or
// fetched, or one to fetch, which is waited for; the same one again for
// repeat one as a song ends. Up to three that will not open are passed over.
void carryNext(int from, bool ended, const char* why) {
    g_c.waitIndex = -1;
    for (int tries = 0; tries < 3; tries++) {
        int go;
        if (ended && g_c.repeat == BGPLAYER_REPEAT_ONE && g_c.cur.path[0] && tries == 0) {
            g_found = g_c.cur;
            go = kGoPlay;
        } else {
            go = nextFrom(from);
        }
        if (go == kGoNone) {
            const bool over = stepFrom(from, 1) < 0;
            carryEnd(over        ? "the queue is over"
                     : g_server[0] ? "no further song is on the memory card or on the server"
                                   : "no further song is on the memory card",
                     over ? kEndOver : kEndStopped);
            return;
        }
        if (go == kGoFetch) {
            waitFor(&g_found, why);
            return;
        }
        if (carryPlay(&g_found, why)) return;
        from = g_found.index;
    }
    carryEnd("three songs in a row would not open", kEndStopped);
}

// VitaPlex BG's queue, handed straight to SceShell as it goes
// (QM_CMD_PLAY_QUEUE): played from its song and place there, as the
// background player would begin it, and the row SceShell's from now. That
// song plays from the file VitaPlex BG made ready, else from its download or
// the player's file of it, else from the server, waited for; and if it can
// be none of them, the next that can is played from its start. true when it
// carries on (g_carry is 2 then, or 0 again if the queue had nothing to
// play); else nothing has changed but the row's owner.
bool carryStartQueue() {
    QueueScan s;
    if (!scanOpen(&s)) {
        logLine("play queue: no queue at %s", BGPLAYER_QUEUE);
        return false;
    }
    carryReset();
    const char* fields;
    bool have = false;
    while (scanNext(&s, &fields)) {
        if (s.index == s.start) {
            parseTrack(fields, s.index, &g_c.cur);
            have = true;
        }
    }
    readerClose();
    g_c.count = s.index + 1;
    g_c.repeat = s.repeat;
    sceClibSnprintf(g_c.queue, sizeof(g_c.queue), "%s", s.written);
    if (!s.whole || !have) {
        logLine("play queue: the queue (%s) is %s", s.written,
                !s.whole ? "not whole" : "without the song it names to begin with");
        return false;
    }
    if (!findSvcObj("play queue") || !g_getSvcObj()) {
        logLine("play queue: no client for SceShell's music service");
        return false;
    }
    const int rcStart = musicTracked(0x30000);
    logLine("play queue: song %d of %d, \"%s\" by %s, %s, from %s; repeat %d; music service "
            "started: 0x%08X",
            s.start + 1, g_c.count, g_c.cur.title, g_c.cur.artist, mmss(s.startMs).s,
            s.file[0] ? s.file : "the queue", g_c.repeat, (unsigned)rcStart);
    if (rcStart < 0) return false;
    carryServer();
    g_c.startIndex = s.start;
    g_c.startAtMs = s.startMs;
    lock();
    g_owner = kOwnerShell;
    g_active = 1;
    g_carry = 2;
    unlock();
    // Nothing is with the shell yet: the song is played from here.
    g_found = g_c.cur;
    g_c.cur.path[0] = 0;
    g_c.cur.own = 0;
    if (s.file[0] && exists(s.file)) {
        sceClibSnprintf(g_found.path, sizeof(g_found.path), "%s", s.file);
        g_found.own = startsWith(s.file, BGPLAYER_SONGS "/");
    } else if (!onCard(&g_found)) {
        g_found.path[0] = 0;
    }
    if (g_found.path[0]) {
        if (!carryPlay(&g_found, "VitaPlex BG's song"))
            carryNext(s.start, false, "VitaPlex BG's song would not open");
    } else if (g_server[0]) {
        waitFor(&g_found, "VitaPlex BG's song");
    } else {
        logLine("play queue: song %d of %d, \"%s\", is not on the memory card, and there is no "
                "server to fetch it from",
                s.start + 1, g_c.count, g_found.title);
        carryNext(s.start, false, "VitaPlex BG's song is not on the memory card");
    }
    g_c.aheadDue = 1;
    return true;
}

// Takes the song to ms, VitaPlex BG's place in it: the seek time, then the
// seek event, as player.c's seekFrom; carryTick watches where it lands.
void seekTo(uint32_t ms) {
    const int rcTime = musicSeekTime(ms), rcSeek = musicEvent(kEventSeek);
    logLine("carry on: song %d of %d to %s, where VitaPlex BG was: seek time 0x%08X, seek 0x%08X",
            g_c.cur.index + 1, g_c.count, mmss(ms).s, (unsigned)rcTime, (unsigned)rcSeek);
    g_c.seekWantMs = ms;
    g_c.seekUs = sceKernelGetSystemTimeWide();
    g_c.seekTries++;
    g_c.maxTime = 0;
    g_c.playMs = ms;
    g_c.havePrev = 0;
}

// Takes the song back to its start: the seek time, then the seek event, as
// player.c's seekFrom does first.
void seekStart(const char* why) {
    const int rcTime = musicSeekTime(0), rcSeek = musicEvent(kEventSeek);
    logLine("carry on: song %d back to its start (%s): seek time 0x%08X, seek 0x%08X",
            g_c.cur.index + 1, why, (unsigned)rcTime, (unsigned)rcSeek);
    g_c.maxTime = 0;
    g_c.playMs = 0;
    g_c.havePrev = 0;
}

// A press on the row, as player.c's watchSong takes them: play/pause stops
// the shell and plays it again, which keeps its place; previous goes back to
// the start past a song's first 3 s, and to the song before within them,
// fetched first if it is not on the card; next goes on past repeat one.
void carryPress(int press, int state, uint32_t time) {
    if (press == QM_PRESS_NEXT) {
        logLine("carry on: next, from the row, at %s", mmss(time).s);
        carryNext(g_c.cur.index, false, "next, from the row");
        return;
    }
    if (press == QM_PRESS_PREVIOUS) {
        if (g_c.started && time >= 3000) {
            g_c.menuPaused = 0;
            seekStart("previous, from the row");
        } else {
            logLine("carry on: previous, from the row, at %s", mmss(time).s);
            const int before = stepFrom(g_c.cur.index, -1);
            if (before >= 0 && trackAt(before, &g_found)) {
                if (onCard(&g_found) || fetchedFor(&g_found)) {
                    if (carryPlay(&g_found, "previous, from the row")) return;
                } else if (g_server[0] && !ringHas(&g_c.lost, before)) {
                    waitFor(&g_found, "previous, from the row");
                    return;
                }
            }
            seekStart("previous, from the row, with the song before not on the card");
        }
        return;
    }
    int rc;
    if (g_c.menuPaused || state != 1) {
        g_c.menuPaused = 0;
        g_c.resuming = 1;
        rc = musicEvent(kEventPlay);
        lock();
        g_playing = 1;
        unlock();
    } else {
        g_c.menuPaused = 1;
        rc = musicEvent(kEventStop);
        lock();
        g_playing = 0;
        unlock();
    }
    logLine("carry on: %s at %s, from the row: 0x%08X", g_c.menuPaused ? "paused" : "play",
            mmss(time).s, (unsigned)rc);
}

// Each look while a song is fetched to play next: it is played once fetched,
// tried again once if its fetch fails, and else passed over. The row's
// buttons act on it: play/pause holds it back once fetched, previous goes
// back to the song before from its start, next passes it over.
void carryWait() {
    const int press = __atomic_exchange_n(&g_carryPress, 0, __ATOMIC_SEQ_CST);
    if (press == QM_PRESS_NEXT) {
        logLine("carry on: next, from the row, as song %d is fetched", g_c.waitIndex + 1);
        lock();
        if (g_fetch.state == kFetchRunning && fetchIsFor(&g_c.wait, kJobSong))
            __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
        unlock();
        carryNext(g_c.waitIndex, false, "next, from the row");
        return;
    }
    if (press == QM_PRESS_PREVIOUS) {
        logLine("carry on: previous, from the row, as song %d is fetched", g_c.waitIndex + 1);
        if (g_c.cur.path[0] && exists(g_c.cur.path)) {
            g_found = g_c.cur;
            if (carryPlay(&g_found, "previous, from the row")) return;
        }
    } else if (press == QM_PRESS_PLAY_PAUSE) {
        g_c.menuPaused = !g_c.menuPaused;
        lock();
        g_playing = g_c.menuPaused ? 0 : 1;
        unlock();
        logLine("carry on: %s as song %d is fetched, from the row",
                g_c.menuPaused ? "paused" : "play", g_c.waitIndex + 1);
    }
    // Something else played in the shell meanwhile, as the system's Music
    // app would play it: not for this to stop.
    int state = 0;
    uint32_t time = 0;
    if (musicStatus(&state, &time) >= 0 && state == 1) {
        if (++g_c.waitPlays >= 2) {
            carryEnd("something else plays in the shell", kEndOther);
            return;
        }
    } else {
        g_c.waitPlays = 0;
    }
    lock();
    // Waited for: its cover is left for after it plays.
    if (g_fetch.state == kFetchRunning && fetchIsFor(&g_c.wait, kJobSong))
        __atomic_store_n(&g_fetch.urgent, 1, __ATOMIC_SEQ_CST);
    unlock();
    g_found = g_c.wait;
    if (fetchedFor(&g_found)) {
        if (g_c.menuPaused) return;
        logLine("carry on: song %d of %d fetched, after %s s of waiting", g_found.index + 1,
                g_c.count, secsOf((uint32_t)(sceKernelGetSystemTimeWide() - g_c.waitUs) / 1000u).s);
        if (!carryPlay(&g_found, g_c.waitWhy))
            carryNext(g_found.index, false, "the one before would not open");
        return;
    }
    lock();
    const int fetchState = g_fetch.state;
    const bool forIt = fetchIsFor(&g_c.wait, kJobSong);
    const bool forGood = forIt && fetchState == kFetchFailed && g_fetch.forGood;
    unlock();
    // Its fetch under way, or to begin once another has stopped.
    if ((forIt && fetchState == kFetchRunning) || g_c.fetchWant == g_c.waitIndex) return;
    // Failed, or its file gone since it came: fetched again once, then
    // passed over.
    if (!forGood && g_c.waitTries < 2) {
        g_c.waitTries++;
        logLine("carry on: song %d of %d: fetched again", g_c.waitIndex + 1, g_c.count);
        fetchStart(&g_c.wait, kJobSong);
        return;
    }
    logLine("carry on: song %d of %d, \"%s\", could not be fetched: passed over",
            g_c.waitIndex + 1, g_c.count, g_c.wait.title);
    if (!forGood && ++g_c.failures >= 3) {
        carryEnd("three songs in a row could not be fetched", kEndStopped);
        return;
    }
    carryNext(g_c.waitIndex, false, g_c.waitWhy);
}

// Each half second while it carries on, as player.c's watchSong does each.
void carryTick() {
    lock();
    const char* stop = g_carryStop;
    const int how = g_carryStopHow;
    unlock();
    if (stop) {
        carryEnd(stop, how);
        return;
    }
    fetchPoll();
    if (g_c.waitIndex >= 0) {
        carryWait();
        return;
    }
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    int state = 0;
    uint32_t time = 0;
    const int rc = musicStatus(&state, &time);
    if (rc < 0) {
        if (g_statusReadErrors < 6) {
            g_statusReadErrors++;
            logLine("carry on: the music service's status: 0x%08X", (unsigned)rc);
        }
        return;
    }
    if (state != g_c.lastState) {
        logLine("carry on: song %d of %d: state %d at %s", g_c.cur.index + 1, g_c.count, state,
                mmss(time).s);
        g_c.lastState = state;
    }
    const int press = __atomic_exchange_n(&g_carryPress, 0, __ATOMIC_SEQ_CST);
    if (press) {
        carryPress(press, state, time);
        return;
    }
    if (!g_c.started) {
        if (state == 1 && g_c.havePrev && time > g_c.prevTime) {
            uint32_t len = 0;
            musicLength(&len);
            if (hasSong(len, &g_c.cur, g_c.lastLenMs)) {
                g_c.started = 1;
                g_c.failures = 0;
                g_c.maxTime = time;
                g_c.lenMs = len;
                logLine("carry on: song %d of %d plays: the shell has it as %s long, the server "
                        "%s, and is at %s",
                        g_c.cur.index + 1, g_c.count, mmss(len).s, mmss(g_c.cur.durationMs).s,
                        mmss(time).s);
                // To VitaPlex BG's place in the song it handed over; else
                // from the start, as the shell can start a file it has had
                // before where it left it (player.c's fromTheStart).
                const uint32_t at = startAt(&g_c.cur);
                g_c.startAtMs = 0;
                if (at >= 2000)
                    seekTo(at);
                else if (time >= 5000)
                    seekStart("the shell started it part-way");
            } else if (!g_c.otherLogged) {
                g_c.otherLogged = 1;
                logLine("carry on: song %d of %d: the shell plays something %s long, not this "
                        "song of %s",
                        g_c.cur.index + 1, g_c.count, mmss(len).s, mmss(g_c.cur.durationMs).s);
            }
        }
        if (!g_c.started && ++g_c.waitTicks > 30) {
            logLine("carry on: song %d of %d: the shell never started it", g_c.cur.index + 1,
                    g_c.count);
            if (++g_c.failures >= 3)
                carryEnd("three songs in a row would not play", kEndStopped);
            else
                carryNext(g_c.cur.index, false, "the one before would not play");
            return;
        }
    }
    g_c.prevTime = time;
    g_c.havePrev = 1;
    if (g_c.started) {
        // Playing again after a stop or the row's play: not its end, wherever
        // it went on from (below), nor is the stop before the shell plays
        // again after the row's play.
        const bool back = state == 1 && (g_c.stoppedUs || g_c.resuming);
        if (g_c.seekWantMs) {
            // Near enough: an M4A download's seek landed 2 to 4 s short (log
            // f3042ca3, build 2035).
            const uint32_t want = g_c.seekWantMs;
            const uint32_t since = (uint32_t)(now - g_c.seekUs) / 1000u;
            if (time + 5000 >= want && time <= want + 6000) {
                logLine("carry on: song %d of %d at %s, %s s after the seek to %s",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, secsOf(since).s,
                        mmss(want).s);
                g_c.seekWantMs = 0;
            } else if (since >= 4000) {
                if (g_c.seekTries < 2) {
                    logLine("carry on: song %d of %d at %s, not %s, 4 s after the seek: again",
                            g_c.cur.index + 1, g_c.count, mmss(time).s, mmss(want).s);
                    seekTo(want);
                } else {
                    logLine("carry on: the seeks did not take song %d of %d to %s; it goes on "
                            "from %s",
                            g_c.cur.index + 1, g_c.count, mmss(want).s, mmss(time).s);
                    g_c.seekWantMs = 0;
                }
            }
        }
        if (time > g_c.maxTime) g_c.maxTime = time;
        const uint32_t lenMs = g_c.cur.durationMs > 0 ? g_c.cur.durationMs : g_c.lenMs;
        if (!back && !g_c.resuming && g_c.maxTime + 10000 >= lenMs &&
            (time + 5000 < g_c.maxTime || state != 1) && !g_c.menuPaused) {
            logLine("carry on: song %d of %d ended (it got to %s of %s)", g_c.cur.index + 1,
                    g_c.count, mmss(g_c.maxTime).s, mmss(lenMs).s);
            scrobbleSong(&g_c.cur);
            carryNext(g_c.cur.index, true, "the one before ended");
            return;
        }
        // Stopped before its end, not from the row: as the power button
        // stops the shell's music. Played again every 5 s while it stays so,
        // as the player does; logged the first times and then once a minute.
        if (state != 1 && !g_c.menuPaused && !g_c.resuming) {
            if (!g_c.stoppedUs) g_c.stoppedUs = now;
            if ((uint32_t)(now - g_c.lastPlayUs) >= 5000000u) {
                g_c.lastPlayUs = now;
                const int rcPlay = musicEvent(kEventPlay);
                if (g_c.replays < 3 || g_c.replays % 12 == 0)
                    logLine("carry on: song %d of %d stopped at %s, before its end (state %d); "
                            "play again: 0x%08X",
                            g_c.cur.index + 1, g_c.count, mmss(time).s, state, (unsigned)rcPlay);
                g_c.replays++;
            }
        } else if (back) {
            if (g_c.stoppedUs)
                logLine("carry on: song %d of %d plays again at %s", g_c.cur.index + 1, g_c.count,
                        mmss(time).s);
            g_c.stoppedUs = 0;
            g_c.replays = 0;
            g_c.resuming = 0;
            // As player.c's watchSong: an M4A download stopped at 1:58 went
            // on at 1:23 (log f3042ca3), and is taken back to where it
            // stopped.
            if (time + 3000 < g_c.playMs || time > g_c.playMs + 3000) {
                const int rcTime = musicSeekTime(g_c.playMs), rcSeek = musicEvent(kEventSeek);
                logLine("carry on: song %d of %d went on at %s, not %s where it stopped; seek "
                        "time 0x%08X, seek 0x%08X",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, mmss(g_c.playMs).s,
                        (unsigned)rcTime, (unsigned)rcSeek);
                g_c.maxTime = time;
                return;
            }
        }
        if (state == 1) g_c.playMs = time;
        // Something else in the shell, as the system's Music app would put
        // there: not this song's length.
        if ((uint32_t)(now - g_c.checkUs) >= 2000000u) {
            g_c.checkUs = now;
            uint32_t len = 0;
            if (musicLength(&len) >= 0 && len > 0 && state == 1 && !hasSong(len, &g_c.cur, 0) &&
                (g_c.lenMs == 0 || len + 3000 < g_c.lenMs || len > g_c.lenMs + 3000)) {
                logLine("carry on: the shell plays a song %s long, not song %d", mmss(len).s,
                        g_c.cur.index + 1);
                carryEnd("something else plays in the shell", kEndOther);
                return;
            }
        }
    }
    if ((uint32_t)(now - g_c.statusUs) >= 2000000u) carryStatus(BGPLAYER_STATE_PLAYING, time);
}

// One look of the music thread's: a test asked for; a queue VitaPlex BG
// handed over begun, once a queue carried on before has stopped; the
// player's queue carried on, begun or looked at.
void musicStep() {
    char path[sizeof(g_musicPath)];
    lock();
    const int test = g_musicTestDue;
    g_musicTestDue = 0;
    if (test) sceClibMemcpy(path, g_musicPath, sizeof(path));
    const int carry = g_carry;
    const bool handed = g_playQueueDue && carry != 2;
    if (handed) {
        // In place of a carry-on that was to begin, and of its stop.
        g_playQueueDue = 0;
        g_carry = 0;
        g_carryStop = NULL;
    }
    unlock();
    if (test) {
        musicTest(path);
        lock();
        g_musicBusy = 0;
        unlock();
    }
    if (handed) {
        if (carryStartQueue())
            logLine("play queue: SceShell plays VitaPlex BG's queue");
        else
            carryNone();
    } else if (carry == 1) {
        if (carryStart()) {
            lock();
            g_carry = 2;
            unlock();
            logLine("carry on: SceShell carries the queue on");
        } else {
            carryNone();
        }
    } else if (carry == 2) {
        carryTick();
    }
}

// The music thread: musicStep every half second, and as soon as asked.
class MusicThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            SceUInt timeout = 500 * 1000;
            sceKernelWaitSema(g_musicSema, 1, &timeout);
            musicStep();
        }
    }
};

// The music thread, made the first time it is wanted, so that a boot with
// no test and no queue carried on has no stack for it in SceShell. true when
// there is one.
bool startMusicThread() {
    if (g_musicThread) return true;
    if (g_musicSema < 0) {
        g_musicSema = sceKernelCreateSema("VitaPlexQM::Music", 0, 0, 1, NULL);
        if (g_musicSema < 0) {
            logLine("music: its semaphore: 0x%08X", (unsigned)g_musicSema);
            return false;
        }
    }
    const int rc = (new MusicThread(kThreadPriority, 0x8000, "VitaPlexQM::Music"))->Start();
    logLine("music: its thread started: 0x%08X", (unsigned)rc);
    g_musicThread = rc >= 0;
    return g_musicThread;
}

// Has SceShell stop carrying the queue on, if it does: at its next look, on
// the music thread. Under g_lock.
void stopCarry(const char* why, int how) {
    if (!g_carry || g_carryStop) return;
    g_carryStop = why;
    g_carryStopHow = how;
    logLine("carry on: to stop: %s", why);
    if (g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
}

// ── Threads ─────────────────────────────────────────────────────────────────
// Watches the quick menu open and close, as ElevenMPV-A's plugin does, gives
// the row back when its owner has gone quiet, and writes the log.
class WatchThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        const SceUInt64 started = sceKernelGetSystemTimeWide();
        SceUInt64 lastLoop = started;
        int wasOpen = 0, openLogged = 0;
        while (!IsCanceled()) {
            SceAppMgrAppState st;
            sceClibMemset(&st, 0, sizeof(st));
            _sceAppMgrGetAppState(&st, sizeof(st), kAppStateVersion);
            const int open = st.isSystemUiOverlaid != 0;
            const SceUInt64 now = sceKernelGetSystemTimeWide();
            lock();
            // The system clock runs on while the Vita sleeps, and every
            // process sleeps with it (player.c logs "did not run for"): the
            // row's owner gets its time again, rather than being taken for
            // gone on the first look after. Writing the log can hold this
            // thread up too: 3.7 s once, as the quick menu's first opening
            // was logged while the player read every app's details (log
            // c1930c5a).
            if (now - lastLoop > 2000000ULL) {
                uint32_t secs, micros;
                splitUs(now - lastLoop, &secs, &micros);
                logLine("the plugin's watch did not run for %u.%u s (the Vita asleep, or the log "
                        "slow to write)",
                        (unsigned)secs, (unsigned)(micros / 100000));
                if (g_active) g_activatedUs = now;
            }
            lastLoop = now;
            if (open && !wasOpen) {
                g_openedUs = now;
                g_opens++;
                g_openSerial++;
                // Past the first opens, those with the row SceShell's are still
                // logged until its own row has been seen lit once.
                g_timelineDue = g_opens <= kTimelineOpens ||
                                (!g_active && !g_sawLitRow && g_opens <= 40);
                openLogged = g_active || g_timelineDue;
                if (openLogged)
                    logLine("quick menu opened%s", g_active ? "" : ", the row SceShell's");
            } else if (!open && wasOpen && openLogged) {
                logLine("quick menu closed after %d ms", msOpen(now));
            }
            g_open = open;
            if (g_active && g_owner != kOwnerShell &&
                now - g_activatedUs > QM_ACTIVE_TIMEOUT_S * 1000000ULL) {
                // The background player gone without giving the row back:
                // the system has ended it, and SceShell carries its queue on.
                // The row stays, the player's song and cover still on it.
                if (g_owner == QM_OWNER_PLAYER && g_carryAllowed && !g_carry &&
                    startMusicThread()) {
                    g_owner = kOwnerShell;
                    g_carry = 1;
                    g_carryStop = NULL;
                    __atomic_store_n(&g_carryPress, 0, __ATOMIC_SEQ_CST);
                    logLine("nothing from the background player for %d s: SceShell carries its "
                            "queue on",
                            QM_ACTIVE_TIMEOUT_S);
                    sceKernelSignalSema(g_musicSema, 1);
                } else {
                    g_active = 0;
                    g_releaseDue = 1;
                    logLine("nothing from %s for %d s: row given back", ownerName(g_owner),
                            QM_ACTIVE_TIMEOUT_S);
                }
            }
            // SceShell's UI thread has never called mainTick: the row is
            // ticked from here instead, as before build 2026.
            if (!g_fallback && !g_mainTicks && now - started > 3000000ULL) {
                g_fallback = 1;
                logLine("SceShell's UI thread has not called the plugin in 3 s: the row is "
                        "ticked from the plugin's own thread");
            }
            if (g_fallback) rowTick(now);
            unlock();
            wasOpen = open;
            logDrain();
            paf::thread::Sleep(100);
        }
    }
};

QmMessage g_message;  // the receiving thread's alone

// Takes what VitaPlex sends; the row's tick shows it.
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
            // A process with the row has the music: SceShell leaves it to
            // it, and does not begin a queue handed over before. The
            // background player goes on with SceShell's song itself; VitaPlex
            // BG plays its own, which SceShell's would play over, so that
            // one stops.
            stopCarry("a process takes the row", m.arg == QM_OWNER_APP ? kEndStopped : kEndYield);
            g_playQueueDue = 0;
            if (m.arg == QM_OWNER_PLAYER) g_carryAllowed = 1;
            if (g_owner != m.arg) g_artSerial = 0;  // the last owner's cover
            g_active = 1;
            g_owner = m.arg;
            g_activatedUs = sceKernelGetSystemTimeWide();
            break;
        case QM_CMD_DEACTIVATE:
            if (g_active) logLine("row given back by %s", ownerName(g_owner));
            stopCarry("the row given back", kEndTakeBack);
            g_active = 0;
            g_releaseDue = 1;
            g_artSerial = 0;
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
            break;
        }
        case QM_CMD_PLAYING:
            if (g_playing != (int)m.arg) logLine("%s", m.arg ? "playing" : "paused");
            g_playing = (int)m.arg;
            break;
        case QM_CMD_LOOK:
            if (m.arg < QM_LOOK_COUNT && g_look != (int)m.arg) {
                g_look = (int)m.arg;
                g_lookLogged = -1;
                logLine("buttons to be switched on with %s", lookName(g_look));
            }
            break;
        case QM_CMD_ART:
            if (g_artSerial != m.arg) {
                if (m.arg) logLine("art: %u ready", (unsigned)m.arg);
                else logLine("art: none");
            }
            g_artSerial = m.arg;
            break;
        case QM_CMD_MUSIC_TEST:
            if (g_musicBusy || g_carry || !startMusicThread()) {
                logLine("music test: done: %s", g_musicBusy ? "one runs already"
                                                : g_carry   ? "SceShell carries a queue on"
                                                            : "no thread for it");
                break;
            }
            // VitaPlex's paths are ASCII: its data folder and a song's rating key.
            toLog(m.title, g_musicPath, sizeof(g_musicPath));
            g_musicBusy = 1;
            g_musicTestDue = 1;
            sceKernelSignalSema(g_musicSema, 1);
            break;
        case QM_CMD_SONG_FILE: {
            QmSongFile f;
            sceClibMemcpy(&f, &m, sizeof(f));
            f.key[sizeof(f.key) - 1] = 0;
            f.path[sizeof(f.path) - 1] = 0;
            g_songFiles[g_songFilesNext] = f;
            g_songFilesNext = (g_songFilesNext + 1) % kSongFiles;
            logLine("song file: song %u, %s", (unsigned)f.arg + 1, f.path);
            break;
        }
        case QM_CMD_PLAY_QUEUE:
            // A queue SceShell carries on yields to the new one, its music
            // left for the new one to stop, and nothing written.
            logLine("play queue: VitaPlex BG hands its queue to SceShell%s",
                    g_carry ? "; the queue carried on stops first" : "");
            g_carryAllowed = 0;
            stopCarry("VitaPlex BG hands a queue over", kEndYield);
            if (startMusicThread()) {
                g_playQueueDue = 1;
                sceKernelSignalSema(g_musicSema, 1);
            } else {
                logLine("play queue: no music thread, so nothing plays");
            }
            break;
        case QM_CMD_TAKE_BACK:
            logLine("take back: VitaPlex BG takes the music back%s",
                    g_carry ? "" : "; no queue carried on here");
            g_carryAllowed = 0;
            g_playQueueDue = 0;  // a queue handed over and not yet begun is not begun
            stopCarry("VitaPlex BG takes the music back", kEndTakeBack);
            break;
        case QM_CMD_HTTP_TEST: {
            if (g_httpBusy || g_fetch.state == kFetchRunning) {
                logLine("http test: done: %s",
                        g_httpBusy ? "one runs already" : "SceShell is fetching a song of the queue");
                break;
            }
            // VitaPlex's addresses, paths and tokens are ASCII. The token is
            // kept no longer than the test needs it, here or in the message.
            toLog(m.title, g_httpBase, sizeof(g_httpBase));
            toLog(m.artist, g_httpPath, sizeof(g_httpPath));
            toLog(m.album, g_httpToken, sizeof(g_httpToken));
            sceClibMemset(g_message.album, 0, sizeof(g_message.album));
            g_httpBytes = m.arg;
            g_httpBusy = 1;
            const SceUID thread = sceKernelCreateThread("VitaPlexQM::Http", httpThread,
                                                        kThreadPriority, kHttpStack, 0, 0, NULL);
            const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
            if (rc < 0) {
                if (thread >= 0) sceKernelDeleteThread(thread);
                logLine("http test: done: no thread for it (0x%08X)", (unsigned)rc);
                sceClibMemset(g_httpToken, 0, sizeof(g_httpToken));
                g_httpBusy = 0;
            }
            break;
        }
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
        logDrain();
        return SCE_KERNEL_START_NO_RESIDENT;
    }
    g_surfacePool = *(paf::graph::SurfacePool* const*)((const char*)power + kPluginSurfacePool);
    logLine("quick menu: %s at %p; texture pool %p", g_impose->GetTypeName(), (void*)g_impose,
            (void*)g_surfacePool);

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
        logDrain();
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

    // PAF's list of what its main (UI) thread calls each time round its loop;
    // ElevenMPV-A's app imports Register, so its NID is the console's.
    paf::common::MainThreadCallList::Register(mainTick, NULL);
    logLine("row's tick registered with SceShell's UI thread");

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
