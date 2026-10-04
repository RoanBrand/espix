/*
 * Fetching a file over HTTP(S).
 *
 * One implementation, deliberately. `upgrade` installs a kernel image through
 * here and `fetch` downloads anything else, because underneath they are the same
 * operation with different trust afterwards: stream a body into a file, refuse
 * when it will not fit, verify it when the caller knows a digest, and rename it
 * into place so an interrupted transfer never leaves something that looks whole.
 * A second copy would be a second thing to fix the day the redirect or the TLS
 * rules change -- and it would be the copy nobody remembered to fix.
 *
 * It is also where the network policy lives. None of this is reachable from an
 * app: what a program needs off the network is declared in a manifest, and
 * espix_appdata_ensure() is what reads that, so a program still cannot open a
 * connection even though the thing that installed it can. The HTTP and TLS
 * stacks are the ones espix already links -- esp_http_client over mbedtls, with
 * the certificate bundle `upgrade` uses -- so a download costs RAM only while
 * it runs and nothing at all when it is idle.
 *
 * The space check is not tidiness. This filesystem also holds the kernel the
 * board booted from, and filling it does not fail cleanly at the end: it leaves
 * a filesystem with no room to write the config that would tidy up next boot.
 * So the room is measured before the first byte and again against the server's
 * Content-Length, and every caller is handed enough to say where room usually
 * comes from -- on this system, an old kernel still sitting in /boot.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "psa/crypto.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"

#define TAG "fetch"

/*
 * Leave the filesystem room to be a filesystem. One that writes its last byte
 * cannot write anything else, including the message that would have explained
 * what went wrong.
 */
#define FETCH_RESERVE (64 * 1024)

/* Hashing is streamed, so one chunk is enough and it comes from the heap: the
 * task that fetches may be an app's, with a stack that cannot spare 4 KB. */
#define HASH_CHUNK 4096

typedef struct {
    FILE                   *f;
    const char             *path;       /* for the progress reports */
    size_t                  done;
    size_t                  total;      /* Content-Length, 0 if it did not say */
    size_t                  limit;      /* bytes we may write */
    bool                    too_big;
    bool                    io_error;
    espix_fetch_progress_fn progress;
    void                   *ctx;
} fetch_sink_t;

static esp_err_t fetch_event(esp_http_client_event_t *evt)
{
    fetch_sink_t *s = evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL &&
        evt->header_value != NULL &&
        strcasecmp(evt->header_key, "Content-Length") == 0) {
        /*
         * Refused here rather than at the end, because the point of knowing the
         * size is not to report it -- it is to stop before writing four
         * megabytes into a three megabyte hole and leaving the filesystem full.
         */
        s->total = (size_t)strtoul(evt->header_value, NULL, 10);
        if (s->total > s->limit) {
            s->too_big = true;
            return ESP_FAIL;            /* aborts the transfer */
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (s->done + (size_t)evt->data_len > s->limit) {
            s->too_big = true;          /* no Content-Length, or it lied */
            return ESP_FAIL;
        }
        if (fwrite(evt->data, 1, (size_t)evt->data_len, s->f) !=
            (size_t)evt->data_len) {
            s->io_error = true;
            return ESP_FAIL;
        }
        s->done += (size_t)evt->data_len;
        if (s->progress != NULL) {
            s->progress(s->ctx, s->path, s->done, s->total);
        }
    }
    return ESP_OK;
}

/* The file's SHA-256 as 64 lowercase hex characters. */
static bool sha256_of(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    uint8_t *buf = malloc(HASH_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return false;
    }

    psa_crypto_init();
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(buf);
        fclose(f);
        return false;
    }

    bool ok  = true;
    size_t n = 0;
    while (ok && (n = fread(buf, 1, HASH_CHUNK, f)) > 0) {
        ok = (psa_hash_update(&op, buf, n) == PSA_SUCCESS);
    }
    if (ferror(f)) {
        ok = false;
    }
    free(buf);
    fclose(f);

    uint8_t mac[32];
    size_t  maclen = 0;
    if (!ok || psa_hash_finish(&op, mac, sizeof(mac), &maclen) != PSA_SUCCESS ||
        maclen != 32) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        sprintf(out + i * 2, "%02x", mac[i]);
    }
    out[64] = 0;
    return true;
}

espix_fetch_status_t espix_net_fetch(const char *url, const char *path,
                                     const char *expect_sha256,
                                     espix_fetch_progress_fn progress, void *ctx,
                                     espix_fetch_info_t *info)
{
    espix_fetch_info_t local = { 0 };
    if (info == NULL) {
        info = &local;
    }

    if (url == NULL || path == NULL || url[0] == 0 || path[0] == 0) {
        return ESPIX_FETCH_ARG;
    }

    espix_fs_info_t fs;
    if (espix_fs_stat_root(&fs) != ESP_OK) {
        return ESPIX_FETCH_IO;
    }
    info->free_now = fs.total_bytes - fs.used_bytes;
    const size_t limit =
        (info->free_now > FETCH_RESERVE) ? info->free_now - FETCH_RESERVE : 0;

    /*
     * A name of its own for the bytes while they arrive. Writing straight over
     * the destination would make an interrupted fetch indistinguishable from a
     * complete one, which matters most for the file that is read on the very
     * next boot.
     */
    char part[320];
    if (snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part)) {
        return ESPIX_FETCH_ARG;
    }

    FILE *f = fopen(part, "wb");
    if (f == NULL) {
        /* Kept because the caller can say something useful about it: EACCES on
         * a system path means "install it with the privilege to write there",
         * which is a different instruction from "the filesystem is broken". */
        info->err = (errno != 0) ? errno : EIO;
        return ESPIX_FETCH_IO;
    }

    fetch_sink_t sink = { .f = f, .path = path, .limit = limit,
                          .progress = progress, .ctx = ctx };

    esp_http_client_config_t cfg = {
        .url                   = url,
        .event_handler         = fetch_event,
        .user_data             = &sink,
        .timeout_ms            = 30000,
        .buffer_size           = 4096,
        /*
         * The request line is built in buffer_size_tx, whose default is 512 --
         * too short for a redirect target, which is how a file host answers.
         */
        .buffer_size_tx        = 2048,
        .keep_alive_enable     = false,
        .max_redirection_count = 5,
        /* esp_http_client follows 3xx inside perform(); a raw file host 302s. */
        .disable_auto_redirect = false,
    };
    if (strncmp(url, "https://", 8) == 0) {
        /* The same trust store `upgrade` uses -- one bundle, not two. */
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    /* Through the redactor: an update URL carries a password in its query. */
    char safe[256];
    espix_net_redact_url(url, safe, sizeof(safe));
    espix_klog(ESPIX_KLOG_INFO, TAG, "fetching %s (%u KiB free)", safe,
               (unsigned)(info->free_now / 1024));

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        fclose(f);
        remove(part);
        return ESPIX_FETCH_NO_NET;
    }

    const esp_err_t err = esp_http_client_perform(c);
    info->http_status   = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    const bool flushed = (fclose(f) == 0);

    info->written = sink.done;
    info->need    = sink.total;

    if (sink.too_big) {
        remove(part);
        return ESPIX_FETCH_NO_ROOM;
    }
    if (sink.io_error || !flushed) {
        if (info->err == 0) {
            info->err = (errno != 0) ? errno : EIO;
        }
        remove(part);
        return ESPIX_FETCH_IO;
    }
    if (err != ESP_OK || info->http_status != 200 || sink.done == 0) {
        remove(part);
        return ESPIX_FETCH_NO_NET;
    }

    if (expect_sha256 != NULL && expect_sha256[0] != 0) {
        char actual[65];
        if (!sha256_of(part, actual)) {
            remove(part);
            return ESPIX_FETCH_IO;
        }
        if (strcasecmp(actual, expect_sha256) != 0) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: sha256 %s, wanted %s", path, actual, expect_sha256);
            remove(part);
            return ESPIX_FETCH_BAD_HASH;
        }
    }

    if (rename(part, path) != 0) {
        info->err = (errno != 0) ? errno : EIO;
        remove(part);
        return ESPIX_FETCH_IO;
    }

    return ESPIX_FETCH_OK;
}
