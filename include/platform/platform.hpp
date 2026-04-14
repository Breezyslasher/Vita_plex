#pragma once

/**
 * VitaPlex platform abstraction layer.
 *
 * This header declares a small platform-agnostic interface. CMake selects
 * exactly one implementation file from src/platform/platform_<name>.cpp at
 * configure time (one per supported target: psv, ps4, switch, desktop,
 * android), so application code can call into the platform layer with
 * **zero #ifdefs**.
 *
 * The pattern mirrors how Vita_Suwayomi structures its platform code:
 *
 *   include/platform/platform.hpp     (interface — this file)
 *   src/platform/platform_psv.cpp     (PSV/Vita implementation)
 *   src/platform/platform_ps4.cpp     (PS4 implementation)
 *   src/platform/platform_switch.cpp  (Switch implementation)
 *   src/platform/platform_desktop.cpp (Linux/macOS/Windows implementation)
 *   src/platform/platform_android.cpp (Android implementation)
 *
 * Adding a new function here requires implementing it in every platform_*.cpp
 * file, which is enforced by the linker — so platforms cannot silently drift.
 */

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace vitaplex {
namespace platform {

/**
 * Per-platform image / grid sizing constraints.
 *
 * Cover image resolution must be tuned per device because:
 *   - PSV has a 960x544 screen and very limited VRAM, so it should request
 *     small thumbnails to avoid OOM and keep scrolling smooth.
 *   - Switch is 1280x720 docked, so medium thumbnails look crisp without
 *     wasting memory.
 *   - PS4 / Desktop / Android run at 1080p+ and benefit from much larger
 *     covers so posters do not look blurry.
 *
 * The struct is returned by getImageConstraints() and consumed by the
 * recycling grid and media item cell. NO platform code should hard-code
 * these numbers anywhere else.
 */
struct ImageConstraints {
    // Portrait poster (movies, TV shows, seasons)
    int posterWidth;
    int posterHeight;

    // Square cover (music albums, artists, playlists)
    int squareCoverSize;

    // Landscape still (TV episodes, clips/extras)
    int landscapeWidth;
    int landscapeHeight;

    // Recycling grid layout
    int gridColumns;        // how many cells per row
    int gridCellSpacing;    // pixels between cells

    // Cell typography (kept here so font sizes scale with cell size)
    int titleFontSize;
    int subtitleFontSize;
    int descriptionFontSize;
};

/**
 * Returns the image constraints for the current platform.
 * The reference points to a static instance — safe to keep across calls.
 */
const ImageConstraints& getImageConstraints();

/**
 * Bootstraps platform-specific subsystems before brls::Application::init().
 * Loads native modules, initializes networking / SSL / HTTP, sets clock
 * speeds, opens log files, etc. Returns false on a fatal failure.
 */
bool init();

/**
 * Releases platform-specific resources. Called once near program exit
 * after brls shutdown. Safe to call even if init() failed.
 */
void shutdown();

/**
 * Path to the on-disk log file the app should write to, or empty string
 * if logging should remain on stdout / brls::Logger only.
 *
 * On platforms whose stdout is invisible (PSV, PS4) this returns a real
 * file path so callers can redirect log output. On Switch/Desktop/Android
 * it returns an empty string.
 */
std::string getLogPath();

/**
 * Opens the platform log file (if any) and subscribes brls::Logger to it.
 * Idempotent. Called from init() but exposed for tests.
 */
void openLogFile();

/**
 * Closes the platform log file (if open). Called from shutdown().
 */
void closeLogFile();

/**
 * Reads a local file into `out`, capping at `maxBytes`.
 * Returns true on success. Used by ImageLoader to load downloaded covers
 * from disk without sprinkling sceIoOpen ifdefs across the UI layer.
 */
bool readLocalFile(const std::string& path,
                   std::vector<uint8_t>& out,
                   std::size_t maxBytes);

/**
 * Whether the platform exits the process via an SDK-specific call instead
 * of a normal `return` from main(). True on PSV (sceKernelExitProcess).
 */
bool needsHardExit();

/**
 * Hard-exits the process with the given exit code if needsHardExit() is true.
 * No-op otherwise. Lets main.cpp call platform::hardExit(1) without ifdefs.
 */
[[noreturn]] void hardExit(int code);

}  // namespace platform
}  // namespace vitaplex
