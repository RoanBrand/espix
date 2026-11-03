/* termios over a terminal's ioctls, which the kernel answers from the session's tty (fds 0-2). */
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int tcgetattr(int fd, struct termios *p)
{
    return ioctl(fd, TCGETS, p);
}

int tcsetattr(int fd, int act, const struct termios *p)
{
    if (act < TCSANOW || act > TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, TCSETS + act, (void *)p);
}

int tcflush(int fd, int sel)
{
    return ioctl(fd, TCFLSH, (void *)(long)sel);
}

/* Output leaves as it is written. */
int tcdrain(int fd)
{
    return isatty(fd) ? 0 : (errno = ENOTTY, -1);
}

pid_t tcgetpgrp(int fd)
{
    int pg;
    return (ioctl(fd, TIOCGPGRP, &pg) == 0) ? (pid_t)pg : -1;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    int pg = (int)pgrp;
    return ioctl(fd, TIOCSPGRP, &pg);
}

void cfmakeraw(struct termios *p)
{
    p->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    p->c_oflag &= ~(tcflag_t)OPOST;
    p->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    p->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    p->c_cflag |= CS8;
    p->c_cc[VMIN]  = 1;
    p->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *p) { return p->c_ispeed; }
speed_t cfgetospeed(const struct termios *p) { return p->c_ospeed; }
int     cfsetispeed(struct termios *p, speed_t sp) { p->c_ispeed = sp; return 0; }
int     cfsetospeed(struct termios *p, speed_t sp) { p->c_ospeed = sp; return 0; }
