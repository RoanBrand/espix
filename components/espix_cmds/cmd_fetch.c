/*
 * fetch: pull a file from an HTTP(S) URL into the filesystem.
 *
 * This exists because a game's data is not the kernel's business. espix ships
 * the program; the data a program needs is fetched once, by the user, into the
 * rootfs. The HTTP and TLS stack is the one espix already links for `upgrade`
 * -- esp_http_client over mbedtls -- so a second client would be a second TLS
 * stack in RAM for no gain, and this costs nothing when it is not running.
 *
 *   fetch <url> <path>
 *
 * Room is checked before a byte is written and again against the response's
 * Content-Length, because a 4 MB download into a 3 MB hole otherwise fails at
 * the worst possible moment. When there is not room, the message says where to
 * get some back: on this system the usual answer is an old kernel still
 * sitting in /boot.
 *
 * The bytes land in <path>.part and are renamed on success, so an interrupted
 * fetch never leaves a half file that looks whole.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "espix_cmds.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"

#define TAG "fetch"

/* Leave the filesystem some room: one that writes its last byte cannot write
 * anything else, including the config that would tidy up next boot. */
#define FETCH_RESERVE (64 * 1024)

typedef struct {
    FILE  *out;
    size_t written;
    size_t limit;      /* bytes we may write */
    bool   too_big;
} fetch_sink_t;

static esp_err_t fetch_event(esp_http_client_event_t *evt)
{
    fetch_sink_t *sink = evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL &&
        strcasecmp(evt->header_key, "Content-Length") == 0) {
        const size_t len = (size_t)strtoul(evt->header_value, NULL, 10);
        if (len > sink->limit) {
            sink->too_big = true;
            return ESP_FAIL;        /* aborts the transfer */
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (sink->written + (size_t)evt->data_len > sink->limit) {
            sink->too_big = true;
            return ESP_FAIL;
        }
        if (fwrite(evt->data, 1, (size_t)evt->data_len, sink->out) !=
            (size_t)evt->data_len) {
            return ESP_FAIL;
        }
        sink->written += (size_t)evt->data_len;
    }
    return ESP_OK;
}

/* The one message that matters, said the same way wherever we run out. */
static void fetch_no_room(espix_session_t *s, size_t need, size_t free_now)
{
    espix_eprintf(s, "fetch: %u bytes needed, %u free on /\n",
                  (unsigned)need, (unsigned)free_now);
    espix_eprintf(s, "fetch: free some space first. An old kernel in /boot is\n"
                     "       the usual candidate: `ls /boot`, then rm one you are\n"
                     "       not running.\n");
}

int cmd_fetch(espix_session_t *s, int argc, char **argv)
{
    if (argc < 3) {
        espix_eprintf(s, "usage: fetch <url> <path>\n");
        return 1;
    }

    const char *url  = argv[1];
    const char *path = argv[2];

    espix_fs_info_t info;
    if (espix_fs_stat_root(&info) != ESP_OK) {
        espix_eprintf(s, "fetch: cannot read the filesystem\n");
        return 1;
    }
    const size_t free_now = info.total_bytes - info.used_bytes;
    const size_t limit    = free_now > FETCH_RESERVE ? free_now - FETCH_RESERVE : 0;

    char part[256];
    if (snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part)) {
        espix_eprintf(s, "fetch: path too long\n");
        return 1;
    }

    FILE *out = fopen(part, "wb");
    if (out == NULL) {
        espix_eprintf(s, "fetch: cannot write %s\n", part);
        return 1;
    }

    fetch_sink_t sink = { .out = out, .written = 0, .limit = limit };

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = fetch_event,
        .user_data         = &sink,
        .timeout_ms        = 30000,
        .keep_alive_enable = false,
        /* The same trust store `upgrade` uses -- one bundle, not two. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* esp_http_client follows 3xx itself; a raw file host redirects. */
        .disable_auto_redirect = false,
    };

    espix_printf(s, "fetch: %s\n", url);

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        fclose(out);
        remove(part);
        espix_eprintf(s, "fetch: no client (no network?)\n");
        return 1;
    }

    const esp_err_t err = esp_http_client_perform(c);
    const int       code = esp_http_client_get_status_code(c);

    esp_http_client_cleanup(c);
    fclose(out);

    if (sink.too_big) {
        remove(part);
        fetch_no_room(s, sink.written, free_now);
        return 1;
    }
    if (err != ESP_OK || code != 200 || sink.written == 0) {
        remove(part);
        espix_eprintf(s, "fetch: failed (%s, HTTP %d)\n", esp_err_to_name(err), code);
        return 1;
    }
    if (rename(part, path) != 0) {
        remove(part);
        espix_eprintf(s, "fetch: cannot rename into place\n");
        return 1;
    }

    espix_printf(s, "fetch: %u bytes -> %s\n", (unsigned)sink.written, path);
    return 0;
}
