/*
 * The working directory, chmod and dup for an app's syscalls (umode_sys.c): the
 * calls IDF's libc answers with stubs (chdir is ENOSYS, getcwd always "/", chmod
 * a no-op that reports success), so espix answers them itself.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

int espix_abi_chdir(const char *path)
{
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Resolve against where the process already is, so chdir("..") and
     * chdir("subdir") work rather than only absolute paths. Same normalisation
     * the shell's `cd` uses, so the two agree.
     */
    char abs[ESPIX_PATH_MAX];
    if (espix_fs_resolve(espix_proc_cwd(), path, abs, sizeof(abs)) != ESP_OK) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /*
     * No root check here, and it is not an oversight: espix_proc_chdir() stats
     * the target, that stat goes through espix's VFS, and the VFS refuses a
     * path outside the process's root with ENOENT -- so a confined process
     * cannot move out, and gets the same answer it would for a directory that
     * is not there. espix_abi_chmod() below is the one that needed its own check,
     * because espix_fs_chmod() never re-enters the VFS.
     */
    switch (espix_proc_chdir(abs)) {
    case ESP_OK:
        return 0;
    case ESP_ERR_NOT_FOUND:
        errno = ENOENT;
        return -1;
    case ESP_ERR_INVALID_STATE:
        /* A kernel task, which has no working directory to move. An app can
         * never see this; it is here so the behaviour is defined. */
        errno = ENOSYS;
        return -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

/*
 * The root applies here through espix_fs_admin_check(), not through the VFS.
 * This resolves its own path and calls espix_fs_chmod() directly, so resolve()
 * never sees it -- the same reason espix_fs_admin_check() exists for ownership.
 * Without that check a confined process could re-mode any file it owns
 * anywhere on the device.
 */
int espix_abi_chmod(const char *path, mode_t mode)
{
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    char abs[ESPIX_PATH_MAX];
    if (espix_fs_resolve(espix_proc_cwd(), path, abs, sizeof(abs)) != ESP_OK) {
        errno = ENAMETOOLONG;
        return -1;
    }

    switch (espix_fs_chmod(abs, mode & ESPIX_MODE_BITS)) {
    case ESP_OK:
        return 0;
    case ESP_ERR_NOT_FOUND:
        errno = ENOENT;
        return -1;
    case ESP_ERR_INVALID_ARG:
        errno = EINVAL;
        return -1;
    case ESP_ERR_NOT_ALLOWED:
        /* A mode change the caller may not make. */
        errno = EACCES;
        return -1;
    default:
        errno = EIO;
        return -1;
    }
}

char *espix_abi_getcwd(char *buf, size_t size)
{
    const char *cwd = espix_proc_cwd();
    const size_t need = strlen(cwd) + 1;

    /*
     * POSIX also allows buf == NULL to mean "allocate one", which glibc
     * supports and not every libc does. Refusing it rather than half-supporting it:
     * an app that wants that can pass a buffer.
     */
    if (buf == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (size < need) {
        errno = ERANGE;
        return NULL;
    }

    memcpy(buf, cwd, need);
    return buf;
}

/* dup() for the syscall layer: a pipe end, or a second descriptor carrying the same key (vfs.c). */
int espix_abi_dup(int fd)
{
    return espix_pipe_is(fd) ? espix_pipe_dup(fd) : espix_fs_dup_fd(fd);
}
