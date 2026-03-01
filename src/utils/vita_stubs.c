/**
 * Vita stubs for missing functions
 * These are required by various libraries but not available on Vita
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

/*
 * pthread_create wrapper: enforce minimum 512KB stack for all threads.
 *
 * Vita's default pthread stack is only 32KB, which is far too small for
 * software H.264 decoding in libavcodec. With hwdec=no, MPV's decoder
 * threads overflow the 32KB stack (crash dump shows SP 2720 bytes below
 * stack base), crashing the app.
 *
 * We use the linker's --wrap feature to intercept all pthread_create calls
 * (including from statically-linked MPV/ffmpeg) and ensure a minimum stack
 * size of 512KB.
 */
#define VITAPLEX_MIN_THREAD_STACK (512 * 1024)

extern int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    pthread_attr_t patched_attr;
    const pthread_attr_t *use_attr = attr;

    if (attr == NULL) {
        /* No attributes given - create one with our minimum stack size */
        pthread_attr_init(&patched_attr);
        pthread_attr_setstacksize(&patched_attr, VITAPLEX_MIN_THREAD_STACK);
        use_attr = &patched_attr;
    } else {
        /* Attributes given - bump stack size if below minimum */
        size_t cur = 0;
        pthread_attr_getstacksize(attr, &cur);
        if (cur < VITAPLEX_MIN_THREAD_STACK) {
            patched_attr = *attr;
            pthread_attr_setstacksize(&patched_attr, VITAPLEX_MIN_THREAD_STACK);
            use_attr = &patched_attr;
        }
    }

    return __real_pthread_create(thread, use_attr, start_routine, arg);
}

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

/* NI flags for getnameinfo (if not defined in headers) */
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST  0x0001
#endif
#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV  0x0002
#endif

/* EAI error codes (if not defined) */
#ifndef EAI_FAMILY
#define EAI_FAMILY      5
#endif
#ifndef EAI_OVERFLOW
#define EAI_OVERFLOW    14
#endif

/**
 * getnameinfo - Convert socket address to hostname/service strings
 *
 * FFmpeg uses this for debug logging of addresses. We provide a minimal
 * implementation that returns numeric IP/port only (no DNS reverse lookup).
 */
int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)salen;  /* We only support AF_INET anyway */
    (void)flags;  /* Always return numeric format on Vita */

    /* Only support IPv4 */
    if (!sa || sa->sa_family != AF_INET) {
        return EAI_FAMILY;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    /* Get host (IP address as string) */
    if (host && hostlen > 0) {
        unsigned char *ip = (unsigned char *)&sin->sin_addr.s_addr;
        int ret = snprintf(host, hostlen, "%u.%u.%u.%u",
                          ip[0], ip[1], ip[2], ip[3]);
        if (ret < 0 || (size_t)ret >= hostlen) {
            return EAI_OVERFLOW;
        }
    }

    /* Get service (port as string) */
    if (serv && servlen > 0) {
        int port = ntohs(sin->sin_port);
        int ret = snprintf(serv, servlen, "%d", port);
        if (ret < 0 || (size_t)ret >= servlen) {
            return EAI_OVERFLOW;
        }
    }

    return 0;
}
