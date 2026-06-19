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
#include <functional>

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

    // Home tab typography / layout. The row height must be at least
    // posterHeight + ~45 so portrait covers aren't clipped top/bottom.
    int homeTitleFontSize;     // big "Home" header
    int homeSectionFontSize;   // "Recently Added …" section headings
    int homeRowHeight;         // carousel row height (clamps tall posters)

    // Detail-view horizontal carousels. Each row must clear its cell's
    // intrinsic height plus ~30-50px for the label/margins, otherwise the
    // cover tops/bottoms get clipped on taller platforms. `homeRowHeight`
    // is reused for portrait poster rows (e.g. seasons on a show page).
    int landscapeRowHeight;    // episode / extras carousel (landscape stills)
    int squareRowHeight;       // music album / track carousel (square covers)

    // List rows (downloads tab, track list) — pixels per row.
    int listRowHeight;

    // LiveTV tab layout.
    int livetvChannelCardWidth;     // horizontal EPG/channel card width
    int livetvChannelRowHeight;     // channel/DVR carousel height
    int livetvGuideHeight;          // EPG guide section height

    // Text truncation budgets — how many characters fit before ellipsis.
    // These are rough values derived from the typical font size and
    // available cell/label width on each platform; on Vita the 960px
    // screen can only show ~55 chars on a full row, while desktop 1920p
    // can comfortably show ~120.
    int maxCellTitleChars;          // poster/landscape cell title
    int maxListTitleChars;          // track / download list row title
    int maxLiveTVProgramChars;      // LiveTV program name under a channel card
    int maxLiveTVChannelChars;      // LiveTV channel name

    // Sidebar layout. Fraction of viewport width × 1000 so we can stay in
    // integer-land. minWidth / maxWidth are clamp limits for the dynamic
    // sidebar growth algorithm in main_activity.cpp.
    int sidebarMinWidth;
    int sidebarMaxWidth;

    // Modal / progress dialog width (centered). Fixed-pixel width; on
    // Vita ~420 is close to half the screen, on desktop we want ~560.
    int dialogWidth;

    // In-memory thumbnail cache slot count for image_loader. Vita has only
    // ~115 MB of usable heap so it caches 20; desktop/PS4/Android can afford
    // 60-100. Each slot holds one decoded cover image (~100-400 KB).
    int imageCacheSize;

    // Library pagination — how many items to request from Plex per page
    // when browsing a library section. Vita's tight RAM/VRAM means it has
    // to paginate aggressively (~60 per page); desktop and PS4 can load
    // hundreds at a time so the "load more" button is effectively a
    // fallback rather than the common case.
    int libraryPageSize;
    // Playlist track rendering batch size (how many rows are inflated per
    // incremental render pass). Desktop can inflate many more per tick.
    int playlistTrackPageSize;
    // Music carousel item cap — how many albums are shown in the Music
    // tab's "Recently Added" / "By Artist" carousels before truncation.
    int musicCarouselLimit;

    // Thumbnail resolutions REQUESTED from the Plex server (photo/:/transcode).
    // Larger values mean sharper covers but more bandwidth + VRAM per image.
    // Vita should request small thumbnails (~200x300 portrait) so they don't
    // blow the image cache; desktop should request ~400x600 so 1080p posters
    // look sharp.
    int posterRequestWidth;         // grid-cell poster thumb width
    int posterRequestHeight;        // grid-cell poster thumb height
    int squareRequestSize;          // grid-cell square cover (music)
    int landscapeRequestWidth;      // grid-cell landscape thumb width
    int landscapeRequestHeight;     // grid-cell landscape thumb height
    int detailPosterRequestWidth;   // detail-view left poster
    int detailPosterRequestHeight;  // detail-view left poster (portrait)
    int photoRequestWidth;          // photo viewer / background art
    int photoRequestHeight;
};

/**
 * Returns the image constraints for the current platform AND current
 * viewport orientation. The reference points to one of two static
 * instances per platform — a landscape one and a portrait one — chosen
 * at call time by isPortrait(). Safe to keep across calls only as long
 * as the orientation doesn't change between use sites; if you cache the
 * reference and the user rotates, you'll be reading stale values.
 *
 * Most views consume this at construction time, which is fine for
 * single-orientation use. Views that need to track rotation should
 * override View::onWindowSizeChanged() and re-query.
 */
const ImageConstraints& getImageConstraints();

/**
 * Live viewport / orientation helpers.
 *
 * These pull from brls::Application::contentWidth / contentHeight, which
 * borealis updates whenever the underlying window resizes. They power
 * the responsive layout: getImageConstraints() switches between its
 * landscape and portrait tables based on isPortrait(), and individual
 * views can read viewportWidth() / viewportHeight() to compute layout
 * dimensions as a fraction of the screen rather than hard-coded pixels.
 *
 * Defined in a shared TU (platform_common.cpp) — same implementation on
 * every target, since borealis abstracts the windowing system already.
 */
bool  isPortrait();
float viewportWidth();
float viewportHeight();

/**
 * Subscribe to viewport-orientation changes. Cb fires whenever
 * isPortrait() flips (NOT on every resize tick — just the orientation
 * boundary), so consumers can re-apply layout without thrash. The
 * subscription survives for the lifetime of the process; callers
 * typically install once during view construction.
 *
 * Implementation registers a brls::Application::getWindowSizeChangedEvent()
 * listener internally and de-bounces orientation flips itself.
 */
void onOrientationChanged(std::function<void()> cb);

/**
 * Per-platform Plex transcode / identification constants.
 *
 * These replace the old PLEX_PLATFORM / PLEX_DEVICE / PLEX_MAX_VIDEO_*
 * #define ifdef chain in include/app/application.hpp. Callers that used
 * to read the macros directly now pull the relevant field from here.
 *
 * Strings are pointers to static string literals owned by the
 * platform_<name>.cpp file — safe to keep long-term.
 */
struct VideoConstraints {
    const char* plexPlatform;     // X-Plex-Platform, e.g. "Desktop"
    const char* plexDevice;       // X-Plex-Device, e.g. "PS4"
    int   maxVideoWidth;          // add-limitation upperBound width
    int   maxVideoHeight;         // add-limitation upperBound height
    int   maxVideoLevel;          // H.264 level cap (e.g. 40, 42, 51)
    int   defaultBitrate;         // kbps when settings.maxBitrate == 0
    const char* defaultResolution;// e.g. "1920x1080"
    // Default enum value (int-cast to avoid including application.hpp)
    // matching vitaplex::VideoQuality. 3=480P, 2=720P, 1=1080P.
    int   defaultVideoQualityIndex;
};

/**
 * Returns the Plex transcode / identification constants for the current
 * platform. Points to a static instance owned by the platform layer.
 */
const VideoConstraints& getVideoConstraints();

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
 * Launch a detached background thread with a platform-appropriate stack
 * size and proper TLS / kernel-bookkeeping setup. Always prefer this over
 * bare `std::thread(...).detach()`.
 *
 *   Switch: libnx's newlib std::thread shim doesn't always register the
 *           stack region with the kernel OR initialize TLS — a thread
 *           launched that way crashes with an Instruction Abort the first
 *           time it indirect-calls through a vtable or std::function. This
 *           routes through pthread_create with explicit attr so the stack
 *           lands in a kernel-managed region and TLS is set up.
 *   PSV:   stdc++ std::thread defaults to a 256 KB stack which overflows
 *           on HLS downloads with deep call stacks; use pthread + attr.
 *   PS4:   same pthread route.
 *   Desktop / Android: std::thread().detach() is fine, but go through the
 *           same entry point so call sites don't have to ifdef.
 *
 * The task is heap-copied; ownership transfers to the new thread, which
 * frees it after the body returns. `stackSize` is a hint — platforms with
 * a fixed thread stack size honor it; std::thread platforms ignore it.
 */
void launchThread(std::function<void()> task,
                  std::size_t stackSize = 512 * 1024);

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
