/* picolibc defines struct utimbuf but not utime(); espix implements it. */
#pragma once

#include_next <utime.h>

#ifdef __cplusplus
extern "C" {
#endif

int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif
