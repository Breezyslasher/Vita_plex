/**
 * VitaPlex hint icon registry — implementation.
 *
 * The resolver tables below map a borealis ControllerButton to a relative
 * resource path. One table per "icon set":
 *
 *   PSV         (always, PSV build)
 *   PlayStation (PS4 build — shares the PS5 Default art for PS-family
 *                buttons, which is a unified set covering PS3-5)
 *   Switch      (Switch build)
 *   SteamDeck   (Desktop / Android with controller as the latest input)
 *   Keyboard    (Desktop / Android with kbd/mouse as the latest input)
 *   Touch       (Desktop / Android with touch as the latest input)
 *
 * On PSV/PS4/Switch the source is fixed; on Desktop and Android we listen
 * to brls's keyboard / mouse / controller hooks and switch in real time.
 */

#include "app/hint_icons.hpp"

#include <borealis.hpp>

#include <atomic>
#include <vector>
#include <mutex>

namespace vitaplex {

namespace {

// Maps a borealis ControllerButton to a relative resource path. Unknown
// buttons (or buttons without a matching art asset on the platform) return
// the empty string — callers are responsible for hiding the hint icon.
const char* psvPath(brls::ControllerButton b) {
    switch (b) {
        case brls::BUTTON_A:     return "images/PSV/cross_button.png";
        case brls::BUTTON_B:     return "images/PSV/circle_button.png";
        case brls::BUTTON_X:     return "images/PSV/square_button.png";
        case brls::BUTTON_Y:     return "images/PSV/triangle_button.png";
        case brls::BUTTON_START: return "images/PSV/start_button.png";
        case brls::BUTTON_BACK:  return "images/PSV/select_button.png";
        case brls::BUTTON_LB:    return "images/PSV/l_button.png";
        case brls::BUTTON_RB:    return "images/PSV/r_button.png";
        default:                 return "";
    }
}

// PS4 native art (the line-art "outline-*" set under images/Ps4/).
// This is the set bundled into the PS4 .pkg by CMakeLists, so any
// button we map here MUST exist as a file in that directory.
const char* ps4Path(brls::ControllerButton b) {
    switch (b) {
        case brls::BUTTON_A:     return "images/Ps4/outline-cross.png";
        case brls::BUTTON_B:     return "images/Ps4/outline-circle.png";
        case brls::BUTTON_X:     return "images/Ps4/outline-square.png";
        case brls::BUTTON_Y:     return "images/Ps4/outline-triangle.png";
        case brls::BUTTON_START: return "images/Ps4/plain-big-option.png";
        case brls::BUTTON_BACK:  return "images/Ps4/outline-share.png";
        case brls::BUTTON_LB:    return "images/Ps4/outline-L1.png";
        case brls::BUTTON_RB:    return "images/Ps4/outline-R1.png";
        case brls::BUTTON_LT:    return "images/Ps4/outline-L2.png";
        case brls::BUTTON_RT:    return "images/Ps4/outline-R2.png";
        case brls::BUTTON_GUIDE: return "images/Ps4/outline-PS.png";
        default:                 return "";
    }
}

const char* switchPath(brls::ControllerButton b) {
    switch (b) {
        case brls::BUTTON_A:     return "images/Nintendo Switch/Default/switch_button_a.png";
        case brls::BUTTON_B:     return "images/Nintendo Switch/Default/switch_button_b.png";
        case brls::BUTTON_X:     return "images/Nintendo Switch/Default/switch_button_x.png";
        case brls::BUTTON_Y:     return "images/Nintendo Switch/Default/switch_button_y.png";
        case brls::BUTTON_START: return "images/Nintendo Switch/Default/switch_button_plus.png";
        case brls::BUTTON_BACK:  return "images/Nintendo Switch/Default/switch_button_minus.png";
        case brls::BUTTON_LB:    return "images/Nintendo Switch/Default/switch_button_l.png";
        case brls::BUTTON_RB:    return "images/Nintendo Switch/Default/switch_button_r.png";
        case brls::BUTTON_LT:    return "images/Nintendo Switch/Default/switch_button_zl.png";
        case brls::BUTTON_RT:    return "images/Nintendo Switch/Default/switch_button_zr.png";
        default:                 return "";
    }
}

const char* steamDeckPath(brls::ControllerButton b) {
    switch (b) {
        case brls::BUTTON_A:     return "images/Steam Deck/Default/steamdeck_button_a.png";
        case brls::BUTTON_B:     return "images/Steam Deck/Default/steamdeck_button_b.png";
        case brls::BUTTON_X:     return "images/Steam Deck/Default/steamdeck_button_x.png";
        case brls::BUTTON_Y:     return "images/Steam Deck/Default/steamdeck_button_y.png";
        case brls::BUTTON_START: return "images/Steam Deck/Default/steamdeck_button_options.png";
        case brls::BUTTON_BACK:  return "images/Steam Deck/Default/steamdeck_button_view.png";
        case brls::BUTTON_LB:    return "images/Steam Deck/Default/steamdeck_button_l1.png";
        case brls::BUTTON_RB:    return "images/Steam Deck/Default/steamdeck_button_r1.png";
        case brls::BUTTON_LT:    return "images/Steam Deck/Default/steamdeck_button_l2.png";
        case brls::BUTTON_RT:    return "images/Steam Deck/Default/steamdeck_button_r2.png";
        case brls::BUTTON_GUIDE: return "images/Steam Deck/Default/steamdeck_button_guide.png";
        default:                 return "";
    }
}

const char* keyboardPath(brls::ControllerButton b) {
    // Best-effort: map common gamepad actions to the keys that brls's
    // SDL2 default binding actually fires.
    //   A      -> Enter   (confirm)
    //   B      -> Esc     (cancel/back)
    //   X      -> X
    //   Y      -> Y
    //   Start  -> +
    //   Back   -> -
    //   LB/RB  -> Q / E
    //   dpad   -> arrow keys
    switch (b) {
        case brls::BUTTON_A:     return "images/Keyboard & Mouse/Default/keyboard_enter.png";
        case brls::BUTTON_B:     return "images/Keyboard & Mouse/Default/keyboard_escape.png";
        case brls::BUTTON_X:     return "images/Keyboard & Mouse/Default/keyboard_x.png";
        case brls::BUTTON_Y:     return "images/Keyboard & Mouse/Default/keyboard_y.png";
        case brls::BUTTON_START: return "images/Keyboard & Mouse/Default/keyboard_plus.png";
        case brls::BUTTON_BACK:  return "images/Keyboard & Mouse/Default/keyboard_minus.png";
        case brls::BUTTON_LB:    return "images/Keyboard & Mouse/Default/keyboard_q.png";
        case brls::BUTTON_RB:    return "images/Keyboard & Mouse/Default/keyboard_e.png";
        case brls::BUTTON_UP:    return "images/Keyboard & Mouse/Default/keyboard_arrow_up.png";
        case brls::BUTTON_DOWN:  return "images/Keyboard & Mouse/Default/keyboard_arrow_down.png";
        case brls::BUTTON_LEFT:  return "images/Keyboard & Mouse/Default/keyboard_arrow_left.png";
        case brls::BUTTON_RIGHT: return "images/Keyboard & Mouse/Default/keyboard_arrow_right.png";
        default:                 return "";
    }
}

const char* touchPath(brls::ControllerButton b) {
    // Touch doesn't have a 1:1 mapping for every controller button — most
    // app interactions reduce to "tap". Keep the table small and focused on
    // the actions that actually surface on a touchscreen.
    switch (b) {
        case brls::BUTTON_A:     return "images/Touch/Default/touch_tap.png";
        case brls::BUTTON_B:     return "images/Touch/Default/touch_swipe_right.png";
        case brls::BUTTON_X:     return "images/Touch/Default/touch_tap_hold.png";
        case brls::BUTTON_Y:     return "images/Touch/Default/touch_tap_double.png";
        case brls::BUTTON_START: return "images/Touch/Default/touch_two.png";
        case brls::BUTTON_BACK:  return "images/Touch/Default/touch_swipe_left.png";
        default:                 return "";
    }
}

// Per-platform default + dynamic-switch state. We use the compiler-set
// platform macros that the existing codebase already relies on
// (__vita__, __PS4__, __SWITCH__, __ANDROID__) rather than the CMake
// PLATFORM_* options, which aren't propagated to C++ as defines.
#if defined(__vita__)
constexpr bool kSourceIsDynamic = false;
constexpr InputSource kDefaultSource = InputSource::Controller;
#elif defined(__PS4__) || defined(__ORBIS__)
constexpr bool kSourceIsDynamic = false;
constexpr InputSource kDefaultSource = InputSource::Controller;
#elif defined(__SWITCH__)
constexpr bool kSourceIsDynamic = false;
constexpr InputSource kDefaultSource = InputSource::Controller;
#elif defined(__ANDROID__)
constexpr bool kSourceIsDynamic = true;
constexpr InputSource kDefaultSource = InputSource::Touch;
#else
constexpr bool kSourceIsDynamic = true;
constexpr InputSource kDefaultSource = InputSource::Controller;
#endif

// Tracker state. Atomic so reads from the draw thread are race-free
// against UI-thread writes from brls input events.
std::atomic<InputSource> g_source{kDefaultSource};
std::mutex g_callbacksMutex;
std::vector<std::function<void()>> g_callbacks;
bool g_initialized = false;

void setSource(InputSource s) {
    InputSource prev = g_source.exchange(s);
    if (prev == s) return;
    // Snapshot the callback list before firing so a callback that
    // subscribes can't invalidate the iterator.
    std::vector<std::function<void()>> snap;
    {
        std::lock_guard<std::mutex> lock(g_callbacksMutex);
        snap = g_callbacks;
    }
    for (auto& cb : snap) cb();
}

// Resolves the path table for a given input source on a dynamic platform.
const char* resolveDynamic(brls::ControllerButton b, InputSource src) {
    switch (src) {
        case InputSource::Controller:    return steamDeckPath(b);
        case InputSource::KeyboardMouse: return keyboardPath(b);
        case InputSource::Touch:         return touchPath(b);
    }
    return "";
}

const char* resolveStatic(brls::ControllerButton b) {
#if defined(__vita__)
    return psvPath(b);
#elif defined(__PS4__) || defined(__ORBIS__)
    return ps4Path(b);
#elif defined(__SWITCH__)
    return switchPath(b);
#else
    (void)b;
    return "";
#endif
}

}  // namespace

std::string HintIcons::getResPath(brls::ControllerButton button) {
    const char* p = kSourceIsDynamic
        ? resolveDynamic(button, g_source.load())
        : resolveStatic(button);
    return p ? std::string(p) : std::string();
}

InputSource HintIcons::currentSource() {
    return g_source.load();
}

void HintIcons::onSourceChanged(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(g_callbacksMutex);
    g_callbacks.push_back(std::move(cb));
}

void HintIcons::init() {
    if (g_initialized) return;
    g_initialized = true;

    // On fixed-source platforms there's nothing to watch.
    if (!kSourceIsDynamic) return;

    auto* platform = brls::Application::getPlatform();
    auto* input = platform ? platform->getInputManager() : nullptr;
    if (!input) return;

    // Keyboard activity flips the source to KeyboardMouse. brls fires
    // this event from the SDL keyboard handler.
    input->getKeyboardKeyStateChanged()->subscribe([](brls::KeyState) {
        setSource(InputSource::KeyboardMouse);
    });

    // Mouse movement also indicates kbd/mouse mode.
    input->getMouseCusorOffsetChanged()->subscribe([](brls::Point) {
        setSource(InputSource::KeyboardMouse);
    });

    // Controller sensor activity (gyro / button presses surface through
    // the unified controller state, but the sensor event fires whenever
    // ANY controller activity ticks — close enough to flip the source).
    input->getControllerSensorStateChanged()->subscribe([](brls::SensorEvent) {
        setSource(InputSource::Controller);
    });
}

}  // namespace vitaplex
