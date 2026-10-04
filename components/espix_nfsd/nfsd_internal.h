#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Shared inside the component: the export table lives with the daemon, and the
 * NFS program needs to read it.
 */

#define NFS_EXPORT_MAX 8
#define NFS_CLIENT_MAX 8

typedef struct {
    uint32_t addr;                  /* network order, as sin_addr is */
    uint32_t mask;
} nfs_client_t;

typedef struct {
    char         path[192];
    bool         ro;
    bool         all;               /* "*" */
    nfs_client_t clients[NFS_CLIENT_MAX];
    int          nclients;
} nfs_export_t;

int                 nfsd_exports_count(void);
const nfs_export_t *nfsd_export(int i);
const nfs_export_t *nfsd_export_for_dir(const char *dir);
bool                nfsd_client_allowed(const nfs_export_t *e, uint32_t src);

/* The reply buffer and the scratch one a read fills; both PSRAM. */
#define NFSD_BUFCAP 16384
#define NFSD_IOCAP  8192
