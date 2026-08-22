/**
 * VitaPlex - In-app updates implementation
 *
 * Ported from pleNx (thcolin/gamepad-media-center-aggregator,
 * app/src/utils/version.cpp) onto VitaPlex's HttpClient and dialogs.
 * See utils/app_update.hpp for the per-platform behaviour.
 */

#include "utils/app_update.hpp"

#include <borealis.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstdint>
#include <memory>
#include <vector>

#include "app/application.hpp"
#include "platform/paths.hpp"
#include "platform/platform.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"

#ifdef __SWITCH__
#include <switch.h>
#include <filesystem>
#endif
#ifdef __PSV__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "utils/vita_install.hpp"
#endif
#ifdef ANDROID
#include <SDL2/SDL.h>
#include <jni.h>
#include <sys/system_properties.h>
#endif
#ifdef __PS4__
#include "utils/ps4_install.hpp"
#endif
#if defined(__linux__) && !defined(ANDROID)
#include <unistd.h>       // readlink, access, fork, setsid, execl/execlp, _exit
#include <sys/stat.h>     // chmod (make the replacement AppImage executable)
#include <filesystem>     // AppImage/self-path maths
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// Desktop macOS only. TARGET_OS_IPHONE is 1 on iOS/tvOS, which keep the
// browser flow; the Mac (TARGET_OS_IPHONE == 0) gets the in-app .app swap.
#if defined(__APPLE__) && !TARGET_OS_IPHONE
#define VITAPLEX_MACOS_DESKTOP 1
#else
#define VITAPLEX_MACOS_DESKTOP 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>      // GetModuleFileNameA + CreateProcessA for the relaunch helper
#endif
#if VITAPLEX_MACOS_DESKTOP
#include <unistd.h>       // fork, setsid, execl, getpid, _exit
#include <sys/stat.h>     // chmod (make the updater script executable)
#include <mach-o/dyld.h>  // _NSGetExecutablePath (locate the .app bundle)
#endif

namespace vitaplex {
namespace app_update {

namespace {

constexpr const char* kRepo = "Breezyslasher/Vita_plex";

// True while a check or an install is running, so the settings cell and
// the startup check can't race each other.
std::atomic<bool> s_busy{false};
// Set by the progress dialog's Cancel; the download's write callback
// checks it and aborts the transfer.
std::atomic<bool> s_cancel{false};

std::string s_selfPath;

// ── Small JSON pulls ─────────────────────────────────────────────────────
// The releases feed is a top-level array; each release carries an assets
// array. String-aware object walking, same technique as the rest of the
// app — a brace inside release notes must not end the walk.

size_t objectEnd(const std::string& body, size_t objStart) {
    int depth = 0;
    bool inString = false;
    size_t pos = objStart;
    for (; pos < body.size(); pos++) {
        const char c = body[pos];
        if (inString) {
            if (c == '\\') { pos++; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return pos + 1;
    }
    return body.size();
}

template <typename Fn>
void forEachTopLevelObject(const std::string& body, Fn fn) {
    size_t pos = body.find('[');
    if (pos == std::string::npos) return;
    pos++;
    while (pos < body.size()) {
        while (pos < body.size() && body[pos] != '{' && body[pos] != ']') pos++;
        if (pos >= body.size() || body[pos] == ']') break;
        size_t end = objectEnd(body, pos);
        if (!fn(body.substr(pos, end - pos))) return;
        pos = end;
    }
}

std::string jsonString(const std::string& obj, const std::string& key) {
    size_t pos = obj.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    pos = obj.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = obj.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos || obj[pos] != '"') return {};
    std::string out;
    for (size_t i = pos + 1; i < obj.size(); i++) {
        char c = obj[i];
        if (c == '\\' && i + 1 < obj.size()) {
            char n = obj[++i];
            if (n == 'n') out += '\n';
            else if (n == 'r') { /* drop */ }
            else if (n == 't') out += '\t';
            else if (n == 'u' && i + 4 < obj.size()) { i += 4; out += '?'; }
            else out += n;
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

int64_t jsonInt(const std::string& obj, const std::string& key) {
    size_t pos = obj.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = obj.find(':', pos);
    if (pos == std::string::npos) return 0;
    return strtoll(obj.c_str() + pos + 1, nullptr, 10);
}

bool jsonBool(const std::string& obj, const std::string& key) {
    size_t pos = obj.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = obj.find(':', pos);
    if (pos == std::string::npos) return false;
    pos = obj.find_first_not_of(" \t\n\r", pos + 1);
    return pos != std::string::npos && obj.compare(pos, 4, "true") == 0;
}

// ── Versions ─────────────────────────────────────────────────────────────
// Tags read "Beta-1.2.6", the built-in version "Beta 1.2.6" or
// "1.2.6.1448": compare the first three numeric runs of each. The build
// number deliberately does not count — updates trigger on version bumps.

bool parseVersion(const std::string& s, int out[3]) {
    int n = 0;
    size_t i = 0;
    while (i < s.size() && n < 3) {
        if (isdigit((unsigned char)s[i])) {
            out[n++] = atoi(s.c_str() + i);
            while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        } else {
            i++;
        }
    }
    for (int k = n; k < 3; k++) out[k] = 0;
    return n > 0;
}

bool isNewer(const std::string& tag, const std::string& current) {
    int a[3], b[3];
    if (!parseVersion(tag, a) || !parseVersion(current, b)) return false;
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

#if defined(__linux__) && !defined(ANDROID)
// ── Linux packaging detection ────────────────────────────────────────────
// The AppImage, the .deb and the Arch package are the SAME PLATFORM_DESKTOP
// binary — nothing at build time says which — so we detect the install kind at
// runtime:
//   * AppImage : the runtime sets $APPIMAGE to the .AppImage path. It's a
//                single, user-owned file → we replace it and relaunch.
//   * Deb      : dpkg lays the binary at /usr/lib/VitaPlex/VitaPlex.
//   * Arch     : pacman-managed system install under /usr.
// Flatpak sets $FLATPAK_ID and updates through the software centre, so it (and
// anything unrecognised) stays browser-only.
enum class LinuxPkg { None, AppImage, Deb, Arch };

std::string selfExePath() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

LinuxPkg linuxPkg() {
    if (std::getenv("APPIMAGE")) return LinuxPkg::AppImage;
    if (std::getenv("FLATPAK_ID") || ::access("/.flatpak-info", F_OK) == 0)
        return LinuxPkg::None;

    const std::string exe = selfExePath();
    if (exe.rfind("/usr/lib/VitaPlex/", 0) == 0 &&
        ::access("/var/lib/dpkg/status", F_OK) == 0)
        return LinuxPkg::Deb;
    if (!exe.empty() && exe.rfind("/usr/", 0) == 0 &&
        ::access("/var/lib/pacman", F_OK) == 0)
        return LinuxPkg::Arch;
    return LinuxPkg::None;
}
#endif

#if defined(_WIN32)
// ── Windows self-update helper ───────────────────────────────────────────
// The Windows build ships as a portable folder — VitaPlex.exe + its runtime
// DLLs + resources/ — zipped at the archive root. The running .exe and the
// loaded DLLs are locked, so the swap can't happen in-process: we download
// the zip, then hand a tiny .bat to a detached cmd that waits for VitaPlex to
// exit, unpacks the zip over the install folder and relaunches. Same "hand
// off, then quit" shape the console/Linux paths already use.
std::string winInstallDir() {
    char buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return {};
    std::string exe(buf, n);
    size_t slash = exe.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : exe.substr(0, slash);
}
#endif

#if VITAPLEX_MACOS_DESKTOP
// ── macOS self-update helpers ────────────────────────────────────────────
// The macOS build ships as VitaPlex.app inside a .dmg. In-app update mounts
// the downloaded dmg and swaps the whole .app bundle from a detached helper
// once VitaPlex has quit, then reopens it — the classic Sparkle shape.
std::string macExePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);          // asks for the buffer size
    if (size == 0) return {};
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) != 0) return {};
    buf.resize(size ? size - 1 : 0);               // drop the trailing NUL
    return buf;
}

// The .app bundle root when we're launched from one, else empty — a loose
// binary can't be swapped as a bundle, so it falls back to the browser.
std::string macAppBundlePath() {
    const std::string exe = macExePath();
    const std::string tail = "/Contents/MacOS/VitaPlex";
    if (exe.size() > tail.size() &&
        exe.compare(exe.size() - tail.size(), tail.size(), tail) == 0) {
        std::string root = exe.substr(0, exe.size() - tail.size());
        if (root.size() >= 4 && root.compare(root.size() - 4, 4, ".app") == 0)
            return root;
    }
    return {};
}
#endif

// ── Platform asset choice ────────────────────────────────────────────────
// Release assets follow "VitaPlex.<tag>-<platform>…"; the suffix decides.
// Empty = this platform has no single downloadable asset (browser-only).
std::string assetSuffix() {
#if defined(__PSV__)
    return ".vpk";
#elif defined(__SWITCH__)
#if defined(VITAPLEX_NRO_DEKO3D)
    return "-switch-deko3d.nro";
#else
    return "-switch-opengl.nro";
#endif
#elif defined(ANDROID)
    // Pick the APK by what the DEVICE supports, not the running binary's
    // ABI: a 64-bit phone must get arm64-v8a even if a 32-bit build was
    // somehow installed on it (the reported "downloaded the wrong arch"
    // bug). ro.product.cpu.abilist lists the device ABIs best-first.
    {
        char abilist[256] = {0};
        __system_property_get("ro.product.cpu.abilist", abilist);
        std::string abis = abilist;
        if (abis.find("arm64-v8a") != std::string::npos)   return "-arm64-v8a.apk";
        if (abis.find("armeabi-v7a") != std::string::npos) return "-armeabi-v7a.apk";
    }
    // Fallback: the running binary's own ABI.
#if defined(__aarch64__)
    return "-arm64-v8a.apk";
#else
    return "-armeabi-v7a.apk";
#endif
#elif defined(__PS4__)
    return "-ps4.pkg";
#elif defined(__linux__) && !defined(ANDROID)
    switch (linuxPkg()) {
        case LinuxPkg::AppImage:
#if defined(__x86_64__)
            return "-x86_64.AppImage";
#elif defined(__aarch64__)
            return "-aarch64.AppImage";
#else
            return {};
#endif
        case LinuxPkg::Deb:
#if defined(__x86_64__)
            return "-Linux_amd64.deb";
#elif defined(__aarch64__)
            return "-Linux_arm64.deb";
#else
            return {};
#endif
        case LinuxPkg::Arch:
            return "-Linux.pkg.tar.zst";
        default:
            return {};   // Flatpak / unknown → browser-only
    }
#elif defined(_WIN32)
#if defined(__aarch64__) || defined(_M_ARM64)
    return "-windows-arm64.zip";
#else
    return "-windows-x64.zip";
#endif
#elif VITAPLEX_MACOS_DESKTOP
    // Only when we can actually swap the .app bundle; a loose binary drops
    // through to the browser.
    if (macAppBundlePath().empty()) return {};
#if defined(__aarch64__) || defined(__arm64__)
    return "-macOS-Silicon.dmg";
#else
    return "-macOS-Intel.dmg";
#endif
#else
    return {};
#endif
}

struct ReleaseInfo {
    std::string tag;
    std::string pageUrl;
    std::string assetUrl;
    int64_t     assetSize = 0;
    std::string notes;         // raw markdown body, rendered by the sheet
    std::string publishedAt;   // ISO timestamp; date fallback for the sheet
    bool        prerelease = false;
};

// ── Dialog building blocks ───────────────────────────────────────────────
// The offer and progress dialogs follow design_handoff_update — the same
// visual language as the Live TV dialogs (livetv_actions.cpp): scrim, dark
// panel, gold accents. The tokens are that file's, plus the success green
// this handoff introduces.

namespace tok {
    inline NVGcolor panel()       { return nvgRGB(0x26, 0x26, 0x2a); }
    inline NVGcolor panelLine()   { return nvgRGB(0x45, 0x45, 0x4d); }
    inline NVGcolor hairline()    { return nvgRGB(0x47, 0x47, 0x47); }
    inline NVGcolor inputBg()     { return nvgRGB(0x2e, 0x2e, 0x34); }
    inline NVGcolor gold()        { return nvgRGB(0xe5, 0xa0, 0x0d); }
    inline NVGcolor goldBright()  { return nvgRGB(0xff, 0xc2, 0x3d); }
    inline NVGcolor goldInk()     { return nvgRGB(0x24, 0x1c, 0x08); }
    inline NVGcolor goldTileBg()  { return nvgRGBA(0xe5, 0xa0, 0x0d, 33); }   // .13
    inline NVGcolor goldTileBrd() { return nvgRGBA(0xe5, 0xa0, 0x0d, 89); }   // .35
    inline NVGcolor goldCardBg()  { return nvgRGBA(0xe5, 0xa0, 0x0d, 23); }   // .09
    inline NVGcolor goldCardBrd() { return nvgRGBA(0xe5, 0xa0, 0x0d, 102); }  // .4
    inline NVGcolor green()       { return nvgRGB(0x5f, 0xe2, 0x87); }
    inline NVGcolor greenBg()     { return nvgRGBA(0x42, 0xd7, 0x6a, 36); }   // .14
    inline NVGcolor greenBrd()    { return nvgRGBA(0x42, 0xd7, 0x6a, 89); }   // .35
    inline NVGcolor text()        { return nvgRGB(255, 255, 255); }
    inline NVGcolor muted()       { return nvgRGB(0xb4, 0xb4, 0xba); }
    inline NVGcolor muted2()      { return nvgRGB(0x8a, 0x8a, 0x90); }
    inline NVGcolor disabled()    { return nvgRGB(0x6a, 0x6a, 0x70); }
    inline NVGcolor track()       { return nvgRGBA(255, 255, 255, 36); }      // .14
    inline NVGcolor btnGray()     { return nvgRGB(0x3e, 0x3e, 0x46); }
    inline NVGcolor scrim()       { return nvgRGBA(10, 9, 14, 150); }
}

// Translucent host so the screen behind shows through the scrim (same
// file-local class as livetv_actions / media_detail_view).
class OverlayActivity : public brls::Activity {
public:
    explicit OverlayActivity(brls::Box* content) : brls::Activity(content) {}
    bool isTranslucent() override { return true; }
};

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool singleLine = true) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(size);
    l->setTextColor(color);
    l->setSingleLine(singleLine);
    return l;
}

enum class BtnStyle { Gold, Gray, Ghost };

brls::Box* makeButton(const std::string& text, BtnStyle style,
                      std::function<void()> onClick) {
    auto* b = new brls::Box();
    b->setAxis(brls::Axis::ROW);
    b->setJustifyContent(brls::JustifyContent::CENTER);
    b->setAlignItems(brls::AlignItems::CENTER);
    b->setHeight(42.0f);
    b->setCornerRadius(10.0f);
    b->setFocusable(true);
    b->setHighlightCornerRadius(10.0f);
    // Focus = the warm halo ring only. The highlight fill borealis paints
    // behind a focused view washes out a gold-filled button (same fix as
    // the guide hero buttons / downloads_tab).
    b->setHideHighlightBackground(true);

    NVGcolor fg = tok::text();
    if (style == BtnStyle::Gold) {
        b->setBackgroundColor(tok::gold());
        fg = tok::goldInk();
    } else if (style == BtnStyle::Gray) {
        b->setBackgroundColor(tok::btnGray());
        b->setBorderColor(tok::hairline());
        b->setBorderThickness(1.0f);
    } else {
        fg = tok::muted();
    }
    b->addView(makeLabel(text, 13.5f, fg));

    b->registerClickAction([onClick](brls::View*) {
        if (onClick) onClick();
        return true;
    });
    b->addGestureRecognizer(new brls::TapGestureRecognizer(b));
    return b;
}

std::string mbLabel(int64_t bytes) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

// ── The in-place installers (Switch / Vita / Android / PS4) ─────────────
#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID) || defined(__PS4__) || (defined(__linux__) && !defined(ANDROID)) || defined(_WIN32) || VITAPLEX_MACOS_DESKTOP

// One row of the progress checklist (design_handoff_update, dialog C):
// a 26px state circle — hairline when pending, spinner while active, green
// check when done — a text line, and an optional 5px gold bar underneath.
struct StepRow {
    brls::Box*             icon    = nullptr;
    brls::Label*           glyph   = nullptr;
    brls::ProgressSpinner* spinner = nullptr;
    brls::Label*           text    = nullptr;
    brls::Box*             track   = nullptr;
    brls::Rectangle*       fill    = nullptr;
    float                  barW    = 0.0f;
};

// Everything the worker thread needs to drive the checklist. The raw view
// pointers stay valid until the overlay pops; `dismissed` flips first on
// every close path, and each UI sync checks it before touching a view.
struct ProgressUi {
    StepRow download, install, relaunch;
    brls::Box* cancel = nullptr;
    std::shared_ptr<std::atomic<bool>> dismissed =
        std::make_shared<std::atomic<bool>>(false);
    std::atomic<int> phase{0};   // 0 = downloading (cancelable), 1 = installing
};

StepRow makeStep(brls::Box* parent, const std::string& label, float barW) {
    StepRow r;
    r.barW = barW;

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setMarginBottom(6.0f);

    r.icon = new brls::Box();
    r.icon->setWidth(26.0f);
    r.icon->setHeight(26.0f);
    r.icon->setCornerRadius(13.0f);
    r.icon->setJustifyContent(brls::JustifyContent::CENTER);
    r.icon->setAlignItems(brls::AlignItems::CENTER);
    r.icon->setMarginRight(12.0f);
    r.glyph = makeLabel("\xE2\x9C\x93", 13.0f, tok::green());
    r.icon->addView(r.glyph);
    r.spinner = new brls::ProgressSpinner();
    r.spinner->setWidth(16.0f);
    r.spinner->setHeight(16.0f);
    r.icon->addView(r.spinner);
    row->addView(r.icon);

    r.text = makeLabel(label, 12.5f, tok::muted2());
    row->addView(r.text);
    parent->addView(row);

    // The bar sits under the text, aligned past the icon column.
    r.track = new brls::Box();
    r.track->setWidth(barW);
    r.track->setHeight(5.0f);
    r.track->setCornerRadius(2.5f);
    r.track->setBackgroundColor(tok::track());
    r.track->setMarginLeft(38.0f);
    r.track->setMarginBottom(8.0f);
    r.fill = new brls::Rectangle();
    r.fill->setWidth(2.0f);
    r.fill->setHeight(5.0f);
    r.fill->setCornerRadius(2.5f);
    r.fill->setColor(tok::gold());
    r.track->addView(r.fill);
    parent->addView(r.track);

    return r;
}

// Row state changes — UI thread only.
void stepPending(StepRow& r, const std::string& text) {
    r.icon->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    r.icon->setBorderColor(tok::track());
    r.icon->setBorderThickness(1.5f);
    r.glyph->setVisibility(brls::Visibility::GONE);
    r.spinner->setVisibility(brls::Visibility::GONE);
    r.spinner->animate(false);
    r.text->setText(text);
    r.text->setTextColor(tok::muted2());
    r.track->setVisibility(brls::Visibility::GONE);
}

void stepActive(StepRow& r, const std::string& text, float fraction) {
    r.icon->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    r.icon->setBorderColor(tok::goldTileBrd());
    r.icon->setBorderThickness(1.5f);
    r.glyph->setVisibility(brls::Visibility::GONE);
    r.spinner->setVisibility(brls::Visibility::VISIBLE);
    r.spinner->animate(true);
    r.text->setText(text);
    r.text->setTextColor(tok::goldBright());
    if (fraction >= 0.0f) {
        float w = r.barW * fraction;
        if (w < 2.0f) w = 2.0f;
        if (w > r.barW) w = r.barW;
        r.fill->setWidth(w);
        r.track->setVisibility(brls::Visibility::VISIBLE);
    } else {
        r.track->setVisibility(brls::Visibility::GONE);
    }
}

void stepDone(StepRow& r, const std::string& text) {
    r.icon->setBackgroundColor(tok::greenBg());
    r.icon->setBorderColor(tok::greenBrd());
    r.icon->setBorderThickness(1.5f);
    r.glyph->setVisibility(brls::Visibility::VISIBLE);
    r.spinner->setVisibility(brls::Visibility::GONE);
    r.spinner->animate(false);
    r.text->setText(text);
    r.text->setTextColor(tok::green());
    r.track->setVisibility(brls::Visibility::GONE);
}

void finishInstall(std::shared_ptr<ProgressUi> ui, std::function<void()> then) {
    brls::sync([ui, then]() {
        if (ui->dismissed->exchange(true)) then();
        else brls::Application::popActivity(brls::TransitionAnimation::FADE, then);
    });
}

#ifdef __PS4__
// Hand off to the updater helper by terminating THIS process immediately.
// brls::Application::quit() does an orderly shutdown, but VitaPlex keeps live
// HTTP threads (session/PIN polling, the update check) that don't stop on a
// dime, so the process can still be alive a few seconds later — exactly when
// the helper, after its short startup wait, uninstalls VPLX00002. Uninstalling
// a title whose process is still running is reported as a crash (CE-36329-3),
// which is what took VitaPlex down on the system-UI build. _Exit() ends the
// process at once — no destructors, no thread joins, no lingering title — so
// the helper's uninstall always lands on an already-gone VitaPlex. The update
// pkg is already on disk and settings were saved before we get here, so there
// is nothing left worth an orderly teardown.
[[noreturn]] void ps4HandoffExit() { std::_Exit(0); }
#endif

void installFailed(const std::string& msg, std::shared_ptr<ProgressUi> ui) {
    finishInstall(ui, [msg]() {
        auto* d = new brls::Dialog("Update failed:\n" + msg);
        d->addButton("OK", []() {});
        d->open();
    });
}

void startInstall(const ReleaseInfo rel) {
    s_cancel = false;

    const float screenW = platform::viewportWidth();
    float panelW = 388.0f;
    // Portrait phones: the viewport is design-width (narrow) and tall, so a
    // fixed-width panel reads as a skinny floating column — take the width.
    if (platform::isPortrait()) {
        // Near-full-width on a phone so it isn't a skinny column, but capped
        // so it doesn't stretch across a big foldable/tablet inner screen.
        float wide = screenW - 90.0f;
        float cap  = panelW + 180.0f;
        panelW = wide < cap ? wide : cap;
    }
    else if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;
    const float barW = panelW - 36.0f - 38.0f;   // panel padding + icon column

    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(tok::scrim());

    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(panelW);
    panel->setBackgroundColor(tok::panel());
    panel->setBorderColor(tok::panelLine());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(16.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    panel->setPadding(16.0f, 18.0f, 12.0f, 18.0f);

    panel->addView(makeLabel("Updating to " + rel.tag, 15.0f, tok::text()));
    auto* keepOpen = makeLabel("Keep VitaPlex open until this finishes", 11.0f, tok::disabled());
    keepOpen->setMarginTop(3.0f);
    keepOpen->setMarginBottom(14.0f);
    panel->addView(keepOpen);

#if defined(__PSV__)
    const char* relaunchLabel = "Reopens automatically";
#elif defined(__SWITCH__)
    const char* relaunchLabel = "Relaunch to apply";
#elif defined(__PS4__)
    const char* relaunchLabel = "Exit while the system installs";
#elif defined(_WIN32) || VITAPLEX_MACOS_DESKTOP
    const char* relaunchLabel = "Reopens automatically";
#else
    const char* relaunchLabel = "System installer opens";
#endif

    auto ui = std::make_shared<ProgressUi>();
    ui->download = makeStep(panel, "", barW);
    ui->install  = makeStep(panel, "Install", barW);
    ui->relaunch = makeStep(panel, relaunchLabel, barW);
    stepActive(ui->download, "Downloading\xE2\x80\xA6 0%", 0.0f);
    stepPending(ui->install, "Install");
    stepPending(ui->relaunch, relaunchLabel);

    // Cancel, bottom-right — download only: once the installer is touching
    // the bubble / executable, aborting could leave it half-written.
    auto cancelFn = [ui]() {
        if (ui->phase.load() != 0) return;
        if (ui->dismissed->exchange(true)) return;
        s_cancel = true;
        brls::Application::popActivity();
    };
    auto* footer = new brls::Box();
    footer->setAxis(brls::Axis::ROW);
    footer->setJustifyContent(brls::JustifyContent::FLEX_END);
    footer->setMarginTop(4.0f);
    ui->cancel = makeButton("Cancel", BtnStyle::Ghost, cancelFn);
    ui->cancel->setWidth(96.0f);
    footer->addView(ui->cancel);
    panel->addView(footer);

    scrim->addView(panel);
    // B cancels while cancelling is allowed; once installing it's swallowed
    // (the old dialog's setCancelable(false), kept).
    scrim->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
        [cancelFn](brls::View*) { cancelFn(); return true; });

    brls::Application::pushActivity(new OverlayActivity(scrim));
    brls::Application::giveFocus(ui->cancel);

    asyncRun([rel, ui]() {
#if defined(__SWITCH__)
        const std::string path = platformPath("update.nro");
#elif defined(ANDROID)
        const std::string path = platformPath("update.apk");
#elif defined(__PS4__)
        const std::string path = platformPath("update.pkg");
#elif defined(__linux__)
        std::string path;
        switch (linuxPkg()) {
            case LinuxPkg::AppImage: {
                // Download straight next to the running .AppImage so the final
                // step is an atomic rename over it (no cross-filesystem copy,
                // and rename doesn't hit ETXTBSY on the running executable).
                const char* ai = std::getenv("APPIMAGE");
                path = ai ? (std::string(ai) + ".new") : platformPath("update.AppImage");
                break;
            }
            case LinuxPkg::Deb:  path = platformPath("update.deb"); break;
            case LinuxPkg::Arch: path = platformPath("update.pkg.tar.zst"); break;
            default:             path = platformPath("update.bin"); break;
        }
#elif defined(_WIN32)
        // Straight into the install folder (absolute, so the detached cmd
        // resolves it no matter what its working directory ends up being).
        const std::string dir = winInstallDir();
        const std::string path = dir.empty() ? platformPath("update.zip")
                                             : (dir + "\\update.zip");
#elif VITAPLEX_MACOS_DESKTOP
        const std::string path = platformPath("update.dmg");
#else
        const std::string path = platformPath("update.vpk");
#endif

        HttpClient client;
        int64_t total = rel.assetSize;
        int64_t got = 0;
        bool ok = false;
        std::string dlErr;

        // One automatic retry: a fresh connection routinely clears the
        // transient failures (a dropped socket, a stale keep-alive).
        for (int attempt = 0; attempt < 2 && !s_cancel.load(); attempt++) {
            if (attempt > 0) {
                brls::Logger::warning("app_update: download failed ({}), retrying", dlErr);
                brls::sync([ui]() {
                    if (!ui->dismissed->load())
                        stepActive(ui->download, "Retrying\xE2\x80\xA6", -1.0f);
                });
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }

            // (Re)open truncating — a failed attempt leaves partial bytes.
#if !defined(__PSV__)
            FILE* f = fopen(path.c_str(), "wb");
            if (!f) { installFailed("cannot open " + path, ui); s_busy = false; return; }
#else
            SceUID f = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (f < 0) { installFailed("cannot open " + path, ui); s_busy = false; return; }
#endif

            got = 0;
            int lastPct = -1;
            dlErr.clear();
            ok = client.downloadFile(
                rel.assetUrl,
                [&](const char* data, size_t size) -> bool {
                    if (s_cancel.load()) return false;
#if !defined(__PSV__)
                    if (fwrite(data, 1, size, f) != size) return false;
#else
                    if (sceIoWrite(f, data, size) != (int)size) return false;
#endif
                    got += (int64_t)size;
                    if (total > 0) {
                        int pct = (int)(got * 100 / total);
                        if (pct != lastPct) {
                            lastPct = pct;
                            brls::sync([ui, pct]() {
                                if (ui->dismissed->load()) return;
                                stepActive(ui->download, "Downloading\xE2\x80\xA6 " +
                                           std::to_string(pct) + "%", (float)pct / 100.0f);
                            });
                        }
                    }
                    return true;
                },
                [&](int64_t sz) { if (total <= 0) total = sz; },
                {}, 0, nullptr, &dlErr);

#if !defined(__PSV__)
            fclose(f);
#else
            sceIoClose(f);
#endif

            if (ok && (rel.assetSize <= 0 || got == rel.assetSize)) break;
            ok = false;
        }

        if (s_cancel.load()) {
#if !defined(__PSV__)
            remove(path.c_str());
#else
            sceIoRemove(path.c_str());
#endif
            s_busy = false;
            return;   // user cancellation, not a failure
        }
        if (!ok) {
#if !defined(__PSV__)
            remove(path.c_str());
#else
            sceIoRemove(path.c_str());
#endif
            // Lead with the transport error — "0/39856545 bytes" hides the
            // actual reason (the Switch's applet-mode socket famine was
            // diagnosed by log only because the dialog didn't carry this).
            std::string why = dlErr.empty()
                ? "incomplete download (" + std::to_string(got) + "/" +
                      std::to_string(rel.assetSize) + " bytes)"
                : dlErr + " (" + std::to_string(got) + "/" +
                      std::to_string(rel.assetSize) + " bytes)";
            installFailed(why, ui);
            s_busy = false;
            return;
        }

        // Download done — from here the update is being applied, so Cancel
        // goes away and B stops working (phase guards both).
        ui->phase = 1;
        const int64_t gotBytes = got;
        brls::sync([ui, gotBytes]() {
            if (ui->dismissed->load()) return;
            stepDone(ui->download, "Downloaded \xC2\xB7 " + mbLabel(gotBytes) + " MB");
            // Disabled, not hidden: it keeps focus (nothing else here takes
            // it), and the phase guard already makes it inert.
            ui->cancel->setAlpha(0.45f);
#if defined(ANDROID) || defined(__PS4__)
            stepActive(ui->install, "Handing to system installer\xE2\x80\xA6", -1.0f);
#else
            stepActive(ui->install, "Installing\xE2\x80\xA6", -1.0f);
#endif
        });

#if defined(ANDROID)
        // Hand the APK to the system package installer via
        // PlatformUtils.installApk — the content:// route works on
        // Android TV too, where no browser exists to fall back on. The
        // JNI call runs on the main thread: SDL attaches that thread to
        // the VM for certain.
        finishInstall(ui, [path]() {
            JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
            if (!env) { brls::Logger::error("app_update: no JNIEnv"); return; }
            jclass utils = env->FindClass("org/libsdl/app/PlatformUtils");
            if (!utils) { brls::Logger::error("app_update: PlatformUtils missing"); return; }
            jmethodID mid = env->GetStaticMethodID(utils, "installApk", "(Ljava/lang/String;)V");
            if (mid) {
                jstring jpath = env->NewStringUTF(path.c_str());
                env->CallStaticVoidMethod(utils, mid, jpath);
                env->DeleteLocalRef(jpath);
            }
            env->DeleteLocalRef(utils);
        });
#elif defined(__PS4__)
        // A running PS4 title can't be replaced in place (BGFT installs into
        // the locked /user/app/<titleid>, and uninstalling the running title
        // kills this process), so hand off to the separate updater helper
        // (VPLX00003): with VitaPlex closed it uninstalls the old title and
        // installs the downloaded pkg via BGFT, which shows its own progress.
        // If the helper isn't installed, keep the pkg and guide a manual
        // install.
        {
            std::string err;
            // Check it's installed BEFORE launching: launching a title that
            // isn't there pops a system "Cannot start the application"
            // (CE-40841-7) dialog, which the user would see mid-update.
            if (ps4::isUpdaterInstalled() && ps4::launchUpdater(err) == 0) {
                brls::sync([ui]() {
                    if (!ui->dismissed->load()) stepDone(ui->install, "Handed to updater");
                });
                // Terminate IMMEDIATELY (see ps4HandoffExit), no confirmation
                // dialog: the helper waits a few seconds and then uninstalls
                // VPLX00002, so VitaPlex's process must be fully gone by then. An
                // orderly quit() leaves HTTP threads running, and uninstalling a
                // title whose process is still alive is killed as a crash
                // (CE-36329-3) — which is exactly what took VitaPlex down.
                finishInstall(ui, []() { ps4HandoffExit(); });
            } else {
                // The helper isn't installed (it removes itself after each run,
                // so this is the normal path). Its pkg ships inside VitaPlex's
                // own pkg — that's why the user only ever installs one app — so
                // install it now, wait for it to become launchable, and go.
                // It's a different title, so this isn't blocked by
                // "already installed".
                std::string setupErr;
                if (ps4::installUpdaterApp(setupErr) == 0) {
                    brls::sync([ui]() {
                        if (!ui->dismissed->load())
                            stepActive(ui->install, "Preparing updater", -1.0f);
                    });
                    // BGFT installs in the background. Wait by asking whether
                    // the title is installed yet — NOT by trying to launch it:
                    // launching a title that isn't ready pops a system "Cannot
                    // start the application" (CE-40841-7) dialog every attempt.
                    bool ready = false;
                    for (int i = 0; i < 60 && !ready; ++i) {
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        ready = ps4::isUpdaterInstalled();
                    }
                    // Let the install settle before the one and only launch.
                    bool launched = false;
                    if (ready) {
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                        std::string tryErr;
                        launched = ps4::launchUpdater(tryErr) == 0;
                    }
                    if (launched) {
                        brls::sync([ui]() {
                            if (!ui->dismissed->load()) stepDone(ui->install, "Handed to updater");
                        });
                        // Terminate immediately for a clean hand-off, same as
                        // the direct-launch path above (see ps4HandoffExit).
                        finishInstall(ui, []() { ps4HandoffExit(); });
                    } else {
                        brls::sync([ui]() {
                            if (!ui->dismissed->load()) stepDone(ui->install, "Downloaded");
                        });
                        finishInstall(ui, [path]() {
                            auto* d = new brls::Dialog(
                                "Update downloaded to\n" + path + "\n\nThe updater "
                                "is still installing. Try Update again in a moment, "
                                "or close VitaPlex and install that .pkg with "
                                "Itemzflow (or Debug Settings > Install Package).");
                            d->addButton("OK", []() {});
                            d->open();
                        });
                    }
                } else {
                    // Couldn't set the helper up either: the pkg is downloaded,
                    // so keep it and point the user at a PKG installer.
                    brls::sync([ui]() {
                        if (!ui->dismissed->load()) stepDone(ui->install, "Downloaded");
                    });
                    finishInstall(ui, [path, setupErr]() {
                        auto* d = new brls::Dialog(
                            "Update downloaded to\n" + path + "\n\nThe in-app "
                            "installer couldn't start (" + setupErr + "). Close "
                            "VitaPlex and install that .pkg with Itemzflow (or "
                            "Debug Settings > Install Package) to finish.");
                        d->addButton("OK", []() {});
                        d->open();
                    });
                }
            }
            s_busy = false;
            return;
        }
#elif defined(__SWITCH__)
        // The romfs is mapped from the running NRO: unmount before
        // replacing the file, exactly as pleNx does.
        romfsExit();
        std::string target = s_selfPath;
        if (target.size() < 4 || target.compare(target.size() - 4, 4, ".nro") != 0)
            target = platformPath("VitaPlex.nro");
        std::error_code ec;
        std::filesystem::remove(target, ec);
        std::filesystem::rename(path, target, ec);
        if (ec) {
            installFailed("could not replace " + target + ": " + ec.message(), ui);
            s_busy = false;
            return;
        }
        brls::sync([ui]() {
            if (!ui->dismissed->load()) stepDone(ui->install, "Installed");
        });
        finishInstall(ui, []() {
            auto* d = new brls::Dialog("Update installed. VitaPlex will now close - relaunch to use the new version.");
            d->addButton("OK", []() { brls::Application::quit(); });
            d->open();
        });
#elif defined(__linux__)
        // Desktop Linux. AppImage self-replaces and relaunches; deb/Arch hand
        // the downloaded package to the system installer (the user confirms the
        // install there). Flatpak/unknown never reach here — their
        // assetSuffix() is empty, so the flow opens the browser instead.
        if (linuxPkg() == LinuxPkg::AppImage) {
            const char* aiEnv = std::getenv("APPIMAGE");
            std::string target = aiEnv ? std::string(aiEnv) : selfExePath();
            brls::sync([ui]() {
                if (!ui->dismissed->load()) stepDone(ui->install, "Installed");
            });
            // `path` was downloaded to "<target>.new" on the same filesystem, so
            // rename() atomically swaps it over the running image — the running
            // process keeps the old inode, and the new file launches on exec.
            finishInstall(ui, [path, target]() {
                std::error_code ec;
                std::filesystem::rename(path, target, ec);
                if (ec)
                    std::filesystem::copy_file(path, target,
                        std::filesystem::copy_options::overwrite_existing, ec);
                ::chmod(target.c_str(), 0755);
                pid_t pid = fork();   // relaunch the new image, detached
                if (pid == 0) {
                    setsid();
                    sleep(1);   // let this instance quit and free the window first
                    execl(target.c_str(), target.c_str(), (char*)nullptr);
                    _exit(127);
                }
                brls::Application::quit();
            });
        } else {
            brls::sync([ui]() {
                if (!ui->dismissed->load()) stepDone(ui->install, "Handed to installer");
            });
            finishInstall(ui, [path]() {
                // Hand the .deb / .pkg.tar.zst to the desktop's package
                // installer, then quit automatically — no confirmation button.
                // The installer is fork+setsid'd (its own session), so it keeps
                // running after we exit; and we MUST exit, because dpkg/pacman
                // can't swap the binary under a live process (the old build
                // would just keep running). AppImage self-relaunches; deb/Arch
                // can't, so the user reopens VitaPlex once the system install
                // finishes.
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
                    _exit(127);
                }
                brls::Application::quit();
            });
        }
#elif defined(_WIN32)
        // Windows portable build. The running VitaPlex.exe and its loaded
        // DLLs are locked, so the swap waits until we exit: write a .bat that
        // spins until VitaPlex.exe is gone, unpacks the downloaded zip over
        // the install folder (bsdtar `tar`, bundled since Win10 1803, with
        // PowerShell Expand-Archive as a fallback) and relaunches — then hand
        // it to a detached, windowless cmd and quit.
        {
            const std::string installDir = winInstallDir();
            if (installDir.empty()) {
                installFailed("could not locate the install folder", ui);
                s_busy = false;
                return;
            }
            auto toWin = [](std::string s) {
                for (char& c : s) if (c == '/') c = '\\';
                return s;
            };
            const std::string zipWin = toWin(path);
            const std::string dirWin = toWin(installDir);
            const std::string batPath = installDir + "\\vitaplex_update.bat";

            FILE* bf = fopen(batPath.c_str(), "wb");
            if (!bf) {
                installFailed("could not write the updater script", ui);
                s_busy = false;
                return;
            }
            // CRLF endings: cmd mishandles bare-LF .bat files.
            fprintf(bf,
                "@echo off\r\n"
                ":waitloop\r\n"
                "tasklist /FI \"IMAGENAME eq VitaPlex.exe\" 2>nul | find /I \"VitaPlex.exe\" >nul\r\n"
                "if not errorlevel 1 (\r\n"
                "  ping -n 2 127.0.0.1 >nul\r\n"
                "  goto waitloop\r\n"
                ")\r\n"
                "where tar >nul 2>&1\r\n"
                "if not errorlevel 1 (\r\n"
                "  tar -xf \"%s\" -C \"%s\"\r\n"
                ") else (\r\n"
                "  powershell -NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"\r\n"
                ")\r\n"
                "start \"\" /D \"%s\" \"%s\\VitaPlex.exe\"\r\n"
                "del \"%s\" >nul 2>&1\r\n"
                "del \"%%~f0\" >nul 2>&1\r\n",
                zipWin.c_str(), dirWin.c_str(),
                zipWin.c_str(), dirWin.c_str(),
                dirWin.c_str(), dirWin.c_str(),
                zipWin.c_str());
            fclose(bf);

            brls::sync([ui]() {
                if (!ui->dismissed->load()) stepDone(ui->install, "Installed");
            });
            finishInstall(ui, [batPath]() {
                std::string cmd = "cmd.exe /c \"" + batPath + "\"";
                std::vector<char> mut(cmd.begin(), cmd.end());
                mut.push_back('\0');
                STARTUPINFOA si;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi;
                ZeroMemory(&pi, sizeof(pi));
                // Windowless; a CreateProcess child outlives us with no job to
                // tie it down, so it's free to do the swap once we're gone.
                if (::CreateProcessA(nullptr, mut.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    ::CloseHandle(pi.hThread);
                    ::CloseHandle(pi.hProcess);
                }
                brls::Application::quit();
            });
            s_busy = false;
            return;
        }
#elif VITAPLEX_MACOS_DESKTOP
        // macOS .app swap, run from a detached shell after VitaPlex quits:
        // wait for the process to die, mount the dmg, ditto its VitaPlex.app
        // over ours, detach and reopen. The app is unsigned and the dmg came
        // from our own HTTP client (no quarantine xattr), so no Gatekeeper
        // prompt is involved.
        {
            const std::string bundle = macAppBundlePath();
            if (bundle.empty()) {
                // assetSuffix() gates on this, so we normally never land here;
                // never clobber a loose binary if we somehow do.
                brls::sync([ui]() {
                    if (!ui->dismissed->load()) stepDone(ui->install, "Downloaded");
                });
                finishInstall(ui, [path]() {
                    auto* d = new brls::Dialog(
                        "Update downloaded to\n" + path + "\n\nOpen it to install.");
                    d->addButton("OK", []() {});
                    d->open();
                });
                s_busy = false;
                return;
            }
            const std::string scriptPath = platformPath("update_apply.sh");
            FILE* sf = fopen(scriptPath.c_str(), "wb");
            if (!sf) {
                installFailed("could not write the updater script", ui);
                s_busy = false;
                return;
            }
            // VitaPlex data paths carry no single quotes, so single-quoting
            // each is enough to keep the shell happy.
            fprintf(sf,
                "#!/bin/sh\n"
                "PID=%d\n"
                "DMG='%s'\n"
                "APP='%s'\n"
                "while kill -0 \"$PID\" 2>/dev/null; do sleep 1; done\n"
                "MNT=\"$(mktemp -d /tmp/vitaplex_dmg.XXXXXX)\"\n"
                "if hdiutil attach \"$DMG\" -nobrowse -readonly -mountpoint \"$MNT\" >/dev/null 2>&1; then\n"
                "  if [ -d \"$MNT/VitaPlex.app\" ]; then\n"
                "    rm -rf \"$APP.old\"\n"
                "    if mv \"$APP\" \"$APP.old\" 2>/dev/null; then\n"
                "      if ditto \"$MNT/VitaPlex.app\" \"$APP\"; then\n"
                "        rm -rf \"$APP.old\"\n"
                "      else\n"
                "        rm -rf \"$APP\"; mv \"$APP.old\" \"$APP\" 2>/dev/null\n"
                "      fi\n"
                "    fi\n"
                "  fi\n"
                "  hdiutil detach \"$MNT\" >/dev/null 2>&1 || hdiutil detach \"$MNT\" -force >/dev/null 2>&1\n"
                "fi\n"
                "rmdir \"$MNT\" 2>/dev/null\n"
                "rm -f \"$DMG\"\n"
                "open \"$APP\"\n"
                "rm -f \"$0\"\n",
                (int)getpid(), path.c_str(), bundle.c_str());
            fclose(sf);
            ::chmod(scriptPath.c_str(), 0755);

            brls::sync([ui]() {
                if (!ui->dismissed->load()) stepDone(ui->install, "Installed");
            });
            finishInstall(ui, [scriptPath]() {
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    execl("/bin/sh", "sh", scriptPath.c_str(), (char*)nullptr);
                    _exit(127);
                }
                brls::Application::quit();
            });
            s_busy = false;
            return;
        }
#else
        // VitaPlex can't promote over itself while it is the running title
        // (the installer returns 0x80101114 "in use"), so hand the install
        // to a tiny bundled stub app — the AutoPlugin2 technique. VitaPlex
        // installs the stub (the stub isn't running, so promoting IT is
        // fine), launches it, and quits; with VitaPlex closed the stub
        // promotes the downloaded update.vpk and relaunches VitaPlex.
        brls::sync([ui]() {
            if (!ui->dismissed->load())
                stepActive(ui->install, "Preparing installer\xE2\x80\xA6", -1.0f);
        });
        // Hand the target version to the stub's progress screen (best-effort;
        // the stub omits the version clause if this file is absent).
        {
            std::string vpath = platformPath("update_version.txt");
            SceUID vf = sceIoOpen(vpath.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (vf >= 0) { sceIoWrite(vf, rel.tag.c_str(), rel.tag.size()); sceIoClose(vf); }
        }
        std::string err;
        int rc = vita::installVpk("app0:updater.vpk", platformPath("updater_stage"), err);
        if (rc != 0) {
            // Couldn't stage the stub — fall back to the manual VitaShell
            // path so the download isn't wasted. update.vpk is kept.
            brls::sync([ui]() {
                if (!ui->dismissed->load()) stepDone(ui->install, "Downloaded");
            });
            finishInstall(ui, [path]() {
                auto* d = new brls::Dialog(
                    "Update downloaded.\n\nThe in-app installer couldn't start, so "
                    "open VitaShell and install this file to finish:\n\n" + path);
                d->addButton("OK", []() {});
                d->open();
            });
            s_busy = false;
            return;
        }
        // Stub installed. Launch it and quit automatically — no OK dialog, so
        // it closes hands-off like the desktop paths. The progress overlay's
        // "Installing update…" relaunch step already shows what's happening; the
        // stub promotes the VPK and reopens VitaPlex. (The Vita OS still shows
        // its own "application will close" confirmation when one title launches
        // another — once here, once when the stub relaunches us — that's a
        // system dialog, not ours.)
        brls::sync([ui]() {
            if (!ui->dismissed->load()) stepActive(ui->relaunch, "Installing update\xE2\x80\xA6", -1.0f);
        });
        finishInstall(ui, []() {
            vita::launchTitle("VPLXUPD01");
            brls::Application::quit();
        });
#endif
        s_busy = false;
    });
}
#endif  // __SWITCH__ || __PSV__ || ANDROID || __PS4__ || (linux && !ANDROID) || _WIN32 || macOS

// ── Release notes → sheet lines (design_handoff_notes_A) ─────────────────
// VitaPlex notes are hand-written markdown with a fixed shape: an H1
// title, **Date:**/**Status:**/**PRs:** meta lines, a blockquote
// screenshots note, then `---`-separated `## Section`s of
// `- **Lead** — description` bullets with optional intro paragraphs.

struct NoteLine {
    enum Kind { Section, Bullet, Para } kind;
    std::string lead;   // bullets only: the bold lead, may be empty
    std::string text;
};

struct ParsedNotes {
    std::string date;   // "June 2026" from the meta lines
    std::string prs;    // "#317–#346"
    std::vector<NoteLine> lines;
    int sections = 0;
};

// Collapse [text](url) → text and strip stray **/` pairs.
std::string cleanInline(const std::string& in) {
    std::string s = in;
    for (size_t i = 0; (i = s.find('[', i)) != std::string::npos;) {
        size_t close = s.find(']', i);
        if (close == std::string::npos) break;
        if (close + 1 < s.size() && s[close + 1] == '(') {
            size_t end = s.find(')', close);
            if (end != std::string::npos) {
                s = s.substr(0, i) + s.substr(i + 1, close - i - 1) + s.substr(end + 1);
                continue;
            }
        }
        i = close + 1;
    }
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '`') continue;
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') { i++; continue; }
        out += s[i];
    }
    return out;
}

ParsedNotes parseNotes(const std::string& md) {
    ParsedNotes out;
    size_t pos = 0;
    while (pos <= md.size()) {
        size_t eol = md.find('\n', pos);
        std::string line = md.substr(pos, eol == std::string::npos ? std::string::npos
                                                                   : eol - pos);
        pos = (eol == std::string::npos) ? md.size() + 1 : eol + 1;

        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        size_t at = line.find_first_not_of(' ');
        if (at == std::string::npos) continue;
        line = line.substr(at);

        // Meta lines fold into the sheet's header caption; the Status line
        // is skipped — the Pre-release chip comes from the release JSON.
        if (line.compare(0, 9, "**Date:**") == 0) {
            out.date = cleanInline(line.substr(9));
            size_t a = out.date.find_first_not_of(' ');
            if (a != std::string::npos) out.date = out.date.substr(a);
            continue;
        }
        if (line.compare(0, 8, "**PRs:**") == 0) {
            out.prs = cleanInline(line.substr(8));
            size_t a = out.prs.find_first_not_of(' ');
            if (a != std::string::npos) out.prs = out.prs.substr(a);
            continue;
        }
        if (line.compare(0, 11, "**Status:**") == 0) continue;

        // The H1 repeats the tag; blockquotes link to the repo (useless
        // without a browser); rules just separate sections.
        if (line.compare(0, 3, "## ") == 0) {
            out.lines.push_back({NoteLine::Section, "", cleanInline(line.substr(3))});
            out.sections++;
            continue;
        }
        if (line[0] == '#' || line[0] == '>' || line.compare(0, 3, "---") == 0) continue;

        if (line.compare(0, 2, "- ") == 0 || line.compare(0, 2, "* ") == 0) {
            std::string body = line.substr(2);
            NoteLine n{NoteLine::Bullet, "", ""};
            if (body.compare(0, 2, "**") == 0) {
                size_t close = body.find("**", 2);
                if (close != std::string::npos) {
                    n.lead = cleanInline(body.substr(2, close - 2));
                    std::string rest = body.substr(close + 2);
                    // The lead line stands alone, so the " — " joiner would
                    // dangle at the start of the description.
                    size_t r = 0;
                    while (r < rest.size() &&
                           (rest[r] == ' ' || rest[r] == '-' ||
                            rest.compare(r, 3, "\xE2\x80\x94") == 0)) {
                        r += (rest[r] == ' ' || rest[r] == '-') ? 1 : 3;
                    }
                    n.text = cleanInline(rest.substr(r));
                }
            }
            if (n.lead.empty()) n.text = cleanInline(body);
            out.lines.push_back(std::move(n));
            continue;
        }

        out.lines.push_back({NoteLine::Para, "", cleanInline(line)});
    }
    return out;
}

// ── The What's New sheet (design_handoff_notes_A) ────────────────────────
// Pushed on top of the offer dialog; B returns to it. Header carries tag,
// date, size and PR range plus a Pre-release chip; the notes scroll with
// up/down while focus stays on the footer buttons; Update Now acts
// without going back to the offer.
void showNotesSheet(const ReleaseInfo rel) {
    const ParsedNotes notes = parseNotes(rel.notes);

    // Wider than the mock and a typography step up throughout: the sheet
    // is a reading surface on a TV across the room, not a phone in hand.
    const float screenW = platform::viewportWidth();
    const float screenH = platform::viewportHeight();
    float panelW = 620.0f;
    // Portrait phones: near full width (see startInstall).
    if (platform::isPortrait()) {
        // Near-full-width on a phone so it isn't a skinny column, but capped
        // so it doesn't stretch across a big foldable/tablet inner screen.
        float wide = screenW - 90.0f;
        float cap  = panelW + 180.0f;
        panelW = wide < cap ? wide : cap;
    }
    else if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;
    const float panelH = screenH - 76.0f;

    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(tok::scrim());

    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(panelW);
    panel->setHeight(panelH);
    panel->setBackgroundColor(tok::panel());
    panel->setBorderColor(tok::panelLine());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(16.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    panel->setClipsToBounds(true);

    // ── Header ──────────────────────────────────────────────────────────
    auto* header = new brls::Box();
    header->setAxis(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setPadding(14.0f, 20.0f, 14.0f, 20.0f);

    auto* tile = new brls::Box();
    tile->setWidth(38.0f);
    tile->setHeight(38.0f);
    tile->setCornerRadius(10.0f);
    tile->setBackgroundColor(tok::goldTileBg());
    tile->setBorderColor(tok::goldTileBrd());
    tile->setBorderThickness(1.0f);
    tile->setJustifyContent(brls::JustifyContent::CENTER);
    tile->setAlignItems(brls::AlignItems::CENTER);
    tile->addView(makeLabel("\xE2\x89\xA1", 18.0f, tok::gold()));
    tile->setMarginRight(12.0f);
    header->addView(tile);

    auto* titles = new brls::Box();
    titles->setAxis(brls::Axis::COLUMN);
    titles->setShrink(1.0f);
    titles->addView(makeLabel(rel.tag, 16.5f, tok::text()));
    {
        std::string caption = notes.date;
        if (caption.empty() && rel.publishedAt.size() >= 10)
            caption = rel.publishedAt.substr(0, 10);
        if (rel.assetSize > 0)
            caption += (caption.empty() ? "" : " \xC2\xB7 ") + mbLabel(rel.assetSize) + " MB";
        if (!notes.prs.empty())
            caption += (caption.empty() ? "" : " \xC2\xB7 ") + std::string("PRs ") + notes.prs;
        if (!caption.empty()) {
            auto* cap = makeLabel(caption, 12.0f, tok::disabled());
            cap->setMarginTop(2.0f);
            titles->addView(cap);
        }
    }
    header->addView(titles);

    auto* hspacer = new brls::Box();
    hspacer->setGrow(1.0f);
    header->addView(hspacer);

    if (rel.prerelease) {
        auto* chip = new brls::Box();
        chip->setAxis(brls::Axis::ROW);
        chip->setAlignItems(brls::AlignItems::CENTER);
        chip->setHeight(24.0f);
        chip->setPadding(0.0f, 11.0f, 0.0f, 11.0f);
        chip->setCornerRadius(12.0f);
        chip->setBackgroundColor(tok::goldTileBg());
        chip->setBorderColor(tok::goldTileBrd());
        chip->setBorderThickness(1.0f);
        chip->addView(makeLabel("Pre-release", 10.5f, tok::goldBright()));
        chip->setMarginLeft(10.0f);
        header->addView(chip);
    }
    panel->addView(header);

    auto* headerRule = new brls::Box();
    headerRule->setHeight(1.0f);
    headerRule->setAlignSelf(brls::AlignSelf::STRETCH);
    headerRule->setBackgroundColor(tok::hairline());
    panel->addView(headerRule);

    // ── Notes area ──────────────────────────────────────────────────────
    auto* scroller = new brls::ScrollingFrame();
    scroller->setGrow(1.0f);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(16.0f, 26.0f, 18.0f, 22.0f);

    if (notes.lines.empty()) {
        auto* empty = makeLabel("No notes for this release.", 13.5f, tok::muted2());
        empty->setMarginTop(40.0f);
        empty->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        content->addView(empty);
    }

    bool first = true;
    for (const NoteLine& n : notes.lines) {
        if (n.kind == NoteLine::Section) {
            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setMarginTop(first ? 2.0f : 20.0f);
            row->setMarginBottom(3.0f);
            auto* tick = new brls::Rectangle();
            tick->setWidth(4.0f);
            tick->setHeight(16.0f);
            tick->setCornerRadius(2.0f);
            tick->setColor(tok::gold());
            tick->setMarginRight(9.0f);
            row->addView(tick);
            row->addView(makeLabel(n.text, 15.0f, nvgRGB(0xea, 0xea, 0xee)));
            content->addView(row);
        } else if (n.kind == NoteLine::Bullet) {
            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::FLEX_START);
            row->setMarginTop(10.0f);
            auto* dot = new brls::Rectangle();
            dot->setWidth(5.0f);
            dot->setHeight(5.0f);
            dot->setCornerRadius(2.5f);
            dot->setColor(tok::muted2());
            dot->setMarginTop(7.0f);
            dot->setMarginRight(10.0f);
            row->addView(dot);
            auto* col = new brls::Box();
            col->setAxis(brls::Axis::COLUMN);
            col->setShrink(1.0f);
            // A borealis label is single-colour, so the bold lead gets its
            // own line and the description wraps below it.
            if (!n.lead.empty())
                col->addView(makeLabel(n.lead, 13.5f, nvgRGB(0xe7, 0xe7, 0xea)));
            if (!n.text.empty()) {
                auto* t = makeLabel(n.text, 12.5f, tok::muted(), false);
                t->setLineHeight(1.35f);
                if (!n.lead.empty()) t->setMarginTop(2.0f);
                col->addView(t);
            }
            row->addView(col);
            content->addView(row);
        } else {
            auto* p = makeLabel(n.text, 12.5f, tok::muted(), false);
            p->setLineHeight(1.35f);
            p->setMarginTop(8.0f);
            content->addView(p);
        }
        first = false;
    }
    scroller->setContentView(content);
    panel->addView(scroller);

    auto* footerRule = new brls::Box();
    footerRule->setHeight(1.0f);
    footerRule->setAlignSelf(brls::AlignSelf::STRETCH);
    footerRule->setBackgroundColor(tok::hairline());
    panel->addView(footerRule);

    // ── Footer ──────────────────────────────────────────────────────────
    auto* footer = new brls::Box();
    footer->setAxis(brls::Axis::ROW);
    footer->setAlignItems(brls::AlignItems::CENTER);
    footer->setPadding(12.0f, 20.0f, 13.0f, 20.0f);

    if (notes.sections > 0) {
        footer->addView(makeLabel(
            "Scroll for more \xC2\xB7 " + std::to_string(notes.sections) +
            (notes.sections == 1 ? " section" : " sections"),
            11.5f, tok::disabled()));
    }
    auto* fspacer = new brls::Box();
    fspacer->setGrow(1.0f);
    footer->addView(fspacer);

    // The primary mirrors the offer's action so the user can act from
    // here without going back.
    brls::Box* primary = nullptr;
#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID) || defined(__PS4__) || (defined(__linux__) && !defined(ANDROID)) || defined(_WIN32) || VITAPLEX_MACOS_DESKTOP
    if (!rel.assetUrl.empty()) {
        primary = makeButton("\xE2\x86\x93  Update Now", BtnStyle::Gold, [rel]() {
            // Pop the sheet, then the offer beneath it, then install.
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [rel]() {
                brls::Application::popActivity(brls::TransitionAnimation::FADE,
                                               [rel]() { startInstall(rel); });
            });
        });
    } else {
        primary = makeButton("Open release page", BtnStyle::Gold, [rel]() {
            brls::Application::getPlatform()->openBrowser(rel.pageUrl);
            s_busy = false;
            brls::Application::popActivity(brls::TransitionAnimation::NONE,
                []() { brls::Application::popActivity(); });
        });
    }
#else
    {
        std::string url = !rel.assetUrl.empty() ? rel.assetUrl : rel.pageUrl;
        primary = makeButton("Download", BtnStyle::Gold, [url]() {
            brls::Application::getPlatform()->openBrowser(url);
            s_busy = false;
            brls::Application::popActivity(brls::TransitionAnimation::NONE,
                []() { brls::Application::popActivity(); });
        });
    }
#endif
    primary->setWidth(170.0f);
    footer->addView(primary);

    auto* back = makeButton("Back", BtnStyle::Ghost, []() {
        brls::Application::popActivity();
    });
    back->setWidth(84.0f);
    back->setMarginLeft(8.0f);
    footer->addView(back);
    panel->addView(footer);

    scrim->addView(panel);

    // Up/down scrolls the notes directly — focus stays on the footer
    // buttons (per the handoff: no row selection, no scrollIntoView).
    auto scrollBy = [scroller, content](float delta) {
        float maxY = content->getHeight() - scroller->getHeight();
        if (maxY < 0.0f) maxY = 0.0f;
        float y = scroller->getContentOffsetY() + delta;
        if (y < 0.0f) y = 0.0f;
        if (y > maxY) y = maxY;
        scroller->setContentOffsetY(y, true);
    };
    scrim->registerAction("Scroll up", brls::ControllerButton::BUTTON_UP,
        [scrollBy](brls::View*) { scrollBy(-72.0f); return true; }, true);
    scrim->registerAction("Scroll down", brls::ControllerButton::BUTTON_DOWN,
        [scrollBy](brls::View*) { scrollBy(72.0f); return true; }, true);
    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) { brls::Application::popActivity(); return true; });
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim,
        []() { brls::Application::popActivity(); }));

    brls::Application::pushActivity(new OverlayActivity(scrim));
    brls::Application::giveFocus(primary);
}

// The offer dialog (design_handoff_update, dialog B): gold strip, icon
// tile, current → new version cards, size/platform caption, then the
// per-platform actions. Release notes render in-app — the What's New
// button opens the notes sheet above.
void offerUpdate(const ReleaseInfo rel) {
    const float screenW = platform::viewportWidth();
    float panelW = 428.0f;
    // Portrait phones: near full width (see startInstall).
    if (platform::isPortrait()) {
        // Near-full-width on a phone so it isn't a skinny column, but capped
        // so it doesn't stretch across a big foldable/tablet inner screen.
        float wide = screenW - 90.0f;
        float cap  = panelW + 180.0f;
        panelW = wide < cap ? wide : cap;
    }
    else if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;

    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(tok::scrim());

    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(panelW);
    panel->setBackgroundColor(tok::panel());
    panel->setBorderColor(tok::panelLine());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(16.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    // The gold strip runs flush along the top edge; the panel's rounded
    // corners clip its ends.
    panel->setClipsToBounds(true);

    auto* strip = new brls::Box();
    strip->setHeight(5.0f);
    strip->setAlignSelf(brls::AlignSelf::STRETCH);
    strip->setBackgroundColor(tok::gold());
    panel->addView(strip);

    // ── Header: icon tile + titles ──────────────────────────────────────
    auto* header = new brls::Box();
    header->setAxis(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setPadding(16.0f, 18.0f, 12.0f, 18.0f);

    auto* tile = new brls::Box();
    tile->setWidth(52.0f);
    tile->setHeight(52.0f);
    tile->setCornerRadius(14.0f);
    tile->setBackgroundColor(tok::goldTileBg());
    tile->setBorderColor(tok::goldTileBrd());
    tile->setBorderThickness(1.0f);
    tile->setJustifyContent(brls::JustifyContent::CENTER);
    tile->setAlignItems(brls::AlignItems::CENTER);
    tile->addView(makeLabel("\xE2\x86\x93", 22.0f, tok::gold()));
    tile->setMarginRight(14.0f);
    header->addView(tile);

    auto* titles = new brls::Box();
    titles->setAxis(brls::Axis::COLUMN);
    titles->setShrink(1.0f);
    titles->addView(makeLabel("Update available", 16.0f, tok::text()));
    auto* sub = makeLabel("A new version of VitaPlex is ready to install.", 11.5f, tok::muted());
    sub->setMarginTop(3.0f);
    titles->addView(sub);
    header->addView(titles);
    panel->addView(header);

    // ── Version cards: current → new ────────────────────────────────────
    const float cardW = (panelW - 36.0f - 34.0f) / 2.0f;
    auto makeCard = [cardW](const char* tag, NVGcolor tagColor, const std::string& value,
                            NVGcolor valueColor, NVGcolor bg, NVGcolor border) {
        auto* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setWidth(cardW);
        card->setCornerRadius(10.0f);
        card->setBackgroundColor(bg);
        card->setBorderColor(border);
        card->setBorderThickness(1.0f);
        card->setPadding(9.0f, 12.0f, 9.0f, 12.0f);
        card->addView(makeLabel(tag, 9.5f, tagColor));
        auto* v = makeLabel(value, 13.0f, valueColor);
        v->setMarginTop(2.0f);
        card->addView(v);
        return card;
    };

    auto* cards = new brls::Box();
    cards->setAxis(brls::Axis::ROW);
    cards->setAlignItems(brls::AlignItems::CENTER);
    cards->setPadding(0.0f, 18.0f, 0.0f, 18.0f);
    cards->addView(makeCard("CURRENT", tok::muted2(), VITA_PLEX_DISPLAY_VERSION,
                            tok::muted(), tok::inputBg(), tok::hairline()));
    auto* arrow = makeLabel("\xE2\x86\x92", 14.0f, tok::muted2());
    arrow->setMarginLeft(10.0f);
    arrow->setMarginRight(10.0f);
    cards->addView(arrow);
    cards->addView(makeCard("NEW", tok::gold(), rel.tag,
                            tok::goldBright(), tok::goldCardBg(), tok::goldCardBrd()));
    panel->addView(cards);

    // ── Size / platform caption ─────────────────────────────────────────
    std::string caption;
    if (rel.assetSize > 0) caption = mbLabel(rel.assetSize) + " MB download";
#if defined(__PSV__)
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("installs in place \xC2\xB7 reopens automatically");
#elif defined(__SWITCH__)
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("installs in place \xC2\xB7 relaunch to apply");
#elif defined(ANDROID)
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("system installer opens when ready");
#elif defined(__PS4__)
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("installs via the PS4 installer \xC2\xB7 exit to finish");
#elif defined(_WIN32) || VITAPLEX_MACOS_DESKTOP
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("installs in place \xC2\xB7 reopens automatically");
#else
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("opens the release page in your browser");
#endif
    auto* cap = makeLabel(caption, 10.5f, tok::disabled(), false);
    cap->setMarginTop(10.0f);
    cap->setMarginLeft(18.0f);
    cap->setMarginRight(18.0f);
    panel->addView(cap);

    // ── Actions ─────────────────────────────────────────────────────────
    // Closing without Later never marks the release skipped — B here means
    // "not right now", Later means "stop offering this one at startup".
    auto dismiss = []() {
        s_busy = false;
        brls::Application::popActivity();
    };
    auto skip = [rel]() {
        AppSettings& s = Application::getInstance().getSettings();
        s.skippedUpdateVersion = rel.tag;
        Application::getInstance().saveSettings();
        s_busy = false;
        brls::Application::popActivity();
    };

    auto* buttons = new brls::Box();
    buttons->setAxis(brls::Axis::ROW);
    buttons->setAlignItems(brls::AlignItems::CENTER);
    buttons->setPadding(14.0f, 18.0f, 16.0f, 18.0f);

    brls::Box* primary = nullptr;
#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID) || defined(__PS4__) || (defined(__linux__) && !defined(ANDROID)) || defined(_WIN32) || VITAPLEX_MACOS_DESKTOP
    // These install in place; the notes sheet is the secondary action.
    if (!rel.assetUrl.empty()) {
        primary = makeButton("\xE2\x86\x93  Update", BtnStyle::Gold, [rel]() {
            brls::Application::popActivity(brls::TransitionAnimation::FADE,
                                           [rel]() { startInstall(rel); });
        });
        primary->setGrow(1.0f);
        buttons->addView(primary);

        auto* whatsNew = makeButton("What's New", BtnStyle::Gray,
                                    [rel]() { showNotesSheet(rel); });
        whatsNew->setWidth(128.0f);
        whatsNew->setMarginLeft(8.0f);
        buttons->addView(whatsNew);
    } else {
        // The release carries no asset for this platform/flavour — the
        // notes (whose sheet links out to the page) are all there is.
        primary = makeButton("What's New", BtnStyle::Gold,
                             [rel]() { showNotesSheet(rel); });
        primary->setGrow(1.0f);
        buttons->addView(primary);
    }
#else
    // Desktop: the browser handles download + install, so the primary IS
    // the release page (the asset directly when one matched).
    {
        std::string url = !rel.assetUrl.empty() ? rel.assetUrl : rel.pageUrl;
        primary = makeButton("Download", BtnStyle::Gold, [url, dismiss]() {
            brls::Application::getPlatform()->openBrowser(url);
            dismiss();
        });
        primary->setGrow(1.0f);
        buttons->addView(primary);

        auto* whatsNew = makeButton("What's New", BtnStyle::Gray,
                                    [rel]() { showNotesSheet(rel); });
        whatsNew->setWidth(128.0f);
        whatsNew->setMarginLeft(8.0f);
        buttons->addView(whatsNew);
    }
#endif

    auto* later = makeButton("Later", BtnStyle::Ghost, skip);
    later->setWidth(84.0f);
    later->setMarginLeft(8.0f);
    buttons->addView(later);
    panel->addView(buttons);

    scrim->addView(panel);
    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [dismiss](brls::View*) { dismiss(); return true; });
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim, dismiss));

    brls::Application::pushActivity(new OverlayActivity(scrim));
    brls::Application::giveFocus(primary);
}

}  // namespace

void setSelfPath(const char* argv0) {
    if (argv0 && argv0[0]) s_selfPath = argv0;
}

void checkForUpdates(bool manual) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;

    asyncRun([manual]() {
        // At startup, give login and the first content fetches priority.
        if (!manual) std::this_thread::sleep_for(std::chrono::seconds(3));

#ifdef __PS4__
        // Tidy up the updater helper left over from a completed update. It is
        // only needed while an update installs, and VitaPlex reinstalls it on
        // demand from the copy bundled in its own pkg. Doing it from HERE, and
        // not from the helper itself, is deliberate: a title that uninstalls
        // itself is killed mid-call and the system reports it as a crash
        // (CE-36329-3). From VitaPlex it's just another title, so it goes
        // quietly.
        ps4::removeUpdaterApp();
#endif

        HttpClient client;
        HttpRequest req;
        // VitaPlex publishes pre-releases; /releases/latest never returns
        // those, so read the feed and take the newest non-draft entry.
        req.url = "https://api.github.com/repos/" + std::string(kRepo) + "/releases?per_page=10";
        req.method = "GET";
        req.headers["Accept"] = "application/vnd.github+json";
        req.timeout = 15;

        HttpResponse resp = client.request(req);
        if (resp.statusCode != 200 || resp.body.empty()) {
            brls::Logger::error("app_update: releases feed HTTP {}", resp.statusCode);
            if (manual) {
                brls::sync([]() {
                    auto* d = new brls::Dialog("Could not reach GitHub to check for updates.");
                    d->addButton("OK", []() {});
                    d->open();
                });
            }
            s_busy = false;
            return;
        }

        ReleaseInfo rel;
        forEachTopLevelObject(resp.body, [&rel](const std::string& obj) {
            if (jsonBool(obj, "draft")) return true;   // keep looking
            rel.tag         = jsonString(obj, "tag_name");
            rel.pageUrl     = jsonString(obj, "html_url");
            rel.notes       = jsonString(obj, "body");
            rel.publishedAt = jsonString(obj, "published_at");
            rel.prerelease  = jsonBool(obj, "prerelease");

            const std::string want = assetSuffix();
            size_t assetsAt = obj.find("\"assets\"");
            if (!want.empty() && assetsAt != std::string::npos) {
                std::string assets = obj.substr(assetsAt);
                size_t pos = assets.find('[');
                while (pos != std::string::npos && pos < assets.size()) {
                    while (pos < assets.size() && assets[pos] != '{' && assets[pos] != ']') pos++;
                    if (pos >= assets.size() || assets[pos] == ']') break;
                    size_t end = objectEnd(assets, pos);
                    std::string a = assets.substr(pos, end - pos);
                    std::string name = jsonString(a, "name");
                    if (name.size() > want.size() &&
                        name.compare(name.size() - want.size(), want.size(), want) == 0) {
                        rel.assetUrl  = jsonString(a, "browser_download_url");
                        rel.assetSize = jsonInt(a, "size");
                        break;
                    }
                    pos = end;
                }
            }
            return false;   // newest non-draft found
        });

        if (rel.tag.empty()) {
            brls::Logger::info("app_update: no published releases");
            s_busy = false;
            return;
        }

        const std::string current = VITA_PLEX_DISPLAY_VERSION;
        if (!isNewer(rel.tag, current)) {
            brls::Logger::info("app_update: up to date ({} vs {})", current, rel.tag);
            if (manual) {
                brls::sync([current]() {
                    auto* d = new brls::Dialog("VitaPlex is up to date (" + current + ").");
                    d->addButton("OK", []() {});
                    d->open();
                });
            }
            s_busy = false;
            return;
        }

        // The startup check respects a skipped release; the settings cell
        // always shows what it found.
        if (!manual &&
            rel.tag == Application::getInstance().getSettings().skippedUpdateVersion) {
            brls::Logger::info("app_update: {} available but skipped by user", rel.tag);
            s_busy = false;
            return;
        }

        brls::Logger::info("app_update: update available {} (current {})", rel.tag, current);
        brls::sync([rel]() { offerUpdate(rel); });
    });
}

}  // namespace app_update
}  // namespace vitaplex
