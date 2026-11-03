/* termios with IDF's layout and values (esp_libc's platform_include/sys/termios.h), which the kernel's tty
 * speaks: a U-mode app's sys/termios.h, and an M-mode app's where its IDF project leaves termios off.
 * Asserted against IDF's in components/espix_proc/umode_net.c. */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VEOF    0
#define VEOL    1
#define VERASE  2
#define VINTR   3
#define VKILL   4
#define VMIN    5
#define VQUIT   6
#define VSTART  7
#define VSTOP   8
#define VSUSP   9
#define VTIME   10
#define NCCS    (VTIME + 1)

/* c_iflag */
#define BRKINT  (1u << 0)
#define ICRNL   (1u << 1)
#define IGNBRK  (1u << 2)
#define IGNCR   (1u << 3)
#define IGNPAR  (1u << 4)
#define INLCR   (1u << 5)
#define INPCK   (1u << 6)
#define ISTRIP  (1u << 7)
#define IUCLC   (1u << 8)
#define IXANY   (1u << 9)
#define IXOFF   (1u << 10)
#define IXON    (1u << 11)
#define PARMRK  (1u << 12)

/* c_oflag */
#define OPOST   (1u << 0)
#define OLCUC   (1u << 1)
#define ONLCR   (1u << 2)
#define OCRNL   (1u << 3)
#define ONOCR   (1u << 4)
#define ONLRET  (1u << 5)
#define OFILL   (1u << 6)

/* c_cflag */
#define CSIZE   (3u << 0)
#define CS5     (0u << 0)
#define CS6     (1u << 0)
#define CS7     (2u << 0)
#define CS8     (3u << 0)
#define CSTOPB  (1u << 2)
#define CREAD   (1u << 3)
#define PARENB  (1u << 4)
#define PARODD  (1u << 5)
#define HUPCL   (1u << 6)
#define CLOCAL  (1u << 7)

/* c_lflag */
#define ECHO    (1u << 0)
#define ECHOE   (1u << 1)
#define ECHOK   (1u << 2)
#define ECHONL  (1u << 3)
#define ICANON  (1u << 4)
#define IEXTEN  (1u << 5)
#define ISIG    (1u << 6)
#define NOFLSH  (1u << 7)
#define TOSTOP  (1u << 8)

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH  0
#define TCIOFLUSH 1
#define TCOFLUSH  2

#define B0      0
#define B9600   13
#define B19200  14
#define B38400  15
#define B57600  16
#define B115200 17

typedef uint8_t  cc_t;
typedef uint32_t speed_t;
typedef uint16_t tcflag_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

int     tcgetattr(int fd, struct termios *p);
int     tcsetattr(int fd, int act, const struct termios *p);
int     tcflush(int fd, int sel);
int     tcdrain(int fd);
void    cfmakeraw(struct termios *p);
speed_t cfgetispeed(const struct termios *p);
speed_t cfgetospeed(const struct termios *p);
int     cfsetispeed(struct termios *p, speed_t sp);
int     cfsetospeed(struct termios *p, speed_t sp);

#ifdef __cplusplus
}
#endif
