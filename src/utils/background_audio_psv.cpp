/**
 * VitaPlex - background audio experiment, Vita backend
 *
 * See utils/background_audio.hpp for what is being tried and why. This file is
 * instrumentation as much as it is a feature: every call into the system is
 * logged under "[bgaudio]" with its return code, and a watcher thread records
 * whether this process keeps running once the user leaves it, so one test run
 * on a console answers the question from the log.
 */

#include "utils/background_audio.hpp"

#include "app/downloads_manager.hpp"
#include "app/music_controller.hpp"
#include "app/music_queue.hpp"
#include "app/plex_client.hpp"
#include "platform/paths.hpp"
#include "platform/platform.hpp"
#include "player/mpv_player.hpp"

#include <borealis.hpp>

#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "ShellAudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Not declared by this SDK's headers. The stubs are all in libSceAppMgr_stub,
// which the app links already. psp2/appmgr.h is left out on purpose, so a later
// SDK declaring these with its own struct types cannot clash with them here.
extern "C" {
int sceAppMgrReceiveEventNum(int* eventNum);
int sceAppMgrReceiveEvent(void* event);
int sceAppMgrAcquireBgmPortWithPriority(int priority);
int sceAppMgrReleaseBgmPort(void);
// patches/psv_platform.cpp: how many times the UI loop has run.
unsigned vitaplex_main_loop_ticks(void);
}

namespace vitaplex {
namespace bgaudio {

namespace {

// SceAppMgrEvent as firmware 3.60 fills it in.
struct AppEvent {
    int event;
    int appId;
    char param[56];
};
static_assert(sizeof(AppEvent) == 0x40, "SceAppMgrEvent is 0x40 bytes");

constexpr int kEventActivate    = 0x10000001;
constexpr int kEventDeactivate  = 0x10000002;
constexpr int kEventResume      = 0x10000003;
constexpr int kEventRequestQuit = 0x20000001;

// The priorities ElevenMPV-A asks for: 0x81 while it decodes in its own
// process, 0x80 while SceShell does the decoding.
constexpr int kPortForApp   = 0x81;
constexpr int kPortForShell = 0x80;

constexpr SceUInt kTickUs      = 250 * 1000;
constexpr int     kBeatTicks   = 16;           // a heartbeat line every 4 s away
constexpr SceInt64 kGapUs      = 3 * 1000000;  // a tick this late was not scheduled

// Formats SceShell decodes, as ElevenMPV-A hands them to it.
const char* const kShellExts[] = {"mp3", "m4a", "aac", "wav", "at9"};

std::string hex(int v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", (unsigned)v);
    return b;
}

std::string mmss(unsigned ms) {
    char b[16];
    std::snprintf(b, sizeof(b), "%u:%02u", ms / 60000, (ms / 1000) % 60);
    return b;
}

const char* stateName(MpvPlayerState s) {
    switch (s) {
        case MpvPlayerState::IDLE:      return "IDLE";
        case MpvPlayerState::LOADING:   return "LOADING";
        case MpvPlayerState::PLAYING:   return "PLAYING";
        case MpvPlayerState::PAUSED:    return "PAUSED";
        case MpvPlayerState::BUFFERING: return "BUFFERING";
        case MpvPlayerState::ENDED:     return "ENDED";
        default:                        return "ERROR";
    }
}

std::string lowerExt(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool shellDecodes(const std::string& ext) {
    for (const char* e : kShellExts)
        if (ext == e) return true;
    return false;
}

bool fileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/:");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::atomic<bool> g_keepPlaying{false};

// ── The BGM port ─────────────────────────────────────────────────────────
// Held at one priority at a time: the app's while mpv plays, the shell's
// while a system player test runs. 0 = not held.
std::mutex g_portMutex;
int g_portPriority = 0;

void holdPort(int priority, const char* why) {
    std::lock_guard<std::mutex> lock(g_portMutex);
    if (g_portPriority == priority) return;
    if (g_portPriority != 0) {
        const int rc = sceAppMgrReleaseBgmPort();
        brls::Logger::info("[bgaudio] BGM port: release {:#x} to change priority: {}",
                           g_portPriority, hex(rc));
    }
    const int rc = sceAppMgrAcquireBgmPortWithPriority(priority);
    brls::Logger::info("[bgaudio] BGM port: acquire at {:#x} for {}: {}", priority, why, hex(rc));
    g_portPriority = rc >= 0 ? priority : 0;
}

// Releases only if held at `priority`, or at any priority when it is 0.
void releasePort(int priority, const char* why) {
    std::lock_guard<std::mutex> lock(g_portMutex);
    if (g_portPriority == 0 || (priority != 0 && g_portPriority != priority)) return;
    const int rc = sceAppMgrReleaseBgmPort();
    brls::Logger::info("[bgaudio] BGM port: release {:#x} ({}): {}", g_portPriority, why, hex(rc));
    g_portPriority = 0;
}

int portPriority() {
    std::lock_guard<std::mutex> lock(g_portMutex);
    return g_portPriority;
}

// ── SceShell ─────────────────────────────────────────────────────────────
// One conversation with the shell at a time. g_shellBusy covers a whole test
// run so a second tap is turned away rather than queued behind the first.
std::mutex g_shellMutex;
std::atomic<bool> g_shellBusy{false};
bool g_shellInit = false;     // guarded by g_shellMutex
bool g_shellPlaying = false;  // guarded by g_shellMutex; a test left it playing
int g_shellService = -1;      // guarded by g_shellMutex; the one g_shellInit refers to

// Which service the next test uses: 0-4 the music player's client types,
// kServiceAppBgm the application-BGM service. See the SceShell tests below.
constexpr int kServiceAppBgm = 5;
std::atomic<int> g_service{0};
// Guarded by g_shellMutex: the last source seen playing, one for the music
// player (all five client types share its one loaded file) and one for the
// application-BGM service, which remembers its own.
std::string g_shellLastSrc[2];
int lastSrcSlot(int service) { return service == kServiceAppBgm ? 1 : 0; }

// ── The watcher ──────────────────────────────────────────────────────────
std::atomic<bool> g_watcherStarted{false};
std::atomic<int> g_loads{0};

// Only the watcher thread touches these.
bool     g_away = false;
SceInt64 g_awayAt = 0;
int      g_awayTicks = 0;
SceInt64 g_awayLongestGapUs = 0;
unsigned g_awayUiTicks = 0;
bool     g_awayPlaying = false;
double   g_awayPos = 0.0;
int      g_awayLoads = 0;

std::string describeMpv(const MpvPlayer::Snapshot& s) {
    if (!s.initialized) return "mpv not running";
    char b[96];
    std::snprintf(b, sizeof(b), "mpv %s at %.1fs, output %d Hz%s", stateName(s.state), s.position,
                  s.outSampleRate, s.audioOnly ? "" : " (video mode)");
    return b;
}

// ", system player …" for the watcher's lines, or empty. Defined with the
// SceShell tests below; never waits for the shell lock.
std::string shellStatusLine();

// gapUs is how late the tick that read the event was. If the system froze the
// process the moment the user left, the event is only read on the way back,
// with the whole absence in that gap, so the departure is dated back to it.
void onDeactivate(SceInt64 now, SceInt64 gapUs) {
    const bool heldFirst = gapUs > kGapUs;
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    g_away = true;
    g_awayAt = heldFirst ? now - gapUs : now;
    g_awayTicks = 0;
    g_awayLongestGapUs = heldFirst ? gapUs : 0;
    g_awayUiTicks = vitaplex_main_loop_ticks();
    g_awayPlaying = s.initialized && (s.state == MpvPlayerState::PLAYING ||
                                      s.state == MpvPlayerState::BUFFERING);
    g_awayPos = s.position;
    g_awayLoads = g_loads.load();
    brls::Logger::info("[bgaudio] deactivated (left for the LiveArea or another app){}: {}, "
                       "BGM port {:#x}, keep playing {}{}",
                       heldFirst ? ", read only after the process was held" : "",
                       describeMpv(s), portPriority(), g_keepPlaying.load(), shellStatusLine());
}

void onActivate(SceInt64 now) {
    if (!g_away) {
        brls::Logger::info("[bgaudio] activated");
        return;
    }
    g_away = false;
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    const double awaySec = (double)(now - g_awayAt) / 1e6;
    const double ranSec = g_awayTicks * (kTickUs / 1e6);
    const int ranPct = awaySec > 0.5 ? (int)std::min(100.0, 100.0 * ranSec / awaySec) : 100;
    const unsigned uiTicks = vitaplex_main_loop_ticks() - g_awayUiTicks;
    const int loads = g_loads.load() - g_awayLoads;
    const double moved = s.position - g_awayPos;

    brls::Logger::info("[bgaudio] activated after {:.1f}s away: watcher ran {:.1f}s of it ({}%), "
                       "longest stall {:.1f}s, UI loop ran {} times, {} new file(s) loaded, "
                       "{}, moved {:+.1f}s{}",
                       awaySec, ranSec, ranPct, g_awayLongestGapUs / 1e6, uiTicks, loads,
                       describeMpv(s), moved, shellStatusLine());

    // The same verdict on screen, so a test does not need the log to be read.
    char music[80];
    if (!g_awayPlaying)
        std::snprintf(music, sizeof(music), "no music was playing when you left");
    else if (loads > 0)
        std::snprintf(music, sizeof(music), "the queue moved on %d track(s)", loads);
    else
        std::snprintf(music, sizeof(music), "the track moved %.0fs", moved);
    // By rate, not by any tick at all: a UI that stalls just after leaving
    // still gets a few frames in first. Running, it does 30 to 60 a second.
    const bool uiRan = uiTicks >= 2.0 * awaySec;
    char b[200];
    if (ranPct >= 90) {
        std::snprintf(b, sizeof(b), "Away %d:%02d. VitaPlex kept running (UI %s); %s",
                      (int)awaySec / 60, (int)awaySec % 60, uiRan ? "too" : "stopped",
                      music);
    } else {
        std::snprintf(b, sizeof(b), "Away %d:%02d. VitaPlex was stopped for %d%% of it; %s",
                      (int)awaySec / 60, (int)awaySec % 60, 100 - ranPct, music);
    }
    const std::string msg = b;
    brls::sync([msg]() { brls::Application::notify(msg); });
}

void heartbeat(SceInt64 now) {
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    brls::Logger::info("[bgaudio] away {:.0f}s: {}, UI loop +{}, {} new file(s){}",
                       (now - g_awayAt) / 1e6, describeMpv(s),
                       vitaplex_main_loop_ticks() - g_awayUiTicks, g_loads.load() - g_awayLoads,
                       shellStatusLine());
}

void stopShellLocked(const char* why);

void onRequestQuit() {
    brls::Logger::info("[bgaudio] the system asked the app to quit");
    // A file the system player was given would otherwise play on after the app
    // is gone, with nothing left to stop it. Not waited for if a test is mid-run
    // (see shellStatusLine); the process is going either way.
    {
        std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
        if (lock.owns_lock()) stopShellLocked("app quitting");
    }
    releasePort(0, "app quitting");
}

void drainAppEvents(SceInt64 now, SceInt64 gapUs) {
    int n = 0;
    const int rc = sceAppMgrReceiveEventNum(&n);
    if (rc < 0 || n <= 0) return;
    for (int i = 0; i < n; i++) {
        AppEvent ev;
        std::memset(&ev, 0, sizeof(ev));
        if (sceAppMgrReceiveEvent(&ev) < 0) break;
        switch (ev.event) {
            case kEventDeactivate:  onDeactivate(now, gapUs); break;
            case kEventActivate:    onActivate(now); break;
            case kEventResume:      brls::Logger::info("[bgaudio] app event: resume"); break;
            case kEventRequestQuit: onRequestQuit(); break;
            default:
                brls::Logger::info("[bgaudio] app event {}", hex(ev.event));
                break;
        }
    }
}

int powerCallback(int, int, int powerInfo, void*) {
    std::string what;
    const struct { unsigned bit; const char* name; } kBits[] = {
        {SCE_POWER_CB_APP_SUSPEND, "app suspend"},
        {SCE_POWER_CB_APP_RESUME, "app resume"},
        {SCE_POWER_CB_APP_RESUMING, "app resuming"},
        {SCE_POWER_CB_SYSTEM_SUSPEND, "system suspend"},
        {SCE_POWER_CB_SYSTEM_RESUME, "system resume"},
        {SCE_POWER_CB_BUTTON_PS_PRESS, "PS button"},
        {SCE_POWER_CB_BUTTON_POWER_PRESS, "power button"},
    };
    for (const auto& k : kBits) {
        if ((unsigned)powerInfo & k.bit) {
            if (!what.empty()) what += ", ";
            what += k.name;
        }
    }
    brls::Logger::info("[bgaudio] power callback {}{}{}", hex(powerInfo), what.empty() ? "" : ": ",
                       what);
    return 0;
}

int watcherMain(SceSize, void*) {
    const SceUID cb = sceKernelCreateCallback("VitaPlexBgAudioPower", 0, powerCallback, nullptr);
    const int rcCb = cb >= 0 ? scePowerRegisterCallback(cb) : cb;
    brls::Logger::info("[bgaudio] watcher running; power callback {}", hex(rcCb));

    SceInt64 last = sceKernelGetSystemTimeWide();
    for (;;) {
        // The CB variant is what runs the power callback on this thread.
        sceKernelDelayThreadCB(kTickUs);
        const SceInt64 now = sceKernelGetSystemTimeWide();
        const SceInt64 gap = now - last;
        last = now;
        if (gap > kGapUs) {
            // Nothing in this loop waits for long, so a tick this late means the
            // thread was not scheduled at all: the process was held, or the
            // console slept.
            brls::Logger::info("[bgaudio] watcher did not run for {:.1f}s", gap / 1e6);
            if (g_away && gap > g_awayLongestGapUs) g_awayLongestGapUs = gap;
        }
        drainAppEvents(now, gap);
        if (g_away) {
            g_awayTicks++;
            if (g_awayTicks % kBeatTicks == 0) heartbeat(now);
        }
    }
    return 0;
}

void startWatcher() {
    if (g_watcherStarted.exchange(true)) return;
    const SceUID thid = sceKernelCreateThread("VitaPlexBgAudio", watcherMain, 0x10000100, 0x10000,
                                              0, 0, nullptr);
    const int rc = thid >= 0 ? sceKernelStartThread(thid, 0, nullptr) : thid;
    if (rc < 0) {
        brls::Logger::error("[bgaudio] could not start the watcher: {}", hex(rc));
        g_watcherStarted = false;
    }
}

// ── SceShell tests ───────────────────────────────────────────────────────
//
// The shell has two services that play a file handed to it. The music
// player service takes a client type when it starts (libShellAudio maps 0-4
// to 0x8-0x80; ElevenMPV-A always uses 0), and which type the system's own
// quick menu controls answer to is unknown. The application-BGM service
// (sceMusicInternalApp*) is a different interface, whose open call is named
// SetUri. Which of these, if any, lights up the quick menu's Music controls,
// and which takes a URL, is what the picker in settings is for.

struct ShellTry {
    bool played = false;  // its clock was seen moving
    bool maybe = false;   // accepted, but this service reports no clock
    std::string line;
};

std::string serviceName(int s) {
    return s == kServiceAppBgm ? std::string("app background music")
                               : "music player, type " + std::to_string(s);
}

// All of these must hold g_shellMutex, and act on whichever service
// g_shellService says is running.
bool appBgm() { return g_shellService == kServiceAppBgm; }

int shellCommand(int eventId) {
    return appBgm() ? sceMusicInternalAppSetPlaybackCommand(eventId, 0)
                    : sceMusicPlayerServiceSendEvent(eventId, 0);
}

struct ShellClock {
    int rc = 0;
    int state = 0;
    unsigned timeMs = 0;
};

ShellClock shellClock() {
    ShellClock c;
    if (appBgm()) {
        SceMusicInternalAppResult res;
        std::memset(&res, 0, sizeof(res));
        c.rc = sceMusicInternalAppGetLastResult(&res);
        c.state = res.state;
        c.timeMs = res.time > 0 ? (unsigned)res.time : 0;
    } else {
        SceMusicPlayerServicePlayStatusExtension st;
        std::memset(&st, 0, sizeof(st));
        c.rc = sceMusicPlayerServiceGetPlayStatusExtension(&st);
        c.state = st.currentState;
        c.timeMs = st.currentTime;
    }
    return c;
}

// The music player service's loaded track length in ms, logged with its
// title. Must hold g_shellMutex, with the music player service running.
int playerDuration(const std::string& label) {
    std::unique_ptr<SceMusicPlayerServiceTrackInfo> info(new SceMusicPlayerServiceTrackInfo());
    const int rc = sceMusicPlayerServiceGetTrackInfo(info.get());
    info->title[sizeof(info->title) - 1] = '\0';
    brls::Logger::info("[bgaudio] system player: {}: track info {}, duration {} ms, title \"{}\"",
                       label, hex(rc), info->duration, info->title);
    return rc < 0 ? -1 : info->duration;
}

// A test holds the shell lock for most of a minute, and a watcher stuck
// waiting for it would read as the process having been stopped.
std::string shellStatusLine() {
    std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
    if (!lock.owns_lock()) return ", system player test running";
    if (!g_shellPlaying) return {};
    const ShellClock c = shellClock();
    char b[80];
    std::snprintf(b, sizeof(b), ", system player state %d at %s (%s)", c.state,
                  mmss(c.timeMs).c_str(), hex(c.rc).c_str());
    return b;
}

// Must hold g_shellMutex.
void stopShellLocked(const char* why) {
    if (!g_shellInit) return;
    const int rcStop = shellCommand(SCE_MUSIC_EVENTID_STOP);
    const int rcTerm = appBgm() ? sceMusicInternalAppTerminate() : sceMusicPlayerServiceTerminate();
    brls::Logger::info("[bgaudio] system player ({}): stop {}, terminate {} ({})",
                       serviceName(g_shellService), hex(rcStop), hex(rcTerm), why);
    g_shellInit = false;
    g_shellService = -1;
    g_shellPlaying = false;
}

// Hands src to the shell's music player and watches whether its clock moves.
// That, not the state it reports, is what counts as playing. Left playing
// when it plays and `leave` is set; stopped otherwise. Must hold g_shellMutex.
ShellTry shellTry(const std::string& label, const std::string& src, bool leave) {
    ShellTry r;
    // The shell takes at most 0x400 characters and would cut a longer one.
    if (src.size() > 0x400) {
        r.line = label + ": " + std::to_string(src.size()) + " characters, more than the shell takes";
        brls::Logger::info("[bgaudio] system player: {}", r.line);
        return r;
    }
    std::vector<char> path(src.begin(), src.end());
    path.push_back('\0');

    // One service at a time: libShellAudio keeps a single session for both.
    const int service = g_service.load();
    if (g_shellInit && g_shellService != service) stopShellLocked("switching service");
    if (!g_shellInit) {
        const int rc = service == kServiceAppBgm ? sceMusicInternalAppInitialize(1)
                                                 : sceMusicPlayerServiceInitialize(service);
        brls::Logger::info("[bgaudio] system player ({}): initialize: {}", serviceName(service),
                           hex(rc));
        if (rc < 0) {
            r.line = label + ": the " + serviceName(service) + " service would not start (" +
                     hex(rc) + ")";
            return r;
        }
        g_shellInit = true;
        g_shellService = service;
    }

    // What was loaded before this. The shell keeps its last file through stops
    // and even through terminate and initialize, and when it cannot open a new
    // source it says nothing and plays the old one, so "the clock moved" is not
    // enough to count as playing: the new source has to be shown to have
    // replaced the old. See the verdict below.
    const ShellClock before = shellClock();
    const int durBefore = appBgm() ? -1 : playerDuration(label + " (before)");

    // The application-BGM service refuses commands before its first SetUri,
    // and every attempt here ends stopped, so it needs no stop first.
    const int rcStop = appBgm() ? 0 : shellCommand(SCE_MUSIC_EVENTID_STOP);
    int rcOpen;
    if (appBgm()) {
        SceMusicOpt opt;
        std::memset(&opt, 0, sizeof(opt));
        rcOpen = sceMusicInternalAppSetUri(path.data(), &opt);
    } else {
        rcOpen = sceMusicPlayerServiceOpen(path.data(), nullptr);
    }
    const int rcPlay = shellCommand(SCE_MUSIC_EVENTID_PLAY);
    brls::Logger::info("[bgaudio] system player ({}): {} ({}): stop {}, open {}, play {}",
                       serviceName(service), label, src, hex(rcStop), hex(rcOpen), hex(rcPlay));
    if (rcOpen < 0) {
        r.line = label + ": refused (" + hex(rcOpen) + ")";
        return r;
    }

    // Up to 12 s for the clock to pass a second beyond where it was first
    // seen moving: a stream has to be fetched and buffered before it starts.
    ShellClock c;
    int lastState = -1;
    bool seenTime = false;
    unsigned firstTime = 0;
    const SceInt64 t0 = sceKernelGetSystemTimeWide();
    while (sceKernelGetSystemTimeWide() - t0 < 12 * 1000000LL) {
        c = shellClock();
        if (c.state != lastState) {
            brls::Logger::info("[bgaudio] system player: {}: state {} at {} ({})", label, c.state,
                               mmss(c.timeMs), hex(c.rc));
            lastState = c.state;
        }
        if (c.timeMs > 0 && !seenTime) {
            seenTime = true;
            firstTime = c.timeMs;
        }
        if (seenTime && c.timeMs >= firstTime + 1000) {
            r.played = true;
            break;
        }
        sceKernelDelayThread(250 * 1000);
    }

    int durAfter = -1;
    if (appBgm()) {
        // Layout unknown; logged whole in case it holds the clock after all.
        unsigned char status[0x40];
        std::memset(status, 0, sizeof(status));
        const int rcStatus = sceMusicInternalAppGetPlaybackStatus(status);
        std::string bytes;
        for (unsigned char b : status) {
            char h[4];
            std::snprintf(h, sizeof(h), "%02X", b);
            bytes += h;
        }
        brls::Logger::info("[bgaudio] system player: {}: playback status {}: {}", label,
                           hex(rcStatus), bytes);
    } else {
        durAfter = playerDuration(label);
    }

    // Did the new source replace the old one? A different length says so, as
    // does the clock starting again from the top, and with nothing loaded
    // before, whatever plays can only be the new source. The same source
    // opened again needs none of that. The application-BGM service gives no
    // length, so for it only the clock can tell.
    const bool sameSource = src == g_shellLastSrc[lastSrcSlot(service)];
    const bool clockWentBack = before.timeMs >= 3000 && firstTime + 2000 < before.timeMs;
    const bool nothingBefore = before.timeMs == 0 && durBefore <= 0;
    const bool replaced = sameSource || clockWentBack || nothingBefore ||
                          (!appBgm() && durAfter != durBefore);

    if (r.played && !replaced) {
        r.played = false;
        r.line = label + ": not opened. The system player went on with the file it had "
                 "before (" + mmss(firstTime) + " into it)";
    } else if (r.played) {
        r.line = label + ": plays (" + mmss(firstTime) + " -> " + mmss(c.timeMs) + ")";
    } else if (seenTime) {
        r.line = label + ": started, then its clock stopped at " + mmss(c.timeMs);
    } else if (appBgm() && rcPlay >= 0) {
        // This service may simply not report a clock, so no verdict from here.
        r.maybe = true;
        r.line = label + ": accepted, but this service reports no progress; listen for it";
    } else {
        r.line = label + ": no sound after 12s (state " + std::to_string(c.state) + ")";
    }
    brls::Logger::info("[bgaudio] system player: {}", r.line);

    if (r.played || r.maybe) g_shellLastSrc[lastSrcSlot(service)] = src;
    if ((r.played || r.maybe) && leave) {
        g_shellPlaying = true;
    } else {
        shellCommand(SCE_MUSIC_EVENTID_STOP);
        g_shellPlaying = false;
    }
    return r;
}

// The system player and mpv would otherwise both be heard.
void pauseMpvForShell() {
    if (MpvPlayer::getInstance().isPlaying()) MusicController::getInstance().playPause(false);
}

// What to do with something a test left playing. `heard` is whether its clock
// was seen moving; the application-BGM service may not report one.
std::string listenAdvice(bool heard) {
    return std::string(heard ? "Playing now. " : "It may be playing: if you hear it, it works. ") +
           "Press PS and listen: if it keeps going outside VitaPlex, this works in the "
           "background. While it plays, hold PS: do the quick menu's Music controls work "
           "for it? \"Stop System Player Test\" ends it.";
}

void finishTest(const Report& done, const std::string& report) {
    g_shellBusy = false;
    brls::sync([done, report]() { done(report); });
}

}  // namespace

void init(bool keepPlaying) {
    g_keepPlaying = keepPlaying;
    brls::Logger::info("[bgaudio] keep playing in background: {}", keepPlaying);
    if (keepPlaying) startWatcher();
}

void setKeepPlaying(bool on) {
    g_keepPlaying = on;
    brls::Logger::info("[bgaudio] keep playing in background set to {}", on);
    if (on) {
        startWatcher();
        if (MpvPlayer::getInstance().isPlaying()) holdPort(kPortForApp, "mpv, already playing");
    } else {
        releasePort(kPortForApp, "setting turned off");
    }
}

bool keepPlaying() { return g_keepPlaying; }

void onAudioPlayback(bool playing) {
    // Held from the first play and through pauses, like ElevenMPV-A; it goes
    // when the player does, or when the setting is turned off.
    if (playing && g_keepPlaying) holdPort(kPortForApp, "mpv playback");
}

void onTrackLoad() { g_loads++; }

void onPlayerShutdown() { releasePort(kPortForApp, "player shut down"); }

void runShellFileTest(Report done) {
    if (g_shellBusy.exchange(true)) {
        done("A system player test is already running.");
        return;
    }
    startWatcher();

    // A test.* file the user placed takes precedence, then any download in a
    // format the shell decodes. Picked here, on the UI thread, where the
    // downloads list is normally read.
    std::string path;
    for (const char* e : kShellExts) {
        const std::string p = platformPath(std::string("test.") + e);
        if (fileExists(p)) {
            path = p;
            break;
        }
    }
    if (path.empty()) {
        for (const auto& d : DownloadsManager::getInstance().getDownloads()) {
            if (d.state == DownloadState::COMPLETED && d.mediaType == "track" &&
                shellDecodes(lowerExt(d.localPath)) && fileExists(d.localPath)) {
                path = d.localPath;
                break;
            }
        }
    }
    if (path.empty()) {
        g_shellBusy = false;
        done("No file to try. Download a track that is MP3, M4A, AAC or WAV, or put a "
             "test.mp3 in " + platformPath("") + ", then run this again.");
        return;
    }

    pauseMpvForShell();
    const std::string service = serviceName(g_service.load());
    platform::launchThread([done, path, service]() {
        ShellTry r;
        {
            std::lock_guard<std::mutex> lock(g_shellMutex);
            holdPort(kPortForShell, "system player test");
            r = shellTry("File " + baseName(path), path, true);
        }
        std::string report = "System player (" + service + "), local file\n\n" + r.line + "\n\n";
        report += (r.played || r.maybe)
            ? listenAdvice(r.played)
            : "The system player did not play it. The log has every return code "
              "(Settings > Interface > View Log, lines with [bgaudio]).";
        finishTest(done, report);
    });
}

void runShellStreamTest(Report done) {
    if (g_shellBusy.exchange(true)) {
        done("A system player test is already running.");
        return;
    }
    const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
    if (!track || track->ratingKey.empty()) {
        g_shellBusy = false;
        done("Play a track first; this tries that track's Plex stream in the system player.");
        return;
    }
    startWatcher();
    pauseMpvForShell();

    const std::string ratingKey = track->ratingKey;
    const std::string title = track->title;
    const std::string service = serviceName(g_service.load());
    platform::launchThread([done, ratingKey, title, service]() {
        PlexClient& plex = PlexClient::getInstance();
        std::vector<std::string> lines;

        // What VitaPlex itself plays on the Vita: Plex's mp3 transcode, a
        // stream made on the fly with no length and no range support.
        // A fresh session for every attempt, so the server never sees one
        // session asked for twice.
        auto transcodeUrl = [&]() {
            std::string url, session;
            if (!plex.getTranscodeUrlSpeculative(ratingKey, url, session)) return std::string();
            return url;
        };
        // The file as stored, when it is in a format the shell decodes: a
        // plain download with a length, the kindest case for a player.
        MediaItem item;
        const bool haveDetails = plex.fetchMediaDetails(ratingKey, item);
        std::string codec = item.audioCodec;
        for (auto& c : codec) c = (char)std::tolower((unsigned char)c);
        const bool fileDecodes = haveDetails && !item.partPath.empty() &&
                                 (codec == "mp3" || codec == "aac");
        const std::string fileUrl =
            fileDecodes ? plex.getServerUrl() + item.partPath + "?X-Plex-Token=" + plex.getAuthToken()
                        : std::string();

        struct Attempt {
            std::string label;
            bool transcode;
            bool http;
        };
        std::vector<Attempt> attempts;
        const bool https = plex.getServerUrl().rfind("https://", 0) == 0;
        attempts.push_back({https ? "MP3 stream, https" : "MP3 stream, http", true, false});
        if (https) attempts.push_back({"MP3 stream, http", true, true});
        if (fileDecodes) {
            attempts.push_back({https ? "Original " + codec + " file, https"
                                      : "Original " + codec + " file, http", false, false});
            if (https) attempts.push_back({"Original " + codec + " file, http", false, true});
        }

        auto urlFor = [&](const Attempt& a) {
            std::string url = a.transcode ? transcodeUrl() : fileUrl;
            if (a.http && url.rfind("https://", 0) == 0) url = "http://" + url.substr(8);
            return url;
        };

        // The first form whose clock moved, failing that the first the service
        // accepted without reporting a clock (application BGM may not).
        int firstPlayed = -1, firstMaybe = -1;
        ShellTry replay;
        {
            std::lock_guard<std::mutex> lock(g_shellMutex);
            holdPort(kPortForShell, "system player test");
            for (std::size_t i = 0; i < attempts.size(); i++) {
                const std::string url = urlFor(attempts[i]);
                if (url.empty()) {
                    lines.push_back(attempts[i].label + ": could not get a URL from the server");
                    continue;
                }
                const ShellTry r = shellTry(attempts[i].label, url, false);
                lines.push_back(r.line);
                if (r.played && firstPlayed < 0) firstPlayed = (int)i;
                if (r.maybe && firstMaybe < 0) firstMaybe = (int)i;
            }
            // Start the best form again and leave it going, so the user can
            // hear whether it carries on outside the app.
            const int best = firstPlayed >= 0 ? firstPlayed : firstMaybe;
            if (best >= 0) {
                const std::string url = urlFor(attempts[best]);
                if (!url.empty()) replay = shellTry(attempts[best].label, url, true);
            }
        }
        const int best = firstPlayed >= 0 ? firstPlayed : firstMaybe;
        const bool playingNow = replay.played || replay.maybe;
        if (!haveDetails) {
            lines.push_back("Original file: could not read the track's details");
        } else if (!fileDecodes) {
            lines.push_back("Original file: skipped, it is " +
                            (codec.empty() ? std::string("an unknown format") : codec) +
                            ", which the shell does not decode");
        }

        std::string report =
            "System player (" + service + "), streaming \"" + title + "\" from Plex\n\n";
        for (const auto& l : lines) report += l + "\n";
        report += "\n";
        if (playingNow) {
            report += attempts[best].label + ": " + listenAdvice(replay.played);
        } else if (best >= 0) {
            report += attempts[best].label +
                      " looked like it worked during the test but did not start again afterwards. "
                      "The log has the details (lines with [bgaudio]).";
        } else {
            report += "The system player did not play the stream in any form. The log has every "
                      "return code (lines with [bgaudio]).";
        }
        finishTest(done, report);
    });
}

int shellService() { return g_service; }

void setShellService(int service) {
    if (service < 0 || service > kServiceAppBgm) service = 0;
    g_service = service;
    brls::Logger::info("[bgaudio] system player tests will use {}", serviceName(service));
}

std::string stopShellTest() {
    std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
    if (!lock.owns_lock()) return "A test is still running; stop it once it has reported.";
    if (!g_shellInit) return "The system player is not playing anything from VitaPlex.";
    stopShellLocked("stopped from settings");
    lock.unlock();
    releasePort(kPortForShell, "system player test over");
    return "System player stopped.";
}

}  // namespace bgaudio
}  // namespace vitaplex
