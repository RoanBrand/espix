/*
 * SSH listener and connection lifecycle.
 *
 * One connection at a time by design for this milestone: the shell's dispatch
 * is already reentrant, but memory and failure isolation are worth proving with
 * a single session before multiplying them.
 */

#include <errno.h>
#include <stdio.h>      /* snprintf, for the refusal line */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>   /* struct timeval, for SO_RCVTIMEO and SO_SNDTIMEO */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_kernel.h"
#include "espix_ssh.h"
#include "sdkconfig.h"
#include "ssh_priv.h"

#define TAG "sshd"

#define LISTEN_BACKLOG 1

static int                s_listen_fd = -1;
static espix_ssh_status_t s_status;

/* ------------------------------------------------------------------ */
/* KEXINIT — what we are willing to speak                              */
/* ------------------------------------------------------------------ */

/*
 * Exactly one algorithm per role. A single choice means no negotiation logic to
 * get wrong, and every one of these is backed by PSA Crypto, which is already
 * linked for TLS.
 */
/*
 * The strict-KEX marker is advertised as a pseudo "algorithm" alongside the
 * real one — that is how OpenSSH signals it. It enables the Terrapin
 * (CVE-2023-48795) mitigation: sequence numbers reset after NEWKEYS, so an
 * attacker cannot delete packets from the unauthenticated prefix undetected.
 * Terrapin attacks exactly the packet layer this file implements, so it is
 * worth the handful of lines.
 */
static const char *const KEX_ALGS      = "curve25519-sha256,"
                                         "kex-strict-s-v00@openssh.com";
#define KEX_ALG_PRIMARY  "curve25519-sha256"
#define KEX_STRICT_CLIENT "kex-strict-c-v00@openssh.com"
static const char *const HOSTKEY_ALGS  = "ecdsa-sha2-nistp256";
static const char *const CIPHER_ALGS   = "aes256-ctr";
static const char *const MAC_ALGS      = "hmac-sha2-256-etm@openssh.com";
static const char *const COMP_ALGS     = "none";

static esp_err_t send_kexinit(ssh_conn_t *c)
{
    ssh_buf_t b;
    ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));

    uint8_t cookie[16];
    /* esp_fill_random rather than PSA here: this is a nonce with no secrecy
     * requirement, and it avoids needing psa_crypto_init() before the banner. */
    esp_fill_random(cookie, sizeof(cookie));

    ssh_put_u8(&b, SSH_MSG_KEXINIT);
    ssh_put_raw(&b, cookie, sizeof(cookie));
    ssh_put_cstr(&b, KEX_ALGS);
    ssh_put_cstr(&b, HOSTKEY_ALGS);
    ssh_put_cstr(&b, CIPHER_ALGS);      /* client to server */
    ssh_put_cstr(&b, CIPHER_ALGS);      /* server to client */
    ssh_put_cstr(&b, MAC_ALGS);
    ssh_put_cstr(&b, MAC_ALGS);
    ssh_put_cstr(&b, COMP_ALGS);
    ssh_put_cstr(&b, COMP_ALGS);
    ssh_put_cstr(&b, "");               /* languages, client to server */
    ssh_put_cstr(&b, "");               /* languages, server to client */
    ssh_put_u8(&b, 0);                  /* no guessed KEX packet follows */
    ssh_put_u32(&b, 0);                 /* reserved */

    if (b.bad || b.len > sizeof(c->kexinit_s)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Kept because the exchange hash covers it verbatim. */
    memcpy(c->kexinit_s, b.buf, b.len);
    c->kexinit_s_len = b.len;

    return ssh_packet_write(c, &b);
}

/* Does `list`, a comma-separated name-list, contain `want`? */
static bool name_list_has(const uint8_t *list, size_t len, const char *want)
{
    const size_t want_len = strlen(want);
    size_t       start = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i == len || list[i] == ',') {
            if (i - start == want_len &&
                memcmp(list + start, want, want_len) == 0) {
                return true;
            }
            start = i + 1;
        }
    }
    return false;
}

/*
 * The same heap the connection itself comes from — see conn_alloc(). PSRAM when
 * the build asks for it, falling back to internal so a KEXINIT cannot fail to
 * allocate merely because PSRAM is full.
 */
static void *conn_mem(size_t n)
{
#if CONFIG_ESPIX_SSH_CONN_IN_PSRAM
    /*
     * Aligned and DMA-capable, because mbedtls drives SHA and AES by DMA over
     * buffers inside this allocation. ESP-IDF requires MALLOC_CAP_DMA for a DMA
     * buffer in external RAM, and warns that synchronising the cache for an
     * unaligned region can silently corrupt memory around it. The struct's
     * crypto buffers are cache-line aligned within it (see ssh_priv.h); this is
     * the other half, aligning the base so those offsets land where they claim.
     */
    void *p = heap_caps_aligned_alloc(SSH_DMA_ALIGN, n,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT |
                                      MALLOC_CAP_DMA);
    if (p != NULL) {
        return p;
    }
#endif
    /* Internal RAM is DMA-capable and cache-coherent, so the fallback needs
     * only the alignment. */
    return heap_caps_aligned_alloc(SSH_DMA_ALIGN, n,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* Idempotent, so the teardown path can call it without knowing whether KEX got
 * far enough to allocate or far enough to have already released. */
static void kexinit_c_release(ssh_conn_t *c)
{
    free(c->kexinit_c);
    c->kexinit_c     = NULL;
    c->kexinit_c_len = 0;
}

static esp_err_t recv_kexinit(ssh_conn_t *c)
{
    if (ssh_packet_read(c) != ESP_OK) {
        return ESP_FAIL;
    }
    if (c->in_payload[0] != SSH_MSG_KEXINIT) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "expected KEXINIT, got msg %u",
                   c->in_payload[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * Kept verbatim for the exchange hash, before any parsing consumes it, and
     * released as soon as KEX is over. There is no size of our own to check:
     * ssh_packet_read() has already refused anything that would not fit the
     * buffer this arrived in.
     */
    kexinit_c_release(c);
    c->kexinit_c = conn_mem(c->in_len);
    if (c->kexinit_c == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "no memory for client KEXINIT (%u bytes)",
                   (unsigned)c->in_len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(c->kexinit_c, c->in_payload, c->in_len);
    c->kexinit_c_len = c->in_len;

    ssh_buf_t b;
    ssh_buf_read_from(&b, c->in_payload, c->in_len);
    ssh_skip(&b, 1);        /* message id */
    ssh_skip(&b, 16);       /* cookie */

    struct { const char *what; const char *need; } checks[] = {
        { "kex",      KEX_ALG_PRIMARY },
        { "host key", HOSTKEY_ALGS },
        { "cipher",   CIPHER_ALGS },
        { "cipher",   CIPHER_ALGS },
        { "mac",      MAC_ALGS },
        { "mac",      MAC_ALGS },
    };

    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        size_t         len = 0;
        const uint8_t *list = ssh_get_string(&b, &len);
        if (b.bad) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (i == 0 && name_list_has(list, len, KEX_STRICT_CLIENT)) {
            c->strict_kex = true;
        }
        if (!name_list_has(list, len, checks[i].need)) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "client offers no %s we support (need %s)",
                       checks[i].what, checks[i].need);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "algorithms agreed%s",
               c->strict_kex ? ", strict kex" : "");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

/*
 * PSRAM only when asked for: this holds the cipher state and both packet
 * buffers. See CONFIG_ESPIX_SSH_CONN_IN_PSRAM.
 *
 * A note here used to reason that external memory was safe because "the AES
 * driver bounces external memory through an internal buffer before using it".
 * That is true of AES and not of SHA: esp_sha_dma_process() takes the pointer
 * it is given and calls esp_cache_msync() on it, which ESP-IDF documents as
 * able to "silently corrupt the memory" when the region is unaligned -- and a
 * buffer at an arbitrary offset inside a calloc'd struct always is. Rounding a
 * write-back out to whole cache lines can then carry neighbouring heap memory
 * with it.
 *
 * So the allocation is cache-line aligned and DMA-capable, and the buffers are
 * aligned within the struct (ssh_priv.h). Both halves are needed: aligning the
 * members means nothing if the base is off a line, and aligning the base means
 * nothing if the members sit at odd offsets from it.
 */
static ssh_conn_t *conn_alloc(void)
{
    ssh_conn_t *c = NULL;

#if CONFIG_ESPIX_SSH_CONN_IN_PSRAM
    c = heap_caps_aligned_alloc(SSH_DMA_ALIGN, sizeof(ssh_conn_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT |
                                MALLOC_CAP_DMA);
#endif
    if (c == NULL) {
        /* Internal RAM is DMA-capable and cache-coherent, so it needs only the
         * alignment. */
        c = heap_caps_aligned_alloc(SSH_DMA_ALIGN, sizeof(ssh_conn_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (c != NULL) {
        /* No aligned calloc, so zero it here: the code below relies on a clean
         * struct, and a stale one would look like a half-open connection. */
        memset(c, 0, sizeof(*c));
    }
    return c;
}

/*
 * How long to wait for the peer to hang up before hanging up on it. A client
 * that is answering takes milliseconds; one that has wandered off costs us this
 * much and no more.
 */
#define LINGER_DRAIN_MS 600

/*
 * Say goodbye, then let the client hang up first.
 *
 * Closing the socket the moment the channel is done is what made roughly two
 * connections in five print "Connection closed by remote host", and, when the
 * close overtook the last packets, report 255 for a command that had succeeded.
 * The server log looked identical either way -- every one of those connections
 * reached "connection closed" normally -- because nothing was wrong with the
 * session. It was only the goodbye.
 *
 * The refusal path in accept_task() needs this too: a reset there discards the
 * "too many connections" line before the client can read it, which is how that
 * message came to be written and never once seen.
 *
 * The fix is to let the client hang up first. For `ssh host <cmd>` the client
 * drives the teardown: having taken exit-status and CHANNEL_CLOSE it sends its
 * own close and a DISCONNECT, then goes away. Reading until recv() returns 0
 * waits for exactly that, and empties the receive buffer on the way -- close()
 * on a socket with data still unread sends an RST rather than a FIN, which
 * would put us back where we started.
 *
 * Sending SSH_MSG_DISCONNECT ourselves was tried and is worse: it arrives while
 * the client is still finishing the channel, which aborts it, and turned two
 * connections in five into seven in fifteen reporting failure for a command
 * that had worked.
 */
static void close_gracefully(int fd)
{
    const int64_t deadline = esp_timer_get_time() + LINGER_DRAIN_MS * 1000;
    char          scratch[64];

    while (esp_timer_get_time() < deadline) {
        const ssize_t n = recv(fd, scratch, sizeof(scratch), 0);
        if (n == 0) {
            break;                  /* peer hung up: nothing left unread */
        }
        if (n < 0 && errno != EINTR) {
            break;                  /* timed out or failed; stop either way */
        }
    }

    close(fd);
}

/*
 * The session count moves under a spinlock: the accept task takes slots and
 * every connection task releases one as it exits, on either core.
 */
static portMUX_TYPE s_sessions_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * Take a slot, or report that the limit is reached.
 *
 * The test and the increment happen together, and in the accept loop rather
 * than in the connection task, because they used to be neither. The check was
 * here and the increment was in connection_task() after conn_alloc(), so
 * between xTaskCreate() returning and the new task first running the counter
 * had not moved -- and every connection arriving in that window was admitted
 * against a stale number.
 *
 * One client at a time never opened that window, which is why it survived this
 * long. The parallel test harness opens a connection per worker at once on
 * every run, and each overshoot costs 8KB of internal task stack that, at the
 * upper end of the range, is not there.
 */
static bool sessions_take(void)
{
    bool taken;

    portENTER_CRITICAL(&s_sessions_mux);
    taken = (s_status.sessions < CONFIG_ESPIX_SSH_MAX_SESSIONS);
    if (taken) {
        s_status.sessions++;
    }
    portEXIT_CRITICAL(&s_sessions_mux);

    return taken;
}

static void sessions_release(void)
{
    portENTER_CRITICAL(&s_sessions_mux);
    if (s_status.sessions > 0) {
        s_status.sessions--;
    }
    portEXIT_CRITICAL(&s_sessions_mux);
}

/* Runs with a slot already taken by the accept loop; it releases it on every
 * exit path, including the early one. */
static void connection_task(void *arg)
{
    const int fd = (int)(intptr_t)arg;

    /* Heap, not stack: two 4KB packet buffers plus state would need a task
     * stack far larger than the protocol logic itself warrants. */
    ssh_conn_t *c = conn_alloc();
    if (c == NULL) {
        /*
         * Say why, for the same reason the session limit does.
         *
         * Closing without a word makes the client print "Connection closed by
         * remote host", which is indistinguishable from a crash, a hang or a
         * truncated command -- an ambiguity that has already cost this project
         * debugging time once, which is why the refusal above sends a line.
         * Out of memory is if anything more confusing to meet in silence: the
         * device is up, answering, and simply cannot afford another session.
         *
         * RFC 4253 4.2 lets a server send CRLF-terminated lines before its
         * version string and requires clients to cope; the one rule is that
         * such a line must not begin with "SSH-". The free figure is included
         * because it is the number that decides whether to wait and retry or
         * to go and look at what is holding memory.
         */
        char      msg[96];
        const int n = snprintf(msg, sizeof(msg),
                               "espix: out of memory for another connection "
                               "(%u KB internal free)\r\n",
                               (unsigned)(heap_caps_get_free_size(
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
        if (n > 0) {
            (void)send(fd, msg, (size_t)n, 0);
        }

        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "no memory for a connection; %u KB internal free",
                   (unsigned)(heap_caps_get_free_size(
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));

        close_gracefully(fd);   /* or the reset discards the line just sent */
        sessions_release();
        vTaskDelete(NULL);
        return;
    }
    c->fd = fd;

    s_status.accepted++;

    do {
        if (ssh_transport_banner(c) != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "version exchange failed");
            break;
        }
        if (send_kexinit(c) != ESP_OK) {
            break;
        }
        const esp_err_t kex_err = recv_kexinit(c);
        if (kex_err != ESP_OK) {
            /*
             * Say which one. Every failure here used to report "no common
             * algorithm", so an oversized KEXINIT from a newer OpenSSH was
             * indistinguishable from a genuine cipher mismatch — and got
             * diagnosed as one, slowly, by pinning algorithms until it worked.
             * recv_kexinit() has already logged the detail locally; this is
             * what the client is told.
             */
            const char *why = "no common algorithm";
            if (kex_err == ESP_ERR_NO_MEM) {
                why = "out of memory for KEXINIT";
            } else if (kex_err == ESP_ERR_INVALID_SIZE) {
                why = "malformed KEXINIT";
            } else if (kex_err == ESP_ERR_INVALID_RESPONSE) {
                why = "expected KEXINIT";
            }
            ssh_send_disconnect(c, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, why);
            break;
        }

        if (ssh_packet_read(c) != ESP_OK ||
            c->in_payload[0] != SSH_MSG_KEX_ECDH_INIT) {
            ssh_send_disconnect(c, SSH_DISCONNECT_PROTOCOL_ERROR,
                                "expected KEX_ECDH_INIT");
            break;
        }
        if (ssh_kex_run(c) != ESP_OK) {
            ssh_send_disconnect(c, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                                "key exchange failed");
            break;
        }

        /* ssh_kex_run() is the last reader. What follows — authentication, then
         * a shell that owns the connection until logout — has no use for it. */
        kexinit_c_release(c);

        if (ssh_auth_run(c) != ESP_OK) {
            /* ssh_auth_run has already sent whatever disconnect is warranted;
             * saying more here would be a second, conflicting reason. */
            break;
        }

        /* Authenticated: hand the connection to the session channel, which
         * owns it until the user logs out. */
        ssh_channel_run(c);
    } while (0);

    close_gracefully(c->fd);
    kexinit_c_release(c);   /* no-op on the happy path, which released earlier */
    ssh_kex_release_keys(c);/* the PSA slots free(c) cannot reach */
    free(c);

    sessions_release();
    espix_klog(ESPIX_KLOG_INFO, TAG, "connection closed");

    vTaskDelete(NULL);
}

static void accept_task(void *arg)
{
    (void)arg;

    for (;;) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);

        const int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            /* A transient accept error must not kill the listener; a permanent
             * one would otherwise spin, hence the pause. */
            espix_klog(ESPIX_KLOG_WARN, TAG, "accept failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Disable Nagle: a shell echoes single keystrokes, and coalescing them
         * makes typing feel broken. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        /*
         * Set before the session limit is checked, not after: the refusal
         * path below reads from this socket too, and without a timeout that
         * read blocks forever -- in the accept loop, which stops the server
         * answering anything at all.
         *
         * A receive timeout, so no read can block this connection's task
         * forever. read_exact() turns it into "wait indefinitely" between
         * packets and "give up eventually" partway through one -- see the note
         * there for why those must differ.
         *
         * Without it a peer that sent half a packet and stopped parked the task
         * permanently. The task never exited, so s_status.sessions never came
         * back down, and enough such connections made the server refuse
         * everything thereafter. It is also reachable by anything that can open
         * a socket to port 22, which makes it a denial of service rather than
         * only a bug.
         */
        const struct timeval io_timeout = {
            .tv_sec  = 0,
            .tv_usec = 250 * 1000,
        };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));

        /*
         * And the same in the other direction, for the same reason -- which was
         * missed the first time round, so the paragraph above stayed true of
         * sends long after it stopped being true of reads.
         *
         * A peer that stops reading and does *not* close parks this task inside
         * send() for as long as it stays silent: the window shuts, lwIP has
         * nowhere to put the bytes, and a blocking socket simply waits. Enough
         * such peers exhaust CONFIG_ESPIX_SSH_MAX_SESSIONS, which is the same
         * denial of service the receive timeout was added to close. (A peer
         * that stops reading and then *closes* was always handled: the send
         * fails and teardown runs.)
         *
         * This is also what makes write_all()'s EAGAIN branch reachable. On a
         * blocking socket send() never returns EAGAIN, so that retry -- and the
         * BLOCKED_WRITE_TIMEOUT_MS clock it keeps -- was dead code, and an
         * earlier investigation's "zero EAGAIN on every connection" was true
         * and meaningless. With a send timeout the branch does its intended
         * job: retry while the peer is merely slow, give up when it is gone.
         */
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

        if (!sessions_take()) {
            /*
             * Refusing cleanly beats letting a connection half-work. The limit
             * is above one because scp and sftp open their own connection, and
             * transferring a file while a shell is open is the normal case.
             *
             * The count in the message is what the test harness reads to learn
             * the device's real limit, rather than trusting a sdkconfig that
             * may predate the Kconfig default -- so keep the "(N of M in use)"
             * shape.
             *
             * Say why. RFC 4253 4.2 lets a server send arbitrary CRLF-terminated
             * lines before its version string, and requires clients to cope with
             * them -- it is how OpenSSH reports "Exceeded MaxStartups". The one
             * rule is that such a line must not begin with "SSH-".
             *
             * Closing without a word is what this used to do, and the client
             * then prints "Connection closed by remote host", which is
             * indistinguishable from a crash, a hang, or a truncated command.
             * That ambiguity cost real debugging time: refused connections were
             * being read as truncated ones.
             */
            char msg[80];
            const int n = snprintf(msg, sizeof(msg),
                                   "espix: too many connections (%d of %d in use)\r\n",
                                   s_status.sessions,
                                   CONFIG_ESPIX_SSH_MAX_SESSIONS);
            if (n > 0) {
                (void)send(fd, msg, (size_t)n, 0);
            }

            espix_klog(ESPIX_KLOG_WARN, TAG, "refusing connection: %d already open",
                       s_status.sessions);
            close_gracefully(fd);   /* or the reset discards the line just sent */
            continue;
        }

        espix_klog(ESPIX_KLOG_INFO, TAG, "connection from %s",
                   inet_ntoa(peer.sin_addr));

        if (xTaskCreate(connection_task, "sshd:conn",
                        CONFIG_ESPIX_SSH_TASK_STACK, (void *)(intptr_t)fd,
                        CONFIG_ESPIX_SSH_TASK_PRIO, NULL) != pdPASS) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot start connection task");
            sessions_release();     /* the task that would have done this never ran */
            close(fd);
        }
    }
}

esp_err_t espix_ssh_start(void)
{
    if (s_listen_fd >= 0) {
        return ESP_OK;
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        return ESP_FAIL;
    }

    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    const struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(CONFIG_ESPIX_SSH_PORT),
    };

    if (bind(s_listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s_listen_fd, LISTEN_BACKLOG) != 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot listen on port %d: %d",
                   CONFIG_ESPIX_SSH_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(accept_task, "sshd", 3072, NULL,
                    CONFIG_ESPIX_SSH_TASK_PRIO, NULL) != pdPASS) {
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_ERR_NO_MEM;
    }

    if (ssh_hostkey_init() != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no host key; not accepting connections");
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    s_status.running = true;
    s_status.port    = CONFIG_ESPIX_SSH_PORT;

    espix_klog(ESPIX_KLOG_INFO, TAG, "listening on port %d",
               CONFIG_ESPIX_SSH_PORT);
    return ESP_OK;
}

void espix_ssh_status(espix_ssh_status_t *out)
{
    if (out != NULL) {
        *out = s_status;
    }
}

void ssh_server_note_rejection(void)
{
    s_status.rejected++;
}
