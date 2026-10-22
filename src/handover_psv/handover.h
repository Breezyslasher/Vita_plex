/**
 * VitaPlex handing its queue to SceShell: what the app and VitaPlex's quick
 * menu plugin, inside SceShell, say to each other through files.
 *
 * The system closes VitaPlex to start a game. SceShell, which no app or game
 * closes, carries the queue on: the plugin plays it through the shell's
 * music player, which plays on beside games where VitaPlex cannot run
 * (docs/DESIGN_NOTES.md, BG Tests 10 to 21, builds 2032 to 2047), and fetches
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
 * Up to build 2047 they were named after the background player VitaPlex had
 * up to build 2045 (bgplayer-queue.txt, bgplayer-status.txt and the folder
 * bgplayer), a program of its own in the package that played the queue the
 * same way until the system ended it as an app with extended memory opened.
 * VitaPlex removes what is left under those names (HANDOVER_OLD_*).
 *
 * Plain C, for the plugin's sake, and included by the app's C++ as well.
 */

#ifndef VITAPLEX_HANDOVER_H
#define VITAPLEX_HANDOVER_H

#define HANDOVER_DIR       "ux0:data/VitaPlex/"
#define HANDOVER_QUEUE     HANDOVER_DIR "handover-queue.txt"
#define HANDOVER_STATUS    HANDOVER_DIR "handover-status.txt"
// The songs handed to the shell, named "<ratingKey>.<extension>", with their
// covers: the song before, for previous, the one playing, and the next,
// whichever of the two plays them; VitaPlex plays them too, once it has the
// music back. The server's MP3 of a song the shell does not play is named
// ".mp3", as the plugin and VitaPlex both fetch it. A file is written beside
// its name, with ".part" on the end, and moved in once whole; VitaPlex
// writes the song it makes ready for a hand-over with ".ready" on the end
// instead, so that it never shares a file with the plugin's fetch of the
// same song, and the plugin a cover it copies with ".copy".
#define HANDOVER_SONGS     HANDOVER_DIR "handover"
// The same up to build 2047, and the background player's logs before that.
#define HANDOVER_OLD_QUEUE  HANDOVER_DIR "bgplayer-queue.txt"
#define HANDOVER_OLD_STATUS HANDOVER_DIR "bgplayer-status.txt"
#define HANDOVER_OLD_SONGS  HANDOVER_DIR "bgplayer"
#define HANDOVER_OLD_LOG    HANDOVER_DIR "bgplayer.log"
#define HANDOVER_OLD_LOG2   HANDOVER_DIR "bgplayer.prev.log"

// The queue, one "key=value" a line:
//
//   written=<the system clock in seconds, %.6f>
//   index=<the song to start with, its place in the list below from 0>
//   positionMs=<where in it>
//   repeat=<0 off, 1 the one song, 2 all>
//   file=<that song as a file the app made ready for the shell, or nothing>
//   offlineSkip=<1: a song the server does not answer for is passed over for
//                the next song on the memory card, and with none, held
//                paused (the app's setting, build 2054); 0 or missing: songs
//                that cannot be fetched are passed over, and three in a row
//                stop the queue>
//   track=<fields, below>        one a song, in the order they play
//   end
//
// The system clock (sceKernelGetSystemTimeWide) runs on from one process to
// the next, so a time in a file can be compared with another process's.
// A queue with no "end" line is not whole, and is not read. Lines it does
// not know of are passed over: queues written up to build 2045 had port=
// and qm= for the background player.
#define HANDOVER_REPEAT_OFF 0
#define HANDOVER_REPEAT_ONE 1
#define HANDOVER_REPEAT_ALL 2
// The most songs a queue holds; the app writes no more than these, from the
// one it had got to on.
#define HANDOVER_MAX_TRACKS 1000

// A track line's fields, separated by '|', which the app leaves out of them.
// The plugin reads all but the rating; the app all but the cover, to set its
// queue up again without asking the server. A line with fewer, from an older
// app, has the rest empty.
enum {
    HANDOVER_TRACK_KEY,       // the ratingKey
    HANDOVER_TRACK_DURATION,  // ms, as the server gives it
    HANDOVER_TRACK_LOCAL,     // a download of it on the memory card, or nothing
    HANDOVER_TRACK_RATING,    // the user's rating, 0 to 10
    HANDOVER_TRACK_THUMB,     // its cover on the server (a path), or nothing
    HANDOVER_TRACK_TITLE,
    HANDOVER_TRACK_ARTIST,
    HANDOVER_TRACK_ALBUM,
    HANDOVER_TRACK_COVER,     // the download's cover as a PNG of QM_ART_SIZE, or nothing
    HANDOVER_TRACK_FIELDS
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
//             it names, which plays from positionMs once fetched, or holds it
//             paused as the server does not answer (offlineSkip); missing
//             otherwise>
//   paused=<1 while the quick menu's play/pause holds the music, which
//           VitaPlex then takes back paused (build 2052); missing otherwise>
//   end
#define HANDOVER_STATE_PLAYING "playing"   // handed to the shell
#define HANDOVER_STATE_OVER    "over"      // the queue has played out
#define HANDOVER_STATE_STOPPED "stopped"   // ended without the queue being over

#endif
