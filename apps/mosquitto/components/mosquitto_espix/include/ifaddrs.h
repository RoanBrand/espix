/*
 * A listener may name the interface it binds to, and mosquitto resolves that
 * name through getifaddrs(). espix has interfaces -- it just does not publish a
 * list of them to an app, so the answer is "none" and a listener that names one
 * is refused rather than silently bound elsewhere.
 */
#ifndef ESPIX_SHIM_IFADDRS_H
#define ESPIX_SHIM_IFADDRS_H

#include <sys/socket.h>

struct ifaddrs {
    struct ifaddrs  *ifa_next;
    char            *ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    union {
        struct sockaddr *ifu_broadaddr;
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
    void            *ifa_data;
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#endif /* ESPIX_SHIM_IFADDRS_H */
