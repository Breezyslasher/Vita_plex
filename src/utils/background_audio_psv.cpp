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
#include "utils/http_client.hpp"
#include "utils/now_playing.hpp"

#include <borealis.hpp>

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "bgplayer.h"
#include "qm_cover.h"
#include "qm_link.h"

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

// ── Handing the music over (see "Handing the music to SceShell" below) ───
// When the queue was handed over, on the way out; 0 before. Past
// kQuitShutdownUs after it the watcher ends the process itself.
std::atomic<SceInt64> g_handedOverAt{0};
// The last press of PS (the power callback): a deactivation within 2 s of
// one is the user leaving, as BG Tests 20 and 21 took it.
std::atomic<SceInt64> g_psPressUs{0};
// When this process set up handing the music over, on the system clock,
// which runs on from one process to the next as the notes have it.
double g_initSecs = 0;
// VitaPlex's quick menu plugin was in SceShell already, and was told this app
// takes the music back (takeBack, qm_link.h's qmTakeBack): any queue SceShell
// played then stopped, and its last status follows.
bool g_takeBackSent = false;
bool handOver(const char* why);
void dropReadySong();

// ── The quick menu's Music row (qm_link.h) ───────────────────────────────
// What MusicController publishes, which now_playing.cpp hands here on the
// Vita, for the watcher to put on the row through VitaPlex's quick menu
// plugin. The row is this process's while it has music to show; it goes to
// SceShell with the queue, and back to the system as the app goes.
std::atomic<bool> g_qmEnabled{false};   // the setting
std::atomic<bool> g_qmActive{false};    // the plugin has the row for this process
std::atomic<bool> g_qmGone{false};      // handed over or quitting: not taken again
// Held for each quickMenuTick, and as the queue goes to SceShell, so that no
// heartbeat (QM_CMD_ACTIVATE) of this process's follows QM_CMD_PLAY_QUEUE:
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

// ── The song's cover on the row (QM_CMD_ART) ─────────────────────────────
// Made by a thread of its own, as the watcher asks for it: Plex's thumbnail
// through its transcoder at twice the size the row shows, or a download's
// local copy, made into the PNG the plugin loads (qm_cover.h). The watcher
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
    brls::Logger::info("[bgaudio] quick menu: row given back ({}): {}", why, hex(qmDeactivate()));
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
        else if (ev.event == kEventRequestQuit) onRequestQuit(now);
    }
}

int powerCallback(int, int, int powerInfo, void*) {
    if ((unsigned)powerInfo & SCE_POWER_CB_BUTTON_PS_PRESS)
        g_psPressUs = sceKernelGetSystemTimeWide();
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
        while (qmPoll() != QM_NONE) {
        }
        const int rc = qmActivate(QM_OWNER_APP);
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
        qmDeactivate();
        return;
    }
    if (!g_qmActive) return;
    // VitaPlex's plugin gives the row back by itself once these stop.
    if (now - lastBeat >= 1000000) {
        lastBeat = now;
        qmHeartbeat(QM_OWNER_APP);
    }
    if (song.serial != sentSerial && qmSetSong(song.title.c_str(), song.artist.c_str(),
                                               song.album.c_str()) >= 0)
        sentSerial = song.serial;
    const int playing = song.playing ? 1 : 0;
    if (playing != sentPlaying && qmSetPlaying(playing) >= 0) sentPlaying = playing;
    // The cover: the last one stays on the row while the next is made.
    uint32_t art = 0;
    if (qmArtFor(song.artUrl, &art) && (int64_t)art != sentArt && qmSetArt(art) >= 0)
        sentArt = art;
    for (int press; (press = qmPoll()) != QM_NONE;) {
        const nowplaying::Transport t = press == QM_NEXT       ? nowplaying::Transport::Next
                                        : press == QM_PREVIOUS ? nowplaying::Transport::Previous
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
// game closes, carries the queue on: VitaPlex's quick menu plugin plays it
// there through the shell's music player, which plays on beside games
// (docs/DESIGN_NOTES.md, BG Tests 10 to 21 and builds 2032 to 2047), and
// fetches the songs not on the card as they come up. The two talk through the
// files bgplayer.h lays out in ux0:data/VitaPlex and the plugin's pipes
// (qm_protocol.h). The app hands the queue over as it goes and takes the
// music back as it opens again.

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
    return m == RepeatMode::ONE ? BGPLAYER_REPEAT_ONE
         : m == RepeatMode::ALL ? BGPLAYER_REPEAT_ALL : BGPLAYER_REPEAT_OFF;
}

// ── The song made ready for SceShell ──────────────────────────────────────
// SceShell plays files, so a song handed over that is not on the card is
// fetched whole before it plays: 5.8 s and 5.0 s of silence in build 2045's
// run (log 5ae2ab41), each time for the song VitaPlex had been playing. So
// while a song plays here and could be handed over, its file is fetched in
// the background to the folder of songs, as SceShell would fetch it, and the
// queue names it (file=) once whole: SceShell plays it at once, and removes
// it once played. A download is left alone, since SceShell plays it where it
// is, and so is a song in a format the shell does not play, of which SceShell
// fetches the server's MP3 itself. One song at a time: the song before's file
// goes, and a fetch for a song no longer playing is stopped.
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

// The fetch, on a thread of its own: the song's details, its first part, and
// that file as the server has it into BGPLAYER_SONGS through a file beside
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
    // The formats the plugin plays as they are (its shellExt).
    std::string ext = lowerExt(item.partPath);
    if (ext == "mp4") ext = "m4a";
    if (ext != "mp3" && ext != "m4a" && ext != "aac" && ext != "wav") {
        brls::Logger::info("[bgaudio] SceShell: \"{}\" is {}: SceShell would fetch the server's MP3 "
                           "of it itself",
                           title, ext.empty() ? "of no known format" : ext);
        return;
    }
    const std::string path = std::string(BGPLAYER_SONGS) + "/" + key + "." + ext;
    const std::string part = path + ".ready";
    sceIoMkdir(BGPLAYER_SONGS, 0777);
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
    HttpClient client;
    const bool fetched = client.downloadFile(
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
    const bool closed = std::fclose(f) == 0;
    // Whole, as the answer gave its size, else as the details did: the client
    // takes a transfer cut short for one done. The answer's first, since the
    // details can be older than the file: 4,418,566 bytes for a song that
    // came whole at 4,343,040, which was thrown away, and SceShell fetched it
    // again with the music stopped (log dd6a5eb9, build 2046).
    const long long want = length > 0 ? length : item.partSize;
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
    std::string before;
    unsigned gen;
    {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        if (g_ready.key == key) return;
        before.swap(g_ready.path);
        g_ready.key = key;
        gen = ++g_ready.gen;
    }
    if (!before.empty()) std::remove(before.c_str());   // the song before's, not handed over
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

// The queue in the order it plays, the song playing and where in it, as
// bgplayer.h lays it out. A queue longer than bgplayer.h allows goes from
// the song playing on; the song's file made ready, if it is whole, in file=,
// and SceShell's from then. A download's cover is the PNG the server made of
// it beside the download, which the plugin shows as it is (bgplayer.h's
// BGPLAYER_TRACK_COVER). On the UI thread, which owns the queue. The song,
// for the log, in *what, and its ratingKey in *key.
bool writeQueue(std::string& what, std::string& key) {
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

    const QueueItem& now = items[(std::size_t)order[from + (std::size_t)at]];
    const std::string file = handReadySong(now.ratingKey);
    char head[320];
    std::snprintf(head, sizeof(head),
                  "written=%.6f\nindex=%d\npositionMs=%lld\nrepeat=%d\nfile=%s\n", systemSecs(),
                  at, positionMs, repeatCode(q.getRepeatMode()), file.c_str());
    std::string text = head;
    DownloadsManager& downloads = DownloadsManager::getInstance();
    for (std::size_t i = from; i < order.size() && i - from < BGPLAYER_MAX_TRACKS; i++) {
        const QueueItem& t = items[(std::size_t)order[i]];
        std::string local, cover;
        DownloadItem dl;
        if (downloads.getDownloadCopy(t.ratingKey, dl) && dl.state == DownloadState::COMPLETED) {
            local = dl.localPath;
            cover = DownloadsManager::smallCoverPath(dl.thumbPath);
            if (!cover.empty() && !fileExists(cover)) cover.clear();
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
    what = std::to_string(std::min<std::size_t>(order.size() - from, BGPLAYER_MAX_TRACKS)) +
           " song(s), from \"" + now.title + "\" at " + mmss((unsigned)positionMs) +
           (file.empty() ? std::string(", for SceShell to fetch") : ", from " + file);
    key = now.ratingKey;
    return true;
}

// VitaPlex's quick menu plugin open, loaded into SceShell first if it is not
// there: what SceShell plays the queue through. On the UI thread.
bool shellPluginOpen() {
    if (qmIsOpen()) return true;
    char what[320];
    const int rc = qmOpen(what, sizeof(what));
    brls::Logger::info("[bgaudio] SceShell: {} ({})", what, hex(rc));
    return rc >= 0;
}

// The cover made for the row here, put beside the songs for SceShell
// (QM_SONG_COVER_FMT) as the song playing's, so that the row keeps it.
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
    char from[64], to[160];
    std::snprintf(from, sizeof(from), QM_ART_PATH_FMT, (unsigned)(serial & 1));
    std::snprintf(to, sizeof(to), QM_SONG_COVER_FMT, key.c_str());
    const std::string part = std::string(to) + ".part";
    if (!copyFile(from, part) || !moveIn(part, to)) {
        std::remove(part.c_str());
        brls::Logger::info("[bgaudio] SceShell: the song's cover could not be put at {}", to);
    }
}

// The queue, written already, to SceShell (qm_protocol.h's
// QM_CMD_PLAY_QUEUE). The row is given back first, with the tick's lock
// held, so that nothing of this process's follows. true when the plugin
// took it.
bool handToShell() {
    std::lock_guard<std::mutex> tickLock(g_qmTickMutex);
    quickMenuRelease("handed to SceShell");
    const int rc = qmPlayQueue();
    brls::Logger::info("[bgaudio] SceShell: asked to play the queue: {}", hex(rc));
    return rc >= 0;
}

// Music playing goes to SceShell through VitaPlex's quick menu plugin, which
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
        brls::Logger::warning("[bgaudio] SceShell: {}, but VitaPlex's quick menu plugin is not "
                              "there to play the queue; nothing handed over",
                              why);
        return false;
    }
    std::string what, key;
    if (!writeQueue(what, key)) {
        brls::Logger::error("[bgaudio] SceShell: {}, but the queue could not be written to {}", why,
                            BGPLAYER_QUEUE);
        return false;
    }
    // The folder of songs, there for SceShell to fetch into, with the song's
    // cover.
    sceIoMkdir(BGPLAYER_SONGS, 0777);
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

// The songs SceShell played the queue from, once nothing plays them; but for
// the file of the song at keepKey, whole as the plugin moves its files in,
// which goes on here. Its path, or "".
std::string removePlayerSongs(const std::string& keepKey = std::string()) {
    const SceUID d = sceIoDopen(BGPLAYER_SONGS);
    if (d < 0) return std::string();
    SceIoDirent e;
    int removed = 0;
    std::string kept;
    for (;;) {
        std::memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        const std::string name = e.d_name, path = std::string(BGPLAYER_SONGS) + "/" + name;
        const std::string ext = lowerExt(name);
        if (!keepKey.empty() && kept.empty() && name.rfind(keepKey + ".", 0) == 0 &&
            name.find('.') == keepKey.size() && ext != "png" && ext != "part" && ext != "sent" &&
            ext != "ready") {
            kept = path;
            continue;
        }
        if (std::remove(path.c_str()) == 0) removed++;
    }
    sceIoDclose(d);
    if (removed)
        brls::Logger::info("[bgaudio] take back: removed {} song file(s) SceShell left{}", removed,
                           kept.empty() ? "" : ", but for the file of the song taken back");
    return kept;
}

// The song the shell plays on for SceShell's queue as VitaPlex opens: its
// clock read twice, half a second apart, and its length checked against the
// queue's song, so that the system Music app's song is left alone (BG Test
// 21, build 2012). Stopped when it is the queue's. Where it was, or -1.
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

// Why the queue at BGPLAYER_QUEUE is not the one SceShell played, as its
// status says, which the take-back goes on with; "" when it is.
std::string queueProblem(bool haveQueue, const std::map<std::string, std::string>& queue,
                         std::size_t tracks, const std::map<std::string, std::string>& status) {
    char why[128];
    if (!haveQueue) {
        std::snprintf(why, sizeof(why), "its queue at %s is %s", BGPLAYER_QUEUE,
                      fileExists(BGPLAYER_QUEUE) ? "not whole" : "gone");
    } else if (fieldNum(queue, "written") != fieldNum(status, "queue")) {
        std::snprintf(why, sizeof(why), "its queue at %s is another, written at %.6f, not %.6f",
                      BGPLAYER_QUEUE, fieldNum(queue, "written"), fieldNum(status, "queue"));
    } else if (fieldNum(status, "index") < 0 || fieldNum(status, "index") >= (double)tracks) {
        std::snprintf(why, sizeof(why), "its song %d is not among the %d of its queue",
                      (int)fieldNum(status, "index") + 1, (int)tracks);
    } else {
        return std::string();
    }
    return why;
}

// What SceShell had got to with the queue handed over, taken back as VitaPlex
// opens: told to stop going on, it writes where it got to, and its song, left
// playing in the shell, is stopped there. The queue then plays on here from
// that song and place. Off the UI thread: it waits on the shell.
void takeBack() {
    // SceShell, if it plays a queue, stops there now that this app is ready
    // to go on with it, and writes where it got to (qm_link.h's qmTakeBack).
    // Not as the app starts: one closed before it got this far left
    // SceShell's song to play out with nothing to go on after it (log
    // dcb5cc5d, build 2043). Never loads the plugin.
    const double askedShell = systemSecs();
    const int rcShell = qmTakeBack();
    g_takeBackSent = rcShell >= 0;
    if (g_takeBackSent)
        brls::Logger::info("[bgaudio] quick menu plugin told VitaPlex takes the music back ({})",
                           hex(rcShell));
    std::map<std::string, std::string> status, queue;
    std::vector<std::vector<std::string>> tracks;
    const bool haveQueue = readNote(BGPLAYER_QUEUE, queue, &tracks);
    if (!readNote(BGPLAYER_STATUS, status, nullptr)) {
        removePlayerSongs();   // never handed over, or long done
        return;
    }
    // The system clock starts again with the Vita, so a status written later
    // than now is from before it last started: one read as written 1101 s
    // after VitaPlex started (log fbc54add). Nothing of it plays.
    if (fieldNum(status, "written") > systemSecs() + 1.0) {
        brls::Logger::info("[bgaudio] take back: SceShell's status (\"{}\") is from before the "
                           "Vita last started",
                           fieldText(status, "state"));
        removePlayerSongs();
        return;
    }
    // SceShell playing the queue (qm_protocol.h) was told above to stop, and
    // says where it stopped: its status is read again until it is one written
    // after that.
    for (int i = 0; i < 20 && g_takeBackSent && fieldNum(status, "written") < askedShell &&
                    fieldText(status, "state") == BGPLAYER_STATE_PLAYING;
         i++) {
        sceKernelDelayThread(100 * 1000);
        status.clear();
        readNote(BGPLAYER_STATUS, status, nullptr);
    }
    // Written as it stopped, after it was told; else as it played, which it
    // writes every 2 s, before this app started. A status older than that is
    // of music long over.
    const double written = fieldNum(status, "written");
    const bool fresh = written >= askedShell || g_initSecs - written < 20.0;
    brls::Logger::info("[bgaudio] take back: SceShell last said \"{}\", song {} at {}, {:.1f} s "
                       "{} VitaPlex started",
                       fieldText(status, "state"), (int)fieldNum(status, "index") + 1,
                       mmss((unsigned)std::max(0.0, fieldNum(status, "positionMs"))),
                       std::fabs(written - g_initSecs), written >= g_initSecs ? "after" : "before");
    const bool said = fieldText(status, "state") == BGPLAYER_STATE_PLAYING;
    int index = -1;
    long long at = -1;
    if (said && !fresh) {
        brls::Logger::info("[bgaudio] take back: that is too long ago to go on from");
    } else if (said) {
        // The shell's song is stopped only with the queue to go on with here:
        // log c8dab58b stopped one with none, and left silence.
        const std::string problem = queueProblem(haveQueue, queue, tracks.size(), status);
        if (!problem.empty()) {
            brls::Logger::warning("[bgaudio] take back: {}; its song is left to play on in the "
                                  "system player",
                                  problem);
            return;
        }
        if (fieldNum(status, "fetching") == 1) {
            // SceShell had stopped the shell to fetch the song it names
            // (qm_protocol.h): nothing plays there, and the song goes on here
            // from its start.
            index = (int)fieldNum(status, "index");
            at = (long long)std::max(0.0, fieldNum(status, "positionMs"));
            brls::Logger::info("[bgaudio] take back: SceShell was fetching song {}; it plays here "
                               "from {}",
                               index + 1, mmss((unsigned)at));
        } else {
            at = takeFromShell((int)fieldNum(status, "durationMs"));
            if (at >= 0) index = (int)fieldNum(status, "index");
        }
    }
    // Not while the shell may still play one of them: a file written over as
    // the shell played it preceded a crash (logs 8fe35d97, 7ba90482). The
    // song taken back keeps its file, made ready for the next hand-over.
    if (!said || at >= 0) {
        const std::string key = index >= 0 ? fieldText(status, "ratingKey") : std::string();
        const std::string kept = removePlayerSongs(key);
        if (!kept.empty()) keepReadySong(key, kept);
    }
    if (index < 0 || at < 0) return;

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
            brls::Logger::info("[bgaudio] take back: VitaPlex plays something already; the queue "
                               "is not taken back");
            return;
        }
        MusicQueue::getInstance().setRepeatMode(repeat == BGPLAYER_REPEAT_ONE ? RepeatMode::ONE
                                                : repeat == BGPLAYER_REPEAT_ALL ? RepeatMode::ALL
                                                                                : RepeatMode::OFF);
        brls::Logger::info("[bgaudio] take back: the queue goes on here, \"{}\" at {}",
                           items[(std::size_t)index].title, mmss((unsigned)at));
        brls::Application::pushActivity(
            PlayerActivity::createWithRestoredQueue(items, index, at));
    });
}

}  // namespace

void init(int outsideMusic) {
    if (outsideMusic < kOutsideStops || outsideMusic > kOutsideHandsOver)
        outsideMusic = kOutsideStops;
    g_outsideMusic = outsideMusic;
    g_keepPlaying = outsideMusic != kOutsideStops;
    g_initSecs = systemSecs();
    qmInit();
    brls::Logger::info("[bgaudio] music after leaving: {}",
                       outsideMusic == kOutsidePlaysOn     ? "plays on, then SceShell"
                       : outsideMusic == kOutsideHandsOver ? "SceShell at once"
                                                           : "stops");
    startWatcher();
    // SceShell may be playing a queue handed over before (qm_link.h): it
    // plays on until takeBack, once the first frame is up, tells it to stop.
    // Whatever the setting: music handed over before it was changed comes
    // back all the same. In the first frame, once the server is connected
    // and the main screen is up; the song plays on in the shell until then.
    brls::sync([]() { platform::launchThread([]() { takeBack(); }); });
}

void setOutsideMusic(int mode) {
    if (mode < kOutsideStops || mode > kOutsideHandsOver) mode = kOutsideStops;
    g_outsideMusic = mode;
    const bool on = mode != kOutsideStops;
    g_keepPlaying = on;
    brls::Logger::info("[bgaudio] music after leaving set to {}", mode);
    if (on) {
        if (MpvPlayer::getInstance().isPlaying()) holdPort("mpv, already playing");
    } else {
        dropReadySong();
        releasePort("setting turned off");
    }
}

int outsideMusic() { return g_outsideMusic; }

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
    if (!on && g_qmActive.exchange(false)) qmDeactivate();
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
