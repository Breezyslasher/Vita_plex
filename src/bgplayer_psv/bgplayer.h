/**
 * VitaPlex BG handing its queue to SceShell: what the app and VitaPlex's
 * quick menu plugin, inside SceShell, say to each other through files.
 *
 * The system closes VitaPlex BG to start a game. SceShell, which no app or
 * game closes, carries the queue on: the plugin plays it through the shell's
 * music player, which plays on beside games where VitaPlex cannot run
 * (docs/DESIGN_NOTES.md, BG Tests 10 to 21, builds 2032 to 2046), and fetches
 * the songs that are not on the memory card as they come up. The app tells
 * the plugin when through its pipes (qm_protocol.h); the rest goes through
 * files in the app's data directory, which both can reach. Each is written
 * whole beside its place and then moved in, so that neither reads half of
 * one:
 *
 *   queue    the app's queue in the order it plays, the song it had got to and
 *            where in it, and the repeat mode. Written by the app as it hands
 *            the music over (QM_CMD_PLAY_QUEUE); read by the plugin as it
 *            begins, and by the app as it takes the music back.
 *   status   where SceShell has got to: the song, its place in the queue and
 *            in the song, and whether it plays. Written by the plugin every
 *            few seconds and at every change; read by the app as it opens.
 *
 * The names are those of the background player VitaPlex BG had up to build
 * 2045, a program of its own in the package that played the queue the same
 * way, and wrote the same files, until the system ended it as an app with
 * extended memory opened.
 *
 * Plain C, for the plugin's sake, and included by the app's C++ as well.
 */

#ifndef VITAPLEX_BGPLAYER_H
#define VITAPLEX_BGPLAYER_H

#define BGPLAYER_DIR       "ux0:data/VitaPlex/"
#define BGPLAYER_QUEUE     BGPLAYER_DIR "bgplayer-queue.txt"
#define BGPLAYER_STATUS    BGPLAYER_DIR "bgplayer-status.txt"
// The songs handed to the shell: the one playing and the next. A file is
// written beside its name, with ".part" on the end, and moved in once whole.
#define BGPLAYER_SONGS     BGPLAYER_DIR "bgplayer"

// The queue, one "key=value" a line:
//
//   written=<the system clock in seconds, %.6f>
//   index=<the song to start with, its place in the list below from 0>
//   positionMs=<where in it>
//   repeat=<0 off, 1 the one song, 2 all>
//   file=<that song as a file the app made ready for the shell, or nothing>
//   track=<fields, below>        one a song, in the order they play
//   end
//
// The system clock (sceKernelGetSystemTimeWide) runs on from one process to
// the next, so a time in a file can be compared with another process's.
// A queue with no "end" line is not whole, and is not read. Lines it does
// not know of are passed over: queues written up to build 2045 had port=
// and qm= for the background player.
#define BGPLAYER_REPEAT_OFF 0
#define BGPLAYER_REPEAT_ONE 1
#define BGPLAYER_REPEAT_ALL 2
// The most songs a queue holds; the app writes no more than these, from the
// one it had got to on.
#define BGPLAYER_MAX_TRACKS 1000

// A track line's fields, separated by '|', which the app leaves out of them.
// The plugin reads all but the rating and the cover; the app all of them, to
// set its queue up again without asking the server. A line with fewer, from
// an older app, has the rest empty.
enum {
    BGPLAYER_TRACK_KEY,       // the ratingKey
    BGPLAYER_TRACK_DURATION,  // ms, as the server gives it
    BGPLAYER_TRACK_LOCAL,     // a download of it on the memory card, or nothing
    BGPLAYER_TRACK_RATING,    // the user's rating, 0 to 10
    BGPLAYER_TRACK_THUMB,     // its cover on the server (a path), or nothing
    BGPLAYER_TRACK_TITLE,
    BGPLAYER_TRACK_ARTIST,
    BGPLAYER_TRACK_ALBUM,
    BGPLAYER_TRACK_COVER,     // the download's copy of its cover, or nothing
    BGPLAYER_TRACK_FIELDS
};

// The status, the same way:
//
//   written=<the system clock>
//   queue=<the queue's "written", the one it plays>
//   index=<the song's place in that queue>
//   ratingKey=<the song>
//   positionMs=<where in it>
//   durationMs=<its length, as the server gives it>
//   state=<one of the three below>
//   by=<SceShell: VitaPlex's quick menu plugin wrote it; missing when the
//       background player of builds up to 2045 did>
//   fetching=<1 when SceShell has stopped the shell's music to fetch the song
//             it names, which plays from positionMs once fetched; missing
//             otherwise>
//   end
#define BGPLAYER_STATE_PLAYING "playing"   // handed to the shell
#define BGPLAYER_STATE_OVER    "over"      // the queue has played out
#define BGPLAYER_STATE_STOPPED "stopped"   // ended without the queue being over

#endif
