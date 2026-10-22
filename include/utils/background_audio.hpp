/**
 * VitaPlex - music after leaving the app (Vita)
 *
 * VitaPlex is packaged as a non-game application, the kind the system lets
 * run on once it is left with PS: music playing goes on through mpv, on the
 * system's background-music (BGM) port, as ElevenMPV-A's does. The system
 * closes such an app to start a game, though, so as it does, the queue goes
 * to SceShell, which no app or game closes: VitaPlex's quick menu plugin
 * (src/qm_plugin_psv) plays it on there through the shell's music player,
 * which plays on beside games, and fetches the songs that are not on the
 * memory card as they come up (qm_protocol.h, bgplayer.h). By the setting it
 * goes there as soon as the app is left. Opened again, VitaPlex takes the
 * music back where SceShell got to.
 *
 * The same plugin puts the song, its cover and working buttons in the quick
 * menu's Music row while VitaPlex plays.
 *
 * Other platforms get the inline no-ops below, so no call site needs an #ifdef.
 * docs/DESIGN_NOTES.md has how this was found out, test by test.
 */

#pragma once

#include <string>

namespace vitaplex {
namespace bgaudio {

#if defined(__vita__)

// Whether music can go on after leaving the app here.
inline bool available() { return true; }

// What happens to music playing as the user leaves VitaPlex (the setting).
enum OutsideMusic {
    kOutsideStops = 0,        // it stops
    kOutsidePlaysOn = 1,      // VitaPlex plays on; SceShell takes over when the
                              // system closes it
    kOutsideHandsOver = 2,    // SceShell takes over at once, on PS
};

// Called once at startup, after settings are loaded, with the saved setting.
// Also takes the music back from SceShell, once the first frame is up.
void init(int outsideMusic);

// The setting. Anything but kOutsideStops holds the BGM port while music
// plays, and has mpv resample music to 44.1 kHz so its Vita output uses that
// port (keepPlaying).
void setOutsideMusic(int mode);
int outsideMusic();
bool keepPlaying();

// Ends the process.
[[noreturn]] void exitProcess(int code);

// From MpvPlayer, audio-only mode only.
void onAudioPlayback(bool playing);
void onPlayerShutdown();

// The quick menu's Music row, through VitaPlex's quick menu plugin
// (src/bgplayer_psv/qm_link.h): the song on it and its three buttons working
// while VitaPlex plays. SceShell takes the row itself as it plays the queue
// handed over. On or off, as saved.
void setQuickMenuControls(bool on);
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
inline void onPlayerShutdown() {}
inline void setQuickMenuControls(bool) {}
inline void quickMenuNowPlaying(const std::string&, const std::string&, const std::string&,
                                const std::string&, bool) {}
inline void quickMenuNowPlayingCleared() {}

#endif

}  // namespace bgaudio
}  // namespace vitaplex
