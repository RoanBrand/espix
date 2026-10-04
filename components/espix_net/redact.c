/*
 * A URL as it is safe to log.
 *
 * fetch logs what it was asked to get, and a ddns update URL carries the
 * password in its query string -- so the log carried it every fifteen minutes,
 * to the klog and the console, which is where a password stops being one. This
 * takes the secrets out and leaves the rest readable, so the line still says
 * what was fetched.
 *
 * Two shapes carry one: a query parameter whose name says it is a secret, and
 * the userinfo of an authority (scheme://user:password@host), where the
 * password is what follows the colon.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "espix_net.h"

static const char *const s_secret_keys[] = {
    "password=", "passwd=", "pwd=", "token=", "apikey=", "api_key=",
    "secret=", "key=",
};

static size_t copy_secret(char *out, size_t o, size_t len)
{
    if (o + 3 < len) {
        out[o++] = '*';
        out[o++] = '*';
        out[o++] = '*';
    }
    return o;
}

size_t espix_net_redact_url(const char *url, char *out, size_t len)
{
    size_t      o = 0;
    const char *p = url;

    if (url == NULL || out == NULL || len == 0) {
        return 0;
    }

    while (*p != 0 && o + 1 < len) {
        /* An authority: mask the password in user:password@host. */
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
            out[o++] = *p++;
            out[o++] = *p++;
            out[o++] = *p++;

            const char *at    = NULL;
            const char *colon = NULL;
            for (const char *q = p; *q != 0 && *q != '/'; q++) {
                if (*q == '@') {
                    at = q;
                    break;
                }
            }
            if (at != NULL) {
                for (const char *q = p; q < at; q++) {
                    if (*q == ':') {
                        colon = q;
                        break;
                    }
                }
            }
            if (colon != NULL) {
                while (p <= colon && o + 1 < len) {
                    out[o++] = *p++;
                }
                p = at;                         /* the password is dropped */
                o = copy_secret(out, o, len);
            }
            continue;
        }

        /* A query parameter whose name says it holds one. */
        const bool boundary = (p == url) || p[-1] == '?' || p[-1] == '&';
        bool       masked   = false;
        if (boundary) {
            for (size_t k = 0; k < sizeof(s_secret_keys) / sizeof(s_secret_keys[0]); k++) {
                const size_t kl = strlen(s_secret_keys[k]);
                if (strncmp(p, s_secret_keys[k], kl) != 0) {
                    continue;
                }
                for (size_t j = 0; j < kl && o + 1 < len; j++) {
                    out[o++] = p[j];
                }
                p += kl;
                while (*p != 0 && *p != '&') {
                    p++;                        /* the value is dropped */
                }
                o     = copy_secret(out, o, len);
                masked = true;
                break;
            }
        }
        if (masked) {
            continue;
        }

        out[o++] = *p++;
    }

    out[o] = 0;
    return o;
}
