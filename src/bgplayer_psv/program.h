/**
 * What the player program's process (main.c) and its playback (player.c)
 * share. The protocol with the app is in bgplayer.h.
 */

#ifndef VITAPLEX_BGPLAYER_PROGRAM_H
#define VITAPLEX_BGPLAYER_PROGRAM_H

#include <stddef.h>

// A line in the player's log (BGPLAYER_LOG), with the time in front.
void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// A call's result as a number, or as the error code it is when negative.
const char* result(char* buf, size_t size, int value);
// The system clock in seconds, as the notes carry it (bgplayer.h).
double systemSecs(void);

extern volatile int g_netReady;   // SceNet is up (netMain)
// When this process started, on the system clock: a request written before
// it was for another one.
extern double g_startedAt;

// Whether the app is running, by the title id the system lists it by.
int appRunning(void);
// Seconds since PS was last held, which opens the quick menu; a large number
// if it has not been (main.c's power callback).
double quickMenuSince(void);
// Ends the process, through the system call under sceKernelExitProcess when
// that has not ended it in 3 s (main.c, quitNow).
void quitNow(void) __attribute__((noreturn));

// player.c: plays the queue the app handed over, on its own thread.
void playerStart(void);
// From the watcher. Its app, opened again, takes the music back: the music
// stops and the player stays (playerPause). Asked for it again, it plays the
// queue the app has written, once the app has gone (playerPlay).
void playerPause(const char* why);
void playerPlay(const char* why);
// For good, on the way out: the system asked the program to quit.
void playerStop(const char* why);
// The power callback's argument: the power button stops the shell's music,
// and this plays it again, as ElevenMPV-A does.
void playerPowerEvent(int powerInfo);
// From the watcher, four times a second: the quick menu's Music row through
// VitaPlex's plugin (or ElevenMPV-A's), while the queue says so and a song
// plays.
void playerQuickMenuTick(void);

// apps.c: which other apps run beside the player, and which of them have
// extended memory, logged as that changes, on a thread of its own.
void appsWatchStart(void);

#endif
