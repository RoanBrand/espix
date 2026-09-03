/*
 * File modes: a rule for the common case, a file for the exceptions.
 *
 * LittleFS stores no permission bits. It does store user attributes -- the port
 * keeps mtime in one -- but joltwallet's wrapper never exposes the `lfs_t *`
 * those need (`static esp_littlefs_t * _efs[]` in esp_littlefs.c, with every
 * lookup static and littlefs_api.h under PRIV_INCLUDE_DIRS), and ESP-IDF's VFS
 * has no chmod hook to route one through either. So the mode lives here.
 *
 * Two halves, and the split is what makes this cheap:
 *
 *   - A *rule* supplies the default. Directories are 0755, files that start
 *     with the ELF magic are 0755, everything else is 0644. Nothing is stored,
 *     so the flashed rootfs image needs no mode data and /bin is executable on
 *     a fresh device without anyone having written a byte.
 *
 *   - /etc/modes records only what someone changed with chmod. On a device
 *     nobody has run chmod on, it does not exist. A mode that matches the rule
 *     again is *removed* rather than stored, so the file cannot grow into a
 *     shadow copy of the filesystem.
 *
 * The consequence worth knowing: a flash write happens when you run chmod and
 * at no other time, which matters on a filesystem that pays a block erase per
 * write.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#define TAG "fs"

/*
 * Overrides are held as pointers rather than [ESPIX_PATH_MAX] arrays: at 128
 * bytes a path, a fixed table of 64 would cost 8KB of BSS to hold what is
 * usually nothing at all.
 */
typedef struct {
    char  *path;
    mode_t mode;
} mode_override_t;

static mode_override_t   s_over[ESPIX_MODE_OVERRIDES_MAX];
static size_t            s_over_count;
static bool              s_loaded;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */
/* The rule                                                            */
/* ------------------------------------------------------------------ */

/*
 * True if `abs_path` starts with the ELF magic.
 *
 * This is the same test exec_fallback() used to gate execution on before there
 * were mode bits, moved here so that "is it a program" is asked in one place.
 * It costs an open and a four-byte read per rule-derived file, which `ls -l`
 * pays once per entry -- acceptable because LittleFS caches the block, and
 * because an override short-circuits it entirely.
 */
static bool looks_executable(const char *abs_path)
{
    FILE *f = fopen(abs_path, "rb");
    if (f == NULL) {
        return false;
    }

    char         magic[4] = { 0 };
    const size_t got      = fread(magic, 1, sizeof(magic), f);
    fclose(f);

    return got == sizeof(magic) && memcmp(magic, "\177ELF", sizeof(magic)) == 0;
}

static mode_t mode_from_rule(const char *abs_path, const struct stat *st)
{
    if (S_ISDIR(st->st_mode)) {
        return 0755;
    }
    return looks_executable(abs_path) ? 0755 : 0644;
}

/* ------------------------------------------------------------------ */
/* The override table                                                  */
/* ------------------------------------------------------------------ */

/* Index of `abs_path` in the table, or -1. Caller holds the lock. */
static int over_find(const char *abs_path)
{
    for (size_t i = 0; i < s_over_count; i++) {
        if (strcmp(s_over[i].path, abs_path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Drop entry `i`, closing the gap. Caller holds the lock. */
static void over_drop(size_t i)
{
    free(s_over[i].path);
    s_over[i] = s_over[s_over_count - 1];
    s_over_count--;
}

/* Caller holds the lock. Returns false only if the table is full. */
static bool over_set(const char *abs_path, mode_t mode)
{
    const int i = over_find(abs_path);

    if (i >= 0) {
        s_over[i].mode = mode;
        return true;
    }
    if (s_over_count == ESPIX_MODE_OVERRIDES_MAX) {
        return false;
    }

    char *copy = strdup(abs_path);
    if (copy == NULL) {
        return false;
    }

    s_over[s_over_count].path = copy;
    s_over[s_over_count].mode = mode;
    s_over_count++;
    return true;
}

/*
 * Read /etc/modes. One `path=octal` per line, `#` to end of line, same shape as
 * every other file espix keeps in /etc so that `cat /etc/modes` is useful.
 *
 * A line that does not parse is dropped with a warning rather than failing the
 * mount: this file is meant to be editable by hand, and a typo in it must not
 * cost you a filesystem.
 */
static void modes_load(void)
{
    FILE *f = fopen(ESPIX_MODES_PATH, "r");
    if (f == NULL) {
        return;             /* nobody has run chmod; the rule covers everything */
    }

    char line[ESPIX_PATH_MAX + 16];

    while (fgets(line, sizeof(line), f) != NULL) {
        /* An over-long line arrives in pieces, and the pieces after the first
         * carry no '#'. Drop the remainder rather than parse its tail. */
        if (strchr(line, '\n') == NULL && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
            continue;
        }

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        p[strcspn(p, "\r\n")] = '\0';

        if (*p == '#' || *p == '\0') {
            continue;
        }

        char *eq = strrchr(p, '=');
        if (eq == NULL || eq == p) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: ignoring \"%s\"",
                       ESPIX_MODES_PATH, p);
            continue;
        }
        *eq = '\0';

        char      *end = NULL;
        const long v   = strtol(eq + 1, &end, 8);

        if (end == eq + 1 || *end != '\0' || v < 0 || v > ESPIX_MODE_BITS) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: bad mode for %s",
                       ESPIX_MODES_PATH, p);
            continue;
        }

        if (!over_set(p, (mode_t)v)) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: more than %d overrides; the rest use defaults",
                       ESPIX_MODES_PATH, ESPIX_MODE_OVERRIDES_MAX);
            break;
        }
    }

    fclose(f);

    if (s_over_count > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%u mode override%s from %s",
                   (unsigned)s_over_count, s_over_count == 1 ? "" : "s",
                   ESPIX_MODES_PATH);
    }
}

/*
 * Rewrite /etc/modes from the table, or delete it once the table is empty.
 * Caller holds the lock.
 *
 * Whole-file rewrite rather than append: an entry can be removed as well as
 * added -- chmod'ing something back to its rule-derived mode does exactly that
 * -- and at 64 lines the cost of being simple is nothing.
 */
static esp_err_t modes_save(void)
{
    if (s_over_count == 0) {
        if (unlink(ESPIX_MODES_PATH) != 0 && errno != ENOENT) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    FILE *f = fopen(ESPIX_MODES_PATH, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }

    fprintf(f, "# Written by chmod. Paths whose mode differs from the default:\n"
               "# directories 755, ELF binaries 755, everything else 644.\n");

    for (size_t i = 0; i < s_over_count; i++) {
        fprintf(f, "%s=%03o\n", s_over[i].path, (unsigned)s_over[i].mode);
    }

    return (fclose(f) == 0) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

void espix_fs_mode_init(void)
{
    if (s_loaded) {
        return;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "no memory for the mode table");
            return;
        }
    }

    modes_load();
    s_loaded = true;
}

mode_t espix_fs_mode(const char *abs_path, const struct stat *st)
{
    struct stat own;

    if (abs_path == NULL) {
        return 0;
    }
    if (st == NULL) {
        if (stat(abs_path, &own) != 0) {
            return 0;
        }
        st = &own;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int i = over_find(abs_path);
        if (i >= 0) {
            const mode_t m = s_over[i].mode;
            xSemaphoreGive(s_lock);
            return m;
        }
        xSemaphoreGive(s_lock);
    }

    return mode_from_rule(abs_path, st);
}

bool espix_fs_is_executable(const char *abs_path)
{
    struct stat st;

    if (abs_path == NULL || stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return (espix_fs_mode(abs_path, &st) & S_IXUSR) != 0;
}

esp_err_t espix_fs_chmod(const char *abs_path, mode_t mode)
{
    struct stat st;

    if (abs_path == NULL || (mode & ~(mode_t)ESPIX_MODE_BITS) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const int i = over_find(abs_path);

    /*
     * A mode that the rule already produces is not an override. Removing it
     * instead of storing it is what keeps /etc/modes an exceptions list --
     * `chmod +x` then `chmod -x` on a plain file leaves no trace, as it should.
     */
    if (mode == mode_from_rule(abs_path, &st)) {
        if (i < 0) {
            xSemaphoreGive(s_lock);
            return ESP_OK;              /* nothing stored, nothing to change */
        }
        over_drop((size_t)i);
    } else if (!over_set(abs_path, mode)) {
        xSemaphoreGive(s_lock);
        espix_klog(ESPIX_KLOG_WARN, TAG, "mode table full (%d entries)",
                   ESPIX_MODE_OVERRIDES_MAX);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = modes_save();
    xSemaphoreGive(s_lock);
    return err;
}

void espix_fs_mode_forget(const char *abs_path)
{
    if (abs_path == NULL || s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = over_find(abs_path);
    if (i >= 0) {
        over_drop((size_t)i);
        (void)modes_save();
    }
    xSemaphoreGive(s_lock);
}

void espix_fs_mode_rename(const char *old_abs, const char *new_abs)
{
    if (old_abs == NULL || new_abs == NULL || s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const int i = over_find(old_abs);
    if (i >= 0) {
        char *copy = strdup(new_abs);
        if (copy != NULL) {
            free(s_over[i].path);
            s_over[i].path = copy;
            (void)modes_save();
        }
    }

    xSemaphoreGive(s_lock);
}

/*
 * "-rwxr-xr-x", the way ls writes it.
 *
 * Never emits s, S, t or T. espix stores nine bits, not twelve: setuid, setgid
 * and sticky are each defined in terms of the owner of the file they are set
 * on, and no file here records an owner. Printing those letters would report a
 * protection that nothing implements, to someone reading `ls -l` who would
 * reasonably believe it.
 */
void espix_fs_mode_str(mode_t mode, bool is_dir, char *out, size_t len)
{
    char s[11];

    s[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; i++) {
        /* Bit 8 is owner-read, bit 0 is other-execute. */
        s[1 + i] = (mode & (mode_t)(1u << (8 - i))) ? "rwx"[i % 3] : '-';
    }
    s[10] = '\0';

    strlcpy(out, s, len);
}
