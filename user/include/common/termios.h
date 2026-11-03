/* termios, for both builds: the system's sys/termios.h, and espix's definitions (IDF's layout) where that is
 * empty -- an M-mode app's IDF project with CONFIG_VFS_SUPPORT_TERMIOS off. cfmakeraw() is espix's in both. */
#pragma once

#include <sys/termios.h>
#ifndef TCSANOW
#include "espix_termios.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void cfmakeraw(struct termios *p);

#ifdef __cplusplus
}
#endif
