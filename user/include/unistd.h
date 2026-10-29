/* picolibc's <unistd.h>, plus gettid(), which glibc declares there too. */
#pragma once

#include_next <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t gettid(void);

#ifdef __cplusplus
}
#endif
