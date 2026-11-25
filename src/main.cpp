/**
 * VitaPlex - Plex Client (multi-platform)
 *
 * Main entry point with Borealis UI framework.
 *
 * All platform-specific bootstrap (Vita sysmodules / network init, PS4
 * Orbis modules, Android APK asset extraction, log file routing, …) lives
 * in src/platform/platform_<name>.cpp. CMake selects exactly one of those
 * implementation files at configure time, so this file is platform-agnostic
 * apart from the Android SDL_main entry point that the SDL2 framework
 * mandates.
 *
 * Based on switchfin architecture (https://github.com/dragonflylee/switchfin)
 * UI powered by Borealis (https://github.com/dragonflylee/borealis)
 */

#include <borealis.hpp>
#include <nanovg.h>
#ifdef __ANDROID__
#include <SDL2/SDL_hints.h>   // the screen-keyboard hint (not SDL.h: its SDL_main.h renames main)
#endif
#ifdef USE_LIBROMFS
#include <romfs/romfs.hpp>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#include <cstdio>
#endif
#include "app/application.hpp"
#include "utils/app_update.hpp"
#include "utils/background_audio.hpp"
#include "utils/shell_integration.hpp"
#include "app/plex_client.hpp"
#include "view/media_item_cell.hpp"
#include "view/recycling_grid.hpp"
#include "view/media_detail_view.hpp"
#include "view/video_view.hpp"
#include "app/downloads_manager.hpp"
#include "app/hint_icons.hpp"
#include "utils/http_client.hpp"
#include "platform/platform.hpp"

// Chinese / Japanese / Korean characters in the menus (song, album and film
// titles). borealis draws them on Switch from the console's own fonts; on
// every other platform its font (DejaVu Sans) has none, and they came out
// blank. resources/cjk/subfont.ttf (Noto Sans CJK, cut down to the common
// characters; also mpv's fallback for subtitles) is added as a fallback.
static void loadCjkFallbackFont() {
#if !defined(__SWITCH__)
    const std::string path = BRLS_ASSET("cjk/subfont.ttf");
    bool ok = false;
#ifdef USE_LIBROMFS
    try {
        auto& font = romfs::get(path.substr(5));   // after "@res/"
        ok = font.valid() &&
             brls::Application::loadFontFromMemory("cjk", (void*)font.data(), font.size(), false);
    } catch (...) {
    }
#else
    ok = brls::Application::loadFontFromFile("cjk", path);
#endif
    if (ok) {
        nvgAddFallbackFontId(brls::Application::getNVGContext(),
                             brls::Application::getFont(brls::FONT_REGULAR),
                             brls::Application::getFont("cjk"));
        brls::Logger::info("CJK fallback font loaded");
    } else {
        brls::Logger::warning("CJK fallback font not loaded from {}", path);
    }
#endif
}

// One keyboard at a time on Android. Text fields open borealis' own
// on-screen keyboard (EditTextDialog) when a remote or controller is in use,
// and SDL_StartTextInput(), which that dialog also calls, raised Android's
// keyboard on top of it (a TV screenshot: both at once). SDL shows the
// system keyboard only while SDL_HINT_ENABLE_SCREEN_KEYBOARD is on (SDL
// 2.32: on Android that is all StartTextInput does besides enabling text
// events), so it follows the input in use: touch gets Android's keyboard
// (borealis hides its own then), a remote or controller gets borealis'.
// A hardware keyboard still types either way.
#ifdef __ANDROID__
static void followInputForScreenKeyboard() {
    auto apply = [](brls::InputType t) {
        SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, t == brls::InputType::TOUCH ? "1" : "0");
    };
    apply(brls::Application::getInputType());
    brls::Application::getGlobalInputTypeChangeEvent()->subscribe(apply);
}
#endif

static void registerCustomViews() {
    brls::Application::registerXMLView("MediaItemCell", vitaplex::MediaItemCell::create);
    brls::Application::registerXMLView("RecyclingGrid", vitaplex::RecyclingGrid::create);
    brls::Application::registerXMLView("vitaplex:VideoView", vitaplex::VideoView::create);
}

// Shared entry point used by main() on every platform and by SDL_main() on
// Android (which lives in src/platform/platform_android.cpp).
extern "C" int VitaPlexMainEntry(int argc, char* argv[]) {
#ifdef __SWITCH__
    // Refuse applet-mode launches (opening the homebrew menu from the album
    // instead of over a title). Applets get a fraction of the RAM — video
    // playback dies — and only 2 BSD socket sessions, so any concurrent
    // network use fails with "couldn't connect" (the in-app updater's
    // download was the reproducible victim).
    {
        AppletType at = appletGetAppletType();
        if (at != AppletType_Application && at != AppletType_SystemApplication) {
            consoleInit(NULL);
            printf("\n  VitaPlex needs full memory to run.\n");
            printf("\n  Launch it with title override: hold R while\n");
            printf("  opening any game, then start VitaPlex from\n");
            printf("  the homebrew menu.\n");
            printf("\n  Press + to exit.\n");
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            PadState pad;
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
                consoleUpdate(NULL);
            }
            consoleExit(NULL);
            return 0;
        }
    }
#endif

    // Where our own executable lives — on Switch the updater replaces it.
    if (argc > 0) vitaplex::app_update::setSelfPath(argv[0]);

    // A URL on the command line is how a desktop deep link arrives: the
    // .desktop declares x-scheme-handler/plex and x-scheme-handler/vitaplex,
    // and the shell runs Exec with the URL appended. MainActivity already knows
    // what to do with one; this is the only piece that was missing.
    if (argc > 1 && argv[1]) {
        const std::string arg = argv[1];
        if (arg.rfind("plex://", 0) == 0 || arg.rfind("vitaplex://", 0) == 0 ||
            arg.rfind("https://app.plex.tv", 0) == 0) {
            vitaplex::platform::offerDeepLink(arg);
        }
    }

    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

    // Bootstrap the current platform: load native modules, init networking,
    // open the platform-specific log file (if any). Implementation lives in
    // src/platform/platform_<name>.cpp — selected by CMake.
    if (!vitaplex::platform::init()) {
        brls::Logger::error("Platform init failed");
        vitaplex::platform::shutdown();
        if (vitaplex::platform::needsHardExit()) {
            vitaplex::platform::hardExit(1);
        }
        return 1;
    }

    // Declare this process's identity to the shell before any window exists —
    // on Windows the AppUserModelID is what the taskbar groups the button
    // under, and what a toast is later sent as, and it has to be set first.
    // No-op on every other platform.
    vitaplex::shell::init();

    // Initialize Borealis
    if (!brls::Application::init()) {
        brls::Logger::error("Failed to initialize Borealis");
        vitaplex::platform::shutdown();
        if (vitaplex::platform::needsHardExit()) {
            vitaplex::platform::hardExit(1);
        }
        return 1;
    }

    // Override sidebar padding for better text visibility on Vita's small screen
    brls::Style style = brls::getStyle();
    style.addMetric("brls/sidebar/padding_left", 20.0f);
    style.addMetric("brls/sidebar/padding_right", 20.0f);

    // Create window
    brls::Application::createWindow(APP_DISPLAY_NAME);
    loadCjkFallbackFont();
#ifdef __ANDROID__
    followInputForScreenKeyboard();
#endif

    // Set theme colors (Plex-like)
    brls::Application::getPlatform()->getThemeVariant();

    registerCustomViews();

    // Wire up runtime input-source tracking so hint icons can switch
    // between controller / kbd+mouse / touch on desktop and android.
    // Must run AFTER brls::Application::init() so the platform input
    // manager exists.
    vitaplex::HintIcons::init();

    // Initialize downloads manager
    vitaplex::DownloadsManager::getInstance().init();

    // Initialize application
    vitaplex::Application& app = vitaplex::Application::getInstance();
    if (!app.init()) {
        brls::Logger::error("Failed to initialize VitaPlex");
        vitaplex::platform::shutdown();
        if (vitaplex::platform::needsHardExit()) {
            vitaplex::platform::hardExit(1);
        }
        return 1;
    }

    // Settings are loaded by now, so the shell can be told whether it may write
    // the Start Menu shortcut that toasts are keyed on. Windows-only in effect.
    vitaplex::shell::setShortcutAllowed(app.getSettings().windowsStartMenuShortcut);

    // The Vita's music outside the app starts watching here, and takes the
    // music back from SceShell. No-op on every other platform.
    vitaplex::bgaudio::init(app.getSettings().vitaOutsideMusic);
    vitaplex::bgaudio::setOfflineSkip(app.getSettings().vitaOfflineSkip);
    vitaplex::bgaudio::setQuickMenuControls(app.getSettings().vitaQuickMenuControls);

    // Run application (blocking)
    app.run();

    // Shutdown
    app.shutdown();
    vitaplex::platform::shutdown();

    if (vitaplex::platform::needsHardExit()) {
        vitaplex::platform::hardExit(0);
    }
    return 0;
}

// iOS / tvOS start in SDL's own main() (SDL2main), which runs
// UIApplicationMain and only then calls SDL_main (platform_ios.mm). A main()
// here would win the link and skip UIKit's start entirely, so the app died
// as soon as SDL asked for a window.
#if !(defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV))
int main(int argc, char* argv[]) {
    return VitaPlexMainEntry(argc, argv);
}
#endif
