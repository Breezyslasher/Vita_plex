/**
 * The quick menu's Music row, and the music after VitaPlex has gone, through
 * VitaPlex's quick menu plugin in SceShell.
 *
 * The row is SceShell's, and no call an app can make lights its buttons for
 * it (docs/DESIGN_NOTES.md: every system player service, every way of taking
 * the BGM port). Code inside SceShell can: VitaPlex's plugin
 * (src/qm_plugin_psv, module/vitaplex_qm.suprx in VitaPlex.vpk), modelled on
 * ElevenMPV-A's. This loads it, unless it is in SceShell already this boot,
 * and speaks to it in the messages qm_protocol.h lays out: the song on the
 * row's two lines, its cover and ⏸, the presses of its three buttons back,
 * and the queue handed over for SceShell to play on.
 *
 * Plain C; every call is safe from any thread once qmInit has run, and none
 * waits for long.
 */

#ifndef VITAPLEX_QM_LINK_H
#define VITAPLEX_QM_LINK_H

#include <stddef.h>

#include "qm_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VITAPLEX_TITLEID
#error "CMakeLists.txt defines VITAPLEX_TITLEID"
#endif

// VitaPlex's plugin, in its app's folder.
#define QM_OWN_PLUGIN_PATH "ux0:app/" VITAPLEX_TITLEID "/module/vitaplex_qm.suprx"

// A press on the row.
enum { QM_NONE = 0, QM_PLAY_PAUSE, QM_NEXT, QM_PREVIOUS };

// Once, before any other call.
void qmInit(void);

// Opens the plugin's pipes, loading the plugin into SceShell first when they
// are not there. 0 when open, else the code that stopped it; what happened,
// for a log, in what.
int qmOpen(char* what, size_t size);
int qmIsOpen(void);

// Has the plugin take the row whenever the quick menu opens, for owner
// (QM_OWNER_*). 0, or the send's code.
int qmActivate(int owner);
// Once a second while the row is wanted: the plugin gives the row back by
// itself when this stops (QM_ACTIVE_TIMEOUT_S). Never waits.
int qmHeartbeat(int owner);
// Gives the row back to the system.
int qmDeactivate(void);

// The song on the row: the title above, "artist / album" below. UTF-8.
int qmSetSong(const char* title, const char* artist, const char* album);
// Whether the song plays (1) or is paused (0).
int qmSetPlaying(int playing);
// The song's cover, written to QM_ART_PATH_FMT under this serial first; 0
// for none.
int qmSetArt(uint32_t serial);
// As VitaPlex goes: SceShell plays its queue, written to HANDOVER_QUEUE
// first, from the queue's song and place (QM_CMD_PLAY_QUEUE).
int qmPlayQueue(void);
// As VitaPlex opens: it takes the music back (QM_CMD_TAKE_BACK). Sent to the
// plugin when it is in SceShell, whether or not this process has opened it,
// and never loads it. 0 when sent; else the code (the pipe's absence when
// the plugin is not there).
int qmTakeBack(void);

// As VitaPlex quits to be updated: the plugin installs the update unpacked to
// QM_UPDATE_DIR once VitaPlex has gone, and launches it again
// (QM_CMD_INSTALL_UPDATE). 0 when sent.
int qmInstallUpdate(void);

// As VitaPlex goes: SceShell carries on the downloads written to
// QM_DOWNLOADS_LIST first, once VitaPlex has gone (QM_CMD_DOWNLOADS). 0 when
// sent.
int qmCarryDownloads(void);
// As VitaPlex opens: SceShell stops the downloads it carries on
// (QM_CMD_TAKE_DOWNLOADS). As qmTakeBack, sent only to a plugin that is in
// SceShell, and never loads it: 0 when sent, else the code (the pipe's
// absence when the plugin is not there, and nothing carried on).
int qmTakeDownloads(void);

// VitaPlex's download notification, shown by the plugin (QM_CMD_DL_PROGRESS):
// two lines, UTF-8, and how far, 0 to 1000, or QM_PROGRESS_UNKNOWN. Only to
// a plugin this process has opened; 0 when sent.
int qmDownloadProgress(const char* title, const char* detail, uint32_t permille);
// The notification ended, with these two lines (QM_CMD_DL_PROGRESS_END).
int qmDownloadProgressEnd(const char* title, const char* detail);

// A press waiting on the row, or QM_NONE. Never waits.
int qmPoll(void);

#ifdef __cplusplus
}
#endif

#endif
