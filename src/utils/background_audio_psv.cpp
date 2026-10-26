/**
 * VitaPlex - music after leaving the app, Vita backend
 *
 * See utils/background_audio.hpp for what it does. A watcher thread reads the
 * system's events for the app: leaving it with PS, by the setting, and the
 * system asking it to quit, as it does to start a game, hand the queue to
 * SceShell; it also keeps the quick menu's Music row up to date. What it does
 * goes to the log under "[bgaudio]", with the system's return codes.
 */

#include "utils/background_audio.hpp"

#include "activity/player_activity.hpp"
#include "app/downloads_manager.hpp"
#include "app/music_controller.hpp"
#include "app/music_queue.hpp"
#include "app/plex_client.hpp"
#include "platform/platform.hpp"
#include "player/mpv_player.hpp"
#include "app/application.hpp"
#include "utils/http_client.hpp"
#include "utils/now_playing.hpp"
#include "utils/shell_companion_psv.hpp"

#include <borealis.hpp>

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "handover.h"
#include "mp3_frames.h"
#include "sc_cover.h"
#include "shell_companion.h"

#include "ShellAudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
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
// patches/psv_platform.cpp's: the main loop's pace while VitaPlex is left.
void vitaplex_set_app_away(bool away);
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
constexpr int kEventRequestQuit = 0x20000001;

// The priority ElevenMPV-A asks for while it decodes in its own process.
constexpr int kPortForApp = 0x81;

constexpr SceUInt kTickUs = 250 * 1000;
// How long the normal way out gets after the system asks the app to quit,
// before the watcher ends the process itself. Short while the UI thread has
// not even taken the request, since then it is stuck. Longer once it has, so
// a shutdown already under way is not cut off: it rewrites settings.json in
// place, and ending the process mid-write would lose the user's login. A
// device log (280b1532) had the whole of it take 1.2 s, the write at 1.14 s.
constexpr SceInt64 kQuitGraceUs    = 2 * 1000000;
constexpr SceInt64 kQuitShutdownUs = 10 * 1000000;

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

std::string describeMpv(const MpvPlayer::Snapshot& s) {
    if (!s.initialized) return "mpv not running";
    char b[96];
    std::snprintf(b, sizeof(b), "mpv %s at %.1fs%s", stateName(s.state), s.position,
                  s.audioOnly ? "" : " (video mode)");
    return b;
}

bool mpvPlays(const MpvPlayer::Snapshot& s) {
    return s.initialized && (s.state == MpvPlayerState::PLAYING ||
                             s.state == MpvPlayerState::BUFFERING ||
                             s.state == MpvPlayerState::LOADING);
}

std::string lowerExt(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool fileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::atomic<bool> g_keepPlaying{false};
std::atomic<int> g_outsideMusic{kOutsideStops};   // the setting (OutsideMusic)
std::atomic<bool> g_offlineSkip{true};            // the setting (setOfflineSkip)

// ── Handing the music over (see "Handing the music to SceShell" below) ───
// When the queue was handed over, on the way out; 0 before. Past
// kQuitShutdownUs after it the watcher ends the process itself.
std::atomic<SceInt64> g_handedOverAt{0};
// The last press of PS (the power callback): a deactivation within 2 s of
// one is the user leaving, as BG Tests 20 and 21 took it.
std::atomic<SceInt64> g_psPressUs{0};
// The queue SceShell carries on, as takeBack read it for the player it set
// up, which takes the music back once ready (takeBackNow): the queue's
// "written", its songs' ratingKeys, and whether it repeats all.
struct ShellQueue {
    double written = -1;
    std::vector<std::string> keys;
    bool repeatAll = false;
};
std::mutex g_shellQueueMutex;
ShellQueue g_shellQueue;
bool handOver(const char* why);
void dropReadySong();

// ── The quick menu's Music row (shell_companion.h) ───────────────────────
// What MusicController publishes, which now_playing.cpp hands here on the
// Vita, for the watcher to put on the row through vita-shell-companion's
// plugin. The row is this process's while it has music to show; it goes to
// SceShell with the queue, and back to the system as the app goes.
std::atomic<bool> g_qmEnabled{false};   // the setting
std::atomic<bool> g_qmActive{false};    // the plugin has the row for this process
std::atomic<bool> g_qmGone{false};      // handed over or quitting: not taken again
// Held for each quickMenuTick, and as the queue goes to SceShell, so that no
// heartbeat (SC_CMD_ROW_ACTIVATE) of this process's follows SC_CMD_MUSIC_PLAY:
// the plugin leaves its music to any process that takes the row.
std::mutex g_qmTickMutex;
std::mutex g_qmMutex;
struct QmSong {
    std::string title, artist, album;
    std::string artUrl;    // its cover (now_playing.hpp), "" for none
    bool playing = false;
    bool wanted = false;   // published and not cleared since
    unsigned serial = 0;   // one more each time there is new text to send
};
QmSong g_qmSong;   // under g_qmMutex

// ── The song's cover on the row (SC_CMD_ROW_ART) ─────────────────────────
// Made by a thread of its own, as the watcher asks for it: Plex's thumbnail
// through its transcoder at twice the size the row shows, or a download's
// local copy, made into the PNG the plugin loads (sc_cover.h). The watcher
// sends its serial once written.
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
        req.url = withThumbSize(source, SC_ART_SIZE * 2);
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
    return scCoverWrite(bytes.data(), bytes.size(), serial, why, sizeof(why)) == 0 ? "" : why;
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
            serial = scCoverNextSerial(last, start);
            const std::string failed = makeQmArt(source, serial);
            if (failed.empty()) {
                last = serial;
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
// handed to SceShell, whose plugin takes the row itself as it plays it, or
// the app going. The plugin, left on, would keep putting presses into a
// pipe no process reads, and SceShell waits once that pipe is full.
void quickMenuRelease(const char* why) {
    g_qmGone = true;
    if (!g_qmActive.exchange(false)) return;
    brls::Logger::info("[bgaudio] quick menu: row given back ({}): {}", why, hex(scRowRelease()));
}

// ── The BGM port ─────────────────────────────────────────────────────────
// Held at kPortForApp while mpv plays music that is to go on after the app
// is left, as ElevenMPV-A holds it for its own decoders.
std::mutex g_portMutex;
bool g_portHeld = false;   // under g_portMutex

void holdPort(const char* why) {
    std::lock_guard<std::mutex> lock(g_portMutex);
    if (g_portHeld) return;
    const int rc = sceAppMgrAcquireBgmPortWithPriority(kPortForApp);
    brls::Logger::info("[bgaudio] BGM port: acquire for {}: {}", why, hex(rc));
    g_portHeld = rc >= 0;
}

void releasePort(const char* why) {
    std::lock_guard<std::mutex> lock(g_portMutex);
    if (!g_portHeld) return;
    const int rc = sceAppMgrReleaseBgmPort();
    brls::Logger::info("[bgaudio] BGM port: release ({}): {}", why, hex(rc));
    g_portHeld = false;
}

// ── The watcher ──────────────────────────────────────────────────────────

// Left for the LiveArea or another app. A video pauses: it plays to nobody
// once the app is not in front. Music stops too, by the setting; or, by the
// other, goes to SceShell as VitaPlex closes, if it was PS that left it. On
// the UI thread, which owns the player and the queue, and runs on in a
// non-game app sent away (log a93baa71).
void onDeactivate(SceInt64 now) {
    // Its screen unseen meanwhile, the main loop goes a few times a second
    // (patches/psv_platform.cpp), the queue going on all the same: it went
    // round 96 times in 4 s away (log 4e8bcb05).
    vitaplex_set_app_away(true);
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    brls::Logger::info("[bgaudio] deactivated (left for the LiveArea or another app): {}",
                       describeMpv(s));
    if (!mpvPlays(s)) return;
    if (!s.audioOnly) {
        brls::sync([]() {
            brls::Logger::info("[bgaudio] the video paused, with VitaPlex left");
            MpvPlayer::getInstance().pause();
        });
    } else if (g_outsideMusic == kOutsideStops) {
        brls::sync([]() {
            brls::Logger::info("[bgaudio] the music paused, with VitaPlex left");
            MusicController::getInstance().playPause(false);
        });
    } else if (g_outsideMusic == kOutsideHandsOver && now - g_psPressUs.load() < 2000000) {
        // SceShell takes the queue over and VitaPlex goes, so that a game
        // started next finds nothing to close.
        brls::sync([]() {
            if (handOver("left with PS")) brls::Application::quit();
        });
    }
}

// In front again: the main loop at its own pace.
void onActivate() {
    vitaplex_set_app_away(false);
    brls::Logger::info("[bgaudio] activated (in front again)");
}

// When the system asked the app to quit; 0 until it has. Watcher thread only.
SceInt64 g_quitAskedAt = 0;
// Set on the UI thread once it has taken the request and begun the normal
// shutdown; see kQuitShutdownUs.
std::atomic<bool> g_quitUnderway{false};

void onRequestQuit(SceInt64 now) {
    brls::Logger::info("[bgaudio] the system asked the app to quit; quitting");
    quickMenuRelease("the app is going");
    releasePort("app quitting");

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
    // Music playing goes to SceShell on the way, by the setting: this is what
    // a game started over VitaPlex brings.
    if (g_quitAskedAt == 0) {
        g_quitAskedAt = now;
        brls::sync([]() {
            g_quitUnderway = true;
            if (!handOver("the system asked VitaPlex to quit")) dropReadySong();
            brls::Application::quit();
        });
    }
}

void drainAppEvents(SceInt64 now) {
    int n = 0;
    const int rc = sceAppMgrReceiveEventNum(&n);
    if (rc < 0 || n <= 0) return;
    for (int i = 0; i < n; i++) {
        AppEvent ev;
        std::memset(&ev, 0, sizeof(ev));
        if (sceAppMgrReceiveEvent(&ev) < 0) break;
        if (ev.event == kEventDeactivate) onDeactivate(now);
        else if (ev.event == kEventActivate) onActivate();
        else if (ev.event == kEventRequestQuit) onRequestQuit(now);
    }
}

int powerCallback(int, int, int powerInfo, void*) {
    // Each logged: build 2047 dropped it, and log 7c38c1fa could not say
    // whether the power button or PS was pressed as its log stopped.
    std::string what;
    const struct {
        unsigned bit;
        const char* name;
    } kBits[] = {
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
    if ((unsigned)powerInfo & SCE_POWER_CB_BUTTON_PS_PRESS)
        g_psPressUs = sceKernelGetSystemTimeWide();
    // Back from standby: the main loop at its own pace, should the system
    // send no activate event for it (no log has VitaPlex in front through a
    // standby). One that is still away then draws at its pace until left
    // again: wasted, never slow.
    if ((unsigned)powerInfo & (SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_APP_RESUMING))
        vitaplex_set_app_away(false);
    return 0;
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
    static int sentPlaying = -1;
    static int64_t sentArt = -1;   // the cover's serial sent, -1 for none yet
    std::lock_guard<std::mutex> tickLock(g_qmTickMutex);
    if (!g_qmEnabled || g_qmGone) return;
    QmSong song;
    {
        std::lock_guard<std::mutex> lock(g_qmMutex);
        song = g_qmSong;
    }
    const bool wanted = song.wanted && g_handedOverAt == 0;
    if (!scIsOpen()) {
        if (!wanted || (lastTry != 0 && now - lastTry < 30 * 1000000LL)) return;
        lastTry = now;
        if (!shellcomp::open("[bgaudio] quick menu")) return;
    }
    if (wanted && !g_qmActive) {
        // A press left from whoever had the row before is not one for this.
        while (scPoll() != SC_NONE) {
        }
        const int rc = scRowActivate();
        if (rc < 0) {
            brls::Logger::info("[bgaudio] quick menu: row not taken: {}", hex(rc));
            return;
        }
        g_qmActive = true;
        sentSerial = song.serial - 1;   // so the song goes again
        sentPlaying = -1;
        sentArt = -1;
        lastBeat = now;
    } else if (!wanted && g_qmActive) {
        g_qmActive = false;
        scRowRelease();
        return;
    }
    if (!g_qmActive) return;
    // The plugin gives the row back by itself once these stop.
    if (now - lastBeat >= 1000000) {
        lastBeat = now;
        scRowActivate();
    }
    if (song.serial != sentSerial && scRowSong(song.title.c_str(), song.artist.c_str(),
                                               song.album.c_str()) >= 0)
        sentSerial = song.serial;
    const int playing = song.playing ? 1 : 0;
    if (playing != sentPlaying && scRowPlaying(playing) >= 0) sentPlaying = playing;
    // The cover: the last one stays on the row while the next is made.
    uint32_t art = 0;
    if (qmArtFor(song.artUrl, &art) && (int64_t)art != sentArt && scRowArt(art) >= 0)
        sentArt = art;
    for (int press; (press = scPoll()) != SC_NONE;) {
        const nowplaying::Transport t = press == SC_NEXT       ? nowplaying::Transport::Next
                                        : press == SC_PREVIOUS ? nowplaying::Transport::Previous
                                                               : nowplaying::Transport::Toggle;
        brls::sync([t]() { nowplaying::dispatchTransport(t); });
    }
}

int watcherMain(SceSize, void*) {
    const SceUID cb = sceKernelCreateCallback("VitaPlexBgAudioPower", 0, powerCallback, nullptr);
    const int rcCb = cb >= 0 ? scePowerRegisterCallback(cb) : cb;
    if (rcCb < 0) brls::Logger::error("[bgaudio] no power callback: {}", hex(rcCb));
    for (;;) {
        // The CB variant is what runs the power callback on this thread.
        sceKernelDelayThreadCB(kTickUs);
        const SceInt64 now = sceKernelGetSystemTimeWide();
        drainAppEvents(now);
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
        // Handed over and quitting by itself: SceShell has the music.
        const SceInt64 handedAt = g_handedOverAt;
        if (handedAt != 0 && now - handedAt > kQuitShutdownUs) {
            brls::Logger::warning("[bgaudio] still running {:.1f}s after handing the music to "
                                  "SceShell; ending the process",
                                  (now - handedAt) / 1e6);
            exitProcess(0);
        }
        quickMenuTick(now);
    }
    return 0;
}

// The watcher, whatever the settings: it is also what answers the system's
// request to quit, which the non-game package hangs the console without
// (onRequestQuit).
void startWatcher() {
    static bool started = false;
    if (started) return;
    started = true;
    const SceUID thid = sceKernelCreateThread("VitaPlexBgAudio", watcherMain, 0x10000100, 0x10000,
                                              0, 0, nullptr);
    const int rc = thid >= 0 ? sceKernelStartThread(thid, 0, nullptr) : thid;
    if (rc < 0) brls::Logger::error("[bgaudio] could not start the watcher: {}", hex(rc));
}

// ── Handing the music to SceShell ────────────────────────────────────────
//
// The system closes VitaPlex to start a game, and SceShell, which no app or
// game closes, carries the queue on: vita-shell-companion's plugin plays it
// there through the shell's music player, which plays on beside games
// (docs/DESIGN_NOTES.md, BG Tests 10 to 21 and builds 2032 to 2047), and
// fetches the songs not on the card as they come up. The two talk through the
// files of the plugin's protocol in ux0:data/VitaPlex (handover.h) and its
// pipes (shell_companion.h). The app hands the queue over as it goes and
// takes the music back as it opens again.

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

// A field of a track line: '|' separates them, and a line ends the track.
std::string trackField(std::string s) {
    for (auto& c : s)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

int repeatCode(RepeatMode m) {
    return m == RepeatMode::ONE ? HANDOVER_REPEAT_ONE
         : m == RepeatMode::ALL ? HANDOVER_REPEAT_ALL : HANDOVER_REPEAT_OFF;
}

// ── The songs' files in the folder of songs ───────────────────────────────
// SceShell plays files, so a song handed over that is not on the card is
// fetched whole before it plays: 5.8 s and 5.0 s of silence in build 2045's
// run (log 5ae2ab41), each time for the song VitaPlex had been playing. So
// while a song plays here and could be handed over, its file is fetched in
// the background to the folder of songs, as SceShell would fetch it, and the
// queue names it (file=) once whole: SceShell plays it at once. A download
// is left alone, since SceShell plays it where it is. A song in a format the
// shell does not play comes as the server's MP3 of it, as SceShell would
// fetch that: it waited 23.3 s for one (log 8e7a4b23).
//
// The files stay while their songs are near: the song before, for previous,
// the one playing, and the next, which SceShell fetches ahead. VitaPlex plays
// them too, once it has the music back (songFile): its stream of a song
// whose file it had took 7.3 s to begin (log 8e7a4b23). Files of other songs
// go as the queue moves (pruneSongs). A fetch for a song no longer playing is
// stopped.
std::mutex g_readyMutex;
struct ReadySong {
    std::string key;    // the song playing here, "" for none
    std::string path;   // its file, once whole; "" until then
    unsigned gen = 0;   // one more for each song, and as the music goes
};
ReadySong g_ready;      // under g_readyMutex

unsigned readyGen() {
    std::lock_guard<std::mutex> lock(g_readyMutex);
    return g_ready.gen;
}

long long sizeOnCard(const std::string& path) {
    SceIoStat st;
    std::memset(&st, 0, sizeof(st));
    return sceIoGetstat(path.c_str(), &st) >= 0 ? (long long)st.st_size : -1;
}

// The whole file of the song at key in the folder of songs, under the names
// SceShell and VitaPlex give them (<key>.<extension>, moved in once whole);
// "" for none.
std::string songFileFor(const std::string& key) {
    if (key.empty()) return std::string();
    for (const char* ext : {"mp3", "m4a", "aac", "wav"}) {
        const std::string path = std::string(HANDOVER_SONGS) + "/" + key + "." + ext;
        if (sizeOnCard(path) > 0) return path;
    }
    return std::string();
}

// The queue in the order it plays, and the place of the song playing in it.
// false when it has none.
bool playOrder(std::vector<int>& order, int& at) {
    MusicQueue& q = MusicQueue::getInstance();
    const std::vector<QueueItem>& items = q.getQueue();
    order.clear();
    at = q.getCurrentIndex();
    if (q.isShuffleEnabled() && q.getShuffleOrder().size() == items.size()) {
        order = q.getShuffleOrder();
        at = q.getShufflePosition();
    } else {
        for (int i = 0; i < (int)items.size(); i++) order.push_back(i);
    }
    return at >= 0 && at < (int)order.size();
}

// The ratingKeys of the song at `at` in `keys`, in play order, and of the
// songs on either side of it, round the end with repeat all: the songs whose
// files stay.
std::vector<std::string> nearKeys(const std::vector<std::string>& keys, int at, bool round) {
    std::vector<std::string> near;
    const int n = (int)keys.size();
    for (int d = -1; d <= 1 && n > 0; d++) {
        int i = at + d;
        if (round) i = ((i % n) + n) % n;
        if (i >= 0 && i < n && !keys[(std::size_t)i].empty()) near.push_back(keys[(std::size_t)i]);
    }
    return near;
}

// The songs near the one playing here (nearKeys), from the queue. On the UI
// thread, which owns it.
std::vector<std::string> nearPlaying() {
    std::vector<int> order;
    int at = 0;
    if (!playOrder(order, at)) return {};
    const std::vector<QueueItem>& items = MusicQueue::getInstance().getQueue();
    const bool round = MusicQueue::getInstance().getRepeatMode() == RepeatMode::ALL;
    const int n = (int)order.size();
    std::vector<std::string> near;
    for (int d = -1; d <= 1; d++) {
        int i = at + d;
        if (round) i = ((i % n) + n) % n;
        if (i < 0 || i >= n) continue;
        const std::string& key = items[(std::size_t)order[(std::size_t)i]].ratingKey;
        if (!key.empty()) near.push_back(key);
    }
    return near;
}

// The folder of songs cleared of all but the files of the songs at `keep`,
// and their covers: whatever else SceShell or VitaPlex left there. How many
// files went.
int pruneSongs(const std::vector<std::string>& keep) {
    const SceUID d = sceIoDopen(HANDOVER_SONGS);
    if (d < 0) return 0;
    SceIoDirent e;
    int removed = 0;
    for (;;) {
        std::memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        const std::string name = e.d_name;
        bool kept = false;
        for (const std::string& key : keep)
            kept = kept || (name.size() > key.size() && name.compare(0, key.size(), key) == 0 &&
                            name[key.size()] == '.');
        if (kept) continue;
        if (std::remove((std::string(HANDOVER_SONGS) + "/" + name).c_str()) == 0) removed++;
    }
    sceIoDclose(d);
    return removed;
}

// The song made ready forgotten, and its file removed: the music stopped, or
// the app going without handing it over. A fetch under way stops.
void dropReadySong() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        g_ready.gen++;
        path.swap(g_ready.path);
        g_ready.key.clear();
    }
    if (!path.empty()) std::remove(path.c_str());
}

// The file made ready for the song at key, for the queue's file= as it goes
// to SceShell, whose it is from then; "" when none is whole. A fetch still
// under way stops, and SceShell fetches the song itself.
std::string handReadySong(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_readyMutex);
    g_ready.gen++;
    std::string path;
    if (g_ready.key == key && !g_ready.path.empty() && sizeOnCard(g_ready.path) > 0)
        path = g_ready.path;
    g_ready.path.clear();
    g_ready.key.clear();
    return path;
}

// The song at key, whose whole file SceShell had at path as it gave the music
// back: made ready already, for VitaPlex plays it on.
void keepReadySong(const std::string& key, const std::string& path) {
    std::lock_guard<std::mutex> lock(g_readyMutex);
    g_ready.gen++;
    g_ready.key = key;
    g_ready.path = path;
}

// What the framer passes on (mp3_frames.h), into the file.
int mp3Write(void* ctx, const uint8_t* data, uint32_t n) {
    return std::fwrite(data, 1, n, (FILE*)ctx) == n ? 0 : -1;
}

// The server's MP3 of the song at key, as SceShell asks for it (the plugin's
// fetchMp3), in a session of its own: the decision, then start.mp3, framed as
// it comes into f, behind a frame that gives its length. true when it came
// to its end, the bytes that came in *got and the frames' length in *ms;
// else why in *why. Stopped once the song is no longer the one playing (gen).
bool fetchServerMp3(unsigned gen, const std::string& key, const std::string& server,
                    const std::string& token, FILE* f, long long* got, uint32_t* ms,
                    std::string* why) {
    static unsigned sessions = 0;
    const std::string query =
        "?path=" + HttpClient::urlEncode("/library/metadata/" + key) +
        "&mediaIndex=0&partIndex=0&directPlay=0&directStream=1&directStreamAudio=1&hasMDE=1"
        "&location=lan&audioBoost=100&audioChannelCount=2&protocol=http&musicBitrate=320"
        "&session=vitaplex-ready-" + key + "-" + std::to_string(++sessions) +
        "&X-Plex-Client-Profile-Name=Generic&X-Plex-Client-Profile-Extra=" +
        HttpClient::urlEncode("add-transcode-target(type=musicProfile&context=streaming"
                              "&protocol=http&container=mp3&audioCodec=mp3)");
    const std::map<std::string, std::string> headers = {
        {"X-Plex-Token", token},
        {"X-Plex-Client-Identifier", "vitaplex-sceshell"},
        {"X-Plex-Product", "VitaPlex"}};
    HttpRequest req;
    req.url = server + "/music/:/transcode/universal/decision" + query;
    req.headers = headers;
    req.timeout = 15;
    HttpClient decision;
    const HttpResponse r = decision.request(req);
    if (r.statusCode != 200) {
        *why = "its transcode decision: HTTP " + std::to_string(r.statusCode);
        return false;
    }
    // As SceShell does, start.mp3 whatever the decision says.
    std::unique_ptr<Mp3Frames> mp3(new Mp3Frames);
    mp3fInit(mp3.get());
    bool writeFailed = false;
    HttpClient client;
    const bool fetched = client.downloadFile(
        server + "/music/:/transcode/universal/start.mp3" + query,
        [&](const char* data, size_t size) {
            if (readyGen() != gen) return false;
            if (mp3fFeed(mp3.get(), (const uint8_t*)data, (uint32_t)size, mp3Write, f) < 0) {
                writeFailed = true;
                return false;
            }
            *got += (long long)size;
            return true;
        },
        nullptr, headers, 0, nullptr, why);
    if (!fetched || writeFailed) {
        if (writeFailed) *why = "its file not written";
        return false;
    }
    const int made = mp3fFinish(mp3.get());
    if (made < 0) {
        *why = "no MP3 frame in the server's MP3 of it";
        return false;
    }
    *ms = mp3fMs(mp3.get());
    if (made > 0 && (std::fseek(f, 0, SEEK_SET) != 0 ||
                     std::fwrite(mp3->frame, 1, mp3->room, f) != mp3->room)) {
        *why = "the frame giving its length not written";
        return false;
    }
    return true;
}

// The fetch, on a thread of its own: the song's details, its first part, and
// that file as the server has it into HANDOVER_SONGS through a file beside
// it, moved in once all of it has come. It stops, and leaves nothing, once
// the song is no longer the one playing (gen). That file ends in ".ready",
// not in the plugin's ".part": SceShell, handed the queue as this stops, may
// be fetching the same song, and the two must never share a file. It is
// moved in under g_readyMutex, which a hand-over takes too, so that a file
// moved in is either named in the queue or never moved in.
void readyFetch(unsigned gen, const std::string& key, const std::string& title,
                const std::string& server, const std::string& token) {
    // Not for a song skipped past in its first seconds, and not as mpv
    // fills its own cache at a song's start; after that the server sends it
    // at about the pace it plays (log 7c97dfa5: 1.29 MB in 22 s). Soon, for
    // a game started a few seconds into a song: 6.7 s in build 2045's run.
    for (int i = 0; i < 20; i++) {
        sceKernelDelayThread(100 * 1000);
        if (readyGen() != gen) return;
    }
    MediaItem item;
    if (!PlexClient::getInstance().fetchMediaDetails(key, item) || item.partPath.empty()) {
        brls::Logger::info("[bgaudio] SceShell: \"{}\" not made ready: no file of it in its "
                           "details",
                           title);
        return;
    }
    // The formats the plugin plays as they are (its shellExt); any other as
    // the server's MP3 of it.
    std::string ext = lowerExt(item.partPath);
    if (ext == "mp4") ext = "m4a";
    const bool asItIs = ext == "mp3" || ext == "m4a" || ext == "aac" || ext == "wav";
    const std::string from = ext.empty() ? "a file of no known format" : ext;
    if (!asItIs) ext = "mp3";
    // A second transcode of the song, beside the one mpv plays it from, not
    // until mpv's own is well under way: 10 s in.
    for (int i = 0; !asItIs && i < 80; i++) {
        sceKernelDelayThread(100 * 1000);
        if (readyGen() != gen) return;
    }
    const std::string path = std::string(HANDOVER_SONGS) + "/" + key + "." + ext;
    const std::string part = path + ".ready";
    sceIoMkdir(HANDOVER_SONGS, 0777);
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) {
        brls::Logger::info("[bgaudio] SceShell: \"{}\" not made ready: {} not written", title, part);
        return;
    }
    // In large writes: stdio's own buffer made the background player's
    // copies slow (log 2114643f).
    std::unique_ptr<char[]> buffer(new char[64 * 1024]);
    std::setvbuf(f, buffer.get(), _IOFBF, 64 * 1024);
    long long got = 0;
    int64_t length = -1;
    bool writeFailed = false;
    std::string why;
    const double t0 = systemSecs();
    bool fetched;
    if (asItIs) {
        HttpClient client;
        fetched = client.downloadFile(
            server + item.partPath + "?download=1",
            [&](const char* data, size_t size) {
                if (readyGen() != gen) return false;
                if (std::fwrite(data, 1, size, f) != size) {
                    writeFailed = true;
                    return false;
                }
                got += (long long)size;
                return true;
            },
            [&](int64_t total) { length = total; }, {{"X-Plex-Token", token}}, 0, nullptr, &why);
    } else {
        brls::Logger::info("[bgaudio] SceShell: \"{}\" is {}: the server's MP3 of it made ready", title,
                           from);
        uint32_t ms = 0;
        fetched = fetchServerMp3(gen, key, server, token, f, &got, &ms, &why);
        // A transcode has no length, so one cut short ends as one done would:
        // its frames are to make the song, to the 3 s the plugin allows.
        if (fetched && item.duration > 0 && ms + 3000 < (uint32_t)item.duration) {
            fetched = false;
            why = "its frames make " + mmss(ms) + " of its " + mmss((unsigned)item.duration);
        }
    }
    const bool closed = std::fclose(f) == 0;
    // Whole, as the answer gave its size, else as the details did: the client
    // takes a transfer cut short for one done. The answer's first, since the
    // details can be older than the file: 4,418,566 bytes for a song that
    // came whole at 4,343,040, which was thrown away, and SceShell fetched it
    // again with the music stopped (log dd6a5eb9, build 2046). A transcode
    // sends no length: what the framer took is whole.
    const long long want = !asItIs ? 0 : length > 0 ? length : item.partSize;
    const bool stopped = readyGen() != gen;
    if (!fetched || writeFailed || !closed || got <= 0 || (want > 0 && got != want) || stopped) {
        std::remove(part.c_str());
        if (!stopped)
            brls::Logger::info("[bgaudio] SceShell: \"{}\" not made ready: {}", title,
                               writeFailed || !closed ? "its file not written"
                               : !why.empty()         ? why
                                                      : std::to_string(got) + " bytes of its " +
                                                            std::to_string(want) + " came");
        return;
    }
    const double took = systemSecs() - t0;
    std::lock_guard<std::mutex> lock(g_readyMutex);
    if (g_ready.gen != gen) {
        std::remove(part.c_str());   // no longer the song playing, or handed over
    } else if (!moveIn(part, path)) {
        std::remove(part.c_str());
        brls::Logger::info("[bgaudio] SceShell: \"{}\" not made ready: {} not moved in", title, part);
    } else {
        g_ready.path = path;
        brls::Logger::info("[bgaudio] SceShell: \"{}\" made ready for a hand-over, {:.2f} MB in {:.1f} "
                           "s, at {}",
                           title, got / 1048576.0, took, path);
    }
}

// From the UI thread, as MusicController publishes what plays (now_playing.hpp):
// the song playing made ready, when it is a new one that could be handed over.
void readySongForPlaying() {
    if (g_outsideMusic == kOutsideStops || g_handedOverAt != 0) return;
    MusicQueue& q = MusicQueue::getInstance();
    const QueueItem* t = q.getCurrentTrack();
    if (!t || t->ratingKey.empty() || !q.isMusicQueue()) return;
    const std::string key = t->ratingKey, title = t->title;
    unsigned gen;
    {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        if (g_ready.key == key) return;
        g_ready.path.clear();
        g_ready.key = key;
        gen = ++g_ready.gen;
    }
    // The files of the songs near this one stay; the others go.
    const int pruned = pruneSongs(nearPlaying());
    if (pruned)
        brls::Logger::info("[bgaudio] SceShell: {} file(s) of songs no longer near removed", pruned);
    // Its file there already, kept as the music came back or fetched before:
    // ready as it is.
    const std::string kept = songFileFor(key);
    if (!kept.empty()) {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        if (g_ready.gen == gen) g_ready.path = kept;
        return;
    }
    DownloadItem dl;
    if (DownloadsManager::getInstance().getDownloadCopy(key, dl) &&
        dl.state == DownloadState::COMPLETED)
        return;   // SceShell plays the download
    std::string server = PlexClient::getInstance().getServerUrl();
    const std::string token = PlexClient::getInstance().getAuthToken();
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty()) return;
    platform::launchThread([gen, key, title, server, token]() {
        readyFetch(gen, key, title, server, token);
    });
}

// VitaPlex's server, with no '/' at its end; "" with none.
std::string plexServer() {
    std::string server = PlexClient::getInstance().getServerUrl();
    while (!server.empty() && server.back() == '/') server.pop_back();
    return server;
}

// The header lines every request of the plugin's for VitaPlex carries:
// VitaPlex's names for itself, as its own requests have them, so that Plex
// shows the music the shell plays as VitaPlex's and keeps its place, and the
// token. The plugin never logs them.
std::string plexHeaderLines(const std::string& token) {
    std::string h = std::string("X-Plex-Client-Identifier: ") + PLEX_CLIENT_ID +
                    "\r\nX-Plex-Product: VitaPlex\r\nX-Plex-Platform: PlayStation Vita"
                    "\r\nX-Plex-Device: PS Vita";
    if (!token.empty()) h += "\r\nX-Plex-Token: " + token;
    return h;
}

// The server's MP3 of the song at key, as the plugin fetches a song in a
// format the shell does not play, or one VitaPlex has no file of: `which` is
// "decision", fetched first, or "start.mp3"; {session} the plugin fills in,
// one number for both. Kept short: the plugin holds 512 bytes of altPrep. An
// MP3 on the server comes as it is (directStreamAudio).
std::string mp3Url(const std::string& server, const std::string& key, const char* which) {
    return server + "/music/:/transcode/universal/" + which + "?path=" +
           HttpClient::urlEncode("/library/metadata/" + key) +
           "&mediaIndex=0&partIndex=0&directPlay=0&directStream=1&directStreamAudio=1&hasMDE=1"
           "&location=lan&protocol=http&musicBitrate=320&session=vp-" + key +
           "-{session}&X-Plex-Client-Profile-Name=Generic&X-Plex-Client-Profile-Extra=" +
           HttpClient::urlEncode("add-transcode-target(type=musicProfile&context=streaming"
                                 "&protocol=http&container=mp3&audioCodec=mp3)");
}

// A cover from the server as the row shows it: a PNG of SC_ART_SIZE.
std::string coverUrl(const std::string& server, const std::string& thumb) {
    if (thumb.empty()) return std::string();
    char size[16];
    std::snprintf(size, sizeof(size), "%d", SC_ART_SIZE);
    return server + "/photo/:/transcode?width=" + size + "&height=" + size +
           "&minSize=1&upscale=1&format=png&url=" + HttpClient::urlEncode(thumb);
}

// The written= of a file of the plugin's queue, as scWriteQueue wrote it: the
// status names the queue it plays by it, and VitaPlex's own copy carries the
// same. "" when it cannot be read.
std::string writtenOf(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    char line[64] = "";
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!got || std::strncmp(line, "written=", 8) != 0) return std::string();
    std::string w = line + 8;
    while (!w.empty() && (w.back() == '\n' || w.back() == '\r')) w.pop_back();
    return w;
}

// The queue in the order it plays, the song playing and where in it: the
// plugin's queue (SC_QUEUE, written with scWriteQueue), and VitaPlex's own
// copy (HANDOVER_QUEUE) with the same written=. A queue longer than the plugin
// allows goes from the song playing on. Each song plays from its download, or
// for the one playing, the file made ready for it (whole, and the shell's from
// then); else the plugin fetches the server's MP3 of it. A download's cover is
// the PNG the server made of it beside the download; the others, the server's
// as a PNG. The server is told where the music is and of each song played,
// as VitaPlex tells it. On the UI thread, which owns the queue. The song, for
// the log, in *what, and its ratingKey in *key.
bool writeQueue(std::string& what, std::string& key) {
    MusicQueue& q = MusicQueue::getInstance();
    const std::vector<QueueItem>& items = q.getQueue();
    std::vector<int> order;
    int at = 0;
    if (!playOrder(order, at)) return false;
    std::size_t from = 0;
    if (order.size() > HANDOVER_MAX_TRACKS) {
        from = (std::size_t)at;
        at = 0;
    }

    // Where in the song: mpv's clock counts from where its stream begins,
    // which a seek that restarted the transcode moved part-way in.
    const double position = MpvPlayer::getInstance().getPosition();
    const long long positionMs = std::max(
        0LL, MusicController::getInstance().streamStartOffsetMs() +
                 (position > 0 ? (long long)(position * 1000) : 0));

    const QueueItem& now = items[(std::size_t)order[from + (std::size_t)at]];
    const std::string file = handReadySong(now.ratingKey);
    const std::string server = plexServer();
    const std::string token = PlexClient::getInstance().getAuthToken();

    // The fields of each song, kept while scWriteQueue reads them.
    struct Strings {
        std::string id, local, url, altUrl, altPrep, coverUrl, coverPng;
    };
    std::vector<Strings> strings;
    std::vector<ScTrack> tracks;
    std::string mine;   // VitaPlex's own copy's track lines
    DownloadsManager& downloads = DownloadsManager::getInstance();
    for (std::size_t i = from; i < order.size() && i - from < HANDOVER_MAX_TRACKS; i++) {
        const QueueItem& t = items[(std::size_t)order[i]];
        Strings st;
        st.id = t.ratingKey;
        DownloadItem dl;
        if (downloads.getDownloadCopy(t.ratingKey, dl) && dl.state == DownloadState::COMPLETED) {
            st.local = dl.localPath;
            st.coverPng = DownloadsManager::smallCoverPath(dl.thumbPath);
            if (!st.coverPng.empty() && !fileExists(st.coverPng)) st.coverPng.clear();
        }
        if (&t == &now && !file.empty()) st.local = file;
        if (!server.empty() && !t.ratingKey.empty()) {
            st.altUrl = mp3Url(server, t.ratingKey, "start.mp3");
            st.altPrep = mp3Url(server, t.ratingKey, "decision");
            st.coverUrl = coverUrl(server, t.thumb);
        }
        char rating[16];
        std::snprintf(rating, sizeof(rating), "%.1f", t.userRating);
        mine += "track=" + trackField(t.ratingKey) + "|" + std::to_string(t.duration * 1000) +
                "|" + trackField(st.local == file && !file.empty() ? std::string() : st.local) +
                "|" + rating + "|" + trackField(t.thumb) + "|" + trackField(t.title) + "|" +
                trackField(t.artist) + "|" + trackField(t.album) + "|" + trackField(st.coverPng) +
                "\n";
        strings.push_back(std::move(st));
    }
    tracks.resize(strings.size());
    for (std::size_t k = 0; k < strings.size(); k++) {
        const QueueItem& t = items[(std::size_t)order[from + k]];
        const Strings& st = strings[k];
        ScTrack& tr = tracks[k];
        std::memset(&tr, 0, sizeof(tr));
        tr.id = st.id.c_str();
        tr.durationMs = (uint32_t)std::max(0, t.duration) * 1000u;
        tr.local = st.local.c_str();
        tr.altUrl = st.altUrl.c_str();
        tr.altPrep = st.altPrep.c_str();
        tr.title = t.title.c_str();
        tr.artist = t.artist.c_str();
        tr.album = t.album.c_str();
        tr.coverUrl = st.coverUrl.c_str();
        tr.coverPng = st.coverPng.c_str();
    }

    ScQueue sq;
    std::memset(&sq, 0, sizeof(sq));
    sq.index = (uint32_t)at;
    sq.positionMs = (uint32_t)positionMs;
    sq.repeat = repeatCode(q.getRepeatMode());
    sq.offlineSkip = g_offlineSkip ? 1 : 0;
    const std::string headers = plexHeaderLines(token);
    sq.headers = headers.c_str();
    // As PlexClient::reportTimeline and the background player's scrobble.
    const std::string timeline =
        server.empty() ? std::string()
                       : server + "/:/timeline?ratingKey={id}&key=%2Flibrary%2Fmetadata%2F{id}"
                                  "&state={state}&time={positionMs}&duration={durationMs}";
    const std::string scrobble =
        server.empty() ? std::string()
                       : server + "/:/scrobble?key={id}&identifier=com.plexapp.plugins.library";
    sq.state.url = timeline.c_str();
    sq.stateEveryS = 10;
    sq.played.url = scrobble.c_str();
    // No remote control (the plugin's control.* lines) yet: Plex's companion
    // protocol is to be tried in VitaPlex itself first.
    if (scWriteQueue(&sq, tracks.data(), (uint32_t)tracks.size()) < 0) return false;

    const std::string written = writtenOf(shellcomp::path(SC_QUEUE));
    char head[160];
    std::snprintf(head, sizeof(head), "written=%s\nindex=%d\npositionMs=%lld\nrepeat=%d\n",
                  written.c_str(), at, positionMs, repeatCode(q.getRepeatMode()));
    if (written.empty() || !writeWhole(HANDOVER_QUEUE, head + mine + "end\n")) return false;
    what = std::to_string(tracks.size()) + " song(s), from \"" + now.title + "\" at " +
           mmss((unsigned)positionMs) +
           (file.empty() ? std::string(", for SceShell to fetch") : ", from " + file);
    key = now.ratingKey;
    return true;
}

// vita-shell-companion's plugin open, loaded into SceShell first if it is
// not there: what SceShell plays the queue through. On the UI thread.
bool shellPluginOpen() {
    return shellcomp::open("[bgaudio] SceShell");
}

// The cover made for the row here, put beside the songs for SceShell
// ("<ratingKey>.png" in the folder of songs) as the song playing's, so that
// the row keeps it.
// Before the plugin is asked, which then fetches any it does not find.
void shellSongCover(const std::string& key) {
    uint32_t serial = 0;
    std::string made, wanted;
    {
        std::lock_guard<std::mutex> lock(g_qmArtMutex);
        serial = g_qmArtSerial;
        made = g_qmArtDone;
    }
    {
        std::lock_guard<std::mutex> lock(g_qmMutex);
        wanted = g_qmSong.artUrl;
    }
    if (key.empty() || serial == 0 || made != wanted) return;
    char artName[32], to[200];
    std::snprintf(artName, sizeof(artName), SC_ART_FMT, (unsigned)(serial & 1));
    const std::string from = shellcomp::path(artName);
    std::snprintf(to, sizeof(to), "%s/%s.png", HANDOVER_SONGS, key.c_str());
    const std::string part = std::string(to) + ".part";
    if (!copyFile(from, part) || !moveIn(part, to)) {
        std::remove(part.c_str());
        brls::Logger::info("[bgaudio] SceShell: the song's cover could not be put at {}", to);
    }
}

// The queue, written already, to SceShell (SC_CMD_MUSIC_PLAY). The row is
// given back first, with the tick's lock held, so that nothing of this
// process's follows. true when the plugin took it.
bool handToShell() {
    std::lock_guard<std::mutex> tickLock(g_qmTickMutex);
    quickMenuRelease("handed to SceShell");
    const int rc = scMusicPlay();
    brls::Logger::info("[bgaudio] SceShell: asked to play the queue: {}", hex(rc));
    return rc >= 0;
}

// Music playing goes to SceShell through vita-shell-companion's plugin, which
// plays the queue on there through the shell's music player: the queue and
// the song's cover written first, then mpv paused and the BGM port given
// back, so that the two never play at once, then the plugin asked. The music
// plays on once the app has gone, so the caller quits. Without the plugin, or
// if it will not take the queue, the music is not handed over: it plays on
// here. On the UI thread. true when it was handed over.
bool handOver(const char* why) {
    if (g_outsideMusic == kOutsideStops || g_handedOverAt != 0) return false;
    const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
    const bool playing = mpvPlays(s) && s.audioOnly;
    if (!playing || !MusicQueue::getInstance().isMusicQueue()) {
        // Which of the two: log b8cde643 said "no music playing" with mpv
        // playing a queue of downloaded songs that was not marked as music.
        brls::Logger::info("[bgaudio] SceShell: {}, {} ({}); nothing handed over", why,
                           playing ? "with a queue that is not music" : "with no music playing",
                           describeMpv(s));
        return false;
    }
    if (!shellPluginOpen()) {
        brls::Logger::warning("[bgaudio] SceShell: {}, but the shell companion plugin is not "
                              "there to play the queue; nothing handed over",
                              why);
        return false;
    }
    std::string what, key;
    if (!writeQueue(what, key)) {
        brls::Logger::error("[bgaudio] SceShell: {}, but the queue could not be written to {} "
                            "and {}",
                            why, shellcomp::path(SC_QUEUE), HANDOVER_QUEUE);
        return false;
    }
    // The folder of songs, there for SceShell to fetch into, with the song's
    // cover.
    sceIoMkdir(HANDOVER_SONGS, 0777);
    shellSongCover(key);
    MusicController::getInstance().playPause(false);
    releasePort("handed to SceShell");
    if (handToShell()) {
        g_handedOverAt = sceKernelGetSystemTimeWide();
        brls::Logger::info("[bgaudio] SceShell: {}: handed over {}", why, what);
        return true;
    }
    brls::Logger::warning("[bgaudio] SceShell did not take the queue; the music plays on here");
    MusicController::getInstance().playPause(true);
    return false;
}

// ── Taking the music back ────────────────────────────────────────────────

// A status SceShell wrote longer ago than this is not of a queue it carries on
// now: it writes one every 2 s as it does.
const double kCarryingSecs = 10.0;

// A note as handover.h lays them out: "key=value" lines, and "end" last. The
// track lines in order, split into their fields, in *tracks when asked for.
bool readNote(const char* path, std::map<std::string, std::string>& fields,
              std::vector<std::vector<std::string>>* tracks) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    // Read whole, in 64 KB reads: a queue of a thousand songs, some 200 KB,
    // took 1.3 s to read through stdio's 1 KB reads with VitaPlex starting
    // (log 7c97dfa5), and more than 2.8 s in log 7c38c1fa.
    std::setvbuf(f, nullptr, _IOFBF, 64 * 1024);
    std::string text;
    char buf[16384];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    bool whole = false;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
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
            std::size_t from = 0;
            for (;;) {
                const std::size_t bar = value.find('|', from);
                parts.push_back(value.substr(from, bar == std::string::npos ? bar : bar - from));
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
            parts.resize(HANDOVER_TRACK_FIELDS);
            tracks->push_back(std::move(parts));
        } else if (key != "track") {
            fields[key] = value;
        }
    }
    return whole;
}

// SceShell's status, read again a few times when it is missing or not whole:
// the plugin replaces it by removing it and moving the new one in
// (carryStatusOf), and one read between the two finds none.
bool readStatus(std::map<std::string, std::string>& status) {
    for (int i = 0; i < 5; i++) {
        if (i) sceKernelDelayThread(20 * 1000);
        status.clear();
        if (readNote(HANDOVER_STATUS, status, nullptr)) return true;
    }
    return false;
}

double fieldNum(const std::map<std::string, std::string>& m, const char* key) {
    const auto it = m.find(key);
    return it == m.end() ? -1 : std::atof(it->second.c_str());
}

std::string fieldText(const std::map<std::string, std::string>& m, const char* key) {
    const auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

// The songs SceShell played the queue from, once nothing plays them; but for
// the files of the songs at keep, whole as the plugin moves its files in,
// which go on here. Logged when any went.
void removePlayerSongs(const std::vector<std::string>& keep = {}) {
    const int removed = pruneSongs(keep);
    if (removed)
        brls::Logger::info("[bgaudio] take back: removed {} file(s) SceShell left{}", removed,
                           keep.empty() ? "" : ", but for those of the songs near the one taken back");
}

// The song the shell plays on for SceShell's queue, when the plugin did not
// stop it as told (takeBackFromShell): its clock read twice, half a second
// apart, and its length checked against the queue's song, so that the system
// Music app's song is left alone (BG Test 21, build 2012). Stopped when it is
// the queue's. Where it was, or -1.
long long takeFromShell(int durationMs) {
    const int rcInit = sceMusicPlayerServiceInitialize(0);
    if (rcInit < 0) {
        brls::Logger::info("[bgaudio] take back: the system player would not start ({}); its "
                           "song is left as it is",
                           hex(rcInit));
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
    brls::Logger::info("[bgaudio] take back: the system player is at {} then {} (state {}), "
                       "with a song {} long against the queue's {}",
                       mmss(a.currentTime), mmss(b.currentTime), b.currentState,
                       mmss((unsigned)std::max(0, info->duration)),
                       mmss((unsigned)std::max(0, durationMs)));
    long long at = -1;
    if (plays && ours) {
        at = b.currentTime;
        const int rcStop = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0);
        brls::Logger::info("[bgaudio] take back: its song plays on in the system player; "
                           "stopped there ({}) to go on here",
                           hex(rcStop));
    }
    sceMusicPlayerServiceTerminate();
    return at;
}

// Why the queue at HANDOVER_QUEUE is not the one SceShell played, as its
// status says, which the take-back goes on with; "" when it is.
std::string queueProblem(bool haveQueue, const std::map<std::string, std::string>& queue,
                         std::size_t tracks, const std::map<std::string, std::string>& status) {
    char why[128];
    if (!haveQueue) {
        std::snprintf(why, sizeof(why), "its queue at %s is %s", HANDOVER_QUEUE,
                      fileExists(HANDOVER_QUEUE) ? "not whole" : "gone");
    } else if (fieldNum(queue, "written") != fieldNum(status, "queue")) {
        std::snprintf(why, sizeof(why), "its queue at %s is another, written at %.6f, not %.6f",
                      HANDOVER_QUEUE, fieldNum(queue, "written"), fieldNum(status, "queue"));
    } else if (fieldNum(status, "index") < 0 || fieldNum(status, "index") >= (double)tracks) {
        std::snprintf(why, sizeof(why), "its song %d is not among the %d of its queue",
                      (int)fieldNum(status, "index") + 1, (int)tracks);
    } else {
        return std::string();
    }
    return why;
}

// A queue SceShell carries on (handover.h), as VitaPlex opens: its player
// set up here at SceShell's song and place, with SceShell playing on, and the
// music taken back by the player once it is ready to play it (takeBackNow).
// Up to build 2048 SceShell was told first and its song stopped here before
// the player was set up, and mpv with it: 5 s of silence (log 4e8bcb05), or,
// when VitaPlex went before it got to the shell, the song left playing there
// with nothing looking after it (log 7c38c1fa). Off the UI thread: it reads
// the queue, of a thousand songs at most.
void takeBack() {
    std::map<std::string, std::string> status, queue;
    std::vector<std::vector<std::string>> tracks;
    if (!readStatus(status)) {
        removePlayerSongs();   // never handed over, or long done
        return;
    }
    const double now = systemSecs();
    const double written = fieldNum(status, "written");
    // The system clock starts again with the Vita, so a status written later
    // than now is from before it last started: one read as written 1101 s
    // after VitaPlex started (log fbc54add). Nothing of it plays.
    if (written > now + 1.0) {
        brls::Logger::info("[bgaudio] take back: SceShell's status (\"{}\") is from before the "
                           "Vita last started",
                           fieldText(status, "state"));
        removePlayerSongs();
        return;
    }
    const std::string state = fieldText(status, "state");
    const int index = (int)fieldNum(status, "index");
    const long long at = (long long)std::max(0.0, fieldNum(status, "positionMs"));
    brls::Logger::info("[bgaudio] take back: SceShell last said \"{}\", song {} at {}{}{}, "
                       "{:.1f} s ago",
                       state, index + 1, mmss((unsigned)at),
                       fieldNum(status, "fetching") == 1 ? " as it is fetched" : "",
                       fieldNum(status, "paused") == 1 ? ", paused" : "", now - written);
    // SceShell writes its status every 2 s as it carries a queue on: one not
    // written for longer is of music over, or of a plugin no longer there.
    // Its files stay then, as the shell may yet play one of them: a file
    // written over as the shell played it preceded a crash (logs 8fe35d97,
    // 7ba90482). VitaPlex's own music clears them as it goes (pruneSongs).
    if (state != HANDOVER_STATE_PLAYING) {
        removePlayerSongs();
        return;
    }
    if (now - written > kCarryingSecs) {
        brls::Logger::info("[bgaudio] take back: that is too long ago for SceShell to be "
                           "carrying it on");
        return;
    }
    const bool haveQueue = readNote(HANDOVER_QUEUE, queue, &tracks);
    const std::string problem = queueProblem(haveQueue, queue, tracks.size(), status);
    if (!problem.empty()) {
        brls::Logger::warning("[bgaudio] take back: {}; SceShell plays on", problem);
        return;
    }

    std::vector<MediaItem> items;
    items.reserve(tracks.size());
    for (const auto& t : tracks) {
        MediaItem item;
        item.ratingKey = t[HANDOVER_TRACK_KEY];
        item.duration = std::atoi(t[HANDOVER_TRACK_DURATION].c_str());
        item.userRating = (float)std::atof(t[HANDOVER_TRACK_RATING].c_str());
        item.thumb = t[HANDOVER_TRACK_THUMB];
        item.title = t[HANDOVER_TRACK_TITLE];
        item.grandparentTitle = t[HANDOVER_TRACK_ARTIST];
        item.parentTitle = t[HANDOVER_TRACK_ALBUM];
        item.type = "track";
        item.mediaType = MediaType::MUSIC_TRACK;
        items.push_back(std::move(item));
    }
    const int repeat = (int)fieldNum(queue, "repeat");
    {
        std::lock_guard<std::mutex> lock(g_shellQueueMutex);
        g_shellQueue.written = fieldNum(queue, "written");
        g_shellQueue.keys.clear();
        for (const auto& t : tracks) g_shellQueue.keys.push_back(t[HANDOVER_TRACK_KEY]);
        g_shellQueue.repeatAll = repeat == HANDOVER_REPEAT_ALL;
    }
    brls::sync([items, index, at, repeat]() {
        const MpvPlayer::Snapshot s = MpvPlayer::getInstance().snapshot();
        if (s.initialized && s.state != MpvPlayerState::IDLE && s.state != MpvPlayerState::ENDED &&
            s.state != MpvPlayerState::ERROR) {
            brls::Logger::info("[bgaudio] take back: VitaPlex plays something already; the queue "
                               "is not taken back");
            return;
        }
        MusicQueue::getInstance().setRepeatMode(repeat == HANDOVER_REPEAT_ONE ? RepeatMode::ONE
                                                : repeat == HANDOVER_REPEAT_ALL ? RepeatMode::ALL
                                                                                : RepeatMode::OFF);
        brls::Logger::info("[bgaudio] take back: the queue goes on here from \"{}\" once its "
                           "player is ready; SceShell plays on until then",
                           items[(std::size_t)index].title);
        brls::Application::pushActivity(
            PlayerActivity::createWithRestoredQueue(items, index, at, true));
    });
}

// The music taken back from SceShell for the player takeBack set up, now
// that it is ready to play (takeBackNow): SceShell is told to stop, stops its
// song, and writes where in the status once more, which it did 0.14 s after
// it was told in log d6e76980. The songs near the one going on here keep
// their files, which VitaPlex plays on from (songFile): the song before, for
// previous, that song, made ready for the next hand-over too, and the next,
// which SceShell fetched ahead (log 8e7a4b23: 22.6 s to stream it again).
// On the UI thread, which waits on the shell meanwhile.
ShellPlace takeBackFromShell() {
    ShellQueue q;
    {
        std::lock_guard<std::mutex> lock(g_shellQueueMutex);
        q = g_shellQueue;
    }
    ShellPlace place;
    const double asked = systemSecs();
    const int rc = scMusicTakeBack();
    std::map<std::string, std::string> status;
    bool answered = false;
    for (int i = 0; rc >= 0 && i < 60 && !answered; i++) {
        sceKernelDelayThread(25 * 1000);
        status.clear();
        if (!readNote(HANDOVER_STATUS, status, nullptr)) continue;
        answered = fieldNum(status, "written") >= asked;
        // The queue over since VitaPlex read the status: no answer comes, and
        // the UI need not wait for one.
        if (!answered && fieldText(status, "state") != HANDOVER_STATE_PLAYING) break;
    }
    const double took = systemSecs() - asked;
    // The files go only with nothing of the queue playing in the shell: a
    // file written over as the shell played it preceded a crash (logs
    // 8fe35d97, 7ba90482).
    bool prune = answered;
    if (answered) {
        place.index = (int)fieldNum(status, "index");
        place.atMs = (long long)std::max(0.0, fieldNum(status, "positionMs"));
        place.play = fieldNum(status, "queue") == q.written;
        // Paused from the quick menu: it stays paused here (build 2052).
        place.paused = fieldNum(status, "paused") == 1;
        brls::Logger::info("[bgaudio] take back: SceShell stopped at song {}, {} (\"{}\"), {:.2f} s "
                           "after it was told{}{}",
                           place.index + 1, mmss((unsigned)place.atMs), fieldText(status, "state"),
                           took, place.paused ? ", paused from the quick menu" : "",
                           place.play ? "" : ", in another queue");
    } else {
        // No answer: SceShell carried nothing on by then, its queue over or
        // something else playing in the shell, or its plugin is one of before
        // build 2049, which leaves its song playing. A song of the queue still
        // in the shell is stopped here, as it was up to build 2048.
        readStatus(status);
        const std::string state = fieldText(status, "state");
        brls::Logger::info("[bgaudio] take back: no answer from SceShell in {:.1f} s ({}); it "
                           "last said \"{}\"",
                           took, hex(rc), state);
        prune = state != HANDOVER_STATE_PLAYING;
        if (state == HANDOVER_STATE_PLAYING && fieldNum(status, "queue") == q.written) {
            const long long at = takeFromShell((int)fieldNum(status, "durationMs"));
            if (at >= 0) {
                place.play = true;
                place.index = (int)fieldNum(status, "index");
                place.atMs = at;
                prune = true;
            }
        }
    }
    if (place.play && (place.index < 0 || place.index >= (int)q.keys.size())) {
        brls::Logger::warning("[bgaudio] take back: its song {} is not among the {} of the queue",
                              place.index + 1, q.keys.size());
        place.play = false;
    }
    if (prune) {
        std::vector<std::string> near;
        if (place.play) near = nearKeys(q.keys, place.index, q.repeatAll);
        removePlayerSongs(near);
        const std::string key = place.play ? q.keys[(std::size_t)place.index] : std::string();
        const std::string kept = songFileFor(key);
        if (!kept.empty()) keepReadySong(key, kept);
        for (const std::string& k : near)
            if (!songFileFor(k).empty())
                brls::Logger::info("[bgaudio] take back: kept the file of song {}, {}", k,
                                   k == key ? "the one taken back" : "near it");
    }
    return place;
}

// What VitaPlex's own plugin (up to build 2095) and the background player
// (up to build 2047) left under their names (handover.h's HANDOVER_OLD*),
// removed; but not while SceShell may still play from them, a queue handed
// over by such a build carrying on in this boot, its status written in the
// last 30 s.
void removeOldNames() {
    const double now = systemSecs();
    int removed = 0;
    const struct {
        const char* status;
        const char* songs;
        const char* files[4];
    } kOld[2] = {
        {HANDOVER_OLD17_STATUS, HANDOVER_OLD17_SONGS, {HANDOVER_OLD17_STATUS, nullptr}},
        {HANDOVER_OLD_STATUS,
         HANDOVER_OLD_SONGS,
         {HANDOVER_OLD_QUEUE, HANDOVER_OLD_STATUS, HANDOVER_OLD_LOG, HANDOVER_OLD_LOG2}},
    };
    for (const auto& old : kOld) {
        std::map<std::string, std::string> status;
        const double written =
            readNote(old.status, status, nullptr) ? fieldNum(status, "written") : -1.0;
        if (fieldText(status, "state") == HANDOVER_STATE_PLAYING && written <= now + 1.0 &&
            now - written < 30.0) {
            brls::Logger::info("[bgaudio] the files at {} left as they are: SceShell may still "
                               "play from them",
                               old.songs);
            continue;
        }
        for (const char* path : old.files)
            if (path) removed += std::remove(path) == 0 ? 1 : 0;
        const SceUID d = sceIoDopen(old.songs);
        if (d >= 0) {
            SceIoDirent e;
            for (;;) {
                std::memset(&e, 0, sizeof(e));
                if (sceIoDread(d, &e) <= 0) break;
                if (e.d_name[0] == '.') continue;
                const std::string path = std::string(old.songs) + "/" + e.d_name;
                removed += std::remove(path.c_str()) == 0 ? 1 : 0;
            }
            sceIoDclose(d);
            sceIoRmdir(old.songs);
        }
    }
    if (removed)
        brls::Logger::info("[bgaudio] removed {} file(s) under the names older builds gave them",
                           removed);
}

}  // namespace

std::string songFile(const std::string& ratingKey) { return songFileFor(ratingKey); }

ShellPlace takeBackNow() { return takeBackFromShell(); }

void init(int outsideMusic) {
    if (outsideMusic < kOutsideStops || outsideMusic > kOutsideHandsOver)
        outsideMusic = kOutsideStops;
    g_outsideMusic = outsideMusic;
    g_keepPlaying = outsideMusic != kOutsideStops;
    shellcomp::init();
    brls::Logger::info("[bgaudio] music after leaving: {}",
                       outsideMusic == kOutsidePlaysOn     ? "plays on, then SceShell"
                       : outsideMusic == kOutsideHandsOver ? "SceShell at once"
                                                           : "stops");
    startWatcher();
    // SceShell may be playing a queue handed over before: it
    // plays on until the player takeBack sets up for it, once the first
    // frame is up, is ready and tells it to stop (takeBackNow). Whatever the
    // setting: music handed over before it was changed comes back all the
    // same. In the first frame, once the server is connected and the main
    // screen is up; the song plays on in the shell until then.
    brls::sync([]() {
        platform::launchThread([]() {
            takeBack();
            removeOldNames();
        });
    });
}

void setOutsideMusic(int mode) {
    if (mode < kOutsideStops || mode > kOutsideHandsOver) mode = kOutsideStops;
    g_outsideMusic = mode;
    const bool on = mode != kOutsideStops;
    g_keepPlaying = on;
    brls::Logger::info("[bgaudio] music after leaving set to {}", mode);
    if (on) {
        if (MpvPlayer::getInstance().isPlaying()) holdPort("mpv, already playing");
        // The song playing, begun with the setting off, made ready for a
        // hand-over now: else SceShell fetched it as VitaPlex quit, 14 s
        // after the setting was changed, and waited 11.7 s for it (log
        // 9ed6aa55).
        readySongForPlaying();
    } else {
        dropReadySong();
        releasePort("setting turned off");
    }
}

int outsideMusic() { return g_outsideMusic; }

void setOfflineSkip(bool on) {
    g_offlineSkip = on;
    brls::Logger::info("[bgaudio] SceShell, with the server not answering: {}",
                       on ? "on to the songs on the memory card" : "songs passed over");
}

bool installUpdate() {
    if (!shellPluginOpen()) {
        brls::Logger::warning("[bgaudio] update: the shell companion plugin is not there to "
                              "install it");
        return false;
    }
    const int rc = scUpdateInstall();
    brls::Logger::info("[bgaudio] update: the plugin asked to install it once VitaPlex has gone: "
                       "{}",
                       hex(rc));
    if (rc < 0) return false;
    // As onRequestQuit does as the system closes VitaPlex.
    quickMenuRelease("the app quits to be updated");
    releasePort("quitting to be updated");
    if (!handOver("VitaPlex quits to be updated")) dropReadySong();
    return true;
}

bool installUpdateVpk(const std::string& vpkPath) {
    if (!shellPluginOpen()) {
        brls::Logger::warning("[bgaudio] update: the shell companion plugin is not there to "
                              "install it");
        return false;
    }
    const int rc = scUpdateInstallVpk(vpkPath.c_str());
    brls::Logger::info("[bgaudio] update: the plugin asked to unpack and install {} once VitaPlex "
                       "has gone: {}",
                       vpkPath, rc == -2 ? std::string("not sent, the plugin can't") : hex(rc));
    if (rc < 0) return false;
    // As onRequestQuit does as the system closes VitaPlex.
    quickMenuRelease("the app quits to be updated");
    releasePort("quitting to be updated");
    if (!handOver("VitaPlex quits to be updated")) dropReadySong();
    return true;
}

bool keepPlaying() { return g_keepPlaying; }

void exitProcess(int code) {
    sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000 * 1000);
}

void onAudioPlayback(bool playing) {
    // Held from the first play and through pauses, like ElevenMPV-A; it goes
    // when the player does, or when the setting is turned off.
    if (playing && g_keepPlaying) holdPort("mpv playback");
}

void onPlayerShutdown() { releasePort("player shut down"); }

void setQuickMenuControls(bool on) {
    g_qmEnabled = on;
    if (!on && g_qmActive.exchange(false)) scRowRelease();
}

void quickMenuNowPlaying(const std::string& title, const std::string& artist,
                         const std::string& album, const std::string& artUrl, bool playing) {
    {
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
    readySongForPlaying();
}

void quickMenuNowPlayingCleared() {
    {
        std::lock_guard<std::mutex> lock(g_qmMutex);
        g_qmSong.wanted = false;
    }
    dropReadySong();
}

}  // namespace bgaudio
}  // namespace vitaplex
