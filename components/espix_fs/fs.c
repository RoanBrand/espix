/*
 * espix root filesystem: mount, layout, path resolution.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#include "espix_fs_priv.h"

#define TAG "fs"

/* Created on first boot so a freshly-formatted filesystem still looks sane,
 * even without the baked fsroot image. */
static const char *const k_skeleton[] = {
    "/bin", "/etc", "/home", "/tmp", "/var", "/var/log",
};

static bool s_mounted;

bool espix_fs_is_mounted(void)
{
    return s_mounted;
}

static void ensure_skeleton(void)
{
    for (size_t i = 0; i < sizeof(k_skeleton) / sizeof(k_skeleton[0]); i++) {
        if (mkdir(k_skeleton[i], 0755) == 0) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "created %s", k_skeleton[i]);
        } else if (errno != EEXIST) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "mkdir %s failed: %s",
                       k_skeleton[i], strerror(errno));
        }
    }
}

esp_err_t espix_fs_mount_root(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    /*
     * Two steps, and keeping them apart is the point.
     *
     * esp_littlefs_mount() mounts the filesystem and hands back the driver
     * without registering it at any base path, so littlefs is live and
     * addressable by no path at all. espix then registers its own VFS at ""
     * -- the fallback, which is what makes a filesystem the root -- and calls
     * littlefs through the returned ops.
     *
     * That ordering is what gives espix somewhere to stand: every file call in
     * the system now passes through espix_fs_access_check() before reaching
     * the filesystem, including an app's fopen(), which used to bypass espix
     * entirely. See vfs.c, and docs/ARCHITECTURE.md for why the check belongs
     * at this layer rather than inside a filesystem.
     *
     * Registering littlefs at a path as well would undo it: that path would
     * reach the filesystem with the checks skipped.
     *
     * base_path is "" rather than NULL so the port's F_GETPATH answers with
     * the same absolute paths espix uses.
     */
    const esp_vfs_littlefs_conf_t conf = {
        .base_path              = "",
        .partition_label        = ESPIX_FS_ROOT_PARTITION,
        .format_if_mount_failed = true,
        .grow_on_mount          = true,
    };

    const esp_vfs_fs_ops_t *lower_ops = NULL;
    void                   *lower_ctx = NULL;

    esp_err_t err = esp_littlefs_mount(&conf, &lower_ops, &lower_ctx);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "rootfs mount failed: %s",
                   esp_err_to_name(err));
        return err;
    }

    err = espix_vfs_register_root(lower_ops, lower_ctx);
    if (err != ESP_OK) {
        return err;
    }

    s_mounted = true;
    ensure_skeleton();

    /* No chdir() here on purpose. ESP-IDF has no process-wide working
     * directory: chdir() is a hardcoded ENOSYS stub and getcwd() always answers
     * "/" (esp_libc/src/realpath.c). espix's cwd lives only in
     * espix_session_t.cwd, and every command resolves through
     * espix_fs_resolve() to an absolute path before touching the filesystem —
     * which is why nothing here ever depended on the syscall. */

    espix_fs_info_t info;
    if (espix_fs_stat_root(&info) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "rootfs mounted, %u KB used of %u KB",
                   (unsigned)(info.used_bytes / 1024),
                   (unsigned)(info.total_bytes / 1024));
    } else {
        espix_klog(ESPIX_KLOG_INFO, TAG, "rootfs mounted");
    }

    return ESP_OK;
}

esp_err_t espix_fs_stat_root(espix_fs_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_littlefs_info(ESPIX_FS_ROOT_PARTITION,
                             &out->total_bytes, &out->used_bytes);
}

/*
 * Normalise an absolute path in place: collapse "//", drop ".", resolve ".."
 * by popping the previous segment, and strip any trailing slash. `buf` must
 * already start with '/'.
 */
static void normalise(char *buf)
{
    char *out = buf;         /* write cursor; buf[0] == '/' stays put */
    const char *in = buf + 1;

    out++;   /* keep the leading slash */

    while (*in != '\0') {
        /* Start of a segment; skip redundant separators. */
        if (*in == '/') {
            in++;
            continue;
        }

        const char *seg = in;
        while (*in != '\0' && *in != '/') {
            in++;
        }
        const size_t seg_len = (size_t)(in - seg);

        if (seg_len == 1 && seg[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            /* Pop the last written segment, if any. */
            while (out > buf + 1 && out[-1] != '/') {
                out--;
            }
            if (out > buf + 1) {
                out--;      /* drop the separator too */
            }
            continue;
        }

        if (out > buf + 1) {
            *out++ = '/';
        }
        memmove(out, seg, seg_len);
        out += seg_len;
    }

    *out = '\0';
}

esp_err_t espix_fs_resolve(const char *cwd, const char *path,
                           char *out, size_t out_len)
{
    if (out == NULL || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cwd == NULL || cwd[0] == '\0') {
        cwd = "/";
    }

    int n;
    if (path == NULL || path[0] == '\0') {
        n = snprintf(out, out_len, "%s", cwd);
    } else if (path[0] == '/') {
        n = snprintf(out, out_len, "%s", path);
    } else if (strcmp(cwd, "/") == 0) {
        n = snprintf(out, out_len, "/%s", path);
    } else {
        n = snprintf(out, out_len, "%s/%s", cwd, path);
    }

    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    normalise(out);
    return ESP_OK;
}

bool espix_fs_within(const char *abs_path, const char *dir)
{
    if (abs_path == NULL || dir == NULL || dir[0] == '\0') {
        return false;
    }
    if (strcmp(dir, "/") == 0) {
        return true;            /* contains everything */
    }

    const size_t len = strlen(dir);
    if (strncmp(abs_path, dir, len) != 0) {
        return false;
    }
    return abs_path[len] == '\0' || abs_path[len] == '/';
}

esp_err_t espix_fs_rm_rf(const char *abs_path)
{
    if (abs_path == NULL || abs_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!S_ISDIR(st.st_mode)) {
        return (unlink(abs_path) == 0) ? ESP_OK : ESP_FAIL;
    }

    DIR *dir = opendir(abs_path);
    if (dir == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char child[ESPIX_PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", abs_path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        /* Recursion depth is bounded by path length, which is bounded by
         * ESPIX_PATH_MAX, so this cannot run away. */
        err = espix_fs_rm_rf(child);
        if (err != ESP_OK) {
            break;
        }
    }

    closedir(dir);

    if (err != ESP_OK) {
        return err;
    }
    return (rmdir(abs_path) == 0) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Config files                                                        */
/* ------------------------------------------------------------------ */

bool espix_fs_conf_get(const char *path, const char *key,
                       char *out, size_t len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }

    const size_t key_len = strlen(key);
    char  line[128];
    bool  found = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        /*
         * A line longer than the buffer arrives in pieces, and the pieces
         * after the first carry no leading '#' -- so the tail of a long
         * comment can look like a setting. Drop the remainder and skip it.
         * Nothing legitimate here is this long.
         */
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
        if (*p == '#' || *p == '\0') {
            continue;
        }
        if (strncmp(p, key, key_len) != 0 || p[key_len] != '=') {
            continue;
        }

        p += key_len + 1;
        p[strcspn(p, "\r\n")] = '\0';
        strlcpy(out, p, len);
        found = true;
        break;      /* first match wins */
    }

    fclose(f);
    return found;
}
