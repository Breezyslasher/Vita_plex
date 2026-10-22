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
// then asks for the row. Version 2's plugin waits before it takes the row and
// keeps it as set while the quick menu is open; version 3's lights the
// buttons and icons where they are drawn, and sets the song's lines white,
// which version 2's set transparent; version 4's shows ⏸ while VitaPlex
// plays (QM_CMD_PLAYING) and logs each icon's textures; version 5's changes
// the row on SceShell's own UI thread, and shows the song's cover on the
// Music heading's icon (QM_CMD_ART); version 6's plays a file itself, from
// inside SceShell, for a test (QM_CMD_MUSIC_TEST); version 7's carries the
// background player's queue on when the system ends the player (below);
// version 8's fetches the start of a song from the server, from inside
// SceShell, for a test (QM_CMD_HTTP_TEST); version 9's fetches the queue's
// songs that are not on the memory card as it carries the queue on, and
// says so in the status ("fetching=1"); version 10's takes the queue from
// VitaPlex BG itself (QM_CMD_PLAY_QUEUE), fetches covers for the songs on the
// card too, and marks the songs it played to their end played; version 11's
// fetches songs in formats the shell does not play as the server's MP3s, and
// has no background player to carry on for (QM_CMD_SONG_FILE and
// QM_OWNER_PLAYER gone).
#define QM_PIPE_TO_PLUGIN   "VitaPlexQM11::ToPlugin"
#define QM_PIPE_FROM_PLUGIN "VitaPlexQM11::FromPlugin"

// SceShell carries the queue on. The system closes VitaPlex BG to start a
// game, and SceShell is never ended. So VitaPlex BG hands its queue to
// SceShell as it goes (QM_CMD_PLAY_QUEUE), and the plugin plays it from
// inside SceShell, beginning at the queue's song and place in it:
//
// - That song plays from the file VitaPlex BG made ready, else its download,
//   else it is fetched; then the queue's next song: its download if the shell
//   plays its format, else the song's file fetched from VitaPlex's server
//   into the folder of songs (bgplayer.h's BGPLAYER_SONGS), as the server's
//   MP3 of it with a length header in front when the shell does not play
//   its format. The next song is fetched while the one before plays; one
//   that has not come by the time it is wanted is waited for, the shell
//   stopped meanwhile. Songs the server has no file of are passed over, as
//   are all those not on the card when VitaPlex's settings name no server.
// - The row is SceShell's: the song, its cover (QM_SONG_COVER_FMT, made by
//   VitaPlex BG for its song, fetched for the rest), and its buttons, which
//   act on SceShell's music.
// - A song played to its end is marked played on the server.
// - The status (bgplayer.h) is written with "by=SceShell" in it, so that
//   VitaPlex BG takes the music back where SceShell has got to; with
//   "fetching=1" while it waits for a song, which VitaPlex BG then plays from
//   where the status says. It asks first (QM_CMD_TAKE_BACK).
// - It stops at the queue's end, when no further song is on the card or the
//   server, when three songs in a row will not come, when the shell plays
//   something else, and when any process takes the row.
//
// Up to version 10 it also carried on the queue of VitaPlex BG's background
// player, which the system ended as an app with extended memory opened.

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
    QM_CMD_LOOK,        // arg: QM_LOOK_*, how the row's buttons are switched on
    // arg: the song's cover by its serial, written to QM_ART_PATH_FMT before
    // it is sent; 0 for none, the heading's own icon. A cover is its
    // sender's: the plugin forgets it when the row is given back or another
    // owner takes it.
    QM_CMD_ART,
    // A test: the plugin plays the file at the path in title (UTF-16)
    // through SceShell's music service itself, from inside SceShell, for
    // QM_MUSIC_TEST_S at most, and logs each step to qm-plugin.log, each
    // line with "music test:" in it and the last with "music test: done".
    // Whether this works decides whether the background music can move into
    // SceShell, where no game ends it (docs/DESIGN_NOTES.md).
    QM_CMD_MUSIC_TEST,
    // From VitaPlex BG, once ready to go on with the music it handed over: it
    // takes the music back. A queue SceShell carries on stops where it is,
    // with the song left playing for VitaPlex BG to stop, and the status
    // written one last time; a queue handed over and not yet begun is not
    // begun.
    QM_CMD_TAKE_BACK,
    // A test: the plugin fetches from the server through Sony's HTTP library,
    // as SceShell has it, from inside SceShell. title has the server's
    // address ("https://host:port"), artist the path of a song's file on it,
    // album the token, which goes as a header and is never logged; arg how
    // many bytes from the file's start to fetch. It asks for /identity with
    // no token, then for the song's start with it, and logs each step to
    // qm-plugin.log, each line with "http test:" in it and the last with
    // "http test: done". Whether this works decides whether SceShell can
    // fetch the queue's songs that are not on the memory card.
    QM_CMD_HTTP_TEST,
    // From VitaPlex BG as it goes: SceShell plays the queue at BGPLAYER_QUEUE
    // (bgplayer.h) itself, from its song and place there (index=, positionMs=,
    // and file= when the app made that song ready as a file), and takes the
    // row for it. Sent once the row has been given back, and the queue
    // written whole.
    QM_CMD_PLAY_QUEUE
};
enum { QM_OWNER_APP = 1 };

// A song's cover for SceShell to show as it carries the queue on: a PNG as
// QM_ART_PATH_FMT's, in the folder of songs (bgplayer.h's BGPLAYER_SONGS)
// with the song's ratingKey in its name. VitaPlex BG puts its song's there as
// it hands the queue over; SceShell fetches the others.
#define QM_SONG_COVER_FMT "ux0:data/VitaPlex/bgplayer/%s.png"
// The row's buttons stay dimmed with ElevenMPV-A's way, ButtonBase::Enable(false),
// though presses get through (screenshot, build 2019), and with
// Enable(true) as well (build 2022), the buttons then in their normal state:
// their icons are planes of their own, and SceShell fades buttons and icons
// where they are drawn (build 2023). Which look lights them is for the
// console to say.
enum {
    QM_LOOK_ENABLE = 0,         // ButtonBase::Enable(true)
    QM_LOOK_ENABLE_NORMAL,      // that, then ChangeButtonState(normal)
    QM_LOOK_AS_ELEVENMPVA,      // ButtonBase::Enable(false)
    QM_LOOK_LIT,                // Enable(true) and normal, buttons and icons lit: the default
    QM_LOOK_COUNT
};
#define QM_ACTIVE_TIMEOUT_S 5
#define QM_MUSIC_TEST_S     30
#define QM_HTTP_TEST_S      60     // the longest a fetch test may take
#define QM_HTTP_TEST_BYTES  65536  // what it fetches of the song

// The plugin's log, one file a boot; VitaPlex BG reads a test's lines back
// from it.
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
