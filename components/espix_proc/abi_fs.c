/*
 * The filesystem surface for loadable apps.
 *
 * Before this, an app could not open a file. The ELF loader's built-in tables
 * publish `close` and `fwrite` and nothing else of the family -- no `fopen`,
 * `open`, `read`, `stat`, `opendir`, `mkdir`, `unlink` or `rename` -- so an app
 * that touched the filesystem failed to *load*, an unresolved symbol being a
 * load-time error rather than a runtime one. For a system whose point is
 * running cross-compiled apps off its own filesystem, that was the largest gap
 * between the claim and the behaviour.
 *
 * Almost all of it is a plain table with no wrappers, and that is the payoff of
 * espix owning the root VFS. libc's real open(), stat() and opendir() already
 * dispatch into espix's own VFS, so they already get the permission check and
 * the working-directory resolution on the way through. There is nothing left to
 * interpose.
 *
 * Two exceptions, both because ESP-IDF's versions are stubs rather than
 * implementations: chdir() sets errno to ENOSYS and returns -1, and getcwd()
 * unconditionally answers "/" (esp_libc/src/realpath.c). espix has a real
 * per-process working directory, so it publishes its own under those names --
 * neither is in the loader's built-in table, so nothing is being shadowed.
 *
 * chmod is included, having been left out when file modes landed. The reason it
 * was left out was that publishing a permissions call to programs which could
 * not open a file would be scaffolding rather than a feature. That reason is
 * gone.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_elf.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/* ------------------------------------------------------------------ */
/* The two espix has to implement                                      */
/* ------------------------------------------------------------------ */

/*
 * chdir() and getcwd() for an app.
 *
 * These are espix's, not libc's, because IDF's are stubs. They are also the
 * only reason the working directory is visible to an app at all: espix's VFS
 * resolves relative paths against espix_proc_cwd() whether or not the app ever
 * calls these, so a program that just opens "data.txt" works -- but one that
 * wants to move, or to report where it is, needs them.
 *
 * Named with the abi_ prefix and mapped by string below, following
 * abi_signal.c. Defining a function actually called chdir() here would collide
 * with the libc stub at link time.
 */
static int abi_chdir(const char *path)
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
 * chmod() for an app, and this one is not optional either -- for a worse reason
 * than chdir's.
 *
 * ESP-IDF's chmod is `return 0;`, with a comment explaining that
 * std::filesystem throws if it fails so succeeding is friendlier. That means
 * libc's chmod tells an app the mode was set and changes nothing. Exporting it
 * would publish a lie; ENOSYS would at least be true. espix has a real one, so
 * it publishes that.
 *
 * (Same defect shape as pthread_sigmask, which is also a force-linked no-op
 * returning success -- see docs/UPSTREAM.md.)
 */
static int abi_chmod(const char *path, mode_t mode)
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
    default:
        errno = EIO;
        return -1;
    }
}

static char *abi_getcwd(char *buf, size_t size)
{
    const char *cwd = espix_proc_cwd();
    const size_t need = strlen(cwd) + 1;

    /*
     * POSIX also allows buf == NULL to mean "allocate one", which glibc
     * supports and newlib does not. Refusing it rather than half-supporting it:
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

/* ------------------------------------------------------------------ */
/* Export                                                              */
/* ------------------------------------------------------------------ */

static esp_elf_symbol_table_t s_fs_syms[] = {

    /*
     * stdio. `fopen` is the one an app reaches for first, and the rest is what
     * you need to do anything useful once you hold a FILE *.
     *
     * The loader's own table has `fprintf`, `fputc`, `fputs`, `fwrite` and
     * `free` -- output only, which is why an app could print but never read.
     * Not `fopen`, and not `fclose`: the first app to try this failed to load
     * on `fclose` specifically, which is a fair illustration of how narrow the
     * old surface was.
     */
    ESP_ELFSYM_EXPORT(fopen),
    ESP_ELFSYM_EXPORT(fclose),
    ESP_ELFSYM_EXPORT(fread),
    ESP_ELFSYM_EXPORT(fgets),
    ESP_ELFSYM_EXPORT(fseek),
    ESP_ELFSYM_EXPORT(ftell),
    ESP_ELFSYM_EXPORT(rewind),
    ESP_ELFSYM_EXPORT(feof),
    ESP_ELFSYM_EXPORT(ferror),
    ESP_ELFSYM_EXPORT(remove),

    /*
     * POSIX file calls. Unwrapped: each of these enters espix's VFS, which
     * resolves the path against the calling process's working directory and
     * asks espix_fs_access_check() before touching the filesystem.
     */
    ESP_ELFSYM_EXPORT(open),
    ESP_ELFSYM_EXPORT(read),
    ESP_ELFSYM_EXPORT(write),
    ESP_ELFSYM_EXPORT(lseek),
    ESP_ELFSYM_EXPORT(stat),
    ESP_ELFSYM_EXPORT(fstat),
    ESP_ELFSYM_EXPORT(unlink),
    ESP_ELFSYM_EXPORT(rename),
    ESP_ELFSYM_EXPORT(mkdir),
    ESP_ELFSYM_EXPORT(rmdir),
    ESP_ELFSYM_EXPORT(truncate),
    ESP_ELFSYM_EXPORT(ftruncate),
    ESP_ELFSYM_EXPORT(fsync),

    /*
     * `stat` reports the real mode, so an app sees exactly the bits `ls -l`
     * shows -- espix's VFS fills them in for every caller rather than only for
     * its own commands. Which is what makes a `chmod` worth publishing; it is
     * down with chdir and getcwd, because libc's is a no-op that lies.
     */

    /*
     * Directories. `readdir_r` alongside `readdir` for the same reason
     * abi_time.c exports both localtime forms: the reentrant one is what an app
     * should prefer, and the plain one is what most example code calls.
     *
     * `access` is deliberately absent. LittleFS's port never implemented it, so
     * espix's VFS leaves it NULL too, and exporting a call that always fails
     * would be worse than an app failing to load and being told why.
     */
    ESP_ELFSYM_EXPORT(opendir),
    ESP_ELFSYM_EXPORT(readdir),
    ESP_ELFSYM_EXPORT(readdir_r),
    ESP_ELFSYM_EXPORT(closedir),
    ESP_ELFSYM_EXPORT(rewinddir),
    ESP_ELFSYM_EXPORT(telldir),
    ESP_ELFSYM_EXPORT(seekdir),

    /*
     * espix's own, under libc's names, because IDF's three are stubs: chdir
     * fails with ENOSYS, getcwd always answers "/", and chmod silently returns
     * success. See the notes on each.
     */
    { "chdir",  (const void *)abi_chdir },
    { "getcwd", (const void *)abi_getcwd },
    { "chmod",  (const void *)abi_chmod },

    ESP_ELFSYM_END
};

void espix_proc_abi_fs_register(void)
{
    if (esp_elf_register_symbol(s_fs_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish the filesystem to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "filesystem published to apps");
}
