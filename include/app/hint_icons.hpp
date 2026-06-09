/**
 * VitaPlex hint icon registry.
 *
 * Resolves abstract button names (A/B/X/Y/Start/Select/L/R/dpad) to the
 * platform-specific PNG path under resources/images/<Platform>/. The set
 * of platforms is fixed at compile time except on desktop and android,
 * where the icon set follows the current input source (controller / kbd
 * & mouse / touch) and switches at runtime.
 *
 *   PSV       -> resources/images/PSV/
 *   PS4       -> resources/images/PS5/
 *   Switch    -> resources/images/Nintendo Switch/Default/
 *   Desktop   -> resources/images/Steam Deck/Default/   (controller)
 *                resources/images/Keyboard & Mouse/Default/
 *                resources/images/Touch/Default/
 *   Android   -> resources/images/Touch/Default/        (touch by default)
 *                resources/images/Steam Deck/Default/
 *                resources/images/Keyboard & Mouse/Default/
 */

#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>

namespace vitaplex {

enum class InputSource {
    Controller,
    KeyboardMouse,
    Touch,
};

class HintIcons {
public:
    // Resolves a borealis ControllerButton to a platform-specific resource
    // path (relative to RESOURCE_PREFIX, e.g. "images/PSV/cross_button.png").
    // Returns an empty string if no icon exists for the requested button on
    // the current platform / input source.
    static std::string getResPath(brls::ControllerButton button);

    // Current input source. On PSV/PS4/Switch this is fixed at Controller.
    // On Desktop / Android it tracks the most recently used input.
    static InputSource currentSource();

    // Subscribe to input source changes. Callback fires on the brls UI
    // thread whenever currentSource() flips value. Subscription lasts the
    // lifetime of the process.
    static void onSourceChanged(std::function<void()> cb);

    // Initialize input source tracking. Wires up the brls input hooks that
    // detect controller / keyboard&mouse / touch activity on desktop and
    // android. No-op on PSV / PS4 / Switch. Call once from app startup.
    static void init();
};

}  // namespace vitaplex
