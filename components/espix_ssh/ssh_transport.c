/*
 * SSH transport: identification exchange, binary packet protocol, wire types.
 *
 * Currently unencrypted — this is the framing layer that KEX will later install
 * keys into. Structured so that adding encryption means filling in two hooks
 * (read: verify MAC then decrypt; write: encrypt then MAC) rather than
 * rewriting the packet paths.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include "espix_kernel.h"
#include "ssh_priv.h"

#define TAG "ssh"

/* RFC 4253 §4.2: "SSH-protoversion-softwareversion SP comments" then CR LF. */
const char *const ssh_server_version = "SSH-2.0-espix_0.1.0";

/* Unencrypted packets pad to a multiple of 8 (RFC 4253 §6). */
#define CIPHER_BLOCK 8
#define MIN_PADDING  4

/* ------------------------------------------------------------------ */
/* Wire types                                                          */
/* ------------------------------------------------------------------ */

void ssh_buf_init(ssh_buf_t *b, uint8_t *storage, size_t cap)
{
    b->buf = storage;
    b->cap = cap;
    b->len = 0;
    b->pos = 0;
    b->bad = false;
}

void ssh_buf_read_from(ssh_buf_t *b, uint8_t *data, size_t len)
{
    b->buf = data;
    b->cap = len;
    b->len = len;
    b->pos = 0;
    b->bad = false;
}

static bool room(ssh_buf_t *b, size_t n)
{
    if (b->bad || b->len + n > b->cap) {
        b->bad = true;
        return false;
    }
    return true;
}

void ssh_put_u8(ssh_buf_t *b, uint8_t v)
{
    if (room(b, 1)) {
        b->buf[b->len++] = v;
    }
}

void ssh_put_u32(ssh_buf_t *b, uint32_t v)
{
    if (room(b, 4)) {
        b->buf[b->len++] = (uint8_t)(v >> 24);
        b->buf[b->len++] = (uint8_t)(v >> 16);
        b->buf[b->len++] = (uint8_t)(v >> 8);
        b->buf[b->len++] = (uint8_t)v;
    }
}

void ssh_put_raw(ssh_buf_t *b, const void *data, size_t len)
{
    if (room(b, len)) {
        memcpy(b->buf + b->len, data, len);
        b->len += len;
    }
}

void ssh_put_string(ssh_buf_t *b, const void *data, size_t len)
{
    ssh_put_u32(b, (uint32_t)len);
    ssh_put_raw(b, data, len);
}

void ssh_put_cstr(ssh_buf_t *b, const char *s)
{
    ssh_put_string(b, s, strlen(s));
}

static bool avail(ssh_buf_t *b, size_t n)
{
    if (b->bad || b->pos + n > b->len) {
        b->bad = true;
        return false;
    }
    return true;
}

uint8_t ssh_get_u8(ssh_buf_t *b)
{
    return avail(b, 1) ? b->buf[b->pos++] : 0;
}

uint32_t ssh_get_u32(ssh_buf_t *b)
{
    if (!avail(b, 4)) {
        return 0;
    }
    const uint32_t v = ((uint32_t)b->buf[b->pos] << 24) |
                       ((uint32_t)b->buf[b->pos + 1] << 16) |
                       ((uint32_t)b->buf[b->pos + 2] << 8) |
                       (uint32_t)b->buf[b->pos + 3];
    b->pos += 4;
    return v;
}

const uint8_t *ssh_get_string(ssh_buf_t *b, size_t *out_len)
{
    const uint32_t len = ssh_get_u32(b);

    /* Length is attacker-controlled: reject before it is used as a size. */
    if (b->bad || len > b->len - b->pos) {
        b->bad = true;
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }

    const uint8_t *p = b->buf + b->pos;
    b->pos += len;
    if (out_len) {
        *out_len = len;
    }
    return p;
}

bool ssh_get_bool(ssh_buf_t *b)
{
    return ssh_get_u8(b) != 0;
}

void ssh_skip(ssh_buf_t *b, size_t len)
{
    if (avail(b, len)) {
        b->pos += len;
    }
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t read_exact(int fd, void *dst, size_t len)
{
    uint8_t *p = dst;
    size_t   got = 0;

    while (got < len) {
        const ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0) {
            return ESP_ERR_INVALID_STATE;   /* peer closed */
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t write_all(int fd, const void *src, size_t len)
{
    const uint8_t *p = src;
    size_t         sent = 0;

    while (sent < len) {
        const ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Identification exchange (RFC 4253 §4.2)                             */
/* ------------------------------------------------------------------ */

esp_err_t ssh_transport_banner(ssh_conn_t *c)
{
    char line[SSH_VERSION_MAX];
    int  n = snprintf(line, sizeof(line), "%s\r\n", ssh_server_version);

    if (write_all(c->fd, line, (size_t)n) != ESP_OK) {
        return ESP_FAIL;
    }

    /*
     * The client may send any number of UTF-8 lines before its identification
     * string; only the line starting with "SSH-" counts. Read byte-at-a-time
     * because there is no framing yet and over-reading would consume the first
     * binary packet.
     */
    for (int attempt = 0; attempt < 8; attempt++) {
        size_t len = 0;

        for (;;) {
            char ch;
            if (read_exact(c->fd, &ch, 1) != ESP_OK) {
                return ESP_FAIL;
            }
            if (ch == '\n') {
                break;
            }
            if (ch == '\r') {
                continue;
            }
            if (len + 1 < sizeof(c->client_version)) {
                c->client_version[len++] = ch;
            } else {
                return ESP_ERR_INVALID_SIZE;    /* absurdly long: give up */
            }
        }
        c->client_version[len] = '\0';

        if (strncmp(c->client_version, "SSH-", 4) == 0) {
            /* We only speak 2.0; 1.99 means "either", which is fine. */
            if (strncmp(c->client_version, "SSH-2.0", 7) != 0 &&
                strncmp(c->client_version, "SSH-1.99", 8) != 0) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            espix_klog(ESPIX_KLOG_INFO, TAG, "client is %s",
                       c->client_version);
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

/* ------------------------------------------------------------------ */
/* Binary packet protocol (RFC 4253 §6)                                */
/* ------------------------------------------------------------------ */

esp_err_t ssh_packet_read(ssh_conn_t *c)
{
    uint8_t header[4];
    if (read_exact(c->fd, header, sizeof(header)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t packet_len = ((uint32_t)header[0] << 24) |
                                ((uint32_t)header[1] << 16) |
                                ((uint32_t)header[2] << 8) |
                                (uint32_t)header[3];

    /* Bound before allocating anything against it. The lower bound keeps a
     * malformed length from underflowing the payload arithmetic below. */
    if (packet_len < CIPHER_BLOCK || packet_len > SSH_MAX_PACKET - 4) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "bad packet length %u",
                   (unsigned)packet_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (read_exact(c->fd, c->in_buf, packet_len) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t padding = c->in_buf[0];
    if (padding < MIN_PADDING || (size_t)padding + 1 > packet_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    c->in_payload = c->in_buf + 1;
    c->in_len     = packet_len - padding - 1;
    c->seq_in++;

    if (c->in_len == 0) {
        return ESP_ERR_INVALID_SIZE;    /* every packet carries a message id */
    }
    return ESP_OK;
}

esp_err_t ssh_packet_write(ssh_conn_t *c, ssh_buf_t *b)
{
    if (b->bad) {
        /* A writer that overflowed would otherwise emit a truncated packet,
         * which the peer would read as a protocol error at a confusing point. */
        espix_klog(ESPIX_KLOG_ERROR, TAG, "refusing to send overflowed packet");
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * RFC 4253 §6: the multiple-of-8 requirement covers
     *
     *     packet_length || padding_length || payload || padding
     *
     * so the 4-byte length field is *part of* the padded quantity even though
     * packet_length itself does not count it. Padding only
     * padding_length+payload+padding leaves every packet 4 bytes off, which a
     * client reports as "padding error: need N block 8 mod 4".
     */
    const size_t framed  = 4 + 1 + b->len;
    size_t       padding = CIPHER_BLOCK - (framed % CIPHER_BLOCK);
    if (padding < MIN_PADDING) {
        padding += CIPHER_BLOCK;
    }

    /* packet_length counts everything after itself. */
    const size_t packet_len = 1 + b->len + padding;

    /*
     * Check the invariant here rather than let the peer discover it. A framing
     * error looks like a MAC failure or a corrupt packet from the other end,
     * which is a long way from the arithmetic that caused it.
     */
    if ((4 + packet_len) % CIPHER_BLOCK != 0 || padding > 255) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "framing bug: payload %u padding %u total %u",
                   (unsigned)b->len, (unsigned)padding,
                   (unsigned)(4 + packet_len));
        return ESP_FAIL;
    }

    uint8_t frame[4 + 1];

    frame[0] = (uint8_t)(packet_len >> 24);
    frame[1] = (uint8_t)(packet_len >> 16);
    frame[2] = (uint8_t)(packet_len >> 8);
    frame[3] = (uint8_t)packet_len;
    frame[4] = (uint8_t)padding;

    /* Zero padding is legal while unencrypted; once a cipher is active it must
     * be random, which is a KEX-stage concern. */
    uint8_t pad[CIPHER_BLOCK * 2] = {0};

    if (write_all(c->fd, frame, sizeof(frame)) != ESP_OK ||
        write_all(c->fd, b->buf, b->len) != ESP_OK ||
        write_all(c->fd, pad, padding) != ESP_OK) {
        return ESP_FAIL;
    }

    c->seq_out++;
    return ESP_OK;
}

esp_err_t ssh_send_disconnect(ssh_conn_t *c, uint32_t reason, const char *text)
{
    ssh_buf_t b;
    ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));

    ssh_put_u8(&b, SSH_MSG_DISCONNECT);
    ssh_put_u32(&b, reason);
    ssh_put_cstr(&b, text ? text : "");
    ssh_put_cstr(&b, "");           /* language tag */

    return ssh_packet_write(c, &b);
}
