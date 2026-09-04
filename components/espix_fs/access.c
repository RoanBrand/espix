/*
 * Who is asking, and may they.
 *
 * This is the seam that did not exist until espix owned the root VFS. Before,
 * an app's fopen() went libc -> ESP-IDF's VFS -> joltwallet's littlefs port
 * without a line of espix code on the path, so there was nowhere to ask the
 * question. Checking inside `cat` instead would have been a boundary you step
 * around by running a program.
 *
 * It answers "allow" for everything today, and that is deliberate rather than
 * unfinished. Enforcement needs a *subject* to compare a mode against, and no
 * file records an owner yet -- see the uid/gid item in docs/ROADMAP.md. Landing
 * the seam on its own means the hard part, being on the path at all for apps as
 * well as commands, is provable before any policy sits on top of it.
 *
 * The DEBUG log is how that gets proved: `cat /etc/motd` and a loaded app
 * should both appear here, and only the second one is a case no other seam in
 * espix can see.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"

#include "espix_fs_priv.h"

#define TAG "fs"

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

int espix_fs_access_check(const char *abs_path, espix_fs_access_t op, int flags)
{
    if (abs_path == NULL) {
        return EINVAL;
    }

    /*
     * A task espix does not know as a process is espix itself -- the console,
     * an SSH connection task, SNTP, the WiFi driver. Those are the kernel, and
     * gating them would mean failing to read /etc/passwd at boot.
     *
     * Takes no lock: espix_proc_pid_of_task() only reads the table, and this
     * runs on the path of every file operation in the system.
     */
    const espix_pid_t pid = espix_proc_pid_of_task(xTaskGetCurrentTaskHandle());
    if (pid == ESPIX_PID_NONE) {
        return 0;
    }

    /*
     * A process, so there is an identity to hold it to. Nothing compares
     * against it yet; the log is what proves this path is reached, including
     * from a loaded app, which is the case that motivated the whole change.
     */
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "pid %d: %s %s (flags 0x%x)",
               (int)pid, op_name(op), abs_path, (unsigned)flags);

    return 0;
}
