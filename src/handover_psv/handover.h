/**
 * VitaPlex handing its queue to SceShell: the files on VitaPlex's side.
 *
 * The system closes VitaPlex to start a game. SceShell, which no app or game
 * closes, carries the queue on: vita-shell-companion's plugin plays it
 * through the shell's music player, which plays on beside games where
 * VitaPlex cannot run (docs/DESIGN_NOTES.md, BG Tests 10 to 21, builds 2032
 * to 2047), and fetches the songs that are not on the memory card as they
 * come up. What the plugin reads and writes is its protocol's
 * (lib/vita-shell-companion/docs/PROTOCOL.md, §4), in VitaPlex's data
 * folder: the queue (SC_QUEUE), its status (SC_QUEUE_STATUS) and the folder
 * of songs (SC_SONGS).
 *
 * VitaPlex also keeps its own copy of the queue, HANDOVER_QUEUE, written as
 * it hands the music over, beside the plugin's and with the same written=:
 * what it needs to set its queue up again as it takes the music back, without
 * asking the server (the rating, the cover's path on the server), which the
 * plugin's queue has no field for. The plugin never reads it.
 *
 * Up to the move to vita-shell-companion (build 2096) VitaPlex had a plugin of
 * its own, and the plugin read this queue itself, with its songs in
 * HANDOVER_DIR "handover" and the status in handover-status.txt; up to build
 * 2047 the same were named after the background player VitaPlex had up to
 * build 2045 (bgplayer-queue.txt, bgplayer-status.txt and the folder
 * bgplayer). VitaPlex removes what is left under those names
 * (HANDOVER_OLD_*).
 */

#ifndef VITAPLEX_HANDOVER_H
#define VITAPLEX_HANDOVER_H

#include "sc_protocol.h"

#define HANDOVER_DIR       "ux0:data/VitaPlex/"
#define HANDOVER_QUEUE     HANDOVER_DIR "handover-queue.txt"
#define HANDOVER_STATUS    HANDOVER_DIR SC_QUEUE_STATUS
// The songs handed to the shell, named "<ratingKey>.<extension>", with their
// covers ("<ratingKey>.png"): the song before, for previous, the one playing,
// and the next, whichever of the two plays them; VitaPlex plays them too, once
// it has the music back. The server's MP3 of a song the shell does not play is
// named ".mp3", as the plugin and VitaPlex both fetch it. The plugin writes a
// file beside its name with ".part" on the end and moves it in once whole;
// VitaPlex writes the song it makes ready for a hand-over with ".ready" on the
// end instead, so that it never shares a file with the plugin's fetch of the
// same song.
#define HANDOVER_SONGS     HANDOVER_DIR SC_SONGS
// VitaPlex's own plugin's names, up to build 2095.
#define HANDOVER_OLD17_SONGS  HANDOVER_DIR "handover"
#define HANDOVER_OLD17_STATUS HANDOVER_DIR "handover-status.txt"
// The background player's, up to build 2047, and its logs before that.
#define HANDOVER_OLD_QUEUE  HANDOVER_DIR "bgplayer-queue.txt"
#define HANDOVER_OLD_STATUS HANDOVER_DIR "bgplayer-status.txt"
#define HANDOVER_OLD_SONGS  HANDOVER_DIR "bgplayer"
#define HANDOVER_OLD_LOG    HANDOVER_DIR "bgplayer.log"
#define HANDOVER_OLD_LOG2   HANDOVER_DIR "bgplayer.prev.log"

// VitaPlex's queue, one "key=value" a line:
//
//   written=<the same as the plugin's queue's, which its status names>
//   index=<the song to start with, its place in the list below from 0>
//   positionMs=<where in it>
//   repeat=<SC_REPEAT_*>
//   track=<fields, below>        one a song, in the order they play
//   end
#define HANDOVER_REPEAT_OFF SC_REPEAT_OFF
#define HANDOVER_REPEAT_ONE SC_REPEAT_ONE
#define HANDOVER_REPEAT_ALL SC_REPEAT_ALL
#define HANDOVER_MAX_TRACKS SC_MAX_TRACKS

// A track line's fields, separated by '|', which VitaPlex leaves out of them.
enum {
    HANDOVER_TRACK_KEY,       // the ratingKey
    HANDOVER_TRACK_DURATION,  // ms, as the server gives it
    HANDOVER_TRACK_LOCAL,     // a download of it on the memory card, or nothing
    HANDOVER_TRACK_RATING,    // the user's rating, 0 to 10
    HANDOVER_TRACK_THUMB,     // its cover on the server (a path), or nothing
    HANDOVER_TRACK_TITLE,
    HANDOVER_TRACK_ARTIST,
    HANDOVER_TRACK_ALBUM,
    HANDOVER_TRACK_COVER,     // the download's cover as a PNG of SC_ART_SIZE, or nothing
    HANDOVER_TRACK_FIELDS
};

// The plugin's status (SC_QUEUE_STATUS) says state= one of SC_STATE_*.
#define HANDOVER_STATE_PLAYING SC_STATE_PLAYING
#define HANDOVER_STATE_OVER    SC_STATE_OVER
#define HANDOVER_STATE_STOPPED SC_STATE_STOPPED

#endif
