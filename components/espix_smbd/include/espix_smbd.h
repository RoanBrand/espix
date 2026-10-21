/*
 * smbd -- the SMB2/3 file server.
 *
 * One share: the root is /etc/smb.conf's `path` (default /home) and the name
 * its `share` (default home). It is read-write to guests, and `user` and
 * `password` add one NTLMv2 account whose session can sign, which is what
 * Windows logs in with.
 *
 * Run it as a unit (`smbd always smbd`), the way nfsd and sshd are run.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serve until keep_going() returns false, then stop cleanly. */
esp_err_t espix_smbd_run(bool (*keep_going)(void));

/* The share as configured, for a status line or a log message. */
const char *espix_smbd_share(void);
const char *espix_smbd_path(void);

#ifdef __cplusplus
}
#endif
