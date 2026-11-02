/* getrandom() for U-mode apps: the hardware RNG, behind GETRANDOM. */
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 0x01
#define GRND_RANDOM   0x02

ssize_t getrandom(void *buf, size_t len, unsigned int flags);
int     getentropy(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
