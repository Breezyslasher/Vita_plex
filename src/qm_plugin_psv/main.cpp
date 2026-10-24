/**
 * VitaPlex's quick menu plugin: the song, its cover, and buttons that work,
 * in the Music row of the quick menu (hold PS), while VitaPlex plays; and
 * VitaPlex's queue played on from inside SceShell once VitaPlex has gone.
 *
 * The row is SceShell's, and only code inside SceShell can change it. VitaPlex
 * loads this module into SceShell with taiHEN (qm_link.c) the first time it
 * wants the row in a boot, and it stays until the Vita restarts. The two talk
 * through the message pipes qm_protocol.h lays out.
 *
 * It does what ElevenMPV-A's shell plugin does (GrapheneCt/ElevenMPV-A,
 * GPL-3.0), in its way: it finds the quick menu through power_manage_plugin,
 * the row's buttons and two lines of text by their id hashes, and while the
 * quick menu is open puts the song on the lines and its own callbacks on the
 * buttons. It differs where that plugin fell short (docs/DESIGN_NOTES.md):
 *
 * - It changes SceShell's widgets on SceShell's own UI thread, through PAF's
 *   main thread call list, as ElevenMPV-A's app does its own. Its own threads
 *   watch the quick menu, take VitaPlex's messages and write the log.
 * - It switches the buttons on and lights them and their icons, which
 *   SceShell draws faded with no music of its own: the icons' own alpha,
 *   0.3, is set to 1 (build 2028), and put back when the row is SceShell's
 *   again.
 * - It takes the row 200 ms after the quick menu opens, as ElevenMPV-A's
 *   plugin about does: the song put on the lines at once did not show (build
 *   2022). While the quick menu stays open it puts back what SceShell
 *   changes: buttons switched off, other text.
 * - While VitaPlex plays, the play icon shows ⏸; and the Music heading's icon
 *   shows the song's cover, which VitaPlex writes to a file (QM_ART_PATH_FMT).
 * - A press is sent without waiting, so a process that stopped reading cannot
 *   hold SceShell up, and the row is given back by itself when no process has
 *   asked for it again in QM_ACTIVE_TIMEOUT_S.
 * - VitaPlex hands its queue to it as it goes (QM_CMD_PLAY_QUEUE): it plays
 *   the queue on through SceShell's music service, from the song and place
 *   VitaPlex had got to, fetches the songs that are not on the memory card
 *   from the server through Sony's HTTP library as SceShell has it, the
 *   server's MP3 of those in formats the shell does not play, and their
 *   covers, and marks the songs it played to their end played.
 * - It installs VitaPlex's updates once VitaPlex has quit
 *   (QM_CMD_INSTALL_UPDATE), as the system's installer refuses the title
 *   running, and launches VitaPlex again.
 * - It carries VitaPlex's downloads on once VitaPlex has gone
 *   (QM_CMD_DOWNLOADS), music and videos alike, into the files VitaPlex had
 *   begun, with their progress in a notification of SceShell's, until
 *   VitaPlex takes them back as it opens (QM_CMD_TAKE_DOWNLOADS).
 *
 * Built with vitasdk against the PAF headers scripts/vita/fetch_paf_headers.sh
 * fetches; its PAF imports are in paf_stubs.S. Logs to
 * ux0:data/VitaPlex/qm-plugin.log, one file a boot.
 */

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>

#include <limits>  // before PAF's headers: their vector uses it without including it
#include <stdarg.h>
#include <stdint.h>

#include <paf.h>

#include "handover.h"
#include "mp3_frames.h"
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
// Not declared by every SDK's headers (qm_link.c declares it the same way).
extern "C" int sceAppMgrGetIdByName(SceUID* pid, const char* name);


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

// FNV-1a over UTF-16, to tell texts apart.
uint32_t hashText(const uint16_t* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ *s) * 16777619u;
    return h;
}

bool startsWith(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (*s != *prefix) return false;
    return true;
}

// ── State ───────────────────────────────────────────────────────────────────
// The Music row's parts, by the id hashes SceShell's quick menu gives them:
// the buttons and lines as ElevenMPV-A's main.h and main.cpp have them, the
// rest as the plugin's log listed the row (build 2022).
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
// The row's heading's icon, "Music"'s: a plane at its group's left, 0x0 and
// drawn at its texture's 80x80, as the other rows' icons are.
const uint32_t kHashHeadingIcon = 0xE219E0DE;
// A third line at the title's place, "There are no songs.", which SceShell
// keeps at an own alpha of 0 (build 2028's timeline, log c1930c5a).
const uint32_t kHashNoSongs = 0x5608C548;
// The song's lines' own alphas as SceShell has them as the quick menu opens,
// before it fades them out with no music of its own: the title's 1, the
// artist and album's 0.8 (log c1930c5a, where they went to 0 by 429 ms).
const float kTopAlpha = 1.0f, kBottomAlpha = 0.8f;

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

// The row's owner while SceShell itself plays the queue VitaPlex handed it
// (qm_protocol.h), beside QM_OWNER_APP. 2 was VitaPlex's background player's,
// up to build 2045.
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
int g_playing = -1;
uint16_t g_title[256], g_artist[256], g_album[256];
uint32_t g_artSerial;  // the cover at QM_ART_PATH, 0 for none
char g_artPath[160];   // the file a kShellArt serial names
int g_releaseDue;      // the row given back, to be left as SceShell had it
// The music thread's (startMusicThread), woken for a queue handed over, for a
// press while SceShell carries the queue on, and to stop that.
SceUID g_musicSema = -1;
// From SceShell's UI thread (onPress), atomic: a press on the row for the
// music thread while SceShell carries the queue on, QM_PRESS_*.
int g_carryPress;

// The quick menu, by the watch thread:
int g_open;            // it is open
SceUInt64 g_openedUs;  // when it was seen open
int g_openSerial;      // opens so far; the row's tick sees a new one by it

// The row, by its tick (rowTick), on SceShell's UI thread: found anew each
// tick, so that nothing is kept of a row SceShell has since done away with.
paf::ui::Widget *g_rew, *g_play, *g_ff, *g_textTop, *g_textBottom, *g_noSongs;
paf::ui::Widget *g_iconRew, *g_iconPlay, *g_iconFf, *g_iconPause, *g_headingIcon;
uint32_t g_topHash, g_bottomHash;  // the lines as this plugin set them
int g_tickOpenSerial;              // the open this tick's state is for
SceUInt64 g_lastTickUs;
int g_applied;    // the row is ours this time it is open
int g_mainTicks;  // times SceShell's UI thread has called mainTick
int g_fallback;   // it never has: the watch thread ticks the row itself
// Once a boot: PAF's widget layout is its headers' (checkLayout).
int g_layoutChecked, g_layoutOk;

// What became of the row in an open of the quick menu, logged as it closes
// (noteClosed): the first opens of a boot, and the first after the row
// changes hands, when SceShell kept the song hidden (build 2049).
struct OpenNote {
    int logIt;       // to be logged as it closes
    int foundMs;     // the row's buttons first found, -1 if never
    int takenMs;     // the row taken, -1 if not
    uint32_t owner;  // whose it was taken for
    // What the plugin put back or changed after taking it.
    int buttons, lines, shown, playIcon, cover, lit;
    int firstPutMs;  // the first of those, -1 if none
    int snapped;     // the lines as they were 1 s in, in snap
    char snap[220];
};
OpenNote g_note;
// Opens still to be logged: the first of a boot, and a few more each time the
// row changes hands (notesWanted).
int g_notesLeft = 6;

// Ms the quick menu has been open, at now: a 32-bit division (splitUs has
// why), an open of over an hour reading as 71 minutes.
int msOpen(SceUInt64 now) {
    const SceUInt64 us = now - g_openedUs;
    return us > 0xFFFFFFFFull ? (int)(0xFFFFFFFFu / 1000u) : (int)((uint32_t)us / 1000u);
}

// The row has changed hands: its next opens are logged. Under g_lock.
void notesWanted() {
    if (g_notesLeft < 3) g_notesLeft = 3;
}

void lock() {
    sceKernelLockMutex(g_lock, 1, NULL);
}
void unlock() {
    sceKernelUnlockMutex(g_lock, 1);
}

const char* ownerName(uint32_t owner) {
    return owner == QM_OWNER_APP ? "VitaPlex"
         : owner == kOwnerShell  ? "SceShell"
                                 : "?";
}

// Members PAF's headers keep protected: read, and a dimmed icon's alphas
// written, only once the layout has been checked (checkLayout).
struct WidgetPeek : paf::ui::Widget {
    static WidgetPeek* of(paf::ui::Widget* w) {
        return static_cast<WidgetPeek*>(w);
    }
    static const paf::math::v4& size(paf::ui::Widget* w) {
        return of(w)->m_size;
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
};

// A vector's x or y, rounded: vitasdk-paf-component's v4 keeps its four floats
// to itself.
int coord(const paf::math::v4& v, int i) {
    return (int)reinterpret_cast<const float*>(&v)[i];
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


// A widget's texture, with the reference GetTexture adds kept for good: the
// plugin's own hold on it, so it outlives any row SceShell makes.
paf::graph::Surface* holdTexture(paf::ui::Widget* w) {
    TexRef ref(NULL);
    w->GetTexture(ref.ptr);
    return ref.ptr.get();
}

// A widget's texture in its first draw object, to compare with: the
// reference GetTexture adds is dropped again without ever freeing the
// texture from this thread, should SceShell have let go of it meanwhile.
paf::graph::Surface* currentTexture(paf::ui::Widget* w) {
    TexRef ref(NULL);
    w->GetTexture(ref.ptr);
    paf::graph::Surface* s = ref.ptr.get();
    if (s) s->UnsafeRelease();
    return s;
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

// The row's widgets, by their ids: on its tick. true when its buttons are
// there; they come a moment after the quick menu.
bool findRow() {
    g_rew = find(kHashRew);
    g_play = find(kHashPlay);
    g_ff = find(kHashFf);
    g_textTop = find(kHashTextTop);
    g_textBottom = find(kHashTextBottom);
    g_noSongs = find(kHashNoSongs);
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

// One button or icon lit; true when it changed anything. A widget hidden by
// its colour or an alpha is left so.
bool light(paf::ui::Widget* w) {
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
    // A draw object that was faded is set by hand even if SetColor_core lit
    // it: kLitColor marks it as the plugin's, to be put back. One whose
    // layout is not the headers' is left faded.
    if (dimDrawn) {
        Lit* l = drawLayoutOk(w, d) ? litEntry(w) : NULL;
        if (l) {
            if (!l->d) {
                l->d = d;
                l->was = drawn;
            }
            DrawPeek::setColor(d, kLitColor);
        }
    }
    return true;
}

bool lightAll() {
    bool changed = light(g_iconRew);
    changed |= light(g_iconPlay);
    changed |= light(g_iconFf);
    changed |= light(g_rew);
    changed |= light(g_play);
    changed |= light(g_ff);
    return changed;
}

// Puts back the own alphas and draw objects' colours the plugin changed,
// where they are still its: with the row's widgets found this tick
// (findRow). They must be the widgets lit, so a row SceShell made anew is
// left alone.
void unlightAll() {
    if (!g_litCount) return;
    for (int i = 0; i < g_litCount; i++) {
        const Lit& l = g_lit[i];
        paf::ui::Widget* const found[6] = {g_iconRew, g_iconPlay, g_iconFf, g_rew, g_play, g_ff};
        bool same = false;
        for (int j = 0; j < 6 && !same; j++) same = found[j] == l.w;
        if (!same) continue;
        if (l.alphaWas >= 0.0f && WidgetPeek::ownAlpha(l.w) >= 0.99f)
            WidgetPeek::setOwnAlpha(l.w, l.alphaWas);
        if (l.d && l.w->GetDrawObj(0) == l.d && DrawPeek::color(l.d) == kLitColor)
            DrawPeek::setColor(l.d, l.was);
    }
    g_litCount = 0;
}

// ── The play/pause icon ─────────────────────────────────────────────────────
// While VitaPlex plays, the play icon is given the pause icon's texture, and
// its own back when paused or when the row is SceShell's again (build 2025).
paf::graph::Surface *g_playTex, *g_pauseTex;  // held for good (holdTexture)
int g_pauseShown;  // the play icon has the pause icon's texture

// Holds the two icons' textures, the first time the row is taken in a boot:
// SceShell's own then, before the plugin has changed either.
void holdPlayTextures() {
    if (g_playTex || !g_iconPlay || !g_iconPause) return;
    g_playTex = holdTexture(g_iconPlay);
    g_pauseTex = holdTexture(g_iconPause);
}

// The play icon as VitaPlex's music is, or as SceShell's own row had it:
// true when it changed it.
bool showPlaying(bool pause) {
    if (!g_iconPlay || !g_playTex || !g_pauseTex) return false;
    paf::graph::Surface* want = pause ? g_pauseTex : g_playTex;
    if (currentTexture(g_iconPlay) == want) return false;
    giveTexture(g_iconPlay, want);
    g_pauseShown = pause;
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
bool showOwnIcon() {
    g_artShown = 0;
    if (!g_headingIcon || !g_headingTex || currentTexture(g_headingIcon) == g_headingTex)
        return false;
    giveTexture(g_headingIcon, g_headingTex);
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
    if (want == 0) return showOwnIcon();
    const bool own = currentTexture(g_headingIcon) == g_headingTex;
    if (!own && g_artShown == want) return false;
    if (g_artFailed == want) return own ? false : showOwnIcon();
    paf::graph::Surface* art = loadArt(want);
    if (!art) {
        g_artFailed = want;
        logLine("art: %u not loaded", (unsigned)want);
        return own ? false : showOwnIcon();
    }
    giveTexture(g_headingIcon, art);
    art->UnsafeRelease();  // the icon's reference is the one left
    g_artShown = want;
    return true;
}

// ── Switching the buttons on ────────────────────────────────────────────────
// Each button switched on and put in its normal state: PlaneButton has a
// ChangeButtonState of its own, which may also put the button's normal look
// on it. ElevenMPV-A's way, Enable(false), and Enable(true) alone left them
// dimmed (builds 2019 to 2022).
void switchOn(paf::ui::Widget* w) {
    paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(w);
    b->Enable(true);
    b->ChangeButtonState(paf::ui::ButtonBase::ST_BTN_NORMAL);
}

void switchOnAll() {
    switchOn(g_rew);
    switchOn(g_play);
    switchOn(g_ff);
    lightAll();
}

// While the row is ours: a button SceShell has switched off or dimmed again
// since, as its own set-up of the row may do after ours, is switched on
// again. true when it changed anything.
bool watchButtons() {
    if (!g_layoutOk) return false;
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(buttons[i]);
        // State 1 is dimmed; 2 to 4 are a press under way, left alone.
        if (buttonEnabled(b) == 1 && buttonState(b) != 1) continue;
        switchOn(buttons[i]);
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
    setTexts();
    return true;
}

// A widget's own alpha set to want, should it be another; one that does not
// read as a float from 0 to 1 is left alone. true when it changed it.
bool setOwnAlphaIf(paf::ui::Widget* w, float want) {
    if (!w) return false;
    const float a = WidgetPeek::ownAlpha(w);
    if (a < 0.0f || (a > want - 0.01f && a < want + 0.01f)) return false;
    WidgetPeek::setOwnAlpha(w, want);
    return true;
}

// SceShell's own alpha for "There are no songs." from before showLines hid
// it, to put back with the row; below 0 when there is nothing to put back.
// It stayed hidden once the row was SceShell's again in log 48ccef45.
float g_noSongsOwnAlpha = -1.0f;

// While the row is ours: the song's lines shown, and "There are no songs."
// hidden, by their own alphas. SceShell fades the lines out as the quick menu
// opens when it sees no music of its own, which it did on the first open
// after VitaPlex began to play: the song showed only from the second (build
// 2049). true when it changed anything.
bool showLines() {
    if (!g_layoutOk) return false;
    bool changed = setOwnAlphaIf(g_textTop, kTopAlpha);
    changed |= setOwnAlphaIf(g_textBottom, kBottomAlpha);
    if (g_noSongs && g_noSongsOwnAlpha < 0.0f) {
        const float a = WidgetPeek::ownAlpha(g_noSongs);
        if (a > 0.01f && a <= 1.0f) g_noSongsOwnAlpha = a;  // what is changed below
    }
    changed |= setOwnAlphaIf(g_noSongs, 0.0f);
    return changed;
}

// "There are no songs." as SceShell had it before showLines hid it.
void putNoSongsBack() {
    if (g_noSongsOwnAlpha < 0.0f) return;
    if (g_noSongs && g_layoutOk) WidgetPeek::setOwnAlpha(g_noSongs, g_noSongsOwnAlpha);
    g_noSongsOwnAlpha = -1.0f;
}

// A line as it is, for the open's note: whose text it holds, its own alpha,
// and its draw object's colour.
int noteLine(char* out, int cap, const char* name, paf::ui::Widget* w, uint32_t ours) {
    if (!w) return sceClibSnprintf(out, cap, "%s none", name);
    paf::wstring s;
    w->GetString(s);
    const uint16_t* text = (const uint16_t*)s.c_str();
    const char* whose = !text || !text[0] ? "empty" : hashText(text) == ours ? "ours" : "other";
    const float a = g_layoutOk ? WidgetPeek::ownAlpha(w) : -1.0f;
    paf::graph::DrawObj* d = w->GetDrawObj(0);
    return sceClibSnprintf(out, cap, "%s %s, alpha %d%%, drawn %08X", name, whose,
                           a < 0.0f ? -1 : (int)(a * 100.0f + 0.5f),
                           d ? (unsigned)DrawPeek::color(d) : 0u);
}

// The lines and buttons as they are 1 s into the open, into the note.
void noteLines() {
    const int cap = (int)sizeof(g_note.snap);
    int n = 0;
    // Each part where the last ended, the length kept within the buffer: a
    // part cut short returns the length it would have had.
    auto clamp = [&](int wrote) { n = wrote < 0 ? n : n + wrote < cap - 1 ? n + wrote : cap - 1; };
    clamp(noteLine(g_note.snap, cap, "title", g_textTop, g_topHash));
    clamp(noteLine(g_note.snap + n, cap - n, "; artist", g_textBottom, g_bottomHash));
    clamp(noteLine(g_note.snap + n, cap - n, "; no-songs", g_noSongs, 0));
    if (g_layoutOk && g_rew && g_play && g_ff)
        clamp(sceClibSnprintf(g_note.snap + n, cap - n, "; buttons on %d%d%d",
                              buttonEnabled(static_cast<paf::ui::ButtonBase*>(g_rew)),
                              buttonEnabled(static_cast<paf::ui::ButtonBase*>(g_play)),
                              buttonEnabled(static_cast<paf::ui::ButtonBase*>(g_ff))));
    g_note.snapped = 1;
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
    if (!found)
        logLine("row: the play button reports id 0x%08X and its parent %p has %d children: "
                "the widget layout is not the headers', so nothing is read from it",
                (unsigned)id, (void*)parent, siblings);
}

// ── Taking, keeping and giving back the row ─────────────────────────────────
// Takes the row, the quick menu having been open kTakeAfterUs. Another of
// SceShell's overlays, such as the LiveArea's, reads as the quick menu open
// too, with no row's buttons to be found. true when taken.
bool takeRow(SceUInt64 now, bool found) {
    if (!found) return false;
    if (!g_layoutChecked) checkLayout(g_play);
    switchOnAll();
    holdPlayTextures();
    showPlaying(g_playing == 1);
    g_rew->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                            (void*)(uintptr_t)QM_PRESS_PREVIOUS);
    g_play->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                             (void*)(uintptr_t)QM_PRESS_PLAY_PAUSE);
    g_ff->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                           (void*)(uintptr_t)QM_PRESS_NEXT);
    setTexts();
    showLines();
    showArt();
    g_applied = 1;
    g_note.takenMs = msOpen(now);
    g_note.owner = g_owner;
    return true;
}

// While the row is ours and the quick menu open, ten times a second: puts
// back what SceShell has changed since, and shows what VitaPlex has sent
// since, each noted for the open's log.
void keepRow(SceUInt64 now) {
    const int buttons = watchButtons(), lines = keepTexts(), shown = showLines(),
              playIcon = showPlaying(g_playing == 1), cover = showArt(), lit = lightAll();
    g_note.buttons += buttons;
    g_note.lines += lines;
    g_note.shown += shown;
    g_note.playIcon += playIcon;
    g_note.cover += cover;
    g_note.lit += lit;
    if (g_note.firstPutMs < 0 && (buttons || lines || shown || playIcon || cover || lit))
        g_note.firstPutMs = msOpen(now);
}

// The row given back with the quick menu open: it is left as SceShell's own
// set-up had it with no music of its own (build 2023's log): the lines empty,
// the buttons off and faded, play showing ▶ and the heading its own icon.
void releaseRow() {
    unlightAll();
    if (g_pauseShown) showPlaying(false);
    showOwnIcon();
    putNoSongsBack();
    const paf::wstring empty;
    if (g_textTop) g_textTop->SetString(empty);
    if (g_textBottom) g_textBottom->SetString(empty);
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    for (int i = 0; i < 3; i++) static_cast<paf::ui::ButtonBase*>(buttons[i])->Enable(false);
    g_applied = 0;
}

// The row SceShell's, with something of the plugin's left on it since it was
// last ours. true when it changed anything.
bool restoreRow() {
    if (!g_litCount && !g_pauseShown && !g_artShown && g_noSongsOwnAlpha < 0.0f) return false;
    putNoSongsBack();
    unlightAll();
    // Its play icon is put back only with the button off: with it on,
    // SceShell's own music is in the row, and ⏸ may be SceShell's.
    if (g_pauseShown && !static_cast<paf::ui::ButtonBase*>(g_play)->IsEnable()) showPlaying(false);
    g_pauseShown = 0;
    if (g_artShown) showOwnIcon();
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
    if (found && g_note.foundMs < 0) g_note.foundMs = msOpen(now);
    if (g_releaseDue) {
        g_releaseDue = 0;
        if (g_applied) releaseRow();
    }
    if (g_active && !g_applied && now - g_openedUs >= kTakeAfterUs)
        takeRow(now, found);
    else if (g_active && g_applied)
        keepRow(now);
    else if (!g_active && found)
        restoreRow();
    if (g_note.logIt && !g_note.snapped && found && now - g_openedUs >= 1000000u) noteLines();
}

// PAF's main thread call list's: SceShell's UI thread, each time round its
// loop. It never waits for the plugin's lock, so SceShell is never held up.
void mainTick(void* data) {
    (void)data;
    g_mainTicks++;
    if (g_fallback) return;
    if (sceKernelTryLockMutex(g_lock, 1) < 0) return;
    rowTick(sceKernelGetSystemTimeWide());
    unlock();
}

// ── SceShell's music service ────────────────────────────────────────────────
// VitaPlex's background player, up to build 2045, played through SceShell's
// music service from a process of its own, which the system ended as an app
// with extended memory started. Code in SceShell drives the same service
// (build 2032's test, docs/DESIGN_NOTES.md), so the queue is played from here
// once VitaPlex has gone (qm_protocol.h). The service's client object (sceShellSvcGetSvcObj)
// is found with taiHEN, so that the plugin does not import it and takes
// whatever SceShell has: in build 2032 SceShell's own module SceShellSvc had
// it. It is spoken to with libShellAudio's calls (lib/libshellaudio), as the
// background player made them. The BGM port is left to the service, which
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
// as the background player started it, 1 and 8.
struct SvcTracking {
    int unk;
    int t1, t2;
};

GetSvcObj g_getSvcObj;
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

// ── Sony's SSL and HTTP libraries, as SceShell has them ─────────────────────
// What SceShell fetches the queue's songs that are not on the memory card
// with (build 2040's test found them working there). Found with taiHEN as the
// music service is: imported, a library SceShell did not have would keep the
// whole plugin out. What holds for all of SceShell is left as it is: the TLS
// options and certificates, and the libraries' start, ended only when the
// plugin began them. The token goes as a header, never logged.
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
const int kHttpPost = 1;                         // SCE_HTTP_METHOD_POST
const unsigned kHeaderOverwrite = 0;             // SCE_HTTP_HEADER_OVERWRITE
const int kHttpAlreadyInited = (int)0x80431020;  // SCE_HTTP_ERROR_ALREADY_INITED
const int kSslAlreadyInited = (int)0x80435020;   // what SceShell's gave (build 2040)
// The pools, should the plugin be what starts the libraries.
const unsigned kSslPool = 256 * 1024, kHttpPool = 64 * 1024;

// A library's functions, from SceShell's module of it, else from its file
// loaded into SceShell now. `who` begins each line it logs. true when every
// one is found.
bool findLibrary(const char* who, const char* module, uint32_t libnid, const char* path,
                 LibFn* fns, int count) {
    uintptr_t fn = 0;
    int rc = vitaplexQmGetModuleExportFunc(module, libnid, fns[0].nid, &fn);
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
// codes, which say what is to be ended after (httpEnd). A start that failed
// otherwise than as begun already is logged. true when the HTTP library can
// be used. The fetch, the server's and the download threads each start them
// and end them as they go, and may run at once: the first to start them
// starts them, and the last to end ends them, so that none ends them under
// another (g_netLock).
SceUID g_netLock = -1;
int g_netUsers;              // under g_netLock: threads between start and end
int g_netRcSsl, g_netRcHttp;  // under g_netLock: the first's starts
bool httpUsable(int rcHttp) {
    return rcHttp >= 0 || rcHttp == kHttpAlreadyInited;
}
bool httpStart(const char* who, int* rcSsl, int* rcHttp) {
    sceKernelLockMutex(g_netLock, 1, NULL);
    if (g_netUsers == 0) {
        g_netRcSsl = ((FnUnsigned)g_ssl[kSslInit].fn)(kSslPool);
        if (g_netRcSsl < 0 && g_netRcSsl != kSslAlreadyInited)
            logLine("%s: SSL library not started: 0x%08X", who, (unsigned)g_netRcSsl);
        g_netRcHttp = httpFn<FnUnsigned>(kHttpInit)(kHttpPool);
        if (!httpUsable(g_netRcHttp)) {
            logLine("%s: HTTP library not started: 0x%08X", who, (unsigned)g_netRcHttp);
            if (g_netRcSsl >= 0) ((FnVoid)g_ssl[kSslTerm].fn)();
            g_netRcSsl = -1;
        }
    }
    *rcSsl = g_netRcSsl;
    *rcHttp = g_netRcHttp;
    const bool usable = httpUsable(g_netRcHttp);
    if (usable) g_netUsers++;
    sceKernelUnlockMutex(g_netLock, 1);
    return usable;
}

// What httpStart started, ended once no thread uses it. Called with the codes
// it gave, whether or not it was called: nothing is ended for a start that
// failed or never was.
void httpEnd(int rcSsl, int rcHttp) {
    (void)rcSsl;
    if (!httpUsable(rcHttp)) return;
    sceKernelLockMutex(g_netLock, 1, NULL);
    if (g_netUsers > 0 && --g_netUsers == 0) {
        if (g_netRcHttp >= 0) httpFn<FnVoid>(kHttpTerm)();
        if (g_netRcSsl >= 0) ((FnVoid)g_ssl[kSslTerm].fn)();
    }
    sceKernelUnlockMutex(g_netLock, 1);
}

// A template with the time-outs build 2040's test found to do: 5 s to resolve
// and to connect, 10 s to send and to receive. Its id, or the code it failed
// with.
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
// headers, as the player sends them, with client as its id, the token too
// when given, and a range when given. `who` and `what` name it in the log,
// which has the call that failed, and why TLS refused it; with no `who`,
// nothing is logged. 0, with *conn and *req to read from and then close
// (httpClose); else the code of the call that failed, with nothing left to
// close.
int httpOpen(int tmpl, const char* who, const char* what, const char* url, const char* token,
             const char* range, int* conn, int* req, const char* client = "vitaplex-sceshell") {
    *req = -1;
    *conn = httpFn<FnConnection>(kHttpCreateConnection)(tmpl, url, 1);
    if (*conn < 0) {
        if (who) logLine("%s: %s: connection 0x%08X", who, what, (unsigned)*conn);
        return *conn;
    }
    *req = httpFn<FnRequest>(kHttpCreateRequest)(*conn, kHttpGet, url, 0);
    if (*req < 0) {
        const int rc = *req;
        if (who) logLine("%s: %s: request 0x%08X", who, what, (unsigned)rc);
        httpClose(*conn, -1);
        return rc;
    }
    int rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Client-Identifier", client,
                                              kHeaderOverwrite);
    if (rc >= 0)
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Product", "VitaPlex", kHeaderOverwrite);
    if (rc >= 0 && token && token[0])
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Token", token, kHeaderOverwrite);
    if (rc >= 0 && range) rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "Range", range, kHeaderOverwrite);
    if (rc >= 0) rc = httpFn<FnSend>(kHttpSend)(*req, NULL, 0);
    if (rc < 0) {
        if (who) {
            int err = 0;
            unsigned detail = 0;
            const int rcSsl = httpFn<FnSslError>(kHttpsSslError)(*req, &err, &detail);
            char why[128];
            sslDetail(detail, why, sizeof(why));
            logLine("%s: %s: failed 0x%08X; TLS error 0x%08X, detail 0x%X%s (0x%08X)", who, what,
                    (unsigned)rc, (unsigned)err, detail, why, (unsigned)rcSsl);
        }
        httpClose(*conn, *req);
        return rc;
    }
    return 0;
}

// ── SceShell carries the queue on ───────────────────────────────────────────
// (qm_protocol.h) VitaPlex hands its queue over as it goes: SceShell plays
// it from the song and place VitaPlex had got to, and then the queue's
// next song, and the one after, from here. Everything comes from the files
// VitaPlex keeps (handover.h): the queue for the songs and their
// downloads. The status is written as VitaPlex reads it, with
// "by=SceShell", so that it takes the music back where SceShell has got to.
// The row is SceShell's meanwhile: the song's lines, its cover
// (QM_SONG_COVER_FMT), and its buttons, which act on the music here. Its
// lines in the log begin "carry on:". Up to build 2045 this carried on the
// queue of VitaPlex's background player, a program of its own the system
// ended as an app with extended memory opened, which VitaPlex no longer has;
// the comments' player.c is its source, in the history.
const int kLookAhead = 20;        // songs looked at for the next on the card
// How long the music thread looks every 25 ms after VitaPlex's song is
// played, for the seek to its place (Carry::fastUntilUs).
const SceUInt64 kFastLookUs = 4 * 1000 * 1000;
const SceUInt kFastLookEveryUs = 25 * 1000;
// Its looks while the shell plays a song of the queue: every 100 ms, so that
// a song the power button stops is played again within one. With a look each
// half second the shell was seen playing again 0.5 to 1.6 s after it was
// seen stopped (logs d6e76980, e92d16a1). Each half second otherwise: as a
// song is fetched, and with no queue (musicLookUs).
const SceUInt kLookEveryUs = 100 * 1000;
const SceUInt kSlowLookEveryUs = 500 * 1000;
// A song the shell never starts is passed over after 15 s of looks.
const int kNeverStartedLooks = 150;
// A song stopped before its end is played again at once, and again every
// 0.6 s while it stays stopped: in log e92d16a1 one was still stopped a
// second after a play, and played after the next.
const uint32_t kReplayEveryUs = 600 * 1000;
// How long a song played again after a stop is watched for going on further
// back than where it stopped (Carry::backUs). In log 0790f7dc an M4A jumped
// back within 1.6 s of playing again once; the other times are bounded by
// the next look the log has, 3.8, 7.6 and 11 s on.
const uint32_t kBackWatchUs = 10 * 1000 * 1000;
// Stopping: VitaPlex taking the music back, ready to play it, the service
// stopped and ended and the place it stopped at written; the row given back,
// the place written and the music left; the music left to a process that
// took the row, with nothing written; something else in the shell, the
// status saying stopped; the queue played out, or nothing more on the card:
// the service stopped and ended.
enum { kEndTakeBack = 1, kEndRowBack, kEndYield, kEndOther, kEndOver, kEndStopped };

// Under g_lock:
int g_carry;             // 2 while SceShell carries a queue on, else 0
const char* g_carryStop; // why it is to stop, or NULL ...
int g_carryStopHow;      // ... and how (kEnd*)
int g_playQueueDue;      // VitaPlex has handed its queue over (QM_CMD_PLAY_QUEUE)
uint32_t g_shellArts;    // kShellArt serials so far

// The music thread's alone.
struct Track {
    int index;          // its place in the queue, from 0
    char key[16];       // its ratingKey
    uint32_t durationMs;  // the server's length, 0 if unknown
    char local[128];    // a download of it, or ""
    char thumb[128];    // its cover on the server (a path), or ""
    char title[192], artist[128], album[128];  // UTF-8, for the row
    char cover[128];    // its download's cover, a PNG on the card, or ""
    char path[128];     // the file the shell is given
    int own;            // in the folder of songs (HANDOVER_SONGS): removed once two songs on
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
    // VitaPlex's setting (handover.h's offlineSkip): a song the server does
    // not answer for is passed over for the next song on the card, and with
    // none, held (carryOnCard).
    int offlineSkip;
    Track cur;          // the song with the shell
    Track prev;         // the one it had before, whose file stays for previous
    int started;        // seen playing, with its length (newSong)
    int menuPaused;     // the row's play/pause stopped it
    int resuming;       // the row's play pressed, the shell not yet playing
    int failures;       // songs in a row that would not play
    uint32_t maxTime, prevTime, lenMs, lastLenMs;
    uint32_t playMs;    // the clock as it last played, where it goes on from after a stop
    int havePrev, replays, lastState;
    int otherLogged;    // the shell seen playing something else in its place, logged
    int waitTicks;      // looks so far for it to play: looks, not the clock, which runs on
                        // through a sleep
    SceUInt64 songUs;      // when it was handed to the shell
    SceUInt64 stoppedUs;   // stopped before its end since, 0 if not
    SceUInt64 lastPlayUs, statusUs, checkUs;
    // A song not on the card, fetched to play next with the shell stopped
    // meanwhile: its place, -1 when none; the song; since when; the fetches
    // begun for it; why it is next (carryPlay's why); and since when the
    // shell has been seen playing something meanwhile, 0 if it is not.
    int waitIndex;
    Track wait;
    SceUInt64 waitUs;
    int waitTries;
    const char* waitWhy;
    SceUInt64 waitPlayUs;
    int waitArt;      // its cover on the row
    int held;         // paused as the server did not answer, fetched again on the row's play
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
    // VitaPlex's song as it handed the queue over (-1 for none), and where
    // in it: the shell is taken there once it plays (seekTo), and a seek to
    // there under way (0 for none), since when, and how many times asked.
    int startIndex;
    uint32_t startAtMs;
    uint32_t seekWantMs;
    SceUInt64 seekUs;
    int seekTries;
    // Until when the music thread looks every 25 ms, not every 100: from
    // the play of VitaPlex's song to its seek landing, so that the seek goes
    // as soon as the shell plays (kFastLookUs). 0 when not.
    SceUInt64 fastUntilUs;
    // A song played again after a stop: where it had stopped, and when it
    // played again, for kBackWatchUs. 0 when not watched.
    uint32_t backFromMs;
    SceUInt64 backUs;
    // What the server was last told of the music (report), and when.
    const char* reportState;
    SceUInt64 reportUs;
};
Carry g_c;
// The music's states as the server is told them (report).
const char* const kPlaying = "playing";
const char* const kPaused = "paused";
const char* const kBuffering = "buffering";
const char* const kStopped = "stopped";
void report(const Track* t, const char* state, uint32_t timeMs);
Track g_found;  // a song looked at for what comes next
Track g_scan;   // a song read from the queue as it is looked through (nextOnCard)
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

// The value of key in a note (handover.h's "key=value" lines) into out, cut
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
    char file[128];     // that song as a file VitaPlex made ready (file=), or ""
    int offlineSkip;    // the setting (offlineSkip=), 0 when missing
};

bool scanOpen(QueueScan* s) {
    s->index = -1;
    s->whole = false;
    s->written[0] = 0;
    s->repeat = HANDOVER_REPEAT_OFF;
    s->start = 0;
    s->startMs = 0;
    s->file[0] = 0;
    s->offlineSkip = 0;
    return readerOpen(HANDOVER_QUEUE);
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
        } else if (startsWith(g_line, "offlineSkip=")) {
            s->offlineSkip = toInt(g_line + 12) == 1;
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

// A song's fields as handover.h lays them out (HANDOVER_TRACK_*).
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
    wholeField(&p, t->cover, sizeof(t->cover));
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

// Where t is on the card, into t->path: its download, if the shell plays its
// format. false when it is not.
bool onCard(Track* t) {
    if (!t->local[0] || !shellPlays(t->local) || !exists(t->local)) return false;
    sceClibSnprintf(t->path, sizeof(t->path), "%s", t->local);
    t->own = 0;
    return true;
}

// t's file in the folder of songs, whole, into t->path: fetched for it here
// or by VitaPlex, and kept as the queue went on or was taken back. false when
// there is none.
bool inFolder(Track* t) {
    static const char* const kExts[4] = {"mp3", "m4a", "aac", "wav"};
    if (!t->key[0]) return false;
    for (int i = 0; i < 4; i++) {
        sceClibSnprintf(t->path, sizeof(t->path), "%s/%s.%s", HANDOVER_SONGS, t->key, kExts[i]);
        if (exists(t->path)) {
            t->own = 1;
            return true;
        }
    }
    t->path[0] = 0;
    return false;
}

// Whether t is on the card, as a download or in the folder of songs, with its
// file in t->path.
bool anyOnCard(Track* t) {
    return onCard(t) || inFolder(t);
}

// The song `steps` on from `from` in the queue's order, back for a negative
// count, repeat all going round; -1 past either end.
int stepFrom(int from, int steps) {
    const int i = from + steps;
    if (i >= 0 && i < g_c.count) return i;
    if (g_c.repeat != HANDOVER_REPEAT_ALL || g_c.count <= 0) return -1;
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

// The status, as handover.h lays it out and the background player wrote it
// (player.c's writeStatus), with "by=SceShell" on it, for t: the song with the
// shell, or one the shell waits for as it is fetched ("fetching=1").
void carryStatusOf(const Track* t, const char* state, uint32_t positionMs, bool fetching) {
    static char text[448];
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    uint32_t secs, micros;
    splitUs(now, &secs, &micros);
    // paused=1 while the row's play/pause holds it, so that VitaPlex takes
    // the music back paused (build 2052; up to build 2051 it played on).
    const int n = sceClibSnprintf(
        text, sizeof(text),
        "written=%u.%06u\nqueue=%s\nindex=%d\nratingKey=%s\npositionMs=%u\ndurationMs=%u\n"
        "state=%s\nby=SceShell\n%s%send\n",
        (unsigned)secs, (unsigned)micros, g_c.queue, t->index, t->key, (unsigned)positionMs,
        (unsigned)t->durationMs, state, fetching ? "fetching=1\n" : "",
        g_c.menuPaused ? "paused=1\n" : "");
    const char* const part = HANDOVER_STATUS ".part";
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int rc = fd;
    if (fd >= 0) {
        rc = sceIoWrite(fd, text, (SceSize)n);
        sceIoClose(fd);
    }
    if (rc == n) {
        // rename does not replace a file on every target (player.c's moveIn).
        sceIoRemove(HANDOVER_STATUS);
        rc = sceIoRename(part, HANDOVER_STATUS);
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

// The file in HANDOVER_SONGS of a song played, and its cover, once the shell
// is two songs on (carryPlay) or the queue is over: as player.c removed its
// own, but for the song before, which previous plays.
void removeOwn(const Track* t) {
    if (!t->own || !t->path[0]) return;
    const int rc = sceIoRemove(t->path);
    char cover[160];
    sceClibSnprintf(cover, sizeof(cover), QM_SONG_COVER_FMT, t->key);
    sceIoRemove(cover);
    logLine("carry on: song %d's file removed: 0x%08X", t->index + 1, (unsigned)rc);
}

// Where t's cover is on the card, into out: the one fetched or made for it
// beside the songs (QM_SONG_COVER_FMT), else its download's, the PNG the
// server made of it (HANDOVER_TRACK_COVER). false when it has neither.
bool coverPath(const Track* t, char* out, int cap) {
    sceClibSnprintf(out, cap, QM_SONG_COVER_FMT, t->key);
    if (exists(out)) return true;
    char ext[8];
    extOf(t->cover, ext, sizeof(ext));
    if (!same(ext, "png") || !exists(t->cover)) return false;
    sceClibSnprintf(out, cap, "%s", t->cover);
    return true;
}

uint8_t g_copyBuf[4096];  // the music thread's

// t's cover, if it has none on the card, as a copy of other's when the two
// have one cover on the server, their album's: the songs of an album after
// the first fetched the same 9190 bytes each, and showed none while their
// files came (log 8e7a4b23). Written beside it first, as the fetch thread's
// are, under a name of its own. The music thread's. true when t's is there.
bool coverFrom(const Track* t, const Track* other) {
    char to[160], from[160], part[168];
    if (coverPath(t, to, sizeof(to))) return true;
    if (!t->thumb[0] || !other->key[0] || same(other->key, t->key) ||
        !same(other->thumb, t->thumb) || !coverPath(other, from, sizeof(from)))
        return false;
    sceClibSnprintf(to, sizeof(to), QM_SONG_COVER_FMT, t->key);
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    sceClibSnprintf(part, sizeof(part), "%s.copy", to);
    const SceUID out = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    bool ok = out >= 0;
    uint32_t bytes = 0;
    for (int n; ok && (n = sceIoRead(in, g_copyBuf, sizeof(g_copyBuf))) != 0;) {
        ok = n > 0 && sceIoWrite(out, g_copyBuf, (SceSize)n) == n;
        bytes += ok ? (uint32_t)n : 0;
    }
    sceIoClose(in);
    if (out >= 0 && sceIoClose(out) < 0) ok = false;
    ok = ok && bytes > 0;
    if (ok) {
        sceIoRemove(to);
        ok = sceIoRename(part, to) >= 0;
    }
    if (!ok) {
        sceIoRemove(part);
        return false;
    }
    logLine("carry on: song %d's cover is song %d's, the same on the server", t->index + 1,
            other->index + 1);
    return true;
}

// The row as t is: its lines, ⏸, and its cover if it has one on the card.
void showTrack(const Track* t) {
    char cover[160];
    const bool art = coverPath(t, cover, sizeof(cover));
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
    if (!coverPath(t, cover, sizeof(cover))) return;
    lock();
    sceClibSnprintf(g_artPath, sizeof(g_artPath), "%s", cover);
    g_artSerial = kShellArt | (++g_shellArts & ~kShellArt);
    unlock();
}

// Where in t the shell is to begin: VitaPlex's place in the song it handed
// over, until the shell has been taken there; 0 for any other.
uint32_t startAt(const Track* t) {
    return t->index == g_c.startIndex ? g_c.startAtMs : 0;
}

// ── Fetching the queue's songs ──────────────────────────────────────────────
// A song the queue goes on to that is not on the memory card is fetched from
// VitaPlex's server, as the background player fetched one (player.c's
// prepareSong): its file as the server has it, when the shell plays its
// format, into the folder of songs (HANDOVER_SONGS), with its cover for the
// row (QM_SONG_COVER_FMT) first. Each stays while it is the song before, for
// previous, and goes after that (carryPlay); all stay as VitaPlex takes the
// music back, which plays on from them. A song's file there already, from
// VitaPlex or a queue carried on before, is played as it is (inFolder). The
// song after the one with the shell is fetched while that one plays; one
// wanted at once (that one ended first, or next was pressed) is waited for,
// the shell stopped meanwhile. With nothing else to fetch, the covers the row is to show come
// next, for the songs on the card too: this song's, then the next one's. A
// download's own cover, or the one of the song before of the same album
// (coverFrom), is never fetched. One fetch at a time, on a thread of its own,
// through Sony's libraries as SceShell has them. The server comes from
// VitaPlex's settings, and the token too, read for each fetch and wiped after
// it. A song in a format the shell does not play, such as FLAC, comes as the
// server's MP3 of it, behind a length header (fetchMp3).
enum { kFetchIdle, kFetchRunning, kFetchDone, kFetchFailed };
enum { kJobSong, kJobCover };  // a song's file and its cover, or its cover alone
const int kFetchStack = 0x8000;
const int kFetchStopped = 1;  // fetchBody's: stopped by a cancel

// Under g_lock. t is the fetch thread's as it runs, but for its place and key.
struct Fetch {
    int state;    // kFetch*
    int kind;     // kJob*
    int cancel;   // set to stop it
    int coverDone;  // a song's job fetched its cover, or found it would not come
    int ends;     // fetches ended so far
    int forGood;  // failed in a way another try would not mend
    int unreached;  // failed with no answer from the server (g_noAnswer), or none asked for
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
// The fetch thread's: a request for its song got no answer from the server,
// failing before any came, as all did with Wi-Fi off (0x80412113 in log
// 0790f7dc).
int g_noAnswer;

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
    const int n = readFile(HANDOVER_DIR "settings.json", buf, cap);
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

// The fetch thread's framer for the server's MP3 of a song (fetchMp3), and
// where what it passes on goes: the file.
Mp3Frames g_mp3;
int mp3Sink(void* ctx, const uint8_t* data, uint32_t n) {
    const SceUID fd = *(const SceUID*)ctx;
    return sceIoWrite(fd, data, (SceSize)n) == (int)n ? 0 : -1;
}

// n bytes over the start of the file at path, opened again for it: the
// plugin has no seek of its own, sceIoLseek not being among its imports.
int writeAtStart(const char* path, const uint8_t* data, uint32_t n) {
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY, 0);
    if (fd < 0) return fd;
    const int w = sceIoWrite(fd, data, (SceSize)n);
    const int rc = sceIoClose(fd);
    return w != (int)n ? (w < 0 ? w : -1) : rc;
}

// The body of req: into fd a chunk at a time through g_fetchBuf, framed as
// an MP3 on the way when mp3 is given (mp3_frames.h), or with fd < 0 into
// g_fetchBuf, as much as fits, ended with 0. Its first 8 bytes into first,
// if given; a file's progress logged every 10 s under label, if given. 0 at
// the body's end (or with g_fetchBuf full), kFetchStopped at a cancel, or the
// code of the read or write that failed; the bytes in *got.
int fetchBody(int req, SceUID fd, const char* label, uint8_t* first, uint32_t* got,
              Mp3Frames* mp3 = NULL) {
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
        if (fd >= 0 && mp3) {
            if (mp3fFeed(mp3, to, (uint32_t)n, mp3Sink, &fd) < 0) {
                rc = -1;
                break;
            }
        } else if (fd >= 0) {
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
    if (coverPath(t, path, sizeof(path))) return 1;
    if (!t->thumb[0]) return 0;
    sceClibSnprintf(path, sizeof(path), QM_SONG_COVER_FMT, t->key);
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

// What g_fetchUrl answers, whole, into the file `to`, through a file beside
// it, moved in once all of it has come; framed on the way when mp3 is given,
// the frame made at the end over the room left for it. The bytes that came
// in *size. kFetchDone, or kFetchFailed with why, and *forGood when another
// try would fail the same way.
int fetchFile(int tmpl, const char* token, const char* label, const char* to, uint32_t* size,
              char* why, int cap, int* forGood, Mp3Frames* mp3 = NULL) {
    char* const part = g_fetchPart;
    sceClibSnprintf(part, sizeof(g_fetchPart), "%s.part", to);
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
        if (status == 200) rc = fetchBody(req, fd, label, NULL, &got, mp3);
        httpClose(conn, req);
    } else {
        g_noAnswer = 1;
    }
    const int rcClose = sceIoClose(fd);
    // An MP3's frame, over the room at its start; none when the server's
    // first frame is one of its own.
    int made = 0;
    if (mp3 && rc == 0 && status == 200 && rcClose >= 0) {
        made = mp3fFinish(mp3);
        if (made > 0) {
            const int rcHead = writeAtStart(part, mp3->frame, mp3->room);
            if (rcHead < 0) {
                sceClibSnprintf(why, cap, "the frame giving its length not written (0x%08X)",
                                (unsigned)rcHead);
                sceIoRemove(part);
                return kFetchFailed;
            }
        } else if (made < 0) {
            sceClibSnprintf(why, cap, "no MP3 frame in the server's MP3 of it (%s MB)",
                            mbOf(got).s);
            *forGood = 1;
            sceIoRemove(part);
            return kFetchFailed;
        }
    }
    // In 32 bits: a song comes in well under 71 minutes.
    const uint32_t ms = (uint32_t)(sceKernelGetSystemTimeWide() - t0) / 1000u;
    // The length the server gave, unless it gave none or 4 GB or more.
    const bool sized = rcLength >= 0 && (length >> 32) == 0;
    if (rc == 0 && status == 200 && got > 0 && rcClose >= 0 && (!sized || (uint32_t)length == got)) {
        logLine("carry on: %s: fetched, %s MB in %s s (%u KB/s)", label, mbOf(got).s, secsOf(ms).s,
                (unsigned)((got >> 10) * 1000u / (ms ? ms : 1)));
        *size = got;
        sceIoRemove(to);
        rc = sceIoRename(part, to);
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

// ── The server's MP3 of a song in another format ────────────────────────────
// As the background player had it (player.c's download and giveHeader, in
// the history): a decision first, then start.mp3 with the same session. A
// streamed transcode has no length header, so the frames go to the shell
// behind a Xing frame giving their count, and without the ID3 tag
// (mp3_frames.h), framed as they come: up to build 2047, two passes over the
// whole file after it had come took 9.6 s of a 23.3 s wait (log 8e7a4b23).
// The framer is the fetch thread's (g_mp3).

// The server's MP3 of the song at key, as player.c's transcodePath asks for
// it, into g_fetchUrl: `which` is "decision" or "start.mp3", with one session
// for both. The token goes as a header (httpOpen), never in the address.
void transcodeUrl(const char* server, const char* key, const char* which, uint32_t session) {
    static char path[64], extra[200];
    char metadata[40];
    sceClibSnprintf(metadata, sizeof(metadata), "/library/metadata/%s", key);
    urlEncode(metadata, path, sizeof(path));
    urlEncode("add-transcode-target(type=musicProfile&context=streaming&protocol=http"
              "&container=mp3&audioCodec=mp3)",
              extra, sizeof(extra));
    sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl),
                    "%s/music/:/transcode/universal/%s?path=%s&mediaIndex=0&partIndex=0"
                    "&directPlay=0&directStream=1&directStreamAudio=1&hasMDE=1&location=lan"
                    "&audioBoost=100&audioChannelCount=2&protocol=http&musicBitrate=320"
                    "&session=vitaplex-sceshell-%s-%u&X-Plex-Client-Profile-Name=Generic"
                    "&X-Plex-Client-Profile-Extra=%s",
                    server, which, path, key, (unsigned)session, extra);
}

// The song as the server's MP3 into t's path: the decision, then start.mp3
// framed as it comes into a file beside it. As fetchFile returns.
int fetchMp3(int tmpl, const char* server, const char* token, const char* label, char* why,
             int cap, int* forGood) {
    Track* const t = &g_fetch.t;
    const uint32_t session = (uint32_t)g_fetch.ends + 1;
    transcodeUrl(server, t->key, "decision", session);
    int conn, req, status = 0;
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", label, g_fetchUrl, token, NULL, &conn, &req);
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the server did not answer the transcode decision (0x%08X)",
                        (unsigned)rc);
        g_noAnswer = 1;
        return kFetchFailed;
    }
    httpFn<FnStatus>(kHttpStatus)(req, &status);
    rc = fetchBody(req, -1, NULL, NULL, &got);
    httpClose(conn, req);
    if (rc == kFetchStopped) {
        sceClibSnprintf(why, cap, "stopped");
        return kFetchFailed;
    }
    char text[96] = "";
    const char* const mc = findText((const char*)g_fetchBuf, "<MediaContainer ");
    if (mc) xmlAttr(mc, "generalDecisionText", text, sizeof(text));
    logLine("carry on: %s: the server's MP3 of it: decision HTTP %d%s%s", label, status,
            text[0] ? ": " : "", text);
    // As the player does, start.mp3 whatever the decision: its answer says.
    transcodeUrl(server, t->key, "start.mp3", session);
    mp3fInit(&g_mp3);
    uint32_t size = 0;
    if (fetchFile(tmpl, token, label, t->path, &size, why, cap, forGood, &g_mp3) != kFetchDone)
        return kFetchFailed;
    const uint32_t ms = mp3fMs(&g_mp3);
    if (g_mp3.tag[0]) {
        logLine("carry on: %s: the server's MP3 has a %s header already: %s", label, g_mp3.tag,
                mmss(ms).s);
    } else {
        // The length from the first frame's bitrate alone, as the shell would
        // have it: every byte from that frame on, at that bitrate.
        const uint32_t guess = (size - g_mp3.firstAt) * 8u / g_mp3.firstKbps;
        logLine("carry on: %s: %u frames make %s; the first frame's %u kbps alone would make it "
                "%s, so it goes to the shell behind a header, without the %u-byte ID3 tag",
                label, (unsigned)g_mp3.frames, mmss(ms).s, (unsigned)g_mp3.firstKbps,
                mmss(guess).s, (unsigned)g_mp3.id3Size);
    }
    return kFetchDone;
}

// The fetch thread's work on g_fetch.t, under label: its cover first, which
// the row shows while the song is waited for (with it after the file, the
// row had the heading's own icon all the while: log 8e7a4b23); then its file
// found on the server as player.c's fetchDetails finds it, the first Part of
// the Track under its key, fetched as it is if the shell plays its format,
// else as the server's MP3 of it. kFetchDone with the file at t.path; else
// kFetchFailed, as fetchFile.
int fetchSongNow(int tmpl, const char* server, const char* token, const char* label, char* why,
                 int cap, int* forGood) {
    Track* const t = &g_fetch.t;
    static char partKey[256];
    if (fetchCover(tmpl, server, token, label) < 0) {
        sceClibSnprintf(why, cap, "stopped");
        return kFetchFailed;
    }
    g_fetch.coverDone = 1;
    sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl), "%s/library/metadata/%s", server, t->key);
    int conn, req, status = 0;
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", label, g_fetchUrl, token, NULL, &conn, &req);
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the server did not answer (0x%08X)", (unsigned)rc);
        g_noAnswer = 1;
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
    const bool asItIs = shellExt(ext);
    sceClibSnprintf(t->path, sizeof(t->path), "%s/%s.%s", HANDOVER_SONGS, t->key,
                    asItIs ? ext : "mp3");
    if (exists(t->path)) {
        logLine("carry on: %s: already on the memory card, at %s", label, t->path);
    } else if (asItIs) {
        logLine("carry on: %s: a %s file of %s MB on the server; fetching it to %s", label, ext,
                mbOf(toUint(num)).s, t->path);
        sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl), "%s%s?download=1", server, partKey);
        uint32_t size = 0;
        if (fetchFile(tmpl, token, label, t->path, &size, why, cap, forGood) != kFetchDone) {
            t->path[0] = 0;
            return kFetchFailed;
        }
    } else {
        logLine("carry on: %s: its file on the server (%s, %s MB) is not one the shell plays; "
                "fetching the server's MP3 of it to %s",
                label, ext[0] ? ext : "no extension", mbOf(toUint(num)).s, t->path);
        if (fetchMp3(tmpl, server, token, label, why, cap, forGood) != kFetchDone) {
            t->path[0] = 0;
            return kFetchFailed;
        }
    }
    return kFetchDone;
}

// The fetch thread: g_fetch.t fetched, or its cover alone, the outcome left
// in g_fetch for the music thread (fetchPoll).
int fetchThread(SceSize, void*) {
    static char server[256], token[160], label[32], why[96];
    sceClibSnprintf(label, sizeof(label), "song %d of %d", g_fetch.t.index + 1, g_fetch.count);
    why[0] = 0;
    int forGood = 0, result = kFetchFailed;
    bool said = false;   // what came of it logged already
    bool canAsk = false;  // the server and the libraries there to ask with
    g_noAnswer = 0;
    if (!readServer((char*)g_fetchBuf, sizeof(g_fetchBuf), server, sizeof(server), token,
                    sizeof(token))) {
        sceClibSnprintf(why, sizeof(why), "no server in VitaPlex's settings");
    } else {
        if (!g_netFound) g_netFound = findLibraries("carry on");
        int rcSsl = -1, rcHttp = -1;
        if (!g_netFound) {
            sceClibSnprintf(why, sizeof(why),
                            "Sony's SSL and HTTP libraries are not all there in SceShell");
        } else if (!httpStart("carry on", &rcSsl, &rcHttp)) {
            sceClibSnprintf(why, sizeof(why), "the HTTP library would not start");
        } else {
            const int tmpl = httpTemplate("carry on");
            canAsk = tmpl >= 0;
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
        httpEnd(rcSsl, rcHttp);
    }
    sceClibMemset(token, 0, sizeof(token));
    if (result != kFetchDone && !said)
        logLine("carry on: %s: %snot fetched: %s", label,
                g_fetch.kind == kJobCover ? "its cover " : "", why);
    const bool stopped = result != kFetchDone && startsWith(why, "stopped");
    lock();
    g_fetch.state = result;
    g_fetch.forGood = forGood;
    // No other song would come either (Carry::offlineSkip).
    g_fetch.unreached = result != kFetchDone && !forGood && !stopped && (!canAsk || g_noAnswer);
    g_fetch.stopped = stopped;
    g_fetch.ends++;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// Whether g_fetch is a job of this kind for t. Under g_lock.
bool fetchIsFor(const Track* t, int kind) {
    return g_fetch.state != kFetchIdle && g_fetch.kind == kind && g_fetch.t.index == t->index &&
           same(g_fetch.t.key, t->key);
}

// The song fetched here and not played, left on the card for VitaPlex as it
// takes the music back, and forgotten: next waited 22.6 s for a song fetched
// here and removed at the take-back (log 8e7a4b23).
void forgetFetched() {
    if (g_c.fetchedIndex >= 0)
        logLine("carry on: song %d, fetched and not played, left for VitaPlex", g_c.fetchedIndex + 1);
    g_c.fetchedIndex = -1;
    g_c.fetchedPath[0] = 0;
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
    const bool later = running;
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
    g_fetch.coverDone = 0;
    g_fetch.forGood = 0;
    g_fetch.unreached = 0;
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
        *card = anyOnCard(out);
        if (*card || !ringHas(&g_c.lost, i)) return i;
    }
    return -1;
}

// Whether t's cover for the row is yet to be fetched: on the server, not on
// the card, and not asked for before.
bool wantsCover(const Track* t) {
    if (!t->thumb[0] || !t->key[0] || ringHas(&g_c.coverTried, t->index)) return false;
    return !coverFrom(t, &g_c.cur);
}

// What to fetch while the song with the shell plays, if anything: the next
// song's file when it is not on the card, unless it failed ahead already
// (it is fetched again, waited for, when it is next); else, with nothing
// running, the covers the row is to show, this song's and then the next
// one's. Nothing while waiting, nor a song ahead for repeat one.
void fetchAhead() {
    g_c.aheadDue = 0;
    if (!g_server[0] || g_c.waitIndex >= 0) return;
    bool card = false;
    const int next = g_c.repeat == HANDOVER_REPEAT_ONE ? -1 : nextSong(&g_ahead, &card);
    takeFetched();
    lock();
    const bool running = g_fetch.state == kFetchRunning;
    const bool inHand =
        next >= 0 && ((g_c.fetchedIndex == next && same(g_c.fetchedKey, g_ahead.key)) ||
                      (running && fetchIsFor(&g_ahead, kJobSong)));
    unlock();
    if (next >= 0 && !card && !inHand && !ringHas(&g_c.failedAhead, next)) {
        coverFrom(&g_ahead, &g_c.cur);
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
// with "fetching=1", so that VitaPlex takes the music back from its start.
void waitFor(const Track* t, const char* why) {
    const int rcStop = musicEvent(kEventStop);
    g_c.wait = *t;
    g_c.waitIndex = t->index;
    g_c.waitUs = sceKernelGetSystemTimeWide();
    g_c.waitTries = 0;
    g_c.waitWhy = why;
    g_c.waitPlayUs = 0;
    g_c.held = 0;
    g_c.menuPaused = 0;
    g_c.resuming = 0;
    // No song plays meanwhile: the music thread back to its usual looks.
    g_c.fastUntilUs = 0;
    g_c.backUs = 0;
    logLine("carry on: song %d of %d (%s), \"%s\", is not on the memory card: the shell stopped "
            "(0x%08X) until it is fetched",
            t->index + 1, g_c.count, why, t->title, (unsigned)rcStop);
    coverFrom(t, &g_c.cur);
    showTrack(t);
    char cover[160];
    g_c.waitArt = coverPath(t, cover, sizeof(cover));
    carryStatusOf(t, HANDOVER_STATE_PLAYING, startAt(t), true);
    report(t, kBuffering, startAt(t));
    lock();
    const bool under = g_fetch.state == kFetchRunning && fetchIsFor(t, kJobSong) && !g_fetch.cancel;
    unlock();
    if (!under) {
        g_c.waitTries = 1;
        fetchStart(t, kJobSong);
    }
}

// ── Telling the server ──────────────────────────────────────────────────────
// Two things go to VitaPlex's server from here, on a thread of its own that
// sends what is waiting and ends, so that a slow or missing server holds
// nothing up: not the music, nor the next song's fetch. Through the
// libraries the fetch finds, with the token read for each run and wiped
// after it.
//
// - A song that played to its end is marked played, as the background
//   player marked it (player.c's scrobble).
// - Where the music is, as VitaPlex tells it while it plays
//   (PlexClient::reportTimeline, from PlayerActivity): the song, playing,
//   paused, buffering while it is fetched, or stopped, and where in it, at
//   each change and every 10 s, under VitaPlex's own client id, so that Plex
//   shows the music going on as VitaPlex's and keeps its place. Only the
//   latest is kept: a server that does not answer is never more than one
//   behind, and its failure is logged once until one is taken.
const int kScrobbles = 8;
struct Scrobble {
    char key[16];  // the song's ratingKey
    int index;     // its place, for the log
};
const char* const kVitaPlexClient = "vita-plex-client-001";  // application.hpp's PLEX_CLIENT_ID
struct Report {
    char key[16];
    int index;  // for the log
    const char* state;
    uint32_t timeMs, durationMs;
    int due;
};
// Under g_lock: the songs waiting to be marked, from g_scrobbleFirst on; the
// report waiting; and whether a thread is sending them.
Scrobble g_scrobbles[kScrobbles];
int g_scrobbleFirst, g_scrobbleCount, g_serving;
Report g_report;
// The server thread's: VitaPlex's settings as read, and whether the last
// report was not taken (logged once).
char g_serverSettings[2048];
int g_reportFailed;

// A GET of url for what; the server's status, or 0 when it never answered,
// the call's code then in *rc. Its failure to be sent is logged unless quiet.
int serverGet(int tmpl, const char* what, const char* url, const char* token, const char* client,
              int* rc, bool quiet = false) {
    int conn, req, status = 0;
    *rc = httpOpen(tmpl, quiet ? NULL : "carry on", what, url, token, NULL, &conn, &req, client);
    if (*rc >= 0) {
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        httpClose(conn, req);
    }
    return status;
}

// The server thread's: each song waiting marked, then the report waiting
// sent, until none is; each said not to be when the server or the libraries
// are not there.
void serverWaiting() {
    static char server[256], token[160], url[400];
    int rcSsl = -1, rcHttp = -1, tmpl = -1;
    if (readServer(g_serverSettings, sizeof(g_serverSettings), server, sizeof(server), token,
                   sizeof(token))) {
        if (!g_netFound) g_netFound = findLibraries("carry on");
        if (g_netFound && httpStart("carry on", &rcSsl, &rcHttp)) tmpl = httpTemplate("carry on");
    }
    const char* const none = server[0] ? "no HTTP library" : "no server in VitaPlex's settings";
    for (;;) {
        Scrobble one;
        Report now;
        lock();
        const bool marking = g_scrobbleCount > 0;
        if (marking) {
            one = g_scrobbles[g_scrobbleFirst];
            g_scrobbleFirst = (g_scrobbleFirst + 1) % kScrobbles;
            g_scrobbleCount--;
        }
        const bool reporting = !marking && g_report.due;
        if (reporting) {
            now = g_report;
            g_report.due = 0;
        }
        unlock();
        int rc = 0;
        if (marking) {
            if (tmpl < 0) {
                logLine("carry on: song %d not marked played: %s", one.index + 1, none);
                continue;
            }
            sceClibSnprintf(url, sizeof(url),
                            "%s/:/scrobble?key=%s&identifier=com.plexapp.plugins.library", server,
                            one.key);
            const int status = serverGet(tmpl, "marking a song played", url, token,
                                         "vitaplex-sceshell", &rc);
            if (status == 200)
                logLine("carry on: song %d marked played", one.index + 1);
            else
                logLine("carry on: song %d not marked played: HTTP %d (0x%08X)", one.index + 1,
                        status, (unsigned)rc);
        } else if (reporting) {
            int status = 0;
            if (tmpl >= 0) {
                sceClibSnprintf(url, sizeof(url),
                                "%s/:/timeline?ratingKey=%s&key=/library/metadata/%s&state=%s"
                                "&time=%u&duration=%u",
                                server, now.key, now.key, now.state, (unsigned)now.timeMs,
                                (unsigned)now.durationMs);
                // Quiet once failed: with Wi-Fi off each report failed, one
                // every 10 s as the songs on the card played (log 0790f7dc),
                // and the log stops at kLogMax.
                status = serverGet(tmpl, "telling the server where the music is", url, token,
                                   kVitaPlexClient, &rc, g_reportFailed != 0);
            }
            if (status == 200 && g_reportFailed) {
                g_reportFailed = 0;
                logLine("carry on: the server told where the music is again: song %d %s at %s",
                        now.index + 1, now.state, mmss(now.timeMs).s);
            } else if (status != 200 && !g_reportFailed) {
                g_reportFailed = 1;
                if (tmpl < 0)
                    logLine("carry on: the server not told where the music is: %s", none);
                else
                    logLine("carry on: the server not told where the music is: HTTP %d (0x%08X)",
                            status, (unsigned)rc);
            }
        } else {
            break;
        }
    }
    if (tmpl >= 0) httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    httpEnd(rcSsl, rcHttp);
    sceClibMemset(token, 0, sizeof(token));
}

int serverThread(SceSize, void*) {
    for (;;) {
        serverWaiting();
        // Ended only with nothing left waiting, under the lock that anything
        // added since would have been put in under.
        lock();
        const bool more = g_scrobbleCount > 0 || g_report.due;
        if (!more) g_serving = 0;
        unlock();
        if (!more) break;
    }
    return sceKernelExitDeleteThread(0);
}

// The server thread started, when the caller found none running (g_serving
// set under g_lock). The music thread's.
void startServing() {
    const SceUID thread = sceKernelCreateThread("VitaPlexQM::Server", serverThread,
                                                kThreadPriority, kFetchStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        lock();
        const int dropped = g_scrobbleCount;
        g_scrobbleCount = 0;
        g_report.due = 0;
        g_serving = 0;
        unlock();
        logLine("carry on: nothing told the server: no thread for it (0x%08X); %d song(s) not "
                "marked played",
                (unsigned)rc, dropped);
    }
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
        start = !g_serving;
        if (start) g_serving = 1;
    }
    unlock();
    if (full) {
        logLine("carry on: song %d not marked played: %d songs are waiting to be already",
                t->index + 1, kScrobbles);
        return;
    }
    if (start) startServing();
}

// The server told that t is in `state` (kPlaying...), at timeMs, in place of
// any report not yet sent. The music thread's.
void report(const Track* t, const char* state, uint32_t timeMs) {
    g_c.reportState = state;
    g_c.reportUs = sceKernelGetSystemTimeWide();
    if (!g_server[0] || !t->key[0]) return;
    const uint32_t len = t->durationMs > 0 ? t->durationMs : g_c.lenMs;
    // Plex refuses a time past the song's length (PlayerActivity's clamp).
    if (len > 0 && timeMs > len) timeMs = len;
    lock();
    sceClibSnprintf(g_report.key, sizeof(g_report.key), "%s", t->key);
    g_report.index = t->index;
    g_report.state = state;
    g_report.timeMs = timeMs;
    g_report.durationMs = len;
    g_report.due = 1;
    const bool start = !g_serving;
    if (start) g_serving = 1;
    unlock();
    if (start) startServing();
}

// Stops carrying the queue on (kEnd*). The row goes back to SceShell unless a
// process has taken it. A fetch under way is stopped, and a song fetched and
// not played removed.
void carryEnd(const char* why, int how) {
    int state = 0;
    uint32_t time = 0;
    musicStatus(&state, &time);
    // VitaPlex takes the music back with its player ready, and plays on from
    // the place written below: the shell is stopped first, so that the status
    // it reads is never one with the shell still playing. Up to build 2048
    // the song was left for VitaPlex to stop, which it never did when it went
    // before it got that far, and the song played on in the shell with
    // nothing looking after it (log 7c38c1fa).
    int rcStop = 0, rcEnd = 0;
    if (how == kEndTakeBack) {
        rcStop = musicEvent(kEventStop);
        rcEnd = musicTracked(0x30001);
    }
    // The queue has gone on to a song being fetched: the music plays, as far
    // as VitaPlex is to know, from that song's start.
    const bool waiting = g_c.waitIndex >= 0;
    const char* written = "nothing";
    if (how == kEndTakeBack || how == kEndRowBack) {
        written = state == 1 || waiting ? HANDOVER_STATE_PLAYING : HANDOVER_STATE_STOPPED;
    } else if (how == kEndOther || how == kEndStopped) {
        written = HANDOVER_STATE_STOPPED;
    } else if (how == kEndOver) {
        written = HANDOVER_STATE_OVER;
    }
    if (how != kEndYield) {
        if (waiting)
            carryStatusOf(&g_c.wait, written, startAt(&g_c.wait), true);
        else
            carryStatus(written, time);
    }
    // The server told the music has stopped, unless VitaPlex goes on with it
    // and says so itself.
    if (how == kEndOver || how == kEndStopped || how == kEndOther) {
        if (waiting)
            report(&g_c.wait, kStopped, startAt(&g_c.wait));
        else
            report(&g_c.cur, kStopped, time);
    }
    lock();
    const bool fetching = g_fetch.state == kFetchRunning;
    if (fetching) __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
    unlock();
    // Taken back, the songs' files stay: VitaPlex plays on from them.
    if (how == kEndTakeBack || how == kEndRowBack) {
        takeFetched();
        forgetFetched();
    } else {
        dropFetched();
    }
    if (how == kEndOver || how == kEndStopped) {
        rcStop = musicEvent(kEventStop);
        rcEnd = musicTracked(0x30001);
        removeOwn(&g_c.cur);
        if (!same(g_c.prev.path, g_c.cur.path)) removeOwn(&g_c.prev);
    }
    char also[64] = "";
    if (waiting)
        sceClibSnprintf(also, sizeof(also), ", song %d to come once fetched", g_c.waitIndex + 1);
    logLine("carry on: stopped at song %d of %d, %s, state %d%s (%s): status %s; stop 0x%08X, "
            "end 0x%08X%s",
            g_c.cur.index + 1, g_c.count, mmss(time).s, state, also, why, written,
            (unsigned)rcStop, (unsigned)rcEnd, fetching ? "; its fetch stopped" : "");
    g_c.waitIndex = -1;
    g_c.fastUntilUs = 0;  // the music thread back to its usual looks
    lock();
    g_carry = 0;
    g_carryStop = NULL;
    if (g_owner == kOwnerShell) {
        g_active = 0;
        g_releaseDue = 1;
        g_artSerial = 0;
        g_playing = -1;
        logLine("carry on: row given back");
        notesWanted();
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

void seekTo(uint32_t ms);

// Hands t to the shell: stop, open and play, as the player did. true when it
// took the file.
bool carryPlay(const Track* t, const char* why) {
    // VitaPlex's place is for its own song alone.
    if (t->index != g_c.startIndex) g_c.startAtMs = 0;
    const uint32_t at = startAt(t);
    const int rcStop = musicEvent(kEventStop), rcOpen = musicOpen(t->path);
    // The place set before play too: should the shell take it then, the
    // song begins there. A seek event right after play went unheeded, and a
    // second of the song's start played before the one after it took (log
    // d6e76980, build 2048).
    const int rcFirst = at >= 2000 ? musicSeekTime(at) : 0;
    const int rcPlay = musicEvent(kEventPlay);
    char first[48] = "";
    if (at >= 2000) sceClibSnprintf(first, sizeof(first), ", seek time first 0x%08X", (unsigned)rcFirst);
    logLine("carry on: song %d of %d (%s), \"%s\" by %s, %s, %s: stop 0x%08X, open 0x%08X%s, "
            "play 0x%08X",
            t->index + 1, g_c.count, why, t->title, t->artist, mmss(t->durationMs).s, t->path,
            (unsigned)rcStop, (unsigned)rcOpen, first, (unsigned)rcPlay);
    if (rcOpen < 0) return false;
    coverFrom(t, &g_c.cur);
    // The song before keeps its file, for previous; the one before that
    // goes, unless the shell has it again or it is the one fetched next:
    // previous waited 37.5 s for a song whose file had gone (log 8e7a4b23).
    if (!same(g_c.cur.path, t->path)) {
        if (!same(g_c.prev.path, t->path) && !same(g_c.prev.path, g_c.cur.path) &&
            !same(g_c.prev.path, g_c.fetchedPath))
            removeOwn(&g_c.prev);
        g_c.prev = g_c.cur;
    }
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
    g_c.held = 0;
    g_c.aheadDue = 1;  // the song after it fetched, if it is not on the card
    g_c.seekWantMs = 0;
    g_c.seekTries = 0;
    g_c.reportState = NULL;  // told once it plays
    g_c.fastUntilUs = 0;
    g_c.backUs = 0;
    // VitaPlex's place, asked for at once as well, so that the song's start
    // is not heard first (1.3 s of it in log 8e7a4b23). The shell did not
    // take this seek in log d6e76980; so the music thread looks every 25 ms
    // until it plays, and carryTick asks again the moment it does.
    if (at >= 2000) {
        seekTo(at);
        g_c.fastUntilUs = g_c.songUs + kFastLookUs;
    }
    // A fetched song, played: its file is the shell's now, removed once the
    // shell is two songs on.
    takeFetched();
    if (g_c.fetchedIndex >= 0 && same(g_c.fetchedPath, t->path)) {
        g_c.fetchedIndex = -1;
        g_c.fetchedPath[0] = 0;
    }
    showTrack(t);
    carryStatus(HANDOVER_STATE_PLAYING, startAt(t));
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
        if (fetchedFor(&g_found) || anyOnCard(&g_found)) return kGoPlay;
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
        if (ended && g_c.repeat == HANDOVER_REPEAT_ONE && g_c.cur.path[0] && tries == 0) {
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

// Whether t is on the memory card, with its file in t->path: its download,
// its file fetched here, or its file in the folder of songs. The folder keeps
// the files of the songs near the one playing alone (handover.h), and is
// looked in for those, `near`, and for the songs the shell has and had,
// wherever they are in the queue.
bool cardHas(Track* t, bool near) {
    if (onCard(t) || fetchedFor(t)) return true;
    if (near) return inFolder(t);
    const Track* const had[2] = {&g_c.cur, &g_c.prev};
    for (int k = 0; k < 2; k++) {
        const Track* const h = had[k];
        if (h->index == t->index && same(h->key, t->key) && h->path[0] && exists(h->path)) {
            sceClibSnprintf(t->path, sizeof(t->path), "%s", h->path);
            t->own = h->own;
            return true;
        }
    }
    return false;
}

// The nearest song after `from` in the queue's order that is on the memory
// card (cardHas), repeat all going round, into *out: from one read of the
// queue, as a thousand songs can be between. Its place, or -1 with none.
int nextOnCard(int from, Track* out) {
    QueueScan s;
    if (!scanOpen(&s)) return -1;
    const bool round = g_c.repeat == HANDOVER_REPEAT_ALL;
    int found = -1;
    bool after = false;  // found after `from`, not going round
    const char* fields;
    while (!after && scanNext(&s, &fields)) {
        const int i = s.index;
        // Before `from` only going round, and the first found there alone.
        if (i == from || (i < from && (!round || found >= 0))) continue;
        const int ahead = i > from ? i - from : i - from + g_c.count;
        parseTrack(fields, i, &g_scan);
        if (!cardHas(&g_scan, ahead <= kLookAhead || g_c.count - ahead <= kLookAhead)) continue;
        *out = g_scan;
        found = i;
        after = i > from;
    }
    readerClose();
    return found;
}

// The song waited for got no answer from the server, as each did with Wi-Fi
// off, and the queue stopped after three (log 0790f7dc, build 2052). With
// VitaPlex's setting (offlineSkip) the queue goes on from the nearest song
// after it on the memory card, from its start; with none, the song is held,
// paused: the row's play fetches it again, and VitaPlex takes the music back
// from it.
void carryOnCard() {
    const int from = g_c.waitIndex;
    const int i = nextOnCard(from, &g_found);
    if (i >= 0) {
        logLine("carry on: song %d of %d, \"%s\", could not be fetched, the server not answering: "
                "on to song %d, on the memory card",
                from + 1, g_c.count, g_c.wait.title, i + 1);
        if (carryPlay(&g_found, "on the memory card, the server not answering")) return;
    }
    g_c.held = 1;
    g_c.menuPaused = 1;
    lock();
    g_playing = 0;
    unlock();
    logLine("carry on: song %d of %d, \"%s\", could not be fetched, the server not answering, and "
            "%s: held, paused, for the row's play or VitaPlex",
            from + 1, g_c.count, g_c.wait.title,
            i >= 0 ? "the song on the memory card would not open"
                   : "no song after it is on the memory card");
    carryStatusOf(&g_c.wait, HANDOVER_STATE_PLAYING, startAt(&g_c.wait), true);
    report(&g_c.wait, kPaused, startAt(&g_c.wait));
}

// VitaPlex's queue, handed to SceShell as it goes (QM_CMD_PLAY_QUEUE):
// played from its song and place there, and the row SceShell's from now. That
// song plays from the file VitaPlex made ready, else from its download,
// else from the server, waited for; and if it can be none of them, the next
// that can is played from its start. true when it
// carries on (g_carry is 2 then, or 0 again if the queue had nothing to
// play); else nothing has changed but the row's owner.
bool carryStartQueue() {
    QueueScan s;
    if (!scanOpen(&s)) {
        logLine("play queue: no queue at %s", HANDOVER_QUEUE);
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
    g_c.offlineSkip = s.offlineSkip;
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
    logLine("play queue: song %d of %d, \"%s\" by %s, %s, from %s; repeat %d; offline skip %s; "
            "music service started: 0x%08X",
            s.start + 1, g_c.count, g_c.cur.title, g_c.cur.artist, mmss(s.startMs).s,
            s.file[0] ? s.file : "the queue", g_c.repeat, g_c.offlineSkip ? "on" : "off",
            (unsigned)rcStart);
    if (rcStart < 0) return false;
    carryServer();
    g_c.startIndex = s.start;
    g_c.startAtMs = s.startMs;
    lock();
    g_owner = kOwnerShell;
    g_active = 1;
    g_carry = 2;
    notesWanted();
    unlock();
    // Nothing is with the shell yet: the song is played from here.
    g_found = g_c.cur;
    g_c.cur.path[0] = 0;
    g_c.cur.own = 0;
    if (s.file[0] && exists(s.file)) {
        sceClibSnprintf(g_found.path, sizeof(g_found.path), "%s", s.file);
        g_found.own = startsWith(s.file, HANDOVER_SONGS "/");
    } else if (!anyOnCard(&g_found)) {
        g_found.path[0] = 0;
    }
    if (g_found.path[0]) {
        if (!carryPlay(&g_found, "VitaPlex's song"))
            carryNext(s.start, false, "VitaPlex's song would not open");
    } else if (g_server[0]) {
        waitFor(&g_found, "VitaPlex's song");
    } else {
        logLine("play queue: song %d of %d, \"%s\", is not on the memory card, and there is no "
                "server to fetch it from",
                s.start + 1, g_c.count, g_found.title);
        carryNext(s.start, false, "VitaPlex's song is not on the memory card");
    }
    g_c.aheadDue = 1;
    return true;
}

// Takes the song to ms, VitaPlex's place in it: the seek time, then the
// seek event, as player.c's seekFrom; carryTick watches where it lands.
void seekTo(uint32_t ms) {
    const int rcTime = musicSeekTime(ms), rcSeek = musicEvent(kEventSeek);
    logLine("carry on: song %d of %d to %s, where VitaPlex was: seek time 0x%08X, seek 0x%08X",
            g_c.cur.index + 1, g_c.count, mmss(ms).s, (unsigned)rcTime, (unsigned)rcSeek);
    g_c.seekWantMs = ms;
    g_c.seekUs = sceKernelGetSystemTimeWide();
    g_c.seekTries++;
    g_c.maxTime = 0;
    g_c.playMs = ms;
    g_c.havePrev = 0;
    g_c.backUs = 0;
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
    g_c.backUs = 0;
}

// A press on the row, as player.c's watchSong takes them: play/pause stops
// the shell and plays it again, which keeps its place; previous goes back to
// the start past a song's first 3 s, and to the song before within them,
// fetched first if it is not on the card; next goes on past repeat one.
void carryPress(int press, int state, uint32_t time) {
    g_c.backUs = 0;  // where the song goes now is the row's doing
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
                if (fetchedFor(&g_found) || anyOnCard(&g_found)) {
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
// tried again once if its fetch fails, and else passed over, or, with no
// answer from the server, the queue goes on from the songs on the card or
// holds it (carryOnCard). The row's buttons act on it: play/pause holds it
// back once fetched, and fetches a song held again; previous goes back to the
// song before from its start, next passes it over.
void carryWait() {
    // Written every 2 s while it waits, as while it plays: VitaPlex takes a
    // status not written for a while for a queue SceShell no longer carries
    // on.
    if ((uint32_t)(sceKernelGetSystemTimeWide() - g_c.statusUs) >= 2000000u)
        carryStatusOf(&g_c.wait, HANDOVER_STATE_PLAYING, startAt(&g_c.wait), true);
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
        // Held as the server did not answer: fetched again, twice at most.
        if (!g_c.menuPaused && g_c.held) {
            g_c.held = 0;
            g_c.waitTries = 0;
        }
    }
    // Something else played in the shell meanwhile, as the system's Music
    // app would play it, seen for half a second: not for this to stop. Timed,
    // as looks can come 25 ms apart.
    int state = 0;
    uint32_t time = 0;
    if (musicStatus(&state, &time) >= 0 && state == 1) {
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        if (!g_c.waitPlayUs) {
            g_c.waitPlayUs = now;
        } else if ((uint32_t)(now - g_c.waitPlayUs) >= 500000u) {
            carryEnd("something else plays in the shell", kEndOther);
            return;
        }
    } else {
        g_c.waitPlayUs = 0;
    }
    // Its cover, fetched before its file, on the row as soon as it comes.
    if (!g_c.waitArt) {
        char cover[160];
        if (coverPath(&g_c.wait, cover, sizeof(cover))) {
            showCover(&g_c.wait);
            g_c.waitArt = 1;
        }
    }
    g_found = g_c.wait;
    if (fetchedFor(&g_found)) {
        if (g_c.menuPaused) return;
        logLine("carry on: song %d of %d fetched, after %s s of waiting", g_found.index + 1,
                g_c.count, secsOf((uint32_t)(sceKernelGetSystemTimeWide() - g_c.waitUs) / 1000u).s);
        if (!carryPlay(&g_found, g_c.waitWhy))
            carryNext(g_found.index, false, "the one before would not open");
        return;
    }
    // Held: only the row's play fetches it again.
    if (g_c.held) return;
    lock();
    const int fetchState = g_fetch.state;
    const bool forIt = fetchIsFor(&g_c.wait, kJobSong);
    const bool forGood = forIt && fetchState == kFetchFailed && g_fetch.forGood;
    const bool unreached = forIt && fetchState == kFetchFailed && g_fetch.unreached;
    unlock();
    // Its fetch under way, or to begin once another has stopped.
    if ((forIt && fetchState == kFetchRunning) || g_c.fetchWant == g_c.waitIndex) return;
    // Failed, or its file gone since it came: fetched again once, then
    // passed over, or with no answer from the server, on to the songs on the
    // card (carryOnCard).
    if (!forGood && g_c.waitTries < 2) {
        g_c.waitTries++;
        logLine("carry on: song %d of %d: fetched again", g_c.waitIndex + 1, g_c.count);
        fetchStart(&g_c.wait, kJobSong);
        return;
    }
    if (unreached && g_c.offlineSkip) {
        carryOnCard();
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

// At each of the music thread's looks while it carries on (musicLookUs), as
// player.c's watchSong did each half second.
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
        // VitaPlex's song playing, short of its place: taken there at once,
        // not once its clock is seen to move, which took another 0.66 s in log
        // d6e76980 (state 1 at 0:00 0.58 s after play). Twice more at most,
        // half a second apart, should the shell not take it yet.
        const uint32_t place = startAt(&g_c.cur);
        if (state == 1 && now < g_c.fastUntilUs && place >= 2000 && time + 5000 < place &&
            g_c.seekTries < 3 && (g_c.seekTries == 1 || (uint32_t)(now - g_c.seekUs) >= 500000u)) {
            seekTo(place);
            return;
        }
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
                // To VitaPlex's place in the song it handed over; else
                // from the start, as the shell can start a file it has had
                // before where it left it (player.c's fromTheStart). Then
                // nothing more this look: what was read is from before the
                // seek. Taken for where the song had got to, 3:11 of a song
                // the shell started again near its end for repeat one made
                // the next look's 0:00 its end, and it began again, and so on.
                const uint32_t at = startAt(&g_c.cur);
                g_c.startAtMs = 0;
                if (at >= 2000) {
                    // Begun there, the seek asked for with the song taken:
                    // else asked for again now that it plays.
                    if (time + 5000 >= at && time <= at + 6000) {
                        logLine("carry on: song %d of %d %s %s, where VitaPlex was, %s s after "
                                "play%s",
                                g_c.cur.index + 1, g_c.count,
                                g_c.seekTries > 1 ? "at" : "began at", mmss(time).s,
                                secsOf((uint32_t)(now - g_c.songUs) / 1000u).s,
                                g_c.seekTries > 1 ? ", taken there as it began" : "");
                        g_c.seekWantMs = 0;
                        g_c.fastUntilUs = 0;
                    } else {
                        g_c.seekTries = 0;
                        seekTo(at);
                        return;
                    }
                } else if (time >= 5000) {
                    seekStart("the shell started it part-way");
                    return;
                }
            } else if (!g_c.otherLogged) {
                g_c.otherLogged = 1;
                logLine("carry on: song %d of %d: the shell plays something %s long, not this "
                        "song of %s",
                        g_c.cur.index + 1, g_c.count, mmss(len).s, mmss(g_c.cur.durationMs).s);
            }
        }
        // Counted in the usual looks, not the 25 ms ones after VitaPlex's
        // song is played.
        if (!g_c.started && now >= g_c.fastUntilUs && ++g_c.waitTicks > kNeverStartedLooks) {
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
        // Only from a status read after the seek: in the tick that asked for
        // it, the shell was read at 0:00 before it, and the time since the
        // seek came out below 0, as 4294.9 s, which asked for it again at
        // once (log 5ae2ab41, build 2045).
        if (g_c.seekWantMs && now > g_c.seekUs) {
            // Near enough: an M4A download's seek landed 2 to 4 s short (log
            // f3042ca3, build 2035).
            const uint32_t want = g_c.seekWantMs;
            const uint32_t since = (uint32_t)(now - g_c.seekUs) / 1000u;
            if (time + 5000 >= want && time <= want + 6000) {
                logLine("carry on: song %d of %d at %s, %s s after the seek to %s, %s s after "
                        "play",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, secsOf(since).s,
                        mmss(want).s, secsOf((uint32_t)(now - g_c.songUs) / 1000u).s);
                g_c.seekWantMs = 0;
                g_c.fastUntilUs = 0;
            } else if (since >= 4000) {
                if (g_c.seekTries < 2) {
                    logLine("carry on: song %d of %d at %s, not %s, 4 s after the seek: again",
                            g_c.cur.index + 1, g_c.count, mmss(time).s, mmss(want).s);
                    seekTo(want);
                    return;
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
        // stops the shell's music, 0.1 to 0.4 s after four of seven presses
        // in log e98e760e (build 2002). Played again at once, and every 0.6 s
        // while it stays so (kReplayEveryUs): with 5 s between tries, the
        // music once stayed stopped 2.6 s (log e18b5831). Logged the first
        // times and then once a minute.
        if (state != 1 && !g_c.menuPaused && !g_c.resuming) {
            if (!g_c.stoppedUs) g_c.stoppedUs = now;
            if ((uint32_t)(now - g_c.lastPlayUs) >= kReplayEveryUs) {
                g_c.lastPlayUs = now;
                const int rcPlay = musicEvent(kEventPlay);
                if (g_c.replays < 3 || g_c.replays % 100 == 0)
                    logLine("carry on: song %d of %d stopped at %s, before its end (state %d); "
                            "play again: 0x%08X",
                            g_c.cur.index + 1, g_c.count, mmss(time).s, state, (unsigned)rcPlay);
                g_c.replays++;
            }
        } else if (back) {
            if (g_c.stoppedUs)
                logLine("carry on: song %d of %d plays again at %s, %s s after it was seen stopped",
                        g_c.cur.index + 1, g_c.count, mmss(time).s,
                        secsOf((uint32_t)(now - g_c.stoppedUs) / 1000u).s);
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
            // It can go on where it stopped and jump back only after: an M4A
            // did, 20 to 37 s, after the look 0.1 s on had seen it where it
            // stopped (log 0790f7dc; looks came every half second up to build
            // 2050). Watched for kBackWatchUs.
            g_c.backFromMs = g_c.playMs;
            g_c.backUs = now;
        } else if (state == 1 && g_c.backUs) {
            const uint32_t since = (uint32_t)(now - g_c.backUs);
            const uint32_t want = g_c.backFromMs + since / 1000u;
            if (since >= kBackWatchUs) {
                g_c.backUs = 0;
            } else if (time + 3000 < want) {
                g_c.backUs = 0;
                const int rcTime = musicSeekTime(want), rcSeek = musicEvent(kEventSeek);
                logLine("carry on: song %d of %d went on at %s, %s s after it played again at %s; "
                        "seek time 0x%08X, seek 0x%08X",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, secsOf(since / 1000u).s,
                        mmss(g_c.backFromMs).s, (unsigned)rcTime, (unsigned)rcSeek);
                g_c.maxTime = time;
                g_c.playMs = want;
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
        // Where the music is, to the server: at each change and every 10 s.
        const char* const told = g_c.menuPaused ? kPaused : kPlaying;
        if (told != g_c.reportState || (uint32_t)(now - g_c.reportUs) >= 10000000u)
            report(&g_c.cur, told, time);
    }
    if ((uint32_t)(now - g_c.statusUs) >= 2000000u) carryStatus(HANDOVER_STATE_PLAYING, time);
}

// One look of the music thread's: a queue VitaPlex handed over begun, once a
// queue carried on before has stopped; the queue carried on, looked at.
void musicStep() {
    lock();
    const int carry = g_carry;
    // Once a queue carried on before has stopped (QM_CMD_PLAY_QUEUE stops it).
    const bool handed = g_playQueueDue && carry != 2;
    if (handed) {
        g_playQueueDue = 0;
        g_carryStop = NULL;
    }
    unlock();
    if (handed) {
        if (carryStartQueue())
            logLine("play queue: SceShell plays VitaPlex's queue");
        else
            carryNone();
    } else if (carry == 2) {
        carryTick();
    }
}

// How long the music thread waits for its next look: 25 ms while VitaPlex's
// song is taken to its place (kFastLookUs), 100 ms while the shell plays a
// song of the queue, half a second else (kLookEveryUs).
SceUInt musicLookUs() {
    if (g_c.fastUntilUs > sceKernelGetSystemTimeWide()) return kFastLookEveryUs;
    lock();
    const int carry = g_carry;
    unlock();
    return carry == 2 && g_c.waitIndex < 0 ? kLookEveryUs : kSlowLookEveryUs;
}

// The music thread: musicStep at each look (musicLookUs), and as soon as
// asked.
class MusicThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            SceUInt timeout = musicLookUs();
            sceKernelWaitSema(g_musicSema, 1, &timeout);
            musicStep();
        }
    }
};

// The music thread, made the first time it is wanted, so that a boot with no
// queue carried on has no stack for it in SceShell. true when there is one.
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
    if (rc < 0) logLine("music: its thread not started: 0x%08X", (unsigned)rc);
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

// ── VitaPlex's updates ──────────────────────────────────────────────────────
// The system's installer, ScePromoterUtil, refuses a title while it runs, so
// VitaPlex cannot install its own update. Up to build 2055 it installed an
// app for it, the updater stub (VPLXUPD01), launched it and quit, and the
// stub installed the update and launched VitaPlex again. SceShell, which no
// app closes, does it now (QM_CMD_INSTALL_UPDATE): VitaPlex unpacks the
// update to QM_UPDATE_DIR with its package head made, asks, and quits. This
// waits for it to be gone, has the installer install the folder, writes what
// came of it to QM_UPDATE_RESULT, and launches VitaPlex again, the old one if
// the install failed, which then says so. The installer is reached as
// Vitacompanion's SceShell module reaches it (devnoname120/vitacompanion,
// promote.c): its module loaded through SceSysmodule unless SceShell has it,
// its functions found by NID, and both left as they are after, the client
// being SceShell's too: no Exit, no unload. It is driven as the stub drove
// it, which installed VitaPlex: PromotePkg, its state looked at until idle,
// then its result.
const uint32_t kPromoterLibrary = 0x31F237B6;      // ScePromoterUtil, in module ScePromoterUtil
const unsigned kSysmoduleNotLoaded = 0x805A1001u;  // sceSysmoduleIsLoadedInternal's "not loaded"
// What the installer gave VitaPlex trying to install itself while it ran.
const unsigned kPromoterInUse = 0x80101114u;
enum { kPromInit, kPromGetState, kPromPromote, kPromGetResult, kPromFns };
LibFn g_promoter[kPromFns] = {{"scePromoterUtilityInit", 0x93451536, 0},
                              {"scePromoterUtilityGetState", 0xABEC74D2, 0},
                              {"scePromoterUtilityPromotePkg", 0x716C81F4, 0},
                              {"scePromoterUtilityGetResult", 0x49B473F0, 0}};
typedef int (*FnPromInit)(void);
typedef int (*FnPromState)(int*);
typedef int (*FnPromote)(const char*, int);
// How long VitaPlex has to go, the installer to be idle, and an install to end.
const uint32_t kUpdateGoneMs = 30000, kUpdateIdleMs = 60000, kUpdateInstallMs = 300000;
const uint32_t kUpdateLookMs = 250;
// The update thread's stack. The installer's calls run on it, and its module's
// start as it loads, with what they take not known here: twice the fetch
// threads'.
const int kUpdateStack = 0x10000;
int g_updating;        // under g_lock: the update thread runs
int g_promoterInited;  // the update thread's: Init called once, its client kept

// Waits up to `ms` while VitaPlex runs. true once it has gone.
bool updateWaitGone(uint32_t ms) {
    for (uint32_t waited = 0;; waited += kUpdateLookMs) {
        SceUID pid = -1;
        if (sceAppMgrGetIdByName(&pid, VITAPLEX_TITLEID) < 0) {
            logLine("update: VitaPlex gone, %s s after it asked", secsOf(waited).s);
            return true;
        }
        if (waited >= ms) return false;
        sceKernelDelayThread(kUpdateLookMs * 1000);
    }
}

// The installer's state once idle, waiting up to `ms`: 0, else what reading
// it gave, or 1 if it stayed busy.
int updateWaitIdle(uint32_t ms) {
    for (uint32_t waited = 0;; waited += kUpdateLookMs) {
        int state = -1;
        const int rc = ((FnPromState)g_promoter[kPromGetState].fn)(&state);
        if (rc < 0) return rc;
        if (state == 0) return 0;
        if (waited >= ms) return 1;
        if (waited % 10000 == 0)
            logLine("update: the installer busy (%d), %s s on", state, secsOf(waited).s);
        sceKernelDelayThread(kUpdateLookMs * 1000);
    }
}

// The installer loaded and its functions found. 0, else the code, with why.
int updateFindPromoter(char* why, int cap) {
    int rc = sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if ((unsigned)rc == kSysmoduleNotLoaded) {
        rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
        logLine("update: the system's installer loaded into SceShell: 0x%08X", (unsigned)rc);
    }
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the system's installer would not load (0x%08X)", (unsigned)rc);
        return rc;
    }
    for (int i = 0; i < kPromFns; i++) {
        uintptr_t fn = 0;
        rc = vitaplexQmGetModuleExportFunc("ScePromoterUtil", kPromoterLibrary, g_promoter[i].nid, &fn);
        if (rc < 0 || !fn) {
            sceClibSnprintf(why, cap, "%s not in the system's installer (0x%08X)", g_promoter[i].name,
                            (unsigned)rc);
            return rc < 0 ? rc : -1;
        }
        g_promoter[i].fn = fn;
    }
    return 0;
}

// QM_UPDATE_DIR installed: once begun, its state looked at until idle, and
// its result read. 0, else the code, with why.
int updatePromote(char* why, int cap) {
    const SceUInt64 began = sceKernelGetSystemTimeWide();
    int rc = ((FnPromote)g_promoter[kPromPromote].fn)(QM_UPDATE_DIR, 0);
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the system's installer would not begin (0x%08X)", (unsigned)rc);
        return rc;
    }
    const int idle = updateWaitIdle(kUpdateInstallMs);
    const uint32_t took = (uint32_t)(sceKernelGetSystemTimeWide() - began) / 1000u;
    if (idle != 0) {
        sceClibSnprintf(why, cap, "the system's installer %s after %s s (0x%08X)",
                        idle == 1 ? "had not finished" : "could not be asked how it went",
                        secsOf(took).s, (unsigned)idle);
        return idle < 0 ? idle : -1;
    }
    int result = 0;
    rc = ((FnPromState)g_promoter[kPromGetResult].fn)(&result);
    logLine("update: installed in %s s: result 0x%08X (0x%08X)", secsOf(took).s, (unsigned)result,
            (unsigned)rc);
    if (rc < 0 || result < 0) {
        const int code = rc < 0 ? rc : result;
        sceClibSnprintf(why, cap, "the system's installer refused it (0x%08X)", (unsigned)code);
        return code;
    }
    return 0;
}

// The whole install. 0, else the code, with why.
int updateInstall(char* why, int cap) {
    if (!exists(QM_UPDATE_DIR "/eboot.bin") || !exists(QM_UPDATE_DIR "/sce_sys/package/head.bin")) {
        sceClibSnprintf(why, cap, "nothing to install at %s", QM_UPDATE_DIR);
        return -1;
    }
    if (!updateWaitGone(kUpdateGoneMs)) {
        sceClibSnprintf(why, cap, "VitaPlex was still running %s s after it asked",
                        secsOf(kUpdateGoneMs).s);
        return -1;
    }
    int rc = updateFindPromoter(why, cap);
    if (rc < 0) return rc;
    // Idle first, as SceShell may be installing something itself; then its
    // client begun, once, as Vitacompanion's module does.
    const int idle = updateWaitIdle(kUpdateIdleMs);
    if (idle != 0) {
        sceClibSnprintf(why, cap, "the system's installer was busy (0x%08X)", (unsigned)idle);
        return idle < 0 ? idle : -1;
    }
    if (!g_promoterInited) {
        g_promoterInited = 1;
        rc = ((FnPromInit)g_promoter[kPromInit].fn)();
        logLine("update: the installer's client begun: 0x%08X", (unsigned)rc);
    }
    // The title can be taken for running a moment after its process has gone.
    for (int attempt = 1;; attempt++) {
        rc = updatePromote(why, cap);
        if ((unsigned)rc != kPromoterInUse || attempt == 3) return rc;
        logLine("update: VitaPlex still taken for running (0x%08X): again in 2 s", (unsigned)rc);
        sceKernelDelayThread(2 * 1000 * 1000);
    }
}

// What came of it, for VitaPlex to read as it starts (QM_UPDATE_RESULT).
void updateResult(int rc, const char* why) {
    char text[224];
    const int n = sceClibSnprintf(text, sizeof(text), "result=0x%08X\n%s%s%send\n", (unsigned)rc,
                                  why[0] ? "why=" : "", why, why[0] ? "\n" : "");
    const char* const part = QM_UPDATE_RESULT ".part";
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int res = fd;
    if (fd >= 0) {
        const int wrote = sceIoWrite(fd, text, (SceSize)n);
        sceIoClose(fd);
        res = wrote == n ? 0 : wrote < 0 ? wrote : -1;
    }
    if (res == 0) {
        sceIoRemove(QM_UPDATE_RESULT);
        res = sceIoRename(part, QM_UPDATE_RESULT);
    }
    if (res < 0) logLine("update: %s not written: 0x%08X", QM_UPDATE_RESULT, (unsigned)res);
}

// The update thread: installs, says how it went, launches VitaPlex again.
int updateThread(SceSize, void*) {
    char why[128];
    why[0] = 0;
    const int rc = updateInstall(why, sizeof(why));
    if (rc == 0)
        logLine("update: VitaPlex updated");
    else
        logLine("update: not installed (0x%08X): %s", (unsigned)rc, why);
    updateResult(rc, why);
    char uri[48];
    sceClibSnprintf(uri, sizeof(uri), "psgm:play?titleid=%s", VITAPLEX_TITLEID);
    const int rcLaunch = sceAppMgrLaunchAppByUri(0x20000, uri);
    logLine("update: VitaPlex launched again: 0x%08X", (unsigned)rcLaunch);
    lock();
    g_updating = 0;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// From VitaPlex as it quits to be updated (QM_CMD_INSTALL_UPDATE), the update
// thread begun. Under g_lock.
void startUpdate() {
    if (g_updating) {
        logLine("update: asked again while one is under way");
        return;
    }
    logLine("update: VitaPlex asks for its update at %s to be installed once it has gone",
            QM_UPDATE_DIR);
    const SceUID thread = sceKernelCreateThread("VitaPlexQM::Update", updateThread, kThreadPriority,
                                                kUpdateStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("update: no thread to install it: 0x%08X", (unsigned)rc);
        // For VitaPlex to say so once it is opened again: nothing launches it.
        updateResult(rc, "the quick menu plugin had no thread to install it");
        return;
    }
    g_updating = 1;
}

// ── VitaPlex's downloads, carried on ────────────────────────────────────────
// (qm_protocol.h, QM_CMD_DOWNLOADS) The system closes VitaPlex to start a
// game, and its downloads stopped with it until it was opened again. As it
// goes it now lists them for SceShell, which carries them on from here once
// VitaPlex has gone: each into the file VitaPlex had begun, from where that
// file ends, by HTTP Range, as VitaPlex's own download goes on with a file
// cut off. A video the server prepares in its download queue is waited for
// here as VitaPlex waits for it, and one not yet in the queue is added to it
// under VitaPlex's client id, with the query and profile VitaPlex would have
// sent. How far it has got goes to QM_DOWNLOADS_STATUS, which VitaPlex reads
// as it takes them back (QM_CMD_TAKE_DOWNLOADS), and to a progress
// notification of SceShell's own, the one VitaPlex shows while it downloads.
// One download at a time, on a thread of its own at the lowest priority, which
// gives way to the music's fetches. Its lines in the log begin "download:".
// The files' URLs and the token are never logged.
enum { kDlWaiting, kDlGoing, kDlDone, kDlGone, kDlFailed };
const char* const kDlStateNames[5] = {"waiting", "going", "done", "gone", "failed"};
const int kDlStack = 0x8000;
// Tries at a file before it is given up, each after a wait longer than the
// last; a try that gets some of it starts the count again.
const int kDlTries = 4;
const uint32_t kDlRetryMs[kDlTries] = {0, 5000, 15000, 60000};
// How long the server may answer 503 for a file it is still preparing, and
// how long a queue item may stay unprepared, as VitaPlex waits for one
// (downloads_manager.cpp, waitForQueueItem): an hour.
const uint32_t kDlPrepareMs = 60u * 60u * 1000u;
// The status and the notification, this often while it goes.
const uint32_t kDlStatusEveryMs = 2000;
const uint32_t kDlLogEveryMs = 10000;
// How long VitaPlex has to go before the downloads are begun.
const uint32_t kDlGoneMs = 30000;
const uint32_t kDlLookMs = 250;

// A download of the list. Under g_lock; the download thread's as it runs.
struct DlItem {
    char key[24];
    int state;        // kDl*
    uint64_t bytes;   // in its file
    uint64_t total;   // its full size, 0 when not known
    char queue[16];   // the download queue's id and the item's, "" for none
    char item[16];
};
DlItem g_dl[QM_DOWNLOADS_MAX];
int g_dlCount;
int g_dlRunning;  // under g_lock: the download thread runs
int g_dlStop;     // under g_lock: stop after the piece being written
int g_dlAgain;    // under g_lock: a new list came as it ran; it is taken up
int g_dlPaused;   // under g_lock: stopped by the notification's Yes (QM_DOWNLOADS_PAUSE)

// The download thread's: what VitaPlex names itself as, from the list's head.
struct DlHead {
    int notify;
    char client[64], platform[32], device[48];
};
DlHead g_dlHead;
// The download thread's: the list a line at a time, as the queue is read
// (LineReader), with room for an add's profile; a piece of a file on its way
// to the card; a URL; an answer of the server's.
struct DlReader {
    SceUID fd;
    char buf[2048];
    int len, pos;
};
DlReader g_dlReader;
char g_dlLine[4096];
uint8_t g_dlBuf[16384];
char g_dlUrl[1536];
char g_dlAnswer[4096];

// Milliseconds since `from`, in 32 bits (splitUs has why there is no 64-bit
// division).
uint32_t msSince(SceUInt64 from) {
    uint32_t secs, micros;
    splitUs(sceKernelGetSystemTimeWide() - from, &secs, &micros);
    return secs * 1000u + micros / 1000u;
}

// src, cut to fit, into dst, always ended with 0.
void copyText(char* dst, const char* src, int cap) {
    int n = 0;
    for (; src[n] && n < cap - 1; n++) dst[n] = src[n];
    dst[n] = 0;
}

bool dlStopped() {
    lock();
    const int stop = g_dlStop;
    unlock();
    return stop != 0;
}

// Waits ms, a look at a time, for as long as nothing stops the downloads.
// false if something did.
bool dlWait(uint32_t ms) {
    for (uint32_t waited = 0; waited < ms; waited += kDlLookMs) {
        if (dlStopped()) return false;
        sceKernelDelayThread(kDlLookMs * 1000);
    }
    return !dlStopped();
}

// A count of bytes as decimal, without a 64-bit division (splitUs has why).
struct Big {
    char s[24];
};
Big bigOf(uint64_t v) {
    Big b;
    uint32_t hi, lo;
    splitUs(v, &hi, &lo);
    if (hi)
        sceClibSnprintf(b.s, sizeof(b.s), "%u%06u", (unsigned)hi, (unsigned)lo);
    else
        sceClibSnprintf(b.s, sizeof(b.s), "%u", (unsigned)lo);
    return b;
}
uint64_t toU64(const char* s) {
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10u + (uint64_t)(*s - '0');
    return v;
}
// Megabytes, "x.yy", of a size that may pass 4 GB.
Num mb64Of(uint64_t bytes) {
    Num m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u.%02u", (unsigned)(bytes >> 20),
                    (unsigned)((((uint32_t)bytes & 0xFFFFF) * 100u) >> 20));
    return m;
}
// bytes of total in percent, in 32 bits.
uint32_t percentOf(uint64_t bytes, uint64_t total) {
    if (bytes > total) bytes = total;
    while (total >= (1u << 24)) {
        total >>= 1;
        bytes >>= 1;
    }
    if (total == 0) return 0;
    return (uint32_t)bytes * 100u / (uint32_t)total;
}

// A file's size on the card, 0 when it is not there.
uint64_t fileSize(const char* path) {
    SceIoStat st;
    sceClibMemset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0 || st.st_size < 0) return 0;
    return (uint64_t)st.st_size;
}

// The list's next line in g_dlLine, as readerNext reads the queue. false at
// its end.
bool dlNext() {
    int n = 0;
    bool any = false;
    for (;;) {
        if (g_dlReader.pos == g_dlReader.len) {
            const int got = sceIoRead(g_dlReader.fd, g_dlReader.buf, sizeof(g_dlReader.buf));
            g_dlReader.len = got > 0 ? got : 0;
            g_dlReader.pos = 0;
            if (g_dlReader.len == 0) break;
        }
        any = true;
        const char c = g_dlReader.buf[g_dlReader.pos++];
        if (c == '\n') break;
        if (c != '\r' && n < (int)sizeof(g_dlLine) - 1) g_dlLine[n++] = c;
    }
    g_dlLine[n] = 0;
    return any;
}

bool dlOpenList() {
    g_dlReader.fd = sceIoOpen(QM_DOWNLOADS_LIST, SCE_O_RDONLY, 0);
    g_dlReader.len = g_dlReader.pos = 0;
    return g_dlReader.fd >= 0;
}

void dlCloseList() {
    if (g_dlReader.fd >= 0) sceIoClose(g_dlReader.fd);
    g_dlReader.fd = -1;
}

// An item's fields, cut at its tabs, in place: how many.
int dlFields(char* line, char** f, int cap) {
    int n = 0;
    f[n++] = line;
    for (char* p = line; *p && n < cap; p++)
        if (*p == '\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    return n;
}
// The fields every item has before how it is fetched.
enum { kFKey, kFPath, kFSize, kFTitle, kFToken, kFKind, kFArg1, kFArg2, kFArg3, kFMax };

// The list read through for its head and its items, which g_dl then holds,
// waiting. false when it is not there or not whole.
bool dlReadList() {
    sceClibMemset(&g_dlHead, 0, sizeof(g_dlHead));
    if (!dlOpenList()) return false;
    int count = 0;
    bool whole = false;
    char* f[kFMax];
    while (dlNext()) {
        if (same(g_dlLine, "end")) {
            whole = true;
            break;
        }
        if (startsWith(g_dlLine, "notify=")) {
            g_dlHead.notify = g_dlLine[7] == '1';
        } else if (startsWith(g_dlLine, "client=")) {
            copyText(g_dlHead.client, g_dlLine + 7, sizeof(g_dlHead.client));
        } else if (startsWith(g_dlLine, "platform=")) {
            copyText(g_dlHead.platform, g_dlLine + 9, sizeof(g_dlHead.platform));
        } else if (startsWith(g_dlLine, "device=")) {
            copyText(g_dlHead.device, g_dlLine + 7, sizeof(g_dlHead.device));
        } else if (startsWith(g_dlLine, "item=") && count < QM_DOWNLOADS_MAX) {
            if (dlFields(g_dlLine + 5, f, kFMax) <= kFArg1) continue;
            const uint64_t bytes = fileSize(f[kFPath]);  // not under the lock: the card
            lock();
            DlItem* d = &g_dl[count];
            sceClibMemset(d, 0, sizeof(*d));
            copyText(d->key, f[kFKey], sizeof(d->key));
            d->state = kDlWaiting;
            d->total = toU64(f[kFSize]);
            d->bytes = bytes;
            unlock();
            count++;
        }
    }
    dlCloseList();
    lock();
    g_dlCount = whole ? count : 0;
    unlock();
    return whole;
}

// The status, as qm_protocol.h lays it out: written beside its name and moved
// in. Under nothing: g_dl is read under g_lock a line at a time.
int g_dlStatusErrors;
void dlStatus(bool running) {
    char text[160];
    const char* const part = QM_DOWNLOADS_STATUS ".part";
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int rc = fd;
    if (fd >= 0) {
        uint32_t secs, micros;
        splitUs(sceKernelGetSystemTimeWide(), &secs, &micros);
        lock();
        const int paused = g_dlPaused;
        unlock();
        int n = sceClibSnprintf(text, sizeof(text), "written=%u.%06u\nrunning=%d\npaused=%d\n",
                                (unsigned)secs, (unsigned)micros, running ? 1 : 0, paused);
        rc = sceIoWrite(fd, text, (SceSize)n) == n ? 0 : -1;
        for (int i = 0; rc == 0; i++) {
            lock();
            const bool more = i < g_dlCount;
            if (more) {
                const DlItem* d = &g_dl[i];
                n = sceClibSnprintf(text, sizeof(text), "item=%s\t%s\t%s\t%s\t%s\t%s\n", d->key,
                                    kDlStateNames[d->state], bigOf(d->bytes).s,
                                    bigOf(d->total).s, d->queue, d->item);
            }
            unlock();
            if (!more) break;
            rc = sceIoWrite(fd, text, (SceSize)n) == n ? 0 : -1;
        }
        if (rc == 0) rc = sceIoWrite(fd, "end\n", 4) == 4 ? 0 : -1;
        const int rcClose = sceIoClose(fd);
        if (rc == 0) rc = rcClose;
    }
    if (rc >= 0) {
        // rename does not replace a file on every target (player.c's moveIn).
        sceIoRemove(QM_DOWNLOADS_STATUS);
        rc = sceIoRename(part, QM_DOWNLOADS_STATUS);
    }
    if (rc < 0 && g_dlStatusErrors < 3) {
        g_dlStatusErrors++;
        logLine("download: status not written: 0x%08X", (unsigned)rc);
    }
}

// ── The downloads' progress notification ──
// SceNotificationUtil's progress notification, the one a system download
// shows and VitaPlex shows while it downloads (src/utils/
// shell_integration_psv.cpp), from SceShell, found with taiHEN as the HTTP
// library is: SceShell's module of it, else the module loaded into SceShell
// through SceSysmodule. Its calls go to SceShell's notification service from
// this thread, never SceShell's UI thread, as the music service's do. Where it
// is not there, or Begin refuses, the downloads go on without it.
const uint32_t kNotifyProgressLibrary = 0x75602769;  // SceNotificationUtilProgress
enum { kNotifyBegin, kNotifyUpdate, kNotifyFinish, kNotifyFns };
LibFn g_notify[kNotifyFns] = {{"sceNotificationUtilProgressBegin", 0xFD0769B0, 0},
                              {"sceNotificationUtilProgressUpdate", 0xD9890A65, 0},
                              {"sceNotificationUtilProgressFinish", 0x8CD688A1, 0}};
typedef int (*FnNotify)(void*);
// The parameters, as vitasdk's notificationutil.h lays them out: two lines of
// 63 UTF-16 units, each followed by a 0, then the rest. Begin's handler, the
// one its module calls with each event, is the word at 0x4E8, not the header's
// eventHandler at 0x4EC, and must not be 0 (docs/DESIGN_NOTES.md, "Vita:
// notifications yes, background downloads no").
struct NotifyText {
    uint16_t text[63];
    int16_t sep0;
    uint16_t sub[63];
    int16_t sep1;
};
struct NotifyBeginParam {
    NotifyText t;
    uint8_t rest[0x4F0 - sizeof(NotifyText)];
    uint16_t cancelText[63];  // the question, at 0x4F0
    int16_t sep2;
};
static_assert(sizeof(NotifyBeginParam) == 0x570, "Begin sends 0x570 bytes");
struct NotifyUpdateParam {
    NotifyText t;
    float progress;
    uint8_t reserved[0x38];
};
struct NotifyFinishParam {
    NotifyText t;
    char path[0x3E8];
};
const uint32_t kNotifyHandlerOffset = 0x4E8;
// Begin sends SceShell 0x570 bytes of its parameters, not the 0x4F0 vitasdk's
// struct has (the method 0x7000B call in Begin's code, read from the build
// 2058 dump): the 0x80 after it are a third line, the question SceShell asks
// when the notification's cancel is pressed, with No and Yes under it. With
// the struct at 0x4F0 the question was whatever came after it in memory:
// blank here, two characters of the stack in VitaPlex (builds 2063 to 2065).
// The notification is the plugin's for all of a download run, VitaPlex's
// part of it included: VitaPlex shows its progress through here while it
// downloads (QM_CMD_DL_PROGRESS), so that the one notification goes on as
// SceShell carries the downloads on once VitaPlex has gone, and as VitaPlex
// takes them back. A notification is its process's, and no call names one
// (Update and Finish take only text): VitaPlex's own could be gone on with
// by nothing once it had quit, and SceShell had to begin a second one
// (builds 2063 to 2069). Under g_notifyLock: the download thread and the
// receiving thread both show it, and the watch thread ends one left over.
SceUID g_notifyLock = -1;
int g_notifyFound;    // 0 not looked for, 1 there, -1 not there
int g_notifyLive;     // Begin has shown it
int g_notifyFailed;   // an Update of it has failed, logged once
SceUInt64 g_notifyHeldUs;  // taken back with it showing: when, for notifyHeldTick
union {
    NotifyBeginParam begin;
    NotifyUpdateParam update;
    NotifyFinishParam finish;
} g_notifyParam;
// Its owner, VitaPlex, for its icon (statusAsVitaPlex, by the BGM hook):
// the hook, and the window after a Begin in which it answers.
TaiHookRef g_statusHookRef;
SceUInt64 g_ownerUntilUs;  // atomic: the window open, until then
const SceUInt64 kOwnerWindowUs = 5ull * 1000 * 1000;
// How long a notification left showing as VitaPlex takes the downloads back
// waits for VitaPlex to go on with it before it is ended.
const SceUInt64 kNotifyHeldUs = 30ull * 1000 * 1000;

// Its events: 0 when its question ("Pause downloads?", the third line of
// Begin's parameters) is answered Yes; No sends none (build 2072). Called
// from inside Update, on the thread that called it, with its module's lock
// held: nothing here calls back into it, and the pause is the watch thread's
// (notifyPauseTick).
int g_notifyPauseAsked;  // atomic
void notifyEvent(int id) {
    logLine("notification: event %d%s", id, id == 0 ? ", Yes: the downloads pause" : "");
    if (id == 0) __atomic_store_n(&g_notifyPauseAsked, 1, __ATOMIC_SEQ_CST);
}

bool notifyFind() {
    if (g_notifyFound) return g_notifyFound > 0;
    g_notifyFound = -1;
    uintptr_t fn = 0;
    int rc = vitaplexQmGetModuleExportFunc("SceNotificationUtil", kNotifyProgressLibrary,
                                           g_notify[0].nid, &fn);
    if (rc < 0) {
        rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
        logLine("notification: SceNotificationUtil loaded into SceShell: 0x%08X", (unsigned)rc);
        if (rc < 0) return false;
    }
    for (int i = 0; i < kNotifyFns; i++) {
        fn = 0;
        rc = vitaplexQmGetModuleExportFunc("SceNotificationUtil", kNotifyProgressLibrary,
                                           g_notify[i].nid, &fn);
        g_notify[i].fn = rc >= 0 ? fn : 0;
        if (rc < 0) {
            logLine("notification: %s not in SceNotificationUtil: 0x%08X", g_notify[i].name,
                    (unsigned)rc);
            return false;
        }
    }
    g_notifyFound = 1;
    return true;
}

// UTF-16 into one of the parameters' lines, cut at 63 units, ended with 0.
void lineOf(uint16_t* to, const uint16_t* from) {
    int n = 0;
    for (; from[n] && n < 63; n++) to[n] = from[n];
    for (; n < 63; n++) to[n] = 0;
}

// The notification shown, or brought up to date: progress from 0 to 1, or
// below 0 when not known. UTF-16 lines.
void notifyShow16(const uint16_t* text, const uint16_t* sub, float progress) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    g_notifyHeldUs = 0;
    if (!notifyFind()) {
        sceKernelUnlockMutex(g_notifyLock, 1);
        return;
    }
    sceClibMemset(&g_notifyParam, 0, sizeof(g_notifyParam));
    if (!g_notifyLive) {
        lineOf(g_notifyParam.begin.t.text, text);
        lineOf(g_notifyParam.begin.t.sub, sub);
        fromUtf8("Pause downloads?", g_notifyParam.begin.cancelText, 63);
        void (*handler)(int) = &notifyEvent;
        sceClibMemcpy((uint8_t*)&g_notifyParam.begin + kNotifyHandlerOffset, &handler,
                      sizeof(handler));
        // Its owner VitaPlex, for its icon (statusAsVitaPlex).
        if (g_statusHookRef)
            __atomic_store_n(&g_ownerUntilUs, sceKernelGetSystemTimeWide() + kOwnerWindowUs,
                             __ATOMIC_SEQ_CST);
        const int rc = ((FnNotify)g_notify[kNotifyBegin].fn)(&g_notifyParam.begin);
        logLine("notification: begun: 0x%08X", (unsigned)rc);
        if (rc < 0) {
            g_notifyFound = -1;  // not tried again this boot
        } else {
            g_notifyLive = 1;
            g_notifyFailed = 0;
        }
        sceKernelUnlockMutex(g_notifyLock, 1);
        return;
    }
    lineOf(g_notifyParam.update.t.text, text);
    lineOf(g_notifyParam.update.t.sub, sub);
    // In percent: SceShell keeps the float as it comes (its 0x7000D handler
    // stores +0x100 straight into the notification) and its bar runs to 100,
    // so a fraction left the bar at its start (builds up to 2075).
    g_notifyParam.update.progress =
        progress < 0.0f ? 0.0f : progress > 1.0f ? 100.0f : progress * 100.0f;
    const int rc = ((FnNotify)g_notify[kNotifyUpdate].fn)(&g_notifyParam.update);
    // As VitaPlex's: closed in SceShell, it may refuse its updates.
    if (rc < 0 && !g_notifyFailed) {
        g_notifyFailed = 1;
        logLine("notification: not brought up to date: 0x%08X", (unsigned)rc);
    }
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// Under g_notifyLock.
void notifyEndLocked(const uint16_t* text, const uint16_t* sub) {
    g_notifyHeldUs = 0;
    if (!g_notifyLive) return;
    g_notifyLive = 0;
    sceClibMemset(&g_notifyParam, 0, sizeof(g_notifyParam));
    lineOf(g_notifyParam.finish.t.text, text);
    lineOf(g_notifyParam.finish.t.sub, sub);
    const int rc = ((FnNotify)g_notify[kNotifyFinish].fn)(&g_notifyParam.finish);
    logLine("notification: finished: 0x%08X", (unsigned)rc);
}

void notifyEnd16(const uint16_t* text, const uint16_t* sub) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    notifyEndLocked(text, sub);
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The download thread's, in UTF-8, with the list's say.
void notifyShow(const char* text, const char* sub, float progress) {
    if (!g_dlHead.notify) return;
    uint16_t t[64], u[64];
    fromUtf8(text, t, 64);
    fromUtf8(sub, u, 64);
    notifyShow16(t, u, progress);
}

void notifyEnd(const char* text, const char* sub) {
    uint16_t t[64], u[64];
    fromUtf8(text, t, 64);
    fromUtf8(sub, u, 64);
    notifyEnd16(t, u);
}

// Taken back with the notification showing: VitaPlex goes on with it as it
// downloads again (QM_CMD_DL_PROGRESS). Ended by notifyHeldTick if it does not.
void notifyHold() {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    if (g_notifyLive) g_notifyHeldUs = sceKernelGetSystemTimeWide();
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The watch thread's: a notification held for VitaPlex longer than
// kNotifyHeldUs, ended. Never under g_lock.
void notifyHeldTick(SceUInt64 now) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    if (g_notifyLive && g_notifyHeldUs && now - g_notifyHeldUs > kNotifyHeldUs) {
        logLine("notification: VitaPlex did not go on with it in 30 s; ended");
        uint16_t t[64], u[64];
        fromUtf8("VitaPlex", t, 64);
        fromUtf8("Downloads paused", u, 64);
        notifyEndLocked(t, u);
    }
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The watch thread's: the notification's Yes carried out. The downloads
// SceShell carries on stop, paused, and the notification ends here; those
// VitaPlex downloads pause as it finds QM_DOWNLOADS_PAUSE, and it ends the
// notification itself. Never under g_lock.
void notifyPauseTick() {
    if (!__atomic_exchange_n(&g_notifyPauseAsked, 0, __ATOMIC_SEQ_CST)) return;
    lock();
    const int running = g_dlRunning;
    if (running) {
        g_dlStop = 1;
        g_dlAgain = 0;
        g_dlPaused = 1;
    }
    unlock();
    if (running) {
        logLine("download: paused from the notification");
        return;  // dlRun ends the notification as it stops
    }
    const SceUID fd = sceIoOpen(QM_DOWNLOADS_PAUSE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    logLine("download: VitaPlex asked to pause its downloads (0x%08X)", (unsigned)fd);
}

// From VitaPlex (QM_CMD_DL_PROGRESS, QM_CMD_DL_PROGRESS_END): what it asked
// for, taken by the receiving thread under g_lock and shown after it, so that
// no notification call is made under g_lock.
struct NotifyAsk {
    int due;    // 1 show, 2 end
    uint32_t permille;
    uint16_t text[64], sub[64];
};
NotifyAsk g_notifyAsk;  // the receiving thread's alone

void notifyAsked() {
    if (!g_notifyAsk.due) return;
    const int due = g_notifyAsk.due;
    g_notifyAsk.due = 0;
    if (due == 1)
        notifyShow16(g_notifyAsk.text, g_notifyAsk.sub,
                     g_notifyAsk.permille == QM_PROGRESS_UNKNOWN
                         ? -1.0f
                         : (float)(g_notifyAsk.permille > 1000 ? 1000 : g_notifyAsk.permille) /
                               1000.0f);
    else
        notifyEnd16(g_notifyAsk.text, g_notifyAsk.sub);
}

// ── Asking the server ──
// A request of the server's for the downloads: as httpOpen makes one, but
// under VitaPlex's own names for itself (the download queue is its client
// id's), with POST when post, JSON asked for when json, and the profile an add
// carries. 0 with *conn and *req to read and close, else the failed call's
// code. Logged as `what`.
int dlOpen(int tmpl, const char* what, const char* url, const char* token, const char* range,
           bool post, bool json, const char* profileExtra, int* conn, int* req) {
    *req = -1;
    *conn = httpFn<FnConnection>(kHttpCreateConnection)(tmpl, url, 1);
    if (*conn < 0) {
        logLine("download: %s: connection 0x%08X", what, (unsigned)*conn);
        return *conn;
    }
    *req = httpFn<FnRequest>(kHttpCreateRequest)(*conn, post ? kHttpPost : kHttpGet, url, 0);
    if (*req < 0) {
        const int rc = *req;
        logLine("download: %s: request 0x%08X", what, (unsigned)rc);
        httpClose(*conn, -1);
        return rc;
    }
    const char* const client = g_dlHead.client[0] ? g_dlHead.client : "VitaPlex";
    int rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Client-Identifier", client,
                                              kHeaderOverwrite);
    if (rc >= 0)
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Product", "VitaPlex", kHeaderOverwrite);
    if (rc >= 0 && g_dlHead.platform[0])
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Platform", g_dlHead.platform,
                                              kHeaderOverwrite);
    if (rc >= 0 && g_dlHead.device[0])
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Device", g_dlHead.device,
                                              kHeaderOverwrite);
    if (rc >= 0 && token && token[0])
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Token", token, kHeaderOverwrite);
    if (rc >= 0 && json)
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "Accept", "application/json", kHeaderOverwrite);
    if (rc >= 0 && profileExtra) {
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Client-Profile-Name", "Generic",
                                              kHeaderOverwrite);
        if (rc >= 0)
            rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "X-Plex-Client-Profile-Extra",
                                                  profileExtra, kHeaderOverwrite);
    }
    if (rc >= 0 && range) rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "Range", range, kHeaderOverwrite);
    if (rc >= 0) rc = httpFn<FnSend>(kHttpSend)(*req, NULL, 0);
    if (rc < 0) {
        logLine("download: %s: failed 0x%08X", what, (unsigned)rc);
        httpClose(*conn, *req);
        return rc;
    }
    return 0;
}

// A short answer of the server's, into g_dlAnswer, ended with 0: its HTTP
// status, or the failed call's code.
int dlAsk(int tmpl, const char* what, const char* url, const char* token, bool post,
          const char* profileExtra) {
    int conn, req, status = 0;
    g_dlAnswer[0] = 0;
    int rc = dlOpen(tmpl, what, url, token, NULL, post, true, profileExtra, &conn, &req);
    if (rc < 0) return rc;
    httpFn<FnStatus>(kHttpStatus)(req, &status);
    int n = 0;
    while (n < (int)sizeof(g_dlAnswer) - 1) {
        const int got = httpFn<FnRead>(kHttpRead)(req, g_dlAnswer + n,
                                                  (unsigned)(sizeof(g_dlAnswer) - 1 - n));
        if (got <= 0) break;
        n += got;
    }
    g_dlAnswer[n] = 0;
    httpClose(conn, req);
    return status;
}

// The first number under "key" in a JSON answer, as VitaPlex's extractJsonInt
// takes it; 0 when there is none.
uint32_t jsonNumber(const char* json, const char* key) {
    char pat[40];
    const int plen = sceClibSnprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* v = findText(json, pat);
    if (!v) return 0;
    v += plen;
    while (*v == ' ' || *v == ':' || *v == '"') v++;
    return toUint(v);
}

// The item added to VitaPlex's download queue on server, with the query and
// profile VitaPlex would have added it with: the queue's id and the item's in
// d. true when the server gave both.
bool dlAdd(int tmpl, DlItem* d, const char* server, const char* token, const char* query,
           const char* profile, const char* label) {
    sceClibSnprintf(g_dlUrl, sizeof(g_dlUrl), "%s/downloadQueue", server);
    int status = dlAsk(tmpl, "the download queue", g_dlUrl, token, true, NULL);
    const uint32_t queue = status == 200 ? jsonNumber(g_dlAnswer, "id") : 0;
    if (queue == 0) {
        logLine("download: %s: no download queue (HTTP %d)", label, status);
        return false;
    }
    const int len = sceClibSnprintf(g_dlUrl, sizeof(g_dlUrl), "%s/downloadQueue/%u/add%s",
                                    server, (unsigned)queue, query);
    if (len >= (int)sizeof(g_dlUrl)) {
        logLine("download: %s: the add's URL is longer than the %u bytes here", label,
                (unsigned)sizeof(g_dlUrl));
        return false;
    }
    status = dlAsk(tmpl, "the add", g_dlUrl, token, true, profile);
    const uint32_t item = status == 200 ? jsonNumber(g_dlAnswer, "id") : 0;
    if (item == 0) {
        logLine("download: %s: not added to download queue %u (HTTP %d)", label,
                (unsigned)queue, status);
        return false;
    }
    lock();
    sceClibSnprintf(d->queue, sizeof(d->queue), "%u", (unsigned)queue);
    sceClibSnprintf(d->item, sizeof(d->item), "%u", (unsigned)item);
    unlock();
    logLine("download: %s: added to download queue %u as item %u", label, (unsigned)queue,
            (unsigned)item);
    return true;
}

// What the notification's second line says, for n of count.
void dlSub(char* out, int cap, int n, int count, const char* what) {
    if (count > 1)
        sceClibSnprintf(out, cap, "%d of %d \xC2\xB7 %s", n + 1, count, what);
    else
        sceClibSnprintf(out, cap, "%s", what);
}

enum { kPrepReady, kPrepGone, kPrepNotReady, kPrepStopped };
// The queue item d waited for until the server has prepared its file, as
// VitaPlex waits for it (waitForQueueItem): its status looked at every 5 s at
// first, every 30 s after 5 minutes. kPrepReady, kPrepGone when the server no
// longer has it or gave it up, kPrepNotReady after an hour or five failures in a
// row, kPrepStopped when the downloads are taken back.
int dlPrepared(int tmpl, DlItem* d, const char* server, const char* token, const char* title,
               int n, int count, const char* label) {
    sceClibSnprintf(g_dlUrl, sizeof(g_dlUrl), "%s/downloadQueue/%s/items/%s", server, d->queue,
                    d->item);
    char last[24], status[24], sub[96];
    last[0] = 0;
    int errors = 0;
    const SceUInt64 t0 = sceKernelGetSystemTimeWide();
    for (;;) {
        const uint32_t ms = msSince(t0);
        if (ms > kDlPrepareMs) {
            logLine("download: %s: not prepared on the server in an hour", label);
            return kPrepNotReady;
        }
        const int http = dlAsk(tmpl, "its queue item", g_dlUrl, token, false, NULL);
        if (http == 404) {
            logLine("download: %s: the server no longer has queue item %s", label, d->item);
            return kPrepGone;
        }
        if (http != 200) {
            if (++errors >= 5) {
                logLine("download: %s: its queue item would not answer (%d)", label, http);
                return kPrepNotReady;
            }
        } else {
            errors = 0;
            jsonString(g_dlAnswer, "status", status, sizeof(status));
            const uint32_t progress = jsonNumber(g_dlAnswer, "progress");
            if (!same(status, last)) {
                logLine("download: %s: queue item %s %s, %u%%", label, d->item, status,
                        (unsigned)progress);
                copyText(last, status, sizeof(last));
            }
            if (same(status, "available")) return kPrepReady;
            if (same(status, "error") || same(status, "expired")) return kPrepGone;
            char what[48];
            sceClibSnprintf(what, sizeof(what), "Preparing on server %u%%", (unsigned)progress);
            dlSub(sub, sizeof(sub), n, count, what);
            notifyShow(title, sub, progress > 0 && progress <= 100 ? (float)progress / 100.0f : -1.0f);
        }
        dlStatus(true);
        const uint32_t wait = ms < 30000 ? 5000 : ms < 120000 ? 10000 : ms < 300000 ? 20000 : 30000;
        if (!dlWait(wait)) return kPrepStopped;
    }
}

// kDl* of fetching url into path from where the file ends: the bytes and full
// size in d as they come.
int dlFetch(int tmpl, DlItem* d, const char* url, const char* path, const char* token,
            const char* title, int n, int count, const char* label) {
    char range[48], sub[96], what[64];
    int tries = 0;
    SceUInt64 prepareFrom = 0;
    for (;;) {
        if (dlStopped()) return kDlGoing;
        uint64_t have = fileSize(path);
        if (d->total > 0 && have == d->total) return kDlDone;
        if (have > 0) sceClibSnprintf(range, sizeof(range), "bytes=%s-", bigOf(have).s);
        int conn, req, status = 0;
        int rc = dlOpen(tmpl, label, url, token, have > 0 ? range : NULL, false, false, NULL,
                        &conn, &req);
        if (rc >= 0) {
            httpFn<FnStatus>(kHttpStatus)(req, &status);
            unsigned long long length = 0;
            const int rcLength = httpFn<FnLength>(kHttpLength)(req, &length);
            if (status == 416 && have > 0) {
                httpClose(conn, req);
                lock();
                d->bytes = d->total = have;
                unlock();
                return kDlDone;
            }
            if (status == 404 || status == 410) {
                httpClose(conn, req);
                logLine("download: %s: HTTP %d", label, status);
                return kDlGone;
            }
            if (status == 503) {
                // Still being prepared: waited for, a while at a time.
                httpClose(conn, req);
                if (!prepareFrom) prepareFrom = sceKernelGetSystemTimeWide();
                if (msSince(prepareFrom) > kDlPrepareMs) {
                    logLine("download: %s: the server answered 503 for an hour", label);
                    return kDlFailed;
                }
                dlSub(sub, sizeof(sub), n, count, "Waiting for the server");
                notifyShow(title, sub, -1.0f);
                dlStatus(true);
                if (!dlWait(10000)) return kDlGoing;
                continue;
            }
            if (status != 200 && status != 206) {
                httpClose(conn, req);
                logLine("download: %s: HTTP %d", label, status);
                rc = -1;
            } else {
                // A whole file when the server answers 200: written anew.
                const bool append = status == 206 && have > 0;
                if (!append) have = 0;
                const uint64_t total = rcLength >= 0 ? have + length : 0;
                if (append && d->total > 0 && total > 0 && total != d->total)
                    logLine("download: %s: the server gives its size as %s, not %s", label,
                            bigOf(total).s, bigOf(d->total).s);
                lock();
                d->state = kDlGoing;
                d->bytes = have;
                if (total > 0) d->total = total;
                unlock();
                const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT |
                                                      (append ? SCE_O_APPEND : SCE_O_TRUNC),
                                            0666);
                if (fd < 0) {
                    httpClose(conn, req);
                    logLine("download: %s: its file not opened: 0x%08X", label, (unsigned)fd);
                    return kDlFailed;
                }
                logLine("download: %s: %s from %s MB of %s MB", label,
                        append ? "going on" : "begun", mb64Of(have).s, mb64Of(d->total).s);
                const SceUInt64 began = sceKernelGetSystemTimeWide();
                SceUInt64 shown = 0, logged = began;
                uint64_t shownBytes = have;
                const uint64_t from = have;
                for (;;) {
                    if (dlStopped()) {
                        rc = 1;
                        break;
                    }
                    // The music's fetch first: a song wanted now, a download not.
                    lock();
                    const bool songFetching = g_fetch.state == kFetchRunning;
                    unlock();
                    if (songFetching) sceKernelDelayThread(50 * 1000);
                    const int got = httpFn<FnRead>(kHttpRead)(req, g_dlBuf, sizeof(g_dlBuf));
                    if (got <= 0) {
                        rc = got;
                        break;
                    }
                    // Taken back as it read: not written, VitaPlex may have the
                    // file again.
                    if (dlStopped()) {
                        rc = 1;
                        break;
                    }
                    const int wrote = sceIoWrite(fd, g_dlBuf, (SceSize)got);
                    if (wrote != got) {
                        logLine("download: %s: not written: 0x%08X", label,
                                (unsigned)(wrote < 0 ? wrote : -1));
                        rc = -2;
                        break;
                    }
                    have += (uint64_t)got;
                    const SceUInt64 now = sceKernelGetSystemTimeWide();
                    if (now - shown >= kDlStatusEveryMs * 1000ull) {
                        lock();
                        d->bytes = have;
                        unlock();
                        const uint32_t ms = shown ? msSince(shown) : 0;
                        const uint32_t kbs =
                            ms ? (uint32_t)((have - shownBytes) >> 10) * 1000u / ms : 0;
                        if (d->total > 0 && kbs > 0)
                            sceClibSnprintf(what, sizeof(what), "%u%% \xC2\xB7 %u.%u MB/s",
                                            (unsigned)percentOf(have, d->total),
                                            (unsigned)(kbs >> 10),
                                            (unsigned)((kbs & 1023) * 10u >> 10));
                        else if (d->total > 0)
                            sceClibSnprintf(what, sizeof(what), "%u%%",
                                            (unsigned)percentOf(have, d->total));
                        else
                            sceClibSnprintf(what, sizeof(what), "%s MB", mb64Of(have).s);
                        dlSub(sub, sizeof(sub), n, count, what);
                        notifyShow(title, sub,
                                   d->total > 0 ? (float)percentOf(have, d->total) / 100.0f
                                                : -1.0f);
                        dlStatus(true);
                        shown = now;
                        shownBytes = have;
                    }
                    if (now - logged >= kDlLogEveryMs * 1000ull) {
                        logged = now;
                        logLine("download: %s: %s of %s MB", label, mb64Of(have).s,
                                mb64Of(d->total).s);
                    }
                }
                sceIoClose(fd);
                httpClose(conn, req);
                lock();
                d->bytes = have;
                unlock();
                const uint32_t ms = msSince(began);
                if (rc == 1) {
                    logLine("download: %s: stopped at %s MB", label, mb64Of(have).s);
                    return kDlGoing;
                }
                if (rc == 0 && (d->total == 0 || have >= d->total)) {
                    logLine("download: %s: whole, %s MB, %s MB of it in %s s", label,
                            mb64Of(have).s, mb64Of(have - from).s, secsOf(ms).s);
                    lock();
                    if (d->total == 0) d->total = have;
                    unlock();
                    return kDlDone;
                }
                if (rc == -2) return kDlFailed;
                logLine("download: %s: cut off at %s MB (0x%08X)", label, mb64Of(have).s,
                        (unsigned)rc);
                if (have > from) tries = 0;  // it got somewhere: the count starts again
            }
        }
        if (++tries >= kDlTries) {
            logLine("download: %s: given up after %d tries", label, kDlTries);
            return kDlFailed;
        }
        dlStatus(true);
        if (!dlWait(kDlRetryMs[tries])) return kDlGoing;
    }
}

// One download of the list: its fields f. Its state when it ends.
int dlOne(int tmpl, int n, int count, char** f, int nf, const char* label) {
    DlItem* d = &g_dl[n];
    const char* const path = f[kFPath];
    const char* const title = f[kFTitle];
    const char* const token = f[kFToken];
    const char* const kind = f[kFKind];
    if (d->total > 0 && fileSize(path) == d->total) {
        logLine("download: %s: whole already", label);
        return kDlDone;
    }
    const char* server = nf > kFArg1 ? f[kFArg1] : "";
    if (same(kind, "add") && nf > kFArg3) {
        if (!dlAdd(tmpl, d, server, token, f[kFArg2], f[kFArg3], label)) return kDlFailed;
    } else if (same(kind, "queue") && nf > kFArg3) {
        lock();
        copyText(d->queue, f[kFArg2], sizeof(d->queue));
        copyText(d->item, f[kFArg3], sizeof(d->item));
        unlock();
    } else {
        logLine("download: %s: fetched in a way not known here (%s)", label, kind);
        return kDlFailed;
    }
    char sub[96];
    dlSub(sub, sizeof(sub), n, count, "Preparing on server");
    notifyShow(title, sub, -1.0f);
    const int prepared = dlPrepared(tmpl, d, server, token, title, n, count, label);
    if (prepared == kPrepStopped) return kDlGoing;
    if (prepared == kPrepGone) {
        lock();
        d->queue[0] = d->item[0] = 0;
        unlock();
        return kDlGone;
    }
    if (prepared != kPrepReady) return kDlFailed;
    // Its file, once prepared: not g_dlUrl, which dlAsk's callers fill.
    static char media[512];
    sceClibSnprintf(media, sizeof(media), "%s/downloadQueue/%s/item/%s/media", server, d->queue,
                    d->item);
    return dlFetch(tmpl, d, media, path, token, title, n, count, label);
}

// Waits up to ms while VitaPlex runs, as the update does (updateWaitGone):
// the file it writes is not touched while it might still write it. true once
// it has gone.
bool dlWaitGone(uint32_t ms) {
    for (uint32_t waited = 0;; waited += kDlLookMs) {
        SceUID pid = -1;
        if (sceAppMgrGetIdByName(&pid, VITAPLEX_TITLEID) < 0) {
            logLine("download: VitaPlex gone, %s s after it handed them over", secsOf(waited).s);
            return true;
        }
        if (waited >= ms || dlStopped()) return false;
        sceKernelDelayThread(kDlLookMs * 1000);
    }
}

// The list carried on, once.
void dlRun() {
    if (!dlReadList()) {
        logLine("download: no whole list at %s", QM_DOWNLOADS_LIST);
        dlStatus(false);
        return;
    }
    const int count = g_dlCount;
    logLine("download: %d download(s) handed over%s", count,
            g_dlHead.notify ? ", with their notification" : "");
    dlStatus(true);
    if (count == 0 || !dlWaitGone(kDlGoneMs)) {
        if (count > 0)
            logLine("download: %s; nothing carried on",
                    dlStopped() ? "taken back first" : "VitaPlex did not go");
        dlStatus(false);
        return;
    }
    int rcSsl = -1, rcHttp = -1, tmpl = -1;
    if (!g_netFound) g_netFound = findLibraries("download");
    if (g_netFound && httpStart("download", &rcSsl, &rcHttp)) tmpl = httpTemplate("download");
    if (tmpl < 0) {
        logLine("download: no way to ask the server here; nothing carried on");
        httpEnd(rcSsl, rcHttp);
        dlStatus(false);
        return;
    }
    // Shown at once, so that it is plain the downloads go on.
    char sub[96];
    sceClibSnprintf(sub, sizeof(sub), "%d to go, in the background", count);
    notifyShow("VitaPlex downloads", sub, -1.0f);
    int done = 0, failed = 0, gone = 0, n = 0;
    const char* lastTitle = "";
    static char title[96];
    title[0] = 0;
    char* f[kFMax];
    bool stopped = false;
    if (dlOpenList()) {
        while (n < count && dlNext()) {
            if (!startsWith(g_dlLine, "item=")) continue;
            const int nf = dlFields(g_dlLine + 5, f, kFMax);
            if (nf <= kFArg1) continue;
            if (dlStopped()) {
                stopped = true;
                break;
            }
            char label[40];
            sceClibSnprintf(label, sizeof(label), "%d of %d (%s)", n + 1, count, f[kFKey]);
            lock();
            g_dl[n].state = kDlGoing;
            unlock();
            const int state = dlOne(tmpl, n, count, f, nf, label);
            lock();
            g_dl[n].state = state;
            if (state == kDlDone) g_dl[n].bytes = g_dl[n].total ? g_dl[n].total : g_dl[n].bytes;
            unlock();
            dlStatus(true);
            if (state == kDlDone) {
                done++;
                copyText(title, f[kFTitle], sizeof(title));
                lastTitle = title;
            } else if (state == kDlGone) {
                gone++;
            } else if (state == kDlFailed) {
                failed++;
            } else {
                stopped = true;
                break;
            }
            n++;
        }
        dlCloseList();
    }
    httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    httpEnd(rcSsl, rcHttp);
    dlStatus(false);
    logLine("download: %s: %d done, %d failed, %d gone from the server, %d not begun",
            stopped ? "stopped" : "all carried on", done, failed, gone, count - n);
    lock();
    const int paused = g_dlPaused;
    unlock();
    if (stopped && paused) {
        notifyEnd("Downloads paused", "Resume them in VitaPlex");
    } else if (stopped) {
        // Left showing, for VitaPlex to go on with as it downloads again.
        notifyHold();
    } else if (failed + gone > 0) {
        sceClibSnprintf(sub, sizeof(sub), "%d downloaded, %d go on when VitaPlex opens", done,
                        failed + gone);
        notifyEnd("Downloads stopped", sub);
    } else if (done == 1) {
        notifyEnd("Download complete", lastTitle);
    } else {
        sceClibSnprintf(sub, sizeof(sub), "%d items are now available offline", done);
        notifyEnd("Downloads complete", sub);
    }
}

// The download thread: the list carried on, and again for each new one that
// comes meanwhile.
int dlThread(SceSize, void*) {
    for (;;) {
        dlRun();
        lock();
        if (g_dlAgain) {
            g_dlAgain = 0;
            g_dlStop = 0;
            unlock();
            continue;
        }
        g_dlRunning = 0;
        unlock();
        break;
    }
    return sceKernelExitDeleteThread(0);
}

// From VitaPlex as it goes (QM_CMD_DOWNLOADS). Under g_lock.
void startDownloads() {
    if (g_dlRunning) {
        // The run under way stops at its next piece and takes the new list up.
        logLine("download: a new list, taken up once the one under way stops");
        g_dlAgain = 1;
        g_dlStop = 1;
        return;
    }
    g_dlStop = 0;
    g_dlAgain = 0;
    g_dlPaused = 0;
    const SceUID thread = sceKernelCreateThread("VitaPlexQM::Download", dlThread, kThreadPriority,
                                                kDlStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("download: no thread to carry them on: 0x%08X", (unsigned)rc);
        return;
    }
    g_dlRunning = 1;
}

// From VitaPlex as it opens (QM_CMD_TAKE_DOWNLOADS). Under g_lock.
void takeDownloads() {
    logLine("download: VitaPlex takes the downloads back%s",
            g_dlRunning ? "" : "; none carried on here");
    g_dlStop = 1;
    g_dlAgain = 0;
}

// ── Threads ─────────────────────────────────────────────────────────────────
// The note of an open begun, as the quick menu is seen open. Under g_lock.
void noteOpened() {
    sceClibMemset(&g_note, 0, sizeof(g_note));
    g_note.foundMs = g_note.takenMs = g_note.firstPutMs = -1;
    g_note.logIt = g_notesLeft > 0;
    if (g_note.logIt) g_notesLeft--;
}

// The note of the open just closed, logged if it is one to log: whether and
// when the row was taken, what was put back after, and the lines 1 s in.
// Under g_lock.
void noteClosed(SceUInt64 now) {
    if (!g_note.logIt) return;
    g_note.logIt = 0;
    char taken[64];
    if (g_note.takenMs >= 0)
        sceClibSnprintf(taken, sizeof(taken), "taken at %d ms", g_note.takenMs);
    else if (!g_active)
        sceClibSnprintf(taken, sizeof(taken), "nobody's");
    else if (g_note.foundMs >= 0)
        sceClibSnprintf(taken, sizeof(taken), "not taken, its buttons found at %d ms",
                        g_note.foundMs);
    else
        sceClibSnprintf(taken, sizeof(taken), "not taken, no buttons found");
    logLine("quick menu: open %d ms, the row %s, %s%s; then buttons %d, lines %d, lines shown "
            "%d, play icon %d, cover %d, lit %d, the first at %d ms",
            msOpen(now), g_note.takenMs >= 0 ? ownerName(g_note.owner) : ownerName(g_owner),
            taken, g_note.takenMs >= 0 ? " for it" : "", g_note.buttons, g_note.lines,
            g_note.shown, g_note.playIcon, g_note.cover, g_note.lit, g_note.firstPutMs);
    if (g_note.snapped) logLine("quick menu: 1 s in: %s", g_note.snap);
}

// Watches the quick menu open and close, as ElevenMPV-A's plugin does, gives
// the row back when its owner has gone quiet, and writes the log.
class WatchThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        const SceUInt64 started = sceKernelGetSystemTimeWide();
        SceUInt64 lastLoop = started;
        int wasOpen = 0;
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
                g_openSerial++;
                noteOpened();
            } else if (!open && wasOpen) {
                noteClosed(now);
            }
            g_open = open;
            if (g_active && g_owner != kOwnerShell &&
                now - g_activatedUs > QM_ACTIVE_TIMEOUT_S * 1000000ULL) {
                // Its owner gone without giving the row back.
                g_active = 0;
                g_releaseDue = 1;
                logLine("nothing from %s for %d s: row given back", ownerName(g_owner),
                        QM_ACTIVE_TIMEOUT_S);
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
            notifyHeldTick(now);
            notifyPauseTick();
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
            notifyAsked();
        }
    }

private:
    static void handle(const QmMessage& m) {
        switch (m.cmd) {
        case QM_CMD_ACTIVATE:
            if (!g_active || g_owner != m.arg) {
                logLine("row wanted by %s", ownerName(m.arg));
                notesWanted();
            }
            // A process with the row has the music: SceShell leaves it to
            // it, and does not begin a queue handed over before. VitaPlex
            // plays its own, which SceShell's would play over, so that one
            // stops.
            stopCarry("a process takes the row", kEndStopped);
            g_playQueueDue = 0;
            if (g_owner != m.arg) g_artSerial = 0;  // the last owner's cover
            g_active = 1;
            g_owner = m.arg;
            g_activatedUs = sceKernelGetSystemTimeWide();
            break;
        case QM_CMD_DEACTIVATE:
            if (g_active) {
                logLine("row given back by %s", ownerName(g_owner));
                notesWanted();
            }
            stopCarry("the row given back", kEndRowBack);
            g_active = 0;
            g_releaseDue = 1;
            g_artSerial = 0;
            break;
        case QM_CMD_SONG:
            sceClibMemcpy(g_title, m.title, sizeof(g_title));
            sceClibMemcpy(g_artist, m.artist, sizeof(g_artist));
            sceClibMemcpy(g_album, m.album, sizeof(g_album));
            g_title[255] = g_artist[255] = g_album[255] = 0;
            break;
        case QM_CMD_PLAYING:
            g_playing = (int)m.arg;
            break;
        case QM_CMD_ART:
            g_artSerial = m.arg;
            break;
        case QM_CMD_PLAY_QUEUE:
            // A queue SceShell carries on yields to the new one, its music
            // left for the new one to stop, and nothing written.
            logLine("play queue: VitaPlex hands its queue to SceShell%s",
                    g_carry ? "; the queue carried on stops first" : "");
            stopCarry("VitaPlex hands a queue over", kEndYield);
            if (startMusicThread()) {
                g_playQueueDue = 1;
                sceKernelSignalSema(g_musicSema, 1);
            } else {
                logLine("play queue: no music thread, so nothing plays");
            }
            break;
        case QM_CMD_TAKE_BACK:
            logLine("take back: VitaPlex takes the music back%s",
                    g_carry ? "" : "; no queue carried on here");
            g_playQueueDue = 0;  // a queue handed over and not yet begun is not begun
            stopCarry("VitaPlex takes the music back", kEndTakeBack);
            break;
        case QM_CMD_INSTALL_UPDATE:
            startUpdate();
            break;
        case QM_CMD_DOWNLOADS:
            startDownloads();
            break;
        case QM_CMD_TAKE_DOWNLOADS:
            takeDownloads();
            break;
        case QM_CMD_DL_PROGRESS:
        case QM_CMD_DL_PROGRESS_END:
            g_notifyAsk.due = m.cmd == QM_CMD_DL_PROGRESS ? 1 : 2;
            g_notifyAsk.permille = m.arg;
            sceClibMemcpy(g_notifyAsk.text, m.title, sizeof(g_notifyAsk.text) - 2);
            sceClibMemcpy(g_notifyAsk.sub, m.artist, sizeof(g_notifyAsk.sub) - 2);
            g_notifyAsk.text[63] = g_notifyAsk.sub[63] = 0;
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

// ── The download notification's owner ──────────────────────────────────────
// A notification shows its owner's icon, and SceShell finds the owner of one
// begun through SceNotificationUtil by asking sceAppMgrGetStatusByAppId
// (SceAppMgrUser 0x47958DE5) about the app that called (0x8123ED38 on 3.60,
// read from SceShell's code, build 2072): the title id at 0xC of the status,
// used when the byte at 0x70 is 0, else NPXS10998 or NPXS10999 for 1 and 2.
// The plugin's caller is SceShell, no app, and its notification had no icon.
// So for the one lookup after the plugin begins its notification, the answer
// is VitaPlex: its title, and its icon. Only an answer that names no app or a
// system title (NPXS...) is changed, so that a game's own is never taken, and
// only once, within kOwnerWindowUs of the Begin. Build 2074 changed only
// answers naming no app, and changed none: the lookup named one, which the
// log did not say; every lookup in the window is logged now.
const uint32_t kStatusTitleId = 0xC, kStatusKind = 0x70, kStatusSize = 0x74;

int statusAsVitaPlex(int appId, uint8_t* status) {
    const TaiHookUser* cur = (const TaiHookUser*)g_statusHookRef;
    const TaiHookUser* next = (const TaiHookUser*)cur->next;
    typedef int (*Fn)(int, uint8_t*);
    const int ret = next ? ((Fn)next->func)(appId, status) : ((Fn)cur->old)(appId, status);
    const SceUInt64 until = __atomic_load_n(&g_ownerUntilUs, __ATOMIC_SEQ_CST);
    if (!until || !status || sceKernelGetSystemTimeWide() > until) return ret;
    char title[16];
    title[0] = 0;
    if (ret >= 0) copyText(title, (const char*)status + kStatusTitleId, 10);
    const bool system = startsWith(title, "NPXS");
    logLine("notification: owner lookup for app 0x%X gave 0x%08X, kind %d, title \"%s\"%s",
            (unsigned)appId, (unsigned)ret, ret >= 0 ? (int)status[kStatusKind] : -1, title,
            ret >= 0 && status[kStatusKind] == 0 && !system ? ", an app's: left" : "; answered VitaPlex");
    if (ret >= 0 && status[kStatusKind] == 0 && !system) return ret;  // an app: its own
    __atomic_store_n(&g_ownerUntilUs, 0, __ATOMIC_SEQ_CST);
    if (ret < 0) sceClibMemset(status, 0, kStatusSize);
    status[kStatusKind] = 0;
    copyText((char*)status + kStatusTitleId, VITAPLEX_TITLEID, 10);
    return 0;
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

    g_lock = sceKernelCreateMutex("VitaPlexQM::Lock", 0, 0, NULL);
    g_netLock = sceKernelCreateMutex("VitaPlexQM::Net", 0, 0, NULL);
    g_notifyLock = sceKernelCreateMutex("VitaPlexQM::Notify", 0, 0, NULL);
    // As ElevenMPV-A's pipes are made: 0x40, user main memory; 0x81, whole
    // messages and openable by name.
    g_toPlugin = sceKernelCreateMsgPipe(QM_PIPE_TO_PLUGIN, 0x40, 0x81,
                                        sizeof(QmMessage) * QM_TO_PLUGIN_SLOTS, NULL);
    g_fromPlugin = sceKernelCreateMsgPipe(QM_PIPE_FROM_PLUGIN, 0x40, 0x81,
                                          sizeof(QmPress) * QM_FROM_PLUGIN_SLOTS, NULL);
    if (g_lock < 0 || g_netLock < 0 || g_notifyLock < 0 || g_toPlugin < 0 || g_fromPlugin < 0) {
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
    if (hooked < 0) logLine("BGM priority SceShell reads not capped: 0x%08X", (unsigned)hooked);
    hook.func_nid = 0x47958DE5;  // sceAppMgrGetStatusByAppId, in the same library
    hook.hook_func = (const void*)statusAsVitaPlex;
    const SceUID hookedStatus = vitaplexQmHookFunctionImport(&g_statusHookRef, &hook);
    if (hookedStatus < 0)
        logLine("the download notification's owner not hooked: 0x%08X", (unsigned)hookedStatus);

    // PAF's list of what its main (UI) thread calls each time round its loop;
    // ElevenMPV-A's app imports Register, so its NID is the console's.
    paf::common::MainThreadCallList::Register(mainTick, NULL);

    (new WatchThread(kThreadPriority, 0x4000, "VitaPlexQM::Watch"))->Start();
    // Twice the watch thread's: it shows VitaPlex's download notification
    // too (notifyAsked), through SceShell's service, as the download thread does.
    (new ReceiveThread(kThreadPriority, 0x8000, "VitaPlexQM::Receive"))->Start();
    logLine("ready");
    return SCE_KERNEL_START_SUCCESS;
}

// It stays: its threads and SceShell's widgets hold on to it.
extern "C" int module_stop(SceSize args, const void* argp) {
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_CANCEL;
}
