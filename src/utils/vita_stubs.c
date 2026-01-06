/**
 * Vita stubs for missing functions
 * These are required by various libraries but not available on Vita
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

/* SDL2 stub - DesktopPlatform uses this but we use PsvPlatform::openBrowser instead */
int SDL_OpenURL(const char *url) {
    (void)url;
    /* PSV uses sceAppUtilLaunchWebBrowser via PsvPlatform::openBrowser() */
    return -1;  /* Return error - actual implementation is in PsvPlatform */
}
