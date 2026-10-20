/**
 * The quick menu's Music row, through ElevenMPV-A's shell plugin.
 *
 * The row is SceShell's, and no call an app can make lights its buttons for
 * it (docs/DESIGN_NOTES.md: every system player service, every way of taking
 * the BGM port). ElevenMPV-A lights them from a plugin it loads into SceShell,
 * shell_plugin.suprx, and talks to it through two message pipes the plugin
 * makes (ElevenMPV-A-Common/include/ipc.h, GrapheneCt/ElevenMPV-A at v7.10).
 * VitaPlexBG.vpk carries that plugin as it is, from ElevenMPV-A's v7.10
 * release, with its licence and source (third_party/elevenmpva-shell-plugin).
 * This loads it the way ElevenMPV-A does, unless the plugin is in SceShell
 * already this boot, and speaks the same messages: the song for the row's
 * two lines, and the presses of its three buttons back.
 *
 * Shared by VitaPlex BG and its background player. Plain C; every call is
 * safe from any thread once qmInit has run, and none waits for long.
 */

#ifndef VITAPLEX_QM_LINK_H
#define VITAPLEX_QM_LINK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BGPLAYER_APP_TITLEID
#error "CMakeLists.txt defines BGPLAYER_APP_TITLEID"
#endif

// The plugin's copies, tried in this order: VitaPlex BG's own, at the place
// in its app's folder that ElevenMPV-A uses in its own; then ElevenMPV-A's
// (title GRVA00008), if ElevenMPV-A is installed.
#define QM_PLUGIN_PATH       "ux0:app/" BGPLAYER_APP_TITLEID "/module/shell_plugin.suprx"
#define QM_PLUGIN_PATH_EMPVA "ux0:app/GRVA00008/module/shell_plugin.suprx"

// A press on the row.
enum { QM_NONE = 0, QM_PLAY_PAUSE, QM_NEXT, QM_PREVIOUS };

// Once, before any other call.
void qmInit(void);

// Opens the plugin's pipes, loading the plugin into SceShell first when they
// are not there, from the first of its copies above that is. 0 when open,
// else the code that stopped it; what happened, for a log, in what.
int qmOpen(char* what, size_t size);
int qmIsOpen(void);

// Has the plugin take the row over whenever the quick menu opens: its
// watching thread started (an app that stops sends the plugin a stop), then
// the row handed to this process. 0, or the first failing send's code.
int qmActivate(void);
// Gives the row back to the system.
int qmDeactivate(void);

// The song on the row: the title above, "artist / album" below. UTF-8.
int qmSetSong(const char* title, const char* artist, const char* album);

// A press waiting on the row, or QM_NONE. Never waits.
int qmPoll(void);

#ifdef __cplusplus
}
#endif

#endif
