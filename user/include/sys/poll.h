/* poll() for U-mode apps: IDF's values, which the kernel's poll uses. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN     0x01
#define POLLRDNORM 0x02
#define POLLRDBAND 0x04
#define POLLPRI    POLLRDBAND
#define POLLOUT    0x08
#define POLLWRNORM POLLOUT
#define POLLWRBAND 0x10
#define POLLERR    0x20
#define POLLHUP    0x40
#define POLLNVAL   0x80

struct pollfd {
    int   fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
