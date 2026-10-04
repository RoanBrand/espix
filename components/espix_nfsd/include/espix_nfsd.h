#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * An NFSv3 server: the portmapper, mountd and nfsd, which is what "serve the
 * stick over the network" means to a Linux or macOS client. It is read-only
 * until the write path exists, and an export is what /etc/exports says it is.
 *
 * espix_nfsd_run() is the daemon and does not return while it is serving: the
 * service manager runs it as a unit, the way a Unix daemon runs in the
 * foreground under its supervisor.
 */
esp_err_t espix_nfsd_run(bool (*keep_going)(void));

/* For the command: what /etc/exports gave us. */
int  espix_nfsd_export_count(void);
bool espix_nfsd_export_info(int i, const char **path, bool *ro, const char **who);
