/**
 * VitaPlex platform layer — shared implementation.
 *
 * Holds the viewport / orientation helpers that don't vary by target.
 * Compiled on EVERY platform (linked alongside platform_<name>.cpp).
 * Everything here defers to brls::Application::contentWidth / contentHeight
 * which borealis updates whenever the window resizes, so the helpers are
 * always live.
 */

#include "platform/platform.hpp"

#include <borealis.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cstdio>    // std::remove / std::rename, for the log rotation below
#include <string>
#include <vector>

namespace vitaplex {
namespace platform {

bool isPortrait() {
    // contentHeight > contentWidth is the canonical portrait test.
    // contentWidth/Height are float by definition in borealis; the
    // strict-greater handles the square-screen edge case as "landscape"
    // which is what every layout expects.
    return brls::Application::contentHeight > brls::Application::contentWidth;
}

float viewportWidth() {
    return brls::Application::contentWidth;
}

float viewportHeight() {
    return brls::Application::contentHeight;
}

bool isPhoneScreen() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS)
    const float vw = viewportWidth();
    const float vh = viewportHeight();
    if (vw <= 0.0f || vh <= 0.0f) return false;   // unknown: assume not
    if (vh > vw) return true;                     // portrait phone or tablet
    return vw < 600.0f;                           // short-edge handset, landscape
#else
    return false;   // PSV / PS4 / Switch / desktop
#endif
}

float uiScale() {
    // The mobile designs are 412 units wide against a 1280-unit viewport.
    // Anything drawn at handset scale multiplies by this to land right.
    return isPhoneScreen() ? (1280.0f / 412.0f) : 1.0f;
}

// Orientation-change dispatch. We register a single brls window-size
// listener and only fire the subscribed callbacks when the orientation
// boundary is actually crossed, so views that re-layout on rotation
// aren't paying for every resize tick (borealis emits a lot of them
// during window drags / animations).
namespace {
struct OrientationDispatcher {
    bool installed = false;
    bool lastIsPortrait = false;
    std::vector<std::function<void()>> callbacks;
};
OrientationDispatcher& dispatcher() {
    static OrientationDispatcher d;
    return d;
}
void ensureInstalled() {
    auto& d = dispatcher();
    if (d.installed) return;
    d.installed = true;
    d.lastIsPortrait = isPortrait();
    auto* evt = brls::Application::getWindowSizeChangedEvent();
    if (!evt) return;
    evt->subscribe([]() {
        auto& d = dispatcher();
        bool now = isPortrait();
        if (now == d.lastIsPortrait) return; // orientation didn't flip
        d.lastIsPortrait = now;
        // Copy the callback list before firing — a callback could
        // register another listener and resize the vector mid-iteration.
        auto cbs = d.callbacks;
        for (auto& cb : cbs) cb();
    });
}
}  // namespace

void onOrientationChanged(std::function<void()> cb) {
    ensureInstalled();
    dispatcher().callbacks.push_back(std::move(cb));
}

// Keep the finished run's log beside the new one.
//
// Every platform opened this file with "w", and the reasoning written down
// next to it is sound as far as it goes: this file is for "it just did the
// wrong thing, send me the log", and an unbounded file nobody rotates is its
// own problem. But truncating on launch is exactly backwards for the bugs
// most worth reporting. Anything whose workaround is "reopen the app" wipes
// its own evidence at the very moment the user works around it — which is how
// a report of "the next track would not start until I reopened the app"
// reached us three times with three copies of a log from a different session,
// because the only log that could have held the answer had been destroyed by
// the restart that made the app usable again.
//
// One previous run, so this is still bounded at two files rather than the
// unbounded growth the truncate was avoiding.
std::string previousLogPath() {
    const std::string path = getLogPath();
    if (path.empty()) return std::string{};

    // Insert before the extension, and only a real extension: a dot in a
    // directory name ("/sdcard/My.Files/vitaplex") is not one.
    const std::size_t dot   = path.rfind('.');
    const std::size_t slash = path.find_last_of("/\\:");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + ".prev";
    return path.substr(0, dot) + ".prev" + path.substr(dot);
}

void rotateLogForNewRun() {
    const std::string path = getLogPath();
    if (path.empty()) return;
    const std::string prev = previousLogPath();
    if (prev.empty() || prev == path) return;

    // rename() will not replace an existing file on some of these targets,
    // so clear the older copy first. Both calls failing is fine and expected
    // on a first run — there is simply nothing to move yet.
    std::remove(prev.c_str());
    std::rename(path.c_str(), prev.c_str());
}

}  // namespace platform
}  // namespace vitaplex
