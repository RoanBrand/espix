/* Directories over getdents, and the working directory. */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sys.h"

typedef struct {
    DIR           dir;      /* picolibc's public head */
    int           fd;
    int           pos, len;
    struct dirent ent;
    char          buf[512] __attribute__((aligned(4)));
} espix_dir_t;

int dirfd(DIR *dir)
{
    if (dir == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ((espix_dir_t *)dir)->fd;
}

DIR *fdopendir(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    espix_dir_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    d->fd = fd;
    return &d->dir;
}

DIR *opendir(const char *path)
{
    const int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return NULL;
    }
    espix_dir_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    d->fd = fd;
    return &d->dir;
}

struct dirent *readdir(DIR *dir)
{
    espix_dir_t *d = (espix_dir_t *)dir;
    if (d->pos >= d->len) {
        const long n = espix_sys3(ESPIX_SYS_GETDENTS, d->fd, d->buf, sizeof(d->buf));
        if (n <= 0) {
            if (n < 0) {
                errno = (int)-n;
            }
            return NULL;
        }
        d->len = (int)n;
        d->pos = 0;
    }
    const struct espix_dirent *e = (const struct espix_dirent *)(d->buf + d->pos);
    d->pos += e->reclen;
    d->ent.d_ino  = e->ino;
    d->ent.d_type = e->type;
    strlcpy(d->ent.d_name, e->name, sizeof(d->ent.d_name));
    return &d->ent;
}

int readdir_r(DIR *dir, struct dirent *entry, struct dirent **result)
{
    const int saved = errno;
    errno = 0;
    struct dirent *e = readdir(dir);
    if (e == NULL) {
        const int err = errno;
        errno = saved;
        *result = NULL;
        return err;
    }
    *entry  = *e;
    *result = entry;
    errno = saved;
    return 0;
}

void rewinddir(DIR *dir)
{
    espix_dir_t *d = (espix_dir_t *)dir;
    d->pos = d->len = 0;
    lseek(d->fd, 0, SEEK_SET);
}

int closedir(DIR *dir)
{
    espix_dir_t *d = (espix_dir_t *)dir;
    const int r = close(d->fd);
    free(d);
    return r;
}

int chdir(const char *path)
{
    return espix_ret(espix_sys1(ESPIX_SYS_CHDIR, path));
}

char *getcwd(char *buf, size_t size)
{
    char *const own = (buf == NULL) ? malloc(size != 0 ? size : (size = 256)) : NULL;
    if (buf == NULL && own == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (espix_ret(espix_sys2(ESPIX_SYS_GETCWD, buf != NULL ? buf : own, size)) < 0) {
        free(own);
        return NULL;
    }
    return buf != NULL ? buf : own;
}

int chmod(const char *path, mode_t mode)
{
    return espix_ret(espix_sys2(ESPIX_SYS_CHMOD, path, mode));
}

/* realpath(): espix has no symlinks, so the path joined to the cwd and normalised is the real one, if it exists. */
char *realpath(const char *path, char *resolved)
{
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    char in[PATH_MAX], out[PATH_MAX];
    if (path[0] == '/') {
        in[0] = '\0';
    } else if (getcwd(in, sizeof(in)) == NULL) {
        return NULL;
    }
    if (strlcat(in, "/", sizeof(in)) >= sizeof(in) || strlcat(in, path, sizeof(in)) >= sizeof(in)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    size_t n = 0;               /* out is "/a/b", without a trailing slash; empty is the root */
    for (const char *seg = in; *seg != '\0';) {
        while (*seg == '/') {
            seg++;
        }
        const char *end = seg;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        const size_t len = (size_t)(end - seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 0 && out[--n] != '/') {
            }
        } else if (len > 0 && !(len == 1 && seg[0] == '.')) {
            if (n + 1 + len >= sizeof(out)) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            out[n++] = '/';
            memcpy(out + n, seg, len);
            n += len;
        }
        seg = end;
    }
    if (n == 0) {
        out[n++] = '/';
    }
    out[n] = '\0';
    struct stat st;
    if (stat(out, &st) != 0) {
        return NULL;
    }
    if (resolved == NULL) {
        return strdup(out);
    }
    memcpy(resolved, out, n + 1);
    return resolved;
}
