/* ioctl() for U-mode apps: FIONBIO and FIONREAD on a socket (lwIP's numbers); a terminal's on fds 0-2, Linux's
 * numbers (espix_syscall.h), and FIONREAD there too. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD   0x4004667f
#define FIONBIO    0x8004667e
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCFLSH     0x540B
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long req, ...);

#ifdef __cplusplus
}
#endif
