/**
 * VitaPlex hint icon registry.
 *
 * Resolves abstract button names (A/B/X/Y/Start/Select/L/R/dpad) to the
 * platform-specific PNG path under resources/images/<Platform>/. The icon
 * set is fixed per platform at compile time — there is no runtime input
 * swapping. Each platform shows exactly one set:
 *
 *   PSV       -> resources/images/PSV/                  (controller)
 *   PS4       -> resources/images/Ps4/                  (outline-* line-art)
 *   Switch    -> resources/images/Nintendo Switch/Default/   (controller)
 *   Desktop   -> resources/images/Keyboard & Mouse/Default/  (keyboard keys)
 *   Android   -> resources/images/Touch/Default/        (touch hints)
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

    // The platform's fixed input source: Controller on PSV/PS4/Switch,
    // KeyboardMouse on Desktop, Touch on Android. Never changes at runtime.
    static InputSource currentSource();

    // Subscribe to input source changes. Retained for API compatibility, but
    // since the source is now fixed per platform the callback never fires.
    // Subscription lasts the lifetime of the process.
    static void onSourceChanged(std::function<void()> cb);

    // Initialize the hint registry. The input source is fixed per platform,
    // so this is a no-op beyond marking the registry ready. Call once from
    // app startup.
    static void init();
};

}  // namespace vitaplex
