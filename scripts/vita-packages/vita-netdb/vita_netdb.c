/**
 * vita_netdb.c - POSIX getaddrinfo() implementation for PS Vita
 *
 * Uses Vita's native sceNetResolver APIs to resolve hostnames.
 * This allows FFmpeg and other libraries to use standard POSIX
 * hostname resolution on Vita.
 */

#include "vita_netdb.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

/* Resolver timeout and retry settings */
#define RESOLVER_TIMEOUT   5  /* seconds */
#define RESOLVER_RETRY     3

/* Static resolver ID (initialized once) */
static int s_resolver_id = -1;
static int s_net_initialized = 0;

/**
 * Initialize Vita network if needed
 */
static int vita_net_init(void) {
    if (s_net_initialized) {
        return 0;
    }

    /* Check if network is already initialized by the app */
    SceNetCtlInfo info;
    int ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
    if (ret >= 0) {
        s_net_initialized = 1;
        return 0;
    }

    /* Network not initialized - this shouldn't happen if app set it up */
    return -1;
}

/**
 * Create resolver if needed
 */
static int vita_resolver_create(void) {
    if (s_resolver_id >= 0) {
        return 0;
    }

    if (vita_net_init() < 0) {
        return -1;
    }

    s_resolver_id = sceNetResolverCreate("vita_netdb", NULL, 0);
    if (s_resolver_id < 0) {
        return -1;
    }

    return 0;
}

/**
 * Parse numeric IP address (e.g., "192.168.1.1")
 * Returns 0 on success, -1 if not a valid IP
 */
static int parse_numeric_ip(const char *node, struct in_addr *addr) {
    unsigned int a, b, c, d;

    if (sscanf(node, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return -1;
    }

    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return -1;
    }

    addr->s_addr = (a) | (b << 8) | (c << 16) | (d << 24);
    return 0;
}

/**
 * Parse port from service string
 */
static int parse_port(const char *service) {
    if (!service || !*service) {
        return 0;
    }

    /* Try numeric port */
    char *endptr;
    long port = strtol(service, &endptr, 10);
    if (*endptr == '\0' && port >= 0 && port <= 65535) {
        return (int)port;
    }

    /* Common service names */
    if (strcmp(service, "http") == 0) return 80;
    if (strcmp(service, "https") == 0) return 443;
    if (strcmp(service, "ftp") == 0) return 21;

    return 0;
}

/**
 * Allocate and populate addrinfo structure
 */
static struct addrinfo *alloc_addrinfo(struct in_addr *addr, int port,
                                        int socktype, int protocol) {
    struct addrinfo *ai = calloc(1, sizeof(struct addrinfo));
    if (!ai) return NULL;

    struct sockaddr_in *sa = calloc(1, sizeof(struct sockaddr_in));
    if (!sa) {
        free(ai);
        return NULL;
    }

    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    sa->sin_addr = *addr;

    ai->ai_flags = 0;
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_canonname = NULL;
    ai->ai_next = NULL;

    return ai;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    struct in_addr addr;
    int socktype = 0;
    int protocol = 0;
    int port;

    if (!res) {
        return EAI_FAIL;
    }
    *res = NULL;

    /* Parse hints */
    if (hints) {
        /* We only support IPv4 */
        if (hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) {
            return EAI_FAMILY;
        }
        socktype = hints->ai_socktype;
        protocol = hints->ai_protocol;
    }

    /* Handle NULL node (passive/wildcard) */
    if (!node) {
        if (hints && (hints->ai_flags & AI_PASSIVE)) {
            addr.s_addr = INADDR_ANY;
        } else {
            addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        port = parse_port(service);
        *res = alloc_addrinfo(&addr, port, socktype, protocol);
        return *res ? 0 : EAI_MEMORY;
    }

    /* Try parsing as numeric IP first */
    if (parse_numeric_ip(node, &addr) == 0) {
        port = parse_port(service);
        *res = alloc_addrinfo(&addr, port, socktype, protocol);
        return *res ? 0 : EAI_MEMORY;
    }

    /* Check for AI_NUMERICHOST flag */
    if (hints && (hints->ai_flags & AI_NUMERICHOST)) {
        return EAI_NONAME;
    }

    /* Need to resolve hostname using Vita's resolver */
    if (vita_resolver_create() < 0) {
        return EAI_FAIL;
    }

    /* Resolve hostname */
    SceNetInAddr vita_addr;
    int ret = sceNetResolverStartNtoa(s_resolver_id, node, &vita_addr,
                                       RESOLVER_TIMEOUT, RESOLVER_RETRY, 0);
    if (ret < 0) {
        /* Map Vita error to EAI error */
        switch (ret) {
            case SCE_NET_RESOLVER_ETIMEDOUT:
                return EAI_AGAIN;
            case SCE_NET_RESOLVER_ENOHOST:
                return EAI_NONAME;
            case SCE_NET_RESOLVER_ENODNS:
                return EAI_FAIL;
            default:
                return EAI_FAIL;
        }
    }

    /* Convert Vita address to standard format */
    addr.s_addr = vita_addr.s_addr;
    port = parse_port(service);

    *res = alloc_addrinfo(&addr, port, socktype, protocol);
    return *res ? 0 : EAI_MEMORY;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        if (res->ai_addr) {
            free(res->ai_addr);
        }
        if (res->ai_canonname) {
            free(res->ai_canonname);
        }
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
        case 0:             return "Success";
        case EAI_AGAIN:     return "Temporary failure in name resolution";
        case EAI_BADFLAGS:  return "Invalid flags";
        case EAI_FAIL:      return "Non-recoverable failure in name resolution";
        case EAI_FAMILY:    return "Address family not supported";
        case EAI_MEMORY:    return "Memory allocation failure";
        case EAI_NONAME:    return "Name does not resolve";
        case EAI_SERVICE:   return "Service not supported";
        case EAI_SOCKTYPE:  return "Socket type not supported";
        case EAI_SYSTEM:    return "System error";
        case EAI_OVERFLOW:  return "Buffer overflow";
        default:            return "Unknown error";
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    /* Only support IPv4 */
    if (!sa || sa->sa_family != AF_INET) {
        return EAI_FAMILY;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    /* Get hostname */
    if (host && hostlen > 0) {
        if (flags & NI_NUMERICHOST) {
            /* Return numeric IP */
            unsigned char *ip = (unsigned char *)&sin->sin_addr.s_addr;
            int ret = snprintf(host, hostlen, "%u.%u.%u.%u",
                               ip[0], ip[1], ip[2], ip[3]);
            if (ret < 0 || (size_t)ret >= hostlen) {
                return EAI_OVERFLOW;
            }
        } else {
            /* Try reverse DNS lookup using Vita's resolver */
            if (vita_resolver_create() < 0) {
                /* Fall back to numeric if resolver fails */
                unsigned char *ip = (unsigned char *)&sin->sin_addr.s_addr;
                int ret = snprintf(host, hostlen, "%u.%u.%u.%u",
                                   ip[0], ip[1], ip[2], ip[3]);
                if (ret < 0 || (size_t)ret >= hostlen) {
                    return EAI_OVERFLOW;
                }
            } else {
                SceNetInAddr vita_addr;
                vita_addr.s_addr = sin->sin_addr.s_addr;

                int ret = sceNetResolverStartAton(s_resolver_id, &vita_addr,
                                                   host, hostlen,
                                                   RESOLVER_TIMEOUT, RESOLVER_RETRY, 0);
                if (ret < 0) {
                    if (flags & NI_NAMEREQD) {
                        return EAI_NONAME;
                    }
                    /* Fall back to numeric */
                    unsigned char *ip = (unsigned char *)&sin->sin_addr.s_addr;
                    ret = snprintf(host, hostlen, "%u.%u.%u.%u",
                                   ip[0], ip[1], ip[2], ip[3]);
                    if (ret < 0 || (size_t)ret >= hostlen) {
                        return EAI_OVERFLOW;
                    }
                }
            }
        }
    }

    /* Get service name (port) */
    if (serv && servlen > 0) {
        int port = ntohs(sin->sin_port);

        if (flags & NI_NUMERICSERV) {
            /* Return numeric port */
            int ret = snprintf(serv, servlen, "%d", port);
            if (ret < 0 || (size_t)ret >= servlen) {
                return EAI_OVERFLOW;
            }
        } else {
            /* Try to get service name, fall back to numeric */
            const char *name = NULL;
            if (port == 80) name = "http";
            else if (port == 443) name = "https";
            else if (port == 21) name = "ftp";
            else if (port == 22) name = "ssh";

            if (name) {
                if (strlen(name) >= servlen) {
                    return EAI_OVERFLOW;
                }
                strcpy(serv, name);
            } else {
                int ret = snprintf(serv, servlen, "%d", port);
                if (ret < 0 || (size_t)ret >= servlen) {
                    return EAI_OVERFLOW;
                }
            }
        }
    }

    return 0;
}
