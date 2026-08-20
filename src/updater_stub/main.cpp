/*
    VitaPlex — updater stub

    A tiny standalone Vita app whose ONLY job is to install a VitaPlex update
    while VitaPlex itself is closed, then relaunch it. This is the AutoPlugin2
    technique: a title cannot promote over itself (the Vita installer returns
    0x80101114 "in use" for the running app), so VitaPlex installs and launches
    THIS stub, which — now that VitaPlex is no longer the running title — can
    promote VitaPlex's downloaded VPK cleanly, then boots VitaPlex.

    Everything is fixed by convention so no arguments need to cross the app
    boundary:
      - the VPK to install : ux0:data/VitaPlex/update.vpk
      - the target version : ux0:data/VitaPlex/update_version.txt (optional)
      - the app to relaunch : VPLEX0001 (VitaPlex's title id)

    The heavy lifting (extract + head.bin forge + promote) is shared verbatim
    with VitaPlex via utils/vita_install, which is deliberately free of any
    borealis / fmt dependency so this stub links it without the UI stack. The
    screen is a plain-vita2d step checklist (design_handoff_stub_B).
*/

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vita2d.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "utils/vita_install.hpp"

namespace {

const char* kUpdateVpk  = "ux0:data/VitaPlex/update.vpk";
const char* kVersionTxt = "ux0:data/VitaPlex/update_version.txt";
const char* kWorkDir    = "ux0:data/VitaPlex/updater_work";
const char* kTargetId   = "VPLEX0001";

// ── Palette (design_handoff_stub_B) ──────────────────────────────────────
#define C_BG        RGBA8(0x14, 0x14, 0x16, 0xFF)
#define C_PANEL     RGBA8(0x1E, 0x1E, 0x21, 0xFF)
#define C_DIVIDER   RGBA8(0x2A, 0x2A, 0x2E, 0xFF)
#define C_GOLD      RGBA8(0xE5, 0xA0, 0x0D, 0xFF)
#define C_GOLDBR    RGBA8(0xFF, 0xC2, 0x3D, 0xFF)
#define C_TILEBG    RGBA8(0xE5, 0xA0, 0x0D, 26)
#define C_TILEBRD   RGBA8(0xE5, 0xA0, 0x0D, 77)
#define C_GOLDFILL  RGBA8(0xE5, 0xA0, 0x0D, 26)
#define C_GREEN     RGBA8(0x5F, 0xE2, 0x87, 0xFF)
#define C_GREENRING RGBA8(0x5F, 0xE2, 0x87, 128)
#define C_GREENFILL RGBA8(0x5F, 0xE2, 0x87, 31)
#define C_RED       RGBA8(0xEF, 0x8A, 0x8D, 0xFF)
#define C_REDFILL   RGBA8(0xEF, 0x8A, 0x8D, 26)
#define C_WHITE     RGBA8(0xFF, 0xFF, 0xFF, 0xFF)
#define C_BODY      RGBA8(0xB4, 0xB4, 0xBA, 0xFF)
#define C_MUTED     RGBA8(0x8A, 0x8A, 0x90, 0xFF)
#define C_FAINT     RGBA8(0x6A, 0x6A, 0x70, 0xFF)
#define C_TODOTXT   RGBA8(0x55, 0x55, 0x5C, 0xFF)
#define C_OUTLINE   RGBA8(0x3E, 0x3E, 0x44, 0xFF)
#define C_TRACK     RGBA8(0xFF, 0xFF, 0xFF, 26)

vita2d_pgf* g_font = nullptr;

// ── UI state, redrawn in full every frame ────────────────────────────────
enum StepState { TODO, NOW, DONE, FAIL };
struct Ui {
    StepState steps[4] = { TODO, TODO, TODO, TODO };
    int   pct = 0;               // overall bar 0-100
    int   spinTick = 0;          // frame counter for the spinner
    std::string version;         // "Beta 1.3.0" or empty
    std::string sizeMB;          // "44.6" or empty
    std::string installCap;      // caption under the Install step
    std::string note;            // bottom note (overridden on failure)
    bool  failed = false;
};
Ui g_ui;

void log(const char* msg) {
    SceUID fd = sceIoOpen("ux0:data/VitaPlex/updater.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, msg, std::strlen(msg));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

std::string readTextFile(const char* path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return {};
    char buf[128];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string s(buf);
    // Trim trailing whitespace / newlines.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string fileSizeMB(const char* path) {
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) return {};
    double mb = (double)st.st_size / (1024.0 * 1024.0);
    char b[32];
    snprintf(b, sizeof(b), "%.1f", mb);
    return b;
}

// ── Drawing helpers ──────────────────────────────────────────────────────

// A ring: outer filled circle in `ring`, inner filled circle in `fill`.
void drawRing(float cx, float cy, float r, unsigned ring, unsigned fill) {
    vita2d_draw_fill_circle(cx, cy, r, ring);
    vita2d_draw_fill_circle(cx, cy, r - 1.6f, fill);
}

// A short 2px-ish line (two 1px lines offset) for crisp glyph strokes.
void thick(float x0, float y0, float x1, float y1, unsigned c) {
    vita2d_draw_line(x0, y0, x1, y1, c);
    vita2d_draw_line(x0 + 1, y0, x1 + 1, y1, c);
    vita2d_draw_line(x0, y0 + 1, x1, y1 + 1, c);
}

void drawCheck(float cx, float cy, unsigned c) {
    thick(cx - 5.0f, cy + 0.5f, cx - 1.5f, cy + 4.0f, c);
    thick(cx - 1.5f, cy + 4.0f, cx + 5.0f, cy - 3.5f, c);
}

void drawCross(float cx, float cy, unsigned c) {
    thick(cx - 4.0f, cy - 4.0f, cx + 4.0f, cy + 4.0f, c);
    thick(cx - 4.0f, cy + 4.0f, cx + 4.0f, cy - 4.0f, c);
}

// A rotating gold dot inside the active circle (the spinner).
void drawSpinner(float cx, float cy, int tick) {
    float a = (float)tick * 0.5f;
    vita2d_draw_fill_circle(cx + std::cos(a) * 6.0f, cy + std::sin(a) * 6.0f, 2.4f, C_GOLD);
}

// Word-wrap `text` to `maxW` and draw from (x, y) downward at `scale`.
void drawWrapped(float x, float y, float maxW, float scale, unsigned color,
                 const std::string& text, float lineH) {
    std::string line, word;
    float ry = y;
    auto flush = [&](const std::string& s) {
        vita2d_pgf_draw_text(g_font, x, ry, color, scale, s.c_str());
        ry += lineH;
    };
    for (size_t i = 0; i <= text.size(); i++) {
        char ch = i < text.size() ? text[i] : ' ';
        if (ch == ' ') {
            std::string cand = line.empty() ? word : line + " " + word;
            if (!cand.empty() &&
                vita2d_pgf_text_width(g_font, scale, cand.c_str()) > maxW && !line.empty()) {
                flush(line);
                line = word;
            } else {
                line = cand;
            }
            word.clear();
        } else {
            word += ch;
        }
    }
    if (!line.empty()) flush(line);
}

void render() {
    vita2d_start_drawing();
    vita2d_clear_screen();

    // ── Left brand panel ─────────────────────────────────────────────────
    vita2d_draw_rectangle(0, 0, 400, 544, C_PANEL);
    vita2d_draw_rectangle(400, 0, 1, 544, C_DIVIDER);

    // Icon tile (square approximation of the rounded tile) + border + arrow.
    vita2d_draw_rectangle(46, 160, 58, 58, C_TILEBG);
    vita2d_draw_rectangle(46, 160, 58, 1, C_TILEBRD);
    vita2d_draw_rectangle(46, 217, 58, 1, C_TILEBRD);
    vita2d_draw_rectangle(46, 160, 1, 58, C_TILEBRD);
    vita2d_draw_rectangle(103, 160, 1, 58, C_TILEBRD);
    {
        float cx = 75.0f;
        vita2d_draw_rectangle(cx - 1, 176, 2, 18, C_GOLD);       // shaft
        thick(cx, 196, cx - 7, 189, C_GOLD);                     // arrowhead L
        thick(cx, 196, cx + 7, 189, C_GOLD);                     // arrowhead R
        vita2d_draw_rectangle(cx - 10, 202, 20, 2, C_GOLD);      // tray
    }

    // Title: "Vita" white + "Plex" gold + " Updater" white.
    {
        float x = 46, y = 262, s = 1.4f;
        vita2d_pgf_draw_text(g_font, x, y, C_WHITE, s, "Vita");
        x += vita2d_pgf_text_width(g_font, s, "Vita");
        vita2d_pgf_draw_text(g_font, x, y, C_GOLD, s, "Plex");
        x += vita2d_pgf_text_width(g_font, s, "Plex");
        vita2d_pgf_draw_text(g_font, x, y, C_WHITE, s, " Updater");
    }

    // Body sentence (version highlighted, clause omitted when unknown).
    if (!g_ui.version.empty()) {
        vita2d_pgf_draw_text(g_font, 46, 294, C_MUTED, 0.85f, "Updating to");
        float vx = 46 + vita2d_pgf_text_width(g_font, 0.85f, "Updating to ");
        vita2d_pgf_draw_text(g_font, vx, 294, C_GOLDBR, 0.85f, g_ui.version.c_str());
        drawWrapped(46, 316, 308, 0.85f, C_MUTED,
                    "VitaPlex will restart automatically when this finishes.", 22);
    } else {
        drawWrapped(46, 294, 308, 0.85f, C_MUTED,
                    "Installing the update. VitaPlex will restart automatically "
                    "when this finishes.", 22);
    }

    // ── Right step column ────────────────────────────────────────────────
    struct Row { const char* label; std::string cap; };
    Row rows[4] = {
        { "Extract package",  g_ui.sizeMB.empty() ? std::string("update.vpk")
                                                   : "update.vpk \xC2\xB7 " + g_ui.sizeMB + " MB" },
        { "Verify contents",  "head.bin forged \xC2\xB7 files OK" },
        { "Install",          g_ui.installCap },
        { "Relaunch VitaPlex", "" },
    };

    const float colX = 469.0f;   // circle centre x
    const float lblX = 496.0f;
    const float y0   = 178.0f;
    const float dy   = 44.0f;

    for (int i = 0; i < 4; i++) {
        float cy = y0 + dy * i;
        StepState s = g_ui.steps[i];

        if (s == TODO) {
            drawRing(colX, cy, 13, C_OUTLINE, C_BG);
        } else if (s == NOW) {
            drawRing(colX, cy, 13, C_GOLD, C_BG);
            vita2d_draw_fill_circle(colX, cy, 11.4f, C_GOLDFILL);
            drawSpinner(colX, cy, g_ui.spinTick);
        } else if (s == DONE) {
            drawRing(colX, cy, 13, C_GREENRING, C_BG);
            vita2d_draw_fill_circle(colX, cy, 11.4f, C_GREENFILL);
            drawCheck(colX, cy, C_GREEN);
        } else { // FAIL
            drawRing(colX, cy, 13, C_RED, C_BG);
            vita2d_draw_fill_circle(colX, cy, 11.4f, C_REDFILL);
            drawCross(colX, cy, C_RED);
        }

        unsigned lblCol = (s == TODO) ? C_TODOTXT
                        : (s == DONE) ? C_BODY : C_WHITE;
        vita2d_pgf_draw_text(g_font, lblX, cy - 2, lblCol, 0.95f, rows[i].label);
        if (!rows[i].cap.empty() && s != TODO)
            vita2d_pgf_draw_text(g_font, lblX, cy + 16, C_FAINT, 0.75f, rows[i].cap.c_str());
    }

    // Progress bar under the last step.
    {
        float bx = 456, by = y0 + dy * 3 + 40, bw = 448;
        vita2d_draw_rectangle(bx, by, bw, 6, C_TRACK);
        int p = g_ui.pct < 0 ? 0 : (g_ui.pct > 100 ? 100 : g_ui.pct);
        if (p > 0)
            vita2d_draw_rectangle(bx, by, bw * p / 100.0f, 6, g_ui.failed ? C_RED : C_GOLD);
        vita2d_pgf_draw_text(g_font, bx, by + 26, C_FAINT, 0.75f, g_ui.note.c_str());
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void relaunchVitaPlex() {
    g_ui.steps[3] = NOW;
    render();
    vita::launchTitle(kTargetId);
}

}  // namespace

int main(int, char*[]) {
    log("stub: started");

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    g_font = vita2d_load_default_pgf();

    g_ui.version = readTextFile(kVersionTxt);
    g_ui.sizeMB  = fileSizeMB(kUpdateVpk);
    g_ui.note    = "Do not power off or press the PS button";
    render();

    // Give the shell a moment to finish tearing down VitaPlex so the promoter
    // no longer sees the title as in use.
    sceKernelDelayThread(3 * 1000 * 1000);

    SceIoStat st;
    if (sceIoGetstat(kUpdateVpk, &st) < 0) {
        log("stub: no update.vpk, relaunching");
        relaunchVitaPlex();
        sceKernelDelayThread(1000 * 1000);
        if (g_font) vita2d_free_pgf(g_font);
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }

    g_ui.steps[0] = NOW;
    render();

    std::string err;
    int rc = vita::installVpk(kUpdateVpk, kWorkDir, err,
        [](int phase, int done, int total) {
            g_ui.spinTick++;
            if (phase == vita::PHASE_EXTRACT) {
                g_ui.steps[0] = NOW;
                g_ui.pct = total > 0 ? done * 55 / total : 0;
            } else if (phase == vita::PHASE_VERIFY) {
                g_ui.steps[0] = DONE;
                g_ui.steps[1] = (done >= 1) ? DONE : NOW;
                g_ui.pct = 62;
            } else if (phase == vita::PHASE_INSTALL) {
                g_ui.steps[0] = DONE;
                g_ui.steps[1] = DONE;
                g_ui.steps[2] = NOW;
                int p = 66 + done / 3;   // indeterminate ramp, poll-driven
                if (p > 99) p = 99;
                g_ui.pct = p;
                g_ui.installCap = "Promoting to the system\xE2\x80\xA6 " + std::to_string(p) + "%";
            }
            render();
        });

    if (rc == 0) {
        log("stub: install ok");
        sceIoRemove(kUpdateVpk);
        sceIoRemove(kVersionTxt);
        g_ui.steps[0] = g_ui.steps[1] = g_ui.steps[2] = DONE;
        g_ui.pct = 100;
        g_ui.installCap = "Installed";
        render();
    } else {
        log(("stub: install failed: " + err).c_str());
        g_ui.failed = true;
        g_ui.steps[2] = FAIL;
        g_ui.installCap = err.size() > 48 ? err.substr(0, 48) : err;
        g_ui.note = "Update kept at ux0:data/VitaPlex/update.vpk - install it with VitaShell";
        render();
        sceKernelDelayThread(4 * 1000 * 1000);
    }

    // Relaunch VitaPlex either way — the updated build on success, the
    // existing one on failure, so the user is never left on the LiveArea.
    relaunchVitaPlex();
    sceKernelDelayThread(1000 * 1000);

    if (g_font) vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
