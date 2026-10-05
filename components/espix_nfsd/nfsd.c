/*
 * An NFSv3 server, in the shape the protocol actually has: a portmapper, a
 * mount daemon and nfsd, which a Linux or macOS client finds by asking 111
 * where the others are. mountd has no well-known port at all, which is the
 * whole reason the portmapper exists.
 *
 * This is the read-only half: the portmapper, mountd, and the exports the
 * server is willing to talk about. nfsd's own procedures come next.
 */

#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"

#include "esp_heap_caps.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_nfsd.h"

#include "nfs3.h"
#include "nfsd_internal.h"
#include "rpc.h"

#define TAG "espix:nfsd"

#define PORTMAP_PROG 100000u
#define PORTMAP_VERS 2u
#define MOUNT_PROG   100005u
#define MOUNT_VERS   3u
#define NFS_PROG     100003u
#define NFS_VERS     3u

/*
 * A client pings this with a NULL before it will mount, and a real nfsd answers
 * that ping successfully -- answering PROG_UNAVAIL is what made the mount fail
 * after MNT, GETATTR and PATHCONF had all been answered correctly. The number is
 * in no registry and the probe carries no data, but every server on the wire
 * answers it, so this does too.
 */
#define NFS_PING_PROG   400122u
#define NFS_PING_VERS   1u

/* statd, the Network Status Monitor, which a client asks the portmapper for
 * before it will mount without a lock option. See handle_nsm(). */
#define NSM_PROG     100024u
#define NSM_VERS     1u

#define PORTMAP_PORT 111
#define MOUNT_PORT   20048          /* ours to choose: mountd has no default */
#define NFS_PORT     2049

#define MAX_EXPORTS 8
#define MAX_CLIENTS 8

#define BUFCAP   NFSD_BUFCAP   /* 16 KB: a read reply has to fit */
#define IOCAP    NFSD_IOCAP
#define MAX_CONNS 4

static nfs_export_t *s_exports;          /* PSRAM, like the buffers */
static int           s_nexports;

static struct {
    uint32_t prog, vers;
    uint16_t port;
} s_reg[8];
static int s_nreg;

static uint8_t *s_req;
static uint8_t *s_rep;
static uint8_t *s_frame;   /* TCP: on the heap, not 8 KB of task stack */
static uint8_t *s_out;
static uint8_t *s_io;                       /* a read fills this */
static bool     s_trace;                    /* append every RPC to /tmp/nfsd.trace */

void espix_nfsd_trace(bool on)
{
    s_trace = on;
}

bool espix_nfsd_tracing(void)
{
    return s_trace;
}

/*
 * A trace on the filesystem rather than in the ring: a protocol conversation
 * has to survive the reader being slow, and the ring is a rolling window. The
 * reply bytes are what matter -- a decode failure on the other end is a
 * disagreement about the shape of these.
 */
static void trace_rpc(const rpc_call_t *c, const uint8_t *rep, size_t len)
{
    if (!s_trace) {
        return;
    }

    FILE *f = fopen("/tmp/nfsd.trace", "a");
    if (f == NULL) {
        return;
    }

    uint32_t status = 0;
    if (c->prog == NFS_PROG && len >= 28) {
        status = ((uint32_t)rep[24] << 24) | ((uint32_t)rep[25] << 16) |
                 ((uint32_t)rep[26] << 8) | rep[27];
    }

    fprintf(f, "prog %u vers %u proc %u xid %u status %u -> %u bytes\n",
            (unsigned)c->prog, (unsigned)c->vers, (unsigned)c->proc,
            (unsigned)c->xid, (unsigned)status, (unsigned)len);
    fprintf(f, "  req:");
    for (size_t i = 0; i < c->len && i < 96; i++) {
        fprintf(f, " %02x", c->buf[i]);
    }
    fprintf(f, "\n  rep:");
    for (size_t i = 0; i < len && i < 160; i++) {
        fprintf(f, " %02x", rep[i]);
    }
    fprintf(f, "\n");
    fclose(f);
}

/* A client keeps its TCP connection open for as long as it likes, so one
 * connection cannot be served to the exclusion of everything else. */
static struct {
    int      fd;
    uint32_t src;
} s_conns[MAX_CONNS];

/* ------------------------------------------------------------- exports --- */

static bool parse_client(nfs_export_t *e, const char *tok)
{
    if (strcmp(tok, "*") == 0) {
        e->all = true;
        return true;
    }

    unsigned a, b, c, d, bits = 32;
    char     host[64];
    strlcpy(host, tok, sizeof(host));

    char *slash = strchr(host, '/');
    if (slash != NULL) {
        *slash = 0;
        bits = (unsigned)atoi(slash + 1);
        if (bits == 0 || bits > 32) {
            bits = 32;
        }
    }
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;               /* hostnames are not resolved yet */
    }
    if (e->nclients >= MAX_CLIENTS) {
        return false;
    }

    nfs_client_t *cl = &e->clients[e->nclients++];
    cl->addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    cl->mask = (bits == 0) ? 0 : htonl(0xFFFFFFFFu << (32 - bits));
    return true;
}

/* <path> <client>[(opts)] ...  -- the Linux syntax, less the hostnames. */
static int load_exports(void)
{
    FILE *f = fopen("/etc/exports", "r");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no /etc/exports; nothing to serve");
        return 0;
    }

    if (s_exports == NULL) {
        s_exports = heap_caps_malloc(MAX_EXPORTS * sizeof(*s_exports),
                                     MALLOC_CAP_SPIRAM);
        if (s_exports == NULL) {
            s_exports = heap_caps_malloc(MAX_EXPORTS * sizeof(*s_exports),
                                         MALLOC_CAP_8BIT);
        }
        if (s_exports == NULL) {
            fclose(f);
            return 0;
        }
    }

    char line[256];
    s_nexports = 0;
    while (fgets(line, sizeof(line), f) != NULL && s_nexports < MAX_EXPORTS) {
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = 0;
        }

        char *save = NULL;
        char *path = strtok_r(line, " \t\r\n", &save);
        if (path == NULL) {
            continue;
        }

        nfs_export_t *e = &s_exports[s_nexports];
        memset(e, 0, sizeof(*e));
        strlcpy(e->path, path, sizeof(e->path));
        e->ro = true;               /* read-only unless it says otherwise */

        bool any = false;
        for (char *tok = strtok_r(NULL, " \t\r\n", &save); tok != NULL;
             tok = strtok_r(NULL, " \t\r\n", &save)) {
            char *opts = strchr(tok, '(');
            if (opts != NULL) {
                *opts++ = 0;
                if (strstr(opts, "rw") != NULL) {
                    e->ro = false;
                }
            }
            any |= parse_client(e, tok);
        }
        if (any) {
            s_nexports++;
        }
    }
    fclose(f);

    espix_klog(ESPIX_KLOG_INFO, TAG, "%d export(s)", s_nexports);
    return s_nexports;
}

static bool client_allowed(const nfs_export_t *e, uint32_t src)
{
    if (e->all) {
        return true;
    }
    for (int i = 0; i < e->nclients; i++) {
        if ((src & e->clients[i].mask) == (e->clients[i].addr & e->clients[i].mask)) {
            return true;
        }
    }
    return false;
}

static const nfs_export_t *export_for(const char *dir)
{
    for (int i = 0; i < s_nexports; i++) {
        if (strcmp(s_exports[i].path, dir) == 0) {
            return &s_exports[i];
        }
    }
    return NULL;
}

int nfsd_exports_count(void)
{
    return s_nexports;
}

const nfs_export_t *nfsd_export(int i)
{
    return (i >= 0 && i < s_nexports) ? &s_exports[i] : NULL;
}

const nfs_export_t *nfsd_export_for_dir(const char *dir)
{
    return export_for(dir);
}

bool nfsd_client_allowed(const nfs_export_t *e, uint32_t src)
{
    return client_allowed(e, src);
}

/* --------------------------------------------------------------- the RPC --- */

static void reg_add(uint32_t prog, uint32_t vers, uint16_t port)
{
    if (s_nreg < (int)(sizeof(s_reg) / sizeof(s_reg[0]))) {
        s_reg[s_nreg].prog = prog;
        s_reg[s_nreg].vers = vers;
        s_reg[s_nreg].port = port;
        s_nreg++;
    }
}

static uint16_t reg_find(uint32_t prog, uint32_t vers)
{
    for (int i = 0; i < s_nreg; i++) {
        if (s_reg[i].prog == prog && s_reg[i].vers == vers) {
            return s_reg[i].port;
        }
    }
    return 0;
}

/*
 * A stream socket may accept fewer bytes than it was handed, so a reply is
 * written in a loop -- ssh_transport.c keeps a write_all() for the same reason.
 * A datagram cannot be partial, which is why the UDP path only checks.
 */
static bool send_all(int fd, const void *src, size_t len)
{
    const uint8_t *p = src;

    while (len > 0) {
        const ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static size_t handle_portmap(const rpc_call_t *c, xdrw_t *w)
{
    switch (c->proc) {
    case 0:                                     /* NULL */
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;

    case 3: {                                   /* GETPORT */
        rpc_call_t r = *c;
        uint32_t prog, vers, prot, port;
        if (!rpc_get_u32(&r, &prog) || !rpc_get_u32(&r, &vers) ||
            !rpc_get_u32(&r, &prot) || !rpc_get_u32(&r, &port)) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
        xdrw_accept(w, RPC_SUCCESS);
        xdrw_u32(w, reg_find(prog, vers));
        return w->len;
    }

    case 4:                                     /* DUMP */
        xdrw_accept(w, RPC_SUCCESS);
        for (int i = 0; i < s_nreg; i++) {
            for (int prot = 0; prot < 2; prot++) {
                xdrw_bool(w, true);         /* this link is present */
                xdrw_u32(w, s_reg[i].prog);
                xdrw_u32(w, s_reg[i].vers);
                xdrw_u32(w, prot == 0 ? RPC_PROT_UDP : RPC_PROT_TCP);
                xdrw_u32(w, s_reg[i].port);
            }
        }
        xdrw_bool(w, false);
        return w->len;

    default:
        xdrw_accept(w, RPC_PROC_UNAVAIL);
        return w->len;
    }
}

/*
 * statd: program 100024, version 1, served on the nfsd port. The program number
 * is what tells an RPC apart, so it needs no socket of its own.
 *
 * A client finds statd through the portmapper and then registers the peers whose
 * crash it would want to know about. There is nothing to remember here: this
 * server cannot restart on its own in a way that leaves a client holding locks,
 * so a monitor is accepted and forgotten, which is what a statd with no state to
 * restore does. The replies still carry the shape a client reads -- an
 * sm_stat_res with a success status, an sm_stat with the state number -- because
 * a client that gets a short reply where it expects two words abandons the
 * mount, and answering at all is what makes a plain mount work with no options.
 */
static size_t handle_nsm(const rpc_call_t *c, xdrw_t *w)
{
    switch (c->proc) {
    case 1:                                     /* SM_STAT */
    case 2:                                     /* SM_MON */
        xdrw_accept(w, RPC_SUCCESS);
        xdrw_u32(w, 0);                         /* STAT_SUCC */
        xdrw_u32(w, 1);                         /* state */
        return w->len;

    case 3:                                     /* SM_UNMON */
    case 4:                                     /* SM_UNMON_ALL */
        xdrw_accept(w, RPC_SUCCESS);
        xdrw_u32(w, 1);                         /* state; sm_stat is one word */
        return w->len;

    case 0:                                     /* SM_NULL */
    case 5:                                     /* SM_SIMU_CRASH: nothing to do */
    case 6:                                     /* SM_NOTIFY: nobody to tell */
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;

    default:
        xdrw_accept(w, RPC_PROC_UNAVAIL);
        return w->len;
    }
}

/*
 * Which client has which export mounted, so the handle table can be given back
 * the moment nobody holds a handle. That moment is only ever known because a
 * client said so: mountd's UMNT, which clients send when they unmount. An entry
 * is twelve bytes and there are eight, the same ceiling the export client lists
 * have.
 *
 * A client that never unmounts -- it crashed, or the cable went -- keeps its
 * entry, and the table is kept for it. That is the safe direction: a handle
 * still held has to resolve, and one we have forgotten cannot.
 */
#define MAX_MOUNTED 8

static struct {
    uint32_t addr;                  /* network order, as sin_addr is */
    int      exp;                   /* index into the export table */
    int      n;                     /* mounts of it by this address */
} s_mounted[MAX_MOUNTED];

static void mount_add(uint32_t src, int exp)
{
    for (int i = 0; i < MAX_MOUNTED; i++) {
        if (s_mounted[i].n > 0 && s_mounted[i].addr == src &&
            s_mounted[i].exp == exp) {
            s_mounted[i].n++;
            return;
        }
    }
    for (int i = 0; i < MAX_MOUNTED; i++) {
        if (s_mounted[i].n == 0) {
            s_mounted[i].addr = src;
            s_mounted[i].exp  = exp;
            s_mounted[i].n    = 1;
            return;
        }
    }
}

static bool nothing_mounted(void)
{
    for (int i = 0; i < MAX_MOUNTED; i++) {
        if (s_mounted[i].n > 0) {
            return false;
        }
    }
    return true;
}

/* True when that was the last one anywhere, so the handles can go back. */
static bool mount_remove(uint32_t src, int exp)
{
    for (int i = 0; i < MAX_MOUNTED; i++) {
        if (s_mounted[i].n > 0 && s_mounted[i].addr == src &&
            s_mounted[i].exp == exp) {
            s_mounted[i].n--;
            break;
        }
    }
    return nothing_mounted();
}

static bool mount_remove_all(uint32_t src)
{
    for (int i = 0; i < MAX_MOUNTED; i++) {
        if (s_mounted[i].addr == src) {
            s_mounted[i].n = 0;
        }
    }
    return nothing_mounted();
}

static size_t handle_mount(const rpc_call_t *c, xdrw_t *w, uint32_t src)
{
    switch (c->proc) {
    case 0:                                     /* NULL */
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;

    case 1: {                                   /* MNT */
        rpc_call_t args = *c;
        char       dir[256];
        if (!rpc_get_string(&args, dir, sizeof(dir))) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
        xdrw_accept(w, RPC_SUCCESS);

        const nfs_export_t *e = export_for(dir);
        if (e == NULL) {
            xdrw_u32(w, 2);                     /* MNT3ERR_NOENT */
            return w->len;
        }
        if (!client_allowed(e, src)) {
            xdrw_u32(w, 13);                    /* MNT3ERR_ACCES */
            return w->len;
        }

        uint8_t fh[16];
        size_t  fhlen = 0;
        if (!nfs3_fh_for_export((int)(e - s_exports), fh, &fhlen)) {
            xdrw_u32(w, 10006);                 /* MNT3ERR_SERVERFAULT */
            return w->len;
        }

        mount_add(src, (int)(e - s_exports));

        xdrw_u32(w, 0);                         /* MNT3_OK */
        xdrw_u32(w, (uint32_t)fhlen);
        xdrw_opaque(w, fh, fhlen);
        xdrw_u32(w, 1);                         /* auth flavours: AUTH_SYS */
        xdrw_u32(w, 1);
        return w->len;
    }

    case 3: {                                   /* UMNT: this client is done */
        rpc_call_t args = *c;
        char       dir[256];
        if (!rpc_get_string(&args, dir, sizeof(dir))) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
        const nfs_export_t *e = export_for(dir);
        if (e != NULL && mount_remove(src, (int)(e - s_exports))) {
            nfs3_slots_cleanup();               /* nobody holds a handle now */
        }
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;
    }

    case 4:                                     /* UMNTALL */
        if (mount_remove_all(src)) {
            nfs3_slots_cleanup();
        }
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;

    case 2:                                     /* DUMP: nobody is mounted yet */
        xdrw_accept(w, RPC_SUCCESS);
        xdrw_bool(w, false);
        return w->len;

    case 5: {                                   /* EXPORT */
        xdrw_accept(w, RPC_SUCCESS);

        int last = -1;
        for (int i = 0; i < s_nexports; i++) {
            if (client_allowed(&s_exports[i], src)) {
                last = i;                       /* not theirs to see, else */
            }
        }
        if (last < 0) {
            xdrw_bool(w, false);                /* the result pointer is null */
            return w->len;
        }

        xdrw_bool(w, true);
        for (int i = 0; i <= last; i++) {
            if (!client_allowed(&s_exports[i], src)) {
                continue;
            }
            xdrw_string(w, s_exports[i].path);
            xdrw_bool(w, false);                /* no netgroups */
            xdrw_bool(w, i != last);            /* another export follows */
        }
        return w->len;
    }

    default:
        xdrw_accept(w, RPC_PROC_UNAVAIL);
        return w->len;
    }
}

static size_t dispatch(const rpc_call_t *c, uint8_t *rep, size_t cap, uint32_t src)
{
    xdrw_t w;
    size_t out;

    xdrw_init(&w, rep, cap, c->xid);

    if (c->prog == PORTMAP_PROG && c->vers == PORTMAP_VERS) {
        out = handle_portmap(c, &w);
    } else if (c->prog == MOUNT_PROG &&
               (c->vers == 1 || c->vers == 2 || c->vers == MOUNT_VERS)) {
        out = handle_mount(c, &w, src);
    } else if (c->prog == NFS_PROG && c->vers == NFS_VERS) {
        nfs3_set_source(src);
        out = nfs3_handle(c, &w, s_io, IOCAP);
    } else if (c->prog == NSM_PROG && c->vers == NSM_VERS) {
        out = handle_nsm(c, &w);
    } else if (c->prog == NFS_PING_PROG && c->vers == NFS_PING_VERS) {
        /* Every procedure of it, not just the NULL: nfsd answers proc 1 with a
         * void success too, and answering PROG_UNAVAIL is what the client is
         * left holding when the mount stops. */
        xdrw_accept(&w, RPC_SUCCESS);
        out = w.len;
    } else if (c->prog == PORTMAP_PROG || c->prog == MOUNT_PROG ||
               c->prog == NFS_PROG || c->prog == NSM_PROG) {
        xdrw_accept(&w, RPC_PROG_MISMATCH);
        xdrw_u32(&w, 1);
        xdrw_u32(&w, 3);
        out = w.len;
    } else {
        xdrw_accept(&w, RPC_PROG_UNAVAIL);
        out = w.len;
    }

    if (s_trace) {
        trace_rpc(c, rep, out);
    }
    return out;
}

static void serve_udp(int fd)
{
    struct sockaddr_in from;
    socklen_t          flen = sizeof(from);

    const int n = recvfrom(fd, s_req, BUFCAP, 0, (struct sockaddr *)&from, &flen);
    if (n <= 0) {
        return;
    }

    rpc_call_t c;
    if (!rpc_parse_call(&c, s_req, (size_t)n)) {
        return;
    }

    const size_t rlen = dispatch(&c, s_rep, BUFCAP, from.sin_addr.s_addr);
    if (rlen > 0) {
        const ssize_t sent = sendto(fd, s_rep, rlen, 0,
                                    (struct sockaddr *)&from, flen);
        if (sent != (ssize_t)rlen) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "udp reply: %d of %u bytes",
                       (int)sent, (unsigned)rlen);
        }
    }
}

static bool recv_all(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        fd_set         r;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

        FD_ZERO(&r);
        FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) {
            return false;
        }
        const int k = recv(fd, buf + got, n - got, 0);
        if (k <= 0) {
            return false;
        }
        got += (size_t)k;
    }
    return true;
}

static void conn_close(int i)
{
    close(s_conns[i].fd);
    s_conns[i].fd = -1;
}

static void conn_service(int i)
{
    uint8_t mark[4];

    if (!recv_all(s_conns[i].fd, mark, 4)) {
        conn_close(i);
        return;
    }
    const uint32_t m   = ((uint32_t)mark[0] << 24) | ((uint32_t)mark[1] << 16) |
                         ((uint32_t)mark[2] << 8) | mark[3];
    const size_t   len = m & 0x7FFFFFFFu;
    if (len == 0 || len > BUFCAP || !recv_all(s_conns[i].fd, s_frame, len)) {
        conn_close(i);
        return;
    }

    rpc_call_t c;
    if (!rpc_parse_call(&c, s_frame, len)) {
        conn_close(i);
        return;
    }

    const size_t rlen = dispatch(&c, s_rep, BUFCAP, s_conns[i].src);
    if (rlen > 0) {
        const size_t fl = rpc_tcp_frame(s_rep, rlen, s_out, BUFCAP + 4);
        if (fl == 0 || !send_all(s_conns[i].fd, s_out, fl)) {
            conn_close(i);
        }
    }
}

static void conn_accept(int lfd)
{
    struct sockaddr_in from;
    socklen_t          flen = sizeof(from);
    const int          cfd  = accept(lfd, (struct sockaddr *)&from, &flen);
    if (cfd < 0) {
        return;
    }

    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].fd < 0) {
            s_conns[i].fd  = cfd;
            s_conns[i].src = from.sin_addr.s_addr;
            return;
        }
    }
    close(cfd);                         /* no room: the client will ask again */
}

static int bind_udp(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int bind_tcp(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

esp_err_t espix_nfsd_run(bool (*keep_going)(void))
{
    if (s_req == NULL) {
        s_req   = heap_caps_malloc(BUFCAP, MALLOC_CAP_SPIRAM);
        s_rep   = heap_caps_malloc(BUFCAP, MALLOC_CAP_SPIRAM);
        s_frame = heap_caps_malloc(BUFCAP, MALLOC_CAP_SPIRAM);
        s_out   = heap_caps_malloc(BUFCAP + 4, MALLOC_CAP_SPIRAM);
        s_io    = heap_caps_malloc(IOCAP, MALLOC_CAP_SPIRAM);
        if (s_req == NULL || s_rep == NULL || s_frame == NULL ||
            s_out == NULL || s_io == NULL) {
            s_req   = heap_caps_malloc(BUFCAP, MALLOC_CAP_8BIT);
            s_rep   = heap_caps_malloc(BUFCAP, MALLOC_CAP_8BIT);
            s_frame = heap_caps_malloc(BUFCAP, MALLOC_CAP_8BIT);
            s_out   = heap_caps_malloc(BUFCAP + 4, MALLOC_CAP_8BIT);
            s_io    = heap_caps_malloc(IOCAP, MALLOC_CAP_8BIT);
        }
        if (s_req == NULL || s_rep == NULL || s_frame == NULL ||
            s_out == NULL || s_io == NULL) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "no buffers");
            return ESP_ERR_NO_MEM;
        }
    }

    if (load_exports() <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const int pm_udp = bind_udp(PORTMAP_PORT);
    const int pm_tcp = bind_tcp(PORTMAP_PORT);
    const int mt_udp = bind_udp(MOUNT_PORT);
    const int mt_tcp = bind_tcp(MOUNT_PORT);
    const int nf_udp = bind_udp(NFS_PORT);
    const int nf_tcp = bind_tcp(NFS_PORT);
    if (pm_udp < 0 || pm_tcp < 0 || mt_udp < 0 || mt_tcp < 0 ||
        nf_udp < 0 || nf_tcp < 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "cannot bind 111, %d or %d (already running?)",
                   MOUNT_PORT, NFS_PORT);
        return ESP_FAIL;
    }

    for (int i = 0; i < MAX_CONNS; i++) {
        s_conns[i].fd = -1;
    }

    reg_add(PORTMAP_PROG, PORTMAP_VERS, PORTMAP_PORT);
    reg_add(MOUNT_PROG, 1, MOUNT_PORT);
    reg_add(MOUNT_PROG, 2, MOUNT_PORT);
    reg_add(MOUNT_PROG, MOUNT_VERS, MOUNT_PORT);
    reg_add(NFS_PROG, NFS_VERS, NFS_PORT);
    /* statd shares the nfsd port: the program number separates them. */
    reg_add(NSM_PROG, NSM_VERS, NFS_PORT);

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "serving: portmap 111, mountd %d, nfsd %d",
               MOUNT_PORT, NFS_PORT);

    for (;;) {
        /* Asked to stop: the sockets go with us, so a restart can bind them. */
        if (keep_going != NULL && !keep_going()) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "stopping");
            break;
        }

        fd_set         r;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

        FD_ZERO(&r);
        FD_SET(pm_udp, &r);
        FD_SET(pm_tcp, &r);
        FD_SET(mt_udp, &r);
        FD_SET(mt_tcp, &r);
        FD_SET(nf_udp, &r);
        FD_SET(nf_tcp, &r);

        int max = pm_udp;
        const int lfds[] = { pm_tcp, mt_udp, mt_tcp, nf_udp, nf_tcp };
        for (size_t i = 0; i < sizeof(lfds) / sizeof(lfds[0]); i++) {
            if (lfds[i] > max) {
                max = lfds[i];
            }
        }
        for (int i = 0; i < MAX_CONNS; i++) {
            if (s_conns[i].fd >= 0) {
                FD_SET(s_conns[i].fd, &r);
                if (s_conns[i].fd > max) {
                    max = s_conns[i].fd;
                }
            }
        }

        if (select(max + 1, &r, NULL, NULL, &tv) <= 0) {
            continue;
        }

        if (FD_ISSET(pm_udp, &r)) serve_udp(pm_udp);
        if (FD_ISSET(mt_udp, &r)) serve_udp(mt_udp);
        if (FD_ISSET(nf_udp, &r)) serve_udp(nf_udp);

        if (FD_ISSET(pm_tcp, &r)) conn_accept(pm_tcp);
        if (FD_ISSET(mt_tcp, &r)) conn_accept(mt_tcp);
        if (FD_ISSET(nf_tcp, &r)) conn_accept(nf_tcp);

        for (int i = 0; i < MAX_CONNS; i++) {
            if (s_conns[i].fd >= 0 && FD_ISSET(s_conns[i].fd, &r)) {
                conn_service(i);
            }
        }
    }

    close(pm_udp);
    close(pm_tcp);
    close(mt_udp);
    close(mt_tcp);
    close(nf_udp);
    close(nf_tcp);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].fd >= 0) {
            close(s_conns[i].fd);
            s_conns[i].fd = -1;
        }
    }

    /* Stopped means stopped: the buffers and the export table go back, so a
     * daemon that is not running costs nothing but the four pointers. A
     * restart allocates them again. */
    free(s_req);     s_req = NULL;
    free(s_rep);     s_rep = NULL;
    free(s_frame);   s_frame = NULL;
    free(s_out);     s_out = NULL;
    free(s_io);      s_io = NULL;
    free(s_exports); s_exports = NULL;
    s_nexports = 0;
    /* Nothing can be holding a handle: no daemon is left to resolve one. */
    nfs3_slots_cleanup();
    memset(s_mounted, 0, sizeof(s_mounted));
    return ESP_OK;
}

int espix_nfsd_export_count(void)
{
    return s_nexports;
}

bool espix_nfsd_export_info(int i, const char **path, bool *ro, const char **who)
{
    if (i < 0 || i >= s_nexports) {
        return false;
    }
    *path = s_exports[i].path;
    *ro   = s_exports[i].ro;
    *who  = s_exports[i].all ? "*" : "listed clients";
    return true;
}
