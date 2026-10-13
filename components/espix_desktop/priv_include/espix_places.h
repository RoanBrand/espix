/*
 * Places: the short list of directories the desktop offers, and the volumes it
 * shows. One model, read by two things -- the browser's left-hand column and
 * the icons on the desktop -- so the two cannot disagree about what is plugged
 * in or where it is mounted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "espix_kernel.h"       /* ESPIX_PATH_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* Four fixed places, plus one row per mount; the mount table holds three. */
#define ESPIX_PLACES_MAX       8
#define ESPIX_PLACE_LABEL_MAX  24

typedef struct {
    char label[ESPIX_PLACE_LABEL_MAX];
    char path[ESPIX_PATH_MAX];
    bool volume;                /* a mount, so it also gets a desktop icon */
} espix_place_t;

/*
 * Fill the array with at most max places and return how many.
 *
 * The fixed few come first, then one row per volume mounted under /mnt that
 * belongs to the esp account -- the desktop shows the account's disks, and
 * neither it nor the browser ever mounts anything itself.
 */
size_t espix_places(espix_place_t *out, size_t max);

#ifdef __cplusplus
}
#endif
