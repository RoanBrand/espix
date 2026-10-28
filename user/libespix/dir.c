/* Directories over getdents, and the working directory. */
#include <dirent.h>
#include <fcntl.h>
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
