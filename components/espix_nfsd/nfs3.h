#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rpc.h"

/*
 * The NFS program, version 3: enough of it to walk a tree and read from it.
 * The reply is written into w, which already holds the RPC header.
 */
size_t nfs3_handle(const rpc_call_t *c, xdrw_t *w, uint8_t *io, size_t iocap);

/* The handle for an export's root, which is what MNT returns. */
bool   nfs3_fh_for_export(int exp, uint8_t *fh, size_t *len);

/* The address the current request came from, set once per request. */
void   nfs3_set_source(uint32_t src);

/*
 * Give the handle table back. Valid to call exactly when no client can hold a
 * handle any more -- the last one has unmounted, or the daemon is stopping --
 * and never on a guess about idleness: a client using the handles it has mints
 * none, so its silence says nothing.
 */
void   nfs3_slots_cleanup(void);
