/* ioctl() for U-mode apps: FIONBIO and FIONREAD on a socket (lwIP's numbers), TIOCGWINSZ on a terminal. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD   0x4004667f
#define FIONBIO    0x8004667e
#define TIOCGWINSZ 0x5413

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
