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

// ── Release notes → plain text ───────────────────────────────────────────
std::string cleanNotes(const std::string& md) {
    std::string out;
    out.reserve(md.size());
    bool lineStart = true;
    for (size_t i = 0; i < md.size(); i++) {
        char c = md[i];
        if (lineStart && (c == '#' || c == '>')) continue;
        if (lineStart && (c == '-' || c == '*') && i + 1 < md.size() && md[i + 1] == ' ') {
            out += "\xE2\x80\xA2";   // bullet
            continue;
        }
        if (c == '`' || c == '*') continue;
        if (c == '[') {   // [text](url) -> text
            size_t close = md.find(']', i);
            size_t paren = close == std::string::npos ? std::string::npos : md.find('(', close);
            if (close != std::string::npos && paren == close + 1) {
                size_t end = md.find(')', paren);
                if (end != std::string::npos) {
                    out += md.substr(i + 1, close - i - 1);
                    i = end;
                    continue;
                }
            }
        }
        out += c;
        lineStart = (c == '\n');
    }
    size_t a = out.find_first_not_of("\n \t");
    size_t z = out.find_last_not_of("\n \t");
    if (a == std::string::npos) return {};
    out = out.substr(a, z - a + 1);
    if (out.size() > 700) out = out.substr(0, 697) + "...";
    return out;
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
    std::string notes;
    std::string pageUrl;
    std::string assetUrl;
    int64_t     assetSize = 0;
};

// ── The in-place installers (Switch / Vita / Android) ───────────────────
#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID)

void finishInstall(brls::Dialog* dialog, std::shared_ptr<std::atomic<bool>> dismissed,
                   std::function<void()> then) {
    brls::sync([dialog, dismissed, then]() {
        if (dismissed->exchange(true)) then();
        else dialog->close(then);
    });
}

void installFailed(const std::string& msg, brls::Dialog* dialog,
                   std::shared_ptr<std::atomic<bool>> dismissed) {
    finishInstall(dialog, dismissed, [msg]() {
        auto* d = new brls::Dialog("Update failed:\n" + msg);
        d->addButton("OK", []() {});
        d->open();
    });
}

void startInstall(const ReleaseInfo rel) {
    s_cancel = false;

    auto* label = new brls::Label();
    label->setFontSize(16);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setText("Downloading " + rel.tag + "\xE2\x80\xA6 0%");

    auto* box = new brls::Box();
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setPadding(24, 40, 24, 40);
    box->addView(label);

    auto* dialog = new brls::Dialog(box);
    dialog->setCancelable(false);
    auto dismissed = std::make_shared<std::atomic<bool>>(false);
    dialog->addButton("Cancel", [dismissed]() {
        dismissed->store(true);
        s_cancel = true;
    });
    dialog->open();

    asyncRun([rel, label, dialog, dismissed]() {
#if defined(__SWITCH__)
        const std::string path = platformPath("update.nro");
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { installFailed("cannot open " + path, dialog, dismissed); s_busy = false; return; }
#elif defined(ANDROID)
        const std::string path = platformPath("update.apk");
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { installFailed("cannot open " + path, dialog, dismissed); s_busy = false; return; }
#else
        const std::string path = platformPath("update.vpk");
        SceUID f = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (f < 0) { installFailed("cannot open " + path, dialog, dismissed); s_busy = false; return; }
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
                        brls::sync([label, dismissed, pct, rel]() {
                            if (!dismissed->load())
                                label->setText("Downloading " + rel.tag + "\xE2\x80\xA6 " +
                                               std::to_string(pct) + "%");
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
                          std::to_string(rel.assetSize) + " bytes)", dialog, dismissed);
            s_busy = false;
            return;
        }

#if defined(ANDROID)
        // Hand the APK to the system package installer via
        // PlatformUtils.installApk — the content:// route works on
        // Android TV too, where no browser exists to fall back on. The
        // JNI call runs on the main thread: SDL attaches that thread to
        // the VM for certain.
        finishInstall(dialog, dismissed, [path]() {
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
            installFailed("could not replace " + target + ": " + ec.message(), dialog, dismissed);
            s_busy = false;
            return;
        }
        finishInstall(dialog, dismissed, []() {
            auto* d = new brls::Dialog("Update installed. VitaPlex will now close - relaunch to use the new version.");
            d->addButton("OK", []() { brls::Application::quit(); });
            d->open();
        });
#else
        brls::sync([label, dismissed]() {
            if (!dismissed->load()) label->setText("Installing\xE2\x80\xA6");
        });
        std::string err;
        int rc = vita::installVpk(path, platformPath("update"), err);
        sceIoRemove(path.c_str());
        if (rc != 0) {
            installFailed(err, dialog, dismissed);
            s_busy = false;
            return;
        }
        finishInstall(dialog, dismissed, []() {
            auto* d = new brls::Dialog("Update installed. VitaPlex will now close - relaunch it from the LiveArea.");
            d->addButton("OK", []() { brls::Application::quit(); });
            d->open();
        });
#endif
        s_busy = false;
    });
}
#endif  // __SWITCH__ || __PSV__

void offerUpdate(const ReleaseInfo rel) {
    std::string text = "Update available: " + rel.tag + "\n(current: " +
                       std::string(VITA_PLEX_DISPLAY_VERSION) + ")";
    if (!rel.notes.empty()) text += "\n\n" + rel.notes;

    auto* dialog = new brls::Dialog(text);
    dialog->addButton("Later", [rel]() {
        // Startup checks stop offering this release; the settings cell
        // always re-offers.
        AppSettings& s = Application::getInstance().getSettings();
        s.skippedUpdateVersion = rel.tag;
        Application::getInstance().saveSettings();
        s_busy = false;
    });

#if defined(__SWITCH__) || defined(__PSV__) || defined(ANDROID)
    // Android downloads and hands the APK to the system installer rather
    // than opening a browser: Android TV ships no browser, so the
    // openBrowser route does nothing there at all.
    if (!rel.assetUrl.empty()) {
        dialog->addButton("Update", [rel]() { startInstall(rel); });
    } else {
        dialog->addButton("OK", []() { s_busy = false; });
    }
#elif defined(__PS4__)
    // No browser to hand off to: show where to get it.
    dialog->addButton("OK", []() { s_busy = false; });
#else
    // Android and desktop: the release page handles download + install
    // (the APK asset directly on Android, so the browser starts the
    // download at once).
    std::string url = !rel.assetUrl.empty() ? rel.assetUrl : rel.pageUrl;
    dialog->addButton("Download", [url]() {
        brls::Application::getPlatform()->openBrowser(url);
        s_busy = false;
    });
#endif
    dialog->open();
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
            rel.notes   = cleanNotes(jsonString(obj, "body"));
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
