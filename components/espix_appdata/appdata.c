/*
 * Reading an app's manifest, and fetching what it declares is missing.
 *
 * The whole of the policy is here and nowhere else, which is the point: the
 * desktop and the shell both ask this one function before they spawn anything,
 * so "run it" means the same thing whether it was a double-click or a command
 * line, and a third caller cannot invent a third answer.
 *
 * The format is deliberately line-oriented and boring, because it has to be
 * writable by hand with vi on the device and readable in a bug report:
 *
 *     # doom's shareware data, fetched once on first launch
 *     data  https://host/doom1.wad  /var/lib/doom/doom1.wad  sha256:1d7d...
 *
 * A missing manifest is not an error. Most programs ship everything they need,
 * and asking about every one of them has to cost nothing.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "espix_appdata.h"
#include "espix_kernel.h"
#include "espix_net.h"

#define TAG "appdata"

#define APPDATA_DIR  "/etc/apps"
#define APPDATA_MAX  4          /* data lines honoured in one manifest */
#define LINE_MAX     900        /* url + path + digest, with room to spare */

typedef struct {
    char url[512];
    char path[256];
    char sha256[65];            /* empty when the manifest did not name one */
} appdata_entry_t;

/*
 * A program's name, not a path. The manifest is looked up by concatenation, so
 * anything that could climb out of /etc/apps -- a slash, a leading dot -- is
 * refused here rather than sanitised later.
 */
static bool name_ok(const char *app)
{
    if (app == NULL || app[0] == 0 || app[0] == '.') {
        return false;
    }
    size_t n = 0;
    for (const char *p = app; *p != 0; p++, n++) {
        const char c = *p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') ||
                           c == '.' || c == '_' || c == '-';
        if (!plain || n >= 63) {
            return false;
        }
    }
    return true;
}

static bool hex64(const char *s)
{
    for (int i = 0; i < 64; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return s[64] == 0;
}

/*
 * Parse the manifest into out. Returns how many data lines it declared, which
 * is zero for a file that only has comments -- and zero as well when the file
 * does not exist, because a caller cannot act differently on the two.
 *
 * The line buffer is the heap's, and the entries the caller's. The task that
 * gets here is the one serving an SSH connection, whose stack is 8 KB and
 * already holds a shell and a TLS handshake by the time a download starts: a
 * megabyte of manifest on the stack is a stack protection fault, which is
 * exactly how this was found.
 */
static size_t manifest_read(const char *file, appdata_entry_t *out, size_t max)
{
    FILE *f = fopen(file, "rb");
    if (f == NULL) {
        return 0;
    }

    char *line = malloc(LINE_MAX);
    if (line == NULL) {
        fclose(f);
        return 0;
    }

    size_t n = 0;

    while (fgets(line, LINE_MAX, f) != NULL) {
        char *nl = strpbrk(line, "\r\n");
        if (nl != NULL) {
            *nl = 0;
        }

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == 0 || *p == '#') {
            continue;
        }

        char key[32] = "", url[512] = "", path[256] = "", hash[80] = "";
        const int got = sscanf(p, "%31s %511s %255s %79s", key, url, path, hash);
        if (got < 3) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot parse '%s'", file, p);
            continue;
        }
        if (strcmp(key, "data") != 0) {
            /* Named rather than ignored: a typo in a manifest is otherwise a
             * download that never happens, for a reason nothing reports. */
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: unknown key '%s'", file, key);
            continue;
        }
        if (path[0] != '/') {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: '%s' is not an absolute path",
                       file, path);
            continue;
        }
        if (hash[0] != 0 && (strncmp(hash, "sha256:", 7) != 0 ||
                             !hex64(hash + 7))) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: '%s' is not sha256:<hex>",
                       file, hash);
            continue;
        }
        if (n == max) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: more than %u data lines",
                       file, (unsigned)max);
            break;
        }

        strlcpy(out[n].url, url, sizeof(out[n].url));
        strlcpy(out[n].path, path, sizeof(out[n].path));
        strlcpy(out[n].sha256, (hash[0] != 0) ? hash + 7 : "",
                sizeof(out[n].sha256));
        n++;
    }

    free(line);
    fclose(f);
    return n;
}

static bool present(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

/*
 * Make sure the directory the destination lives in exists.
 *
 * A manifest names a file, not a directory, and /var/lib/doom is not in the
 * image -- a fresh board has no /var/lib at all. An installer creates what it
 * installs into, so this does, one component at a time and top-down. It is
 * ordinary libc mkdir(), so it goes through espix's own VFS and the permission
 * check applies: a session that may not write under /var is refused here rather
 * than halfway through a four megabyte download.
 */
static int ensure_parent(const char *path)
{
    char dir[256];
    strlcpy(dir, path, sizeof(dir));

    char *slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir) {
        return 0;                   /* the root itself, or a bare name */
    }
    *slash = 0;

    for (char *p = dir + 1; *p != 0; p++) {
        if (*p != '/') {
            continue;
        }
        *p = 0;
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            const int e = errno;    /* reported, not stepped over: a refused
                                     * mkdir is what a four megabyte write would
                                     * have failed with anyway, one step later */
            *p = '/';
            return e;
        }
        *p = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return errno;
    }
    return 0;
}

static espix_appdata_status_t map(espix_fetch_status_t st)
{
    switch (st) {
    case ESPIX_FETCH_NO_ROOM:   return ESPIX_APPDATA_NO_ROOM;
    case ESPIX_FETCH_NO_NET:    return ESPIX_APPDATA_NO_NET;
    case ESPIX_FETCH_BAD_HASH:  return ESPIX_APPDATA_BAD_HASH;
    default:                    return ESPIX_APPDATA_IO;
    }
}

espix_appdata_status_t espix_appdata_ensure(const char *app,
                                            espix_appdata_progress_fn progress,
                                            void *ctx,
                                            espix_appdata_info_t *info)
{
    espix_appdata_info_t local = { 0 };
    if (info == NULL) {
        info = &local;
    }

    if (!name_ok(app)) {
        return ESPIX_APPDATA_OK;
    }

    char file[128];
    snprintf(file, sizeof(file), APPDATA_DIR "/%s.conf", app);

    /* Off the stack for the same reason the line buffer is: see above. */
    appdata_entry_t *entries =
        malloc(APPDATA_MAX * sizeof(appdata_entry_t));
    if (entries == NULL) {
        info->err = ENOMEM;
        return ESPIX_APPDATA_IO;
    }

    const size_t n = manifest_read(file, entries, APPDATA_MAX);

    for (size_t i = 0; i < n; i++) {
        if (present(entries[i].path)) {
            continue;
        }

        strlcpy(info->path, entries[i].path, sizeof(info->path));

        const int derr = ensure_parent(entries[i].path);
        if (derr != 0) {
            info->err = derr;
            free(entries);
            return ESPIX_APPDATA_IO;
        }

        if (progress != NULL) {
            /* Told before the first byte, so a caller can put up a panel: a
             * four megabyte download nobody can see is a hang. */
            progress(ctx, entries[i].path, 0, 0);
        }

        espix_fetch_info_t fi = { 0 };
        const espix_fetch_status_t st =
            espix_net_fetch(entries[i].url, entries[i].path, entries[i].sha256,
                            progress, ctx, &fi);

        info->need     = fi.need;
        info->free_now = fi.free_now;
        info->written  = fi.written;
        info->err      = fi.err;

        if (st != ESPIX_FETCH_OK) {
            free(entries);
            return map(st);
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: %u bytes fetched for %s",
                   entries[i].path, (unsigned)fi.written, app);
    }

    free(entries);
    return ESPIX_APPDATA_OK;
}
