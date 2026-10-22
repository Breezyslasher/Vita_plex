/**
 * What VitaPlex says to its quick menu plugin (src/qm_plugin_psv), and what
 * the plugin says back: two message pipes the plugin makes as it starts in
 * SceShell. Shared by qm_link.c and the plugin. Plain C.
 */

#ifndef VITAPLEX_QM_PROTOCOL_H
#define VITAPLEX_QM_PROTOCOL_H

#include <stdint.h>

// The pipes' names carry the protocol's version. A plugin loaded stays until
// the Vita restarts, so a VitaPlex with another version finds no pipes of
// its version and loads its own plugin beside the older one, which nothing
// then asks for the row. docs/DESIGN_NOTES.md has what each version up to
// 11 added, the tests among them; version 12's is the one VitaPlex.vpk
// itself carries, with the tests and the ways of switching the buttons on
// that were tried gone. Version 13 has the files handover.h names, and keeps
// the song before and the songs fetched ahead for VitaPlex to play on.
#define QM_PIPE_TO_PLUGIN   "VitaPlexQM13::ToPlugin"
#define QM_PIPE_FROM_PLUGIN "VitaPlexQM13::FromPlugin"

// SceShell carries the queue on. The system closes VitaPlex to start a game,
// and SceShell is never ended. So VitaPlex hands its queue to SceShell as it
// goes (QM_CMD_PLAY_QUEUE), and the plugin plays it from inside SceShell,
// beginning at the queue's song and place in it:
//
// - That song plays from the file VitaPlex made ready, else its download,
//   else it is fetched; then the queue's next song: its download if the shell
//   plays its format, else the song's file fetched from VitaPlex's server
//   into the folder of songs (handover.h's HANDOVER_SONGS), as the server's
//   MP3 of it with a length header in front when the shell does not play
//   its format. The next song is fetched while the one before plays; one
//   that has not come by the time it is wanted is waited for, the shell
//   stopped meanwhile. Songs the server has no file of are passed over, as
//   are all those not on the card when VitaPlex's settings name no server.
//   The files of the song before, the one playing and the next stay there,
//   and all of them as VitaPlex takes the music back, which plays on from
//   them.
// - The row is SceShell's: the song, its cover (QM_SONG_COVER_FMT, made by
//   VitaPlex for its song and the downloads after it, fetched for the rest),
//   and its buttons, which act on SceShell's music.
// - A song played to its end is marked played on the server, and the server
//   is told where the music is, as VitaPlex tells it (its timeline).
// - The status (handover.h) is written with "by=SceShell" in it, so that
//   VitaPlex takes the music back where SceShell has got to; with
//   "fetching=1" while it waits for a song, which VitaPlex then plays from
//   where the status says. It asks first (QM_CMD_TAKE_BACK).
// - It stops at the queue's end, when no further song is on the card or the
//   server, when three songs in a row will not come, when the shell plays
//   something else, and when any process takes the row.

// To the plugin, a whole QmMessage each time.
enum {
    // Take the Music row whenever the quick menu opens. Sent again every
    // second for as long as it is wanted; with none for QM_ACTIVE_TIMEOUT_S,
    // the plugin gives the row back itself, as for a process that has gone.
    // arg: QM_OWNER_*.
    QM_CMD_ACTIVATE = 1,
    QM_CMD_DEACTIVATE,  // give the row back
    QM_CMD_SONG,        // title, artist and album for the row's two lines
    QM_CMD_PLAYING,     // arg: 1 playing, 0 paused
    // arg: the song's cover by its serial, written to QM_ART_PATH_FMT before
    // it is sent; 0 for none, the heading's own icon. A cover is its
    // sender's: the plugin forgets it when the row is given back or another
    // owner takes it.
    QM_CMD_ART,
    // From VitaPlex, once ready to go on with the music it handed over: it
    // takes the music back. A queue SceShell carries on stops where it is,
    // with the song left playing for VitaPlex to stop, and the status
    // written one last time; a queue handed over and not yet begun is not
    // begun.
    QM_CMD_TAKE_BACK,
    // From VitaPlex as it goes: SceShell plays the queue at HANDOVER_QUEUE
    // (handover.h) itself, from its song and place there (index=, positionMs=,
    // and file= when the app made that song ready as a file), and takes the
    // row for it. Sent once the row has been given back, and the queue
    // written whole.
    QM_CMD_PLAY_QUEUE
};
enum { QM_OWNER_APP = 1 };

// A song's cover for SceShell to show as it carries the queue on: a PNG as
// QM_ART_PATH_FMT's, in the folder of songs (handover.h's HANDOVER_SONGS)
// with the song's ratingKey in its name. VitaPlex puts its song's there as
// it hands the queue over, and those of the downloads after it; SceShell
// fetches the others, or takes the one of a song of the same album.
#define QM_SONG_COVER_FMT "ux0:data/VitaPlex/handover/%s.png"
#define QM_ACTIVE_TIMEOUT_S 5

// The plugin's log, one file a boot.
#define QM_PLUGIN_LOG "ux0:data/VitaPlex/qm-plugin.log"

// The cover: a PNG, QM_ART_SIZE square, at this path with the lowest bit of
// its serial (0 or 1) in it. One serial after another goes to the other
// file, so that the file the plugin may still be loading is never the one
// being written. Serials only grow in a boot, from process to process: a
// plugin that showed a cover from a VitaPlex since closed never takes a new
// one for it.
#define QM_ART_PATH_FMT "ux0:data/VitaPlex/qm-art-%u.png"
#define QM_ART_SIZE     64

typedef struct QmMessage {
    uint32_t cmd;
    uint32_t arg;
    uint16_t title[256];  // UTF-16, ending in 0
    uint16_t artist[256];
    uint16_t album[256];
} QmMessage;

// From the plugin, a QmPress for each press of the row's buttons.
enum { QM_PRESS_PLAY_PAUSE = 1, QM_PRESS_NEXT, QM_PRESS_PREVIOUS };
typedef struct QmPress {
    uint32_t press;
    uint32_t reserved;
} QmPress;

// How many messages each pipe holds. A press is sent without waiting, from
// SceShell's own thread, and dropped when the pipe is full: a process that
// stopped reading can never hold the quick menu up.
#define QM_TO_PLUGIN_SLOTS   4
#define QM_FROM_PLUGIN_SLOTS 16

#endif
