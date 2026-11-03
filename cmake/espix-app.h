/*
 * Force-included into every M-mode app (cmake/espix-app.cmake): names the
 * kernel publishes that IDF's and lwIP's headers do not declare. A U-mode app
 * finds the same in user/include.
 */
#pragma once

const char *gai_strerror(int err);

/* espix's own, after lwIP's 200-204 (components/espix_proc/include/espix_gai.h). */
#ifndef EAI_AGAIN
#define EAI_AGAIN    205
#define EAI_BADFLAGS 206
#define EAI_SOCKTYPE 207
#define EAI_OVERFLOW 208
#define EAI_SYSTEM   209
#endif

/* A terminal's requests on fds 0-2, Linux's numbers, which espix's tty answers; IDF's sys/ioctl.h has none. */
#ifndef TIOCGWINSZ
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#endif
#ifndef _ESPIX_WINSIZE_DEFINED
#define _ESPIX_WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif
