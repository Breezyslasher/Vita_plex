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

#include "activity/player_activity.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_controller.hpp"
#include "app/music_queue.hpp"
#include "app/plex_client.hpp"
#include "platform/paths.hpp"
#include "platform/platform.hpp"
#include "player/mpv_player.hpp"
#include "utils/http_client.hpp"
#include "utils/now_playing.hpp"
#include "utils/vita_install.hpp"

#include <borealis.hpp>

#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include "bgplayer.h"
#include "qm_cover.h"
#include "qm_link.h"

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
#include <ctime>
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
int sceAppMgrAcquireBgmPort(void);
int sceAppMgrReleaseBgmPort(void);
// SceAppMgr's own, by NID (appmgr_bgm_stub.S): which process the system has
// holding the BGM port and in what state, and the port taken as a music
// player takes it, whose arguments nobody has written down.
int vitaplexGetCurrentBgmState(SceUID* processId, unsigned* state);
int vitaplexAcquireBgmPortForMusicPlayer(int unknown);
int sceAppMgrAppParamGetString(int pid, int param, char* string, SceSize length);
int sceAppMgrGetIdByName(SceUID* pid, const char* name);
// SceBgAppUtil (psp2/bgapputil.h): starts the package's eboot2.bin. vitasdk's
// header says the mode must be 1; BGFTP passes 0.
int sceBgAppUtilStartBgApp(int mode);
// _sceKernelExitProcessForUser, the system call that sceKernelExitProcess
// ends with (src/bgplayer_psv/raw_exit.S).
int vitaplexRawExit(int status);
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
// The two ways of taking it with no priority, for the test that tries each
// for mpv (setOwnPortTest). Out of a priority's range, so that
// g_portPriority holds them as it holds a priority.
constexpr int kPortPlain       = 0x100;  // sceAppMgrAcquireBgmPort
constexpr int kPortMusicPlayer = 0x101;  // sceAppMgrAcquireBgmPortForMusicPlayer

constexpr SceUInt kTickUs      = 250 * 1000;
constexpr int     kBeatTicks   = 16;           // a heartbeat line every 4 s away
constexpr SceInt64 kGapUs      = 3 * 1000000;  // a tick this late was not scheduled
// How long the normal way out gets after the system asks the app to quit,
// before the watcher ends the process itself. Short while the UI thread has
// not even taken the request, since then it is stuck. Longer once it has, so
// a shutdown already under way is not cut off: it rewrites settings.json in
// place, and ending the process mid-write would lose the user's login. A
// device log (280b1532) had the whole of it take 1.2 s, the write at 1.14 s.
constexpr SceInt64 kQuitGraceUs    = 2 * 1000000;
constexpr SceInt64 kQuitShutdownUs = 10 * 1000000;

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

// The title id this process runs under, from its param.sfo: VitaPlex and
// VitaPlex BG are one eboot in two packages.
std::string runningTitleId() {
    char id[16] = {};   // param 12, the title id, needs at least 10 bytes
    const int rc = sceAppMgrAppParamGetString(0, 12, id, sizeof(id));
    if (rc < 0) {
        brls::Logger::error("[bgaudio] could not read the title id: {}", hex(rc));
        return {};
    }
    return id;
}

std::atomic<bool> g_keepPlaying{false};
// The setting (OutsideMusic); kOutsideStops outside VitaPlex BG.
std::atomic<int> g_outsideMusic{kOutsideStops};
// Whether anything has asked for the watcher's reports: background playback,
// or a system player test. The watcher also runs without, but then only to
// answer the system's request to quit (see watcherMain).
std::atomic<bool> g_watcherReports{false};

// ── The background player's side (see "The background player" below) ─────
// sceBgAppUtilStartBgApp has been called: the process then ends through the
// system call under sceKernelExitProcess (exitProcess).
std::atomic<bool> g_bgAppUsed{false};
// When the queue was handed over, on the way out; 0 before. Past
// kQuitShutdownUs after it the watcher ends the process itself.
std::atomic<SceInt64> g_handedOverAt{0};
// The last press of PS (the power callback): a deactivation within 2 s of
// one is the user leaving, as BG Tests 20 and 21 took it.
std::atomic<SceInt64> g_psPressUs{0};
// When PS was last held, which opens the quick menu: for kQuickMenuWatchUs
// after it the watcher logs the BGM owner, mpv and the shell whenever one of
// them changes, so what the Music row's buttons did shows in the log.
std::atomic<SceInt64> g_psHeldUs{0};
constexpr SceInt64 kQuickMenuWatchUs = 60 * 1000000;
// When this process set up the background player's side, on the system
// clock, which runs on from one process to the next as the notes have it.
double g_initSecs = 0;
// VitaPlex's quick menu plugin was in SceShell at init, and told this app
// takes the music back (qm_link.h's qmTakeBack): any queue SceShell carried on
// then stopped, and its last status follows.
bool g_takeBackSent = false;
bool handOver(const char* why);

// ── The quick menu's Music row (qm_link.h) ───────────────────────────────
// What MusicController publishes, which now_playing.cpp hands here on the
// Vita, for the watcher to put on the row through VitaPlex's quick menu
// plugin (or ElevenMPV-A's). The row is this process's while it has music to
// show; it goes to the background player with the queue, and back to the
// system as the app goes.
std::atomic<bool> g_qmEnabled{false};   // the setting
std::atomic<int> g_qmLook{QM_LOOK_LIT};   // the test picker
std::atomic<bool> g_qmActive{false};    // the plugin has the row for this process
std::atomic<bool> g_qmGone{false};      // handed over or quitting: not taken again
std::mutex g_qmMutex;
struct QmSong {
    std::string title, artist, album;
    std::string artUrl;    // its cover (now_playing.hpp), "" for none
    bool playing = false;
    bool wanted = false;   // published and not cleared since
    unsigned serial = 0;   // one more each time there is new text to send
};
QmSong g_qmSong;   // under g_qmMutex

// ── The song's cover on the row (QM_CMD_ART) ─────────────────────────────
// Made by a thread of its own, as the watcher asks for it: Plex's thumbnail
// through its transcoder at twice the size the row shows, or a download's
// local copy, made into the PNG the plugin loads (qm_cover.h, which the
// background player shares). The watcher sends its serial once written.
std::mutex g_qmArtMutex;
std::string g_qmArtWanted;     // the cover asked for, "" for none
std::string g_qmArtDone;       // the last one made, or that failed ...
uint32_t g_qmArtSerial = 0;    // ... and its serial, 0 if it failed
SceUID g_qmArtSema = -1;       // signalled as g_qmArtWanted changes

// Plex's transcoder asked for another size: its width and height.
std::string withThumbSize(std::string url, int size) {
    for (const char* key : {"width=", "height="}) {
        size_t at = url.find(std::string("&") + key);
        if (at == std::string::npos) at = url.find(std::string("?") + key);
        if (at == std::string::npos) continue;
        at += 1 + std::strlen(key);
        const size_t end = url.find('&', at);
        url.replace(at, end == std::string::npos ? std::string::npos : end - at,
                    std::to_string(size));
    }
    return url;
}

// The cover at source written for serial; "" when done, else what failed.
std::string makeQmArt(const std::string& source, uint32_t serial) {
    std::string bytes;
    if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
        HttpRequest req;
        req.url = withThumbSize(source, QM_ART_SIZE * 2);
        req.timeout = 15;
        HttpClient client;
        HttpResponse r = client.request(req);
        if (!r.success) return "not fetched: HTTP " + std::to_string(r.statusCode) + " " + r.error;
        bytes.swap(r.body);
    } else {
        FILE* f = std::fopen(source.c_str(), "rb");
        if (!f) return "its file would not open";
        char buf[16384];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) bytes.append(buf, n);
        std::fclose(f);
    }
    char why[160];
    return qmCoverWrite(bytes.data(), bytes.size(), serial, why, sizeof(why)) == 0 ? "" : why;
}

int qmArtMain(SceSize, void*) {
    uint32_t last = 0;   // the serial of the last cover made
    for (;;) {
        sceKernelWaitSema(g_qmArtSema, 1, nullptr);
        std::string source;
        {
            std::lock_guard<std::mutex> lock(g_qmArtMutex);
            source = g_qmArtWanted;
            if (source == g_qmArtDone) continue;
        }
        uint32_t serial = 0;
        if (!source.empty()) {
            const SceUInt64 start = sceKernelGetSystemTimeWide();
            serial = qmCoverNextSerial(last, start);
            const std::string failed = makeQmArt(source, serial);
            if (failed.empty()) {
                last = serial;
                brls::Logger::info("[bgaudio] quick menu: cover {} made in {} ms", serial,
                                   (sceKernelGetSystemTimeWide() - start) / 1000);
            } else {
                brls::Logger::info("[bgaudio] quick menu: cover {}", failed);
                serial = 0;
            }
        }
        std::lock_guard<std::mutex> lock(g_qmArtMutex);
        g_qmArtDone = source;
        g_qmArtSerial = serial;
    }
    return 0;
}

// From the watcher: the cover for source, made by now or asked for. true
// with its serial (0 for none) once made, false while it is being made.
bool qmArtFor(const std::string& source, uint32_t* serial) {
    if (source.empty()) {
        *serial = 0;
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(g_qmArtMutex);
        if (g_qmArtDone == source) {
            *serial = g_qmArtSerial;
            return true;
        }
        if (g_qmArtWanted == source) return false;
        g_qmArtWanted = source;
    }
    static bool started = false;
    if (!started) {
        started = true;
        g_qmArtSema = sceKernelCreateSema("VitaPlexQmArt", 0, 0, 1, nullptr);
        const SceUID thid =
            g_qmArtSema >= 0 ? sceKernelCreateThread("VitaPlexQmArt", qmArtMain, 0x10000100,
                                                     0x10000, 0, 0, nullptr)
                             : g_qmArtSema;
        const int rc = thid >= 0 ? sceKernelStartThread(thid, 0, nullptr) : thid;
        if (rc < 0) brls::Logger::error("[bgaudio] quick menu: no cover thread: {}", hex(rc));
    }
    sceKernelSignalSema(g_qmArtSema, 1);
    return false;
}

// Gives the row back to the system for good, from any thread: the music
// handed to the background player, which takes the row itself as it plays,
// or the app going. The plugin, left on, would keep putting presses into a
// pipe no process reads, and SceShell waits once that pipe is full.
void quickMenuRelease(const char* why) {
    g_qmGone = true;
    if (!g_qmActive.exchange(false)) return;
    brls::Logger::info("[bgaudio] quick menu: row given back ({}): {}", why, hex(qmDeactivate()));
}

// ── The BGM port ─────────────────────────────────────────────────────────
// Held at one priority at a time: the app's while mpv plays, the shell's
// while a system player test runs. 0 = not held.
std::mutex g_portMutex;
int g_portPriority = 0;

std::string portName(int priority) {
    if (priority == kPortPlain) return "plain";
    if (priority == kPortMusicPlayer) return "as a music player";
    char b[16];
    std::snprintf(b, sizeof(b), "%#x", (unsigned)priority);
    return b;
}

// Which process the system has holding the BGM port, and its state: what the
// quick menu's Music row goes by, as far as ElevenMPV-A's plugin shows (it
// changes what SceShell reads of it). Asked no more once refused.
std::atomic<bool> g_bgmOwnerRefused{false};

std::string bgmOwner() {
    if (g_bgmOwnerRefused) return "BGM owner: not asked, refused before";
    // Room to spare, in case the system writes more than the header says.
    SceUID pid[4] = {0, 0, 0, 0};
    unsigned state[4] = {0, 0, 0, 0};
    const int rc = vitaplexGetCurrentBgmState(pid, state);
    if (rc < 0) {
        g_bgmOwnerRefused = true;
        return "BGM owner: " + hex(rc);
    }
    char b[96];
    std::snprintf(b, sizeof(b), "BGM owner: process 0x%08X%s, state 0x%X", (unsigned)pid[0],
                  pid[0] == sceKernelGetProcessId() ? " (VitaPlex)" : "", state[0]);
    return b;
}

int acquirePort(int priority) {
    if (priority == kPortPlain) return sceAppMgrAcquireBgmPort();
    if (priority == kPortMusicPlayer) return vitaplexAcquireBgmPortForMusicPlayer(0);
    return sceAppMgrAcquireBgmPortWithPriority(priority);
}

void holdPort(int priority, const char* why) {
    std::lock_guard<std::mutex> lock(g_portMutex);
    if (g_portPriority == priority) return;
    if (g_portPriority != 0) {
        const int rc = sceAppMgrReleaseBgmPort();
        brls::Logger::info("[bgaudio] BGM port: release {} to change priority: {}",
                           portName(g_portPriority), hex(rc));
    }
    const int rc = acquirePort(priority);
    brls::Logger::info("[bgaudio] BGM port: acquire {} for {}: {}; {}", portName(priority), why,
                       hex(rc), bgmOwner());
    g_portPriority = rc >= 0 ? priority : 0;
}

// Releases only if held at `priority`, or at any priority when it is 0.
void releasePort(int priority, const char* why) {
    std::lock_guard<std::mutex> lock(g_portMutex);
    if (g_portPriority == 0 || (priority != 0 && g_portPriority != priority)) return;
    const int rc = sceAppMgrReleaseBgmPort();
    brls::Logger::info("[bgaudio] BGM port: release {} ({}): {}", portName(g_portPriority), why,
                       hex(rc));
    g_portPriority = 0;
}

// How mpv takes the port, by the test picker in settings: 0 at kPortForApp
// and only while background playback is on, as ElevenMPV-A does for its own
// decoders; the others whenever mpv plays, to see which, if any, lights the
// quick menu's Music row for VitaPlex. Not saved.
const int kOwnPort[] = {kPortForApp, kPortForShell, kPortPlain, kPortMusicPlayer, 0};
constexpr int kOwnPortChoices = (int)(sizeof(kOwnPort) / sizeof(kOwnPort[0]));
std::atomic<int> g_ownPortTest{0};

void holdPortForMpv(const char* why) {
    const int priority = kOwnPort[g_ownPortTest];
    if (priority != 0)
        holdPort(priority, why);
    else
        releasePort(0, why);
}

// The port a system player test takes, and the background player is told to
// take: the shell's priority, unless the port test picks a way with none, or
// none at all (0). "As a music player" put VitaPlex's icon in the quick
// menu's Music row with mpv playing, its buttons dimmed (log 9c16c0d6), and
// with it held the shell would not play a file: state 10, no sound (log
// dbd9b556). The shell playing with no port taken is what is left to try.
// The one a test took is g_testPort, so that the end of the test gives back
// that one.
int shellTestPort() {
    const int p = kOwnPort[g_ownPortTest];
    return p == kPortPlain || p == kPortMusicPlayer || p == 0 ? p : kPortForShell;
}

int g_testPort = kPortForShell;   // under g_shellMutex

// For a system player test, under g_shellMutex: the port the picker says, or
// none held at all.
void holdPortForTest() {
    g_testPort = shellTestPort();
    if (g_testPort != 0)
        holdPort(g_testPort, "system player test");
    else
        releasePort(0, "system player test, with no port taken");
}

// How the queue tells the background player to take the port (bgplayer.h).
const char* playerPortName() {
    switch (kOwnPort[g_ownPortTest]) {
        case kPortPlain:       return BGPLAYER_PORT_PLAIN;
        case kPortMusicPlayer: return BGPLAYER_PORT_MUSICPLAYER;
        case 0:                return BGPLAYER_PORT_NONE;
        default:               return BGPLAYER_PORT_PRIORITY;
    }
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
// The shell's state while VitaPlex has it playing, else -1; the same way.
int shellStateNow();

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
    const int port = portPriority();
    brls::Logger::info("[bgaudio] deactivated (left for the LiveArea or another app){}: {}, "
                       "BGM port {}, keep playing {}{}",
                       heldFirst ? ", read only after the process was held" : "",
                       describeMpv(s), port != 0 ? portName(port) : "0x0",
                       g_keepPlaying.load(), shellStatusLine());

    // Left with PS, by the setting: the background player takes the queue
    // over and VitaPlex BG goes, so that a game started next finds nothing
    // to close. On the UI thread, which owns the queue, and runs on in a
    // non-game app sent away (log a93baa71).
    if (g_outsideMusic == kOutsideHandsOver && now - g_psPressUs.load() < 2000000) {
        brls::sync([]() {
            if (handOver("left with PS")) brls::Application::quit();
        });
    }
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

// When the system asked the app to quit; 0 until it has. Watcher thread only.
SceInt64 g_quitAskedAt = 0;
// Set on the UI thread once it has taken the request and begun the normal
// shutdown; see kQuitShutdownUs.
std::atomic<bool> g_quitUnderway{false};

void onRequestQuit(SceInt64 now, const AppEvent& ev) {
    // What asks, in full, once: the event may say why (a game started over
    // the app, or its LiveArea closed), which nothing here tells apart yet.
    std::string param;
    for (int i = 0; i < 24; i++) {
        char h[4];
        std::snprintf(h, sizeof(h), "%02X", (unsigned char)ev.param[i]);
        param += h;
    }
    brls::Logger::info("[bgaudio] the system asked the app to quit (app {}, then {}); quitting",
                       hex(ev.appId), param);
    quickMenuRelease("the app is going");
    // A file the system player was given would otherwise play on after the app
    // is gone, with nothing left to stop it. Not waited for if a test is mid-run
    // (see shellStatusLine); the process is going either way.
    {
        std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
        if (lock.owns_lock()) stopShellLocked("app quitting");
    }
    releasePort(0, "app quitting");

    // And then actually quit. The system waits for this process to end before
    // it starts the app the user opened, and this used to stop at the line
    // above. A log (3ea218f5, build 1957) had the request at 17:29:09 and
    // VitaPlex still running four minutes later, while the app the user had
    // opened sat on its loading screen and the console had to be held off.
    // ElevenMPV-A and the test apps exit on this request, and those were
    // closed the same way without any of that.
    //
    // The normal way out first, which saves the settings. watcherMain ends the
    // process itself if that has not happened in time (kQuitGraceUs), since
    // the UI thread may be stuck in a request to a server it cannot reach.
    // Music playing goes to the background player on the way, by the setting:
    // this is what a game started over VitaPlex BG brings.
    if (g_quitAskedAt == 0) {
        g_quitAskedAt = now;
        brls::sync([]() {
            g_quitUnderway = true;
            handOver("the system asked VitaPlex BG to quit");
            brls::Application::quit();
        });
    }
}

// True if a deactivate or activate event was among them. With quitOnly, the
// quit request is the only event acted on; the rest are read and dropped.
bool drainAppEvents(SceInt64 now, SceInt64 gapUs, bool quitOnly) {
    int n = 0;
    const int rc = sceAppMgrReceiveEventNum(&n);
    if (rc < 0 || n <= 0) return false;
    bool focus = false;
    for (int i = 0; i < n; i++) {
        AppEvent ev;
        std::memset(&ev, 0, sizeof(ev));
        if (sceAppMgrReceiveEvent(&ev) < 0) break;
        if (quitOnly && ev.event != kEventRequestQuit) continue;
        switch (ev.event) {
            case kEventDeactivate:  onDeactivate(now, gapUs); focus = true; break;
            case kEventActivate:    onActivate(now); focus = true; break;
            case kEventResume:      brls::Logger::info("[bgaudio] app event: resume"); break;
            case kEventRequestQuit: onRequestQuit(now, ev); break;
            default: {
                // In full: one the quick menu's Music row sends would be new.
                std::string param;
                for (int b = 0; b < 24; b++) {
                    char h[4];
                    std::snprintf(h, sizeof(h), "%02X", (unsigned char)ev.param[b]);
                    param += h;
                }
                brls::Logger::info("[bgaudio] app event {} (app {}, then {})", hex(ev.event),
                                   hex(ev.appId), param);
                break;
            }
        }
    }
    return focus;
}

// Where VitaPlex's own player was when PS was pressed, just before the
// system held the process. Only the watcher thread, which runs the power
// callback, touches these.
MpvPlayer::Snapshot g_atPs;
bool g_haveAtPs = false;

// Whether VitaPlex has the system player going: a test running, or a file a
// test left playing. Never waits for the shell lock.
bool shellInUse() {
    if (g_shellBusy) return true;
    std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
    return !lock.owns_lock() || g_shellPlaying;
}

// Back from being held with no deactivate event, as a game such as VitaPlex
// is: says so on screen, with where its own player was when PS was pressed
// and where it is now, so whether VitaPlex's music went on needs no log. The
// system player plays on by itself, so what it had is named too: music heard
// meanwhile may have been that.
void reportHold(SceInt64 gapUs) {
    const MpvPlayer::Snapshot now = MpvPlayer::getInstance().snapshot();
    const bool wasPlaying = g_haveAtPs && g_atPs.initialized &&
                            (g_atPs.state == MpvPlayerState::PLAYING ||
                             g_atPs.state == MpvPlayerState::BUFFERING);
    const bool shell = shellInUse();
    const unsigned beforeMs = (unsigned)(std::max(0.0, g_atPs.position) * 1000);
    const unsigned nowMs = (unsigned)(std::max(0.0, now.position) * 1000);
    brls::Logger::info("[bgaudio] held {:.1f}s with no deactivate event: {} when PS was pressed, {} "
                       "now; system player in use by VitaPlex: {}",
                       gapUs / 1e6, g_haveAtPs ? describeMpv(g_atPs) : std::string("no PS press seen"),
                       describeMpv(now), shell);

    const int secs = (int)(gapUs / 1000000);
    std::string msg = "VitaPlex was frozen for " + std::to_string(secs) + "s while you were away";
    if (!wasPlaying)
        msg += ".";
    else if (nowMs < beforeMs + 1000)
        msg += ", so its own music stopped at " + mmss(beforeMs) + ".";
    else
        msg += ", yet its music moved from " + mmss(beforeMs) + " to " + mmss(nowMs) + ".";
    if (shell) msg += " The system player was playing for a test; it plays on by itself.";
    g_haveAtPs = false;
    brls::sync([msg]() { brls::Application::notify(msg); });
}

int powerCallback(int, int, int powerInfo, void*) {
    if (!g_watcherReports) return 0;   // running only to answer a quit
    std::string what;
    const struct { unsigned bit; const char* name; } kBits[] = {
        {SCE_POWER_CB_APP_SUSPEND, "app suspend"},
        {SCE_POWER_CB_APP_RESUME, "app resume"},
        {SCE_POWER_CB_APP_RESUMING, "app resuming"},
        {SCE_POWER_CB_SYSTEM_SUSPEND, "system suspend"},
        {SCE_POWER_CB_SYSTEM_RESUME, "system resume"},
        {SCE_POWER_CB_BUTTON_PS_PRESS, "PS button"},
        {SCE_POWER_CB_BUTTON_PS_HOLD, "PS held (quick menu)"},
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
    if ((unsigned)powerInfo & SCE_POWER_CB_BUTTON_PS_PRESS) {
        g_atPs = MpvPlayer::getInstance().snapshot();
        g_haveAtPs = true;
        g_psPressUs = sceKernelGetSystemTimeWide();
    }
    if ((unsigned)powerInfo & SCE_POWER_CB_BUTTON_PS_HOLD) g_psHeldUs = sceKernelGetSystemTimeWide();
    return 0;
}

// For a minute after PS is held: the BGM owner, mpv's state and the shell's,
// logged when any of them changes, so what a button on the Music row did
// shows up. Only the watcher thread calls it.
void watchQuickMenu(SceInt64 now) {
    static std::string last;
    static SceInt64 lastHeld = 0;
    const SceInt64 held = g_psHeldUs;
    if (held == 0 || now - held > kQuickMenuWatchUs) return;
    if (held != lastHeld) {   // held again: start over
        lastHeld = held;
        last.clear();
    }
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    const std::string owner = bgmOwner();
    const std::string key = owner + (s.initialized ? stateName(s.state) : "-") + " " +
                            std::to_string(shellStateNow());
    if (key == last) return;
    last = key;
    brls::Logger::info("[bgaudio] {:.1f}s after PS was held: {}; {}{}", (now - held) / 1e6, owner,
                       describeMpv(s), shellStatusLine());
}

// The quick menu's row, from the watcher alone (but for quickMenuRelease):
// the plugin loaded and its pipes opened the first time there is music to
// show, tried again at most every 30 s while that fails; the row taken while
// MusicController has music published and given back once it clears it; the
// song sent as it changes; and the row's presses passed to MusicController
// the way the OS media controls' are (now_playing.hpp).
void quickMenuTick(SceInt64 now) {
    static SceInt64 lastTry = 0, lastBeat = 0;
    static unsigned sentSerial = 0;
    static int sentPlaying = -1, sentLook = -1;
    static int64_t sentArt = -1;   // the cover's serial sent, -1 for none yet
    if (!g_qmEnabled || g_qmGone) return;
    QmSong song;
    {
        std::lock_guard<std::mutex> lock(g_qmMutex);
        song = g_qmSong;
    }
    const bool wanted = song.wanted && g_handedOverAt == 0;
    if (!qmIsOpen()) {
        if (!wanted || (lastTry != 0 && now - lastTry < 30 * 1000000LL)) return;
        lastTry = now;
        char what[320];
        const int rc = qmOpen(what, sizeof(what));
        brls::Logger::info("[bgaudio] quick menu: {} ({})", what, hex(rc));
        if (rc < 0) return;
    }
    if (wanted && !g_qmActive) {
        // A press left from whoever had the row before is not one for this.
        for (int stale; (stale = qmPoll()) != QM_NONE;)
            brls::Logger::info("[bgaudio] quick menu: an earlier press ({}) dropped", stale);
        const int rc = qmActivate(QM_OWNER_APP);
        brls::Logger::info("[bgaudio] quick menu: row taken: {}", hex(rc));
        if (rc < 0) return;
        g_qmActive = true;
        sentSerial = song.serial - 1;   // so the song goes again
        sentPlaying = sentLook = -1;
        sentArt = -1;
        lastBeat = now;
    } else if (!wanted && g_qmActive) {
        g_qmActive = false;
        brls::Logger::info("[bgaudio] quick menu: row given back, nothing published: {}",
                           hex(qmDeactivate()));
        return;
    }
    if (!g_qmActive) return;
    // VitaPlex's plugin gives the row back by itself once these stop.
    if (now - lastBeat >= 1000000) {
        lastBeat = now;
        qmHeartbeat(QM_OWNER_APP);
    }
    const int look = g_qmLook;
    if (look != sentLook && qmIsOwn()) {
        const int rc = qmSetLook(look);
        brls::Logger::info("[bgaudio] quick menu: buttons to be switched on with look {}: {}",
                           look, hex(rc));
        if (rc >= 0) sentLook = look;
    }
    if (song.serial != sentSerial) {
        const int rc = qmSetSong(song.title.c_str(), song.artist.c_str(), song.album.c_str());
        brls::Logger::info("[bgaudio] quick menu: \"{}\" by {}: {}", song.title, song.artist,
                           hex(rc));
        if (rc >= 0) sentSerial = song.serial;
    }
    const int playing = song.playing ? 1 : 0;
    if (playing != sentPlaying) {
        const int rc = qmSetPlaying(playing);
        if (rc >= 0) sentPlaying = playing;
    }
    // The cover: the last one stays on the row while the next is made.
    uint32_t art = 0;
    if (qmIsOwn() && qmArtFor(song.artUrl, &art) && (int64_t)art != sentArt) {
        const int rc = qmSetArt(art);
        brls::Logger::info("[bgaudio] quick menu: cover {} sent: {}", art, hex(rc));
        if (rc >= 0) sentArt = art;
    }
    for (int press; (press = qmPoll()) != QM_NONE;) {
        const nowplaying::Transport t = press == QM_NEXT       ? nowplaying::Transport::Next
                                        : press == QM_PREVIOUS ? nowplaying::Transport::Previous
                                                               : nowplaying::Transport::Toggle;
        brls::Logger::info("[bgaudio] quick menu: {} pressed",
                           press == QM_NEXT       ? "next"
                           : press == QM_PREVIOUS ? "previous"
                                                  : "play/pause");
        brls::sync([t]() { nowplaying::dispatchTransport(t); });
    }
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
        // With nothing asking for reports, this thread still runs, but only to
        // answer the system's request to quit: the non-game package hangs the
        // console if that goes unanswered (see onRequestQuit), whatever the
        // background playback setting.
        const bool watching = g_watcherReports;
        const bool held = gap > kGapUs;
        if (held && watching) {
            // Nothing in this loop waits for long, so a tick this late means the
            // thread was not scheduled at all: the process was held, or the
            // console slept.
            brls::Logger::info("[bgaudio] watcher did not run for {:.1f}s", gap / 1e6);
            if (g_away && gap > g_awayLongestGapUs) g_awayLongestGapUs = gap;
        }
        const bool focusEvent = drainAppEvents(now, gap, !watching);
        const bool underway = g_quitUnderway;
        if (g_quitAskedAt != 0 &&
            now - g_quitAskedAt > (underway ? kQuitShutdownUs : kQuitGraceUs)) {
            brls::Logger::warning("[bgaudio] still running {:.1f}s after the system asked "
                                  "to quit ({}); ending the process",
                                  (now - g_quitAskedAt) / 1e6,
                                  underway ? "the shutdown did not finish"
                                           : "the UI never took the request");
            exitProcess(0);
        }
        // Handed over and quitting by itself: the background player waits
        // for the app to go before it plays.
        const SceInt64 handedAt = g_handedOverAt;
        if (handedAt != 0 && now - handedAt > kQuitShutdownUs) {
            brls::Logger::warning("[bgaudio] still running {:.1f}s after handing the music to "
                                  "the background player; ending the process",
                                  (now - handedAt) / 1e6);
            exitProcess(0);
        }
        if (!watching) continue;
        watchQuickMenu(now);
        quickMenuTick(now);
        // A game is sent neither event, so without this nothing would say,
        // on the way back, that it was held.
        if (held && !g_away && !focusEvent) reportHold(gap);
        if (g_away) {
            g_awayTicks++;
            if (g_awayTicks % kBeatTicks == 0) heartbeat(now);
        }
    }
    return 0;
}

// reports=false starts it only to answer a quit; see g_watcherReports.
void startWatcher(bool reports = true) {
    if (reports) g_watcherReports = true;
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
    bool interrupted = false;  // VitaPlex was held part way, so it shows nothing
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
// On a console the call was refused (0x8080201F), so once refused it is
// logged no more.
bool g_bgmStateRefused = false;

std::string bgmState() {
    // 20 bytes by libShellAudio's reading; zeroed room for more in case the
    // system writes more, and anything it did write there is shown.
    int st[16];
    std::memset(st, 0, sizeof(st));
    const int rc = shellAudioGetCurrentBGMState(reinterpret_cast<SceShellAudioBGMState*>(st));
    if (rc < 0) g_bgmStateRefused = true;
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

int shellStateNow() {
    std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
    if (!lock.owns_lock() || !g_shellPlaying) return -1;
    return shellClock().state;
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
    bool wentBack = false;
    unsigned firstTime = 0, lastTime = 0;
    const SceInt64 t0 = sceKernelGetSystemTimeWide();
    SceInt64 lastPoll = t0;
    for (;;) {
        const SceInt64 now = sceKernelGetSystemTimeWide();
        // A poll seconds late means this process was held (PS pressed): the
        // shell went on meanwhile, unwatched, so this attempt shows nothing.
        if (now - lastPoll > 2 * 1000000LL) {
            r.interrupted = true;
            break;
        }
        lastPoll = now;
        const SceInt64 elapsedMs = (now - t0) / 1000;
        if (elapsedMs >= t.maxMs) break;
        c = shellClock();
        if (c.state != lastState) {
            brls::Logger::info("[bgaudio] system player: {}: state {} at {} ({})", label, c.state,
                               mmss(c.timeMs), hex(c.rc));
            lastState = c.state;
        }
        // The first readings after an open can still be the old source's
        // clock, stopped where it was; a new source then starts again from
        // the top. So a clock going back starts the measure over, and shows
        // the source changed.
        if (seenTime && c.timeMs + 1000 < lastTime) {
            wentBack = true;
            seenTime = false;
            r.moved = false;
        }
        lastTime = c.timeMs;
        if (c.timeMs > 0 && !seenTime) {
            seenTime = true;
            firstTime = c.timeMs;
        }
        if (seenTime && c.timeMs >= firstTime + 1000) r.moved = true;
        if (r.moved && c.timeMs >= t.playToMs && elapsedMs >= t.minMs) break;
        sceKernelDelayThread(250 * 1000);
    }
    r.played = r.moved && !r.interrupted;
    if (!g_bgmStateRefused)
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
    // An old source at its very end would start again from the top too, so
    // then the clock says nothing.
    const bool sameSource = src == g_shellLastSrc[lastSrcSlot(service)];
    const bool oldAtEnd = durBefore > 0 && before.timeMs + 2000 >= (unsigned)durBefore;
    const bool clockTells = before.timeMs >= 3000 && !oldAtEnd;
    const bool nothingBefore = before.timeMs == 0 && durBefore <= 0;
    const bool lengthChanged = !appBgm() && durAfter != durBefore;
    const bool replaced = sameSource || (wentBack && !oldAtEnd) ||
                          (clockTells ? firstTime + 2000 < before.timeMs
                                      : nothingBefore || lengthChanged);

    if (r.interrupted) {
        r.line = label + ": interrupted, VitaPlex was put in the background during it";
    } else if (r.played && !replaced) {
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

// ── The background player ────────────────────────────────────────────────
//
// VitaPlex BG's own background program (src/bgplayer_psv), which outlives the
// app: the system closes VitaPlex BG to start a game, and the player carries
// the queue on through the shell's music player, which plays on beside games
// (docs/DESIGN_NOTES.md, BG Tests 10 to 21). The two talk through files in
// ux0:data/VitaPlex (bgplayer.h). The app hands the queue over as it goes and
// takes the music back as it opens again; the player waits for the app to go
// before it plays, and never ends while the app is open, since the two share
// app0: and the app lost it each time its program ended under it.

double systemSecs() { return sceKernelGetSystemTimeWide() / 1e6; }

// A file in place of `to`, written beside it: rename does not replace a file
// on every target (platform_common.cpp).
bool moveIn(const std::string& from, const std::string& to) {
    std::remove(to.c_str());
    return std::rename(from.c_str(), to.c_str()) == 0;
}

bool writeWhole(const std::string& path, const std::string& text) {
    const std::string part = path + ".part";
    FILE* f = std::fopen(part.c_str(), "w");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok || !moveIn(part, path)) {
        std::remove(part.c_str());
        return false;
    }
    return true;
}

// A request to the player, one line as bgplayer.h has it.
void ask(const char* path, const char* why) {
    char line[160];
    std::snprintf(line, sizeof(line), "%.6f %s\n", systemSecs(), why);
    writeWhole(path, line);
}

long fileSize(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    const long size = std::fseek(f, 0, SEEK_END) == 0 ? std::ftell(f) : -1;
    std::fclose(f);
    return size;
}

// The CA bundle for the player in ux0:data, unless a copy of its size is
// there already: the background test's player lost app0: when its app went
// after it had started, and every https request with it (log 5d35ef15).
void copyCaBundle() {
    const char* from = "app0:resources/cacert.pem";
    const long size = fileSize(from);
    if (size < 0 || fileSize(BGPLAYER_CA_BUNDLE) == size) return;
    const bool ok = copyFile(from, BGPLAYER_CA_BUNDLE ".part") &&
                    moveIn(BGPLAYER_CA_BUNDLE ".part", BGPLAYER_CA_BUNDLE);
    brls::Logger::info("[bgaudio] background player: CA bundle {} {}",
                       ok ? "copied to" : "could not be copied to", BGPLAYER_CA_BUNDLE);
}

// A field of a track line: '|' separates them, and a line ends the track.
std::string trackField(std::string s) {
    for (auto& c : s)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

int repeatCode(RepeatMode m) {
    return m == RepeatMode::ONE ? BGPLAYER_REPEAT_ONE
         : m == RepeatMode::ALL ? BGPLAYER_REPEAT_ALL : BGPLAYER_REPEAT_OFF;
}

// The queue in the order it plays, the song playing and where in it, as
// bgplayer.h lays it out. A queue longer than the player takes goes from the
// song playing on. On the UI thread, which owns the queue. The song, for the
// log, in *what.
bool writeQueue(std::string& what) {
    MusicQueue& q = MusicQueue::getInstance();
    const std::vector<QueueItem>& items = q.getQueue();
    std::vector<int> order;
    int at = q.getCurrentIndex();
    if (q.isShuffleEnabled() && q.getShuffleOrder().size() == items.size()) {
        order = q.getShuffleOrder();
        at = q.getShufflePosition();
    } else {
        for (int i = 0; i < (int)items.size(); i++) order.push_back(i);
    }
    if (at < 0 || at >= (int)order.size()) return false;
    std::size_t from = 0;
    if (order.size() > BGPLAYER_MAX_TRACKS) {
        from = (std::size_t)at;
        at = 0;
    }

    // Where in the song: mpv's clock counts from where its stream begins,
    // which a seek that restarted the transcode moved part-way in.
    const double position = MpvPlayer::getInstance().getPosition();
    const long long positionMs = std::max(
        0LL, MusicController::getInstance().streamStartOffsetMs() +
                 (position > 0 ? (long long)(position * 1000) : 0));

    char head[192];
    std::snprintf(head, sizeof(head),
                  "written=%.6f\nindex=%d\npositionMs=%lld\nrepeat=%d\nfile=\nport=%s\nqm=%d\n",
                  systemSecs(), at, positionMs, repeatCode(q.getRepeatMode()), playerPortName(),
                  g_qmEnabled ? 1 : 0);
    std::string text = head;
    DownloadsManager& downloads = DownloadsManager::getInstance();
    for (std::size_t i = from; i < order.size() && i - from < BGPLAYER_MAX_TRACKS; i++) {
        const QueueItem& t = items[(std::size_t)order[i]];
        std::string local, cover;
        DownloadItem dl;
        if (downloads.getDownloadCopy(t.ratingKey, dl) && dl.state == DownloadState::COMPLETED) {
            local = dl.localPath;
            cover = dl.thumbPath;   // as MusicController takes it for the row
        }
        char rating[16];
        std::snprintf(rating, sizeof(rating), "%.1f", t.userRating);
        text += "track=" + trackField(t.ratingKey) + "|" + std::to_string(t.duration * 1000) +
                "|" + trackField(local) + "|" + rating + "|" + trackField(t.thumb) + "|" +
                trackField(t.title) + "|" + trackField(t.artist) + "|" + trackField(t.album) +
                "|" + trackField(cover) + "\n";
    }
    text += "end\n";
    if (!writeWhole(BGPLAYER_QUEUE, text)) return false;
    const QueueItem& now = items[(std::size_t)order[from + (std::size_t)at]];
    what = std::to_string(std::min<std::size_t>(order.size() - from, BGPLAYER_MAX_TRACKS)) +
           " song(s), from \"" + now.title + "\" at " + mmss((unsigned)positionMs);
    return true;
}

// The player asked for: a request to play when it runs already, paused since
// the app took the music back; else started. 0 or more when it took it.
int askForPlayer() {
    SceUID pid = -1;
    if (sceAppMgrGetIdByName(&pid, BGPLAYER_TITLEID) >= 0) {
        ask(BGPLAYER_ASK_PLAY, "VitaPlex BG hands the music over as it goes");
        brls::Logger::info("[bgaudio] background player: {} runs, process {}; asked for the "
                           "music again, not started anew",
                           BGPLAYER_TITLEID, hex(pid));
        return 0;
    }
    const int rcLoad = sceSysmoduleLoadModule(SCE_SYSMODULE_BG_APP_UTIL);
    int rc = -1;
    // As BGFTP asks, then as vitasdk documents it if that is refused.
    for (int mode = 0; mode <= 1 && rc < 0; mode++) {
        g_bgAppUsed = true;
        rc = sceBgAppUtilStartBgApp(mode);
        brls::Logger::info("[bgaudio] background player: load the module {}, start it with {}: {}",
                           hex(rcLoad), mode, hex(rc));
    }
    return rc;
}

// Music playing goes to the background player: the queue written, the CA
// bundle copied, the player asked for, and only then mpv paused and the BGM
// port given back, so that a refusal leaves the music playing here. The
// player plays once the app has gone, so the caller quits. On the UI thread.
// true when it was handed over.
bool handOver(const char* why) {
    if (!available() || g_outsideMusic == kOutsideStops || g_handedOverAt != 0) return false;
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    const bool playing = s.initialized && s.audioOnly &&
                         (s.state == MpvPlayerState::PLAYING ||
                          s.state == MpvPlayerState::BUFFERING ||
                          s.state == MpvPlayerState::LOADING);
    if (!playing || !MusicQueue::getInstance().isMusicQueue()) {
        // Which of the two: log b8cde643 said "no music playing" with mpv
        // playing a queue of downloaded songs that was not marked as music.
        brls::Logger::info("[bgaudio] background player: {}, {} ({}); nothing handed over", why,
                           playing ? "with a queue that is not music" : "with no music playing",
                           describeMpv(s));
        return false;
    }
    std::string what;
    if (!writeQueue(what)) {
        brls::Logger::error("[bgaudio] background player: {}, but the queue could not be written "
                            "to {}", why, BGPLAYER_QUEUE);
        return false;
    }
    copyCaBundle();
    const int rc = askForPlayer();
    if (rc < 0) {
        brls::Logger::error("[bgaudio] background player: refused ({}); the music plays on here",
                            hex(rc));
        return false;
    }
    MusicController::getInstance().playPause(false);
    releasePort(0, "handed to the background player");
    g_handedOverAt = sceKernelGetSystemTimeWide();
    quickMenuRelease("handed to the background player");
    brls::Logger::info("[bgaudio] background player: {}: handed over {}", why, what);
    return true;
}

// ── Taking the music back ────────────────────────────────────────────────

// A note as bgplayer.h lays them out: "key=value" lines, and "end" last. The
// track lines in order, split into their fields, in *tracks when asked for.
bool readNote(const char* path, std::map<std::string, std::string>& fields,
              std::vector<std::vector<std::string>>* tracks) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    bool whole = false;
    char buf[2048];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line == "end") {
            whole = true;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "track" && tracks) {
            std::vector<std::string> parts;
            std::size_t at = 0;
            for (;;) {
                const std::size_t bar = value.find('|', at);
                parts.push_back(value.substr(at, bar == std::string::npos ? bar : bar - at));
                if (bar == std::string::npos) break;
                at = bar + 1;
            }
            parts.resize(BGPLAYER_TRACK_FIELDS);
            tracks->push_back(parts);
        } else if (key != "track") {
            fields[key] = value;
        }
    }
    std::fclose(f);
    return whole;
}

double fieldNum(const std::map<std::string, std::string>& m, const char* key) {
    const auto it = m.find(key);
    return it == m.end() ? -1 : std::atof(it->second.c_str());
}

std::string fieldText(const std::map<std::string, std::string>& m, const char* key) {
    const auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

// The songs a player left as the system ended it, once nothing plays them.
void removePlayerSongs() {
    const SceUID d = sceIoDopen(BGPLAYER_SONGS);
    if (d < 0) return;
    SceIoDirent e;
    int removed = 0;
    for (;;) {
        std::memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        if (std::remove((std::string(BGPLAYER_SONGS) + "/" + e.d_name).c_str()) == 0) removed++;
    }
    sceIoDclose(d);
    if (removed)
        brls::Logger::info("[bgaudio] background player: removed {} song file(s) it left", removed);
}

// The song the shell plays on for a player the system ended as VitaPlex BG
// opened, as an app with extended memory does (BG Test 21, build 2012): its
// clock read twice, half a second apart, and its length checked against the
// player's song, so that the system Music app's song is left alone. Stopped
// when it is the player's. Where it was, or -1.
long long takeFromShell(int durationMs) {
    std::lock_guard<std::mutex> lock(g_shellMutex);
    if (g_shellInit) return -1;   // a test has the shell
    const int rcInit = sceMusicPlayerServiceInitialize(0);
    if (rcInit < 0) {
        brls::Logger::info("[bgaudio] background player: the system player would not start "
                           "({}); its song is left as it is", hex(rcInit));
        return -1;
    }
    SceMusicPlayerServicePlayStatusExtension a, b;
    std::memset(&a, 0, sizeof(a));
    std::memset(&b, 0, sizeof(b));
    sceMusicPlayerServiceGetPlayStatusExtension(&a);
    sceKernelDelayThread(500 * 1000);
    sceMusicPlayerServiceGetPlayStatusExtension(&b);
    const bool plays = b.currentTime != a.currentTime || b.currentState == 1;
    std::unique_ptr<SceMusicPlayerServiceTrackInfo> info(new SceMusicPlayerServiceTrackInfo());
    const int rcInfo = sceMusicPlayerServiceGetTrackInfo(info.get());
    const int off = info->duration - durationMs;
    const bool ours = rcInfo >= 0 && durationMs > 0 && off > -3000 && off < 3000;
    brls::Logger::info("[bgaudio] background player: the system player is at {} then {} (state "
                       "{}), with a song {} long against the player's {}",
                       mmss(a.currentTime), mmss(b.currentTime), b.currentState,
                       mmss((unsigned)std::max(0, info->duration)),
                       mmss((unsigned)std::max(0, durationMs)));
    long long at = -1;
    if (plays && ours) {
        at = b.currentTime;
        const int rcStop = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0);
        brls::Logger::info("[bgaudio] background player: its song plays on in the system "
                           "player; stopped there ({}) to go on here", hex(rcStop));
    }
    sceMusicPlayerServiceTerminate();
    return at;
}

// What the background player had got to as VitaPlex BG opens, taken back:
// a running player is asked to pause and says where; one the system ended
// left its song playing in the shell, which is stopped. The queue then plays
// on here from that song and place. Off the UI thread: it waits on the
// player and on the shell.
void takeBack() {
    std::map<std::string, std::string> status, queue;
    std::vector<std::vector<std::string>> tracks;
    const bool haveStatus = readNote(BGPLAYER_STATUS, status, nullptr);
    const bool haveQueue = readNote(BGPLAYER_QUEUE, queue, &tracks);
    SceUID pid = -1;
    const bool listed = sceAppMgrGetIdByName(&pid, BGPLAYER_TITLEID) >= 0;
    if (!haveStatus && !listed) return;   // never handed over, or long done

    int index = -1;
    long long at = -1;
    if (listed) {
        // It stays, paused: it must not end while the app is open.
        const double asked = systemSecs();
        ask(BGPLAYER_ASK_PAUSE, "VitaPlex BG is open again and takes the music back");
        brls::Logger::info("[bgaudio] background player: {} runs, process {}; asked to pause",
                           BGPLAYER_TITLEID, hex(pid));
        bool taken = false;
        for (int i = 0; i < 50 && !taken; i++) {
            sceKernelDelayThread(100 * 1000);
            taken = !fileExists(BGPLAYER_ASK_PAUSE);
        }
        if (!taken) {
            std::remove(BGPLAYER_ASK_PAUSE);
            brls::Logger::warning("[bgaudio] background player: it did not take the request in "
                                  "5 s; the music is left to it");
            return;
        }
        status.clear();
        readNote(BGPLAYER_STATUS, status, nullptr);
        brls::Logger::info("[bgaudio] background player: paused, {} of song {} ({})",
                           mmss((unsigned)std::max(0.0, fieldNum(status, "positionMs"))),
                           (int)fieldNum(status, "index") + 1, fieldText(status, "state"));
        if (fieldText(status, "state") == BGPLAYER_STATE_PAUSED &&
            fieldNum(status, "written") >= asked && fieldNum(status, "wasPlaying") == 1) {
            index = (int)fieldNum(status, "index");
            at = (long long)fieldNum(status, "positionMs");
        }
    } else {
        // The system clock starts again with the Vita, so a status written
        // later than now is from before it last started: one read as written
        // 1101 s after VitaPlex BG started (log fbc54add). Nothing of it plays.
        if (fieldNum(status, "written") > systemSecs() + 1.0) {
            brls::Logger::info("[bgaudio] background player: not running; its status (\"{}\") is "
                               "from before the Vita last started",
                               fieldText(status, "state"));
            removePlayerSongs();
            return;
        }
        // SceShell carrying the queue on (qm_protocol.h) was told at init to
        // stop, and says where it stopped: its status is read again until it
        // is one written after that.
        if (fieldText(status, "by") == "SceShell") {
            for (int i = 0; i < 20 && g_takeBackSent && fieldNum(status, "written") < g_initSecs &&
                            fieldText(status, "state") == BGPLAYER_STATE_PLAYING;
                 i++) {
                sceKernelDelayThread(100 * 1000);
                status.clear();
                readNote(BGPLAYER_STATUS, status, nullptr);
            }
            brls::Logger::info("[bgaudio] background player: SceShell carried its queue on, to "
                               "song {} at {} ({}), {:.1f} s after VitaPlex BG started",
                               (int)fieldNum(status, "index") + 1,
                               mmss((unsigned)std::max(0.0, fieldNum(status, "positionMs"))),
                               fieldText(status, "state"),
                               fieldNum(status, "written") - g_initSecs);
        }
        // Ended by the system as this app opened, the player said it played
        // moments before; it writes every 2 s while it does. SceShell writes
        // the same way as it carries the queue on.
        const double age = g_initSecs - fieldNum(status, "written");
        brls::Logger::info("[bgaudio] background player: not running; it last said \"{}\", {:.1f} "
                           "s before VitaPlex BG started",
                           fieldText(status, "state"), age);
        const bool said = fieldText(status, "state") == BGPLAYER_STATE_PLAYING;
        if (said && age < 20.0) {
            at = takeFromShell((int)fieldNum(status, "durationMs"));
            if (at >= 0) index = (int)fieldNum(status, "index");
        }
        // Not while the shell may still play one of them: a file written over
        // as the shell played it preceded a crash (logs 8fe35d97, 7ba90482).
        // The next player clears what is left as it starts.
        if (!said || at >= 0) removePlayerSongs();
    }
    if (index < 0 || at < 0) return;
    if (!haveQueue || fieldNum(queue, "written") != fieldNum(status, "queue") ||
        index >= (int)tracks.size()) {
        brls::Logger::warning("[bgaudio] background player: its queue at {} is not the one it "
                              "played; nothing to go on with",
                              BGPLAYER_QUEUE);
        return;
    }

    std::vector<MediaItem> items;
    for (const auto& t : tracks) {
        MediaItem item;
        item.ratingKey = t[BGPLAYER_TRACK_KEY];
        item.duration = std::atoi(t[BGPLAYER_TRACK_DURATION].c_str());
        item.userRating = (float)std::atof(t[BGPLAYER_TRACK_RATING].c_str());
        item.thumb = t[BGPLAYER_TRACK_THUMB];
        item.title = t[BGPLAYER_TRACK_TITLE];
        item.grandparentTitle = t[BGPLAYER_TRACK_ARTIST];
        item.parentTitle = t[BGPLAYER_TRACK_ALBUM];
        item.type = "track";
        item.mediaType = MediaType::MUSIC_TRACK;
        items.push_back(item);
    }
    const int repeat = (int)fieldNum(queue, "repeat");
    brls::sync([items, index, at, repeat]() {
        const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
        if (s.initialized && s.state != MpvPlayerState::IDLE && s.state != MpvPlayerState::ENDED &&
            s.state != MpvPlayerState::ERROR) {
            brls::Logger::info("[bgaudio] background player: VitaPlex plays something already; "
                               "the queue is not taken back");
            return;
        }
        MusicQueue::getInstance().setRepeatMode(repeat == BGPLAYER_REPEAT_ONE ? RepeatMode::ONE
                                                : repeat == BGPLAYER_REPEAT_ALL ? RepeatMode::ALL
                                                                                : RepeatMode::OFF);
        brls::Logger::info("[bgaudio] background player: the queue goes on here, \"{}\" at {}",
                           items[(std::size_t)index].title, mmss((unsigned)at));
        brls::Application::pushActivity(
            PlayerActivity::createWithRestoredQueue(items, index, at));
    });
}

}  // namespace

bool available() {
    static const bool bg = runningTitleId() == VITAPLEX_BG_TITLEID;
    return bg;
}

void init(int outsideMusic) {
    // A setting saved where it was offered counts as off in VitaPlex itself.
    if (outsideMusic < kOutsideStops || outsideMusic > kOutsideHandsOver || !available())
        outsideMusic = kOutsideStops;
    g_outsideMusic = outsideMusic;
    g_keepPlaying = outsideMusic != kOutsideStops;
    g_initSecs = systemSecs();
    qmInit();
    // SceShell may be carrying the background player's queue on (qm_link.h):
    // it stops before anything here plays, and writes where it got to for
    // takeBack. A plugin not in SceShell is not loaded for this.
    if (available()) {
        const int rc = qmTakeBack();
        g_takeBackSent = rc >= 0;
        if (g_takeBackSent)
            brls::Logger::info("[bgaudio] quick menu plugin told VitaPlex BG takes the music "
                               "back ({})",
                               hex(rc));
    }
    brls::Logger::info("[bgaudio] running as {}; music after leaving: {}",
                       available() ? "VitaPlex BG" : "VitaPlex",
                       outsideMusic == kOutsidePlaysOn     ? "plays on, then the background player"
                       : outsideMusic == kOutsideHandsOver ? "the background player at once"
                                                           : "stops");
    // Whatever the setting, and in either package: the watcher is also what
    // answers the system's request to quit (see onRequestQuit and
    // watcherMain). Off, that is all it does.
    startWatcher(g_keepPlaying);
    // Whatever the setting too: music handed over before it was changed
    // comes back all the same. In the first frame, once the server is
    // connected and the main screen is up; the song plays on in the shell
    // until then.
    if (available()) {
        brls::sync([]() { platform::launchThread([]() { takeBack(); }); });
    }
}

void setOutsideMusic(int mode) {
    if (!available()) return;
    if (mode < kOutsideStops || mode > kOutsideHandsOver) mode = kOutsideStops;
    g_outsideMusic = mode;
    const bool on = mode != kOutsideStops;
    g_keepPlaying = on;
    brls::Logger::info("[bgaudio] music after leaving set to {}", mode);
    if (on) {
        startWatcher();
        if (MpvPlayer::getInstance().isPlaying()) holdPortForMpv("mpv, already playing");
    } else if (g_ownPortTest == 0) {
        releasePort(kPortForApp, "setting turned off");
    }
}

int outsideMusic() { return g_outsideMusic; }

bool keepPlaying() { return g_keepPlaying; }

void exitProcess(int code) {
    if (g_bgAppUsed) vitaplexRawExit(code);
    sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000 * 1000);
}

void onAudioPlayback(bool playing) {
    // Held from the first play and through pauses, like ElevenMPV-A; it goes
    // when the player does, or when the setting is turned off. The port test
    // takes it its own way, whatever the setting.
    if (playing && (g_keepPlaying || g_ownPortTest != 0)) holdPortForMpv("mpv playback");
}

void onTrackLoad() { g_loads++; }

void onPlayerShutdown() {
    const int priority = kOwnPort[g_ownPortTest];
    releasePort(priority != 0 ? priority : kPortForApp, "player shut down");
}

int ownPortTest() { return g_ownPortTest; }

void setOwnPortTest(int choice) {
    if (choice < 0 || choice >= kOwnPortChoices) choice = 0;
    g_ownPortTest = choice;
    startWatcher();   // for the lines after PS is held
    const int priority = kOwnPort[choice];
    const int test = shellTestPort();
    brls::Logger::info("[bgaudio] port test: mpv takes the BGM port {}{}; system player tests {}; "
                       "the background player {}",
                       priority != 0 ? portName(priority) : "not at all",
                       choice == 0 ? ", while background playback is on" : "",
                       test != 0 ? portName(test) : "none", playerPortName());
    if (!MpvPlayer::getInstance().isPlaying()) return;
    if (choice != 0 || g_keepPlaying)
        holdPortForMpv("port test changed");
    else
        releasePort(0, "port test changed");
}

void setQuickMenuControls(bool on) {
    if (!available()) return;
    g_qmEnabled = on;
    brls::Logger::info("[bgaudio] quick menu controls {}",
                       on ? "on, through VitaPlex's quick menu plugin" : "off");
    if (on) {
        startWatcher();
    } else if (g_qmActive.exchange(false)) {
        brls::Logger::info("[bgaudio] quick menu: row given back, turned off: {}",
                           hex(qmDeactivate()));
    }
}

int quickMenuLook() {
    return g_qmLook;
}

void setQuickMenuLook(int look) {
    if (look < 0 || look >= QM_LOOK_COUNT) return;
    g_qmLook = look;   // the watcher sends it to the plugin
}

void quickMenuNowPlaying(const std::string& title, const std::string& artist,
                         const std::string& album, const std::string& artUrl, bool playing) {
    if (!available()) return;
    std::lock_guard<std::mutex> lock(g_qmMutex);
    if (!g_qmSong.wanted || title != g_qmSong.title || artist != g_qmSong.artist ||
        album != g_qmSong.album)
        g_qmSong.serial++;
    g_qmSong.title = title;
    g_qmSong.artist = artist;
    g_qmSong.album = album;
    g_qmSong.artUrl = artUrl;
    g_qmSong.playing = playing;
    g_qmSong.wanted = true;
}

void quickMenuNowPlayingCleared() {
    if (!available()) return;
    std::lock_guard<std::mutex> lock(g_qmMutex);
    g_qmSong.wanted = false;
}

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
            holdPortForTest();
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
        holdPortForTest();
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
            bool linkHeld = false;     // a link attempt VitaPlex was held through, twice
        };
        int repeated = 0;   // attempts made again because VitaPlex was held
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
                    // is as long and had not played 3 s; the second can. A
                    // try VitaPlex was held through is made again.
                    for (int k = 0; k < 3; k++) {
                        run.disk = shellTry(tag + ", from disk", diskFile, false, sv, fromDisk);
                        if (!run.disk.unsure && !run.disk.interrupted) break;
                        if (run.disk.interrupted && k < 2) repeated++;
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
                        // Connections are counted over every try: one made
                        // while VitaPlex was held still happened.
                        const int connBefore = server->connections();
                        const int tlsBefore = server->tlsHandshakes();
                        const long long sentBefore = server->bytesSent();
                        const std::string url = std::string(link.https ? "https://" : "http://") +
                                                link.host + ":" + std::to_string(port) + server->path();
                        const std::string label = tag + (link.https ? ", https link" : ", http link") +
                                                  (link.host == "127.0.0.1" ? "" : " via " + link.host);
                        ShellTry r;
                        for (int k = 0; k < 2; k++) {
                            r = shellTry(label, url, false, sv, onLink);
                            if (!r.interrupted) break;
                            if (k == 0) repeated++;
                        }
                        const int conns = server->connections() - connBefore;
                        const int tls = server->tlsHandshakes() - tlsBefore;
                        const long long kb = (server->bytesSent() - sentBefore) / 1024;
                        if (r.interrupted && conns == 0) {
                            run.linkHeld = true;
                        } else if (conns == 0) {
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

        std::vector<std::string> fetchedHttp, triedHttps, fromDiskNames, noClock, notSeen, held;
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
            if (run.linkHeld && !run.fetchedHttp && !run.triedHttps) held.push_back(n);
            else if (run.disk.played) fromDiskNames.push_back(n);
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
            if (g_testPort != 0) releasePort(g_testPort, "system player test over");
        }

        std::string report = "System player, streaming \"" + title + "\"\n\n";
        for (const auto& l : lines) report += l + "\n";
        report += "\n";
        if (repeated > 0)
            report += "Leaving VitaPlex holds it, so " + std::to_string(repeated) +
                      (repeated == 1 ? " step was" : " steps were") + " run again. ";
        if (!held.empty())
            report += joinNames(held) + ": VitaPlex was in the background during a link twice, so "
                      "what " + (held.size() == 1 ? "that service does" : "those services do") +
                      " with links is not known. Run the test again and stay in VitaPlex. ";
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
    const int port = g_testPort;
    lock.unlock();
    if (port != 0) releasePort(port, "system player test over");
    return "System player stopped.";
}

// ── SceShell playing a file itself ───────────────────────────────────────
//
// The background player plays through the shell's music service from a
// process of its own, which the system ends as a game with extended memory
// starts, and the queue stops after the song then playing. SceShell is never
// ended: the first step of moving the queue into it is whether VitaPlex's
// quick menu plugin, inside SceShell, can drive the same service
// (QM_CMD_MUSIC_TEST). The port is held as for the tests above, as the
// background player holds it. The plugin plays for QM_MUSIC_TEST_S at most
// and logs as it goes; its lines are read back from its log.

// The plugin's "music test:" lines written past `from` bytes, and whether
// the last of them says it is done.
std::string pluginTestLines(long from, bool* finished) {
    std::string out;
    FILE* f = std::fopen(QM_PLUGIN_LOG, "rb");
    if (!f) return out;
    std::fseek(f, from, SEEK_SET);
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (!std::strstr(line, "music test:")) continue;
        out += line;
        if (std::strstr(line, "music test: done")) *finished = true;
    }
    std::fclose(f);
    return out;
}

long fileLength(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n > 0 ? n : 0;
}

void runPluginMusicTest(Report done) {
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
    platform::launchThread([done, path]() {
        const std::string title = "SceShell plays a file itself (an experiment)\n\n" +
                                  baseName(path) + "\n\n";
        int port;
        {
            std::lock_guard<std::mutex> lock(g_shellMutex);
            holdPortForTest();
            port = g_testPort;
        }
        char what[320] = "";
        int rc = 0;
        if (!qmIsOpen()) {
            rc = qmOpen(what, sizeof(what));
            brls::Logger::info("[bgaudio] SceShell music test: {} ({})", what, hex(rc));
        }
        if (rc < 0 || !qmIsOwn()) {
            if (port != 0) releasePort(port, "SceShell music test not run");
            finishTest(done, title + "VitaPlex's quick menu plugin is not in SceShell" +
                                 (what[0] ? std::string(": ") + what : std::string(".")));
            return;
        }
        const long from = fileLength(QM_PLUGIN_LOG);
        rc = qmMusicTest(path.c_str());
        brls::Logger::info("[bgaudio] SceShell music test: {} sent to the plugin, the BGM port "
                           "held {}: {}",
                           path, port != 0 ? portName(port) : "by nothing of VitaPlex's",
                           hex(rc));
        std::string lines;
        bool finished = false;
        for (int i = 0; rc >= 0 && i < (QM_MUSIC_TEST_S + 15) * 2 && !finished; i++) {
            sceKernelDelayThread(500 * 1000);
            lines = pluginTestLines(from, &finished);
        }
        if (port != 0) releasePort(port, "SceShell music test over");
        brls::Logger::info("[bgaudio] SceShell music test, the plugin's lines:\n{}", lines);
        std::string report = title;
        if (rc < 0)
            report += "The plugin could not be asked: " + hex(rc);
        else
            report += lines.empty() ? std::string("Nothing from the plugin.") : lines;
        if (rc >= 0 && !finished)
            report += "\n\nThe plugin has not said it is done: its last line is the step it "
                      "stopped at.";
        finishTest(done, report);
    });
}

// ── BG Test 11 ───────────────────────────────────────────────────────────
//
// BG Test 10 showed a small system-mode app downloading songs from the server
// and handing them to the shell, on through a game. Here VitaPlex starts that
// app itself, with the song it is playing and where it paused it, and the app
// carries on from there (src/bgtest_psv/player_test.c, as VPLXBGT11). The
// watcher's activate and deactivate events say whether VitaPlex stayed in
// front.

void runHandoffTest(Report done) {
    static const char* const kHelper = "VPLXBGT11";
    const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
    if (!track || track->ratingKey.empty()) {
        done("Play a song first: this hands it to BG Test 11.");
        return;
    }
    if (!fileExists(std::string("ux0:app/") + kHelper + "/eboot.bin")) {
        done("Install VitaPlexBgTest-VPLXBGT11.vpk first. It is in the build's "
             "VitaPlex-vita-bgtest artifact.");
        return;
    }
    startWatcher();

    // Where in the song: mpv's clock counts from where its stream begins, which
    // a seek that restarted the transcode moved part-way in.
    const double position = MpvPlayer::getInstance().getPosition();
    const long long streamStartMs = MusicController::getInstance().streamStartOffsetMs();
    pauseMpvForShell();
    const unsigned positionMs =
        (unsigned)std::max(0LL, streamStartMs + (position > 0 ? (long long)(position * 1000) : 0));
    const QueueItem* next = MusicQueue::getInstance().peekNextTrack();
    const std::string title = track->title;
    const std::string ratingKey = track->ratingKey;
    const std::string nextKey = next ? next->ratingKey : std::string();

    // The helper takes the BGM port and the shell's music player for itself,
    // so VitaPlex lets go of both: the port it holds through a pause, and a
    // file a system player test left playing.
    {
        std::unique_lock<std::mutex> lock(g_shellMutex, std::try_to_lock);
        if (lock.owns_lock() && g_shellInit) stopShellLocked("handed to BG Test 11");
    }
    releasePort(0, "handed to BG Test 11");

    // One line a field, as the helper's readHandoff() reads them.
    const std::string path = platformPath("bgtest11-handoff.txt");
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        done("Could not write " + path + ".");
        return;
    }
    std::fprintf(f, "ratingKey=%s\npositionMs=%u\nnext=%s\nwritten=%ld\n", ratingKey.c_str(),
                 positionMs, nextKey.c_str(), (long)std::time(nullptr));
    std::fclose(f);

    // 0x20000 first: per vita-headers, the one value that does not open the
    // LiveArea, so the one that may leave VitaPlex in front. If it is refused,
    // 0xFFFFF, which VitaPlex's updater launch uses.
    int flags = 0x20000;
    int rc = vita::launchTitleWith(kHelper, flags);
    if (rc < 0) {
        brls::Logger::info("[bgaudio] BG Test 11: launch with 0x20000: {}; trying 0xFFFFF",
                           hex(rc));
        flags = 0xFFFFF;
        rc = vita::launchTitleWith(kHelper, flags);
    }
    brls::Logger::info("[bgaudio] BG Test 11: handed over \"{}\" ({}) at {}, then {}; launch "
                       "with {:#x}: {}",
                       title, ratingKey, mmss(positionMs), nextKey.empty() ? "nothing" : nextKey,
                       flags, hex(rc));
    if (rc < 0) {
        std::remove(path.c_str());   // or BG Test 11, opened later, would play it
        done("BG Test 11 did not start (" + hex(rc) + "). The song is paused here; play it to "
             "carry on.");
        return;
    }
    done("Handed \"" + title + "\" to BG Test 11 at " + mmss(positionMs) + ". In a few seconds "
         "it should go on from there through the system player" +
         (nextKey.empty() ? std::string("") : std::string(", then play the next song")) +
         ", and then quit. Note whether VitaPlex stayed on screen. Then press PS and start a "
         "game. Afterwards, send bgtest-VPLXBGT11.log and vitaplex.log.");
}

}  // namespace bgaudio
}  // namespace vitaplex
