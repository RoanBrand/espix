/*
 * ESP-IDF ships sys/poll.h, not poll.h, and mosquitto's event loop includes the
 * latter. This is that header and nothing else.
 *
 * The distinction matters for the ABI. IDF's poll() is esp_libc's -- a select()
 * over the one descriptor space, so it covers espix's VFS files *and* lwIP
 * sockets -- and it is the name sys/poll.h declares. lwIP's lwip_poll() is
 * sockets only; aliasing to it (as an earlier version of this file did) would
 * have compiled and then quietly ignored every non-socket descriptor.
 */
#ifndef ESPIX_SHIM_POLL_H
#define ESPIX_SHIM_POLL_H

#include <sys/poll.h>

#endif /* ESPIX_SHIM_POLL_H */
