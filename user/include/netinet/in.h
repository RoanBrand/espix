/* IPv4 and IPv6 addresses for U-mode apps: lwIP's layout and values. */
#pragma once

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    union {
        uint32_t u32_addr[4];
        uint8_t  u8_addr[16];
    } un;
};
#define s6_addr un.u8_addr

/* Members in network byte order. */
struct sockaddr_in {
    uint8_t        sin_len;
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct sockaddr_in6 {
    uint8_t         sin6_len;
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ICMPV6  58
#define IPPROTO_UDPLITE 136
#define IPPROTO_RAW     255

#define IP_TOS             1
#define IP_TTL             2
#define IP_ADD_MEMBERSHIP  3
#define IP_DROP_MEMBERSHIP 4
#define IP_MULTICAST_TTL   5
#define IP_MULTICAST_IF    6
#define IP_MULTICAST_LOOP  7
#define IP_PKTINFO         8

#define IPV6_CHECKSUM        7
#define IPV6_JOIN_GROUP      12
#define IPV6_ADD_MEMBERSHIP  IPV6_JOIN_GROUP
#define IPV6_LEAVE_GROUP     13
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#define IPV6_V6ONLY          27

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

#define IN6ADDR_ANY_INIT      {{{ 0, 0, 0, 0 }}}
#define IN6ADDR_LOOPBACK_INIT {{{ 0, 0, 0, 0x01000000 }}}
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6_IS_ADDR_UNSPECIFIED(a) (((a)->un.u32_addr[0] | (a)->un.u32_addr[1] | (a)->un.u32_addr[2] | (a)->un.u32_addr[3]) == 0)
#define IN6_IS_ADDR_LOOPBACK(a)    (((a)->un.u32_addr[0] | (a)->un.u32_addr[1] | (a)->un.u32_addr[2]) == 0 && (a)->un.u32_addr[3] == 0x01000000)
#define IN6_IS_ADDR_V4MAPPED(a)    (((a)->un.u32_addr[0] | (a)->un.u32_addr[1]) == 0 && (a)->un.u32_addr[2] == 0xffff0000)
#define IN6_IS_ADDR_LINKLOCAL(a)   (((a)->un.u8_addr[0] == 0xfe) && (((a)->un.u8_addr[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_MULTICAST(a)   ((a)->un.u8_addr[0] == 0xff)

/* picolibc's <machine/endian.h> may already define these as macros. */
#ifndef htonl
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
#endif
#ifndef htons
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
#endif

#ifdef __cplusplus
}
#endif
