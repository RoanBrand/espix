#include <string.h>

#include "rpc.h"

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

bool rpc_get_u32(rpc_call_t *c, uint32_t *v)
{
    if (c->pos + 4 > c->len) {
        return false;
    }
    *v = rd32(c->buf + c->pos);
    c->pos += 4;
    return true;
}

bool rpc_get_u64(rpc_call_t *c, uint64_t *v)
{
    uint32_t hi, lo;

    if (!rpc_get_u32(c, &hi) || !rpc_get_u32(c, &lo)) {
        return false;
    }
    *v = ((uint64_t)hi << 32) | lo;
    return true;
}

bool rpc_get_opaque(rpc_call_t *c, void *out, size_t n)
{
    const size_t padded = (n + 3) & ~(size_t)3;

    if (c->pos + padded > c->len) {
        return false;
    }
    if (out != NULL) {
        memcpy(out, c->buf + c->pos, n);
    }
    c->pos += padded;
    return true;
}

bool rpc_get_string(rpc_call_t *c, char *out, size_t cap)
{
    uint32_t n;

    if (!rpc_get_u32(c, &n) || n > 4096) {
        return false;
    }
    const size_t padded = (n + 3) & ~(size_t)3;
    if (c->pos + padded > c->len) {
        return false;
    }
    const size_t copy = (n < cap - 1) ? n : cap - 1;
    if (out != NULL) {
        memcpy(out, c->buf + c->pos, copy);
        out[copy] = 0;
    }
    c->pos += padded;
    return true;
}

bool rpc_parse_call(rpc_call_t *c, const uint8_t *buf, size_t len)
{
    uint32_t v, flavor, clen;

    memset(c, 0, sizeof(*c));
    c->buf = buf;
    c->len = len;

    if (!rpc_get_u32(c, &c->xid)) {
        return false;
    }
    if (!rpc_get_u32(c, &v) || v != RPC_MSG_CALL) {
        return false;
    }
    if (!rpc_get_u32(c, &v) || v != 2) {          /* rpcvers */
        return false;
    }
    if (!rpc_get_u32(c, &c->prog) || !rpc_get_u32(c, &c->vers) ||
        !rpc_get_u32(c, &c->proc)) {
        return false;
    }

    /* The credentials. Their declared length is what says where they end, so
     * that is used rather than parsing every flavour we might be sent. */
    if (!rpc_get_u32(c, &flavor) || !rpc_get_u32(c, &clen)) {
        return false;
    }
    c->cred_flavor = flavor;
    const size_t cred_end = c->pos + ((clen + 3) & ~(size_t)3);
    if (cred_end > c->len) {
        return false;
    }
    if (flavor == RPC_AUTH_SYS) {
        uint32_t stamp, machlen;
        if (rpc_get_u32(c, &stamp) && rpc_get_u32(c, &machlen) &&
            c->pos + ((machlen + 3) & ~(size_t)3) <= cred_end) {
            c->pos += (machlen + 3) & ~(size_t)3;
            if (rpc_get_u32(c, &c->uid) && rpc_get_u32(c, &c->gid)) {
                c->have_auth_sys = true;
            }
        }
    }
    c->pos = cred_end;

    /* The verifier, which is always AUTH_NULL in practice: skip it. */
    if (!rpc_get_u32(c, &flavor) || !rpc_get_u32(c, &clen)) {
        return false;
    }
    if (c->pos + ((clen + 3) & ~(size_t)3) > c->len) {
        return false;
    }
    c->pos += (clen + 3) & ~(size_t)3;
    return true;
}

void xdrw_u32(xdrw_t *w, uint32_t v)
{
    if (!w->ok || w->len + 4 > w->cap) {
        w->ok = false;
        return;
    }
    wr32(w->buf + w->len, v);
    w->len += 4;
}

void xdrw_u64(xdrw_t *w, uint64_t v)
{
    xdrw_u32(w, (uint32_t)(v >> 32));
    xdrw_u32(w, (uint32_t)v);
}

void xdrw_bool(xdrw_t *w, bool v)
{
    xdrw_u32(w, v ? 1 : 0);
}

void xdrw_opaque(xdrw_t *w, const void *p, size_t n)
{
    const size_t padded = (n + 3) & ~(size_t)3;

    if (!w->ok || w->len + padded > w->cap) {
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    memset(w->buf + w->len + n, 0, padded - n);
    w->len += padded;
}

void xdrw_string(xdrw_t *w, const char *s)
{
    const size_t n = (s != NULL) ? strlen(s) : 0;

    xdrw_u32(w, (uint32_t)n);
    if (n > 0) {
        xdrw_opaque(w, s, n);
    }
}

void xdrw_header_only(xdrw_t *w, uint32_t xid)
{
    w->ok = true;
    w->len = 0;
    xdrw_u32(w, xid);
    xdrw_u32(w, RPC_MSG_REPLY);
    xdrw_u32(w, RPC_ACCEPTED);
    xdrw_u32(w, RPC_AUTH_NULL);
    xdrw_u32(w, 0);                 /* the verifier's length */
}

void xdrw_accept(xdrw_t *w, uint32_t accept_stat)
{
    xdrw_u32(w, accept_stat);
}

void xdrw_init(xdrw_t *w, uint8_t *buf, size_t cap, uint32_t xid)
{
    w->buf = buf;
    w->cap = cap;
    w->ok  = true;
    w->len = 0;
    xdrw_header_only(w, xid);
}

size_t rpc_tcp_frame(const uint8_t *msg, size_t len, uint8_t *out, size_t cap)
{
    if (cap < len + 4) {
        return 0;
    }
    wr32(out, (uint32_t)(0x80000000u | (uint32_t)len));
    memcpy(out + 4, msg, len);
    return len + 4;
}
