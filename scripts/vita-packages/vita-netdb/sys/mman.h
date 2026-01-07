/**
 * sys/mman.h - Memory mapping stubs for PS Vita
 *
 * Vita doesn't support mmap/munmap, so we provide stub definitions
 * that allow compilation but return errors at runtime.
 */

#ifndef _SYS_MMAN_H_
#define _SYS_MMAN_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protection flags */
#define PROT_NONE       0x00
#define PROT_READ       0x01
#define PROT_WRITE      0x02
#define PROT_EXEC       0x04

/* Map flags */
#define MAP_SHARED      0x0001
#define MAP_PRIVATE     0x0002
#define MAP_FIXED       0x0010
#define MAP_ANONYMOUS   0x0020
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_FILE        0x0000

/* Return value for failed mmap */
#define MAP_FAILED      ((void *)-1)

/* msync flags */
#define MS_ASYNC        0x01
#define MS_SYNC         0x02
#define MS_INVALIDATE   0x04

/* Stub implementations - these will fail at runtime if actually called */
static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, off_t offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    return MAP_FAILED;
}

static inline int munmap(void *addr, size_t length) {
    (void)addr; (void)length;
    return -1;
}

static inline int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return -1;
}

static inline int msync(void *addr, size_t length, int flags) {
    (void)addr; (void)length; (void)flags;
    return -1;
}

static inline int mlock(const void *addr, size_t len) {
    (void)addr; (void)len;
    return -1;
}

static inline int munlock(const void *addr, size_t len) {
    (void)addr; (void)len;
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H_ */
