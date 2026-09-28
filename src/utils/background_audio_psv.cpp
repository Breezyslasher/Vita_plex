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
#include "utils/http_client.hpp"

#include <borealis.hpp>

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ShellAudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

// "a", "a and b", "a, b and c".
std::string joinNames(const std::vector<std::string>& names) {
    std::string s;
    for (std::size_t i = 0; i < names.size(); i++) {
        if (i > 0) s += i + 1 == names.size() ? " and " : ", ";
        s += names[i];
    }
    return s;
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
    bool played = false;  // the new source was seen playing
    bool moved = false;   // the clock moved, whatever it was playing
    bool maybe = false;   // accepted, but this service reports no clock
    bool unsure = false;  // something played, but not known to be the new source
    std::string line;
};

// How long shellTry watches, and how far it lets a source play.
struct TryTiming {
    int maxMs = 12000;      // no clock by then: no sound
    int minMs = 0;          // watch at least this long, even once it plays
    unsigned playToMs = 0;  // once playing, let the clock reach this
};

std::string serviceName(int s) {
    return s == kServiceAppBgm ? std::string("app background music")
                               : "music player, type " + std::to_string(s);
}

std::string shortName(int s) {
    return s == kServiceAppBgm ? std::string("app BGM") : "type " + std::to_string(s);
}

// The BGM port as the system reports it (libShellAudio's
// shellAudioGetCurrentBGMState: owner, priority, state and two fields nobody
// has named). Only logged, next to each attempt: it does not depend on the
// service's own status, which the application-BGM service does not fill in.
std::string bgmState() {
    // 20 bytes by libShellAudio's reading; zeroed room for more in case the
    // system writes more, and anything it did write there is shown.
    int st[16];
    std::memset(st, 0, sizeof(st));
    const int rc = shellAudioGetCurrentBGMState(reinterpret_cast<SceShellAudioBGMState*>(st));
    char b[160];
    std::snprintf(b, sizeof(b), "%s: owner 0x%X, priority 0x%X, 0x%X, state 0x%X, 0x%X",
                  hex(rc).c_str(), (unsigned)st[0], (unsigned)st[1], (unsigned)st[2],
                  (unsigned)st[3], (unsigned)st[4]);
    std::string s = b;
    for (int i = 5; i < 16; i++) {
        if (st[i] == 0) continue;
        std::snprintf(b, sizeof(b), ", [%d] 0x%X", i, (unsigned)st[i]);
        s += b;
    }
    return s;
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
// when it plays and `leave` is set; stopped otherwise. `service` is one of the
// picker's six; switching between them is handled here. Must hold g_shellMutex.
ShellTry shellTry(const std::string& label, const std::string& src, bool leave, int service,
                  const TryTiming& t = TryTiming()) {
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

    // Up to t.maxMs (12 s by default) for the clock to pass a second beyond
    // where it was first seen moving: a stream has to be fetched and buffered
    // before it starts.
    ShellClock c;
    int lastState = -1;
    bool seenTime = false;
    unsigned firstTime = 0;
    const SceInt64 t0 = sceKernelGetSystemTimeWide();
    for (;;) {
        const SceInt64 elapsedMs = (sceKernelGetSystemTimeWide() - t0) / 1000;
        if (elapsedMs >= t.maxMs) break;
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
        if (seenTime && c.timeMs >= firstTime + 1000) r.moved = true;
        if (r.moved && c.timeMs >= t.playToMs && elapsedMs >= t.minMs) break;
        sceKernelDelayThread(250 * 1000);
    }
    r.played = r.moved;
    brls::Logger::info("[bgaudio] system player: {}: BGM port {}", label, bgmState());

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

    // Did the new source replace the old one? With the old one stopped 3 s or
    // more into it, the clock says: a new source starts again from the top.
    // Short of that, a different length says it did, as does there having
    // been nothing loaded, and the same source opened again needs none of
    // it. Otherwise it cannot be told from here, since a new source of the
    // same length would look like the old one going on. The application-BGM
    // service gives no length.
    const bool sameSource = src == g_shellLastSrc[lastSrcSlot(service)];
    const bool clockTells = before.timeMs >= 3000;
    const bool nothingBefore = before.timeMs == 0 && durBefore <= 0;
    const bool lengthChanged = !appBgm() && durAfter != durBefore;
    const bool replaced =
        sameSource || (clockTells ? firstTime + 2000 < before.timeMs : nothingBefore || lengthChanged);

    if (r.played && !replaced) {
        r.played = false;
        r.unsure = !clockTells;
        r.line = r.unsure
            ? label + ": something played, but neither the clock nor the length can tell "
                      "whether it was this or the file loaded before"
            : label + ": not opened. The system player went on with the file it had before (" +
                  mmss(firstTime) + " into it)";
    } else if (r.played) {
        r.line = label + ": plays (" + mmss(firstTime) + " -> " + mmss(c.timeMs) + ")";
    } else if (seenTime) {
        r.line = label + ": started, then its clock stopped at " + mmss(c.timeMs);
    } else if (appBgm() && rcPlay >= 0) {
        // This service may simply not report a clock, so no verdict from here.
        r.maybe = true;
        r.line = label + ": accepted, but this service reports no progress; listen for it";
    } else {
        r.line = label + ": no sound after " + std::to_string(t.maxMs / 1000) + "s (state " +
                 std::to_string(c.state) + ")";
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

// A test.* file the user placed takes precedence, then any download in a
// format the shell decodes. Empty when there is neither. Called on the UI
// thread, where the downloads list is normally read.
std::string pickLocalFile() {
    for (const char* e : kShellExts) {
        const std::string p = platformPath(std::string("test.") + e);
        if (fileExists(p)) return p;
    }
    for (const auto& d : DownloadsManager::getInstance().getDownloads()) {
        if (d.state == DownloadState::COMPLETED && d.mediaType == "track" &&
            shellDecodes(lowerExt(d.localPath)) && fileExists(d.localPath))
            return d.localPath;
    }
    return {};
}

std::string headerValue(const std::map<std::string, std::string>& headers, const char* name) {
    for (const auto& kv : headers) {
        if (kv.first.size() != std::strlen(name)) continue;
        bool same = true;
        for (std::size_t i = 0; i < kv.first.size() && same; i++)
            same = std::tolower((unsigned char)kv.first[i]) == std::tolower((unsigned char)name[i]);
        if (same) return kv.second;
    }
    return {};
}

// Whether a server serves a link, asked with VitaPlex's own HTTP client: the
// first 4 KB, as a player's first read would be. Only for static files; a
// transcode link would start a transcode to answer it.
struct ServerAnswer {
    int status = 0;
    std::string type;
    std::size_t bytes = 0;
};

ServerAnswer askServer(const std::string& label, const std::string& url) {
    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.timeout = 10;
    req.headers["Range"] = "bytes=0-4095";
    const HttpResponse resp = client.request(req);
    ServerAnswer a;
    a.status = resp.statusCode;
    a.type = headerValue(resp.headers, "Content-Type");
    a.bytes = resp.body.size();
    brls::Logger::info("[bgaudio] server check, {}: status {}, type '{}', {} bytes{}{}", label,
                       a.status, a.type, a.bytes, resp.error.empty() ? "" : ", error: ",
                       resp.error);
    return a;
}

std::string describe(const ServerAnswer& a) {
    if (a.status == 0) return "VitaPlex got no answer from the server";
    return "the server answers it (" + std::to_string(a.status) +
           (a.type.empty() ? "" : " " + a.type) + ")";
}

// The first maxBytes of url, written to dest: enough of an MP3 for a player
// to open and start, without waiting for a whole track. False, and nothing
// left behind, if less than 64 KB arrived.
bool downloadPrefix(const std::string& url, const std::string& dest, std::size_t maxBytes) {
    FILE* f = std::fopen(dest.c_str(), "wb");
    if (!f) return false;
    std::size_t got = 0;
    std::string err;
    const SceInt64 t0 = sceKernelGetSystemTimeWide();
    HttpClient client;
    client.downloadFile(url, [&](const char* data, size_t size) {
        const std::size_t take = std::min(size, maxBytes - got);
        if (take > 0 && std::fwrite(data, 1, take, f) != take) return false;
        got += take;
        // Enough: stop the transfer here. A transcode comes no faster than
        // the server makes it, so after 20 s whatever has come will do for
        // attempts of a few seconds each.
        const bool slow = got >= 256 * 1024 && sceKernelGetSystemTimeWide() - t0 > 20 * 1000000LL;
        return got < maxBytes && !slow;
    }, nullptr, {}, 0, nullptr, &err);
    std::fclose(f);
    brls::Logger::info("[bgaudio] control file: {} bytes into {}{}{}", got, dest,
                       got < maxBytes && !err.empty() ? ", " : "",
                       got < maxBytes ? err : std::string());
    if (got < 64 * 1024) {
        std::remove(dest.c_str());
        return false;
    }
    return true;
}

// dest becomes a copy of src. False, and nothing left behind, if it could not
// be made.
bool copyFile(const std::string& src, const std::string& dest) {
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(dest.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    std::vector<char> buf(64 * 1024);
    bool ok = true;
    for (std::size_t n; ok && (n = std::fread(buf.data(), 1, buf.size(), in)) > 0;)
        ok = std::fwrite(buf.data(), 1, n, out) == n;
    ok = ok && !std::ferror(in);
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) std::remove(dest.c_str());
    return ok;
}

// A test's own file, removed when done with. The shell may still have it
// open, so a refusal is logged rather than taken for an error.
void removeTestFile(const std::string& path) {
    if (std::remove(path.c_str()) != 0)
        brls::Logger::info("[bgaudio] could not remove {} (errno {})", path, errno);
}

std::string wifiAddress() {
    SceNetCtlInfo info;
    std::memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return {};
    return info.ip_address;
}

// ── The control: VitaPlex serving a file itself ──────────────────────────
//
// Hands the system player the plainest link there is: plain http, a path
// ending in the file's own extension, no query string, a length and range
// support, for a file it has already played from disk. Every request is
// logged, so "the player never connected" can be told apart from "it
// connected and could not play what it got". An https client is recognised
// by its handshake and turned away. Serves one path, with a per-run name in
// it, and only for the length of the test.
class ControlServer : public std::enable_shared_from_this<ControlServer> {
public:
    // Returns the port, or -1.
    int start(const std::string& file) {
        m_file = file;
        char dir[24];
        std::snprintf(dir, sizeof(dir), "/t%08x",
                      (unsigned)(sceKernelGetSystemTimeWide() ^ (uintptr_t)this));
        m_path = std::string(dir) + "/track." + lowerExt(file);

        m_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) return fail("socket");
        int one = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(0);
        if (bind(m_fd, (sockaddr*)&addr, sizeof(addr)) < 0) return fail("bind");
        if (listen(m_fd, 4) < 0) return fail("listen");
        socklen_t len = sizeof(addr);
        if (getsockname(m_fd, (sockaddr*)&addr, &len) < 0) return fail("getsockname");
        m_port = ntohs(addr.sin_port);

        m_running = true;
        auto self = shared_from_this();
        platform::launchThread([self]() { self->acceptLoop(); });
        brls::Logger::info("[bgaudio] control server: port {}, serving {} as {}", m_port, m_file,
                           m_path);
        return m_port;
    }

    void stop() {
        m_stop = true;
        for (int i = 0; i < 40 && m_running; i++) sceKernelDelayThread(50 * 1000);
    }

    const std::string& path() const { return m_path; }
    // Counted as they are accepted, so none is put down to a later attempt.
    int connections() const { return m_connections; }
    int tlsHandshakes() const { return m_tls; }
    long long bytesSent() const { return m_sent; }

private:
    int fail(const char* what) {
        brls::Logger::info("[bgaudio] control server: {} failed, errno {}", what, errno);
        if (m_fd >= 0) close(m_fd);
        m_fd = -1;
        return -1;
    }

    void acceptLoop() {
        while (!m_stop) {
            pollfd p = {m_fd, POLLIN, 0};
            if (poll(&p, 1, 250) <= 0) continue;
            const int c = accept(m_fd, nullptr, nullptr);
            if (c < 0) continue;
            const int id = ++m_connections;
            auto self = shared_from_this();
            platform::launchThread([self, c, id]() { self->serve(c, id); });
        }
        close(m_fd);
        m_fd = -1;
        m_running = false;
    }

    // Waits for the socket to take more, so a player that stops reading
    // cannot hold this thread past the end of the test.
    bool sendAll(int c, const char* data, std::size_t size) {
        while (size > 0 && !m_stop) {
            pollfd p = {c, POLLOUT, 0};
            if (poll(&p, 1, 250) <= 0) continue;
            const int n = send(c, data, size, 0);
            if (n <= 0) return false;
            data += n;
            size -= (std::size_t)n;
            m_sent += n;
        }
        return size == 0;
    }

    void serve(int c, int id) {
        std::string req;
        char buf[2048];
        const SceInt64 t0 = sceKernelGetSystemTimeWide();
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192 && !m_stop &&
               sceKernelGetSystemTimeWide() - t0 < 5 * 1000000LL) {
            pollfd p = {c, POLLIN, 0};
            if (poll(&p, 1, 250) <= 0) continue;
            const int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, (std::size_t)n);
            if ((unsigned char)req[0] == 0x16) break;
        }

        // 0x16 opens a TLS record: an https client's handshake. There is no
        // certificate here to answer it with, but that it came at all shows
        // the player tried the https link.
        if (!req.empty() && (unsigned char)req[0] == 0x16) {
            m_tls++;
            brls::Logger::info("[bgaudio] control server: request {}: a TLS handshake ({} bytes), "
                               "an https client; closed, as this server has no certificate",
                               id, req.size());
            close(c);
            return;
        }

        // "GET /path HTTP/1.1", then headers one to a line.
        const std::string first = req.substr(0, req.find("\r\n"));
        const std::size_t sp1 = first.find(' ');
        const std::size_t sp2 = sp1 == std::string::npos ? sp1 : first.find(' ', sp1 + 1);
        const std::string method = first.substr(0, sp1);
        const std::string target =
            sp1 == std::string::npos ? std::string() : first.substr(sp1 + 1, sp2 - sp1 - 1);
        std::map<std::string, std::string> headers;
        for (std::size_t at = req.find("\r\n"); at != std::string::npos;) {
            const std::size_t next = req.find("\r\n", at + 2);
            const std::string line = req.substr(at + 2, next == std::string::npos ? next : next - at - 2);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::size_t v = colon + 1;
                while (v < line.size() && line[v] == ' ') v++;
                headers[line.substr(0, colon)] = line.substr(v);
            }
            at = next;
        }
        const std::string range = headerValue(headers, "Range");
        brls::Logger::info("[bgaudio] control server: request {}: {} {}, range '{}', "
                           "user-agent '{}'", id, method, target, range,
                           headerValue(headers, "User-Agent"));

        FILE* f = target == m_path ? std::fopen(m_file.c_str(), "rb") : nullptr;
        if (!f) {
            const char* notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";
            sendAll(c, notFound, std::strlen(notFound));
            close(c);
            return;
        }
        std::fseek(f, 0, SEEK_END);
        const long long size = std::ftell(f);
        long long from = 0, to = size - 1;
        bool partial = false;
        if (range.rfind("bytes=", 0) == 0) {
            const std::string spec = range.substr(6);
            const std::size_t dash = spec.find('-');
            if (dash != std::string::npos) {
                from = std::atoll(spec.substr(0, dash).c_str());
                if (dash + 1 < spec.size()) to = std::atoll(spec.substr(dash + 1).c_str());
                if (to >= size) to = size - 1;
                partial = from > 0 || to < size - 1;
            }
        }
        const std::string ext = lowerExt(m_file);
        const char* type = ext == "mp3" ? "audio/mpeg"
                         : ext == "wav" ? "audio/wav"
                         : ext == "at9" ? "audio/at9" : "audio/mp4";
        const std::string contentRange =
            partial ? "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) +
                          "/" + std::to_string(size) + "\r\n"
                    : std::string();
        char head[400];
        std::snprintf(head, sizeof(head),
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                      "Accept-Ranges: bytes\r\n%sConnection: close\r\n\r\n",
                      partial ? "206 Partial Content" : "200 OK", type, to - from + 1,
                      contentRange.c_str());
        bool ok = sendAll(c, head, std::strlen(head));
        long long sent = 0;
        if (ok && method != "HEAD") {
            std::fseek(f, (long)from, SEEK_SET);
            std::vector<char> chunk(32 * 1024);
            while (ok && from + sent <= to) {
                const std::size_t want = (std::size_t)std::min<long long>(chunk.size(), to - from - sent + 1);
                const std::size_t got = std::fread(chunk.data(), 1, want, f);
                if (got == 0) break;
                ok = sendAll(c, chunk.data(), got);
                if (ok) sent += (long long)got;
            }
        }
        std::fclose(f);
        close(c);
        brls::Logger::info("[bgaudio] control server: request {} done, {} of {} bytes sent", id,
                           sent, to - from + 1);
    }

    std::string m_file, m_path;
    int m_fd = -1;
    int m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<int> m_connections{0};
    std::atomic<int> m_tls{0};
    std::atomic<long long> m_sent{0};
};

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

    const std::string path = pickLocalFile();
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
            r = shellTry("File " + baseName(path), path, true, g_service.load());
        }
        std::string report = "System player (" + service + "), local file\n\n" + r.line + "\n\n";
        report += (r.played || r.maybe)
            ? listenAdvice(r.played)
            : "The system player did not play it. The log has every return code "
              "(Settings > Interface > View Log, lines with [bgaudio]).";
        finishTest(done, report);
    });
}

// Not a guess at what the shell wants but an experiment with controls. The
// first build tried only Plex's live transcode. The next added Plex's own
// file and a control, a plain link VitaPlex served itself, which the music
// player (type 0) and then the application-BGM service never connected to on
// a console. That left two things open: the control was a WAV there, and four
// services were never tried. So now, on all six in turn:
//
//   1. From disk: an MP3 of this track (its first 2 MB, from the server),
//      opened by a path on the memory card. A service that cannot play that
//      proves nothing by failing on a link.
//   2. Links: the same MP3, served by VitaPlex on the plainest link there
//      is (plain http, its own extension, no query string, a length and
//      range support), every connection logged. None at all means the
//      service never tried to fetch it, whatever it returned. Then an https
//      link to the same server: with no certificate that cannot play, but a
//      TLS handshake arriving shows the service does try https links.
//   3. Plex's own file and its MP3 transcode, over http and https, but only
//      on a service that tried one of those links: on any other they cannot
//      work, and earlier runs showed they did not.
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
    const int picked = g_service.load();
    const std::string localFile = pickLocalFile();
    platform::launchThread([done, ratingKey, title, picked, localFile]() {
        PlexClient& plex = PlexClient::getInstance();
        std::vector<std::string> lines;

        // What the server has for this track: the file's link, and the
        // source of the MP3.
        MediaItem item;
        const bool haveDetails = plex.fetchMediaDetails(ratingKey, item);
        const std::string fileExt = lowerExt(item.partPath);
        const bool fileDecodes = haveDetails && !item.partPath.empty() && shellDecodes(fileExt);
        const std::string fileUrl = fileDecodes
            ? plex.getServerUrl() + item.partPath + "?X-Plex-Token=" + plex.getAuthToken()
            : std::string();

        // The MP3: the start of this track's own file when that is an MP3,
        // else of its MP3 transcode (the start of an M4A need not play: its
        // index can be at the end). A download the shell decodes only if
        // neither can be had.
        std::string controlFile;
        bool controlDownloaded = false;
        {
            std::string src = fileExt == "mp3" ? fileUrl : std::string(), session;
            if (src.empty()) plex.getTranscodeUrlSpeculative(ratingKey, src, session);
            const std::string dest = platformPath("bgtest-control.mp3");
            if (!src.empty() && downloadPrefix(src, dest, 2 * 1024 * 1024)) {
                controlFile = dest;
                controlDownloaded = true;
            }
            if (controlFile.empty()) controlFile = localFile;
        }

        std::lock_guard<std::mutex> lock(g_shellMutex);
        holdPort(kPortForShell, "system player test");
        brls::Logger::info("[bgaudio] BGM port before the test: {}", bgmState());

        // 1 and 2, on each service: the picked one first (its http link also
        // on the Wi-Fi address), then the other five. From disk, the clock is
        // let run to 3.5 s, so the attempt after it can tell a new source by
        // the clock going back to the start. A link is given at least 4 s,
        // time enough for a player that fetches to have connected.
        const TryTiming fromDisk{8000, 0, 3500};
        const TryTiming onLink{8000, 4000, 3500};
        struct ServiceRun {
            int service = 0;
            ShellTry disk;
            bool fetchedHttp = false;  // made an http request of the test server
            bool triedHttps = false;   // began a TLS handshake with it
            bool linkPlayed = false;
        };
        std::vector<ServiceRun> runs;
        bool controlValid = !controlFile.empty();
        if (controlFile.empty()) {
            lines.push_back("Control: skipped. No MP3 came from the server, and there is no MP3, "
                            "M4A, AAC or WAV download, or test.mp3, to use instead");
        } else {
            lines.push_back("Control: " +
                            (controlDownloaded ? std::string("the first 2 MB of this track as an MP3")
                                               : "your " + baseName(controlFile)) +
                            ", played by each service from the memory card, then from a plain "
                            "http link VitaPlex serves itself, and an https one to the same server");
            auto server = std::make_shared<ControlServer>();
            const int port = server->start(controlFile);
            if (port < 0) {
                lines.push_back("  skipped: VitaPlex could not start its test server");
                controlValid = false;
            } else if (const ServerAnswer self = askServer(
                           "control server self-check",
                           "http://127.0.0.1:" + std::to_string(port) + server->path());
                       self.status != 200 && self.status != 206) {
                // Without this, "the player never connected" could be the
                // server's fault rather than the player's.
                lines.push_back("  VitaPlex could not fetch from its own test server either (" +
                                describe(self) + "), so the control shows nothing");
                controlValid = false;
            } else {
                lines.push_back("  VitaPlex fetched from the link itself: it works");
                std::vector<int> order = {picked};
                for (int sv = 0; sv <= kServiceAppBgm; sv++)
                    if (sv != picked) order.push_back(sv);
                const std::string wifi = wifiAddress();
                const std::string ext = lowerExt(controlFile);
                std::string diskFile;
                for (int sv : order) {
                    ServiceRun run;
                    run.service = sv;
                    const std::string tag = "  " + shortName(sv);

                    // A name of its own for each service, so that one service
                    // having opened the file cannot count for the next.
                    if (!diskFile.empty() && diskFile != controlFile) removeTestFile(diskFile);
                    const std::string copy =
                        platformPath("bgtest-disk-" + std::to_string(sv) + "." + ext);
                    diskFile = copyFile(controlFile, copy) ? copy : controlFile;
                    // The first try cannot tell when what was loaded before
                    // is as long and had not played 3 s. The second can.
                    for (int k = 0; k < 2; k++) {
                        run.disk = shellTry(tag + ", from disk", diskFile, false, sv, fromDisk);
                        if (!run.disk.unsure) break;
                    }
                    lines.push_back(run.disk.line);

                    // Plex serves https as well as http. The test server
                    // cannot finish an https connection, having no
                    // certificate, but whether one is begun at all it sees.
                    struct Link {
                        std::string host;
                        bool https;
                    };
                    std::vector<Link> links = {{"127.0.0.1", false}, {"127.0.0.1", true}};
                    if (sv == picked && !wifi.empty() && wifi != "127.0.0.1")
                        links.push_back({wifi, false});
                    for (const auto& link : links) {
                        const int connBefore = server->connections();
                        const int tlsBefore = server->tlsHandshakes();
                        const long long sentBefore = server->bytesSent();
                        const std::string url = std::string(link.https ? "https://" : "http://") +
                                                link.host + ":" + std::to_string(port) + server->path();
                        const std::string label = tag + (link.https ? ", https link" : ", http link") +
                                                  (link.host == "127.0.0.1" ? "" : " via " + link.host);
                        ShellTry r = shellTry(label, url, false, sv, onLink);
                        const int conns = server->connections() - connBefore;
                        const int tls = server->tlsHandshakes() - tlsBefore;
                        const long long kb = (server->bytesSent() - sentBefore) / 1024;
                        if (conns == 0) {
                            // Nothing was fetched, so nothing it played was the link.
                            r.played = false;
                            r.line = label + ": never connected to the server" +
                                     (r.moved ? "; the file from before went on"
                                              : r.maybe ? "" : ", and no sound");
                        } else if (tls == conns) {
                            r.played = false;
                            run.triedHttps = true;
                            r.line = label + ": connected and began https (" + std::to_string(tls) +
                                     " TLS handshake(s)), which the test server cannot finish. "
                                     "So it does try https links";
                        } else {
                            run.fetchedHttp = true;
                            // The same MP3 is loaded under its disk name, so
                            // the length cannot see a switch; the server can.
                            if (!r.played && r.moved && kb >= 64) {
                                r.played = true;
                                r.line = label + ": plays";
                            }
                            r.line += " [server: " + std::to_string(conns) + " connection(s), " +
                                      std::to_string(kb) + " KB sent]";
                        }
                        run.linkPlayed = run.linkPlayed || r.played;
                        lines.push_back(r.line);
                    }
                    runs.push_back(run);
                }
                lines.push_back("  (type 0 to 4: the music player's five client types. app BGM: "
                                "the app background music service.)");
                if (!diskFile.empty() && diskFile != controlFile) removeTestFile(diskFile);
            }
            server->stop();
        }
        if (controlDownloaded) removeTestFile(controlFile);

        std::vector<std::string> fetchedHttp, triedHttps, fromDiskNames, noClock, notSeen;
        bool anyLinkPlayed = false;
        // Plex's links go to the service that did best with the control:
        // played the link, else fetched it, else began https.
        int plexService = picked, plexRank = 0;
        for (const auto& run : runs) {
            const std::string n = shortName(run.service);
            if (run.fetchedHttp) fetchedHttp.push_back(n);
            if (run.triedHttps) triedHttps.push_back(n);
            anyLinkPlayed = anyLinkPlayed || run.linkPlayed;
            const int rank = run.linkPlayed ? 3 : run.fetchedHttp ? 2 : run.triedHttps ? 1 : 0;
            if (rank > plexRank) {
                plexRank = rank;
                plexService = run.service;
            }
            if (run.disk.played) fromDiskNames.push_back(n);
            else if (run.disk.maybe) noClock.push_back(n);
            else notSeen.push_back(n);
        }
        const bool linkTried = plexRank > 0;

        // 3. Plex's own file and its MP3 transcode: on a service that tried
        // one of the control's links, or on the picked one when there was no
        // control to ask.
        const bool tryPlex = !controlValid || linkTried;
        const bool https = plex.getServerUrl().rfind("https://", 0) == 0;
        auto asHttp = [](std::string url) {
            if (url.rfind("https://", 0) == 0) url = "http://" + url.substr(8);
            return url;
        };
        struct Attempt {
            std::string label;
            bool transcode;
            bool http;
        };
        std::vector<Attempt> attempts;
        if (tryPlex) {
            lines.push_back("");
            if (!haveDetails) {
                lines.push_back("Plex's file: could not read the track's details");
            } else if (!fileDecodes) {
                lines.push_back("Plex's file: skipped, it is " +
                                (fileExt.empty() ? std::string("an unknown format") : fileExt) +
                                ", which the shell does not decode");
            } else {
                lines.push_back("Plex's " + fileExt + " file, as stored on the server (" +
                                serviceName(plexService) + ")");
                attempts.push_back({"http", false, true});
                if (https) attempts.push_back({"https", false, false});
            }
        }
        const std::size_t firstTranscode = attempts.size();
        if (tryPlex) {
            attempts.push_back({"http", true, true});
            if (https) attempts.push_back({"https", true, false});
        }

        int firstPlayed = -1, firstMaybe = -1;
        auto urlFor = [&](const Attempt& a) {
            std::string url;
            if (a.transcode) {
                // A fresh session for every attempt, so the server never sees
                // one session asked for twice.
                std::string session;
                if (!plex.getTranscodeUrlSpeculative(ratingKey, url, session)) return std::string();
            } else {
                url = fileUrl;
            }
            return a.http ? asHttp(url) : url;
        };
        for (std::size_t i = 0; i < attempts.size(); i++) {
            if (i == firstTranscode) {
                lines.push_back("");
                lines.push_back("Plex's MP3 transcode, as VitaPlex plays it (" +
                                serviceName(plexService) + ")");
            }
            const std::string url = urlFor(attempts[i]);
            if (url.empty()) {
                lines.push_back("  " + attempts[i].label + ": could not get a link from the server");
                continue;
            }
            const std::string server =
                attempts[i].transcode ? std::string() : describe(askServer("file, " + attempts[i].label, url));
            const ShellTry r = shellTry("  " + attempts[i].label, url, false, plexService);
            lines.push_back(r.line + (server.empty() ? "" : " [" + server + "]"));
            if (r.played && firstPlayed < 0) firstPlayed = (int)i;
            if (r.maybe && firstMaybe < 0) firstMaybe = (int)i;
        }

        // Start the best Plex form again and leave it going, so the user can
        // hear whether it carries on outside the app. The control cannot be
        // left going: its server stops with the test.
        const int best = firstPlayed >= 0 ? firstPlayed : firstMaybe;
        ShellTry replay;
        if (best >= 0) {
            const std::string url = urlFor(attempts[best]);
            if (!url.empty()) replay = shellTry("  " + attempts[best].label, url, true, plexService);
        }
        const bool playingNow = replay.played || replay.maybe;
        if (!playingNow) {
            stopShellLocked("test over, nothing left playing");
            releasePort(kPortForShell, "system player test over");
        }

        std::string report = "System player, streaming \"" + title + "\"\n\n";
        for (const auto& l : lines) report += l + "\n";
        report += "\n";
        if (controlValid && !linkTried) {
            if (fromDiskNames.empty()) {
                report += "No service was seen playing the MP3 from the memory card, so this run "
                          "shows nothing about links. The log has every return code (lines with "
                          "[bgaudio]).";
            } else {
                const bool one = fromDiskNames.size() == 1;
                report += joinNames(fromDiskNames) + " played the MP3 from the memory card, but " +
                          (one ? "did not connect" : "none of them connected") +
                          " to the same MP3 on a plain http link (one VitaPlex itself fetched from "
                          "without trouble), or even " + (one ? "open" : "opened") +
                          " a connection for an https link to it. ";
                if (!noClock.empty())
                    report += joinNames(noClock) + (noClock.size() == 1 ? " reports" : " report") +
                              " no progress, so whether it played from the memory card could not "
                              "be measured; it did not connect either. ";
                if (!notSeen.empty())
                    report += joinNames(notSeen) + (notSeen.size() == 1 ? " was" : " were") +
                              " not seen playing it from the memory card, so what " +
                              (notSeen.size() == 1 ? "it" : "they") +
                              " did with the links shows nothing. ";
                report += "So the system player does not fetch links, http or https. It cannot "
                          "stream from Plex whatever the link, and that is not down to Plex, the "
                          "certificate or the query string. Plex's own links were not tried for "
                          "that reason.";
            }
        } else {
            if (!fetchedHttp.empty())
                report += "Fetched the http link: " + joinNames(fetchedHttp) +
                          (anyLinkPlayed ? ". The system player can play a plain http link. "
                                         : ", but did not play it. ");
            if (!triedHttps.empty())
                report += "Began https on the https link: " + joinNames(triedHttps) +
                          ". The test server cannot finish https, so Plex's own https link, "
                          "above, is the test of that. ";
            if (playingNow) {
                report += "Plex: " + std::string(attempts[best].transcode ? "the transcode" : "the file") +
                          " over " + attempts[best].label + ". " + listenAdvice(replay.played);
            } else if (best >= 0) {
                report += "A Plex link looked like it worked during the test but did not start "
                          "again afterwards. The log has the details (lines with [bgaudio]).";
            } else {
                report += "No Plex link played. The log has every return code and request "
                          "(lines with [bgaudio]).";
            }
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
