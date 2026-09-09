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

}  // namespace platform
}  // namespace vitaplex
