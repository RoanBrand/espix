/* Name lookup for U-mode apps: lwIP's resolver, behind GETADDRINFO; its flags and EAI_* codes. */
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

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

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08
#define AI_V4MAPPED    0x10
#define AI_ALL         0x20
#define AI_ADDRCONFIG  0x40

#define EAI_NONAME   200
#define EAI_SERVICE  201
#define EAI_FAIL     202
#define EAI_MEMORY   203
#define EAI_FAMILY   204
/* Not lwIP's: what libespix reports for the rest. */
#define EAI_AGAIN    205
#define EAI_BADFLAGS 206
#define EAI_SOCKTYPE 207
#define EAI_OVERFLOW 208
#define EAI_SYSTEM   209

#define HOST_NOT_FOUND 210
#define NO_DATA        211
#define NO_RECOVERY    212
#define TRY_AGAIN      213

#define NI_MAXHOST     1025
#define NI_MAXSERV     32
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02

extern int h_errno;

int             getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                            struct addrinfo **res);
void            freeaddrinfo(struct addrinfo *res);
const char     *gai_strerror(int err);
struct hostent *gethostbyname(const char *name);
int             getnameinfo(const struct sockaddr *addr, socklen_t len, char *host, socklen_t hostlen,
                            char *serv, socklen_t servlen, int flags);

#ifdef __cplusplus
}
#endif
