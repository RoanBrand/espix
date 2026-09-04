/*
 * Who is asking, and may they.
 *
 * This is the seam that did not exist until espix owned the root VFS. Before,
 * an app's fopen() went libc -> ESP-IDF's VFS -> joltwallet's littlefs port
 * without a line of espix code on the path, so there was nowhere to ask the
 * question. Checking inside `cat` instead would have been a boundary you step
 * around by running a program.
 *
 * Finding the subject, in order:
 *
 *   1. A task that has raised privilege. espix_auth reaching /etc/passwd, and
 *      nothing else; see espix_fs_priv_begin().
 *   2. A process, whose credentials were copied from its session at spawn.
 *      Copied, not followed: a backgrounded process outlives the command that
 *      started it, and the session lives on that command's stack.
 *   3. The task's current session, which covers builtins -- `cat`, `rm` and the
 *      rest run in the session task, so without this a shell user would be
 *      unchecked and the mode bits would be advice rather than policy.
 *   4. Nothing: espix itself. SNTP, the WiFi driver, an SSH connection task
 *      before it has a session. Those are the kernel and are allowed, which is
 *      also what stops boot failing to read its own configuration.
 *
 * What is deliberately not checked: search permission on every intermediate
 * directory. Unix requires `x` on each component of a path; espix checks the
 * final one, and the parent for anything that creates or removes a name. A full
 * walk costs a stat per component on every file operation in the system, and
 * mode_from_rule() already pays an open-and-read per rule-derived file. The gap
 * is written down in docs/KNOWN-ISSUES.md rather than left to be discovered:
 * a directory whose mode says 0700 does not hide what is underneath it from
 * someone who knows the full path.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"

#include "espix_fs_priv.h"

#define TAG "fs"

/* Beside ESPIX_TLS_SESSION_IDX, which espix_shell owns; both need
 * CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS to be larger than they are. */
#define ESPIX_TLS_FSPRIV_IDX 2

void espix_fs_priv_begin(void)
{
    const uintptr_t depth = (uintptr_t)pvTaskGetThreadLocalStoragePointer(
        NULL, ESPIX_TLS_FSPRIV_IDX);
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_FSPRIV_IDX,
                                      (void *)(depth + 1));
}

void espix_fs_priv_end(void)
{
    const uintptr_t depth = (uintptr_t)pvTaskGetThreadLocalStoragePointer(
        NULL, ESPIX_TLS_FSPRIV_IDX);
    if (depth > 0) {
        vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_FSPRIV_IDX,
                                          (void *)(depth - 1));
    }
}

static bool privileged(void)
{
    return pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_FSPRIV_IDX) != NULL;
}

static const char *op_name(espix_fs_access_t op)
{
    switch (op) {
    case ESPIX_FS_ACCESS_OPEN:     return "open";
    case ESPIX_FS_ACCESS_OPENDIR:  return "opendir";
    case ESPIX_FS_ACCESS_UNLINK:   return "unlink";
    case ESPIX_FS_ACCESS_RENAME:   return "rename";
    case ESPIX_FS_ACCESS_MKDIR:    return "mkdir";
    case ESPIX_FS_ACCESS_RMDIR:    return "rmdir";
    case ESPIX_FS_ACCESS_TRUNCATE: return "truncate";
    }
    return "?";
}

/*
 * The caller's identity, or false if there is nobody to hold to account.
 */
static bool subject(uint16_t *uid, uint16_t *gid)
{
    if (espix_proc_cred_of_task(xTaskGetCurrentTaskHandle(), uid, gid)) {
        return true;
    }

    const espix_session_t *s = espix_shell_current();
    if (s != NULL) {
        *uid = s->uid;
        *gid = s->gid;
        return true;
    }

    return false;
}

/*
 * Which triad applies, and whether it carries the bits in `want`.
 *
 * `want` is in owner-triad terms (S_IRUSR, S_IWUSR, S_IXUSR); the shift picks
 * the group or other copy of the same three bits. Owner beats group beats
 * other, and the first match is final -- a file whose mode is 0077 is
 * unreadable *to its owner*, which is Unix behaviour and surprises people, but
 * is what "the owner triad applies to the owner" means.
 */
static bool permitted(mode_t mode, uint16_t f_uid, uint16_t f_gid,
                      uint16_t uid, uint16_t gid, mode_t want)
{
    int shift;

    if (uid == f_uid) {
        shift = 0;
    } else if (gid == f_gid) {
        shift = 3;
    } else {
        shift = 6;
    }

    return (mode & (want >> shift)) == (want >> shift);
}

/* The directory holding `abs_path`, or "/" for a path directly in the root. */
static void parent_of(const char *abs_path, char *out, size_t len)
{
    strlcpy(out, abs_path, len);

    char *slash = strrchr(out, '/');
    if (slash == NULL || slash == out) {
        strlcpy(out, "/", len);
        return;
    }
    *slash = '\0';
}

/* Does `uid` hold `want` on `abs_path`? */
static bool may(const char *abs_path, uint16_t uid, uint16_t gid, mode_t want)
{
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        /*
         * Nothing there to have a mode. Let the operation proceed and fail on
         * its own terms -- reporting EACCES for a path that does not exist
         * would tell a caller which paths exist, one probe at a time.
         */
        return true;
    }

    uint16_t f_uid = 0;
    uint16_t f_gid = 0;
    espix_fs_owner(abs_path, &st, &f_uid, &f_gid);

    return permitted(espix_fs_mode(abs_path, &st), f_uid, f_gid, uid, gid, want);
}

/*
 * The extra rule a sticky directory adds: write permission on it lets you add
 * names, but you may only remove one whose file you own.
 *
 * This is what makes a 1777 /tmp shareable rather than a free-for-all -- without
 * it, "everyone may write the directory" means everyone may delete everyone
 * else's files. Owning the directory itself is also enough, which is how root
 * clears /tmp.
 */
static bool sticky_permits(const char *parent, const char *abs_path,
                           uint16_t uid)
{
    struct stat pst;
    if (stat(parent, &pst) != 0) {
        return true;
    }
    if ((espix_fs_mode(parent, &pst) & S_ISVTX) == 0) {
        return true;            /* not sticky; the directory check was enough */
    }

    uint16_t d_uid = 0;
    espix_fs_owner(parent, &pst, &d_uid, NULL);
    if (uid == d_uid) {
        return true;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0) {
        return true;            /* nothing there; let it fail on its own terms */
    }

    uint16_t f_uid = 0;
    espix_fs_owner(abs_path, &st, &f_uid, NULL);

    return uid == f_uid;
}

int espix_fs_admin_check(const char *abs_path, bool changing_owner)
{
    uint16_t uid = 0;
    uint16_t gid = 0;

    if (abs_path == NULL) {
        return EINVAL;
    }
    if (privileged() || !subject(&uid, &gid) || uid == 0) {
        return 0;
    }
    if (changing_owner) {
        /*
         * Giving a file away is not the owner's to do. Unix restricts it to
         * root because otherwise anyone can escape a quota, or plant a file
         * under someone else's name.
         */
        return EPERM;
    }

    uint16_t f_uid = 0;
    uint16_t f_gid = 0;
    espix_fs_owner(abs_path, NULL, &f_uid, &f_gid);

    return (uid == f_uid) ? 0 : EPERM;
}

void espix_fs_claim(const char *abs_path)
{
    uint16_t uid = 0;
    uint16_t gid = 0;

    /*
     * espix creating something of its own leaves the rule to answer, which is
     * what keeps a boot that writes /etc/passwd or mkdir's the skeleton from
     * stamping an attribute onto every one of them.
     */
    if (abs_path == NULL || privileged() || !subject(&uid, &gid)) {
        return;
    }

    /*
     * Ask the rule before writing anything. A user creating a file in their own
     * home -- which is very nearly every file anyone creates -- is already
     * owned correctly by the rule, so there is nothing to store and no reason
     * to reach into the filesystem's metadata at all.
     *
     * That is not only an optimisation. This runs from inside vfs_open(), with
     * the file the caller just created still open, and a littlefs attribute
     * write against an open entry is the case littlefs#1076 is about and the
     * one espix has not established is safe. Doing it only when the answer
     * would actually differ keeps the ordinary path clear of it entirely.
     */
    uint16_t rule_uid = 0;
    uint16_t rule_gid = 0;
    espix_fs_owner(abs_path, NULL, &rule_uid, &rule_gid);

    if (rule_uid == uid && rule_gid == gid) {
        return;
    }

    /*
     * Raised over the call: this is espix stamping a file on the creator's
     * behalf, not the creator asking to own something, and espix_fs_chown()
     * now refuses the latter to anyone but root.
     */
    espix_fs_priv_begin();
    (void)espix_fs_chown(abs_path, uid, gid);
    espix_fs_priv_end();
}

int espix_fs_access_check(const char *abs_path, espix_fs_access_t op, int flags)
{
    if (abs_path == NULL) {
        return EINVAL;
    }

    if (privileged()) {
        return 0;
    }

    uint16_t uid = 0;
    uint16_t gid = 0;
    if (!subject(&uid, &gid)) {
        return 0;               /* espix itself */
    }
    if (uid == 0) {
        return 0;               /* root is not asked */
    }

    /*
     * Creating and removing a name is permission on the *directory*, not on the
     * file: it is the directory's list being edited. Getting this backwards is
     * the classic mistake -- it would let anyone delete a file they cannot
     * write, or refuse to let a directory's owner remove a file they do not own.
     */
    bool   on_parent = false;
    mode_t want;

    switch (op) {
    case ESPIX_FS_ACCESS_OPEN:
        want = (flags & (O_WRONLY | O_RDWR | O_APPEND | O_TRUNC)) ? S_IWUSR
                                                                  : S_IRUSR;
        if (flags & O_CREAT) {
            /* Only a create needs the directory; opening what is already there
             * does not. Checked below on the file itself as well, which is
             * harmless when it does not yet exist. */
            char parent[ESPIX_PATH_MAX];
            parent_of(abs_path, parent, sizeof(parent));

            struct stat st;
            if (stat(abs_path, &st) != 0 &&
                !may(parent, uid, gid, S_IWUSR | S_IXUSR)) {
                goto denied;
            }
        }
        break;

    case ESPIX_FS_ACCESS_OPENDIR:
        want = S_IRUSR | S_IXUSR;
        break;

    case ESPIX_FS_ACCESS_TRUNCATE:
        want = S_IWUSR;
        break;

    case ESPIX_FS_ACCESS_UNLINK:
    case ESPIX_FS_ACCESS_RENAME:
    case ESPIX_FS_ACCESS_MKDIR:
    case ESPIX_FS_ACCESS_RMDIR:
        want      = S_IWUSR | S_IXUSR;
        on_parent = true;
        break;

    default:
        return 0;
    }

    if (on_parent) {
        char parent[ESPIX_PATH_MAX];
        parent_of(abs_path, parent, sizeof(parent));
        if (!may(parent, uid, gid, want)) {
            goto denied;
        }
        /*
         * mkdir creates a name rather than removing one, so the sticky rule has
         * nothing to say about it -- it restricts *removal*, not addition.
         */
        if (op != ESPIX_FS_ACCESS_MKDIR &&
            !sticky_permits(parent, abs_path, uid)) {
            goto denied;
        }
        return 0;
    }

    if (!may(abs_path, uid, gid, want)) {
        goto denied;
    }
    return 0;

denied:
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "uid %u denied %s %s (flags 0x%x)",
               (unsigned)uid, op_name(op), abs_path, (unsigned)flags);
    return EACCES;
}
