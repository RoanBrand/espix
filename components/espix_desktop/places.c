/*
 * The places themselves. Two facts from outside: what is mounted, which
 * espix_fs owns, and who it belongs to, which the mount table also records --
 * both are lookups in a table rather than anything that touches a filesystem,
 * so this is safe to call from a timer.
 */

#include <string.h>

#include "espix_fs.h"

#include "espix_places.h"

/* Static, in the order they read. */
static const struct {
    const char *label;
    const char *path;
} s_fixed[] = {
    { "Home", "/home/esp" },
    { "Root", "/"         },
    { "Bin",  "/bin"      },
    { "Etc",  "/etc"      },
};
#define FIXED_N ((int)(sizeof(s_fixed) / sizeof(s_fixed[0])))

/* The account a volume has to belong to. esp, uid 1000. */
#define PLACE_UID 1000

static void base_name(const char *path, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');

    strlcpy(out, (slash != NULL && slash[1] != '\0') ? slash + 1 : path, n);
}

size_t espix_places(espix_place_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    size_t n = 0;

    for (int i = 0; i < FIXED_N && n < max; i++) {
        strlcpy(out[n].label, s_fixed[i].label, sizeof(out[n].label));
        strlcpy(out[n].path, s_fixed[i].path, sizeof(out[n].path));
        out[n].volume = false;
        n++;
    }

    for (size_t i = 0; n < max; i++) {
        char path[ESPIX_PATH_MAX];

        if (espix_fs_mount_at(i, path, sizeof(path)) != ESP_OK) {
            break;                  /* past the last mount */
        }
        if (strncmp(path, "/mnt/", 5) != 0) {
            continue;               /* somewhere the desktop does not show */
        }

        uint16_t uid = 0;
        uint16_t gid = 0;

        if (!espix_fs_mount_owner(path, &uid, &gid) || uid != PLACE_UID) {
            continue;               /* somebody else's volume */
        }

        strlcpy(out[n].path, path, sizeof(out[n].path));
        base_name(path, out[n].label, sizeof(out[n].label));
        out[n].volume = true;
        n++;
    }

    return n;
}
