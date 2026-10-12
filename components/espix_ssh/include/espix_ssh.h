/*
 * espix SSH server.
 *
 * A minimal RFC 4251-4254 server built on PSA Crypto, offering exactly one
 * algorithm per role. Written rather than imported because the only SSH
 * component on the ESP registry (wolfSSH) is GPL-or-commercial, which would
 * force every distributed espix image to be GPL.
 *
 * Security posture, stated plainly: this is a hand-written implementation of a
 * security protocol. It is appropriate for a trusted LAN and inappropriate for
 * exposure to the internet. Do not port-forward it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Listen and accept until keep_going() is false, on the calling process -- the
 * sshd unit. Each connection gets a task of its own and outlives a stop.
 */
esp_err_t espix_ssh_run(bool (*keep_going)(void));

/* Host key fingerprint in OpenSSH's SHA256:base64 form, for comparing against
 * what a client shows on first connect. Empty until the key exists. */
const char *espix_ssh_fingerprint(void);

typedef struct {
    bool     running;
    uint16_t port;
    unsigned sessions;      /* currently connected */
    unsigned accepted;      /* since boot */
    unsigned rejected;      /* auth failures since boot */
} espix_ssh_status_t;

void espix_ssh_status(espix_ssh_status_t *out);

#ifdef __cplusplus
}
#endif
