/* Internal to espix_ssh. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Largest packet we accept. RFC 4253 asks that 32768 bytes of payload be
 * supported, but a shell session never approaches it and each connection holds
 * two of these buffers against DIRAM already 40% used. Anything larger is
 * treated as abuse and drops the connection.
 */
#define SSH_MAX_PACKET   4096
#define SSH_VERSION_MAX  256

/* Message numbers (RFC 4253 §12, RFC 4252 §6, RFC 4254 §9). */
enum {
    SSH_MSG_DISCONNECT                = 1,
    SSH_MSG_IGNORE                    = 2,
    SSH_MSG_UNIMPLEMENTED             = 3,
    SSH_MSG_DEBUG                     = 4,
    SSH_MSG_SERVICE_REQUEST           = 5,
    SSH_MSG_SERVICE_ACCEPT            = 6,
    SSH_MSG_KEXINIT                   = 20,
    SSH_MSG_NEWKEYS                   = 21,
    SSH_MSG_KEX_ECDH_INIT             = 30,
    SSH_MSG_KEX_ECDH_REPLY            = 31,
    SSH_MSG_USERAUTH_REQUEST          = 50,
    SSH_MSG_USERAUTH_FAILURE          = 51,
    SSH_MSG_USERAUTH_SUCCESS          = 52,
    SSH_MSG_GLOBAL_REQUEST            = 80,
    SSH_MSG_REQUEST_FAILURE           = 82,
    SSH_MSG_CHANNEL_OPEN              = 90,
    SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91,
    SSH_MSG_CHANNEL_OPEN_FAILURE      = 92,
    SSH_MSG_CHANNEL_WINDOW_ADJUST     = 93,
    SSH_MSG_CHANNEL_DATA              = 94,
    SSH_MSG_CHANNEL_EOF               = 96,
    SSH_MSG_CHANNEL_CLOSE             = 97,
    SSH_MSG_CHANNEL_REQUEST           = 98,
    SSH_MSG_CHANNEL_SUCCESS           = 99,
    SSH_MSG_CHANNEL_FAILURE           = 100,
};

enum {
    SSH_DISCONNECT_PROTOCOL_ERROR        = 2,
    SSH_DISCONNECT_KEY_EXCHANGE_FAILED   = 3,
    SSH_DISCONNECT_MAC_ERROR             = 5,
    SSH_DISCONNECT_SERVICE_NOT_AVAILABLE = 7,
    SSH_DISCONNECT_NO_MORE_AUTH_METHODS  = 14,
};

/*
 * Cursor over a packet payload. Every SSH field is length-prefixed, so bounds
 * are checked per access and a single `bad` flag is inspected once at the end
 * rather than after every field — which is what keeps the parsers readable.
 */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;    /* bytes written (writer) or total valid (reader) */
    size_t   pos;    /* read cursor */
    bool     bad;    /* overflow or truncation seen */
} ssh_buf_t;

void ssh_buf_init(ssh_buf_t *b, uint8_t *storage, size_t cap);
void ssh_buf_read_from(ssh_buf_t *b, uint8_t *data, size_t len);

void ssh_put_u8(ssh_buf_t *b, uint8_t v);
void ssh_put_u32(ssh_buf_t *b, uint32_t v);
void ssh_put_raw(ssh_buf_t *b, const void *data, size_t len);
void ssh_put_string(ssh_buf_t *b, const void *data, size_t len);
void ssh_put_cstr(ssh_buf_t *b, const char *s);

uint8_t        ssh_get_u8(ssh_buf_t *b);
uint32_t       ssh_get_u32(ssh_buf_t *b);
/* Points into the buffer; no copy, valid while the packet lives. */
const uint8_t *ssh_get_string(ssh_buf_t *b, size_t *out_len);
bool           ssh_get_bool(ssh_buf_t *b);
void           ssh_skip(ssh_buf_t *b, size_t len);

/* ------------------------------------------------------------------ */

typedef struct {
    int      fd;
    uint32_t seq_in;
    uint32_t seq_out;

    char     client_version[SSH_VERSION_MAX];

    uint8_t  in_buf[SSH_MAX_PACKET];
    uint8_t *in_payload;
    size_t   in_len;

    uint8_t  out_buf[SSH_MAX_PACKET];
} ssh_conn_t;

esp_err_t ssh_transport_banner(ssh_conn_t *c);
esp_err_t ssh_packet_read(ssh_conn_t *c);
esp_err_t ssh_packet_write(ssh_conn_t *c, ssh_buf_t *b);
esp_err_t ssh_send_disconnect(ssh_conn_t *c, uint32_t reason, const char *text);

/* Our advertised identification string, also fed into the exchange hash. */
extern const char *const ssh_server_version;

#ifdef __cplusplus
}
#endif
