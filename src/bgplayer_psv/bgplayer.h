/**
 * VitaPlex BG's background player: what the app and its player program say
 * to each other.
 *
 * The player is the app's background program, eboot2.bin in VitaPlexBG.vpk
 * (src/bgplayer_psv), which the app starts with sceBgAppUtilStartBgApp as it
 * leaves. It downloads the queue's songs to the memory card and hands them one
 * after another to the shell's music player, which plays on beside games where
 * VitaPlex cannot run (docs/DESIGN_NOTES.md, BG Tests 10 to 21). The two talk
 * through files in the app's data directory, which both can reach. Each is
 * written whole beside its place and then moved in, so that neither reads half
 * of one:
 *
 *   queue    the app's queue in the order it plays, the song it had got to and
 *            where in it, and the repeat mode. Written by the app as it hands
 *            the music over; read by the player as it starts or is asked to
 *            play again, and by the app as it takes the music back.
 *   status   where the player has got to: the song, its place in the queue and
 *            in the song, and whether it plays. Written by the player every
 *            few seconds and at every change; read by the app as it opens.
 *   pause    the app, opened again, asking the player to stop the music and
 *            stay. The app plays on from where it stopped.
 *   play     the app, leaving again, asking a paused player for the music
 *            again: the queue it has just written, once the app has gone.
 *
 * A request is one line, "<written> <why>", and the player takes it by
 * removing it; one written before the player started was for another one.
 *
 * By the setting, VitaPlex BG can hand the queue to SceShell itself instead,
 * through its quick menu plugin (qm_protocol.h's QM_CMD_PLAY_QUEUE), with no
 * player: the same queue file, and the same status, written "by=SceShell".
 *
 * The player never ends while its app is open: the two share app0:, and the
 * app lost it each time its program ended under it (BG Tests 20 and 21, build
 * 2012). Nor does it do anything while its app is going: one with extended
 * memory, as VitaPlex BG has, took 10 s to go beside a busy player and the
 * console crashed; beside an idle one it took 0.2 s.
 *
 * Plain C, for the player, and included by the app's C++ as well.
 */

#ifndef VITAPLEX_BGPLAYER_H
#define VITAPLEX_BGPLAYER_H

// The player's title id, from its sce_sys/param2.sfo, which the system lists
// it by, and its app's (CMakeLists.txt).
#if !defined(BGPLAYER_TITLEID) || !defined(BGPLAYER_APP_TITLEID)
#error "CMakeLists.txt defines BGPLAYER_TITLEID and BGPLAYER_APP_TITLEID"
#endif

#define BGPLAYER_DIR       "ux0:data/VitaPlex/"
#define BGPLAYER_QUEUE     BGPLAYER_DIR "bgplayer-queue.txt"
#define BGPLAYER_STATUS    BGPLAYER_DIR "bgplayer-status.txt"
#define BGPLAYER_ASK_PAUSE BGPLAYER_DIR "bgplayer-pause.txt"
#define BGPLAYER_ASK_PLAY  BGPLAYER_DIR "bgplayer-play.txt"
// Mozilla's CA bundle, copied from the app's resources as it hands over: the
// background test's player lost app0: when its app went after it had started
// (log 5d35ef15), and every https request with it.
#define BGPLAYER_CA_BUNDLE BGPLAYER_DIR "bgplayer-cacert.pem"
// The songs handed to the shell: the one playing and the next. A file is
// written beside its name, with ".part" on the end, and moved in once whole.
#define BGPLAYER_SONGS     BGPLAYER_DIR "bgplayer"
#define BGPLAYER_LOG       BGPLAYER_DIR "bgplayer.log"

// The queue, one "key=value" a line:
//
//   written=<the system clock in seconds, %.6f>
//   index=<the song to start with, its place in the list below from 0>
//   positionMs=<where in it>
//   repeat=<0 off, 1 the one song, 2 all>
//   file=<that song as a file the app made ready for the shell, or nothing>
//   port=<how the player takes the BGM port: one of the four below; the
//         first when the line is missing>
//   qm=<1: the song and its buttons in the quick menu's Music row, through
//       VitaPlex's plugin (qm_link.h); 0 or missing, not>
//   track=<fields, below>        one a song, in the order they play
//   end
//
// The system clock (sceKernelGetSystemTimeWide) runs on from one process to
// the next, so a time in a file can be compared with another process's.
// A queue with no "end" line is not whole, and is not read.
#define BGPLAYER_PORT_PRIORITY    "0x80"         // at 0x80, ElevenMPV-A's priority while
                                                 // SceShell decodes
#define BGPLAYER_PORT_PLAIN       "plain"        // sceAppMgrAcquireBgmPort, no priority
#define BGPLAYER_PORT_MUSICPLAYER "musicplayer"  // sceAppMgrAcquireBgmPortForMusicPlayer
#define BGPLAYER_PORT_NONE        "none"         // not taken
#define BGPLAYER_REPEAT_OFF 0
#define BGPLAYER_REPEAT_ONE 1
#define BGPLAYER_REPEAT_ALL 2
// The most songs a queue holds; the app writes no more than these, from the
// one it had got to on.
#define BGPLAYER_MAX_TRACKS 1000

// A track line's fields, separated by '|', which the app leaves out of them.
// The player reads all but the rating; the app all of them, to set its queue
// up again without asking the server. A line with fewer, from an older app,
// has the rest empty.
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
//   state=<one of the four below>
//   wasPlaying=<for paused: 1 if music was playing as it was asked to pause>
//   by=<SceShell when VitaPlex's quick menu plugin wrote it, carrying the
//       queue on after the system ended the player (qm_protocol.h); missing
//       when the player did>
//   fetching=<1 when SceShell has stopped the shell's music to fetch the song
//             it names, which plays from positionMs once fetched; missing
//             otherwise>
//   end
#define BGPLAYER_STATE_PLAYING "playing"   // handed to the shell
#define BGPLAYER_STATE_PAUSED  "paused"    // stopped for its app, and staying
#define BGPLAYER_STATE_OVER    "over"      // the queue has played out
#define BGPLAYER_STATE_STOPPED "stopped"   // ended without the queue being over

#endif
