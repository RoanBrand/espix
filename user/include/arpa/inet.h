/* Address conversion for U-mode apps. */
#pragma once

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t   inet_addr(const char *cp);
int         inet_aton(const char *cp, struct in_addr *inp);
char       *inet_ntoa(struct in_addr in);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);

#ifdef __cplusplus
}
#endif
