/*
 * The pipes on smbd's IPC$: a DCE/RPC responder for srvsvc (the share list)
 * and wkssvc (who the server is), enough for a client to browse \\host. See
 * srvsvc.c. The configured strings are kept, not copied, so they must outlive
 * every pipe.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct smbd_srvsvc_pipe smbd_srvsvc_pipe_t;

typedef enum {
    SMBD_PIPE_SRVSVC,
    SMBD_PIPE_WKSSVC,
} smbd_pipe_kind_t;

/* What NetrShareEnum and NetrServerGetInfo report. */
void smbd_srvsvc_config(const char *hostname, const char *share, const char *share_path);

smbd_srvsvc_pipe_t *smbd_srvsvc_open(smbd_pipe_kind_t kind);
void smbd_srvsvc_close(smbd_srvsvc_pipe_t *p);

/* What the client wrote; every whole PDU in it is answered before this
 * returns. -1 if the bytes are not DCE/RPC. */
int smbd_srvsvc_write(smbd_srvsvc_pipe_t *p, const uint8_t *buf, size_t len);

/* Up to max bytes of the next message for a client read; *more is set when
 * the message did not fit, and the rest is the next read's. 0: nothing. */
size_t smbd_srvsvc_read(smbd_srvsvc_pipe_t *p, uint8_t *buf, size_t max, int *more);
