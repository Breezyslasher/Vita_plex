/**
 * VitaPlex platform layer — Android implementation.
 *
 * Selected by CMake when -DPLATFORM_ANDROID=ON. Bundles APK asset
 * extraction and supplies the cover image budget for handheld-class
 * Android devices.
 */

#include "platform/platform.hpp"
#include "platform/android_assets.hpp"

#include <borealis.hpp>
#include "utils/http_client.hpp"

#include <cstdio>
#include <fstream>
#include <thread>
#include <jni.h>
#include <SDL2/SDL.h>

// Forward declaration — defined in src/main.cpp. SDL2's Android backend
// dispatches into SDL_main() instead of main(), so we have to provide the
// SDL_main symbol here and forward it to the shared entry point.
extern "C" int VitaPlexMainEntry(int argc, char* argv[]);

extern "C" int SDL_main(int argc, char* argv[]) {
    return VitaPlexMainEntry(argc, argv);
}

namespace vitaplex {
namespace platform {

const ImageConstraints& getImageConstraints() {
    // Android LANDSCAPE (TV box, tablet landscape, phone rotated):
    // 5 cols × 160px (+14px spacing) fits inside borealis's 1280-wide
    // virtual viewport after the sidebar, and keeps GPU texture pressure
    // low on mobile GPUs. Previous 6-column 200px layout overflowed and
    // the last poster of each row was clipped.
    static const ImageConstraints landscape = {
        /* posterWidth        */ 160,
        /* posterHeight       */ 240,
        /* squareCoverSize    */ 160,
        /* landscapeWidth     */ 220,
        /* landscapeHeight    */ 125,
        /* gridColumns        */   5,
        /* gridCellSpacing    */  14,
        /* titleFontSize      */  15,
        /* subtitleFontSize   */  12,
        /* descriptionFontSize*/  11,
        /* homeTitleFontSize  */  28,
        /* homeSectionFontSize*/  20,
        /* homeRowHeight      */ 290,
        /* landscapeRowHeight */ 185,
        /* squareRowHeight    */ 215,

        /* listRowHeight            */  60,
        /* livetvChannelCardWidth   */ 160,
        /* livetvChannelRowHeight   */ 130,
        /* livetvGuideHeight        */ 430,

        /* maxCellTitleChars        */  20,
        /* maxListTitleChars        */  90,
        /* maxLiveTVProgramChars    */  22,
        /* maxLiveTVChannelChars    */  18,

        /* sidebarMinWidth          */ 240,
        /* sidebarMaxWidth          */ 400,

        /* dialogWidth              */ 520,

        /* imageCacheSize           */  60,

        /* libraryPageSize          */ 1000,
        /* playlistTrackPageSize    */ 150,
        /* musicCarouselLimit       */ 100,

        /* posterRequestWidth       */ 320,
        /* posterRequestHeight      */ 480,
        /* squareRequestSize        */ 320,
        /* landscapeRequestWidth    */ 440,
        /* landscapeRequestHeight   */ 250,
        /* detailPosterRequestWidth */ 500,
        /* detailPosterRequestHeight*/ 750,
        /* photoRequestWidth        */ 1920,
        /* photoRequestHeight       */ 1080,
    };

    // Android PORTRAIT (phone). This is the dominant Android case. The
    // viewport is something like 720×1280 in virtual coords — narrower
    // than landscape so the same poster width that was 12% of the
    // landscape screen would be 22% in portrait. Re-tune from scratch:
    //
    //   - 3 columns of slightly larger covers fills the width without
    //     a giant void on either side
    //   - sidebar shrinks aggressively so the grid has room
    //   - LiveTV guide gets taller (we have the vertical real estate)
    //   - homeRowHeight grows to match the taller posters
    //   - dialogs / lists narrow to fit phone-width comfortably
    //   - text-truncation chars drop because narrower rows fit fewer
    static const ImageConstraints portrait = {
        /* posterWidth        */ 170,
        /* posterHeight       */ 255,
        /* squareCoverSize    */ 170,
        /* landscapeWidth     */ 220,
        /* landscapeHeight    */ 125,
        /* gridColumns        */   3,  // 5 -> 3 for narrow phone width
        /* gridCellSpacing    */  10,  // tighter than landscape's 14
        /* titleFontSize      */  15,
        /* subtitleFontSize   */  12,
        /* descriptionFontSize*/  12,
        /* homeTitleFontSize  */  28,
        /* homeSectionFontSize*/  20,
        /* homeRowHeight      */ 315,
        /* landscapeRowHeight */ 185,
        /* squareRowHeight    */ 230,

        /* listRowHeight            */  64,  // taller for finger taps
        /* livetvChannelCardWidth   */ 150,
        /* livetvChannelRowHeight   */ 130,
        /* livetvGuideHeight        */ 720,  // ~2x landscape — fill the height

        /* maxCellTitleChars        */  16,
        /* maxListTitleChars        */  60,
        /* maxLiveTVProgramChars    */  16,
        /* maxLiveTVChannelChars    */  14,

        /* sidebarMinWidth          */ 180,  // tighten so grid has room
        /* sidebarMaxWidth          */ 240,

        /* dialogWidth              */ 420,

        /* imageCacheSize           */  60,

        /* libraryPageSize          */ 1000,
        /* playlistTrackPageSize    */ 150,
        /* musicCarouselLimit       */ 100,

        /* posterRequestWidth       */ 340,
        /* posterRequestHeight      */ 510,
        /* squareRequestSize        */ 340,
        /* landscapeRequestWidth    */ 440,
        /* landscapeRequestHeight   */ 250,
        /* detailPosterRequestWidth */ 540,
        /* detailPosterRequestHeight*/ 810,
        /* photoRequestWidth        */ 1080,  // phone in portrait
        /* photoRequestHeight       */ 1920,
    };

    return isPortrait() ? portrait : landscape;
}

const VideoConstraints& getVideoConstraints() {
    // Most Android devices (phones, tablets, TV boxes) decode 1080p H.264
    // High@L5.1 in hardware via MediaCodec. Bitrate default is kept
    // conservative for mobile networks.
    static const VideoConstraints v = {
        /* plexPlatform     */ "Android",
        /* plexDevice       */ "Android TV",
        /* maxVideoWidth    */ 1920,
        /* maxVideoHeight   */ 1080,
        /* maxVideoLevel    */ 51,
        /* defaultBitrate   */ 8000,
        /* defaultResolution*/ "1920x1080",
        /* defaultVideoQualityIndex */ 1,  // QUALITY_1080P
        /* supportsHevc     */ true,  // Android devices decode HEVC in hardware
    };
    return v;
}

bool supports4KDecode() {
    // Asked of MediaCodec rather than assumed: the same port runs on 4K TV
    // boxes and on phones whose decoder stops at 1080p. Probed once — walking
    // the codec list is not free, and the answer can't change while we run.
    static const bool ok = []() -> bool {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return false;
        jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
        if (!cls) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }
        jmethodID mid = env->GetStaticMethodID(cls, "supports4KDecode", "()Z");
        if (!mid) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cls);
            return false;
        }
        jboolean res = env->CallStaticBooleanMethod(cls, mid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            res = JNI_FALSE;
        }
        env->DeleteLocalRef(cls);
        brls::Logger::info("Android 4K decode: {}", res == JNI_TRUE ? "yes" : "no");
        return res == JNI_TRUE;
    }();
    return ok;
}

void setPreferredRefreshRate(float contentFps) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(cls, "setPreferredRefreshRate", "(F)V");
    if (mid) {
        env->CallStaticVoidMethod(cls, mid, (jfloat)contentFps);
        if (env->ExceptionCheck()) env->ExceptionClear();
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
}

bool displaySupportsHdr() {
    // Probed once: the panel can't change under us, and this runs from mpv init.
    static const bool ok = []() -> bool {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return false;
        jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
        if (!cls) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }
        jmethodID mid = env->GetStaticMethodID(cls, "displaySupportsHdr", "()Z");
        if (!mid) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cls);
            return false;
        }
        jboolean res = env->CallStaticBooleanMethod(cls, mid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            res = JNI_FALSE;
        }
        env->DeleteLocalRef(cls);
        brls::Logger::info("Android display HDR: {}", res == JNI_TRUE ? "yes" : "no");
        return res == JNI_TRUE;
    }();
    return ok;
}

int passthroughCodecs() {
    // Probed once. The answer can change if the user replugs into a different
    // AVR, but mpv only reads it at init, so re-probing would not be acted on.
    static const int mask = []() -> int {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return 0;
        jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
        if (!cls) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return 0;
        }
        jmethodID mid = env->GetStaticMethodID(cls, "passthroughCodecs", "()I");
        if (!mid) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cls);
            return 0;
        }
        jint res = env->CallStaticIntMethod(cls, mid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            res = 0;
        }
        env->DeleteLocalRef(cls);
        return (int)res;
    }();
    return mask;
}

const CaptionStyle& getSystemCaptionStyle() {
    // Read once. Android delivers changes through a listener, but mpv takes
    // these at init, so a mid-playback change would not be acted on anyway.
    static const CaptionStyle style = []() -> CaptionStyle {
        CaptionStyle cs;
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return cs;
        jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
        if (!cls) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return cs;
        }
        jmethodID mid = env->GetStaticMethodID(cls, "captionStyle", "()[I");
        if (!mid) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cls);
            return cs;
        }
        jintArray arr = (jintArray)env->CallStaticObjectMethod(cls, mid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            arr = nullptr;
        }
        if (arr && env->GetArrayLength(arr) >= 6) {
            jint v[6];
            env->GetIntArrayRegion(arr, 0, 6, v);
            cs.valid = true;
            cs.fontScale = v[0] > 0 ? (float)v[0] / 1000.0f : 1.0f;
            cs.foreground = (unsigned)v[1];
            cs.background = (unsigned)v[2];
            cs.edgeColor = (unsigned)v[3];
            cs.edgeType = (int)v[4];
            cs.hasForeground = (v[5] & 1) != 0;
            cs.hasBackground = (v[5] & 2) != 0;
            cs.hasEdgeColor = (v[5] & 4) != 0;
            brls::Logger::info("Android captions: scale {:.2f} edge {} flags {}",
                               cs.fontScale, cs.edgeType, (int)v[5]);
        }
        if (arr) env->DeleteLocalRef(arr);
        env->DeleteLocalRef(cls);
        return cs;
    }();
    return style;
}

namespace {
std::function<void()> g_deepLinkHandler;
}

std::string takePendingDeepLink() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return {};
    jclass cls = env->FindClass("org/VitaPlex/app/VitaPlexActivity");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return {};
    }
    std::string out;
    jmethodID mid = env->GetStaticMethodID(cls, "takePendingDeepLink", "()Ljava/lang/String;");
    if (mid) {
        jstring js = (jstring)env->CallStaticObjectMethod(cls, mid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else if (js) {
            const char* raw = env->GetStringUTFChars(js, nullptr);
            if (raw) {
                out = raw;
                env->ReleaseStringUTFChars(js, raw);
            }
        }
        if (js) env->DeleteLocalRef(js);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    return out;
}

void setDeepLinkHandler(std::function<void()> onLinkArrived) {
    g_deepLinkHandler = std::move(onLinkArrived);
}

// Not in the header: only the JNI trampoline at the bottom of this file calls it.
void invokeDeepLinkHandler() {
    if (g_deepLinkHandler) g_deepLinkHandler();
}

bool init() {
    // Borealis on Android loads resources via fopen("resources/...") which
    // can't read APK assets directly, so extract them to internal storage
    // first. Must run BEFORE brls::Application::init().
    extractAndroidAssets();

    if (!::vitaplex::HttpClient::globalInit()) {
        brls::Logger::error("Failed to initialize curl");
        return false;
    }
    return true;
}

void shutdown() {
    ::vitaplex::HttpClient::globalCleanup();
}

std::string getLogPath() {
    return std::string{};
}

void openLogFile() {}
void closeLogFile() {}

bool readLocalFile(const std::string& path,
                   std::vector<uint8_t>& out,
                   std::size_t maxBytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto size = file.tellg();
    if (size <= 0 || (std::size_t)size > maxBytes) return false;

    file.seekg(0, std::ios::beg);
    out.resize((std::size_t)size);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good() || file.eof();
}

void launchThread(std::function<void()> task, std::size_t /*stackSize*/) {
    // Android's bionic std::thread default stack (~1 MB) is enough for
    // our background work; stackSize hint is ignored here.
    std::thread([t = std::move(task)]() { t(); }).detach();
}

std::size_t maxConcurrentNetworkRequests() {
    return 16;
}

bool needsHardExit() {
    return false;
}

[[noreturn]] void hardExit(int code) {
    std::exit(code);
}

}  // namespace platform
}  // namespace vitaplex

#ifdef __ANDROID__
// Java -> native: a link arrived while the app was already running, so there is
// a UI to open it with right now. onCreate's link takes the polled path instead.
extern "C" JNIEXPORT void JNICALL
Java_org_VitaPlex_app_VitaPlexActivity_nativeDeepLink(JNIEnv*, jclass) {
    brls::sync([]() {
        vitaplex::platform::invokeDeepLinkHandler();
    });
}
#endif
