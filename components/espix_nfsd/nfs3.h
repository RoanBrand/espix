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
