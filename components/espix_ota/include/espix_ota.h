/*
 * OTA: the device side of the A/B update path.
 *
 * The two application slots and otadata belong to the bootloader; this is what
 * writes the passive slot, points the bootloader at it, and then confirms the
 * result on the next boot so the bootloader does not roll it back.
 *
 * Which slot is running is a question for espix_ota_slots() (and `upgrade
 * --slots`), not for the /dev listing: a device node cannot carry otadata state.
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

#define ESPIX_OTA_NAME_MAX 16

typedef struct {
    char     name[ESPIX_OTA_NAME_MAX];  /* "ota0", "ota1", "factory", ... */
    uint32_t offset;
    uint32_t size;
    int      state;                     /* esp_ota_img_states_t, or -1 if unknown */
    bool     active;                    /* the image is running from here */
    bool     next;                      /* the next update would be written here */
} espix_ota_slot_t;

/* Whether `upgrade` was compiled in. */
bool espix_ota_enabled(void);

/* Whether the running image has a second slot to write an update into. */
bool espix_ota_available(void);

/* Read the scan and report what it found. Never fatal. */
esp_err_t espix_ota_init(void);

/*
 * Confirm the running image if the bootloader is holding it as pending-verify.
 * Called once the system is up; a no-op on a normal boot.
 */
void espix_ota_confirm_boot(void);

/* The running slot's short name ("ota0", "factory", ...). */
void espix_ota_running_slot(char *buf, size_t len);

/* The application partitions, in table order. Returns how many were written. */
size_t espix_ota_slots(espix_ota_slot_t *out, size_t n);

/* Called during an install with progress, in bytes. */
typedef void (*espix_ota_progress_fn)(void *ctx, size_t done, size_t total);

/*
 * Write the image at `path` into the passive slot and select it to boot.
 *
 * On failure `err` carries a sentence worth showing a person, including the
 * figures behind an out-of-memory failure. On success it says which slot was
 * written.
 */
esp_err_t espix_ota_install_file(const char *path,
                                 espix_ota_progress_fn progress, void *ctx,
                                 char *err, size_t err_len);

/* The same, reading the image from an open stream -- an SSH session's stdin, so
 * a locally built image never has to land on the rootfs first. */
esp_err_t espix_ota_install_stream(FILE *in,
                                   espix_ota_progress_fn progress, void *ctx,
                                   char *err, size_t err_len);

/* The same, over the network: an HTTPS URL, or HTTP in a dev build with
 * CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP. Follows redirects, so a GitHub
 * releases/latest/download link works as-is. */
esp_err_t espix_ota_install_url(const char *url,
                                espix_ota_progress_fn progress, void *ctx,
                                char *err, size_t err_len);

/* ------------------------------------------------------------------ */
/* The update repo                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char version[32];       /* semver, as the release was tagged */
    char build[72];         /* the release image's ELF SHA-256 */
    char url[256];          /* where the image is fetched from */
    char sha256[72];        /* optional; the .bin's own digest */
    char min_version[32];   /* optional; refuse to update below this */
} espix_ota_manifest_t;

typedef enum {
    ESPIX_OTA_UPDATE_UP_TO_DATE = 0,
    ESPIX_OTA_UPDATE_AVAILABLE,
    ESPIX_OTA_UPDATE_UNKNOWN,
} espix_ota_update_t;

/*
 * Where updates come from: ota.url in /etc/espix.conf, or the URL this build
 * was configured with. Cached after the first call.
 */
const char *espix_ota_source(void);

/* Fetch and parse the manifest. One small GET; the image is not touched. */
esp_err_t espix_ota_manifest_fetch(const char *url, espix_ota_manifest_t *m,
                                   char *err, size_t err_len);

/*
 * A newer semver is an update; so is the same semver with a different build
 * (a rebuilt or rolling release -- see the note in docs/OTA.md). An older
 * semver never is.
 */
espix_ota_update_t espix_ota_compare(const espix_ota_manifest_t *m);

/* False when the manifest demands a newer espix than this one (min_version). */
bool espix_ota_meets_min(const espix_ota_manifest_t *m);

/* Fetch the manifest and record the verdict for the greeting. */
esp_err_t espix_ota_check(const char *url, espix_ota_manifest_t *m,
                          char *err, size_t err_len);

/*
 * What the last check found, from the cached state only -- this is what the
 * greeting calls, and it must never touch the network. Returns false when no
 * check has found anything newer.
 */
bool espix_ota_known_update(char *version, size_t len);

#ifdef __cplusplus
}
#endif
