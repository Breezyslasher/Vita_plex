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
 */

#pragma once

#include <functional>
#include <string>

namespace vitaplex {
namespace bgaudio {

using Report = std::function<void(const std::string&)>;

#if defined(__vita__)

// Whether the tests exist on this platform at all.
inline bool available() { return true; }

// Called once at startup, after settings are loaded, with the saved toggle.
void init(bool keepPlaying);

// The settings toggle: hold the BGM port while music plays, and have mpv
// resample music to 44.1 kHz so its Vita output uses that port.
void setKeepPlaying(bool on);
bool keepPlaying();

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

#else

inline bool available() { return false; }
inline void init(bool) {}
inline void setKeepPlaying(bool) {}
inline bool keepPlaying() { return false; }
inline void onAudioPlayback(bool) {}
inline void onTrackLoad() {}
inline void onPlayerShutdown() {}
inline void runShellFileTest(Report) {}
inline void runShellStreamTest(Report) {}
inline std::string stopShellTest() { return {}; }

#endif

}  // namespace bgaudio
}  // namespace vitaplex
