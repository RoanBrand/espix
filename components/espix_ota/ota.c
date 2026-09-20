/*
 * OTA: writing the passive application slot, and confirming the result.
 */
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_fs.h"
#include "espix_net.h"
#include "espix_time.h"
#include "espix_kernel.h"
#include "espix_ota.h"

#define TAG "ota"

/* esp_ota_write copies out of this, so it must stay valid for the call; 4 KiB
 * is one flash sector's worth and keeps the .bss cost invisible. */
#define OTA_CHUNK 4096

static const char *short_name(uint8_t subtype, const char *label)
{
    switch (subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:   return "ota0";
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:   return "ota1";
    default:                                return label;
    }
}

bool espix_ota_enabled(void)
{
    return CONFIG_ESPIX_OTA_ENABLED;
}

bool espix_ota_available(void)
{
#if CONFIG_ESPIX_OTA_ENABLED
    return esp_ota_get_next_update_partition(NULL) != NULL;
#else
    return false;
#endif
}

void espix_ota_running_slot(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL) {
        strlcpy(buf, "?", len);
        return;
    }
    strlcpy(buf, short_name(run->subtype, run->label), len);
}

size_t espix_ota_slots(espix_ota_slot_t *out, size_t n)
{
    if (out == NULL || n == 0) {
        return 0;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    size_t count = 0;

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL && count < n; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        espix_ota_slot_t *slot = &out[count];

        memset(slot, 0, sizeof(*slot));
        strlcpy(slot->name, short_name(p->subtype, p->label), sizeof(slot->name));
        slot->offset = p->address;
        slot->size   = p->size;
        slot->active = (p == running);
        slot->next   = (p == next);

        esp_ota_img_states_t st;
        slot->state = (esp_ota_get_state_partition(p, &st) == ESP_OK) ? (int)st : -1;
        count++;
    }
    if (it != NULL) {
        esp_partition_iterator_release(it);
    }
    return count;
}

/* Started from espix_ota_init(); defined with the check, far below. */
static void check_task(void *arg);

esp_err_t espix_ota_init(void)
{
#if CONFIG_ESPIX_OTA_ENABLED
    char slot[ESPIX_OTA_NAME_MAX];
    espix_ota_running_slot(slot, sizeof(slot));

    if (!espix_ota_available()) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "OTA is compiled in but there is no second app slot "
                   "(running %s); upgrade will refuse", slot);
        return ESP_OK;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG,
               "running %s; an update would be written to the other slot", slot);

#if CONFIG_ESPIX_OTA_AUTO_CHECK
    if (xTaskCreate(check_task, "ota:check", 8192, NULL, tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no task for the periodic check");
    }
#endif
#else
    espix_klog(ESPIX_KLOG_INFO, TAG, "OTA support not compiled in");
#endif
    return ESP_OK;
}

void espix_ota_confirm_boot(void)
{
#if CONFIG_ESPIX_OTA_ENABLED && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t   st  = ESP_OTA_IMG_UNDEFINED;

    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return;                     /* no otadata; nothing to confirm */
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;                     /* already valid, or not a fresh image */
    }

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    espix_klog(err == ESP_OK ? ESPIX_KLOG_INFO : ESPIX_KLOG_ERROR, TAG,
               "%s: %s", run->label,
               (err == ESP_OK) ? "confirmed; rollback cancelled"
                               : esp_err_to_name(err));
#endif
}

/* The shared half of every install. Whether the bytes come from a file, an SSH
 * session's stdin or the network, they go into the passive slot the same way:
 * sequential writes, so no long bulk erase, and one place that reports what
 * went wrong. `total` is 0 when the length is not known in advance. */
static esp_err_t install_stream(FILE *in, size_t total,
                                espix_ota_progress_fn progress, void *ctx,
                                char *err, size_t err_len)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "this image has only one application slot");
        }
        return ESP_ERR_NOT_FOUND;
    }

    static char chunk[OTA_CHUNK];

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len,
                     "cannot begin on %s: %s (internal free %u KiB, largest %u KiB)",
                     part->label, esp_err_to_name(e),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        }
        return e;
    }

    size_t done = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
        e = esp_ota_write(h, chunk, n);
        if (e != ESP_OK) {
            if (err != NULL) {
                snprintf(err, err_len, "write failed at %u bytes: %s",
                         (unsigned)done, esp_err_to_name(e));
            }
            esp_ota_abort(h);
            return e;
        }
        done += n;
        if (progress != NULL) {
            progress(ctx, done, total);
        }
    }

    if (ferror(in)) {
        if (err != NULL) {
            snprintf(err, err_len, "the image could not be read");
        }
        esp_ota_abort(h);
        return ESP_FAIL;
    }

    e = esp_ota_end(h);
    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len, "the image was refused: %s", esp_err_to_name(e));
        }
        return e;
    }

    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot select %s to boot: %s",
                     part->label, esp_err_to_name(e));
        }
        return e;
    }

    if (err != NULL) {
        snprintf(err, err_len, "%s written (%u bytes); it boots on the next reboot",
                 part->label, (unsigned)done);
    }
    return ESP_OK;
}

esp_err_t espix_ota_install_file(const char *path,
                                 espix_ota_progress_fn progress, void *ctx,
                                 char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "%s: %s", path, strerror(errno));
        }
        return ESP_ERR_NOT_FOUND;
    }

    size_t total = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long end = ftell(f);
        if (end > 0) {
            total = (size_t)end;
        }
        rewind(f);
    }

    const esp_err_t e = install_stream(f, total, progress, ctx, err, err_len);
    fclose(f);
    return e;
}

esp_err_t espix_ota_install_stream(FILE *in,
                                   espix_ota_progress_fn progress, void *ctx,
                                   char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return install_stream(in, 0, progress, ctx, err, err_len);
}

esp_err_t espix_ota_install_url(const char *url,
                                espix_ota_progress_fn progress, void *ctx,
                                char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espix_ota_available()) {
        if (err != NULL) {
            snprintf(err, err_len, "this image has only one application slot");
        }
        return ESP_ERR_NOT_FOUND;
    }

    esp_http_client_config_t http = {
        .url               = url,
        .timeout_ms        = CONFIG_ESPIX_OTA_TIMEOUT_MS,
        .buffer_size       = 4096,
        .keep_alive_enable = true,
    };
    /* The CA bundle only when the URL is really TLS; a dev HTTP pull has nothing
     * to verify against, and needs CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP anyway. */
    if (strncmp(url, "https://", 8) == 0) {
        http.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };
#if CONFIG_SPIRAM
    /* The download buffer need not be internal, and internal is the pool that
     * runs short. The TLS context does have to be, which is why a failure here
     * reports the figures. */
    cfg.buffer_caps = MALLOC_CAP_SPIRAM;
#endif

    espix_klog(ESPIX_KLOG_INFO, TAG, "fetching %s (internal free %u KiB)",
               url, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    esp_https_ota_handle_t h = NULL;
    esp_err_t e = esp_https_ota_begin(&cfg, &h);
    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len,
                     "cannot start: %s (internal free %u KiB, largest %u KiB)",
                     esp_err_to_name(e),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        }
        return e;
    }

    const int total = esp_https_ota_get_image_size(h);
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (progress != NULL) {
            const int got = esp_https_ota_get_image_len_read(h);
            progress(ctx, (size_t)(got > 0 ? got : 0),
                     (total > 0) ? (size_t)total : 0);
        }
    }

    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len, "download failed: %s", esp_err_to_name(e));
        }
        esp_https_ota_abort(h);
        return e;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        if (err != NULL) {
            snprintf(err, err_len, "the download ended before the image did");
        }
        esp_https_ota_abort(h);
        return ESP_FAIL;
    }

    e = esp_https_ota_finish(h);
    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len, "the image was refused: %s", esp_err_to_name(e));
        }
        return e;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "installed from %s", url);
    if (err != NULL) {
        snprintf(err, err_len, "installed from %s; it boots on the next reboot", url);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* The update repo                                                     */
/* ------------------------------------------------------------------ */

#define OTA_CONF_PATH "/etc/espix.conf"
#define MANIFEST_MAX  2048

const char *espix_ota_source(void)
{
    /*
     * Read every time rather than caching for the boot: /etc/espix.conf is meant
     * to be hand-editable and take effect on the next command, the way
     * /etc/wifi.conf does. It is a few hundred bytes off littlefs.
     */
    static char url[256];

    char conf[256] = {0};
    if (espix_fs_conf_get(OTA_CONF_PATH, "ota.url", conf, sizeof(conf)) &&
        conf[0] != '\0') {
        strlcpy(url, conf, sizeof(url));
    } else {
        strlcpy(url, CONFIG_ESPIX_OTA_URL, sizeof(url));
    }
    return url;
}

/*
 * A string field of a flat JSON object. Enough for a manifest this project also
 * writes: no nesting, no escapes, no allocations. Deliberately not a JSON
 * library -- the whole object is version/build/url/sha256/min_version, and this
 * costs a few hundred bytes of ROM rather than tens of kilobytes.
 *
 * The key is found literally and must be followed by a quoted string. That
 * would be too loose for arbitrary JSON; for our own manifest, whose keys are
 * distinct words, it is enough.
 */
static bool json_string(const char *json, const char *key, char *out, size_t len)
{
    const char *p = strstr(json, key);
    if (p == NULL) {
        return false;
    }
    p += strlen(key);
    if (*p == '"') {
        p++;                        /* the key's own closing quote */
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* -1, 0 or 1; a missing component counts as zero, so "0.4" equals "0.4.0". */
static int semver_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        char *ea;
        char *eb;
        const long x = strtol(a, &ea, 10);
        const long y = strtol(b, &eb, 10);
        if (x != y) {
            return (x < y) ? -1 : 1;
        }
        a = (*ea == '.') ? ea + 1 : ea;
        b = (*eb == '.') ? eb + 1 : eb;
    }
    return 0;
}

espix_ota_update_t espix_ota_compare(const espix_ota_manifest_t *m)
{
    if (m == NULL) {
        return ESPIX_OTA_UPDATE_UNKNOWN;
    }

    const int c = semver_cmp(m->version, espix_version());
    if (c > 0) {
        return ESPIX_OTA_UPDATE_AVAILABLE;
    }
    if (c < 0) {
        return ESPIX_OTA_UPDATE_UP_TO_DATE;
    }

    /* Same version. A different build is still worth having -- the middle rule
     * in docs/OTA.md, which makes a rebuilt rolling release an update rather
     * than a silent no-op. */
    if (m->build[0] != '\0' &&
        strncasecmp(m->build, espix_build_id(), 9) != 0) {
        return ESPIX_OTA_UPDATE_AVAILABLE;
    }
    return ESPIX_OTA_UPDATE_UP_TO_DATE;
}

bool espix_ota_meets_min(const espix_ota_manifest_t *m)
{
    if (m == NULL || m->min_version[0] == '\0') {
        return true;
    }
    return semver_cmp(espix_version(), m->min_version) >= 0;
}

esp_err_t espix_ota_manifest_fetch(const char *url, espix_ota_manifest_t *m,
                                   char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (url == NULL || m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(m, 0, sizeof(*m));

    char *buf = NULL;
#if CONFIG_SPIRAM
    buf = heap_caps_malloc(MANIFEST_MAX, MALLOC_CAP_SPIRAM);
#endif
    if (buf == NULL) {
        buf = malloc(MANIFEST_MAX);
    }
    if (buf == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "no memory for the manifest (%u KiB internal free)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_ESPIX_OTA_TIMEOUT_MS,
        .buffer_size       = 1024,
        .keep_alive_enable = true,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        free(buf);
        if (err != NULL) {
            snprintf(err, err_len, "cannot start an HTTP client");
        }
        return ESP_FAIL;
    }

    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            if (err != NULL) {
                snprintf(err, err_len, "%s: HTTP %d", url, status);
            }
            e = ESP_ERR_NOT_FOUND;
        } else {
            int n = 0;
            while (n < MANIFEST_MAX - 1) {
                const int got = esp_http_client_read(c, buf + n, MANIFEST_MAX - 1 - n);
                if (got <= 0) {
                    break;
                }
                n += got;
            }
            buf[n] = '\0';
        }
    } else if (err != NULL) {
        snprintf(err, err_len, "%s: %s", url, esp_err_to_name(e));
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (e != ESP_OK) {
        free(buf);
        return e;
    }

    json_string(buf, "version",     m->version,     sizeof(m->version));
    json_string(buf, "build",       m->build,       sizeof(m->build));
    json_string(buf, "url",         m->url,         sizeof(m->url));
    json_string(buf, "sha256",      m->sha256,      sizeof(m->sha256));
    json_string(buf, "min_version", m->min_version, sizeof(m->min_version));
    free(buf);

    if (m->version[0] == '\0') {
        if (err != NULL) {
            snprintf(err, err_len, "%s: the manifest has no version", url);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (m->url[0] == '\0') {
        if (err != NULL) {
            snprintf(err, err_len, "%s: the manifest has no image URL", url);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* The cached verdict, for the greeting and the periodic check         */
/* ------------------------------------------------------------------ */

#define OTA_STATE_DIR  "/var/lib/espix"
#define OTA_STATE_PATH "/var/lib/espix/update"

static void state_save(const espix_ota_manifest_t *m, esp_err_t e)
{
    mkdir("/var/lib", 0755);
    mkdir(OTA_STATE_DIR, 0755);

    FILE *f = fopen(OTA_STATE_PATH, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "# espix update state; written by the last check\n");
    fprintf(f, "checked=%ld\n", (long)time(NULL));
    if (e == ESP_OK && m != NULL) {
        fprintf(f, "version=%s\n", m->version);
        fprintf(f, "build=%s\n", m->build);
    } else {
        fprintf(f, "error=%s\n", esp_err_to_name(e));
    }
    fclose(f);
}

/*
 * Reads the cached verdict. A plain text file rather than NVS: it is machine
 * state, but espix's habit is that a person can cat it and see why the greeting
 * said what it said, and a daily write is nothing to littlefs.
 */
static bool state_load(espix_ota_manifest_t *m, time_t *checked)
{
    *checked = 0;
    memset(m, 0, sizeof(*m));

    FILE *f = fopen(OTA_STATE_PATH, "r");
    if (f == NULL) {
        return false;
    }

    char line[160];
    bool have = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl = strpbrk(line, "\r\n");
        if (nl != NULL) {
            *nl = 0;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "checked") == 0) {
            *checked = (time_t)strtol(val, NULL, 10);
        } else if (strcmp(key, "version") == 0) {
            strlcpy(m->version, val, sizeof(m->version));
            have = true;
        } else if (strcmp(key, "build") == 0) {
            strlcpy(m->build, val, sizeof(m->build));
        }
    }
    fclose(f);
    return have;
}

esp_err_t espix_ota_check(const char *url, espix_ota_manifest_t *m,
                          char *err, size_t err_len)
{
    espix_ota_manifest_t local;
    espix_ota_manifest_t *out = (m != NULL) ? m : &local;

    const esp_err_t e = espix_ota_manifest_fetch(url, out, err, err_len);
    state_save(out, e);
    return e;
}

bool espix_ota_known_update(char *version, size_t len)
{
    espix_ota_manifest_t m;
    time_t checked;

    if (!state_load(&m, &checked)) {
        return false;
    }
    if (espix_ota_compare(&m) != ESPIX_OTA_UPDATE_AVAILABLE) {
        return false;
    }
    if (version != NULL && len > 0) {
        strlcpy(version, m.version, len);
    }
    return true;
}

static bool auto_check_enabled(void)
{
    char v[8] = {0};

    if (espix_fs_conf_get(OTA_CONF_PATH, "ota.auto_check", v, sizeof(v))) {
        return !(strcasecmp(v, "off") == 0 ||
                 strcasecmp(v, "no") == 0  ||
                 strcmp(v, "0") == 0);
    }
    return CONFIG_ESPIX_OTA_AUTO_CHECK;
}

/*
 * Deliberately undemanding: a first look five minutes after boot (long enough
 * for the network and the clock to settle), then a wake every fifteen minutes
 * that does nothing unless a day has passed since the last check. It records;
 * it never installs.
 */
#define OTA_FIRST_DELAY_MS (5 * 60 * 1000)
#define OTA_WAKE_MS        (15 * 60 * 1000)

static void check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_DELAY_MS));

    for (;;) {
        if (espix_ota_available() && auto_check_enabled()) {
            espix_ota_manifest_t m;
            time_t checked;
            char ifname[ESPIX_IF_NAME_MAX];
            uint32_t gw;

            state_load(&m, &checked);

            const bool routed = espix_net_default_route(ifname, sizeof(ifname), &gw);
            const bool settled = espix_time_is_synced();
            const bool due = (checked == 0) ||
                             (time(NULL) - checked >= CONFIG_ESPIX_OTA_CHECK_PERIOD_S);

            if (routed && settled && due) {
                char err[128];
                espix_ota_manifest_t found;
                const esp_err_t e = espix_ota_check(espix_ota_source(), &found,
                                                    err, sizeof(err));
                if (e == ESP_OK) {
                    espix_klog(ESPIX_KLOG_INFO, TAG, "update check: %s available",
                               found.version);
                } else {
                    espix_klog(ESPIX_KLOG_WARN, TAG, "update check: %s", err);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_WAKE_MS));
    }
}
