/**
 * The quick menu's Music row, through a plugin in SceShell.
 *
 * The row is SceShell's, and no call an app can make lights its buttons for
 * it (docs/DESIGN_NOTES.md: every system player service, every way of taking
 * the BGM port). Code inside SceShell can: VitaPlex's own plugin
 * (src/qm_plugin_psv, module/vitaplex_qm.suprx in VitaPlexBG.vpk), and the
 * one it is modelled on, ElevenMPV-A's shell_plugin.suprx, which
 * VitaPlexBG.vpk also carries as it is (third_party/elevenmpva-shell-plugin).
 * This loads VitaPlex's own, unless it is in SceShell already this boot, and
 * falls back to ElevenMPV-A's where VitaPlex's will not load or open. Each is
 * spoken to in its own messages: VitaPlex's in qm_protocol.h, ElevenMPV-A's
 * in its ipc.h (ElevenMPV-A-Common/include/ipc.h at v7.10). Both put the
 * song on the row's two lines and send the presses of its three buttons back.
 *
 * Shared by VitaPlex BG and its background player. Plain C; every call is
 * safe from any thread once qmInit has run, and none waits for long.
 */

#ifndef VITAPLEX_QM_LINK_H
#define VITAPLEX_QM_LINK_H

#include <stddef.h>

#include "qm_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BGPLAYER_APP_TITLEID
#error "CMakeLists.txt defines BGPLAYER_APP_TITLEID"
#endif

// VitaPlex's own plugin, in VitaPlex BG's folder.
#define QM_OWN_PLUGIN_PATH "ux0:app/" BGPLAYER_APP_TITLEID "/module/vitaplex_qm.suprx"
// ElevenMPV-A's, tried in this order: VitaPlex BG's copy, at the place in its
// app's folder that ElevenMPV-A uses in its own; then ElevenMPV-A's (title
// GRVA00008), if ElevenMPV-A is installed.
#define QM_PLUGIN_PATH       "ux0:app/" BGPLAYER_APP_TITLEID "/module/shell_plugin.suprx"
#define QM_PLUGIN_PATH_EMPVA "ux0:app/GRVA00008/module/shell_plugin.suprx"

// A press on the row.
enum { QM_NONE = 0, QM_PLAY_PAUSE, QM_NEXT, QM_PREVIOUS };

// Once, before any other call.
void qmInit(void);

// Opens a plugin's pipes, loading the plugin into SceShell first when they
// are not there: VitaPlex's own, else ElevenMPV-A's from the first of its
// copies above that is there. 0 when open, else the code that stopped it;
// what happened, for a log, in what.
int qmOpen(char* what, size_t size);
int qmIsOpen(void);
// Whether the plugin open is VitaPlex's own.
int qmIsOwn(void);

// Has the plugin take the row whenever the quick menu opens, for owner
// (QM_OWNER_*). 0, or the first failing send's code.
int qmActivate(int owner);
// Once a second while the row is wanted: VitaPlex's plugin gives the row
// back by itself when this stops (QM_ACTIVE_TIMEOUT_S). Never waits.
int qmHeartbeat(int owner);
// Gives the row back to the system.
int qmDeactivate(void);

// The song on the row: the title above, "artist / album" below. UTF-8.
int qmSetSong(const char* title, const char* artist, const char* album);
// Whether the song plays (1) or is paused (0). VitaPlex's plugin only.
int qmSetPlaying(int playing);
// How VitaPlex's plugin switches the row's buttons on (QM_LOOK_*).
int qmSetLook(int look);
// The song's cover, written to QM_ART_PATH_FMT under this serial first; 0
// for none. VitaPlex's plugin only.
int qmSetArt(uint32_t serial);
// A test: VitaPlex's plugin plays the file at path itself, from inside
// SceShell (QM_CMD_MUSIC_TEST). -1 with ElevenMPV-A's plugin.
int qmMusicTest(const char* path);
// The background player's: a song of its queue that is on the memory card
// (QM_CMD_SONG_FILE), for SceShell to carry the queue on with should the
// system end the player. -1 with ElevenMPV-A's plugin.
int qmSongFile(int index, const char* key, const char* path);
// VitaPlex BG's, as it opens: it takes the music back (QM_CMD_TAKE_BACK).
// Sent to VitaPlex's plugin when it is in SceShell, whether or not this
// process has opened it, and never loads it. 0 when sent; else the code
// (the pipe's absence when the plugin is not there).
int qmTakeBack(void);

// A press waiting on the row, or QM_NONE. Never waits.
int qmPoll(void);

#ifdef __cplusplus
}
#endif

#endif
