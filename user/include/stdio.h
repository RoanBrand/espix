/* picolibc's <stdio.h>, and the POSIX pipes to a command it lacks. */
#pragma once

#include_next <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *popen(const char *command, const char *mode);
int   pclose(FILE *stream);

#ifdef __cplusplus
}
#endif
