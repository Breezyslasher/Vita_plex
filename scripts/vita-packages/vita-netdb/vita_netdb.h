/**
 * vita_netdb.h - POSIX getaddrinfo() implementation for PS Vita
 *
 * This provides getaddrinfo/freeaddrinfo/gai_strerror using Vita's
 * native sceNetResolver APIs, allowing FFmpeg to resolve hostnames.
 */

#ifndef VITA_NETDB_H
#define VITA_NETDB_H

#include <sys/types.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Address info structure (POSIX compatible) */
struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

/* AI flags */
#define AI_PASSIVE      0x0001
#define AI_CANONNAME    0x0002
#define AI_NUMERICHOST  0x0004
#define AI_NUMERICSERV  0x0008
#define AI_V4MAPPED     0x0010
#define AI_ALL          0x0020
#define AI_ADDRCONFIG   0x0040

/* Error codes */
#define EAI_AGAIN       2
#define EAI_BADFLAGS    3
#define EAI_FAIL        4
#define EAI_FAMILY      5
#define EAI_MEMORY      6
#define EAI_NONAME      8
#define EAI_SERVICE     9
#define EAI_SOCKTYPE    10
#define EAI_SYSTEM      11
#define EAI_OVERFLOW    14

/**
 * Resolve hostname to address
 *
 * @param node     Hostname or IP address string
 * @param service  Service name or port number (can be NULL)
 * @param hints    Hints for resolution (can be NULL)
 * @param res      Output: linked list of results
 * @return 0 on success, EAI_* error code on failure
 */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);

/**
 * Free addrinfo linked list
 */
void freeaddrinfo(struct addrinfo *res);

/**
 * Get error string for EAI_* error code
 */
const char *gai_strerror(int errcode);

#ifdef __cplusplus
}
#endif

#endif /* VITA_NETDB_H */
