/**
 * What main.c shares with the background test's player variants
 * (player_test.c, VPLXBGT10 and VPLXBGT11).
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

#ifdef BGTEST_PLAYER
// Starts the player thread: with handoff 0 it picks two songs from the
// library (VPLXBGT10), with 1 it plays the ones VitaPlex handed it
// (VPLXBGT11).
void playerStart(int handoff);
// Stops the system player and gives the BGM port back, on the way out (why
// says which, for the log), unless a song is with the shell.
void playerQuit(const char* why);
#endif

#endif
