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
#if defined(__aarch64__)
    return "-arm64-v8a.apk";
#else
    return "-armeabi-v7a.apk";
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

// ── The in-place installers (Switch / Vita / Android) ───────────────────
#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID)

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
    if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;
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
    const char* relaunchLabel = "Relaunch from LiveArea";
#elif defined(__SWITCH__)
    const char* relaunchLabel = "Relaunch to apply";
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
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { installFailed("cannot open " + path, ui); s_busy = false; return; }
#elif defined(ANDROID)
        const std::string path = platformPath("update.apk");
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { installFailed("cannot open " + path, ui); s_busy = false; return; }
#else
        const std::string path = platformPath("update.vpk");
        SceUID f = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (f < 0) { installFailed("cannot open " + path, ui); s_busy = false; return; }
#endif

        HttpClient client;
        int64_t total = rel.assetSize;
        int64_t got = 0;
        int lastPct = -1;
        bool ok = client.downloadFile(
            rel.assetUrl,
            [&](const char* data, size_t size) -> bool {
                if (s_cancel.load()) return false;
#if defined(__SWITCH__) || defined(ANDROID)
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
            [&](int64_t sz) { if (total <= 0) total = sz; });

#if defined(__SWITCH__) || defined(ANDROID)
        fclose(f);
#else
        sceIoClose(f);
#endif

        if (s_cancel.load()) {
#if defined(__SWITCH__) || defined(ANDROID)
            remove(path.c_str());
#else
            sceIoRemove(path.c_str());
#endif
            s_busy = false;
            return;   // user cancellation, not a failure
        }
        if (!ok || (rel.assetSize > 0 && got != rel.assetSize)) {
#if defined(__SWITCH__) || defined(ANDROID)
            remove(path.c_str());
#else
            sceIoRemove(path.c_str());
#endif
            installFailed("incomplete download (" + std::to_string(got) + "/" +
                          std::to_string(rel.assetSize) + " bytes)", ui);
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
#if defined(ANDROID)
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
#else
        std::string err;
        int lastInstallPct = -1;
        int rc = vita::installVpk(path, platformPath("update"), err,
            [ui, &lastInstallPct](int done, int totalFiles) {
                if (totalFiles <= 0) return;
                int pct = done * 100 / totalFiles;
                if (pct == lastInstallPct) return;
                lastInstallPct = pct;
                brls::sync([ui, pct]() {
                    if (ui->dismissed->load()) return;
                    // Extraction is the long part; the promotion after the
                    // last file reports nothing, so 100% reads "Finishing".
                    if (pct >= 100) stepActive(ui->install, "Finishing\xE2\x80\xA6", 1.0f);
                    else stepActive(ui->install, "Installing\xE2\x80\xA6 " +
                                    std::to_string(pct) + "%", (float)pct / 100.0f);
                });
            });
        sceIoRemove(path.c_str());
        if (rc != 0) {
            installFailed(err, ui);
            s_busy = false;
            return;
        }
        brls::sync([ui]() {
            if (!ui->dismissed->load()) stepDone(ui->install, "Installed");
        });
        finishInstall(ui, []() {
            auto* d = new brls::Dialog("Update installed. VitaPlex will now close - relaunch it from the LiveArea.");
            d->addButton("OK", []() { brls::Application::quit(); });
            d->open();
        });
#endif
        s_busy = false;
    });
}
#endif  // __SWITCH__ || __PSV__ || ANDROID

// The offer dialog (design_handoff_update, dialog B): gold strip, icon
// tile, current → new version cards, size/platform caption, then the
// per-platform actions. Release notes stay on the GitHub page — the
// View on GitHub button opens it where the platform has a browser.
void offerUpdate(const ReleaseInfo rel) {
    const float screenW = platform::viewportWidth();
    float panelW = 428.0f;
    if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;

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
               std::string("installs in place \xC2\xB7 relaunch from LiveArea");
#elif defined(__SWITCH__)
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("installs in place \xC2\xB7 relaunch to apply");
#elif defined(ANDROID)
    caption += (caption.empty() ? "" : " \xC2\xB7 ") +
               std::string("system installer opens when ready");
#elif defined(__PS4__)
    // The release page is the way to get it; keep the address visible in
    // case the browser hand-off fails silently.
    caption += (caption.empty() ? "" : " \xC2\xB7 ") + rel.pageUrl;
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
#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID)
    // These install in place; the GitHub page stays a secondary action.
    if (!rel.assetUrl.empty()) {
        primary = makeButton("\xE2\x86\x93  Update", BtnStyle::Gold, [rel]() {
            brls::Application::popActivity(brls::TransitionAnimation::FADE,
                                           [rel]() { startInstall(rel); });
        });
        primary->setGrow(1.0f);
        buttons->addView(primary);

        auto* gh = makeButton("View on GitHub", BtnStyle::Gray, [rel]() {
            brls::Application::getPlatform()->openBrowser(rel.pageUrl);
        });
        gh->setWidth(148.0f);
        gh->setMarginLeft(8.0f);
        buttons->addView(gh);
    } else {
        // The release carries no asset for this platform/flavour — the
        // page is all there is.
        primary = makeButton("View on GitHub", BtnStyle::Gold, [rel]() {
            brls::Application::getPlatform()->openBrowser(rel.pageUrl);
        });
        primary->setGrow(1.0f);
        buttons->addView(primary);
    }
#elif defined(__PS4__)
    primary = makeButton("View on GitHub", BtnStyle::Gold, [rel]() {
        brls::Application::getPlatform()->openBrowser(rel.pageUrl);
    });
    primary->setGrow(1.0f);
    buttons->addView(primary);
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
            rel.tag     = jsonString(obj, "tag_name");
            rel.pageUrl = jsonString(obj, "html_url");

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
