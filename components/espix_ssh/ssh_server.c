/*
 * SSH listener and connection lifecycle.
 *
 * One connection at a time by design for this milestone: the shell's dispatch
 * is already reentrant, but memory and failure isolation are worth proving with
 * a single session before multiplying them.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    /* Kept verbatim for the exchange hash, before any parsing consumes it. */
    if (c->in_len > sizeof(c->kexinit_c)) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "client KEXINIT too large (%u)",
                   (unsigned)c->in_len);
        return ESP_ERR_INVALID_SIZE;
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

static void connection_task(void *arg)
{
    const int fd = (int)(intptr_t)arg;

    /* Heap, not stack: two 4KB packet buffers plus state would need a task
     * stack far larger than the protocol logic itself warrants. */
    ssh_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    c->fd = fd;

    s_status.sessions++;
    s_status.accepted++;

    do {
        if (ssh_transport_banner(c) != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "version exchange failed");
            break;
        }
        if (send_kexinit(c) != ESP_OK) {
            break;
        }
        if (recv_kexinit(c) != ESP_OK) {
            ssh_send_disconnect(c, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                                "no common algorithm");
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

        if (ssh_auth_run(c) != ESP_OK) {
            /* ssh_auth_run has already sent whatever disconnect is warranted;
             * saying more here would be a second, conflicting reason. */
            break;
        }

        /* Authenticated: hand the connection to the session channel, which
         * owns it until the user logs out. */
        ssh_channel_run(c);
    } while (0);

    close(c->fd);
    free(c);

    s_status.sessions--;
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

        if (s_status.sessions >= 1) {
            /* One at a time for this milestone. Refusing cleanly beats letting
             * a second connection half-work. */
            espix_klog(ESPIX_KLOG_WARN, TAG, "refusing second connection");
            close(fd);
            continue;
        }

        /* Disable Nagle: a shell echoes single keystrokes, and coalescing them
         * makes typing feel broken. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        espix_klog(ESPIX_KLOG_INFO, TAG, "connection from %s",
                   inet_ntoa(peer.sin_addr));

        if (xTaskCreate(connection_task, "sshd:conn",
                        CONFIG_ESPIX_SSH_TASK_STACK, (void *)(intptr_t)fd,
                        CONFIG_ESPIX_SSH_TASK_PRIO, NULL) != pdPASS) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot start connection task");
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
