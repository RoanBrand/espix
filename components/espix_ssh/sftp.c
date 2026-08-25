/*
 * SFTP version 3 (draft-ietf-secsh-filexfer-02), server side.
 *
 * This is what `scp` speaks. OpenSSH 9 and later run scp over SFTP by default
 * and keep the old protocol behind -O, so implementing SFTP rather than legacy
 * SCP is what makes a bare `scp` work — and brings `sftp` and graphical clients
 * along with it.
 *
 * Written rather than imported for the same reason the SSH server was: wolfSSH
 * has SFTP but is GPL-or-commercial, and libssh is LGPL, either of which would
 * set the licence of every firmware built on espix. What made writing it small
 * is that SFTP reuses SSH's wire encoding exactly — length-prefixed strings,
 * u8, u32 — so ssh_buf_t and its accessors serve unchanged.
 *
 * Deliberately partial: enough for scp in both directions, for sftp's ls, cd,
 * get, put, mkdir and rm, and no more. Attributes are the honest shape of the
 * filesystem underneath — LittleFS has neither permissions nor timestamps, so
 * SETSTAT succeeds without doing anything rather than failing a transfer over
 * a mode bit that could never be stored.
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "espix_fs.h"
#include "espix_kernel.h"
#include "ssh_priv.h"

#define TAG "sftp"

/* Packet types (draft-ietf-secsh-filexfer-02 §3). */
enum {
    SSH_FXP_INIT     = 1,   SSH_FXP_VERSION  = 2,
    SSH_FXP_OPEN     = 3,   SSH_FXP_CLOSE    = 4,
    SSH_FXP_READ     = 5,   SSH_FXP_WRITE    = 6,
    SSH_FXP_LSTAT    = 7,   SSH_FXP_FSTAT    = 8,
    SSH_FXP_SETSTAT  = 9,   SSH_FXP_FSETSTAT = 10,
    SSH_FXP_OPENDIR  = 11,  SSH_FXP_READDIR  = 12,
    SSH_FXP_REMOVE   = 13,  SSH_FXP_MKDIR    = 14,
    SSH_FXP_RMDIR    = 15,  SSH_FXP_REALPATH = 16,
    SSH_FXP_STAT     = 17,  SSH_FXP_RENAME   = 18,
    SSH_FXP_STATUS   = 101, SSH_FXP_HANDLE   = 102,
    SSH_FXP_DATA     = 103, SSH_FXP_NAME     = 104,
    SSH_FXP_ATTRS    = 105,
};

/* Status codes (§7). */
enum {
    SSH_FX_OK = 0, SSH_FX_EOF = 1, SSH_FX_NO_SUCH_FILE = 2,
    SSH_FX_PERMISSION_DENIED = 3, SSH_FX_FAILURE = 4,
    SSH_FX_OP_UNSUPPORTED = 8,
};

/* Attribute flags (§5). */
#define SSH_FILEXFER_ATTR_SIZE        0x00000001
#define SSH_FILEXFER_ATTR_PERMISSIONS 0x00000004

/* Open flags (§6.3). */
#define SSH_FXF_READ   0x00000001
#define SSH_FXF_WRITE  0x00000002
#define SSH_FXF_APPEND 0x00000004
#define SSH_FXF_CREAT  0x00000008
#define SSH_FXF_TRUNC  0x00000010
#define SSH_FXF_EXCL   0x00000020

/*
 * Concurrent handles. scp uses one; sftp opens a directory and a file at once.
 * Four leaves room without pretending this is a multi-user server.
 */
#define SFTP_HANDLES 4

/*
 * Largest payload we will return for one READ. The client asks for far more —
 * OpenSSH requests 32KB — but every byte lives in the packet buffer alongside
 * the framing, and DIRAM is the scarce resource here. A smaller reply costs
 * round-trips, not correctness.
 */
#define SFTP_READ_MAX 2048

typedef struct {
    bool  used;
    bool  is_dir;
    FILE *file;
    DIR  *dir;
    char  path[ESPIX_PATH_MAX];
} sftp_handle_t;

typedef struct {
    ssh_conn_t   *conn;
    sftp_handle_t handles[SFTP_HANDLES];
    /* Reassembly across CHANNEL_DATA boundaries: one SFTP packet does not have
     * to arrive in one SSH packet, and a write of any size will not. */
    uint8_t       in[SSH_MAX_PACKET];
    size_t        in_len;

    /*
     * Replies are built here, never in conn->out_buf: that is where
     * send_data() assembles the CHANNEL_DATA framing, so a reply written there
     * would be overwritten by the packet meant to carry it.
     */
    uint8_t       out[SSH_MAX_PACKET];
} sftp_t;

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */

static int handle_alloc(sftp_t *s)
{
    for (int i = 0; i < SFTP_HANDLES; i++) {
        if (!s->handles[i].used) {
            memset(&s->handles[i], 0, sizeof(s->handles[i]));
            s->handles[i].used = true;
            return i;
        }
    }
    return -1;
}

static sftp_handle_t *handle_get(sftp_t *s, ssh_buf_t *in)
{
    size_t         len = 0;
    const uint8_t *raw = ssh_get_string(in, &len);

    if (in->bad || raw == NULL || len != sizeof(uint32_t)) {
        return NULL;
    }

    const uint32_t i = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                       ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];

    if (i >= SFTP_HANDLES || !s->handles[i].used) {
        return NULL;
    }
    return &s->handles[i];
}

static void handle_close(sftp_handle_t *h)
{
    if (h->file != NULL) {
        fclose(h->file);
    }
    if (h->dir != NULL) {
        closedir(h->dir);
    }
    memset(h, 0, sizeof(*h));
}

/* ------------------------------------------------------------------ */
/* Replies                                                             */
/* ------------------------------------------------------------------ */

/*
 * Every reply is a length-prefixed SFTP packet inside CHANNEL_DATA. Written
 * raw, never through the channel's cooked path: that turns LF into CR LF for
 * the terminal, which would quietly corrupt every binary transferred.
 */
static esp_err_t sftp_send(sftp_t *s, ssh_buf_t *b)
{
    if (b->bad) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hdr[4];
    hdr[0] = (uint8_t)(b->len >> 24);
    hdr[1] = (uint8_t)(b->len >> 16);
    hdr[2] = (uint8_t)(b->len >> 8);
    hdr[3] = (uint8_t)b->len;

    if (ssh_channel_send_raw(s->conn, hdr, sizeof(hdr)) != ESP_OK) {
        return ESP_FAIL;
    }
    return ssh_channel_send_raw(s->conn, b->buf, b->len);
}

static esp_err_t send_status(sftp_t *s, uint32_t id, uint32_t code,
                             const char *text)
{
    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));

    ssh_put_u8(&b, SSH_FXP_STATUS);
    ssh_put_u32(&b, id);
    ssh_put_u32(&b, code);
    ssh_put_cstr(&b, text);
    ssh_put_cstr(&b, "");           /* language tag */

    return sftp_send(s, &b);
}

/* errno mapped to the closest thing SFTP has to say about it. */
static uint32_t status_for(int err)
{
    switch (err) {
    case 0:      return SSH_FX_OK;
    case ENOENT: return SSH_FX_NO_SUCH_FILE;
    case EACCES:
    case EPERM:  return SSH_FX_PERMISSION_DENIED;
    default:     return SSH_FX_FAILURE;
    }
}

/*
 * Attributes, as far as this filesystem has any. Size and the directory bit
 * are real; the permission bits are a constant that only tells a client which
 * of the two it is looking at, because LittleFS stores neither owner nor mode.
 */
static void put_attrs(ssh_buf_t *b, const struct stat *st)
{
    ssh_put_u32(b, SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS);
    ssh_put_u32(b, 0);                          /* size, high word */
    ssh_put_u32(b, (uint32_t)st->st_size);
    ssh_put_u32(b, S_ISDIR(st->st_mode) ? (S_IFDIR | 0755) : (S_IFREG | 0644));
}

/* Resolve a client path against "/" — SFTP paths are absolute, and a client
 * that sends "." expects the root it was given by REALPATH. */
static bool resolve(const char *in, char *out, size_t len)
{
    return espix_fs_resolve("/", (in != NULL && in[0] != '\0') ? in : "/",
                            out, len) == ESP_OK;
}

/* Copy a client-supplied string out of the packet buffer, which the reply is
 * about to overwrite. */
static bool take_path(ssh_buf_t *in, char *out, size_t len)
{
    size_t         n = 0;
    const uint8_t *p = ssh_get_string(in, &n);

    if (in->bad || p == NULL || n >= len) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t do_realpath(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    char given[ESPIX_PATH_MAX];
    char abs[ESPIX_PATH_MAX];

    if (!take_path(in, given, sizeof(given)) ||
        !resolve(given, abs, sizeof(abs))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_NAME);
    ssh_put_u32(&b, id);
    ssh_put_u32(&b, 1);             /* one name */
    ssh_put_cstr(&b, abs);
    ssh_put_cstr(&b, abs);          /* long name; clients accept the same */
    ssh_put_u32(&b, 0);             /* no attributes */
    return sftp_send(s, &b);
}

static esp_err_t do_stat(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    char given[ESPIX_PATH_MAX];
    char abs[ESPIX_PATH_MAX];
    struct stat st;

    if (!take_path(in, given, sizeof(given)) ||
        !resolve(given, abs, sizeof(abs))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }
    if (stat(abs, &st) != 0) {
        return send_status(s, id, SSH_FX_NO_SUCH_FILE, "no such file");
    }

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_ATTRS);
    ssh_put_u32(&b, id);
    put_attrs(&b, &st);
    return sftp_send(s, &b);
}

static esp_err_t do_fstat(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    sftp_handle_t *h = handle_get(s, in);
    struct stat    st;

    if (h == NULL || stat(h->path, &st) != 0) {
        return send_status(s, id, SSH_FX_FAILURE, "bad handle");
    }

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_ATTRS);
    ssh_put_u32(&b, id);
    put_attrs(&b, &st);
    return sftp_send(s, &b);
}

static esp_err_t send_handle(sftp_t *s, uint32_t id, int index)
{
    uint8_t raw[4] = {
        (uint8_t)(index >> 24), (uint8_t)(index >> 16),
        (uint8_t)(index >> 8),  (uint8_t)index,
    };

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_HANDLE);
    ssh_put_u32(&b, id);
    ssh_put_string(&b, raw, sizeof(raw));
    return sftp_send(s, &b);
}

static esp_err_t do_open(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    char given[ESPIX_PATH_MAX];

    if (!take_path(in, given, sizeof(given))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }
    const uint32_t flags = ssh_get_u32(in);

    const int index = handle_alloc(s);
    if (index < 0) {
        return send_status(s, id, SSH_FX_FAILURE, "too many open files");
    }
    sftp_handle_t *h = &s->handles[index];

    if (!resolve(given, h->path, sizeof(h->path))) {
        handle_close(h);
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }

    /* Only the combinations a transfer actually uses. Append and exclusive
     * create are accepted so a client that asks does not fail outright. */
    const char *mode = "rb";
    if (flags & SSH_FXF_WRITE) {
        mode = (flags & SSH_FXF_APPEND) ? "ab"
             : (flags & (SSH_FXF_CREAT | SSH_FXF_TRUNC)) ? "wb" : "r+b";
    }

    if ((flags & SSH_FXF_EXCL) && access(h->path, F_OK) == 0) {
        handle_close(h);
        return send_status(s, id, SSH_FX_FAILURE, "already exists");
    }

    h->file = fopen(h->path, mode);
    if (h->file == NULL) {
        const uint32_t code = status_for(errno);
        handle_close(h);
        return send_status(s, id, code, "cannot open");
    }
    return send_handle(s, id, index);
}

static esp_err_t do_opendir(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    char given[ESPIX_PATH_MAX];

    if (!take_path(in, given, sizeof(given))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }

    const int index = handle_alloc(s);
    if (index < 0) {
        return send_status(s, id, SSH_FX_FAILURE, "too many open files");
    }
    sftp_handle_t *h = &s->handles[index];
    h->is_dir = true;

    if (!resolve(given, h->path, sizeof(h->path))) {
        handle_close(h);
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }

    h->dir = opendir(h->path);
    if (h->dir == NULL) {
        const uint32_t code = status_for(errno);
        handle_close(h);
        return send_status(s, id, code, "cannot open directory");
    }
    return send_handle(s, id, index);
}

/*
 * One entry per reply rather than the batch a real server sends. It costs
 * round-trips on a long directory, and it keeps every name and its attributes
 * inside one packet buffer without a second-guess about the arithmetic.
 */
static esp_err_t do_readdir(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    sftp_handle_t *h = handle_get(s, in);

    if (h == NULL || h->dir == NULL) {
        return send_status(s, id, SSH_FX_FAILURE, "bad handle");
    }

    const struct dirent *de = readdir(h->dir);
    if (de == NULL) {
        return send_status(s, id, SSH_FX_EOF, "end of directory");
    }

    char full[ESPIX_PATH_MAX];
    struct stat st;
    memset(&st, 0, sizeof(st));

    if (espix_fs_resolve(h->path, de->d_name, full, sizeof(full)) != ESP_OK ||
        stat(full, &st) != 0) {
        st.st_mode = (de->d_type == DT_DIR) ? S_IFDIR : S_IFREG;
    }

    /* The long name is what `sftp`'s ls prints; ls(1)'s shape is what clients
     * expect to parse, so give them that even though the mode is nominal. */
    /* Sized for a full directory entry name, not a path: the two limits are
     * unrelated and dirent's is the larger. */
    char longname[sizeof(de->d_name) + 64];
    snprintf(longname, sizeof(longname), "%s 1 esp esp %8u Jan  1 00:00 %s",
             S_ISDIR(st.st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
             (unsigned)st.st_size, de->d_name);

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_NAME);
    ssh_put_u32(&b, id);
    ssh_put_u32(&b, 1);
    ssh_put_cstr(&b, de->d_name);
    ssh_put_cstr(&b, longname);
    put_attrs(&b, &st);
    return sftp_send(s, &b);
}

static esp_err_t do_read(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    sftp_handle_t *h = handle_get(s, in);

    if (h == NULL || h->file == NULL) {
        return send_status(s, id, SSH_FX_FAILURE, "bad handle");
    }

    ssh_get_u32(in);                            /* offset, high word */
    const uint32_t offset = ssh_get_u32(in);
    uint32_t       want   = ssh_get_u32(in);

    if (in->bad) {
        return send_status(s, id, SSH_FX_FAILURE, "malformed read");
    }
    if (want > SFTP_READ_MAX) {
        want = SFTP_READ_MAX;
    }

    if (fseek(h->file, (long)offset, SEEK_SET) != 0) {
        return send_status(s, id, SSH_FX_FAILURE, "seek failed");
    }

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_DATA);
    ssh_put_u32(&b, id);

    /* Read straight into the packet, after the length word we are about to
     * fill in, so the file's bytes are never copied twice. */
    uint8_t     *data = b.buf + b.len + sizeof(uint32_t);
    const size_t room = b.cap - b.len - sizeof(uint32_t);
    const size_t got  = fread(data, 1, (want < room) ? want : room, h->file);

    if (got == 0) {
        return send_status(s, id, SSH_FX_EOF, "end of file");
    }

    ssh_put_u32(&b, (uint32_t)got);
    b.len += got;
    return sftp_send(s, &b);
}

static esp_err_t do_write(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    sftp_handle_t *h = handle_get(s, in);

    if (h == NULL || h->file == NULL) {
        return send_status(s, id, SSH_FX_FAILURE, "bad handle");
    }

    ssh_get_u32(in);                            /* offset, high word */
    const uint32_t offset = ssh_get_u32(in);

    size_t         len  = 0;
    const uint8_t *data = ssh_get_string(in, &len);

    if (in->bad || data == NULL) {
        return send_status(s, id, SSH_FX_FAILURE, "malformed write");
    }
    if (fseek(h->file, (long)offset, SEEK_SET) != 0) {
        return send_status(s, id, SSH_FX_FAILURE, "seek failed");
    }
    if (len > 0 && fwrite(data, 1, len, h->file) != len) {
        return send_status(s, id, SSH_FX_FAILURE, "write failed");
    }
    return send_status(s, id, SSH_FX_OK, "");
}

static esp_err_t do_close(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    sftp_handle_t *h = handle_get(s, in);

    if (h == NULL) {
        return send_status(s, id, SSH_FX_FAILURE, "bad handle");
    }
    handle_close(h);
    return send_status(s, id, SSH_FX_OK, "");
}

/* remove, mkdir and rmdir differ only in which call they make. */
static esp_err_t do_path_op(sftp_t *s, uint32_t id, ssh_buf_t *in, uint8_t type)
{
    char given[ESPIX_PATH_MAX];
    char abs[ESPIX_PATH_MAX];

    if (!take_path(in, given, sizeof(given)) ||
        !resolve(given, abs, sizeof(abs))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }

    int rc = -1;
    switch (type) {
    case SSH_FXP_REMOVE: rc = unlink(abs);      break;
    case SSH_FXP_MKDIR:  rc = mkdir(abs, 0755); break;
    case SSH_FXP_RMDIR:  rc = rmdir(abs);       break;
    default:             break;
    }

    if (rc != 0) {
        return send_status(s, id, status_for(errno), "operation failed");
    }
    return send_status(s, id, SSH_FX_OK, "");
}

static esp_err_t do_rename(sftp_t *s, uint32_t id, ssh_buf_t *in)
{
    char from[ESPIX_PATH_MAX], to[ESPIX_PATH_MAX];
    char abs_from[ESPIX_PATH_MAX], abs_to[ESPIX_PATH_MAX];

    if (!take_path(in, from, sizeof(from)) ||
        !take_path(in, to, sizeof(to)) ||
        !resolve(from, abs_from, sizeof(abs_from)) ||
        !resolve(to, abs_to, sizeof(abs_to))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }
    if (rename(abs_from, abs_to) != 0) {
        return send_status(s, id, status_for(errno), "rename failed");
    }
    return send_status(s, id, SSH_FX_OK, "");
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t dispatch(sftp_t *s, uint8_t *packet, size_t len)
{
    ssh_buf_t in;
    ssh_buf_read_from(&in, packet, len);

    const uint8_t type = ssh_get_u8(&in);

    if (type == SSH_FXP_INIT) {
        /* The version is all INIT carries that we act on; extensions follow
         * and are ignored, which is allowed and what most servers do. */
        ssh_buf_t b;
        ssh_buf_init(&b, s->out, sizeof(s->out));
        ssh_put_u8(&b, SSH_FXP_VERSION);
        ssh_put_u32(&b, 3);
        return sftp_send(s, &b);
    }

    const uint32_t id = ssh_get_u32(&in);
    if (in.bad) {
        return ESP_ERR_INVALID_SIZE;
    }

    switch (type) {
    case SSH_FXP_REALPATH: return do_realpath(s, id, &in);
    case SSH_FXP_STAT:
    case SSH_FXP_LSTAT:    return do_stat(s, id, &in);
    case SSH_FXP_FSTAT:    return do_fstat(s, id, &in);
    case SSH_FXP_OPEN:     return do_open(s, id, &in);
    case SSH_FXP_OPENDIR:  return do_opendir(s, id, &in);
    case SSH_FXP_READDIR:  return do_readdir(s, id, &in);
    case SSH_FXP_READ:     return do_read(s, id, &in);
    case SSH_FXP_WRITE:    return do_write(s, id, &in);
    case SSH_FXP_CLOSE:    return do_close(s, id, &in);
    case SSH_FXP_RENAME:   return do_rename(s, id, &in);
    case SSH_FXP_REMOVE:
    case SSH_FXP_MKDIR:
    case SSH_FXP_RMDIR:    return do_path_op(s, id, &in, type);

    /*
     * Succeed without doing anything. scp sets permissions and times on every
     * file it uploads; LittleFS can store neither, and failing here would fail
     * the transfer over an attribute that was never going to survive.
     */
    case SSH_FXP_SETSTAT:
    case SSH_FXP_FSETSTAT: return send_status(s, id, SSH_FX_OK, "");

    default:
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "unsupported request %u", type);
        return send_status(s, id, SSH_FX_OP_UNSUPPORTED, "unsupported");
    }
}

esp_err_t espix_sftp_run(ssh_conn_t *c)
{
    /* Two packet buffers plus the handle table: on the heap, so a device that
     * never transfers a file does not carry them. */
    sftp_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "out of memory for an sftp session");
        return ESP_ERR_NO_MEM;
    }
    s->conn = c;

    espix_klog(ESPIX_KLOG_INFO, TAG, "sftp session for %s", c->user);

    for (;;) {
        uint8_t *chunk = NULL;
        size_t   n     = 0;

        if (ssh_channel_recv_raw(c, &chunk, &n) != ESP_OK) {
            break;                      /* client closed, or the link died */
        }
        if (n == 0) {
            continue;
        }
        if (s->in_len + n > sizeof(s->in)) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "oversized packet; dropping session");
            break;
        }
        memcpy(s->in + s->in_len, chunk, n);
        s->in_len += n;

        /*
         * One CHANNEL_DATA is not one SFTP packet: a large write spans several
         * and several small requests can share one. Consume whole packets and
         * keep the remainder.
         */
        bool fatal = false;

        while (!fatal && s->in_len >= 4) {
            const uint32_t plen = ((uint32_t)s->in[0] << 24) |
                                  ((uint32_t)s->in[1] << 16) |
                                  ((uint32_t)s->in[2] << 8) | (uint32_t)s->in[3];

            if (plen == 0 || plen > sizeof(s->in) - 4) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "bad packet length %u",
                           (unsigned)plen);
                s->in_len = 0;
                break;
            }
            if (s->in_len < 4 + plen) {
                break;                  /* wait for the rest */
            }
            if (dispatch(s, s->in + 4, plen) != ESP_OK) {
                fatal = true;
                break;
            }
            memmove(s->in, s->in + 4 + plen, s->in_len - 4 - plen);
            s->in_len -= 4 + plen;
        }

        if (fatal) {
            break;
        }
    }

    for (int i = 0; i < SFTP_HANDLES; i++) {
        if (s->handles[i].used) {
            handle_close(&s->handles[i]);
        }
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "sftp session for %s ended", c->user);
    free(s);
    return ESP_OK;
}
