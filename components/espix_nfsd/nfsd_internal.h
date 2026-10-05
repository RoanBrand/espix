#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Shared inside the component: the export table lives with the daemon, and the
 * NFS program needs to read it.
 */

#define NFS_EXPORT_MAX 8
#define NFS_CLIENT_MAX 8

/* How a client's identity is treated, under Linux's own names. */
typedef enum {
    NFS_SQUASH_ROOT,                /* root_squash: a client's root is nobody */
    NFS_SQUASH_NONE,                /* no_root_squash */
    NFS_SQUASH_ALL,                 /* all_squash: everyone is nobody */
} nfs_squash_t;

/* What Linux calls nobody, and the anonuid/anongid default. */
#define NFS_ANON_UID 65534
#define NFS_ANON_GID 65534

/*
 * One client rule: who may mount, and what they may do. Options are per client
 * because that is what /etc/exports is for -- one host writable, the world
 * read-only -- and "*" is a rule like any other, matching with a zero mask, so
 * addresses and options are read by the same code for both.
 */
typedef struct {
    uint32_t     addr;                  /* network order, as sin_addr is */
    uint32_t     mask;                  /* 0 matches everyone */
    bool         ro;
    nfs_squash_t squash;
    uint16_t     anon_uid, anon_gid;
} nfs_client_t;

typedef struct {
    char         path[192];
    nfs_client_t clients[NFS_CLIENT_MAX];
    int          nclients;
} nfs_export_t;

int                 nfsd_exports_count(void);
const nfs_export_t *nfsd_export(int i);
const nfs_export_t *nfsd_export_for_dir(const char *dir);
bool                nfsd_client_allowed(const nfs_export_t *e, uint32_t src);

/* The rule that governs one client: the most specific match wins, so a "*" line
 * and a host line can sit together and the host gets what it says. NULL when
 * the export does not list the client at all. */
const nfs_client_t *nfsd_client_rule(const nfs_export_t *e, uint32_t src);
bool                nfsd_may_write(const nfs_export_t *e, uint32_t src);

/* The reply buffer and the scratch one a read fills; both PSRAM. */
#define NFSD_BUFCAP 16384
#define NFSD_IOCAP  8192
