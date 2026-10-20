/**
 * VitaPlex - background audio experiment (Vita)
 *
 * Leaving VitaPlex on a Vita, for the LiveArea or another app, stops the
 * music. The "Background Music" setting only covers leaving the player screen
 * inside the app. Two ways past that are being tried, and this is the harness
 * that finds out on real hardware which of them holds:
 *
 *   BGM port   Keep playing through mpv, in this process, on the audio port the
 *              system lets an app keep once it is not in front
 *              (sceAppMgrAcquireBgmPortWithPriority, as ElevenMPV-A does for
 *              its own decoders). Streams exactly as now, provided the process
 *              keeps running while deactivated, which is what the watcher
 *              records.
 *   SceShell   Hand the audio to the system's music service, the one the Music
 *              app plays through (lib/libshellaudio). Keeps playing whatever
 *              happens to this process, but only formats and sources the shell
 *              itself accepts. Whether that includes an http(s) URL is exactly
 *              what the stream test is for.
 *
 * Everything goes to the log under "[bgaudio]", with every system call's return
 * code, so one test run can be read back afterwards. Other platforms get the
 * inline no-ops below and no call site needs an #ifdef.
 *
 * On a Vita only VitaPlex BG has the tests: the same eboot as VitaPlex,
 * packaged as a non-game app (VitaPlexBG.vpk), the kind the system lets run on
 * in the background. VitaPlex itself is a game, held the moment it leaves the
 * foreground, and it is the package that is released, so it gets none of this
 * until background music works there.
 *
 * VitaPlex BG plays on after PS through the BGM port, but the system closes it
 * to start a game. Its package carries a background player for that
 * (src/bgplayer_psv, bgplayer.h): a small program of its own that outlives the
 * app and plays the queue through the shell's music player, which plays on
 * beside games. The app hands it the queue as the system closes it, or as soon
 * as it is left, by the setting, and takes the music back from it when opened
 * again.
 */

#pragma once

#include <functional>
#include <string>

namespace vitaplex {
namespace bgaudio {

using Report = std::function<void(const std::string&)>;

#if defined(__vita__)

// Whether the tests exist here at all: running as VitaPlex BG.
bool available();

// What happens to music playing as the user leaves VitaPlex BG (the setting).
enum OutsideMusic {
    kOutsideStops = 0,        // it stops, as in VitaPlex
    kOutsidePlaysOn = 1,      // VitaPlex BG plays on; its background player
                              // takes over when the system closes it
    kOutsideHandsOver = 2,    // the background player takes over at once, on PS
};

// Called once at startup, after settings are loaded, with the saved setting.
// Outside VitaPlex BG it is taken as kOutsideStops, whatever was saved. In
// VitaPlex BG it also takes the music back from its background player, once
// the first frame is up.
void init(int outsideMusic);

// The setting. Anything but kOutsideStops holds the BGM port while music
// plays, and has mpv resample music to 44.1 kHz so its Vita output uses that
// port (keepPlaying).
void setOutsideMusic(int mode);
int outsideMusic();
bool keepPlaying();

// Ends the process: through the system call under sceKernelExitProcess once
// VitaPlex BG has asked for its background player, since that function did
// not return then (docs/DESIGN_NOTES.md, BG Tests 18 to 21).
[[noreturn]] void exitProcess(int code);

// From MpvPlayer, audio-only mode only.
void onAudioPlayback(bool playing);
void onTrackLoad();
void onPlayerShutdown();

// SceShell tests. The work runs on its own thread and can take most of a
// minute; done is then called on the UI thread with a report for a person.
void runShellFileTest(Report done);
void runShellStreamTest(Report done);
// Stops whatever the tests left playing. Returns what happened.
std::string stopShellTest();

// BG Test 11: pauses the song VitaPlex is playing, writes it, where it was
// paused and the queue's next song for VPLXBGT11, and starts that app, which
// carries them on through the shell. done gets a report for a person.
void runHandoffTest(Report done);

// Which shell service the tests use: 0-4 are the music player service's
// client types (ElevenMPV-A uses 0), 5 the application-BGM service. Which of
// them the quick menu's Music controls answer to, and whether any takes a
// URL, is what switching between them finds out. Not saved.
int shellService();
void setShellService(int service);

// How VitaPlex BG takes the system's BGM port: 0 at priority 0x81 for mpv
// while background playback is on, as before; 1 at 0x80, 2 with no priority,
// 3 as a music player, 4 not at all, each whenever mpv plays. The system
// player tests and the background player (through the queue) take it the
// same way, except at 0x80 for 0 and 1. "As a music player" put VitaPlex's
// icon in the quick menu's Music row, its buttons dimmed, with mpv playing
// (log 9c16c0d6), and the system player would not play with it held (log
// dbd9b556). Not saved.
int ownPortTest();
void setOwnPortTest(int choice);

// The quick menu's Music row, through VitaPlex's quick menu plugin, or
// ElevenMPV-A's where that will not load (src/bgplayer_psv/qm_link.h): the
// song on it and its three buttons working, for VitaPlex BG's own playback
// and, through the queue, the background player's. On or off, as saved.
void setQuickMenuControls(bool on);
// How VitaPlex's plugin switches the row's buttons on (QM_LOOK_* in
// qm_protocol.h): ElevenMPV-A's way left them dimmed. Not saved.
int quickMenuLook();
void setQuickMenuLook(int look);
// From the OS now-playing bridge (now_playing.cpp): what MusicController
// publishes, and that it was cleared. artUrl is now_playing.hpp's: the
// cover's thumbnail URL, a download's local copy, or "" for none.
void quickMenuNowPlaying(const std::string& title, const std::string& artist,
                         const std::string& album, const std::string& artUrl, bool playing);
void quickMenuNowPlayingCleared();

#else

inline bool available() { return false; }
inline void init(int) {}
inline void setOutsideMusic(int) {}
inline int outsideMusic() { return 0; }
inline bool keepPlaying() { return false; }
inline void onAudioPlayback(bool) {}
inline void onTrackLoad() {}
inline void onPlayerShutdown() {}
inline void runShellFileTest(Report) {}
inline void runShellStreamTest(Report) {}
inline std::string stopShellTest() { return {}; }
inline void runHandoffTest(Report) {}
inline int shellService() { return 0; }
inline void setShellService(int) {}
inline int ownPortTest() { return 0; }
inline void setOwnPortTest(int) {}
inline void setQuickMenuControls(bool) {}
inline int quickMenuLook() { return 0; }
inline void setQuickMenuLook(int) {}
inline void quickMenuNowPlaying(const std::string&, const std::string&, const std::string&,
                                const std::string&, bool) {}
inline void quickMenuNowPlayingCleared() {}

#endif

}  // namespace bgaudio
}  // namespace vitaplex
