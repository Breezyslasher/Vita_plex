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
// SceShell, for a test (QM_CMD_HTTP_TEST).
#define QM_PIPE_TO_PLUGIN   "VitaPlexQM8::ToPlugin"
#define QM_PIPE_FROM_PLUGIN "VitaPlexQM8::FromPlugin"

// SceShell carries the queue on. The system ends the background player as an
// app with extended memory starts, and SceShell is never ended. So when the
// player stops sending QM_CMD_ACTIVATE without giving the row back, the
// plugin plays the rest of its queue itself, from inside SceShell:
//
// - The song playing, from the player's status (bgplayer.h), plays on to its
//   end. Then the queue's next song on the memory card plays: its download
//   if the shell plays its format, else the file the player had ready for it
//   (QM_CMD_SONG_FILE). Songs that are on neither are passed over.
// - The row stays: the song, its cover if the player made one
//   (QM_SONG_COVER_FMT), and its buttons, which now act on SceShell's music.
// - The player's status file is written as the player writes it, with
//   "by=SceShell" in it, so that VitaPlex BG takes the music back where
//   SceShell has got to. It asks first (QM_CMD_TAKE_BACK).
// - It stops at the queue's end, when no further song is on the card, when
//   the shell plays something else, and when any process takes the row.

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
    // From the background player: a song of its queue that is on the memory
    // card, as a QmSongFile. Sent for the song with the shell and for the next
    // one once it is ready, so that SceShell can carry the queue on.
    QM_CMD_SONG_FILE,
    // From VitaPlex BG, as it opens: it takes the music back. A queue SceShell
    // carries on stops where it is, with the song left playing for VitaPlex BG
    // to stop, and the status written one last time. None is carried on after
    // this until the background player takes the row again.
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
    QM_CMD_HTTP_TEST
};
enum { QM_OWNER_APP = 1, QM_OWNER_PLAYER = 2 };

// QM_CMD_SONG_FILE's message, sent in a QmMessage's place.
typedef struct QmSongFile {
    uint32_t cmd;    // QM_CMD_SONG_FILE
    uint32_t arg;    // the song's place in the queue, from 0
    char key[16];    // its ratingKey
    char path[128];  // the file the shell is given
} QmSongFile;

// A song's cover for SceShell to show as it carries the queue on: a PNG as
// QM_ART_PATH_FMT's, made by the background player for the next song once it
// is ready, in the player's folder of songs (bgplayer.h's BGPLAYER_SONGS) with
// the song's ratingKey in its name.
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
