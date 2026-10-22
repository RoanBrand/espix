/*
 * srvsvc and wkssvc -- the share list and who the server is, over the named
 * pipes of those names on IPC$.
 *
 * Opening \\host with no share makes a client ask the server what it shares:
 * Explorer for a bare address, Finder and smbutil view, a phone that has only
 * an address field. They open the srvsvc pipe and speak DCE/RPC to it: a BIND,
 * then NetrShareEnum, NetrShareGetInfo or NetrServerGetInfo. Windows also asks
 * wkssvc for NetrWkstaGetInfo, and gives up on the server ("Windows cannot
 * access") if that pipe is missing, even with the share list in hand.
 *
 * That is all this answers, written by hand rather than with libdcerpc: the
 * library's generic NDR runtime and coders for the whole of [MS-SRVS] cost
 * ~65 KB of a kernel partition that OTA can barely stage, to send three
 * replies. What is here is the connection-oriented PDU framing of [C706]
 * chapter 12 (BIND/ALTER_CONTEXT, REQUEST/RESPONSE, FAULT, fragments), NDR32
 * little-endian, and the four procedures' marshalling, from smbd's own
 * configuration: one disk share, plus IPC$.
 *
 * A pipe is message-mode: each PDU the server produces is one message, and a
 * read too small for it gets the rest on the next read (BUFFER_OVERFLOW).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "espix_kernel.h"
#include "smbd_srvsvc.h"

static const char *TAG = "espix:smbd";

/* [C706] 12.6: PDU types and flags */
#define PT_REQUEST        0
#define PT_RESPONSE       2
#define PT_FAULT          3
#define PT_BIND           11
#define PT_BIND_ACK       12
#define PT_ALTER_CONTEXT  14
#define PT_ALTER_RESP     15

#define PFC_FIRST_FRAG    0x01
#define PFC_LAST_FRAG     0x02
#define PFC_DID_NOT_EXEC  0x20
#define PFC_OBJECT_UUID   0x80

#define HDR_LEN           16
#define MAX_FRAG          4280        /* what Windows offers, and enough */
#define MAX_REQUEST       (64 * 1024)

/* [C706] Appendix E / [MS-RPCE] 2.2.2.11 fault codes */
#define NCA_OP_RNG_ERROR  0x1c010002u
#define NCA_UNK_IF        0x1c010003u
#define NCA_FAULT_NDR     0x000006f7u

/* [MS-SRVS] procedures, share types and the Win32 errors they return */
#define OP_SHARE_ENUM     15
#define OP_SHARE_GET_INFO 16
#define OP_SERVER_GET_INFO 21
#define OP_WKSTA_GET_INFO 0         /* [MS-WKST] NetrWkstaGetInfo */
#define STYPE_DISKTREE    0x00000000u
#define STYPE_IPC         0x00000003u
#define STYPE_SPECIAL     0x80000000u
#define WERR_OK           0u
#define WERR_UNKNOWN_LEVEL 124u
#define WERR_NET_NAME_NOT_FOUND 2310u
#define PLATFORM_ID_NT    500u
#define SV_TYPE_SERVER_SET (0x00000001u | 0x00000002u | 0x00001000u | 0x00008000u)

/* Syntax identifiers as they appear on the wire: UUID fields little-endian. */
static const uint8_t SRVSVC_UUID[16] = {   /* 4b324fc8-1670-01d3-1278-5a47bf6ee188 v3 */
    0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01,
    0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88,
};
static const uint8_t WKSSVC_UUID[16] = {   /* 6bffd098-a112-3610-9833-46c3f87e345a v1 */
    0x98, 0xd0, 0xff, 0x6b, 0x12, 0xa1, 0x10, 0x36,
    0x98, 0x33, 0x46, 0xc3, 0xf8, 0x7e, 0x34, 0x5a,
};
static const uint8_t NDR32_SYNTAX[20] = {  /* 8a885d04-1ceb-11c9-9fe8-08002b104860 v2 */
    0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11,
    0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60,
    0x02, 0x00, 0x00, 0x00,
};
static const uint8_t FEATURE_NEG_PREFIX[8] = {  /* 6cb71c2c-9812-4540-<bitmask> */
    0x2c, 0x1c, 0xb7, 0x6c, 0x12, 0x98, 0x40, 0x45,
};

typedef struct msg {
    struct msg *next;
    size_t      len;
    size_t      off;        /* read so far */
    uint8_t     data[];
} msg_t;

struct smbd_srvsvc_pipe {
    smbd_pipe_kind_t kind;
    uint8_t  *in;           /* bytes written, not yet a whole PDU */
    size_t    in_len;
    uint8_t  *req;          /* a fragmented REQUEST's stub so far */
    size_t    req_len;
    uint16_t  max_frag;     /* what the client will take per fragment */
    uint32_t  assoc;
    msg_t    *out;          /* PDUs for the client's reads, oldest first */
    msg_t   **out_tail;
};

typedef struct {
    const char *name;
    uint32_t    type;
    const char *remark;
    const char *path;
} share_t;

static share_t     s_shares[2];
static const char *s_hostname = "espix";

/* ------------------------------------------------------------- buffers */

/* A growing little-endian buffer. A failed allocation sticks, so callers
 * check once at the end instead of after every put. */
typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
    size_t   base;          /* where NDR alignment is counted from */
    bool     oom;
} buf_t;

static void put(buf_t *b, const void *src, size_t n)
{
    if (b->oom) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n) {
            cap *= 2;
        }
        uint8_t *p = realloc(b->p, cap);
        if (p == NULL) {
            b->oom = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    if (src != NULL) {
        memcpy(b->p + b->len, src, n);
    } else {
        memset(b->p + b->len, 0, n);
    }
    b->len += n;
}

static void put8(buf_t *b, uint8_t v)   { put(b, &v, 1); }
static void put16(buf_t *b, uint16_t v) { const uint8_t x[2] = { v, v >> 8 }; put(b, x, 2); }
static void put32(buf_t *b, uint32_t v)
{
    const uint8_t x[4] = { v, v >> 8, v >> 16, v >> 24 };
    put(b, x, 4);
}

static void align(buf_t *b, size_t n)
{
    while ((b->len - b->base) % n) {
        put8(b, 0);
    }
}

/* A request's stub, read with the same alignment rules. */
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         off;
    bool           bad;
} rd_t;

static uint32_t get32(rd_t *r)
{
    r->off = (r->off + 3) & ~(size_t)3;
    if (r->off + 4 > r->len) {
        r->bad = true;
        return 0;
    }
    const uint8_t *x = r->p + r->off;
    r->off += 4;
    return (uint32_t)x[0] | ((uint32_t)x[1] << 8) | ((uint32_t)x[2] << 16) |
           ((uint32_t)x[3] << 24);
}

/* A [string] wchar_t*: max, offset, actual count, then the UTF-16 units.
 * Kept as ASCII, '?' for anything else; NetName is only compared. */
static void get_string(rd_t *r, char *out, size_t outlen)
{
    (void)get32(r);
    (void)get32(r);
    const uint32_t n = get32(r);
    if (r->bad || n > (r->len - r->off) / 2) {
        r->bad = true;
        return;
    }
    size_t j = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t c = (uint16_t)(r->p[r->off + 2 * i] | (r->p[r->off + 2 * i + 1] << 8));
        if (c != 0 && j + 1 < outlen) {
            out[j++] = (c < 0x80) ? (char)c : '?';
        }
    }
    if (outlen > 0) {
        out[j] = 0;
    }
    r->off += 2 * (size_t)n;
}

/* [in, string, unique] ServerName: a referent, then the string if non-null. */
static void skip_unique_string(rd_t *r)
{
    char ignored[2];
    if (get32(r) != 0) {
        get_string(r, ignored, sizeof(ignored));
    }
}

/* -------------------------------------------------------------- NDR out */

/*
 * A struct's members, as NDR sees them: a [string] pointer, or a 32-bit value.
 * NDR writes the fixed part of a struct (pointers as referent ids) and then
 * what its pointers point at, deferred; an array of structs writes all the
 * fixed parts, then every element's deferred data in order. These two
 * functions are those two passes.
 */
typedef struct {
    bool        str;
    const char *s;          /* NULL: a null pointer */
    uint32_t    v;
} field_t;

#define STR(x) { true, (x), 0 }
#define U32(x) { false, NULL, (x) }

static uint32_t s_referent;

static void put_ref(buf_t *b, const void *target)
{
    put32(b, target ? (s_referent += 4) : 0);
}

static void put_fixed(buf_t *b, const field_t *f, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (f[i].str) {
            put_ref(b, f[i].s);
        } else {
            put32(b, f[i].v);
        }
    }
}

static void put_wstring(buf_t *b, const char *s)
{
    const uint32_t n = (uint32_t)strlen(s) + 1;   /* with the terminator */
    align(b, 4);
    put32(b, n);
    put32(b, 0);
    put32(b, n);
    for (uint32_t i = 0; i < n; i++) {
        put16(b, (uint8_t)s[i]);
    }
}

static void put_deferred(buf_t *b, const field_t *f, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (f[i].str && f[i].s != NULL) {
            put_wstring(b, f[i].s);
        }
    }
}

/* One share at a level, 0 fields for a level not served. */
static size_t share_fields(uint32_t level, const share_t *sh, field_t *f)
{
    size_t n = 0;
    switch (level) {
    case 0:
        f[n++] = (field_t)STR(sh->name);
        break;
    case 1:
    case 501:
        f[n++] = (field_t)STR(sh->name);
        f[n++] = (field_t)U32(sh->type);
        f[n++] = (field_t)STR(sh->remark);
        if (level == 501) {
            f[n++] = (field_t)U32(0);           /* flags */
        }
        break;
    case 2:
    case 502:
        f[n++] = (field_t)STR(sh->name);
        f[n++] = (field_t)U32(sh->type);
        f[n++] = (field_t)STR(sh->remark);
        f[n++] = (field_t)U32(0);               /* permissions */
        f[n++] = (field_t)U32(0xffffffffu);     /* max uses */
        f[n++] = (field_t)U32(0);               /* current uses */
        f[n++] = (field_t)STR(sh->path);
        f[n++] = (field_t)STR(NULL);            /* password */
        if (level == 502) {
            f[n++] = (field_t)U32(0);           /* security descriptor size */
            f[n++] = (field_t)STR(NULL);        /* ...and none */
        }
        break;
    case 1005:
        f[n++] = (field_t)U32(0);   /* flags: manual caching, the default */
        break;
    default:
        break;
    }
    return n;
}

static size_t server_fields(uint32_t level, field_t *f)
{
    size_t n = 0;
    if (level != 100 && level != 101 && level != 102) {
        return 0;
    }
    f[n++] = (field_t)U32(PLATFORM_ID_NT);
    f[n++] = (field_t)STR(s_hostname);
    if (level == 100) {
        return n;
    }
    f[n++] = (field_t)U32(10);                  /* version 10.0 */
    f[n++] = (field_t)U32(0);
    f[n++] = (field_t)U32(SV_TYPE_SERVER_SET);
    f[n++] = (field_t)STR("espix");
    if (level == 102) {
        f[n++] = (field_t)U32(0);               /* users */
        f[n++] = (field_t)U32(0xffffffffu);     /* disc: -1, never */
        f[n++] = (field_t)U32(0);               /* hidden */
        f[n++] = (field_t)U32(240);             /* announce */
        f[n++] = (field_t)U32(3000);            /* announce delta */
        f[n++] = (field_t)U32(0);               /* licenses */
        f[n++] = (field_t)STR("C:\\");          /* user path */
    }
    return n;
}

/* --------------------------------------------------------- procedures */

#define MAX_FIELDS 12
#define N_SHARES   (sizeof(s_shares) / sizeof(s_shares[0]))

/* NetrShareEnum: every share at the level asked for. */
static int share_enum(rd_t *r, buf_t *b)
{
    skip_unique_string(r);
    const uint32_t level = get32(r);
    (void)get32(r);                             /* the union's tag */
    if (get32(r) != 0) {                        /* the container a client sends */
        (void)get32(r);                         /* is empty: EntriesRead, */
        if (get32(r) != 0) {                    /* and a null Buffer */
            r->bad = true;
        }
    }
    (void)get32(r);                             /* PreferedMaximumLength */
    const bool resume = (get32(r) != 0);
    if (resume) {
        (void)get32(r);
    }
    if (r->bad) {
        return -1;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "srvsvc: NetrShareEnum level %u", (unsigned)level);

    field_t f[N_SHARES][MAX_FIELDS];
    size_t nf = 0;
    for (size_t i = 0; i < N_SHARES; i++) {
        nf = share_fields(level, &s_shares[i], f[i]);
    }
    const bool served = (nf != 0 && level != 1005);

    put32(b, level);                            /* InfoStruct.Level */
    put32(b, level);                            /* the union's tag */
    if (served) {
        put_ref(b, b);                          /* the container */
        put32(b, N_SHARES);                     /* EntriesRead */
        put_ref(b, b);                          /* Buffer */
        put32(b, N_SHARES);                     /* the array's size */
        for (size_t i = 0; i < N_SHARES; i++) {
            put_fixed(b, f[i], nf);
        }
        for (size_t i = 0; i < N_SHARES; i++) {
            put_deferred(b, f[i], nf);
        }
        align(b, 4);
    } else {
        put32(b, 0);
    }
    put32(b, served ? N_SHARES : 0);            /* TotalEntries */
    put_ref(b, resume ? b : NULL);              /* ResumeHandle */
    if (resume) {
        put32(b, 0);
    }
    put32(b, served ? WERR_OK : WERR_UNKNOWN_LEVEL);
    return 0;
}

/* NetrShareGetInfo: one share by name. */
static int share_get_info(rd_t *r, buf_t *b)
{
    char want[64];
    skip_unique_string(r);
    get_string(r, want, sizeof(want));
    const uint32_t level = get32(r);
    if (r->bad) {
        return -1;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "srvsvc: NetrShareGetInfo '%s' level %u",
               want, (unsigned)level);

    const share_t *sh = NULL;
    for (size_t i = 0; i < N_SHARES; i++) {
        if (strcasecmp(s_shares[i].name, want) == 0) {
            sh = &s_shares[i];
        }
    }
    field_t f[MAX_FIELDS];
    const size_t nf = (sh != NULL) ? share_fields(level, sh, f) : 0;

    put32(b, level);                            /* the union's tag */
    put_ref(b, nf ? b : NULL);
    if (nf) {
        put_fixed(b, f, nf);
        put_deferred(b, f, nf);
        align(b, 4);
    }
    put32(b, sh == NULL ? WERR_NET_NAME_NOT_FOUND : nf ? WERR_OK : WERR_UNKNOWN_LEVEL);
    return 0;
}

/* NetrServerGetInfo: who the server is. */
static int server_get_info(rd_t *r, buf_t *b)
{
    skip_unique_string(r);
    const uint32_t level = get32(r);
    if (r->bad) {
        return -1;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "srvsvc: NetrServerGetInfo level %u", (unsigned)level);

    field_t f[MAX_FIELDS];
    const size_t nf = server_fields(level, f);
    put32(b, level);
    put_ref(b, nf ? b : NULL);
    if (nf) {
        put_fixed(b, f, nf);
        put_deferred(b, f, nf);
        align(b, 4);
    }
    put32(b, nf ? WERR_OK : WERR_UNKNOWN_LEVEL);
    return 0;
}

/* NetrWkstaGetInfo: the workstation side of who the server is. */
static int wksta_get_info(rd_t *r, buf_t *b)
{
    skip_unique_string(r);
    const uint32_t level = get32(r);
    if (r->bad) {
        return -1;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "wkssvc: NetrWkstaGetInfo level %u", (unsigned)level);

    field_t f[MAX_FIELDS];
    size_t nf = 0;
    if (level == 100 || level == 101 || level == 102) {
        f[nf++] = (field_t)U32(PLATFORM_ID_NT);
        f[nf++] = (field_t)STR(s_hostname);
        f[nf++] = (field_t)STR("WORKGROUP");
        f[nf++] = (field_t)U32(10);             /* version 10.0 */
        f[nf++] = (field_t)U32(0);
        if (level >= 101) {
            f[nf++] = (field_t)STR("C:\\");     /* lanroot */
        }
        if (level == 102) {
            f[nf++] = (field_t)U32(0);          /* logged-on users */
        }
    }
    put32(b, level);                            /* the union's tag */
    put_ref(b, nf ? b : NULL);
    if (nf) {
        put_fixed(b, f, nf);
        put_deferred(b, f, nf);
        align(b, 4);
    }
    put32(b, nf ? WERR_OK : WERR_UNKNOWN_LEVEL);
    return 0;
}

/* --------------------------------------------------------------- PDUs */

static void queue(smbd_srvsvc_pipe_t *p, const uint8_t *data, size_t len)
{
    msg_t *m = malloc(sizeof(*m) + len);
    if (m == NULL) {
        return;     /* the client's read then finds the pipe empty */
    }
    m->next = NULL;
    m->len = len;
    m->off = 0;
    memcpy(m->data, data, len);
    *p->out_tail = m;
    p->out_tail = &m->next;
}

static void put_header(buf_t *b, uint8_t type, uint8_t flags, uint32_t call_id)
{
    const uint8_t drep[4] = { 0x10, 0, 0, 0 };  /* little-endian, ASCII, IEEE */
    put8(b, 5);
    put8(b, 0);
    put8(b, type);
    put8(b, flags);
    put(b, drep, 4);
    put16(b, 0);                                /* frag_length, set on finish */
    put16(b, 0);                                /* auth_length */
    put32(b, call_id);
}

static void finish(smbd_srvsvc_pipe_t *p, buf_t *b)
{
    if (!b->oom && b->len >= HDR_LEN) {
        b->p[8] = (uint8_t)b->len;
        b->p[9] = (uint8_t)(b->len >> 8);
        queue(p, b->p, b->len);
    }
    free(b->p);
}

static void fault(smbd_srvsvc_pipe_t *p, uint32_t call_id, uint16_t ctx, uint32_t status)
{
    buf_t b = {0};
    put_header(&b, PT_FAULT, PFC_FIRST_FRAG | PFC_LAST_FRAG | PFC_DID_NOT_EXEC, call_id);
    put32(&b, 0);                               /* alloc hint */
    put16(&b, ctx);
    put8(&b, 0);                                /* cancel count */
    put8(&b, 0);
    put32(&b, status);
    put32(&b, 0);
    finish(p, &b);
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "srvsvc: fault 0x%08x", (unsigned)status);
}

/* BIND or ALTER_CONTEXT: accept the pipe's interface over NDR32, decline
 * everything else. */
static void bind(smbd_srvsvc_pipe_t *p, const uint8_t *pdu, size_t len, bool alter,
                 uint32_t call_id)
{
    if (len < HDR_LEN + 12) {
        return;
    }
    const uint16_t client_recv = (uint16_t)(pdu[HDR_LEN + 2] | (pdu[HDR_LEN + 3] << 8));
    const uint32_t assoc = (uint32_t)pdu[HDR_LEN + 4] | ((uint32_t)pdu[HDR_LEN + 5] << 8) |
                           ((uint32_t)pdu[HDR_LEN + 6] << 16) |
                           ((uint32_t)pdu[HDR_LEN + 7] << 24);
    const uint8_t n_ctx = pdu[HDR_LEN + 8];

    p->max_frag = (client_recv >= 1024 && client_recv < MAX_FRAG) ? client_recv : MAX_FRAG;
    if (!alter) {
        p->assoc = assoc ? assoc : 0x53b2;
    }

    buf_t b = {0};
    put_header(&b, alter ? PT_ALTER_RESP : PT_BIND_ACK, PFC_FIRST_FRAG | PFC_LAST_FRAG,
               call_id);
    put16(&b, p->max_frag);                     /* max xmit */
    put16(&b, p->max_frag);                     /* max recv */
    put32(&b, p->assoc);
    /* The secondary address: the pipe, on a BIND_ACK; empty on an
     * ALTER_CONTEXT_RESP. */
    const bool wks = (p->kind == SMBD_PIPE_WKSSVC);
    const char *port = wks ? "\\PIPE\\wkssvc" : "\\PIPE\\srvsvc";
    const uint8_t *iface = wks ? WKSSVC_UUID : SRVSVC_UUID;
    const uint8_t iface_major = wks ? 1 : 3;
    if (alter) {
        put16(&b, 0);
    } else {
        put16(&b, (uint16_t)(strlen(port) + 1));
        put(&b, port, strlen(port) + 1);
    }
    while (b.len % 4) {
        put8(&b, 0);
    }
    put8(&b, n_ctx);
    put8(&b, 0);
    put16(&b, 0);

    size_t off = HDR_LEN + 12;
    for (uint8_t i = 0; i < n_ctx; i++) {
        if (off + 24 > len) {
            break;
        }
        const uint8_t n_ts = pdu[off + 2];
        const uint8_t *abstract = pdu + off + 4;
        const uint8_t *ts = pdu + off + 24;
        bool ndr32 = false;
        for (uint8_t t = 0; t < n_ts && ts + 20 * (t + 1) <= pdu + len; t++) {
            if (memcmp(ts + 20 * t, NDR32_SYNTAX, 20) == 0) {
                ndr32 = true;
            }
        }
        if (memcmp(abstract, FEATURE_NEG_PREFIX, 8) == 0) {
            /* Bind-time feature negotiation, [MS-RPCE] 3.3.1.5.3: acknowledged,
             * with no features. */
            put16(&b, 3);
            put16(&b, 0);
            put(&b, NULL, 20);
        } else if (memcmp(abstract, iface, 16) == 0 && abstract[16] == iface_major && ndr32) {
            put16(&b, 0);                       /* acceptance */
            put16(&b, 0);
            put(&b, NDR32_SYNTAX, 20);
        } else {
            /* Provider rejection: the abstract syntax (1) or, srvsvc over
             * NDR64, the transfer syntax (2) is not supported. */
            put16(&b, 2);
            put16(&b, memcmp(abstract, iface, 16) == 0 ? 2 : 1);
            put(&b, NULL, 20);
        }
        off += 24 + 20 * (size_t)n_ts;
    }
    finish(p, &b);
}

/* A whole REQUEST: run the procedure, send its stub back in fragments. */
static void request(smbd_srvsvc_pipe_t *p, uint32_t call_id, uint16_t ctx, uint16_t opnum,
                    const uint8_t *stub, size_t stub_len)
{
    rd_t r = { stub, stub_len, 0, false };
    buf_t out = {0};
    int rc;

    s_referent = 0x00020000;
    const int key = (p->kind == SMBD_PIPE_WKSSVC) ? 1000 + opnum : opnum;
    switch (key) {
    case OP_SHARE_ENUM:      rc = share_enum(&r, &out);      break;
    case OP_SHARE_GET_INFO:  rc = share_get_info(&r, &out);  break;
    case OP_SERVER_GET_INFO: rc = server_get_info(&r, &out); break;
    case 1000 + OP_WKSTA_GET_INFO: rc = wksta_get_info(&r, &out); break;
    default:
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "%s: opnum %u not served",
                   p->kind == SMBD_PIPE_WKSSVC ? "wkssvc" : "srvsvc", (unsigned)opnum);
        free(out.p);
        fault(p, call_id, ctx, NCA_OP_RNG_ERROR);
        return;
    }
    if (rc != 0 || out.oom) {
        free(out.p);
        fault(p, call_id, ctx, NCA_FAULT_NDR);
        return;
    }

    const size_t room = p->max_frag - (HDR_LEN + 8);
    size_t done = 0;
    do {
        const size_t n = (out.len - done < room) ? out.len - done : room;
        uint8_t flags = 0;
        if (done == 0) {
            flags |= PFC_FIRST_FRAG;
        }
        if (done + n == out.len) {
            flags |= PFC_LAST_FRAG;
        }
        buf_t b = {0};
        put_header(&b, PT_RESPONSE, flags, call_id);
        put32(&b, (uint32_t)(out.len - done));  /* alloc hint */
        put16(&b, ctx);
        put8(&b, 0);
        put8(&b, 0);
        put(&b, out.p + done, n);
        finish(p, &b);
        done += n;
    } while (done < out.len);
    free(out.p);
}

/* One complete PDU from the client. */
static void pdu_in(smbd_srvsvc_pipe_t *p, const uint8_t *pdu, size_t len)
{
    const uint8_t type = pdu[2];
    const uint8_t flags = pdu[3];
    const uint32_t call_id = (uint32_t)pdu[12] | ((uint32_t)pdu[13] << 8) |
                             ((uint32_t)pdu[14] << 16) | ((uint32_t)pdu[15] << 24);
    const uint16_t auth_len = (uint16_t)(pdu[10] | (pdu[11] << 8));

    if (type == PT_BIND || type == PT_ALTER_CONTEXT) {
        bind(p, pdu, len, type == PT_ALTER_CONTEXT, call_id);
        return;
    }
    if (type != PT_REQUEST || len < HDR_LEN + 8) {
        return;     /* AUTH3, cancels and the like have no answer */
    }
    const uint16_t ctx = (uint16_t)(pdu[HDR_LEN + 4] | (pdu[HDR_LEN + 5] << 8));
    const uint16_t opnum = (uint16_t)(pdu[HDR_LEN + 6] | (pdu[HDR_LEN + 7] << 8));
    size_t start = HDR_LEN + 8 + ((flags & PFC_OBJECT_UUID) ? 16 : 0);
    /* Any auth trailer (8 bytes of sec_trailer plus the token) is dropped. */
    size_t end = len;
    if (auth_len != 0) {
        end = (auth_len + 8 <= len) ? len - auth_len - 8 : 0;
    }
    if (start > end) {
        fault(p, call_id, ctx, NCA_FAULT_NDR);
        return;
    }
    if (pdu[4] != 0x10) {
        fault(p, call_id, ctx, NCA_FAULT_NDR);  /* big-endian: not spoken here */
        return;
    }

    if (flags & PFC_FIRST_FRAG) {
        p->req_len = 0;
    }
    const size_t n = end - start;
    if (p->req_len + n > MAX_REQUEST) {
        p->req_len = 0;
        fault(p, call_id, ctx, NCA_FAULT_NDR);
        return;
    }
    uint8_t *grown = realloc(p->req, p->req_len + n + 1);
    if (grown == NULL) {
        fault(p, call_id, ctx, NCA_FAULT_NDR);
        return;
    }
    p->req = grown;
    memcpy(p->req + p->req_len, pdu + start, n);
    p->req_len += n;
    if (flags & PFC_LAST_FRAG) {
        request(p, call_id, ctx, opnum, p->req, p->req_len);
        p->req_len = 0;
    }
}

/* ---------------------------------------------------------------- API */

void smbd_srvsvc_config(const char *hostname, const char *share, const char *share_path)
{
    s_hostname = hostname;
    s_shares[0] = (share_t){ share, STYPE_DISKTREE, "espix", share_path };
    s_shares[1] = (share_t){ "IPC$", STYPE_IPC | STYPE_SPECIAL, "IPC Service", "" };
}

smbd_srvsvc_pipe_t *smbd_srvsvc_open(smbd_pipe_kind_t kind)
{
    smbd_srvsvc_pipe_t *p = calloc(1, sizeof(*p));
    if (p != NULL) {
        p->kind = kind;
        p->max_frag = MAX_FRAG;
        p->out_tail = &p->out;
    }
    return p;
}

void smbd_srvsvc_close(smbd_srvsvc_pipe_t *p)
{
    if (p == NULL) {
        return;
    }
    while (p->out != NULL) {
        msg_t *m = p->out;
        p->out = m->next;
        free(m);
    }
    free(p->in);
    free(p->req);
    free(p);
}

int smbd_srvsvc_write(smbd_srvsvc_pipe_t *p, const uint8_t *buf, size_t len)
{
    uint8_t *grown = realloc(p->in, p->in_len + len + 1);
    if (grown == NULL) {
        return -1;
    }
    p->in = grown;
    memcpy(p->in + p->in_len, buf, len);
    p->in_len += len;

    while (p->in_len >= HDR_LEN) {
        if (p->in[0] != 5) {
            p->in_len = 0;
            return -1;      /* not DCE/RPC */
        }
        const size_t frag = (size_t)(p->in[8] | (p->in[9] << 8));
        if (frag < HDR_LEN || frag > MAX_REQUEST) {
            p->in_len = 0;
            return -1;
        }
        if (p->in_len < frag) {
            break;          /* the rest of the PDU comes with the next write */
        }
        pdu_in(p, p->in, frag);
        memmove(p->in, p->in + frag, p->in_len - frag);
        p->in_len -= frag;
    }
    return 0;
}

size_t smbd_srvsvc_read(smbd_srvsvc_pipe_t *p, uint8_t *buf, size_t max, int *more)
{
    *more = 0;
    msg_t *m = p->out;
    if (m == NULL) {
        return 0;
    }
    size_t n = m->len - m->off;
    if (n > max) {
        n = max;
        *more = 1;
    }
    memcpy(buf, m->data + m->off, n);
    m->off += n;
    if (m->off == m->len) {
        p->out = m->next;
        if (p->out == NULL) {
            p->out_tail = &p->out;
        }
        free(m);
    }
    return n;
}
