/**
 * What main.c shares with the background test's player variants
 * (player_test.c, VPLXBGT10 and VPLXBGT11), and with the background program
 * of VPLXBGT14 to 21 and its starter (bgapp.c, bgstart.c).
 */

#ifndef BGTEST_H
#define BGTEST_H

#include <stddef.h>

// A line in the test's log, with the seconds since start in front.
void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// A call's result as a number, or as the error code it is when negative.
const char* result(char* buf, size_t size, int value);

extern volatile int g_away;      // deactivated and not yet activated
extern volatile int g_netReady;  // SceNet is up (netMain)
// The app's title id: for the background program, the app that started it.
const char* bgtestTitle(void);

// VPLXBGT14 to 21 (bgapp.c). The app asks for its background program,
// eboot2.bin, and leaves it a note with its title id and its start (t0, on the
// system clock); 0 or more when the system took the request.
int bgappStart(const char* title, double t0);
// The note, read by the background program as it starts and removed. 1 when
// it is fresh, with the app's title id and start, and how long ago it asked.
int bgappTakeNote(char* title, size_t size, double* t0, double* since);
// 1 while a note is waiting to be read: an app has just asked for its
// background program, which takes about a second to start and read it.
int bgappNoteWaiting(void);
// The app asking its running background program to stop, and why, in a note
// of its own; and taking the request back if no program has read it.
void bgappAskQuit(const char* why);
void bgappCancelQuit(void);
// 1 while that request is waiting to be read.
int bgappQuitWaiting(void);
// The request, read by the background program and removed: 1 with why, or -1
// when it was made before the program started (at startedAt, on the system
// clock), so was for another one.
int bgappTakeQuit(double startedAt, char* why, size_t size);

#ifdef BGTEST_PLAYER
// How the player starts: with two songs from the library, 15 s after the app
// is sent away (VPLXBGT10, and the player of 12 and 13); with the ones VitaPlex
// handed it, at once (VPLXBGT11); or with two from the library at once, as its
// app's background program, which is never in front (14 to 21).
enum { PLAYER_AWAY, PLAYER_HANDOFF, PLAYER_BACKGROUND };
// Starts the player thread.
void playerStart(int mode);
// Stops the system player and gives the BGM port back, on the way out (why
// says which, for the log), unless a song is with the shell.
void playerQuit(const char* why);
// The same whatever is playing, and for good: the background program, asked
// by its app or the system to go, takes the music with it.
void playerStop(const char* why);
// The power callback's argument (main.c): the power button stops the shell's
// music, and this plays it again, as ElevenMPV-A does.
void playerPowerEvent(int powerInfo);
#endif

#endif
