/*
 * SSH user authentication (RFC 4252), password method only.
 *
 * The shape of the exchange is dictated by how clients behave rather than by
 * what a server would design: OpenSSH probes with "none" to discover which
 * methods exist, then tries every public key it has loaded, and only then asks
 * the user for a password. All of those arrive as USERAUTH_REQUEST and all but
 * the last must be refused with a FAILURE that advertises "password" — refusing
 * without advertising leaves the client with nothing to try.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_auth.h"
#include "espix_kernel.h"
#include "ssh_priv.h"

#define TAG "sshauth"

#define SERVICE_USERAUTH   "ssh-userauth"
#define SERVICE_CONNECTION "ssh-connection"

/* Methods we will accept. Advertised in every FAILURE so the client knows what
 * to fall back to. */
#define METHODS "password"

/*
 * Password attempts before the connection is dropped. OpenSSH's client offers
 * three prompts by default, so matching that avoids the client appearing to
 * hang up on itself mid-prompt.
 */
#define MAX_PASSWORD_TRIES 3

/*
 * Total requests, including the "none" probe and every public key offered.
 * A client with several keys legitimately sends a handful; anything far beyond
 * that is not a real client and should not be able to keep a task busy.
 */
#define MAX_REQUESTS 24

/* Delay after a wrong password. Not a real defence — an attacker can open a new
 * connection — but it takes online guessing from thousands per second to a
 * handful, for one line and no complexity. */
#define FAIL_DELAY_MS 1000

static esp_err_t send_failure(ssh_conn_t *c)
{
    ssh_buf_t b;
    ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));

    ssh_put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
    ssh_put_cstr(&b, METHODS);
    ssh_put_u8(&b, 0);      /* not a partial success */

    return ssh_packet_write(c, &b);
}

static esp_err_t send_success(ssh_conn_t *c)
{
    ssh_buf_t b;
    ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
    ssh_put_u8(&b, SSH_MSG_USERAUTH_SUCCESS);
    return ssh_packet_write(c, &b);
}

/* SERVICE_REQUEST must name ssh-userauth before anything else may happen. */
static esp_err_t accept_service(ssh_conn_t *c)
{
    if (ssh_packet_read(c) != ESP_OK) {
        return ESP_FAIL;
    }
    if (c->in_payload[0] != SSH_MSG_SERVICE_REQUEST) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "expected SERVICE_REQUEST, got %u",
                   c->in_payload[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ssh_buf_t in;
    ssh_buf_read_from(&in, c->in_payload, c->in_len);
    ssh_skip(&in, 1);

    size_t         name_len = 0;
    const uint8_t *name = ssh_get_string(&in, &name_len);
    if (in.bad || name == NULL ||
        name_len != strlen(SERVICE_USERAUTH) ||
        memcmp(name, SERVICE_USERAUTH, name_len) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "unsupported service requested");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ssh_buf_t b;
    ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
    ssh_put_u8(&b, SSH_MSG_SERVICE_ACCEPT);
    ssh_put_cstr(&b, SERVICE_USERAUTH);
    return ssh_packet_write(c, &b);
}

esp_err_t ssh_auth_run(ssh_conn_t *c)
{
    if (accept_service(c) != ESP_OK) {
        return ESP_FAIL;
    }

    unsigned tries    = 0;
    unsigned requests = 0;

    while (requests++ < MAX_REQUESTS) {
        if (ssh_packet_read(c) != ESP_OK) {
            return ESP_FAIL;    /* client gave up */
        }

        const uint8_t msg = c->in_payload[0];

        /* Both are legal at any time and carry nothing we need. */
        if (msg == SSH_MSG_IGNORE || msg == SSH_MSG_DEBUG) {
            requests--;
            continue;
        }
        if (msg == SSH_MSG_DISCONNECT) {
            return ESP_FAIL;
        }
        if (msg != SSH_MSG_USERAUTH_REQUEST) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "unexpected message %u", msg);
            return ESP_ERR_INVALID_RESPONSE;
        }

        ssh_buf_t in;
        ssh_buf_read_from(&in, c->in_payload, c->in_len);
        ssh_skip(&in, 1);

        size_t         user_len = 0, svc_len = 0, method_len = 0;
        const uint8_t *user   = ssh_get_string(&in, &user_len);
        const uint8_t *svc    = ssh_get_string(&in, &svc_len);
        const uint8_t *method = ssh_get_string(&in, &method_len);

        if (in.bad || user == NULL || svc == NULL || method == NULL) {
            return ESP_ERR_INVALID_SIZE;
        }

        /* The only service worth authenticating for. */
        if (svc_len != strlen(SERVICE_CONNECTION) ||
            memcmp(svc, SERVICE_CONNECTION, svc_len) != 0) {
            ssh_send_disconnect(c, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE,
                                "unknown service");
            return ESP_ERR_NOT_SUPPORTED;
        }

        /* Copy the name before any further parsing; it points into in_buf,
         * which the reply will overwrite. Over-long names cannot match an
         * account, so they simply fail. */
        char name[sizeof(c->user)];
        if (user_len >= sizeof(name)) {
            send_failure(c);
            continue;
        }
        memcpy(name, user, user_len);
        name[user_len] = '\0';

        const bool is_password = (method_len == strlen("password") &&
                                  memcmp(method, "password", method_len) == 0);

        if (!is_password) {
            /* "none" probe, or a public key we cannot check. Refusing while
             * advertising password is what steers the client to prompt. */
            send_failure(c);
            continue;
        }

        /* RFC 4252 §8: boolean says whether a password *change* follows. We do
         * not support changing a password mid-login; `passwd` exists for that. */
        const bool changing = ssh_get_bool(&in);

        size_t         pw_len = 0;
        const uint8_t *pw = ssh_get_string(&in, &pw_len);
        if (in.bad || pw == NULL) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (changing) {
            send_failure(c);
            continue;
        }

        char password[ESPIX_PASSWORD_MAX];
        if (pw_len >= sizeof(password)) {
            /* Too long to be one of ours; treat as a failed attempt rather
             * than a protocol error. */
            tries++;
            send_failure(c);
            continue;
        }
        memcpy(password, pw, pw_len);
        password[pw_len] = '\0';

        const bool ok = espix_auth_verify(name, password);

        /* Clear it promptly: the packet buffer is reused, but this copy is
         * ours and would otherwise sit on the stack for the session's life. */
        memset(password, 0, sizeof(password));

        if (ok) {
            strlcpy(c->user, name, sizeof(c->user));
            espix_klog(ESPIX_KLOG_INFO, TAG, "%s authenticated", c->user);
            return send_success(c);
        }

        tries++;
        ssh_server_note_rejection();
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "failed password for %s (attempt %u of %d)",
                   name, tries, MAX_PASSWORD_TRIES);

        vTaskDelay(pdMS_TO_TICKS(FAIL_DELAY_MS));

        if (tries >= MAX_PASSWORD_TRIES) {
            ssh_send_disconnect(c, SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                                "too many authentication failures");
            return ESP_ERR_INVALID_STATE;
        }

        send_failure(c);
    }

    espix_klog(ESPIX_KLOG_WARN, TAG, "too many auth requests; dropping");
    ssh_send_disconnect(c, SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                        "too many requests");
    return ESP_ERR_INVALID_STATE;
}
