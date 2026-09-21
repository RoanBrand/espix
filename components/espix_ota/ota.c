/*
 * OTA: the kernel keeps images as files and the loader installs them.
 *
 * This side never writes an application slot. It archives the running image
 * into /boot, verifies and queues other images, and records good / previous /
 * pending in NVS for the loader to act on. See docs/OTA.md section 11.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_fs.h"
#include "espix_net.h"
#include "espix_time.h"
#include "espix_kernel.h"
#include "espix_ota.h"

#define TAG "ota"

/* One flash sector, and it stays in .bss so a download costs no heap on the
 * path where heap is what runs out. */
#define OTA_CHUNK 4096

#define NVS_NS "espix_boot"

/* ------------------------------------------------------------------ */
/* State: what the loader will read                                    */
/* ------------------------------------------------------------------ */

static bool name_get(nvs_handle_t h, const char *key, char *out, size_t len)
{
    out[0] = 0;
    size_t l = len;
    return (nvs_get_str(h, key, out, &l) == ESP_OK) && out[0] != 0;
}

static void name_set(nvs_handle_t h, const char *key, const char *val)
{
    if (val != NULL && val[0] != 0) {
        nvs_set_str(h, key, val);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
}

void espix_ota_self_name(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    snprintf(buf, len, "espix-%s-%s.bin", espix_version(), espix_build_id());
}

void espix_ota_state(char *good, size_t gl, char *previous, size_t pl,
                     char *pending, size_t nl)
{
    if (good != NULL && gl > 0)      good[0] = 0;
    if (previous != NULL && pl > 0)  previous[0] = 0;
    if (pending != NULL && nl > 0)   pending[0] = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (good != NULL)     name_get(h, "good", good, gl);
    if (previous != NULL) name_get(h, "previous", previous, pl);
    if (pending != NULL)  name_get(h, "pending", pending, nl);
    nvs_close(h);
}

bool espix_ota_pending(char *name, size_t len)
{
    char p[ESPIX_OTA_NAME_MAX] = {0};
    espix_ota_state(NULL, 0, NULL, 0, p, sizeof(p));
    if (p[0] == 0) {
        return false;
    }
    if (name != NULL && len > 0) {
        strlcpy(name, p, len);
    }
    return true;
}

bool espix_ota_previous(char *name, size_t len)
{
    char p[ESPIX_OTA_NAME_MAX] = {0};
    espix_ota_state(NULL, 0, p, sizeof(p), NULL, 0);
    if (p[0] == 0) {
        return false;
    }
    if (name != NULL && len > 0) {
        strlcpy(name, p, len);
    }
    return true;
}

/* Keep the named files; drop every other archived image. Steady state is two
 * (good + previous), which is what makes room for the next upgrade. */
static void prune_boot(const char *good, const char *previous, const char *pending)
{
    DIR *d = opendir(ESPIX_OTA_BOOT_DIR);
    if (d == NULL) {
        return;
    }

    struct dirent *de;
    char path[320];

    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        const size_t l = strlen(n);

        if (strncmp(n, "espix-", 6) != 0 || l < 5 ||
            strcmp(n + l - 4, ".bin") != 0) {
            continue;
        }
        if ((good != NULL && strcmp(n, good) == 0) ||
            (previous != NULL && strcmp(n, previous) == 0) ||
            (pending != NULL && strcmp(n, pending) == 0)) {
            continue;
        }
        snprintf(path, sizeof(path), ESPIX_OTA_BOOT_DIR "/%s", n);
        if (unlink(path) == 0) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "dropped the old image %s", n);
        }
    }
    closedir(d);
}

esp_err_t espix_ota_archive_self(char *name, size_t len)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    espix_ota_self_name(name, len);
    char path[320];
    snprintf(path, sizeof(path), ESPIX_OTA_BOOT_DIR "/%s", name);

    /* Verify against the hash baked into the image before copying it: this is
     * what makes the /boot file a trustworthy restore point rather than a copy
     * of whatever happens to be in the slot. */
    esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
    esp_image_metadata_t meta;
    const esp_err_t verr = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (verr != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s does not verify (%s); not archiving",
                   run->label, esp_err_to_name(verr));
        return verr;
    }

    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == (size_t)meta.image_len) {
        return ESP_OK;                  /* already archived, and the right size */
    }

    char part[328];
    snprintf(part, sizeof(part), ESPIX_OTA_BOOT_DIR "/.%s.part", name);

    FILE *f = fopen(part, "wb");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot write %s: %s", part,
                   strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        fclose(f);
        unlink(part);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = ESP_OK;
    uint32_t off = 0;
    uint32_t remain = meta.image_len;

    while (remain > 0) {
        const size_t n = (remain > OTA_CHUNK) ? OTA_CHUNK : remain;
        if (esp_partition_read(run, off, buf, n) != ESP_OK ||
            fwrite(buf, 1, n, f) != n) {
            e = ESP_FAIL;
            break;
        }
        off += n;
        remain -= n;
    }
    free(buf);

    if (fclose(f) != 0 && e == ESP_OK) {
        e = ESP_FAIL;
    }
    if (e != ESP_OK || rename(part, path) != 0) {
        unlink(part);
        espix_klog(ESPIX_KLOG_ERROR, TAG, "could not archive %s", name);
        return ESP_FAIL;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "archived %s (%u bytes)", name,
               (unsigned)meta.image_len);
    return ESP_OK;
}

void espix_ota_confirm_boot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;

    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return;                         /* no otadata; nothing to confirm */
    }

    char self[ESPIX_OTA_NAME_MAX];
    espix_ota_self_name(self, sizeof(self));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char good[ESPIX_OTA_NAME_MAX] = {0};
        char prev[ESPIX_OTA_NAME_MAX] = {0};

        name_get(h, "good", good, sizeof(good));
        name_get(h, "previous", prev, sizeof(prev));

        if (strcmp(good, self) != 0) {
            /* A new image is running: what was good becomes the rollback
             * target, unless this is the very first boot and there was none. */
            name_set(h, "previous", (good[0] != 0) ? good : prev);
            name_set(h, "good", self);
            prune_boot(self, (good[0] != 0) ? good : prev, NULL);
        }
        nvs_close(h);
    }

    if (st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW) {
        const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        espix_klog(e == ESP_OK ? ESPIX_KLOG_INFO : ESPIX_KLOG_ERROR, TAG,
                   "%s: %s", run->label,
                   (e == ESP_OK) ? "confirmed; rollback cancelled"
                                 : esp_err_to_name(e));
    }
}

/* ------------------------------------------------------------------ */
/* Handing an image to the loader                                      */
/* ------------------------------------------------------------------ */

static esp_err_t sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    psa_crypto_init();
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(f);
        return ESP_FAIL;
    }

    static uint8_t buf[OTA_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            fclose(f);
            return ESP_FAIL;
        }
    }
    const bool bad = ferror(f);
    fclose(f);
    if (bad) {
        psa_hash_abort(&op);
        return ESP_FAIL;
    }

    uint8_t mac[32];
    size_t maclen = 0;
    if (psa_hash_finish(&op, mac, sizeof(mac), &maclen) != PSA_SUCCESS ||
        maclen != 32) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < 32; i++) {
        sprintf(out + i * 2, "%02x", mac[i]);
    }
    out[64] = 0;
    return ESP_OK;
}

esp_err_t espix_ota_download(const char *url, const char *name,
                             const char *expect_sha256,
                             espix_ota_progress_fn progress, void *ctx,
                             char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = 0;
    }
    if (url == NULL || name == NULL || name[0] == 0 ||
        strchr(name, '/') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espix_ota_available()) {
        if (err != NULL) {
            snprintf(err, err_len, "this image has no loader slot to install from");
        }
        return ESP_ERR_NOT_FOUND;
    }

    char path[320];
    char part[328];
    snprintf(path, sizeof(path), ESPIX_OTA_BOOT_DIR "/%s", name);
    snprintf(part, sizeof(part), ESPIX_OTA_BOOT_DIR "/.%s.part", name);

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_ESPIX_OTA_TIMEOUT_MS,
        .buffer_size       = 4096,
        .keep_alive_enable = true,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "fetching %s (internal free %u KiB)", url,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot start an HTTP client");
        }
        return ESP_FAIL;
    }

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot reach %s: %s", url,
                     (e == ESP_ERR_TIMEOUT) ? "timed out" : esp_err_to_name(e));
        }
        esp_http_client_cleanup(c);
        return e;
    }

    esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        if (err != NULL) {
            snprintf(err, err_len, "%s: the server answered HTTP %d", url, status);
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NOT_FOUND;
    }

    const int total = esp_http_client_get_content_length(c);
    FILE *f = fopen(part, "wb");
    if (f == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot write %s: %s", part, strerror(errno));
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NOT_FOUND;
    }

    static char buf[OTA_CHUNK];
    int got;
    size_t done = 0;

    while ((got = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)got, f) != (size_t)got) {
            e = ESP_ERR_NO_MEM;         /* almost always ENOSPC */
            break;
        }
        done += (size_t)got;
        if (progress != NULL) {
            progress(ctx, done, (total > 0) ? (size_t)total : 0);
        }
    }
    if (e == ESP_OK && got < 0) {
        e = ESP_FAIL;
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (fclose(f) != 0 && e == ESP_OK) {
        e = ESP_FAIL;
    }

    if (e != ESP_OK) {
        unlink(part);
        if (err != NULL) {
            snprintf(err, err_len, "the download failed: %s",
                     (e == ESP_ERR_NO_MEM) ? "out of space" : esp_err_to_name(e));
        }
        return e;
    }

    if (expect_sha256 != NULL && expect_sha256[0] != 0) {
        char actual[65];
        if (sha256_file(part, actual) != ESP_OK) {
            unlink(part);
            if (err != NULL) {
                snprintf(err, err_len, "cannot hash what was downloaded");
            }
            return ESP_FAIL;
        }
        if (strcasecmp(actual, expect_sha256) != 0) {
            unlink(part);
            if (err != NULL) {
                snprintf(err, err_len,
                         "checksum mismatch: expected %.12s..., got %.12s...",
                         expect_sha256, actual);
            }
            return ESP_ERR_INVALID_CRC;
        }
    }

    if (rename(part, path) != 0) {
        unlink(part);
        if (err != NULL) {
            snprintf(err, err_len, "cannot put %s in place", path);
        }
        return ESP_FAIL;
    }

    if (err != NULL) {
        snprintf(err, err_len, "downloaded %s (%u bytes)", name, (unsigned)done);
    }
    return ESP_OK;
}

esp_err_t espix_ota_adopt(const char *path, char *name, size_t len,
                          char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = 0;
    }
    if (path == NULL || name == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    if (base[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char dst[320];
    snprintf(dst, sizeof(dst), ESPIX_OTA_BOOT_DIR "/%s", base);
    strlcpy(name, base, len);

    if (strcmp(path, dst) == 0) {
        return ESP_OK;                  /* already where the loader looks */
    }

    FILE *in = fopen(path, "rb");
    if (in == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "%s: %s", path, strerror(errno));
        }
        return ESP_ERR_NOT_FOUND;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        if (err != NULL) {
            snprintf(err, err_len, "cannot write %s: %s", dst, strerror(errno));
        }
        return ESP_ERR_NOT_FOUND;
    }

    static char buf[OTA_CHUNK];
    size_t n, done = 0;
    esp_err_t e = ESP_OK;

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            e = ESP_ERR_NO_MEM;
            break;
        }
        done += n;
    }
    if (ferror(in)) {
        e = ESP_FAIL;
    }
    fclose(in);
    if (fclose(out) != 0 && e == ESP_OK) {
        e = ESP_FAIL;
    }

    if (e != ESP_OK || done == 0) {
        unlink(dst);
        if (err != NULL) {
            snprintf(err, err_len, "%s", (done == 0) ? "the file is empty"
                                                     : "the copy failed");
        }
        return (e != ESP_OK) ? e : ESP_FAIL;
    }

    if (err != NULL) {
        snprintf(err, err_len, "adopted %s (%u bytes)", name, (unsigned)done);
    }
    return ESP_OK;
}

esp_err_t espix_ota_queue(const char *name, char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = 0;
    }
    if (name == NULL || name[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *loader = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (loader == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "the partition table has no loader slot (ota1)");
        }
        return ESP_ERR_NOT_FOUND;
    }

    char path[320];
    snprintf(path, sizeof(path), ESPIX_OTA_BOOT_DIR "/%s", name);
    FILE *probe = fopen(path, "rb");
    if (probe == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "%s: %s", path, strerror(errno));
        }
        return ESP_ERR_NOT_FOUND;
    }
    fclose(probe);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot record the pending image");
        }
        return ESP_FAIL;
    }
    name_set(h, "pending", name);
    nvs_close(h);

    const esp_err_t e = esp_ota_set_boot_partition(loader);
    if (e != ESP_OK) {
        /* Do not leave a pending image the loader will never be asked about. */
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            name_set(h, "pending", NULL);
            nvs_close(h);
        }
        if (err != NULL) {
            snprintf(err, err_len, "cannot select the loader to boot: %s",
                     esp_err_to_name(e));
        }
        return e;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "queued %s for the loader", name);
    if (err != NULL) {
        snprintf(err, err_len, "queued %s", name);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* The slot table, for upgrade --slots                                 */
/* ------------------------------------------------------------------ */

bool espix_ota_enabled(void)
{
    return CONFIG_ESPIX_OTA_ENABLED;
}

bool espix_ota_available(void)
{
    const esp_partition_t *loader = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (loader == NULL) {
        return false;
    }
    /* The partition existing is not enough: the loader has to be in it, or
     * queueing an image would select a slot the bootloader cannot boot. */
    esp_app_desc_t desc;
    return esp_ota_get_partition_description(loader, &desc) == ESP_OK;
}

static const char *short_name(uint8_t subtype, const char *label)
{
    switch (subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:   return "ota0";
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:   return "ota1";
    default:                                return label;
    }
}

static void sha_prefix(const esp_app_desc_t *d, char *out, size_t len)
{
    snprintf(out, len, "%02x%02x%02x%02x%02x",
             d->app_elf_sha256[0], d->app_elf_sha256[1], d->app_elf_sha256[2],
             d->app_elf_sha256[3], d->app_elf_sha256[4]);
    if (strlen(out) > 9) {
        out[9] = 0;
    }
}

size_t espix_ota_slots(espix_ota_slot_t *out, size_t n)
{
    if (out == NULL || n == 0) {
        return 0;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
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
        slot->boot   = (p == boot);

        esp_ota_img_states_t st;
        slot->state = (esp_ota_get_state_partition(p, &st) == ESP_OK) ? (int)st : -1;

        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
            strlcpy(slot->version, desc.version, sizeof(slot->version));
            sha_prefix(&desc, slot->build, sizeof(slot->build));
        }
        count++;
    }
    if (it != NULL) {
        esp_partition_iterator_release(it);
    }
    return count;
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
 * A string field of a flat JSON object, matched on the quoted key so that
 * "version" cannot be found inside "min_version". Enough for a manifest this
 * project also writes: no nesting, no escapes, no allocations.
 */
static bool json_string(const char *json, const char *quoted_key,
                        char *out, size_t len)
{
    const char *p = strstr(json, quoted_key);
    if (p == NULL) {
        return false;
    }
    p += strlen(quoted_key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
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
        err[0] = 0;
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
                if (status == 404) {
                    snprintf(err, err_len, "no manifest found (HTTP 404)");
                } else {
                    snprintf(err, err_len,
                             "%s: the server answered HTTP %d", url, status);
                }
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
        const char *why = (e == ESP_ERR_TIMEOUT)      ? "timed out"
                        : (e == ESP_ERR_HTTP_CONNECT) ? "could not connect"
                        : esp_err_to_name(e);
        snprintf(err, err_len, "cannot reach %s: %s", url, why);
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (e != ESP_OK) {
        free(buf);
        return e;
    }

    json_string(buf, "\"version\"",     m->version,     sizeof(m->version));
    json_string(buf, "\"build\"",       m->build,       sizeof(m->build));
    json_string(buf, "\"url\"",         m->url,         sizeof(m->url));
    json_string(buf, "\"sha256\"",      m->sha256,      sizeof(m->sha256));
    json_string(buf, "\"min_version\"", m->min_version, sizeof(m->min_version));
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
 * The check is event-driven: it runs once the network says an address exists,
 * not on a fixed delay, so it costs nothing at boot and does not wait five
 * minutes to notice a link that came up in two. The loop re-reads the routing
 * table itself, so any IP event is only a hint.
 *
 * The interval is measured on the monotonic clock, which NTP cannot move. The
 * stored wall time guards only the across-reboot case, and only while the clock
 * is trustworthy; a device with no time source checks once per boot, which is
 * the honest best a board with no RTC can do.
 */
#define OTA_WAKE_MS (15 * 60 * 1000)

static TaskHandle_t s_check_task;

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;

    /* The event loop task must not do network work, so this only wakes the
     * checker; a notification arriving while it works is coalesced. */
    if (s_check_task != NULL) {
        xTaskNotifyGive(s_check_task);
    }
}

static void check_task(void *arg)
{
    (void)arg;
    const int64_t period_us =
        (int64_t)CONFIG_ESPIX_OTA_CHECK_PERIOD_S * 1000000LL;
    int64_t last_us = 0;

    for (;;) {
        if (espix_ota_available() && auto_check_enabled()) {
            espix_ota_manifest_t m;
            time_t checked;
            char ifname[ESPIX_IF_NAME_MAX];
            uint32_t gw;

            state_load(&m, &checked);

            const int64_t now = esp_timer_get_time();
            const bool routed = espix_net_default_route(ifname, sizeof(ifname), &gw);
            bool due = (last_us == 0) || (now - last_us >= period_us);

            if (due && espix_time_is_synced() && checked != 0) {
                const long delta = (long)time(NULL) - (long)checked;
                /* A negative delta means the clock was reset backwards since
                 * the last check; trust the monotonic interval instead. */
                due = (delta < 0) ||
                      (delta >= CONFIG_ESPIX_OTA_CHECK_PERIOD_S);
            }

            if (routed && due) {
                char err[128];
                espix_ota_manifest_t found;
                const esp_err_t e = espix_ota_check(espix_ota_source(), &found,
                                                    err, sizeof(err));
                last_us = now;
                if (e == ESP_OK) {
                    espix_klog(ESPIX_KLOG_INFO, TAG, "update check: %s available",
                               found.version);
                } else {
                    espix_klog(ESPIX_KLOG_WARN, TAG, "update check: %s", err);
                }
            }
        }

        /* A GOT_IP wakes us; the timeout is the backstop for the 24h re-check
         * and for a route that was already up when we registered. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OTA_WAKE_MS));
    }
}

esp_err_t espix_ota_init(void)
{
    if (!espix_ota_enabled()) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "OTA support not compiled in");
        return ESP_OK;
    }
    if (!espix_ota_available()) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "OTA is compiled in but there is no loader slot (ota1); "
                   "upgrade will refuse");
        return ESP_OK;
    }

    mkdir(ESPIX_OTA_BOOT_DIR, 0755);

    char self[ESPIX_OTA_NAME_MAX];
    if (espix_ota_archive_self(self, sizeof(self)) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "running %s, archived as /boot/%s",
                   espix_ota_running_slot_name(), self);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "this image is not archived in /boot; rollback will not "
                   "have it");
    }

#if CONFIG_ESPIX_OTA_AUTO_CHECK
    /*
     * The default event loop that espix_time and espix_net also want, so that
     * this component's place in the boot order does not decide whether it
     * works -- whichever gets here first creates it.
     */
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no event loop: %s",
                   esp_err_to_name(loop_err));
    } else if (xTaskCreate(check_task, "ota:check", 8192, NULL,
                           tskIDLE_PRIORITY + 1, &s_check_task) != pdPASS) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no task for the periodic check");
    } else if (esp_event_handler_instance_register(
                   IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "no IP event handler; the check falls back to polling");
    }
#endif
    return ESP_OK;
}

const char *espix_ota_running_slot_name(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return (run != NULL) ? short_name(run->subtype, run->label) : "?";
}
