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
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "view/media_item_cell.hpp"
#include "view/recycling_grid.hpp"
#include "view/media_detail_view.hpp"
#include "view/video_view.hpp"
#include "app/downloads_manager.hpp"
#include "utils/http_client.hpp"
#include "platform/platform.hpp"

static void registerCustomViews() {
    brls::Application::registerXMLView("MediaItemCell", vitaplex::MediaItemCell::create);
    brls::Application::registerXMLView("RecyclingGrid", vitaplex::RecyclingGrid::create);
    brls::Application::registerXMLView("vitaplex:VideoView", vitaplex::VideoView::create);
}

// Shared entry point used by main() on every platform and by SDL_main() on
// Android (which lives in src/platform/platform_android.cpp).
extern "C" int VitaPlexMainEntry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

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
    brls::Application::createWindow("VitaPlex");

    // Set theme colors (Plex-like)
    brls::Application::getPlatform()->getThemeVariant();

    registerCustomViews();

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

int main(int argc, char* argv[]) {
    return VitaPlexMainEntry(argc, argv);
}
