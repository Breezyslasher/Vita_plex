/**
 * Vita stubs for missing POSIX functions
 * These are required by fmt library but not available on Vita
 */

#include <stdio.h>

/* Thread-safe stdio locking stubs - Vita is single-threaded for stdio anyway */
void flockfile(FILE *filehandle) {
    (void)filehandle;
    /* No-op on Vita */
}

void funlockfile(FILE *filehandle) {
    (void)filehandle;
    /* No-op on Vita */
}
