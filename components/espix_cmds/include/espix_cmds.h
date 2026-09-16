/*
 * espix built-in commands.
 *
 * Commands are plain functions registered into the espix_shell registry. They
 * take the session they were invoked from, so they must write with
 * espix_printf()/espix_puts() rather than printf() — otherwise their output
 * goes to the console instead of the session (or the file) that asked for it.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register every built-in. Call once, after the filesystem is mounted. */
void espix_cmds_register_all(void);

/*
 * The USB device behind any mount of it has gone.
 *
 * Marks those mounts dead -- so anything still reading fails instead of touching
 * a block device espix_usb is about to release -- and unmounts them if nothing is
 * open. Called from espix_usb's detach hook, through main, which is the only
 * place that knows both the device names and the mount table.
 */
void espix_blk_device_gone(const char *dev);

/*
 * A storage device has appeared: apply /etc/fstab to it.
 *
 * Created here, in the file's own comments, on first use -- and created with
 * everything commented out, because the volume count is 2 and a wildcard on a
 * four-partition stick would ask for more than that.
 */
void espix_blk_device_added(const char *dev);

#ifdef __cplusplus
}
#endif
