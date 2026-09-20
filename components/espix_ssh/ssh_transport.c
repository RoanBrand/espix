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

#include "esp_timer.h"
#include "mbedtls/platform_util.h"   /* mbedtls_platform_zeroize */
#include "sha/sha_core.h"            /* the HMAC key schedule, prepared below */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_kernel.h"
#include "espix_proc.h"   /* espix_proc_stopping(), so a writing app can be signalled */
#include "ssh_priv.h"

#define TAG "ssh"

/*
 * RFC 4253 §4.2: "SSH-protoversion-softwareversion SP comments" then CR LF.
 *
 * Built from ESPIX_VERSION_STR rather than written out, so the banner cannot
 * drift from what `motd` and espix_version() report -- which it had, sitting at
 * 0.1.0 through a release that had moved on.
 */
const char *const ssh_server_version = "SSH-2.0-espix_" ESPIX_VERSION_STR;

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

void ssh_put_mpint(ssh_buf_t *b, const uint8_t *be, size_t len)
{
    /* Strip leading zeros: mpint is minimal-length. */
    size_t i = 0;
    while (i < len && be[i] == 0) {
        i++;
    }

    if (i == len) {
        ssh_put_u32(b, 0);      /* zero is the empty string */
        return;
    }

    /* A set top bit would read as a negative two's-complement number, so a
     * zero byte goes in front. Forgetting this breaks roughly half of all
     * exchange hashes, at random, which is a memorable way to learn it. */
    const bool pad = (be[i] & 0x80) != 0;

    ssh_put_u32(b, (uint32_t)(len - i + (pad ? 1 : 0)));
    if (pad) {
        ssh_put_u8(b, 0);
    }
    ssh_put_raw(b, be + i, len - i);
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

/*
 * How long a peer may leave a packet half-delivered before the connection is
 * given up on. Only ever reached mid-packet; see the note in read_exact().
 */
#define PARTIAL_READ_TIMEOUT_MS 5000

/*
 * How long a peer may refuse to drain before a send is given up on. Longer than
 * the read side's patience because the causes differ: a stalled read is a peer
 * that stopped talking, while a stalled write is a peer that is merely slow to
 * consume, which a large scp to a busy client legitimately is.
 */
#define BLOCKED_WRITE_TIMEOUT_MS 15000

static esp_err_t read_exact(int fd, void *dst, size_t len)
{
    uint8_t *p = dst;
    size_t   got = 0;
    int64_t  stalled_since = 0;     /* us; 0 until a packet is part-read */

    while (got < len) {
        const ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0) {
            return ESP_ERR_INVALID_STATE;   /* peer closed */
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            /*
             * Two different waits arrive here as EAGAIN, and telling them apart
             * is the whole point of this branch.
             *
             * The socket carries SO_RCVTIMEO (see ssh_server.c), and it is also
             * briefly O_NONBLOCK whenever esp_linenoise's terminal probe runs
             * on this same descriptor. So EAGAIN means "nothing right now",
             * never "the peer is gone".
             *
             * With `got == 0` nothing of a packet has arrived, which is an idle
             * session waiting on a keystroke and may legitimately last hours.
             * Wait indefinitely.
             *
             * With `got > 0` a packet is half-delivered and the peer has stopped
             * mid-message. That is broken or vanished, and waiting forever is
             * what parked connection tasks permanently: the task never exited,
             * so the server's session count never came back down, and after
             * CONFIG_ESPIX_SSH_MAX_SESSIONS of them every new connection was
             * refused. Bound it.
             */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (got > 0) {
                    /*
                     * Measure the stall with a clock, not by counting turns of
                     * this loop: each turn blocks for SO_RCVTIMEO and then
                     * delays again, so a per-iteration tally bears no relation
                     * to elapsed time.
                     */
                    const int64_t now = esp_timer_get_time();
                    if (stalled_since == 0) {
                        stalled_since = now;
                    } else if (now - stalled_since >=
                               (int64_t)PARTIAL_READ_TIMEOUT_MS * 1000) {
                        espix_klog(ESPIX_KLOG_WARN, TAG,
                                   "peer stopped %u bytes into a %u-byte packet",
                                   (unsigned)got, (unsigned)len);
                        return ESP_ERR_TIMEOUT;
                    }
                }
                vTaskDelay(1);
                continue;
            }
            return ESP_FAIL;
        }
        got += (size_t)n;
        stalled_since = 0;          /* progress resets the patience */
    }
    return ESP_OK;
}

/*
 * Send the whole buffer, retrying rather than abandoning it half-sent.
 *
 * The EAGAIN branch is the point. Every SSH packet goes out through here, and
 * returning early leaves *half a packet on the wire*: the peer resynchronises
 * onto the middle of a frame and the connection dies. Worse, seq_out is only
 * bumped after all three sends succeed, so a partial send also desynchronises
 * the sequence number the MAC is computed over -- every later packet would
 * then fail to verify even if it were assembled perfectly.
 *
 * EAGAIN is expected on this descriptor, for the reason read_exact() spells out
 * above: the socket is briefly O_NONBLOCK whenever esp_linenoise's terminal
 * probe runs on it. O_NONBLOCK belongs to the descriptor and not to a
 * direction, so it applies to send() as much as to recv(). The read side was
 * taught to expect it; this side never was.
 *
 * Honesty about what this does *not* fix: it was written to explain the
 * `Corrupted MAC` failures documented in KNOWN-ISSUES, and measurement says it
 * does not. Instrumenting this branch showed it never being taken on a failing
 * run -- zero EAGAIN across every connection, and the corruption happened
 * anyway. It stays because abandoning a partially-written packet is wrong on
 * its own terms and would eventually be somebody's very bad afternoon, not
 * because it closes that bug.
 *
 * The stall is bounded whether or not anything has been sent yet, which is
 * where this parts company with read_exact(). An idle read may legitimately
 * wait hours for a keystroke; a send that cannot progress means the peer has
 * stopped draining, and a connection task parked there forever is what once
 * exhausted the session limit.
 *
 * DEBUG for the timeout because it is diagnostic and the failure surfaces to
 * the caller anyway, not because a louder level would be unsafe -- it would
 * have been, until klog stopped echoing through the calling task's stdout. See
 * s_console_out in espix_kernel/klog.c: the first draft of this logged at WARN
 * and re-entered write_all() from inside its own half-sent packet.
 */
static esp_err_t write_all(int fd, const void *src, size_t len)
{
    const uint8_t *p = src;
    size_t         sent = 0;
    int64_t        blocked_since = 0;   /* us; 0 while making progress */

    while (sent < len) {
        const ssize_t n = send(fd, p + sent, len - sent, 0);

        if (n > 0) {
            sent += (size_t)n;
            blocked_since = 0;          /* progress resets the patience */
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* By the clock, not by counting turns: each turn delays for an
             * interval of its own, so a per-iteration tally would bear no
             * relation to elapsed time. Same correction read_exact() needed. */
            const int64_t now = esp_timer_get_time();

            if (blocked_since == 0) {
                blocked_since = now;
            } else if (now - blocked_since >=
                       (int64_t)BLOCKED_WRITE_TIMEOUT_MS * 1000) {
                /* DEBUG, and that is load-bearing: see the note above. */
                espix_klog(ESPIX_KLOG_DEBUG, TAG,
                           "peer stopped reading %u bytes into a %u-byte send",
                           (unsigned)sent, (unsigned)len);
                return ESP_ERR_TIMEOUT;
            }
            /*
             * The delivery point a writing process never had.
             *
             * This loop is where an app blocks when the peer stops draining,
             * and until now nothing here could be interrupted: `kill` set
             * stop_requested and the app never looked, so the grace always
             * expired and the task was deleted -- inside lwIP, holding the
             * channel's transmit lock. Both of those leave dangling state, and
             * between them they account for every panic this bug has produced.
             *
             * Giving up here unwinds the process back through its own stack
             * instead, so it releases the lock and leaves lwIP cleanly. The
             * packet is abandoned half-sent, which is why the caller closes the
             * channel on a write failure -- but the connection was ending
             * anyway, and a truncated stream is strictly better than a task
             * deleted mid-syscall.
             *
             * espix_proc_stopping() rather than espix_sigcheck(): the latter
             * runs handlers and can park on SIGSTOP, and neither is safe with a
             * packet half-written and tx_lock held.
             */
            if (espix_proc_stopping()) {
                espix_klog(ESPIX_KLOG_DEBUG, TAG,
                           "send interrupted %u bytes into %u; process stopping",
                           (unsigned)sent, (unsigned)len);
                return ESP_ERR_INVALID_STATE;
            }

            vTaskDelay(1);
            continue;
        }
        return ESP_FAIL;
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

/*
 * With hmac-sha2-256-etm@openssh.com the length field stays in clear and the
 * MAC covers the ciphertext, so a packet can be authenticated before anything
 * is decrypted — which is both the Terrapin-era recommendation and much simpler
 * than decrypting a block just to learn how much more to read.
 *
 * Note the block size changes with the cipher: 8 while unencrypted, 16 once
 * AES is active. And the padded region differs too — before encryption the
 * length field counts toward the multiple, with ETM it does not, because it is
 * not part of the encrypted run.
 */
static size_t block_size(const ssh_dir_t *d)
{
    return d->active ? SSH_AES_IV_LEN : CIPHER_BLOCK;
}

/*
 * The quantity that must be a multiple of the block size.
 *
 * In the clear it is packet_length ‖ padding_length ‖ payload ‖ padding — the
 * 4-byte length field counts, even though packet_length does not include
 * itself. Under ETM the length is not encrypted, so only the encrypted run
 * aligns and those 4 bytes drop out.
 *
 * Deliberately one function used by both the reader and the writer. Writing the
 * rule out twice is what produced the same off-by-four bug twice: once sending
 * (a client reporting "padding error ... mod 4") and once receiving (rejecting
 * a perfectly good 1564-byte packet).
 */
static size_t aligned_len(const ssh_dir_t *d, size_t packet_len)
{
    return d->active ? packet_len : 4 + packet_len;
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA256 over the peripheral                                     */
/* ------------------------------------------------------------------ */

/*
 * HMAC's key schedule is two SHA-256 states, and it is fixed for the life of
 * the key. psa_mac_*() rebuilds it on every operation -- two SHA setups, each a
 * heap allocation and a peripheral reset, plus a PSA key-slot lookup -- which on
 * a per-packet path was measured at ~300us of setup before a byte was hashed.
 * The states are prepared once instead (ssh_mac_prepare) and each packet is
 * hashed directly, with no allocation and no key slot.
 *
 * These are the SHA engine's own building blocks rather than a hash API: the
 * caller holds the hardware, and the message length is tracked here because the
 * final block's bit count is not something the engine keeps for us.
 */
typedef struct {
    uint8_t  buf[SSH_HASH_BLOCK];   /* a partial block waiting for more */
    size_t   len;                   /* bytes in buf */
    uint64_t total;                 /* whole message, for the length field */
} sha_stream_t;

static void sha_stream_start(sha_stream_t *s)
{
    s->len = 0;
    /*
     * The prepared state has already absorbed one block -- the ipad or the
     * opad -- so the length the final block carries counts from there.
     * Starting at zero instead produces a perfectly well-formed MAC that no
     * peer agrees with, which is how this was found.
     */
    s->total = SSH_HASH_BLOCK;
}

/*
 * data may be in PSRAM and unaligned: esp_sha_block() presents a block to the
 * engine from the CPU, so neither matters and no cache synchronization is
 * involved, unlike the DMA path the PSA driver takes.
 */
static void sha_stream_update(sha_stream_t *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    if (n == 0) {
        return;
    }
    s->total += n;

    if (s->len != 0) {
        const size_t fill = SSH_HASH_BLOCK - s->len;
        if (n < fill) {
            memcpy(s->buf + s->len, p, n);
            s->len += n;
            return;
        }
        memcpy(s->buf + s->len, p, fill);
        esp_sha_block(SHA2_256, s->buf, false);
        p += fill;
        n -= fill;
        s->len = 0;
    }

    while (n >= SSH_HASH_BLOCK) {
        esp_sha_block(SHA2_256, p, false);
        p += SSH_HASH_BLOCK;
        n -= SSH_HASH_BLOCK;
    }

    if (n != 0) {
        memcpy(s->buf, p, n);
        s->len = n;
    }
}

static void sha_stream_finish(sha_stream_t *s, uint8_t out[SSH_HASH_LEN])
{
    const uint64_t bits = s->total * 8;

    s->buf[s->len++] = 0x80;
    if (s->len > 56) {
        memset(s->buf + s->len, 0, SSH_HASH_BLOCK - s->len);
        esp_sha_block(SHA2_256, s->buf, false);
        s->len = 0;
    }
    memset(s->buf + s->len, 0, 56 - s->len);
    for (unsigned i = 0; i < 8; i++) {
        s->buf[SSH_HASH_BLOCK - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    esp_sha_block(SHA2_256, s->buf, false);
    esp_sha_read_digest_state(SHA2_256, out);
}

static void mac_selftest(const uint8_t *key, size_t key_len,
                         const uint8_t *inner, const uint8_t *outer);

void ssh_mac_prepare(const uint8_t *key, size_t key_len,
                     uint8_t inner[SSH_HASH_LEN], uint8_t outer[SSH_HASH_LEN])
{
    uint8_t ipad[SSH_HASH_BLOCK];
    uint8_t opad[SSH_HASH_BLOCK];

    memset(ipad, 0x36, sizeof(ipad));
    memset(opad, 0x5c, sizeof(opad));
    for (size_t i = 0; i < key_len && i < SSH_HASH_BLOCK; i++) {
        ipad[i] ^= key[i];
        opad[i] ^= key[i];
    }

    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);
    /* start=true reloads the IV, so the second block does not disturb the first
     * before its state has been read out. */
    esp_sha_block(SHA2_256, ipad, true);
    esp_sha_read_digest_state(SHA2_256, inner);
    esp_sha_block(SHA2_256, opad, true);
    esp_sha_read_digest_state(SHA2_256, outer);
    esp_sha_release_hardware();

    mbedtls_platform_zeroize(ipad, sizeof(ipad));
    mbedtls_platform_zeroize(opad, sizeof(opad));

    /* Once per boot, on the first key: see mac_selftest(). */
    static bool tested;
    if (!tested) {
        tested = true;
        mac_selftest(key, key_len, inner, outer);
    }
}

/*
 * A known-answer test, because this is hand-driven HMAC and "every login still
 * works" would also be true of an implementation that is consistently wrong.
 * The comparison is against PSA, which reaches the same hardware by another
 * route, and the check runs through the code the packets do -- prepare the
 * states, hash in two halves -- rather than beside it.
 *
 * It has already earned its place: a final block whose length field forgot the
 * already-absorbed pad block produced a well-formed MAC that no peer agreed
 * with, and every login was refused.
 */
static void mac_selftest(const uint8_t *key, size_t key_len,
                         const uint8_t *inner, const uint8_t *outer)
{
    static const uint8_t msg[] = "espix hmac selftest";
    uint8_t      inner_digest[SSH_HASH_LEN];
    uint8_t      mine[SSH_MAC_LEN], theirs[SSH_MAC_LEN];
    sha_stream_t s;
    size_t       n = 0;

    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);
    esp_sha_write_digest_state(SHA2_256, (void *)inner);
    sha_stream_start(&s);
    sha_stream_update(&s, msg, sizeof(msg) - 1);
    sha_stream_finish(&s, inner_digest);
    esp_sha_write_digest_state(SHA2_256, (void *)outer);
    sha_stream_start(&s);
    sha_stream_update(&s, inner_digest, sizeof(inner_digest));
    sha_stream_finish(&s, mine);
    esp_sha_release_hardware();

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    mbedtls_svc_key_id_t k = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attr, key, key_len, &k) != PSA_SUCCESS ||
        psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, sizeof(msg) - 1,
                        theirs, sizeof(theirs), &n) != PSA_SUCCESS) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "HMAC self-test could not run");
        return;
    }
    psa_destroy_key(k);

    if (memcmp(mine, theirs, SSH_MAC_LEN) != 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "HMAC self-test FAILED -- the packet MAC is wrong");
        return;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "HMAC self-test ok");
}

/*
 * HMAC-SHA256 over up to three parts, using the schedule prepared for d. Parts
 * rather than one assembled buffer because the receive path's sequence and
 * header are not contiguous with the ciphertext, and copying a whole packet to
 * make them so is most of what this exists to avoid. Takes and releases the
 * hardware, so it must not be called while it is held.
 */
static void mac_parts(const ssh_dir_t *d,
                      const void *a, size_t alen,
                      const void *b, size_t blen,
                      const void *c, size_t clen,
                      uint8_t mac[SSH_MAC_LEN])
{
    uint8_t      inner[SSH_HASH_LEN];
    uint8_t      inner_state[SSH_HASH_LEN];
    uint8_t      outer_state[SSH_HASH_LEN];
    sha_stream_t s;

    /* The prepared states live in the connection struct, which is in PSRAM;
     * the digest-register transfers want internal RAM. */
    memcpy(inner_state, d->mac_inner, sizeof(inner_state));
    memcpy(outer_state, d->mac_outer, sizeof(outer_state));

    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);

    esp_sha_write_digest_state(SHA2_256, inner_state);
    sha_stream_start(&s);
    sha_stream_update(&s, a, alen);
    sha_stream_update(&s, b, blen);
    sha_stream_update(&s, c, clen);
    sha_stream_finish(&s, inner);

    esp_sha_write_digest_state(SHA2_256, outer_state);
    sha_stream_start(&s);
    sha_stream_update(&s, inner, sizeof(inner));
    sha_stream_finish(&s, mac);

    esp_sha_release_hardware();
    mbedtls_platform_zeroize(inner, sizeof(inner));
}

/* Length-independent compare, so a peer cannot time its way to a forgery. */
static bool mac_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

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

    /* Bound before it is used as a size. The lower bound also keeps a malformed
     * length from underflowing the payload arithmetic below. */
    const size_t blk = block_size(&c->rx);
    if (packet_len < blk || packet_len > SSH_MAX_PACKET - 4 ||
        (aligned_len(&c->rx, packet_len) % blk) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "bad packet length %u (blk %u)",
                   (unsigned)packet_len, (unsigned)blk);
        return ESP_ERR_INVALID_SIZE;
    }

    if (read_exact(c->fd, c->in_buf, packet_len) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    if (c->rx.active) {
        uint8_t mac[SSH_MAC_LEN];
        if (read_exact(c->fd, mac, sizeof(mac)) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }

        /* MAC input is seq ‖ length ‖ ciphertext, in wire order. */
        uint8_t seq[4];
        seq[0] = (uint8_t)(c->seq_in >> 24);
        seq[1] = (uint8_t)(c->seq_in >> 16);
        seq[2] = (uint8_t)(c->seq_in >> 8);
        seq[3] = (uint8_t)c->seq_in;

        /*
         * Streamed in three parts rather than assembled into one buffer.
         *
         * The assembled version used tx_frame as its scratch space -- and the
         * transmit path keeps tx_frame live across its write_all(), because the
         * ciphertext it is sending *is* tx_frame + 8. Receive runs under
         * rx_lock and transmit under tx_lock, so the two never excluded each
         * other: an inbound packet arriving mid-send overwrote ciphertext that
         * had already been MAC'd, and the peer reported a corrupt MAC an
         * unbounded distance from the cause. The same collision in the other
         * order failed a perfectly good inbound packet.
         *
         * A lock big enough to cover both directions would serialise reads
         * against writes, which is exactly what a duplex transport must not do.
         * Streaming the MAC needs no shared buffer at all, so there is nothing
         * left to race over.
         */
        uint8_t expected[SSH_MAC_LEN];
        mac_parts(&c->rx, seq, sizeof(seq), header, sizeof(header),
                  c->in_buf, packet_len, expected);

        if (!mac_equal(expected, mac, sizeof(mac))) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "MAC mismatch on packet %u",
                       (unsigned)c->seq_in);
            return ESP_ERR_INVALID_MAC;
        }

        /* Authenticated: only now is it safe to decrypt in place. */
        size_t produced = 0;
        if (psa_cipher_update(&c->rx.cipher, c->in_buf, packet_len,
                              c->in_buf, packet_len,
                              &produced) != PSA_SUCCESS ||
            produced != packet_len) {
            return ESP_FAIL;
        }
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

/*
 * Everything here builds in c->tx_frame and steps c->tx.cipher, which is a
 * stream cipher: two writers at once corrupt both, and the peer sees a MAC it
 * cannot verify. Callers hold the channel's tx_lock for that reason.
 */
esp_err_t ssh_packet_write(ssh_conn_t *c, ssh_buf_t *b)
{

    if (b->bad) {
        /* A writer that overflowed would otherwise emit a truncated packet,
         * which the peer would read as a protocol error at a confusing point. */
        espix_klog(ESPIX_KLOG_ERROR, TAG, "refusing to send overflowed packet");
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t blk = block_size(&c->tx);

    /*
     * RFC 4253 §6 requires
     *     packet_length ‖ padding_length ‖ payload ‖ padding
     * to be a multiple of the block size — the 4-byte length field counts,
     * even though packet_length itself does not include it. Padding only the
     * last three leaves every packet 4 bytes off, which a client reports as
     * "padding error: need N block 8 mod 4".
     *
     * With ETM the length is *not* encrypted, so it is excluded and only the
     * encrypted run has to align.
     */
    const size_t framed = aligned_len(&c->tx, 1 + b->len);
    size_t padding = blk - (framed % blk);
    if (padding < MIN_PADDING) {
        padding += blk;
    }

    const size_t packet_len = 1 + b->len + padding;

    /*
     * Check the invariant here rather than let the peer discover it: a framing
     * error surfaces at the far end as a corrupt packet or a MAC failure, a
     * long way from the arithmetic that caused it.
     *
     * The tx_frame bound is load-bearing, not merely a sanity check: the whole
     * packet is assembled contiguously in tx_frame and sent in one call, so
     * this is what guarantees the MAC has somewhere to go at
     * tx_frame + 8 + packet_len.
     */
    const size_t aligned = aligned_len(&c->tx, packet_len);
    if ((aligned % blk) != 0 || padding > 255 ||
        packet_len + SSH_MAC_LEN + 8 > sizeof(c->tx_frame)) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "framing bug: payload %u padding %u aligned %u blk %u",
                   (unsigned)b->len, (unsigned)padding, (unsigned)aligned,
                   (unsigned)blk);
        return ESP_FAIL;
    }

    uint8_t len_be[4];
    len_be[0] = (uint8_t)(packet_len >> 24);
    len_be[1] = (uint8_t)(packet_len >> 16);
    len_be[2] = (uint8_t)(packet_len >> 8);
    len_be[3] = (uint8_t)packet_len;

    if (!c->tx.active) {
        /* Assembled and sent as one packet, like the encrypted branch below.
         * Handshake volume makes the syscall count irrelevant here; having one
         * shape in this function rather than two is the point. */
        uint8_t *out = c->tx_frame + 4;         /* length ‖ packet, contiguous */
        memcpy(out, len_be, 4);
        out[4] = (uint8_t)padding;
        memcpy(out + 5, b->buf, b->len);
        memset(out + 5 + b->len, 0, padding);   /* zero padding is legal in the clear */

        if (write_all(c->fd, out, 4 + packet_len) != ESP_OK) {
            return ESP_FAIL;
        }
        c->seq_out++;
        return ESP_OK;
    }

    /*
     * Encrypted: assemble padding_length ‖ payload ‖ padding, encrypt it, then
     * MAC over seq ‖ length ‖ ciphertext. Padding must be random now — it is
     * the only unpredictable material in a short packet.
     */
    uint8_t *plain = c->tx_frame + 8;
    plain[0] = (uint8_t)padding;
    memcpy(plain + 1, b->buf, b->len);
    if (psa_generate_random(plain + 1 + b->len, padding) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    size_t produced = 0;
    if (psa_cipher_update(&c->tx.cipher, plain, packet_len, plain, packet_len,
                          &produced) != PSA_SUCCESS || produced != packet_len) {
        return ESP_FAIL;
    }

    c->tx_frame[0] = (uint8_t)(c->seq_out >> 24);
    c->tx_frame[1] = (uint8_t)(c->seq_out >> 16);
    c->tx_frame[2] = (uint8_t)(c->seq_out >> 8);
    c->tx_frame[3] = (uint8_t)c->seq_out;
    memcpy(c->tx_frame + 4, len_be, 4);

    /*
     * The MAC goes straight after the ciphertext, which is where the wire wants
     * it: tx_frame now holds seq ‖ length ‖ ciphertext ‖ mac, and everything
     * from offset 4 onwards is the packet exactly as it is sent. The framing
     * check above reserved this room.
     */
    mac_parts(&c->tx, c->tx_frame, 8 + packet_len, NULL, 0, NULL, 0,
              c->tx_frame + 8 + packet_len);

    /*
     * One send, not three. Each is a mailbox round-trip and a context switch
     * into lwIP's tcpip thread, and TCP_NODELAY is set, so sending the length,
     * ciphertext and MAC separately could put a 60-byte line of app output on
     * the wire as three segments carrying 4, 80 and 32 bytes.
     */
    if (write_all(c->fd, c->tx_frame + 4,
                  4 + packet_len + SSH_MAC_LEN) != ESP_OK) {
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
