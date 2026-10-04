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

#include "rpc.h"

#define TAG "espix:nfsd"

#define PORTMAP_PROG 100000u
#define PORTMAP_VERS 2u
#define MOUNT_PROG   100005u
#define MOUNT_VERS   3u

#define PORTMAP_PORT 111
#define MOUNT_PORT   20048          /* ours to choose: mountd has no default */
#define NFS_PORT     2049

#define MAX_EXPORTS 8
#define MAX_CLIENTS 8

#define BUFCAP 8192

typedef struct {
    uint32_t addr;                  /* network order, as sin_addr is */
    uint32_t mask;
} nfs_client_t;

typedef struct {
    char         path[192];
    bool         ro;
    bool         all;               /* "*" */
    nfs_client_t clients[MAX_CLIENTS];
    int          nclients;
} nfs_export_t;

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

static size_t handle_mount(const rpc_call_t *c, xdrw_t *w, uint32_t src)
{
    switch (c->proc) {
    case 0:                                     /* NULL */
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;

    case 3: {                                   /* UMNT */
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;
    }

    case 4:                                     /* UMNTALL */
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

    xdrw_init(&w, rep, cap, c->xid);

    if (c->prog == PORTMAP_PROG && c->vers == PORTMAP_VERS) {
        return handle_portmap(c, &w);
    }
    if (c->prog == MOUNT_PROG &&
        (c->vers == 1 || c->vers == 2 || c->vers == MOUNT_VERS)) {
        return handle_mount(c, &w, src);
    }

    /* Unknown program: say so, with the version range we do have. */
    if (c->prog == PORTMAP_PROG || c->prog == MOUNT_PROG) {
        xdrw_accept(&w, RPC_PROG_MISMATCH);
        xdrw_u32(&w, 1);
        xdrw_u32(&w, 3);
    } else {
        xdrw_accept(&w, RPC_PROG_UNAVAIL);
    }
    return w.len;
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
        sendto(fd, s_rep, rlen, 0, (struct sockaddr *)&from, flen);
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

static void serve_tcp_conn(int fd, uint32_t src)
{
    for (;;) {
        uint8_t mark[4];

        if (!recv_all(fd, mark, 4)) {
            break;
        }
        const uint32_t m   = ((uint32_t)mark[0] << 24) | ((uint32_t)mark[1] << 16) |
                             ((uint32_t)mark[2] << 8) | mark[3];
        const size_t   len = m & 0x7FFFFFFFu;
        if (len == 0 || len > BUFCAP || !recv_all(fd, s_frame, len)) {
            break;
        }

        rpc_call_t c;
        if (!rpc_parse_call(&c, s_frame, len)) {
            break;
        }
        const size_t rlen = dispatch(&c, s_rep, BUFCAP, src);
        if (rlen > 0) {
            const size_t fl = rpc_tcp_frame(s_rep, rlen, s_out, BUFCAP + 4);
            if (fl == 0 || send(fd, s_out, fl, 0) < 0) {
                break;
            }
        }
    }
    close(fd);
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
        if (s_req == NULL || s_rep == NULL || s_frame == NULL || s_out == NULL) {
            s_req   = heap_caps_malloc(BUFCAP, MALLOC_CAP_8BIT);
            s_rep   = heap_caps_malloc(BUFCAP, MALLOC_CAP_8BIT);
            s_frame = heap_caps_malloc(BUFCAP, MALLOC_CAP_8BIT);
            s_out   = heap_caps_malloc(BUFCAP + 4, MALLOC_CAP_8BIT);
        }
        if (s_req == NULL || s_rep == NULL || s_frame == NULL || s_out == NULL) {
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
    if (pm_udp < 0 || pm_tcp < 0 || mt_udp < 0 || mt_tcp < 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "cannot bind 111/20048 (already running?)");
        return ESP_FAIL;
    }

    reg_add(PORTMAP_PROG, PORTMAP_VERS, PORTMAP_PORT);
    reg_add(MOUNT_PROG, 1, MOUNT_PORT);
    reg_add(MOUNT_PROG, 2, MOUNT_PORT);
    reg_add(MOUNT_PROG, MOUNT_VERS, MOUNT_PORT);

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "serving: portmap 111, mountd %d, nfsd %d not yet",
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
        int max = pm_udp;
        if (pm_tcp > max) max = pm_tcp;
        if (mt_udp > max) max = mt_udp;
        if (mt_tcp > max) max = mt_tcp;

        if (select(max + 1, &r, NULL, NULL, &tv) <= 0) {
            continue;
        }

        if (FD_ISSET(pm_udp, &r)) serve_udp(pm_udp);
        if (FD_ISSET(mt_udp, &r)) serve_udp(mt_udp);

        for (int i = 0; i < 2; i++) {
            const int lfd = (i == 0) ? pm_tcp : mt_tcp;
            if (!FD_ISSET(lfd, &r)) {
                continue;
            }
            struct sockaddr_in from;
            socklen_t          flen = sizeof(from);
            const int cfd = accept(lfd, (struct sockaddr *)&from, &flen);
            if (cfd >= 0) {
                serve_tcp_conn(cfd, from.sin_addr.s_addr);
            }
        }
    }

    close(pm_udp);
    close(pm_tcp);
    close(mt_udp);
    close(mt_tcp);
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
