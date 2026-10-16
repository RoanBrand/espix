/*
 * Force-included into every mosquitto source (see the component's CMakeLists).
 *
 * It carries the two things the broker expects a Unix libc to have that lwIP
 * and newlib do not: every syslog facility name, and gai_strerror() -- lwIP
 * only declares that one when CONFIG_LWIP_NETDB_HAS_GAI_STRERROR is set, and
 * espix does not publish it either. The implementation is beside this header,
 * in espix_platform.c.
 */
#ifndef ESPIX_COMPAT_H
#define ESPIX_COMPAT_H

/* glibc's value. lwIP never returns it; mosquitto only compares. */
#ifndef EAI_SYSTEM
#define EAI_SYSTEM (-11)
#endif

const char *gai_strerror(int errcode);

#endif /* ESPIX_COMPAT_H */
