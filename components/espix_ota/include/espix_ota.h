/*
 * OTA: kernels are files, and the loader installs them.
 *
 * The kernel never writes an application slot -- it cannot write the one it is
 * running from, and with a single kernel slot there is no other. Instead it
 * keeps images as files under /boot, records which is good, previous and
 * pending in NVS, and hands over to the loader (ota_1) when something needs
 * installing. The loader does the flash write. See docs/OTA.md section 11.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_OTA_NAME_MAX  64
#define ESPIX_OTA_BOOT_DIR  "/boot"

typedef struct {
    char     name[16];                  /* "ota0", "ota1", "factory" */
    char     role[8];                   /* "kernel" or "loader" */
    char     version[32];               /* from the slot's app descriptor */
    char     build[72];                 /* its espix build id, if any */
    uint32_t offset;
    uint32_t size;
    int      state;                     /* esp_ota_img_states_t, or -1 */
} espix_ota_slot_t;

bool   espix_ota_enabled(void);
bool   espix_ota_available(void);
esp_err_t espix_ota_init(void);

/* The name this running image should have under /boot. */
void   espix_ota_self_name(char *buf, size_t len);

/* "ota0" / "ota1" / "factory" for the partition we booted from. */
const char *espix_ota_running_slot_name(void);

/*
 * Make sure /boot holds this running image, verified against the hash baked
 * into the slot, and record it as good. Called on every boot; cheap when the
 * file is already there.
 */
esp_err_t espix_ota_archive_self(char *name, size_t len);

/* The NVS triad, for the greeting and for --slots. */
void   espix_ota_state(char *good, size_t gl,
                       char *previous, size_t pl,
                       char *pending, size_t nl);

/* Application partitions, for --slots. */
size_t espix_ota_slots(espix_ota_slot_t *out, size_t n);

/* ------------------------------------------------------------------ */
/* Handing an image to the loader                                      */
/* ------------------------------------------------------------------ */

typedef void (*espix_ota_progress_fn)(void *ctx, size_t done, size_t total);

/* Download url into /boot/<name>, then check it against expect_sha256. */
esp_err_t espix_ota_download(const char *url, const char *name,
                             const char *expect_sha256,
                             espix_ota_progress_fn progress, void *ctx,
                             char *err, size_t err_len);

/* Record name as pending and select the loader; the caller then reboots. */
esp_err_t espix_ota_queue(const char *name, char *err, size_t err_len);

/* Name one of these, if set. */
bool   espix_ota_pending(char *name, size_t len);
bool   espix_ota_previous(char *name, size_t len);

/* Copy a local file into /boot under its own basename. --file. */
esp_err_t espix_ota_adopt(const char *path, char *name, size_t len,
                          char *err, size_t err_len);

/* ------------------------------------------------------------------ */
/* The update repo                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char version[32];
    char build[72];
    char url[256];
    char sha256[72];
    char min_version[32];
    char board[24];                     /* "s3-n16r8"; empty means "any" */
    char chip[16];                      /* "esp32s3"; empty means "any" */
} espix_ota_manifest_t;

typedef enum {
    ESPIX_OTA_UPDATE_UP_TO_DATE = 0,
    ESPIX_OTA_UPDATE_AVAILABLE,
    ESPIX_OTA_UPDATE_UNKNOWN,
} espix_ota_update_t;

const char *espix_ota_source(void);
esp_err_t espix_ota_manifest_fetch(const char *url, espix_ota_manifest_t *m,
                                   char *err, size_t err_len);
espix_ota_update_t espix_ota_compare(const espix_ota_manifest_t *m);
bool espix_ota_meets_min(const espix_ota_manifest_t *m);
esp_err_t espix_ota_check(const char *url, espix_ota_manifest_t *m,
                          char *err, size_t err_len);
bool espix_ota_known_update(char *version, size_t len);

/* Confirm this image: roll the NVS triad, prune /boot, cancel rollback. */
void espix_ota_confirm_boot(void);

#ifdef __cplusplus
}
#endif
