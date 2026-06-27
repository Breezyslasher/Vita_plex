/**
 * VitaPlex Switch stubs.
 *
 * pthread_create wrapper: enforce a minimum 1 MB stack for every thread.
 *
 * libnx hardcodes a 128 KB default pthread stack (libnx newlib.c:
 * "if (!stack_size) stack_size = 128*1024;"), and it is NOT a configurable
 * weak symbol. libmpv / ffmpeg create their player, demuxer and decoder
 * threads with a NULL attr, so they inherit that 128 KB and overflow during
 * mpv_initialize / decoding — the deterministic Instruction Abort on a
 * 0x21000-byte stack region seen the instant a video or track starts on
 * Switch (Atmosphère report: garbage registers + return into unmapped code,
 * the classic stack-overflow signature).
 *
 * The Vita build already fixes the same class of bug (src/utils/vita_stubs.c,
 * 512 KB floor for Vita's tiny 32 KB default). Switch needs the same wrap with
 * a larger floor since it decodes higher-resolution video. Enabled by
 * -Wl,--wrap=pthread_create in the Switch link options.
 */

#include <pthread.h>

#define VITAPLEX_SWITCH_MIN_THREAD_STACK (1024 * 1024)

extern int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    pthread_attr_t patched_attr;
    const pthread_attr_t *use_attr = attr;

    if (attr == NULL) {
        /* No attributes given — create one with our minimum stack size. */
        pthread_attr_init(&patched_attr);
        pthread_attr_setstacksize(&patched_attr, VITAPLEX_SWITCH_MIN_THREAD_STACK);
        use_attr = &patched_attr;
    } else {
        /* Attributes given — bump the stack size only if below our floor. */
        size_t cur = 0;
        pthread_attr_getstacksize(attr, &cur);
        if (cur < VITAPLEX_SWITCH_MIN_THREAD_STACK) {
            patched_attr = *attr;
            pthread_attr_setstacksize(&patched_attr, VITAPLEX_SWITCH_MIN_THREAD_STACK);
            use_attr = &patched_attr;
        }
    }

    return __real_pthread_create(thread, use_attr, start_routine, arg);
}
