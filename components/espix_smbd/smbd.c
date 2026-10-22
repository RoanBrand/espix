/*
 * smbd -- the SMB2/3 file server.
 *
 * One share and, by default, guest-only. /etc/smb.conf may name a `user` and
 * `password`; with those the daemon authenticates NTLMv2 and turns signing
 * on, which is what Windows needs, and guest access still works alongside.
 * The root is `path` (default /home).
 *
 * libsmb2's server engine is a bare SMB2/3 state machine with a handler
 * table; it does not touch a filesystem. These handlers are the other half:
 * an SMB2 create/read/write/close/query is mapped onto espix's VFS with
 * ordinary POSIX calls, the same path nfs3.c takes, which is why a share is
 * /home and not a second filesystem.
 *
 * It runs one select() loop on one task, so every handler runs to completion
 * before the next. That is what makes one connection table and one pair of
 * reply scratch buffers safe without a lock.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_smbd.h"
#include "smbd_srvsvc.h"

static const char *TAG = "espix:smbd";

#define SMBD_CONF_PATH     "/etc/smb.conf"
#define SMBD_MAX_CONNS     16
#define SMBD_MAX_OPEN      32
#define SMBD_MAX_TREES     8
#define SMBD_PATH_MAX      512
#define SMBD_DEFAULT_PATH  "/home"
#define SMBD_DEFAULT_SHARE "home"
#define SMBD_DEFAULT_IO    (64 * 1024)

/* MS-SMB2 2.2.33: start the directory enumeration over; return one entry. */
#define SMBD_QDIR_RESTART  0x01
#define SMBD_QDIR_SINGLE   0x02

#define PAD64(x) (((x) + 7u) & ~7u)
#define PAD32(x) (((x) + 3u) & ~3u)

/*
 * One open file or directory on a connection. The client is given `id` in the
 * create reply and hands it back on every later request, so this is the only
 * thing that links an SMB2 file id to a VFS descriptor.
 */
typedef struct {
    bool     used;
    bool     is_dir;
    int      fd;
    int      flags;      /* the open(2) flags, to reopen after a rename */
    DIR     *dir;
    uint64_t id;
    char     path[SMBD_PATH_MAX];
    /* A directory entry that did not fit the client's last buffer; readdir
     * cannot be put back, so it is carried here instead of being lost. */
    char     pending[256];
    bool     have_pending;
    bool     delete_on_close;
    bool     dir_queried;  /* a search has been made since open or restart */
    uint32_t dir_index;   /* file_index for the next directory entry */
    smbd_srvsvc_pipe_t *pipe;   /* the srvsvc pipe on IPC$: no fd, no path */
} smbd_open_t;

typedef struct {
    bool                 used;
    struct smb2_context *smb2;
    uint64_t             next_id;
    uint64_t             last_id;   /* most recent create, for related compounds */
    bool                 in_session;
    bool                 authed;
    char                 user[64];
    char                 last[80];  /* the last request a handler saw, for the close line */
    /* The trees connected, by the id libsmb2 patch 0008 shows the handler,
     * and whether each is IPC$: a create there names a pipe, not a path. */
    struct {
        uint32_t id;
        bool     ipc;
    }                    trees[SMBD_MAX_TREES];
    smbd_open_t          opens[SMBD_MAX_OPEN];
} smbd_conn_t;

static smbd_conn_t       *s_conns;      /* PSRAM, allocated in smbd_run */
static struct smb2_server s_server;

static bool (*s_keep_going)(void);
static char  s_path[SMBD_PATH_MAX] = SMBD_DEFAULT_PATH;
static char  s_share[256]          = SMBD_DEFAULT_SHARE;
static uint32_t s_max_io = SMBD_DEFAULT_IO;

/*
 * Optional credentials from /etc/smb.conf. With them, a client that asks for
 * signing -- Windows 10/11, and every Windows 11 24H2 -- can be given a
 * session key to sign with, which a guest session cannot produce. Without
 * them the daemon stays guest-only, as before.
 */
static char  s_user[64];
static char  s_pass[128];

/* Reply scratch, allocated once: the handlers fill them and the library
 * encodes out of them before returning, so one pair serves every connection.
 * Kept off the stack because a client may ask for a full 64K listing. */
static uint8_t *s_dents;      /* array of struct ...directoryinformation, fixed stride */
static uint8_t *s_dnames;     /* the entry names those structs point at */
static size_t   s_dents_len;
static size_t   s_dnames_len;

/* ------------------------------------------------------------ stat cache */

#define SMBD_STAT_SLOTS 32
#define SMBD_STAT_TTL   pdMS_TO_TICKS(1000)

/*
 * littlefs walks the path on flash for every stat(), and its fstat() does the
 * same through the open file's name: ~40 ms for a file two levels down on a
 * 71% full root. Windows asks about one file many times in a burst -- opening
 * one picture took 108 attribute queries and 22 opens -- so a short-lived
 * cache answers the burst. smbd forgets what it changes itself; the TTL bounds
 * how stale a change made on the device can look.
 */
typedef struct {
    bool        used;
    int         err;        /* 0, or the errno stat() failed with */
    TickType_t  at;
    struct stat st;
    char        path[SMBD_PATH_MAX];
} smbd_stat_t;

static smbd_stat_t *s_stats;    /* PSRAM, allocated in smbd_run */
static size_t       s_stat_next;

static smbd_stat_t *stat_slot(const char *path)
{
    for (size_t i = 0; i < SMBD_STAT_SLOTS; i++) {
        if (s_stats[i].used && strcmp(s_stats[i].path, path) == 0) {
            return &s_stats[i];
        }
    }
    return NULL;
}

static int cached_stat(const char *path, struct stat *st)
{
    const TickType_t now = xTaskGetTickCount();
    smbd_stat_t *e = (s_stats != NULL) ? stat_slot(path) : NULL;

    if (e != NULL && now - e->at < SMBD_STAT_TTL) {
        if (e->err != 0) {
            errno = e->err;
            return -1;
        }
        *st = e->st;
        return 0;
    }

    const int rc = stat(path, st);
    const int err = (rc == 0) ? 0 : errno;
    const size_t len = strlen(path);
    if (s_stats != NULL && len < SMBD_PATH_MAX) {
        if (e == NULL) {
            e = &s_stats[s_stat_next++ % SMBD_STAT_SLOTS];
        }
        e->used = true;
        e->err = err;
        e->at = now;
        if (rc == 0) {
            e->st = *st;
        }
        memcpy(e->path, path, len + 1);
    }
    errno = err;
    return rc;
}

/* Forget one path, or everything when a rename or rmdir moves many at once. */
static void stat_forget(const char *path)
{
    if (s_stats == NULL) {
        return;
    }
    if (path == NULL) {
        for (size_t i = 0; i < SMBD_STAT_SLOTS; i++) {
            s_stats[i].used = false;
        }
        return;
    }
    smbd_stat_t *e = stat_slot(path);
    if (e != NULL) {
        e->used = false;
    }
}

/* ------------------------------------------------------------------- log */

#define SMBD_QUIET_SECS 30

/*
 * One line per distinct event, then a count. Finder opens a session every
 * ~200 ms while a share is mounted, and Windows retries a refused logon ~25
 * times in as many milliseconds. Logged line by line, either fills the kernel
 * ring, and klog then drops the DEBUG lines that would explain what came next.
 * A repeat inside the window goes to DEBUG instead.
 */
typedef struct {
    char     line[160];
    time_t   since;
    uint32_t repeats;
} smbd_quiet_t;

static smbd_quiet_t s_quiet_session;
static smbd_quiet_t s_quiet_logon;
static smbd_quiet_t s_quiet_close;

static void log_quiet(smbd_quiet_t *q, espix_klog_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void log_quiet(smbd_quiet_t *q, espix_klog_level_t level, const char *fmt, ...)
{
    char line[sizeof(q->line)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    const time_t now = time(NULL);
    if (strcmp(line, q->line) == 0 && now - q->since < SMBD_QUIET_SECS) {
        q->repeats++;
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "%s", line);
        return;
    }
    if (q->repeats > 0) {
        espix_klog(level, TAG, "%s (%u more)", q->line, (unsigned)q->repeats);
    }
    memcpy(q->line, line, sizeof(q->line));
    q->since = now;
    q->repeats = 0;
    espix_klog(level, TAG, "%s", line);
}

/* A failed request: its status, and one DEBUG line saying which and why. */
static int fail(struct smb2_context *smb2, uint32_t status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(struct smb2_context *smb2, uint32_t status, const char *fmt, ...)
{
    char what[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "%s -> 0x%08x", what, (unsigned)status);
    smb2_set_error_status(smb2, status);
    return -1;
}

/* ------------------------------------------------------------------ config */

static void conf_str(const char *key, char *out, size_t outlen)
{
    char v[SMBD_PATH_MAX];
    if (espix_fs_conf_get(SMBD_CONF_PATH, key, v, sizeof(v)) && v[0]) {
        snprintf(out, outlen, "%s", v);
    }
}

static void smbd_load_config(void)
{
    char v[64];

    conf_str("path", s_path, sizeof(s_path));
    conf_str("share", s_share, sizeof(s_share));
    conf_str("user", s_user, sizeof(s_user));
    conf_str("password", s_pass, sizeof(s_pass));

    /* No trailing slash: the resolver appends /name to it. */
    size_t n = strlen(s_path);
    while (n > 1 && s_path[n - 1] == '/') {
        s_path[--n] = 0;
    }

    if (espix_fs_conf_get(SMBD_CONF_PATH, "max_io", v, sizeof(v))) {
        const long kb = strtol(v, NULL, 10);
        if (kb >= 4 && kb <= 128) {
            s_max_io = (uint32_t)kb * 1024;
        }
    }
}

static bool smbd_wanted(void)
{
    char v[8] = {0};
    if (espix_fs_conf_get(SMBD_CONF_PATH, "enabled", v, sizeof(v))) {
        return !(strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0 ||
                 strcmp(v, "0") == 0);
    }
    return true;
}

/* ------------------------------------------------------------- connection */

static smbd_conn_t *conn_of(struct smb2_context *smb2)
{
    if (s_conns == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SMBD_MAX_CONNS; i++) {
        if (s_conns[i].used && s_conns[i].smb2 == smb2) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static smbd_conn_t *conn_new(struct smb2_context *smb2)
{
    if (s_conns == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SMBD_MAX_CONNS; i++) {
        if (!s_conns[i].used) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            s_conns[i].used = true;
            s_conns[i].smb2 = smb2;
            for (size_t j = 0; j < SMBD_MAX_OPEN; j++) {
                s_conns[i].opens[j].fd = -1;
            }
            return &s_conns[i];
        }
    }
    return NULL;
}

static void open_clear(smbd_open_t *o)
{
    if (o->dir) {
        closedir(o->dir);
    }
    if (o->fd >= 0) {
        close(o->fd);
    }
    if (o->delete_on_close && o->path[0]) {
        (void)(o->is_dir ? rmdir(o->path) : unlink(o->path));
        stat_forget(o->is_dir ? NULL : o->path);
    }
    if (o->pipe != NULL) {
        smbd_srvsvc_close(o->pipe);
        o->pipe = NULL;
    }
    o->used = false;
    o->is_dir = false;
    o->dir = NULL;
    o->fd = -1;
    o->have_pending = false;
    o->pending[0] = 0;
}

static bool tree_add(smbd_conn_t *c, uint32_t id, bool ipc)
{
    for (size_t i = 0; i < SMBD_MAX_TREES; i++) {
        if (c->trees[i].id == 0) {
            c->trees[i].id = id;
            c->trees[i].ipc = ipc;
            return true;
        }
    }
    return false;
}

static void tree_remove(smbd_conn_t *c, uint32_t id)
{
    for (size_t i = 0; i < SMBD_MAX_TREES; i++) {
        if (c->trees[i].id == id) {
            c->trees[i].id = 0;
        }
    }
}

/* Whether the request being handled is on IPC$. */
static bool tree_is_ipc(smbd_conn_t *c, struct smb2_context *smb2)
{
    const uint32_t id = smb2_get_request_tree_id(smb2);
    for (size_t i = 0; i < SMBD_MAX_TREES; i++) {
        if (id != 0 && c->trees[i].id == id) {
            return c->trees[i].ipc;
        }
    }
    return false;
}

static void conn_free(smbd_conn_t *c)
{
    if (c == NULL) {
        return;
    }
    for (size_t i = 0; i < SMBD_MAX_OPEN; i++) {
        if (c->opens[i].used) {
            open_clear(&c->opens[i]);
        }
    }
    c->used = false;
    c->smb2 = NULL;
}

static bool id_is_compound(const smb2_file_id file_id);

static smbd_open_t *open_find(smbd_conn_t *c, const smb2_file_id file_id)
{
    uint64_t id;
    memcpy(&id, file_id, sizeof(id));
    for (size_t i = 0; i < SMBD_MAX_OPEN; i++) {
        if (c->opens[i].used && c->opens[i].id == id) {
            return &c->opens[i];
        }
    }
    return NULL;
}

/*
 * A related compound -- CREATE, then QUERY or CLOSE in the same PDU -- has no
 * way for the client to know the file id the server is about to assign, so it
 * sends libsmb2's compound_file_id (all ones) and the server is expected to
 * resolve it to whatever the preceding create opened. libsmb2 does not do that
 * substitution for the server, so it is done here: the most recent create on
 * this connection is the referent, and create is the only thing that moves it.
 */
static smbd_open_t *open_find_any(smbd_conn_t *c, const smb2_file_id file_id)
{
    smbd_open_t *o = open_find(c, file_id);
    if (o != NULL) {
        return o;
    }
    if (id_is_compound(file_id)) {
        for (size_t i = 0; i < SMBD_MAX_OPEN; i++) {
            if (c->opens[i].used && c->opens[i].id == c->last_id) {
                return &c->opens[i];
            }
        }
    }
    return NULL;
}

static smbd_open_t *open_alloc(smbd_conn_t *c, smb2_file_id out_id)
{
    for (size_t i = 0; i < SMBD_MAX_OPEN; i++) {
        if (!c->opens[i].used) {
            memset(&c->opens[i], 0, sizeof(c->opens[i]));
            c->opens[i].used = true;
            c->opens[i].fd = -1;
            c->opens[i].id = ++c->next_id;
            c->last_id = c->opens[i].id;
            memset(out_id, 0, SMB2_FD_SIZE);
            memcpy(out_id, &c->opens[i].id, sizeof(c->opens[i].id));
            return &c->opens[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ paths */

/*
 * Turn an SMB2 name (`dir\file`, "" for the share root) into a path under the
 * share. Absolute names and `..` are refused rather than clamped: a share must
 * not be a way out of its own root, and refusing is cheaper than proving a
 * normalisation correct.
 */
static bool resolve_path(const char *name, char *out, size_t outlen)
{
    size_t n = (size_t)snprintf(out, outlen, "%s", s_path);
    if (n >= outlen) {
        return false;
    }
    if (name == NULL || name[0] == 0) {
        return true;
    }
    if (name[0] == '\\' || name[0] == '/') {
        return false;
    }
    /* One separator between the share root and the relative name. The root is
     * normalised without a trailing slash, except when it is "/" itself. */
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= outlen) {
            return false;
        }
        out[n++] = '/';
    }

    for (const char *p = name; *p; p++) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == 0 || p[2] == '\\' || p[2] == '/')) {
            return false;
        }
        if (n + 1 >= outlen) {
            return false;
        }
        out[n++] = (*p == '\\') ? '/' : *p;
    }
    out[n] = 0;
    return true;
}

static void win_time(const time_t t, struct smb2_timeval *out)
{
    out->tv_sec = t;
    out->tv_usec = 0;
}

/*
 * 100 ns ticks since 1601-01-01, which is what the SMB2 create and close
 * replies carry raw -- unlike query-info, whose structs hold a timeval and let
 * the library convert. The epoch difference is 11644473600 seconds.
 */
static uint64_t filetime(const time_t t)
{
    return ((uint64_t)t + 11644473600ULL) * 10000000ULL;
}

/* littlefs keeps only the modification time: st_atime and st_ctime come
 * back 0, which every client shows as 1970. Report the mtime for those. */
static time_t atime_of(const struct stat *st)
{
    return (st->st_atime != 0) ? st->st_atime : st->st_mtime;
}

static time_t ctime_of(const struct stat *st)
{
    return (st->st_ctime != 0) ? st->st_ctime : st->st_mtime;
}

static uint32_t attrs_of(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                                : SMB2_FILE_ATTRIBUTE_ARCHIVE;
}

/* --------------------------------------------------------------- handlers */

static void note_request(smbd_conn_t *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void note_request(smbd_conn_t *c, const char *fmt, ...)
{
    if (c == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->last, sizeof(c->last), fmt, ap);
    va_end(ap);
}

static const char *user_or_none(struct smb2_context *smb2)
{
    const char *user = smb2_get_user(smb2);
    return (user != NULL) ? user : "";
}

static void on_error(struct smb2_context *smb2, const char *error_string)
{
    static const char failed[] = "Authentication failed: ";

    if (error_string == NULL || error_string[0] == 0) {
        return;
    }
    if (strncmp(error_string, failed, sizeof(failed) - 1) == 0) {
        const char *why = error_string + sizeof(failed) - 1;
        /* A name we do not hold was already reported by h_authorize. */
        if (strstr(why, "server can not authorize") == NULL) {
            if (strstr(why, "NTProof") != NULL) {
                why = "wrong password";
            }
            log_quiet(&s_quiet_logon, ESPIX_KLOG_WARN, "logon failed for '%s': %s",
                      user_or_none(smb2), why);
            return;
        }
    } else if (strstr(error_string, "ignature") != NULL ||
               strstr(error_string, "not signed") != NULL ||
               strstr(error_string, "Signing required") != NULL) {
        /* Each of these ends the connection. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s", error_string);
        return;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "%s", error_string);
}

static int h_session(struct smb2_server *srvr, struct smb2_context *smb2)
{
    (void)srvr;
    const bool authed = smb2_session_is_authenticated(smb2);
    const bool required = smb2_get_signing_required(smb2);

    smbd_conn_t *c = conn_of(smb2);
    if (c != NULL) {
        c->in_session = true;
        c->authed = authed;
        snprintf(c->user, sizeof(c->user), "%s", user_or_none(smb2));
    }
    log_quiet(&s_quiet_session, ESPIX_KLOG_INFO, "session '%s' %s, signing %s, dialect %04x",
              user_or_none(smb2), authed ? "authenticated" : "as guest",
              required ? "required" : (authed ? "when asked" : "off"),
              smb2_get_dialect(smb2));
    return 0;
}

static int h_authorize(struct smb2_server *srvr, struct smb2_context *smb2,
                       const char *user, const char *domain,
                       const char *workstation)
{
    (void)srvr; (void)workstation;
    /* The one line that says which client this is: Windows authenticates with
     * a name, Finder's Guest and KDE offer none. */
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "authorize user '%s' domain '%s'",
               user ? user : "", domain ? domain : "");

    if (s_user[0] != 0) {
        if (user == NULL || user[0] == 0 || strcasecmp(user, "guest") == 0) {
            /* Anonymous, which allow_anonymous permits. */
            return 0;
        }
        if (strcasecmp(user, s_user) == 0) {
            /*
             * libsmb2 asks this handler for the password, then verifies the
             * client's NTLMv2 proof against it and exports the session key,
             * which is what makes signing possible. It wipes the password
             * from the context once it has been used.
             */
            smb2_set_password(smb2, s_pass);
            return 0;
        }
        /*
         * A name we do not hold. Answering success would tell the client its
         * logon worked, so it would never ask for a password and would carry
         * on in a guest session; refusing is what raises the credential
         * dialog. Windows retries a refusal many times before it asks.
         */
        log_quiet(&s_quiet_logon, ESPIX_KLOG_WARN,
                  "logon refused for '%s': not the configured user", user);
        return -1;
    }

    /* No credential configured: the guest server this started as. */
    return 0;
}

static int h_logoff(struct smb2_server *srvr, struct smb2_context *smb2)
{
    (void)srvr; (void)smb2;
    return 0;
}

/*
 * The share component of a tree-connect path. The wire form is UTF-16 and is
 * "hostshare"; some clients send just "share", and some include the
 * terminating NUL in path_length, which must not become part of the name. Kept
 * as ASCII in a small buffer: a share name is short and this is only compared
 * and logged, never used to build a path.
 */
static void share_name(const struct smb2_tree_connect_request *req,
                       char *out, size_t outlen)
{
    out[0] = 0;
    if (req->path == NULL || req->path_length < 2 || outlen < 2) {
        return;
    }
    size_t units = req->path_length / 2;
    while (units > 0 && req->path[units - 1] == 0) {
        units--;
    }
    size_t start = 0;
    for (size_t i = units; i-- > 0; ) {
        if (req->path[i] == '\\') {
            start = i + 1;
            break;
        }
    }
    size_t j = 0;
    for (size_t i = start; i < units && j + 1 < outlen; i++) {
        const uint16_t c = req->path[i];
        out[j++] = (c < 0x80) ? (char)c : '?';
    }
    out[j] = 0;
}

static int h_tree_connect(struct smb2_server *srvr, struct smb2_context *smb2,
                          struct smb2_tree_connect_request *req,
                          struct smb2_tree_connect_reply *rep)
{
    (void)srvr;

    char name[64];
    share_name(req, name, sizeof(name));
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "tree connect '%s'", name);
    note_request(conn_of(smb2), "tree connect '%s'", name);

    /* IPC$ is the named-pipe tree. macOS and Windows connect to it before a
     * share, for DFS referrals and srvsvc, and treat a refusal as the server
     * being absent rather than as one feature missing. Accept the tree; the
     * pipes inside it are what is not implemented. */
    const bool ipc = (strcasecmp(name, "IPC$") == 0);
    if (!ipc && strcasecmp(name, s_share) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "tree connect refused '%s' (share is '%s')",
                   name, s_share);
        return fail(smb2, SMB2_STATUS_BAD_NETWORK_NAME,
                    "tree connect '%s'", name);
    }
    smbd_conn_t *c = conn_of(smb2);
    if (c != NULL && !tree_add(c, smb2_get_request_tree_id(smb2), ipc)) {
        return fail(smb2, SMB2_STATUS_INSUFFICIENT_RESOURCES,
                    "tree connect '%s'", name);
    }
    if (ipc) {
        rep->share_type = SMB2_SHARE_TYPE_PIPE;
        rep->maximal_access = 0x001f00a9;
        rep->share_flags = 0;
        rep->capabilities = 0;
        return 0;
    }
    rep->share_type = SMB2_SHARE_TYPE_DISK;
    rep->maximal_access = 0x001f01ff;   /* FILE_ALL_ACCESS */
    rep->share_flags = 0;
    rep->capabilities = 0;
    return 0;
}

static int h_tree_disconnect(struct smb2_server *srvr, struct smb2_context *smb2,
                             const uint32_t tree_id)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c != NULL) {
        tree_remove(c, tree_id);
    }
    return 0;
}

/*
 * The VFS's errno, as the NT status a client understands. Until patch 0002 the
 * library answered NOT_IMPLEMENTED to every failure, so a client that probed
 * for a file which was not there could not tell that from a server that cannot
 * do the operation at all -- and a rename, which opens the destination first,
 * gives up on anything that is not "no such file".
 */
static uint32_t status_of_errno(int err)
{
    switch (err) {
    case ENOENT:  return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
    case EACCES:
    case EPERM:   return SMB2_STATUS_ACCESS_DENIED;
    case EEXIST:  return SMB2_STATUS_OBJECT_NAME_COLLISION;
    case ENOTDIR: return SMB2_STATUS_NOT_A_DIRECTORY;
    case EISDIR:  return SMB2_STATUS_FILE_IS_A_DIRECTORY;
    case ENOSPC:  return SMB2_STATUS_DISK_FULL;
    case ENOTEMPTY: return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
    case EBUSY:   return SMB2_STATUS_SHARING_VIOLATION;
    case EINVAL:  return SMB2_STATUS_INVALID_PARAMETER;
    case ENAMETOOLONG: return SMB2_STATUS_OBJECT_NAME_INVALID;
    case EMFILE:
    case ENFILE:  return SMB2_STATUS_TOO_MANY_OPENED_FILES;
    default:      return SMB2_STATUS_NOT_IMPLEMENTED;
    }
}

static int create_failed(struct smb2_context *smb2, smbd_open_t *o,
                         const char *name, int err)
{
    /* Explorer probes desktop.ini in every folder, so a missing file is routine. */
    if (err != ENOENT) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "create '%s' failed: errno %d",
                   name ? name : "", err);
    }
    open_clear(o);
    return fail(smb2, status_of_errno(err), "create '%s'", name ? name : "");
}

/*
 * A create on IPC$ names a pipe. srvsvc and wkssvc are served (srvsvc.c); any
 * other name is "no such pipe", which clients take as that service being
 * absent. Nothing here touches the filesystem.
 */
static int create_pipe(smbd_conn_t *c, struct smb2_context *smb2,
                       struct smb2_create_request *req,
                       struct smb2_create_reply *rep)
{
    const char *name = req->name ? req->name : "";
    /* "srvsvc", "\srvsvc" and "\PIPE\srvsvc" all name the same pipe. */
    while (*name == '\\') {
        name++;
    }
    if (strncasecmp(name, "PIPE\\", 5) == 0) {
        name += 5;
    }
    smbd_pipe_kind_t kind;
    if (strcasecmp(name, "srvsvc") == 0) {
        kind = SMBD_PIPE_SRVSVC;
    } else if (strcasecmp(name, "wkssvc") == 0) {
        kind = SMBD_PIPE_WKSSVC;
    } else {
        return fail(smb2, SMB2_STATUS_OBJECT_NAME_NOT_FOUND, "create pipe '%s'", name);
    }
    smbd_open_t *o = open_alloc(c, rep->file_id);
    if (o == NULL) {
        return fail(smb2, SMB2_STATUS_TOO_MANY_OPENED_FILES, "create pipe '%s'", name);
    }
    o->pipe = smbd_srvsvc_open(kind);
    if (o->pipe == NULL) {
        open_clear(o);
        return fail(smb2, SMB2_STATUS_INSUFFICIENT_RESOURCES, "create pipe '%s'", name);
    }
    rep->create_action = 1;     /* opened */
    rep->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "create pipe '%s' ok", name);
    return 0;
}

static int h_create(struct smb2_server *srvr, struct smb2_context *smb2,
                    struct smb2_create_request *req,
                    struct smb2_create_reply *rep)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "create '%s' disp %u opts %08x access %08x",
               req->name ? req->name : "", (unsigned)req->create_disposition,
               (unsigned)req->create_options, (unsigned)req->desired_access);
    note_request(c, "create '%.60s'", req->name ? req->name : "");

    /* A name on IPC$ is a pipe. Its root ("") is not: Finder opens it as a
     * directory, and it stays the disk share's root, as it always was. */
    if (tree_is_ipc(c, smb2) && req->name != NULL && req->name[0] != 0) {
        return create_pipe(c, smb2, req, rep);
    }

    char path[SMBD_PATH_MAX];
    if (!resolve_path(req->name, path, sizeof(path))) {
        return fail(smb2, SMB2_STATUS_OBJECT_NAME_INVALID,
                    "create '%s'", req->name ? req->name : "");
    }

    struct stat st;
    const bool exists = (cached_stat(path, &st) == 0);
    const bool want_dir = ((req->create_options & SMB2_FILE_DIRECTORY_FILE) != 0) ||
                          (exists && S_ISDIR(st.st_mode));

    /* Clients tell a folder from a file by opening it as a file and being
     * refused: Samsung's My Files does this before listing a folder. */
    if (exists && S_ISDIR(st.st_mode) &&
        (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE) != 0) {
        return fail(smb2, SMB2_STATUS_FILE_IS_A_DIRECTORY,
                    "create '%s'", req->name ? req->name : "");
    }

    smbd_open_t *o = open_alloc(c, rep->file_id);
    if (o == NULL) {
        return fail(smb2, SMB2_STATUS_TOO_MANY_OPENED_FILES,
                    "create '%s'", req->name ? req->name : "");
    }
    snprintf(o->path, sizeof(o->path), "%s", path);

    if (want_dir) {
        DIR *d = opendir(path);
        if (d == NULL &&
            (req->create_disposition == SMB2_FILE_CREATE ||
             req->create_disposition == SMB2_FILE_OPEN_IF ||
             req->create_disposition == SMB2_FILE_OVERWRITE_IF)) {
            if (mkdir(path, 0755) == 0) {
                stat_forget(path);
                d = opendir(path);
            }
        }
        if (d == NULL) {
            return create_failed(smb2, o, req->name, errno);
        }
        o->is_dir = true;
        o->dir = d;
        rep->create_action = (exists || req->create_disposition != SMB2_FILE_CREATE)
                                 ? 1u : 2u;
    } else {
        int flags;
        switch (req->create_disposition) {
        case SMB2_FILE_CREATE:       flags = O_CREAT | O_EXCL;  break;
        case SMB2_FILE_OPEN_IF:      flags = O_CREAT;           break;
        case SMB2_FILE_OVERWRITE:    flags = O_TRUNC;           break;
        case SMB2_FILE_OVERWRITE_IF: flags = O_CREAT | O_TRUNC; break;
        case SMB2_FILE_SUPERSEDE:    flags = O_CREAT | O_TRUNC; break;
        case SMB2_FILE_OPEN:
        default:                     flags = 0;                 break;
        }
        if (req->desired_access &
            (SMB2_FILE_WRITE_DATA | SMB2_FILE_APPEND_DATA | SMB2_DELETE)) {
            flags |= O_RDWR;
        } else {
            flags |= O_RDONLY;
        }
        errno = 0;
        const int fd = open(path, flags, 0666);
        if (fd < 0) {
            return create_failed(smb2, o, req->name, errno);
        }
        if (flags & (O_CREAT | O_TRUNC)) {
            stat_forget(path);
        }
        o->fd = fd;
        o->flags = flags;
        rep->create_action = (req->create_disposition == SMB2_FILE_CREATE) ? 2u : 1u;
    }

    if (cached_stat(path, &st) == 0) {
        rep->creation_time = filetime(ctime_of(&st));
        rep->last_access_time = filetime(atime_of(&st));
        rep->last_write_time = filetime(st.st_mtime);
        rep->change_time = filetime(ctime_of(&st));
        rep->allocation_size = PAD64((uint64_t)st.st_size);
        rep->end_of_file = (uint64_t)st.st_size;
        rep->file_attributes = attrs_of(&st);
    } else {
        rep->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "create '%s' ok action %u id %02x",
               req->name ? req->name : "", (unsigned)rep->create_action,
               (unsigned)rep->file_id[0]);
    return 0;
}

static int h_close(struct smb2_server *srvr, struct smb2_context *smb2,
                   struct smb2_close_request *req,
                   struct smb2_close_reply *rep)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o == NULL) {
        /* Closing a handle that was never opened -- a compound that follows a
         * create which failed sends exactly that -- is a no-op, not an error.
         * Returning failure here painted the second half of such a compound
         * with a status the client then blamed on the first half's operation. */
        return 0;
    }
    if (o->pipe != NULL) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "close pipe");
        open_clear(o);
        return 0;
    }

    struct stat st;
    if (cached_stat(o->path, &st) == 0) {
        rep->creation_time = filetime(ctime_of(&st));
        rep->last_access_time = filetime(atime_of(&st));
        rep->last_write_time = filetime(st.st_mtime);
        rep->change_time = filetime(ctime_of(&st));
        rep->allocation_size = PAD64((uint64_t)st.st_size);
        rep->end_of_file = (uint64_t)st.st_size;
        rep->file_attributes = attrs_of(&st);
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "close '%s'%s", o->path,
               o->delete_on_close ? " (delete)" : "");
    open_clear(o);
    return 0;
}

static int h_flush(struct smb2_server *srvr, struct smb2_context *smb2,
                   struct smb2_flush_request *req)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o == NULL || o->is_dir || o->fd < 0) {
        return 0;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "flush '%s'", o->path);
    (void)fsync(o->fd);
    return 0;
}

/*
 * Answer a READ or a TRANSCEIVE with the pipe's next message. Built here, the
 * way libsmb2's own dcerpcd does it, rather than by the library: a fragment
 * larger than the client's buffer has to go out with STATUS_BUFFER_OVERFLOW,
 * which tells the client to read the rest, and a handler's reply cannot carry
 * a warning status. Returning 1 tells libsmb2 the reply is already queued.
 */
static int pipe_reply(struct smb2_context *smb2, smbd_open_t *o, uint16_t cmd,
                      const smb2_file_id file_id, uint32_t max, uint32_t flags)
{
    const uint32_t cap = (max < 0x10000u) ? max : 0x10000u;
    uint8_t *data = malloc(cap ? cap : 1);
    if (data == NULL) {
        return fail(smb2, SMB2_STATUS_INSUFFICIENT_RESOURCES, "pipe reply");
    }
    int more = 0;
    const size_t n = smbd_srvsvc_read(o->pipe, data, cap, &more);

    struct smb2_pdu *pdu;
    if (cmd == SMB2_READ) {
        if (n == 0) {
            free(data);
            return fail(smb2, SMB2_STATUS_PIPE_EMPTY, "pipe read");
        }
        struct smb2_read_reply rep;
        memset(&rep, 0, sizeof(rep));
        rep.data = data;            /* the encoder takes it */
        rep.data_length = (uint32_t)n;
        pdu = smb2_cmd_read_reply_async(smb2, &rep, NULL, NULL);
    } else {
        struct smb2_ioctl_reply rep;
        memset(&rep, 0, sizeof(rep));
        rep.ctl_code = SMB2_FSCTL_PIPE_TRANSCEIVE;
        memcpy(rep.file_id, file_id, SMB2_FD_SIZE);
        rep.output = data;          /* the encoder copies it */
        rep.output_count = (uint32_t)n;
        rep.flags = flags;
        pdu = smb2_cmd_ioctl_reply_async(smb2, &rep, NULL, NULL);
        free(data);
    }
    if (pdu == NULL) {
        return -1;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "pipe %s -> %u bytes%s",
               cmd == SMB2_READ ? "read" : "transceive", (unsigned)n,
               more ? ", more to read" : "");
    if (more) {
        smb2_set_pdu_status(smb2, pdu, SMB2_STATUS_BUFFER_OVERFLOW);
    }
    smb2_set_pdu_message_id(smb2, pdu, smb2_get_last_request_message_id(smb2));
    smb2_queue_pdu(smb2, pdu);
    return 1;
}

static int h_read(struct smb2_server *srvr, struct smb2_context *smb2,
                  struct smb2_read_request *req,
                  struct smb2_read_reply *rep)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o == NULL) {
        return fail(smb2, SMB2_STATUS_FILE_CLOSED,
                    "read off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)req->length);
    }
    if (o->pipe != NULL) {
        return pipe_reply(smb2, o, SMB2_READ, req->file_id, req->length, 0);
    }
    if (o->is_dir || o->fd < 0) {
        return fail(smb2, SMB2_STATUS_INVALID_DEVICE_REQUEST,
                    "read off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)req->length);
    }

    const uint32_t len = req->length;
    uint8_t *buf = malloc(len ? len : 1);
    if (buf == NULL) {
        return -1;
    }
    const ssize_t n = pread(o->fd, buf, len, (off_t)req->offset);
    if (n < 0) {
        const int e = errno;
        free(buf);
        return fail(smb2, status_of_errno(e), "read off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)len);
    }
    if (n == 0 && len > 0) {
        /* MS-SMB2 3.3.5.12: a read at or past the end is END_OF_FILE, not an
         * empty success. */
        free(buf);
        return fail(smb2, SMB2_STATUS_END_OF_FILE, "read off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)len);
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "read off %llu len %u -> %d",
               (unsigned long long)req->offset, (unsigned)len, (int)n);
    rep->data_offset = 0;
    rep->data = buf;
    rep->data_length = (uint32_t)n;
    rep->data_remaining = 0;
    return 0;
}

static int h_write(struct smb2_server *srvr, struct smb2_context *smb2,
                   struct smb2_write_request *req,
                   struct smb2_write_reply *rep)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o == NULL) {
        return fail(smb2, SMB2_STATUS_FILE_CLOSED,
                    "write off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)req->length);
    }
    if (o->pipe != NULL) {
        if (smbd_srvsvc_write(o->pipe, req->buf, req->length) != 0) {
            return fail(smb2, SMB2_STATUS_INVALID_PARAMETER, "pipe write: not DCE/RPC");
        }
        rep->count = req->length;
        rep->remaining = 0;
        return 0;
    }
    if (o->is_dir || o->fd < 0) {
        return fail(smb2, SMB2_STATUS_INVALID_DEVICE_REQUEST,
                    "write off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)req->length);
    }
    const ssize_t n = pwrite(o->fd, req->buf, req->length, (off_t)req->offset);
    stat_forget(o->path);
    if (n < 0) {
        return fail(smb2, status_of_errno(errno),
                    "write off %llu len %u",
                    (unsigned long long)req->offset, (unsigned)req->length);
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "write off %llu len %u -> %d",
               (unsigned long long)req->offset, (unsigned)req->length, (int)n);
    rep->count = (uint32_t)n;
    rep->remaining = 0;
    return 0;
}

static int h_lock(struct smb2_server *srvr, struct smb2_context *smb2,
                  struct smb2_lock_request *req)
{
    (void)srvr; (void)smb2; (void)req;
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "lock (granted, no lock manager)");
    return 0;
}

/*
 * No FSCTL is implemented. FSCTL_VALIDATE_NEGOTIATE_INFO never gets here:
 * libsmb2 answers it itself. INVALID_DEVICE_REQUEST is what a client takes as
 * "not on this server" and moves on from; DFS referrals have their own status,
 * which tells a client the share is not a DFS root.
 */
static int h_ioctl(struct smb2_server *srvr, struct smb2_context *smb2,
                   struct smb2_ioctl_request *req,
                   struct smb2_ioctl_reply *rep)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    note_request(c, "ioctl %08x", (unsigned)req->ctl_code);

    /* A DCE/RPC call on a pipe: write the request, answer with the reply. */
    if (req->ctl_code == SMB2_FSCTL_PIPE_TRANSCEIVE && c != NULL) {
        smbd_open_t *o = open_find_any(c, req->file_id);
        if (o == NULL || o->pipe == NULL) {
            return fail(smb2, SMB2_STATUS_INVALID_HANDLE, "transceive");
        }
        if (smbd_srvsvc_write(o->pipe, req->input, req->input_count) != 0) {
            return fail(smb2, SMB2_STATUS_INVALID_PARAMETER, "transceive: not DCE/RPC");
        }
        return pipe_reply(smb2, o, SMB2_IOCTL, req->file_id,
                          req->max_output_response, req->flags);
    }
    /* WaitNamedPipe: the one pipe there is never busy. */
    if (req->ctl_code == SMB2_FSCTL_PIPE_WAIT) {
        rep->ctl_code = req->ctl_code;
        memcpy(rep->file_id, req->file_id, SMB2_FD_SIZE);
        return 0;
    }
    return fail(smb2, req->ctl_code == SMB2_FSCTL_DFS_GET_REFERRALS
                          ? SMB2_STATUS_FS_DRIVER_REQUIRED
                          : SMB2_STATUS_INVALID_DEVICE_REQUEST,
                "ioctl %08x", (unsigned)req->ctl_code);
}

static int h_cancel(struct smb2_server *srvr, struct smb2_context *smb2)
{
    (void)srvr; (void)smb2;
    return 0;
}

static int h_echo(struct smb2_server *srvr, struct smb2_context *smb2)
{
    (void)srvr; (void)smb2;
    return 0;
}

static bool id_is_zero(const smb2_file_id file_id)
{
    for (size_t i = 0; i < SMB2_FD_SIZE; i++) {
        if (file_id[i]) {
            return false;
        }
    }
    return true;
}

/*
 * The "compound file id" libsmb2 sends when a request is not about an open
 * handle -- all ones. A filesystem-info query on a tree has no file, so the
 * client uses this, and treating it as unknown is what made macOS's mount fail
 * at FileFsFullSizeInformation with EOPNOTSUPP.
 */
static bool id_is_compound(const smb2_file_id file_id)
{
    for (size_t i = 0; i < SMB2_FD_SIZE; i++) {
        if (file_id[i] != 0xff) {
            return false;
        }
    }
    return true;
}

/*
 * A stable, per-name id for a directory entry. littlefs leaves st_ino at 0,
 * and a client that identifies entries by FileId would then be told that every
 * entry in the directory is the same file -- which is a good way to have the
 * first name shown for all of them.
 */
static uint64_t name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s != 0) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void dir_entry_fill(struct smb2_fileidbothdirectoryinformation *fsb,
                           const char *name, const struct stat *st,
                           uint32_t index, uint8_t *name_store)
{
    memset(fsb, 0, sizeof(*fsb));
    fsb->file_index = index;
    win_time(ctime_of(st), &fsb->creation_time);
    win_time(atime_of(st), &fsb->last_access_time);
    win_time(st->st_mtime, &fsb->last_write_time);
    win_time(ctime_of(st), &fsb->change_time);
    fsb->end_of_file = (uint64_t)st->st_size;
    fsb->allocation_size = PAD64((uint64_t)st->st_size);
    fsb->file_attributes = attrs_of(st);
    fsb->file_name_length = (uint32_t)(2 * strlen(name));
    fsb->ea_size = 0;
    fsb->short_name_length = 0;
    fsb->file_id = (st->st_ino != 0) ? (uint64_t)st->st_ino : name_hash(name);
    snprintf((char *)name_store, 256, "%s", name);
    fsb->name = (const char *)name_store;
}

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/*
 * SMB search patterns: '*' any run, '?' one character, compared without case.
 * Honouring the pattern is not optional -- macOS probes a share for its
 * metadata markers (.metadata_never_index, .Spotlight-V100, DCIM, Store-V1,
 * ...) by naming them, and a server that ignores the name answers "yes" to
 * every probe with whatever the first entry happens to be. That is what put
 * one name on the whole listing.
 */
static bool name_matches(const char *pattern, const char *name)
{
    const char *p;
    const char *n;
    const char *star = NULL;
    const char *mark = NULL;

    if (pattern == NULL || pattern[0] == '\0') {
        return true;
    }
    p = pattern;
    n = name;
    while (*n != '\0') {
        if (*p == '?' || lower_ascii(*p) == lower_ascii(*n)) {
            p++;
            n++;
        } else if (*p == '*') {
            star = p++;
            mark = n;
        } else if (star != NULL) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

/*
 * The wire size of one entry's fixed part in each directory class libsmb2
 * encodes (patch 0005), 0 for any other. Used to keep a reply inside the
 * client's buffer, and to refuse a class rather than send an empty success.
 */
static size_t dir_class_fixed(uint8_t info_class)
{
    switch (info_class) {
    case SMB2_FILE_DIRECTORY_INFORMATION:         return 64;
    case SMB2_FILE_FULL_DIRECTORY_INFORMATION:    return 68;
    case SMB2_FILE_BOTH_DIRECTORY_INFORMATION:    return 94;
    case SMB2_FILE_NAMES_INFORMATION:             return 12;
    case SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION: return SMB2_FILEID_FULL_DIRECTORY_INFORMATION_SIZE;
    case SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION: return SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE;
    default:                                      return 0;
    }
}

static int h_query_directory(struct smb2_server *srvr, struct smb2_context *smb2,
                             struct smb2_query_directory_request *req,
                             struct smb2_query_directory_reply *rep)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o == NULL) {
        return fail(smb2, SMB2_STATUS_FILE_CLOSED,
                    "query_directory class %u", (unsigned)req->file_information_class);
    }
    if (!o->is_dir || o->dir == NULL) {
        return fail(smb2, SMB2_STATUS_INVALID_PARAMETER,
                    "query_directory class %u", (unsigned)req->file_information_class);
    }
    note_request(c, "query_directory class %u", (unsigned)req->file_information_class);
    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "query_directory class %u flags %02x index %u name '%s'",
               (unsigned)req->file_information_class, (unsigned)req->flags,
               (unsigned)req->file_index, req->name ? req->name : "");
    const size_t fixed = dir_class_fixed(req->file_information_class);
    if (fixed == 0) {
        return fail(smb2, SMB2_STATUS_INVALID_INFO_CLASS,
                    "query_directory class %u", (unsigned)req->file_information_class);
    }
    if (req->flags & SMBD_QDIR_RESTART) {
        rewinddir(o->dir);
        o->have_pending = false;
        o->dir_index = 0;
        o->dir_queried = false;
    }
    const bool first = !o->dir_queried;
    o->dir_queried = true;
    /*
     * macOS does not keep one open directory between entries: with
     * FILE_ID_BOTH + RETURN_SINGLE_ENTRY + RESTART_SCANS it opens the directory
     * afresh for every entry and asks for it by file_index. Honouring RESTART
     * literally would then return entry 0 for ever -- a listing of one name
     * repeated, which is exactly what it did. Seek to the requested index and
     * report it, so the answer is the entry that was asked for.
     */
    if (req->file_index > 0) {
        rewinddir(o->dir);
        uint32_t skip = req->file_index;
        while (skip > 0) {
            const struct dirent *de = readdir(o->dir);
            if (de == NULL) {
                break;
            }
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            if (!name_matches(req->name, de->d_name)) {
                continue;
            }
            skip--;
        }
        if (skip > 0) {
            espix_klog(ESPIX_KLOG_DEBUG, TAG,
                       "query_directory '%s' index %u: past the end",
                       req->name ? req->name : "", (unsigned)req->file_index);
            rep->output_buffer = s_dents;
            rep->output_buffer_length = 0;
            return 0;
        }
        o->dir_index = req->file_index;
    }

    const size_t stride = PAD64(sizeof(struct smb2_fileidbothdirectoryinformation));
    const size_t cap = s_dents_len;
    const size_t client_cap = req->output_buffer_length;
    size_t used = 0;         /* bytes of s_dents */
    size_t nnames = 0;
    size_t encoded_used = 0;
    char path[SMBD_PATH_MAX];

    for (;;) {
        char entry[256];
        if (o->have_pending) {
            snprintf(entry, sizeof(entry), "%s", o->pending);
            o->have_pending = false;
        } else {
            const struct dirent *de = readdir(o->dir);
            while (de != NULL &&
                   (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)) {
                de = readdir(o->dir);
            }
            if (de == NULL) {
                break;
            }
            snprintf(entry, sizeof(entry), "%s", de->d_name);
        }

        if (!name_matches(req->name, entry)) {
            continue;
        }

        /* The entry's own path, for its attributes. Joined by hand rather than
         * with snprintf because the compiler cannot see that the two halves
         * fit, and flags the format under -Werror. */
        const size_t plen = strlen(o->path);
        const size_t elen = strlen(entry);
        if (plen + 1 + elen + 1 > sizeof(path)) {
            continue;       /* a path this long is not reachable through the VFS */
        }
        memcpy(path, o->path, plen);
        path[plen] = '/';
        memcpy(path + plen + 1, entry, elen + 1);

        struct stat st;
        if (cached_stat(path, &st) != 0) {
            /* Report the entry with unknown attributes rather than dropping it
             * from the listing: a name this VFS cannot stat is still a name
             * that is there, and skipping it shifts every later index. */
            memset(&st, 0, sizeof(st));
        }

        const size_t name_len = strlen(entry);
        const size_t enc = PAD64(fixed + 2 * name_len);
        if (used + stride > cap || nnames + name_len + 1 > s_dnames_len ||
            (client_cap != 0 && encoded_used + enc > client_cap)) {
            /* Does not fit: carry it to the next call rather than lose it.
             * readdir cannot be rewound by one entry, so dropping it here
             * would silently omit a file from the listing. The client's own
             * limit is checked on the encoded size, which is not the input
             * stride: a long name makes the wire form longer than its slot. */
            snprintf(o->pending, sizeof(o->pending), "%s", entry);
            o->have_pending = true;
            break;
        }
        encoded_used += enc;

        struct smb2_fileidbothdirectoryinformation *fsb =
            (struct smb2_fileidbothdirectoryinformation *)(void *)(s_dents + used);
        dir_entry_fill(fsb, entry, &st, o->dir_index++, s_dnames + nnames);
        nnames += name_len + 1;
        used += stride;

        /* As many entries as fit, now that libsmb2 patch 0005 writes
         * next_entry_offset relative to its entry; one if asked for one. */
        if (req->flags & SMBD_QDIR_SINGLE) {
            break;
        }
    }

    if (used == 0 && first) {
        /* MS-SMB2 3.3.5.18: nothing matching on the first search is "no such
         * file"; NO_MORE_FILES, which the library sends for an empty reply,
         * is for a search that has already returned something. */
        return fail(smb2, SMB2_STATUS_NO_SUCH_FILE, "query_directory '%s'",
                    req->name ? req->name : "");
    }
    if (used == 0) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "query_directory '%s': no more entries",
                   req->name ? req->name : "");
    }
    rep->output_buffer = s_dents;
    rep->output_buffer_length = (uint32_t)used;
    return 0;
}

static int h_query_info(struct smb2_server *srvr, struct smb2_context *smb2,
                        struct smb2_query_info_request *req,
                        struct smb2_query_info_reply *rep)
{
    (void)srvr;
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "query_info type %u class %u",
               (unsigned)req->info_type, (unsigned)req->file_info_class);
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    note_request(c, "query_info type %u class %u",
                 (unsigned)req->info_type, (unsigned)req->file_info_class);

    char path[SMBD_PATH_MAX];
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o != NULL && o->pipe != NULL) {
        /* A pipe has no file behind it; the standard information is all a
         * client asks of one. */
        if (req->info_type != SMB2_0_INFO_FILE ||
            req->file_info_class != SMB2_FILE_STANDARD_INFORMATION) {
            return fail(smb2, SMB2_STATUS_INVALID_PARAMETER,
                        "query_info type %u class %u on a pipe",
                        (unsigned)req->info_type, (unsigned)req->file_info_class);
        }
        struct smb2_file_standard_info *fs = (struct smb2_file_standard_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        fs->allocation_size = 4096;
        fs->number_of_links = 1;
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    if (o != NULL) {
        snprintf(path, sizeof(path), "%s", o->path);
    } else if (id_is_zero(req->file_id) || id_is_compound(req->file_id)) {
        snprintf(path, sizeof(path), "%s", s_path);
    } else {
        return fail(smb2, SMB2_STATUS_FILE_CLOSED,
                    "query_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
    }

    struct stat st;
    if (cached_stat(path, &st) != 0) {
        return fail(smb2, status_of_errno(errno),
                    "query_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
    }

    if (req->info_type == SMB2_0_INFO_FILESYSTEM) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "query_info fs class %u, client max %u",
                   (unsigned)req->file_info_class,
                   (unsigned)req->output_buffer_length);
        espix_fs_info_t root = {0};
        if (espix_fs_stat_root(&root) != ESP_OK || root.total_bytes == 0) {
            root.total_bytes = 0;
            root.used_bytes = 0;
        }
        switch (req->file_info_class) {
        case SMB2_FILE_FS_SIZE_INFORMATION: {
            struct smb2_file_fs_size_info *fs = (struct smb2_file_fs_size_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            const uint64_t unit = 4096;
            fs->total_allocation_units = root.total_bytes / unit;
            fs->available_allocation_units = (root.total_bytes - root.used_bytes) / unit;
            fs->sectors_per_allocation_unit = 8;
            fs->bytes_per_sector = 512;
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        case SMB2_FILE_FS_ATTRIBUTE_INFORMATION: {
            static const char fname[] = "espix";
            struct smb2_file_fs_attribute_info *fs =
                (struct smb2_file_fs_attribute_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            fs->filesystem_attributes = 0;
            fs->maximum_component_name_length = 255;
            fs->filesystem_name_length = (uint32_t)strlen(fname);
            fs->filesystem_name = (const uint8_t *)fname;
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        /*
         * The filesystem classes a volume mount asks for. macOS queries
         * FileFsVolumeInformation and FileFsFullSizeInformation after the tree
         * connect and reports EOPNOTSUPP on the mount if either is refused,
         * which is exactly what "mount error: Operation not supported" was.
         */
        case SMB2_FILE_FS_VOLUME_INFORMATION: {
            static const char label[] = "espix";
            struct smb2_file_fs_volume_info *fs =
                (struct smb2_file_fs_volume_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            fs->volume_serial_number = 0x45535058u;   /* "ESPX" */
            fs->supports_objects = 0;
            fs->volume_label = (const uint8_t *)label;
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        case SMB2_FILE_FS_FULL_SIZE_INFORMATION: {
            struct smb2_file_fs_full_size_info *fs =
                (struct smb2_file_fs_full_size_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            const uint64_t unit = 4096;
            fs->total_allocation_units = root.total_bytes / unit;
            fs->caller_available_allocation_units =
                (root.total_bytes - root.used_bytes) / unit;
            fs->actual_available_allocation_units =
                fs->caller_available_allocation_units;
            fs->sectors_per_allocation_unit = 8;
            fs->bytes_per_sector = 512;
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        case SMB2_FILE_FS_SECTOR_SIZE_INFORMATION: {
            struct smb2_file_fs_sector_size_info *fs =
                (struct smb2_file_fs_sector_size_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            fs->logical_bytes_per_sector = 512;
            fs->physical_bytes_per_sector_for_atomicity = 512;
            fs->physical_bytes_per_sector_for_performance = 4096;
            fs->file_system_effective_physical_bytes_per_sector_for_atomicity = 512;
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        case SMB2_FILE_FS_DEVICE_INFORMATION: {
            struct smb2_file_fs_device_info *fs =
                (struct smb2_file_fs_device_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            fs->device_type = FILE_DEVICE_DISK;
            fs->characteristics = 0;
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        case SMB2_FILE_FS_CONTROL_INFORMATION: {
            struct smb2_file_fs_control_info *fs =
                (struct smb2_file_fs_control_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        case SMB2_FILE_FS_OBJECT_ID_INFORMATION: {
            struct smb2_file_fs_object_id_info *fs =
                (struct smb2_file_fs_object_id_info *)(void *)s_dents;
            memset(fs, 0, sizeof(*fs));
            rep->output_buffer = fs;
            rep->output_buffer_length = sizeof(*fs);
            return 0;
        }
        default:
            espix_klog(ESPIX_KLOG_WARN, TAG, "query_info fs class %u not handled",
                       (unsigned)req->file_info_class);
            return fail(smb2, SMB2_STATUS_INVALID_INFO_CLASS,
                        "query_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
        }
    }

    if (req->info_type == SMB2_0_INFO_SECURITY) {
        /*
         * No NT ACLs are stored, so the honest answer is an empty descriptor:
         * success, with no owner/group/DACL beyond what the mode implies. It
         * must be empty rather than a synthesized descriptor because the
         * library cannot encode one a handler built -- its security case is a
         * passthrough only -- while returning not-supported makes the client
         * abandon whichever operation asked. That is the "operation not
         * supported" Finder showed.
         */
        rep->output_buffer = NULL;
        rep->output_buffer_length = 0;
        return 0;
    }

    if (req->info_type != SMB2_0_INFO_FILE) {
        return fail(smb2, SMB2_STATUS_NOT_SUPPORTED,
                    "query_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
    }

    switch (req->file_info_class) {
    case SMB2_FILE_BASIC_INFORMATION: {
        struct smb2_file_basic_info *fs = (struct smb2_file_basic_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        win_time(ctime_of(&st), &fs->creation_time);
        win_time(atime_of(&st), &fs->last_access_time);
        win_time(st.st_mtime, &fs->last_write_time);
        win_time(ctime_of(&st), &fs->change_time);
        fs->file_attributes = attrs_of(&st);
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_STANDARD_INFORMATION: {
        struct smb2_file_standard_info *fs = (struct smb2_file_standard_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        fs->allocation_size = PAD64((uint64_t)st.st_size);
        fs->end_of_file = (uint64_t)st.st_size;
        fs->number_of_links = 1;
        fs->directory = S_ISDIR(st.st_mode) ? 1 : 0;
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_NETWORK_OPEN_INFORMATION: {
        struct smb2_file_network_open_info *fs =
            (struct smb2_file_network_open_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        win_time(ctime_of(&st), &fs->creation_time);
        win_time(atime_of(&st), &fs->last_access_time);
        win_time(st.st_mtime, &fs->last_write_time);
        win_time(ctime_of(&st), &fs->change_time);
        fs->allocation_size = PAD64((uint64_t)st.st_size);
        fs->end_of_file = (uint64_t)st.st_size;
        fs->file_attributes = attrs_of(&st);
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_ALL_INFORMATION: {
        struct smb2_file_all_info *fs = (struct smb2_file_all_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        win_time(ctime_of(&st), &fs->basic.creation_time);
        win_time(atime_of(&st), &fs->basic.last_access_time);
        win_time(st.st_mtime, &fs->basic.last_write_time);
        win_time(ctime_of(&st), &fs->basic.change_time);
        fs->basic.file_attributes = attrs_of(&st);
        fs->standard.allocation_size = PAD64((uint64_t)st.st_size);
        fs->standard.end_of_file = (uint64_t)st.st_size;
        fs->standard.number_of_links = 1;
        fs->standard.directory = S_ISDIR(st.st_mode) ? 1 : 0;
        fs->index_number = (uint64_t)st.st_ino;
        fs->access_flags = 0x001f01ff;
        fs->name = (const uint8_t *)"";
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_ATTRIBUTE_TAG_INFORMATION: {
        struct smb2_file_attribute_tag_info *fs =
            (struct smb2_file_attribute_tag_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        fs->file_attributes = attrs_of(&st);
        fs->reparse_tag = 0;
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_INTERNAL_INFORMATION: {
        /*
         * The index number. The Linux CIFS client asks for this class every
         * time it opens anything, and smbclient.stat asks for it among its
         * probes, so without it a Linux desktop answers ENOSYS ("Function not
         * implemented") on every open and stat fails. libsmb2 ships no encoder
         * for the class, which is why components/libsmb2/patches adds one; it
         * sends these 8 bytes as they are.
         */
        uint64_t *fs = (uint64_t *)(void *)s_dents;
        *fs = (st.st_ino != 0) ? (uint64_t)st.st_ino : name_hash(path);
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_EA_INFORMATION: {
        /* No extended attributes: EaSize 0, which libsmb2 patch 0006 encodes.
         * Windows asks for it to answer GetFileInformationByHandle. */
        uint32_t *fs = (uint32_t *)(void *)s_dents;
        *fs = 0;
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_STREAM_INFORMATION: {
        if (S_ISDIR(st.st_mode)) {
            return fail(smb2, SMB2_STATUS_INVALID_INFO_CLASS,
                        "query_info streams of a directory");
        }
        /* One unnamed data stream: the file's contents. */
        struct smb2_file_stream_info *fs = (struct smb2_file_stream_info *)(void *)s_dents;
        memset(fs, 0, sizeof(*fs));
        fs->stream_size = (uint64_t)st.st_size;
        fs->stream_allocation_size = PAD64((uint64_t)st.st_size);
        fs->stream_name = "::$DATA";
        rep->output_buffer = fs;
        rep->output_buffer_length = sizeof(*fs);
        return 0;
    }
    case SMB2_FILE_NAME_INFORMATION:
        /* libsmb2 has no encoder for this class: a success went out empty. */
    default:
        /* A client probing for a class it can do without takes this as "use
         * another"; NOT_IMPLEMENTED reads as the whole operation failing. */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "query_info file class %u not handled",
                   (unsigned)req->file_info_class);
        return fail(smb2, SMB2_STATUS_INVALID_INFO_CLASS,
                    "query_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
    }
}

/* Windows FILETIME: 100 ns ticks since 1601-01-01. */
static time_t from_filetime(const uint64_t ft)
{
    return (time_t)((ft / 10000000ULL) - 11644473600ULL);
}

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/*
 * SetInfo. The wire buffers are raw, not decoded by the library, so each
 * class is read here. A client that copies a file sets the times afterwards,
 * a delete sets the disposition and closes, and a rename carries the new name
 * as UTF-16 -- all three are things Finder and a CP do, and refusing any of
 * them is what "operation not supported" was.
 */
static int h_set_info(struct smb2_server *srvr, struct smb2_context *smb2,
                      struct smb2_set_info_request *req)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c == NULL) {
        return -1;
    }
    smbd_open_t *o = open_find_any(c, req->file_id);
    if (o == NULL) {
        return fail(smb2, SMB2_STATUS_FILE_CLOSED,
                    "set_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "set_info type %u class %u len %u path '%s'",
               (unsigned)req->info_type, (unsigned)req->file_info_class,
               (unsigned)req->buffer_length, o->path);
    note_request(c, "set_info type %u class %u",
                 (unsigned)req->info_type, (unsigned)req->file_info_class);

    if (req->info_type != SMB2_0_INFO_FILE) {
        return fail(smb2, SMB2_STATUS_NOT_SUPPORTED,
                    "set_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
    }

    if (req->file_info_class == SMB2_FILE_END_OF_FILE_INFORMATION) {
        if (o->is_dir || o->fd < 0 || req->buffer_length < 8) {
            return fail(smb2, SMB2_STATUS_INVALID_PARAMETER,
                        "set_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
        }
        stat_forget(o->path);
        if (ftruncate(o->fd, (off_t)rd_u64(req->input_data)) != 0) {
            return fail(smb2, status_of_errno(errno),
                        "set_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
        }
        return 0;
    }

    /* Explorer preallocates before a copy. There is no preallocation in the
     * VFS, and the end-of-file set that follows is what sizes the file. */
    if (req->file_info_class == SMB2_FILE_ALLOCATION_INFORMATION) {
        return 0;
    }

    if (req->file_info_class == SMB2_FILE_BASIC_INFORMATION) {
        if (req->buffer_length < 36) {
            return -1;
        }
        const uint8_t *p = (const uint8_t *)req->input_data;
        const uint64_t a = rd_u64(p + 8);    /* last access */
        const uint64_t w = rd_u64(p + 16);   /* last write */
        if (a != 0 || w != 0) {
            /* utime() is the VFS's time entry point; utimes() is not linked
             * into this image, and nfs3.c sets times the same way. */
            struct utimbuf ut;
            ut.actime = (a != 0) ? from_filetime(a) : from_filetime(w);
            ut.modtime = (w != 0) ? from_filetime(w) : from_filetime(a);
            (void)utime(o->path, &ut);
            stat_forget(o->path);
        }
        return 0;
    }

    if (req->file_info_class == SMB2_FILE_DISPOSITION_INFORMATION) {
        if (req->buffer_length < 1) {
            return -1;
        }
        o->delete_on_close = (((const uint8_t *)req->input_data)[0] != 0);
        return 0;
    }

    if (req->file_info_class == SMB2_FILE_RENAME_INFORMATION) {
        /*
         * The SMB2 form of FILE_RENAME_INFORMATION is not the one MS-FSCC
         * sketches first: after replace_if_exists(1) and reserved(7) comes a
         * root_directory(8) -- almost always zero -- and only then
         * file_name_length(4) and the UTF-16 name. Reading the length at 8 is
         * reading root_directory, so every rename looked like an empty name.
         */
        if (req->buffer_length < 20) {
            return -1;
        }
        const uint8_t *p = (const uint8_t *)req->input_data;
        const uint32_t nl = rd_u32(p + 16);
        if (nl == 0 || nl > 2 * (SMBD_PATH_MAX - 1) ||
            (uint64_t)20 + nl > req->buffer_length) {
            return -1;
        }
        char name[SMBD_PATH_MAX];
        size_t j = 0;
        for (size_t i = 0; i + 1 < nl && j + 1 < sizeof(name); i += 2) {
            const uint16_t cu = (uint16_t)p[20 + i] |
                                ((uint16_t)p[20 + i + 1] << 8);
            name[j++] = (cu < 0x80) ? (char)cu : '?';
        }
        name[j] = 0;
        char newpath[SMBD_PATH_MAX];
        if (!resolve_path(name, newpath, sizeof(newpath))) {
            return fail(smb2, SMB2_STATUS_OBJECT_NAME_NOT_FOUND,
                        "set_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
        }

        /*
         * littlefs refuses to rename a file that is open -- "Cannot rename;
         * src ... is open", EBUSY -- and every client renames through an open
         * handle: it opens the source, then sends SetInfo on it. Close it for
         * the rename and reopen at the new path, so the handle the client
         * still holds keeps working.
         */
        char oldpath[SMBD_PATH_MAX];
        snprintf(oldpath, sizeof(oldpath), "%s", o->path);
        const bool was_dir = o->is_dir;
        const int was_fd = o->fd;
        DIR *was_dirp = o->dir;
        o->fd = -1;
        o->dir = NULL;
        o->is_dir = false;
        if (was_dirp != NULL) {
            closedir(was_dirp);
        } else if (was_fd >= 0) {
            close(was_fd);
        }
        stat_forget(NULL);
        if (rename(oldpath, newpath) != 0) {
            const int e = errno;
            espix_klog(ESPIX_KLOG_WARN, TAG, "rename '%s' -> '%s' failed: errno %d",
                       oldpath, newpath, e);
            (void)fail(smb2, status_of_errno(e), "rename");
            if (was_dir) {
                o->dir = opendir(oldpath);
                o->is_dir = (o->dir != NULL);
            } else if (was_fd >= 0) {
                o->fd = open(oldpath, o->flags);
            }
            return -1;
        }
        snprintf(o->path, sizeof(o->path), "%s", newpath);
        if (was_dir) {
            o->dir = opendir(newpath);
            o->is_dir = (o->dir != NULL);
        } else if (was_fd >= 0) {
            o->fd = open(newpath, o->flags);
        }
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "rename '%s' -> '%s'", oldpath, newpath);
        return 0;
    }

    return fail(smb2, SMB2_STATUS_INVALID_INFO_CLASS,
                "set_info type %u class %u",
            (unsigned)req->info_type, (unsigned)req->file_info_class);
}

static int h_destruction(struct smb2_server *srvr, struct smb2_context *smb2)
{
    (void)srvr;
    smbd_conn_t *c = conn_of(smb2);
    if (c != NULL) {
        /*
         * The last request a handler saw is where the client stopped. Session
         * setup and validate-negotiate are answered inside libsmb2, so a close
         * right after "tree connect" with an authenticated session points at
         * those.
         */
        char who[96];
        if (!c->in_session) {
            snprintf(who, sizeof(who), "no session");
        } else {
            snprintf(who, sizeof(who), "'%s' %s", c->user,
                     c->authed ? "authenticated" : "as guest");
        }
        log_quiet(&s_quiet_close, ESPIX_KLOG_INFO, "closed: %s, last request %s",
                  who, c->last[0] ? c->last : "none");
    }
    conn_free(c);
    return 0;
}

static void on_new_client(struct smb2_context *smb2, void *cb_data)
{
    (void)cb_data;
    /* Logged at accept, before any request, so a client that negotiates and
     * then gives up still leaves a trace of having connected. */
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "client connected");
    if (conn_new(smb2) == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "connection table full");
        return;
    }
    /*
     * 2.0.2 up to 3.0.2, never 3.1.1. libsmb2 signs the TREE_CONNECT reply
     * unconditionally when the dialect is 3.1.1 (pdu.c, "SMB2_TREE_CONNECT &&
     * dialect == 0311"), because 3.1.1 folds it into the preauth integrity
     * hash. A guest session has no signing key, so the client is handed a
     * signed reply it cannot verify and drops the connection -- which is
     * exactly what smbprotocol did ("key must be bytes-like"). Guest sessions
     * still exist alongside a credential, so the rule still bites.
     *
     * Not 3.0.2 alone: a Samsung phone offers nothing newer than 2.1 (patch
     * 0007 adds this set). The price of 3.0.x is FSCTL_VALIDATE_NEGOTIATE_INFO,
     * which those clients send signed after their first tree connect and
     * which must be answered signed; libsmb2 patch 0004 is what makes that so.
     */
    smb2_set_version(smb2, SMB2_VERSION_UPTO_0302);
    smb2_register_error_callback(smb2, on_error);
}

static void extra_service(struct smb2_server *server, fd_set *rfds, fd_set *wfds)
{
    (void)rfds; (void)wfds;
    if (s_keep_going && !s_keep_going()) {
        server->stop_requested = 1;
    }
}

/* -------------------------------------------------------------------- run */

static struct smb2_server_request_handlers s_handlers = {
    .destruction_event   = h_destruction,
    .authorize_user      = h_authorize,
    .session_established = h_session,
    .logoff_cmd          = h_logoff,
    .tree_connect_cmd    = h_tree_connect,
    .tree_disconnect_cmd = h_tree_disconnect,
    .create_cmd          = h_create,
    .close_cmd           = h_close,
    .flush_cmd           = h_flush,
    .read_cmd            = h_read,
    .write_cmd           = h_write,
    .lock_cmd            = h_lock,
    .ioctl_cmd           = h_ioctl,
    .cancel_cmd          = h_cancel,
    .echo_cmd            = h_echo,
    .query_directory_cmd = h_query_directory,
    .query_info_cmd      = h_query_info,
    .set_info_cmd        = h_set_info,
};

const char *espix_smbd_share(void)
{
    return s_share;
}

const char *espix_smbd_path(void)
{
    return s_path;
}

esp_err_t espix_smbd_run(bool (*keep_going)(void))
{
    if (!smbd_wanted()) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "disabled by %s", SMBD_CONF_PATH);
        return ESP_OK;
    }

    s_keep_going = keep_going;
    smbd_load_config();

    const size_t scratch = s_max_io;
    const size_t conns_bytes = SMBD_MAX_CONNS * sizeof(smbd_conn_t);

    /* All three in PSRAM. The connection table is the largest of them at a
     * few opens per connection each carrying a path, and none of it is
     * touched often enough to want internal RAM. */
    s_dents = heap_caps_malloc(scratch, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dnames = heap_caps_malloc(scratch, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_conns = heap_caps_malloc(conns_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Optional: without it every stat goes to littlefs, as before. */
    s_stats = heap_caps_calloc(SMBD_STAT_SLOTS, sizeof(*s_stats),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_dents == NULL || s_dnames == NULL || s_conns == NULL) {
        free(s_dents);
        free(s_dnames);
        free(s_conns);
        s_dents = NULL;
        s_dnames = NULL;
        s_conns = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_dents_len = scratch;
    s_dnames_len = scratch;
    memset(s_conns, 0, conns_bytes);

    memset(&s_server, 0, sizeof(s_server));

    s_server.handlers = &s_handlers;
    s_server.port = 445;
    s_server.allow_anonymous = 1;
    /*
     * Signing only when there is a credential to hand the client a session
     * key. Guest stays unsigned, exactly as before, so clients that never
     * asked for signing are unaffected.
     */
    s_server.signing_enabled = (s_user[0] != 0 && s_pass[0] != 0) ? 1 : 0;
    s_server.max_transact_size = s_max_io;
    s_server.max_read_size = s_max_io;
    s_server.max_write_size = s_max_io;
    s_server.extra_service = extra_service;

    const char *host = espix_net_hostname();
    snprintf(s_server.hostname, sizeof(s_server.hostname), "%s",
             (host && host[0]) ? host : "espix");
    snprintf(s_server.domain, sizeof(s_server.domain), "WORKGROUP");

    smbd_srvsvc_config(s_server.hostname, s_share, s_path);

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "serving \\\\%s\\%s over %s, guest%s%s, %u KiB buffers",
               s_server.hostname, s_share, s_path,
               s_server.signing_enabled ? " and user " : "",
               s_server.signing_enabled ? s_user : "",
               (unsigned)(s_max_io / 1024));

    /* smb2_serve_port() returns when stop_requested is set or on a fatal
     * error; a stopped request is a clean stop, not a failure. */
    (void)smb2_serve_port(&s_server, SMBD_MAX_CONNS, on_new_client, NULL);

    if (s_conns != NULL) {
        for (size_t i = 0; i < SMBD_MAX_CONNS; i++) {
            if (s_conns[i].used) {
                conn_free(&s_conns[i]);
            }
        }
    }
    free(s_dents);
    free(s_dnames);
    free(s_conns);
    free(s_stats);
    s_dents = NULL;
    s_dnames = NULL;
    s_conns = NULL;
    s_stats = NULL;
    espix_klog(ESPIX_KLOG_INFO, TAG, "stopped");
    return ESP_OK;
}
