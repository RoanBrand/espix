#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ONC RPC, version 2, and just enough XDR to speak NFSv3. */

#define RPC_MSG_CALL     0
#define RPC_MSG_REPLY    1
#define RPC_ACCEPTED     0
#define RPC_SUCCESS      0
#define RPC_PROG_UNAVAIL 1
#define RPC_PROG_MISMATCH 2
#define RPC_PROC_UNAVAIL 3
#define RPC_GARBAGE_ARGS 4
#define RPC_SYSTEM_ERR   5

#define RPC_AUTH_NULL 0
#define RPC_AUTH_SYS  1

#define RPC_PROT_TCP 6
#define RPC_PROT_UDP 17

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;          /* where the arguments begin, once parsed */
    uint32_t       xid;
    uint32_t       prog, vers, proc;
    uint32_t       cred_flavor;
    uint32_t       uid, gid;
    bool           have_auth_sys;
} rpc_call_t;

bool rpc_parse_call(rpc_call_t *c, const uint8_t *buf, size_t len);
bool rpc_get_u32(rpc_call_t *c, uint32_t *v);
bool rpc_get_u64(rpc_call_t *c, uint64_t *v);
bool rpc_get_opaque(rpc_call_t *c, void *out, size_t n);
bool rpc_get_string(rpc_call_t *c, char *out, size_t cap);

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     ok;
} xdrw_t;

void xdrw_init(xdrw_t *w, uint8_t *buf, size_t cap, uint32_t xid);
void xdrw_u32(xdrw_t *w, uint32_t v);
void xdrw_u64(xdrw_t *w, uint64_t v);
void xdrw_bool(xdrw_t *w, bool v);
void xdrw_opaque(xdrw_t *w, const void *p, size_t n);
void xdrw_string(xdrw_t *w, const char *s);
void xdrw_accept(xdrw_t *w, uint32_t accept_stat);
void xdrw_header_only(xdrw_t *w, uint32_t xid);   /* xid + reply + accepted + verf */

/* A TCP request is one or more record-marked fragments; this writes one. */
size_t rpc_tcp_frame(const uint8_t *msg, size_t len, uint8_t *out, size_t cap);
