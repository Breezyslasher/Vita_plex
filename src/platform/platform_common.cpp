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
