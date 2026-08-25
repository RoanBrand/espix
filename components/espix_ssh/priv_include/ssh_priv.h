/* Internal to espix_ssh. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "psa/crypto.h"

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

/* Sizes fixed by the one algorithm set we offer. */
#define SSH_AES_KEY_LEN   32    /* aes256 */
#define SSH_AES_IV_LEN    16
#define SSH_MAC_KEY_LEN   32    /* hmac-sha2-256 */
#define SSH_MAC_LEN       32
#define SSH_HASH_LEN      32    /* sha256 */
#define SSH_X25519_LEN    32
#define SSH_P256_POINT    65    /* 0x04 || X || Y */

/*
 * The exchange hash covers both complete KEXINIT payloads, so both must be kept
 * from the moment they cross the wire until KEX completes. The client's is not
 * small — OpenSSH offers long name-lists, ~1.3KB in practice — and this is the
 * dominant per-connection cost.
 */
#define SSH_KEXINIT_MAX_C 1600
#define SSH_KEXINIT_MAX_S 256

typedef struct {
    bool                  active;
    psa_cipher_operation_t cipher;   /* CTR is a stream: one long operation */
    mbedtls_svc_key_id_t  cipher_key;
    mbedtls_svc_key_id_t  mac_key;
} ssh_dir_t;

typedef struct {
    int      fd;
    uint32_t seq_in;
    uint32_t seq_out;

    char     client_version[SSH_VERSION_MAX];

    uint8_t  kexinit_c[SSH_KEXINIT_MAX_C];
    size_t   kexinit_c_len;
    uint8_t  kexinit_s[SSH_KEXINIT_MAX_S];
    size_t   kexinit_s_len;

    /* Terrapin (CVE-2023-48795) mitigation, when the client offers it. */
    bool     strict_kex;

    uint8_t  session_id[SSH_HASH_LEN];
    bool     have_session_id;

    /* Authenticated user. Wider than espix's own limit so an over-long name
     * fails authentication rather than being silently truncated into a
     * different, possibly valid, account. */
    char     user[64];

    ssh_dir_t rx;
    ssh_dir_t tx;

    uint8_t  in_buf[SSH_MAX_PACKET];
    uint8_t *in_payload;
    size_t   in_len;

    uint8_t  out_buf[SSH_MAX_PACKET];
    uint8_t  frame[SSH_MAX_PACKET + SSH_MAC_LEN + 8];
} ssh_conn_t;

esp_err_t ssh_transport_banner(ssh_conn_t *c);
esp_err_t ssh_packet_read(ssh_conn_t *c);
esp_err_t ssh_packet_write(ssh_conn_t *c, ssh_buf_t *b);
esp_err_t ssh_send_disconnect(ssh_conn_t *c, uint32_t reason, const char *text);

/* SSH mpint: two's-complement big-endian, leading zeros stripped, one 0x00
 * prepended when the top bit would otherwise read as negative. */
void ssh_put_mpint(ssh_buf_t *b, const uint8_t *be, size_t len);

/* Our advertised identification string, also fed into the exchange hash. */
extern const char *const ssh_server_version;

/* ------------------------------------------------------------------ */
/* Host key (ssh_hostkey.c)                                            */
/* ------------------------------------------------------------------ */

/* Load /etc/ssh/host_ecdsa_key, generating it on first call. */
esp_err_t ssh_hostkey_init(void);

/* SSH-encoded public host key: string("ecdsa-sha2-nistp256"),
 * string("nistp256"), string(point). */
esp_err_t ssh_hostkey_blob(uint8_t *out, size_t cap, size_t *out_len);

/* ecdsa-sha2-nistp256 signature blob over `hash`. */
esp_err_t ssh_hostkey_sign(const uint8_t *hash, size_t hash_len,
                           uint8_t *out, size_t cap, size_t *out_len);

/* ------------------------------------------------------------------ */
/* Key exchange (ssh_kex.c)                                            */
/* ------------------------------------------------------------------ */

/*
 * Runs from a received KEX_ECDH_INIT through NEWKEYS in both directions,
 * leaving c->rx and c->tx active. The next packet read or written is encrypted.
 */
esp_err_t ssh_kex_run(ssh_conn_t *c);

/* ------------------------------------------------------------------ */
/* Authentication (ssh_auth.c)                                         */
/* ------------------------------------------------------------------ */

/*
 * Handles SERVICE_REQUEST through to USERAUTH_SUCCESS, leaving the
 * authenticated name in c->user. Returns non-OK if the client gave up, ran out
 * of attempts, or misbehaved — in every one of those cases the caller should
 * close the connection.
 */
esp_err_t ssh_auth_run(ssh_conn_t *c);

/* Session channel: open, pty-req, shell or sftp, then the session. */
esp_err_t ssh_channel_run(ssh_conn_t *c);

/*
 * Raw channel I/O, for a subsystem carrying bytes rather than a terminal.
 *
 * The interactive path deliberately translates line endings in both
 * directions, because espix is the pty. A file transfer must not go anywhere
 * near that — LF becoming CR LF would corrupt every binary — so these two move
 * bytes through the channel untouched.
 *
 * recv_raw hands back a pointer into the connection's packet buffer, valid
 * until the next call.
 */
esp_err_t ssh_channel_send_raw(ssh_conn_t *c, const void *data, size_t len);
esp_err_t ssh_channel_recv_raw(ssh_conn_t *c, uint8_t **out, size_t *out_len);

/* ------------------------------------------------------------------ */
/* SFTP subsystem (sftp.c)                                             */
/* ------------------------------------------------------------------ */

/* Serve SFTP on an already-open channel until the client goes away. */
esp_err_t espix_sftp_run(ssh_conn_t *c);

/* Counted so `sshd` status can show them; incremented by ssh_auth.c. */
void ssh_server_note_rejection(void);

#ifdef __cplusplus
}
#endif
