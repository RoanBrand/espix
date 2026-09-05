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
 * filesystem underneath: mtime and the permission bits are both real and both
 * come straight out of stat(), and SETSTAT applies a mode rather than
 * pretending to. Owner and group are still absent, because no file records one.
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#include "espix_auth.h"
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

/* Attribute flags (§5). UIDGID is never sent, only skipped over: espix records
 * no owner, but a client may set one alongside the permissions it wants. */
#define SSH_FILEXFER_ATTR_SIZE        0x00000001
#define SSH_FILEXFER_ATTR_UIDGID      0x00000002
#define SSH_FILEXFER_ATTR_PERMISSIONS 0x00000004
#define SSH_FILEXFER_ATTR_ACMODTIME   0x00000008

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
 * Largest payload we will return for one READ. The client asks for more still —
 * OpenSSH requests 32KB — but every byte lives in the reply buffer alongside
 * the framing, so this is a straight memory-for-round-trips trade. 8KB rather
 * than the original 2KB because the buffer can now live in PSRAM.
 */
#define SFTP_READ_MAX 8192

/* Reply buffer: a DATA reply is the largest thing we build, at SFTP_READ_MAX
 * plus its type, request id and length. The rest have paths in them and are
 * far smaller. */
#define SFTP_OUT_MAX (SFTP_READ_MAX + 256)

/*
 * Reassembly buffer, which holds only packets that are *not* writes — a write
 * streams straight to the file, so nothing here scales with transfer size.
 *
 * Sized so one arriving channel packet always fits alongside the largest
 * partial packet we would ever retain, which lets espix_sftp_run() copy a whole
 * chunk in one go. That matters for more than tidiness: see the aliasing note
 * on the copy itself.
 */
#define SFTP_IN_MAX (SSH_MAX_PACKET + SSH_CHANNEL_MAX_PACKET)

/*
 * The fixed part of a WRITE: type, request id, handle string, 64-bit offset and
 * the payload length — 25 bytes with the 4-byte handles we hand out. Rounded up
 * for slack; a client sending a longer handle than it was given fails to parse
 * and has its request refused, which is correct either way.
 */
#define SFTP_WRITE_HEAD 32

/*
 * Sanity bound on a single WRITE. SFTP sets no limit and the payload is
 * streamed rather than buffered, so this is not a memory constraint — it guards
 * only against a desynchronised or hostile client announcing a length that
 * would have us writing for hours. OpenSSH's own server caps a message at
 * 256KB.
 */
#define SFTP_WRITE_MAX (256 * 1024 + 1024)

typedef struct {
    bool  used;
    bool  is_dir;
    FILE *file;
    DIR  *dir;
    char  path[ESPIX_PATH_MAX];
} sftp_handle_t;

typedef struct {
    ssh_conn_t   *conn;

    /*
     * Who the client authenticated as. Held for two reasons: the permission
     * check finds it through the task's current session, and resolve() roots
     * relative paths at its cwd so a client lands in its own home.
     */
    espix_session_t *session;

    sftp_handle_t handles[SFTP_HANDLES];

    /* Reassembly across CHANNEL_DATA boundaries: one SFTP packet does not have
     * to arrive in one SSH packet. Writes bypass this entirely. */
    uint8_t       in[SFTP_IN_MAX];
    size_t        in_len;

    /*
     * Replies are built here, never in conn->out_buf: that is where
     * send_data() assembles the CHANNEL_DATA framing, so a reply written there
     * would be overwritten by the packet meant to carry it.
     */
    uint8_t       out[SFTP_OUT_MAX];

    /*
     * A WRITE being streamed to disk.
     *
     * The payload of a write is bounded only by the client's buffer size —
     * OpenSSH sends 32KB by default and -B raises it — so it is written as it
     * arrives instead of being reassembled. That is what makes an upload
     * independent of any buffer here.
     *
     * While w_active, incoming bytes are file contents and must never be
     * parsed as packets. w_file is NULL once the request has been refused or a
     * write has failed: the payload is then discarded rather than abandoned,
     * because the stream has to stay in step to report the error at all.
     */
    bool          w_active;
    FILE         *w_file;
    uint32_t      w_id;
    size_t        w_left;       /* payload bytes still expected */
    const char   *w_error;      /* NULL while the write is still good */
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
 * Attributes. Size, the directory bit, the modification time and now the
 * permission bits are all real -- espix's VFS fills the mode into st_mode, so a
 * client sees the same nine bits the device's own `ls -l` shows, from the same
 * stat() call. Owner and group are still absent, because no file records one.
 *
 * atime is sent because the protocol pairs the two in one flag and a client
 * that asked for times expects both. LittleFS keeps no access time, so it
 * repeats mtime rather than inventing one.
 *
 * A file written before the clock was set carries a 1970 mtime, and that is
 * reported honestly: `sftp ls -l` showing 1970 is the filesystem telling the
 * truth about when it thought that write happened.
 */
static void put_attrs(ssh_buf_t *b, const struct stat *st, mode_t mode)
{
    ssh_put_u32(b, SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS |
                   SSH_FILEXFER_ATTR_ACMODTIME);
    ssh_put_u32(b, 0);                          /* size, high word */
    ssh_put_u32(b, (uint32_t)st->st_size);
    ssh_put_u32(b, (S_ISDIR(st->st_mode) ? S_IFDIR : S_IFREG) |
                   (mode & ESPIX_MODE_BITS));

    /* SFTP v3 times are 32-bit seconds; espix's time_t is 64-bit, so this is
     * the one place the 2038 problem is real. It is the protocol's, not
     * espix's -- v4 widened the field, and nothing here speaks v4. */
    ssh_put_u32(b, (uint32_t)st->st_mtime);     /* atime, see above */
    ssh_put_u32(b, (uint32_t)st->st_mtime);
}

/*
 * The directory a relative client path is measured from, and what REALPATH
 * hands back as the starting point.
 *
 * The account's home, as every other SFTP server does. It used to be "/", which
 * was merely unusual until the permission check started applying here: / is
 * root-owned and 0755, so `scp file host:` -- no remote path, the commonest
 * invocation there is -- would have begun failing for everyone. A session whose
 * home is missing already falls back to /, which is right for an account made
 * without one.
 */
static const char *base_dir(const sftp_t *s)
{
    if (s->session != NULL && s->session->cwd[0] != '\0') {
        return s->session->cwd;
    }
    return "/";
}

/* Resolve a client path against the base directory. Absolute paths ignore it,
 * which is what makes an absolute SFTP path mean what it says. */
static bool resolve(const sftp_t *s, const char *in, char *out, size_t len)
{
    return espix_fs_resolve(base_dir(s),
                            (in != NULL && in[0] != '\0') ? in : ".",
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
        !resolve(s, given, abs, sizeof(abs))) {
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
        !resolve(s, given, abs, sizeof(abs))) {
        return send_status(s, id, SSH_FX_FAILURE, "bad path");
    }
    if (stat(abs, &st) != 0) {
        return send_status(s, id, SSH_FX_NO_SUCH_FILE, "no such file");
    }

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_ATTRS);
    ssh_put_u32(&b, id);
    put_attrs(&b, &st, st.st_mode);
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
    put_attrs(&b, &st, st.st_mode);
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

    if (!resolve(s, given, h->path, sizeof(h->path))) {
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

    if (!resolve(s, given, h->path, sizeof(h->path))) {
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
/* The name for `id`, or the number itself when nothing claims it. `is_group`
 * picks the namespace: a gid is not a uid, however alike they look here. */
static void id_name(uint16_t id, bool is_group, char *out, size_t len)
{
    const char *name = is_group ? espix_auth_group_name(id)
                                : espix_auth_name_for_uid(id);
    if (name != NULL) {
        strlcpy(out, name, len);
    } else {
        snprintf(out, len, "%u", (unsigned)id);
    }
}

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

    bool have_path = true;

    if (espix_fs_resolve(h->path, de->d_name, full, sizeof(full)) != ESP_OK ||
        stat(full, &st) != 0) {
        st.st_mode = (de->d_type == DT_DIR) ? S_IFDIR : S_IFREG;
        have_path  = false;
    }

    /* Without a resolvable path there is nothing to ask about the mode, so fall
     * back to the same defaults the rule would have produced for a file it
     * could not read. */
    const mode_t mode = have_path ? st.st_mode
                                  : (S_ISDIR(st.st_mode) ? 0755 : 0644);

    /*
     * The long name is what `sftp`'s ls prints; ls(1)'s shape is what clients
     * expect to parse, so give them that.
     *
     * The date has to be built into this string, not just into the attributes
     * below: OpenSSH's `ls -l` renders the longname verbatim and never looks at
     * the attrs, so a correct mtime in put_attrs() and a hardcoded one here
     * shows the client the hardcoded one. Which is what it did -- espix's own
     * `ls -l` said "Aug 31 08:21" while sftp said "Jan  1 00:00" for the same
     * file.
     */
    char when[16];
    if (st.st_mtime > 0) {
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(when, sizeof(when), "%b %e %H:%M", &tm);
    } else {
        /* No mtime attribute at all, which is every file in the flashed image.
         * ls(1) has no spelling for that, so use the epoch it would show. */
        strlcpy(when, "Jan  1  1970", sizeof(when));
    }

    /*
     * Sized for a full directory entry name, not a path: the two limits are
     * unrelated and dirent's is the larger.
     *
     * The mode string is built from the real mode by the same function espix's
     * own `ls -l` uses, so the two cannot drift apart. It used to be a pair of
     * constants here, which meant an executable uploaded over scp showed as
     * -rw-r--r-- in the client and -rwxr-xr-x on the device.
     */
    char perms[11];
    espix_fs_mode_str(mode, S_ISDIR(st.st_mode), perms, sizeof(perms));

    /*
     * The owner, likewise, rather than the "esp esp" that used to be written
     * here whatever the file said. A uid with no account prints as a number,
     * which is what every other ls does and is more use than a wrong name.
     *
     * Copied rather than pointed at: espix_auth_name_for_uid() answers out of a
     * single-entry cache, so asking it about the group would move the ground
     * under a pointer still held for the owner.
     */
    uint16_t uid = 0;
    uint16_t gid = 0;
    if (have_path) {
        espix_fs_owner(full, &st, &uid, &gid);
    }

    char owner[ESPIX_USER_MAX];
    char group[ESPIX_USER_MAX];
    id_name(uid, false, owner, sizeof(owner));
    id_name(gid, true,  group, sizeof(group));

    char longname[sizeof(de->d_name) + 96];
    snprintf(longname, sizeof(longname), "%s 1 %s %s %8u %s %s",
             perms, owner, group, (unsigned)st.st_size, when, de->d_name);

    ssh_buf_t b;
    ssh_buf_init(&b, s->out, sizeof(s->out));
    ssh_put_u8(&b, SSH_FXP_NAME);
    ssh_put_u32(&b, id);
    ssh_put_u32(&b, 1);
    ssh_put_cstr(&b, de->d_name);
    ssh_put_cstr(&b, longname);
    put_attrs(&b, &st, mode);
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

/* ------------------------------------------------------------------ */
/* Writes, which stream rather than reassemble                         */
/* ------------------------------------------------------------------ */

/*
 * Parse a WRITE header and enter streaming mode. `pkt` points at the type byte;
 * `plen` is the packet's own length word, which is what governs framing. The
 * request's payload-length field is compared against it but never used to
 * decide how many bytes to consume — a client whose two lengths disagreed would
 * otherwise leave us reading the next packet from the wrong offset.
 *
 * Returns the number of header bytes consumed; everything after them, to
 * `plen`, is payload. A request that cannot be honoured still streams, with the
 * payload discarded, so the stream stays in step and the error reaches the
 * client instead of the session dying.
 */
static size_t write_begin(sftp_t *s, uint8_t *pkt, size_t avail, size_t plen)
{
    /* Bounded by plen as well as by what has arrived: reading past the packet
     * would consume the head of the one behind it. */
    ssh_buf_t in;
    ssh_buf_read_from(&in, pkt, (avail < plen) ? avail : plen);

    ssh_get_u8(&in);                            /* type */
    const uint32_t id = ssh_get_u32(&in);
    sftp_handle_t *h  = handle_get(s, &in);

    ssh_get_u32(&in);                           /* offset, high word */
    const uint32_t offset = ssh_get_u32(&in);
    const uint32_t len    = ssh_get_u32(&in);

    s->w_active = true;
    s->w_id     = id;
    s->w_file   = NULL;
    s->w_error  = NULL;

    if (in.bad) {
        /* Nothing after the type byte parsed, so only plen says where this
         * packet ends. Skip all of it. */
        s->w_left  = plen - 1;
        s->w_error = "malformed write";
        return 1;
    }

    s->w_left = plen - in.pos;

    if (len != s->w_left) {
        s->w_error = "inconsistent write length";
    } else if (h == NULL || h->file == NULL) {
        s->w_error = "bad handle";
    } else if (fseek(h->file, (long)offset, SEEK_SET) != 0) {
        s->w_error = "seek failed";
    } else {
        s->w_file = h->file;
    }

    return in.pos;
}

/* Hand payload to the file. Returns how much of `data` belonged to this write;
 * anything left over is the next packet. */
static size_t write_feed(sftp_t *s, const uint8_t *data, size_t len)
{
    const size_t take = (len < s->w_left) ? len : s->w_left;

    if (s->w_file != NULL && take > 0 &&
        fwrite(data, 1, take, s->w_file) != take) {
        s->w_file  = NULL;              /* keep draining, but stop writing */
        s->w_error = "write failed";
    }
    s->w_left -= take;
    return take;
}

static esp_err_t write_finish(sftp_t *s)
{
    const uint32_t id  = s->w_id;
    const char    *err = s->w_error;

    s->w_active = false;
    s->w_file   = NULL;
    s->w_error  = NULL;

    if (err != NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "write failed: %s", err);
        return send_status(s, id, SSH_FX_FAILURE, err);
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

/*
 * SETSTAT and FSETSTAT: apply what espix can store, ignore the rest.
 *
 * Only the permission bits are acted on. Size would mean truncating, times
 * would mean utime(), and neither is what a client sends these for on this
 * device -- but a client that sends them alongside permissions must still find
 * its permissions applied, so the fields before PERMISSIONS are read and
 * skipped rather than causing a refusal.
 *
 * Two things are deliberately not errors. Setting the mode of a directory does
 * nothing, because the rule already gives every directory 0755 and there is no
 * traversal check to make it mean something. And setuid, setgid and sticky are
 * masked off rather than rejected: SFTP has no partial-success status, so
 * failing the request would fail the whole `scp -p` over a bit the client
 * probably copied from the local file without meaning anything by it. The
 * shell's own chmod refuses them out loud instead, where there is a person to
 * tell.
 *
 * The mask is ESPIX_PERM_BITS and not ESPIX_MODE_BITS, and that distinction is
 * now load-bearing rather than tidy. espix acts on setuid, and this code runs
 * on the connection task -- neither a process nor a session -- so
 * espix_fs_access_check() treats it as espix itself and does not gate it.
 * Passing the whole mode through would let anyone with an SFTP login set setuid
 * on a root-owned binary and then run it. See docs/KNOWN-ISSUES.md: SFTP being
 * unchecked is a wider gap than this one line, and this line is what stops that
 * gap from being an escalation.
 */
static esp_err_t do_setstat(sftp_t *s, uint32_t id, ssh_buf_t *in, uint8_t type)
{
    char        abs[ESPIX_PATH_MAX];
    const char *path = abs;

    if (type == SSH_FXP_FSETSTAT) {
        const sftp_handle_t *h = handle_get(s, in);
        if (h == NULL) {
            return send_status(s, id, SSH_FX_FAILURE, "bad handle");
        }
        path = h->path;
    } else {
        char given[ESPIX_PATH_MAX];
        if (!take_path(in, given, sizeof(given)) ||
            !resolve(s, given, abs, sizeof(abs))) {
            return send_status(s, id, SSH_FX_FAILURE, "bad path");
        }
    }

    const uint32_t flags = ssh_get_u32(in);

    /* Fields arrive in flag order, so everything before PERMISSIONS has to be
     * consumed to find it. */
    if (flags & SSH_FILEXFER_ATTR_SIZE) {
        ssh_get_u32(in);
        ssh_get_u32(in);
    }
    if (flags & SSH_FILEXFER_ATTR_UIDGID) {
        ssh_get_u32(in);
        ssh_get_u32(in);
    }

    if ((flags & SSH_FILEXFER_ATTR_PERMISSIONS) == 0) {
        return send_status(s, id, SSH_FX_OK, "");
    }

    const uint32_t perms = ssh_get_u32(in);
    if (in->bad) {
        return send_status(s, id, SSH_FX_FAILURE, "truncated attributes");
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return send_status(s, id, status_for(errno), "no such file");
    }
    if (S_ISDIR(st.st_mode)) {
        return send_status(s, id, SSH_FX_OK, "");
    }

    const esp_err_t err = espix_fs_chmod(path, (mode_t)perms & ESPIX_PERM_BITS);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "chmod %s failed: %s",
                   path, esp_err_to_name(err));

        /* espix_fs_chmod() answers in esp_err_t rather than errno, so this is
         * the one place status_for() cannot be used. Only the owner may change
         * a mode, and a client refused deserves to be told which of the two
         * things went wrong. */
        return (err == ESP_ERR_NOT_ALLOWED)
                   ? send_status(s, id, SSH_FX_PERMISSION_DENIED,
                                 "not the owner")
                   : send_status(s, id, SSH_FX_FAILURE,
                                 "cannot set permissions");
    }
    return send_status(s, id, SSH_FX_OK, "");
}

/* remove, mkdir and rmdir differ only in which call they make. */
static esp_err_t do_path_op(sftp_t *s, uint32_t id, ssh_buf_t *in, uint8_t type)
{
    char given[ESPIX_PATH_MAX];
    char abs[ESPIX_PATH_MAX];

    if (!take_path(in, given, sizeof(given)) ||
        !resolve(s, given, abs, sizeof(abs))) {
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
        !resolve(s, from, abs_from, sizeof(abs_from)) ||
        !resolve(s, to, abs_to, sizeof(abs_to))) {
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

/* Every request except WRITE, which pump() intercepts before it gets here
 * because its payload is streamed rather than reassembled. */
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
    case SSH_FXP_CLOSE:    return do_close(s, id, &in);
    case SSH_FXP_RENAME:   return do_rename(s, id, &in);
    case SSH_FXP_REMOVE:
    case SSH_FXP_MKDIR:
    case SSH_FXP_RMDIR:    return do_path_op(s, id, &in, type);

    case SSH_FXP_SETSTAT:
    case SSH_FXP_FSETSTAT: return do_setstat(s, id, &in, type);

    default:
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "unsupported request %u", type);
        return send_status(s, id, SSH_FX_OP_UNSUPPORTED, "unsupported");
    }
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

/* Drop `n` consumed bytes off the front of the reassembly buffer. */
static void consume(sftp_t *s, size_t n)
{
    s->in_len -= n;
    memmove(s->in, s->in + n, s->in_len);
}

/*
 * Consume everything the reassembly buffer now holds: payload for a write in
 * progress, then whole packets, leaving any partial one for the next chunk.
 */
static esp_err_t pump(sftp_t *s)
{
    for (;;) {
        /* Bytes belonging to a write are file contents. Parsing them as
         * packets is how a large upload used to derail the session. */
        if (s->w_active) {
            consume(s, write_feed(s, s->in, s->in_len));
            if (s->w_left > 0) {
                return ESP_OK;              /* the rest is still in flight */
            }
            if (write_finish(s) != ESP_OK) {
                return ESP_FAIL;
            }
            continue;
        }

        if (s->in_len < 4) {
            return ESP_OK;
        }

        const uint32_t plen = ((uint32_t)s->in[0] << 24) |
                              ((uint32_t)s->in[1] << 16) |
                              ((uint32_t)s->in[2] << 8) | (uint32_t)s->in[3];

        if (plen == 0) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "zero-length packet");
            return ESP_FAIL;
        }
        if (s->in_len < 5) {
            return ESP_OK;                  /* need the type byte to route it */
        }

        if (s->in[4] == SSH_FXP_WRITE) {
            if (plen > SFTP_WRITE_MAX) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "write of %u bytes refused",
                           (unsigned)plen);
                return ESP_FAIL;
            }
            /* Only the fixed header has to be here; the payload follows it
             * straight to the file. */
            const size_t head = (plen < SFTP_WRITE_HEAD) ? plen
                                                         : SFTP_WRITE_HEAD;
            if (s->in_len < 4 + head) {
                return ESP_OK;
            }
            consume(s, 4 + write_begin(s, s->in + 4, s->in_len - 4, plen));
            continue;                       /* the branch above drains it */
        }

        /*
         * Every other request is small — paths and attributes — so a length
         * this buffer cannot hold means the stream has desynchronised. There is
         * no recovering from that: resyncing on a length word found in the
         * middle of someone's data is what filled the log with "bad packet
         * length" instead of failing the transfer.
         */
        if (plen > SSH_MAX_PACKET - 4) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "bad packet length %u",
                       (unsigned)plen);
            return ESP_FAIL;
        }
        if (s->in_len < 4 + plen) {
            return ESP_OK;                  /* wait for the rest */
        }
        if (dispatch(s, s->in + 4, plen) != ESP_OK) {
            return ESP_FAIL;
        }
        consume(s, 4 + plen);
    }
}

/*
 * PSRAM when the board has it: this is the largest allocation the SSH server
 * makes, it lives only for the duration of a transfer, and nothing in it is a
 * DMA target — recv() memcpy's out of lwIP's own buffers. Falls back to
 * internal RAM, so a board without PSRAM behaves the same, only tighter.
 */
static sftp_t *sftp_alloc(void)
{
#if CONFIG_ESPIX_SSH_SFTP_IN_PSRAM
    sftp_t *s = heap_caps_calloc(1, sizeof(sftp_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s != NULL) {
        return s;
    }
#endif
    return calloc(1, sizeof(sftp_t));
}

esp_err_t espix_sftp_run(ssh_conn_t *c, espix_session_t *session)
{
    /* On the heap, so a device that never transfers a file does not carry it. */
    sftp_t *s = sftp_alloc();
    if (s == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "out of memory for an sftp session");
        return ESP_ERR_NO_MEM;
    }
    s->conn    = c;
    s->session = session;

    /*
     * Make it this task's current session, which is how espix_fs_access_check()
     * finds a caller. Without it every file operation below runs on a task that
     * is neither a process nor a session, which the check reads as espix itself
     * and allows -- so an authenticated client could fetch files its own shell
     * login is refused.
     *
     * Cleared before returning, without exception. The connection task lives on
     * after this call and `session` is a frame in its caller; leaving the
     * pointer behind is the same use-after-free that used to reboot the board
     * from the exec path.
     */
    espix_shell_set_current(session);

    espix_klog(ESPIX_KLOG_INFO, TAG, "sftp session for %s at %s", c->user,
               base_dir(s));

    for (;;) {
        uint8_t *chunk = NULL;
        size_t   n     = 0;

        if (ssh_channel_recv_raw(c, &chunk, &n) != ESP_OK) {
            break;                      /* client closed, or the link died */
        }
        if (n == 0) {
            continue;
        }

        /*
         * Copy the whole chunk before touching any of it, and never read
         * `chunk` again below.
         *
         * It points into the channel's own receive buffer, and pump() sends
         * replies: a send that runs out of peer window pumps the channel to
         * collect the adjustment, which overwrites exactly that buffer. Holding
         * the pointer across a reply would be reading whatever arrived next.
         * SFTP_IN_MAX guarantees the copy fits in one go, which is what lets
         * this be a single statement rather than a loop that has to re-read it.
         */
        if (s->in_len + n > sizeof(s->in)) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "oversized packet; dropping session");
            break;
        }
        memcpy(s->in + s->in_len, chunk, n);
        s->in_len += n;

        if (pump(s) != ESP_OK) {
            break;
        }
    }

    for (int i = 0; i < SFTP_HANDLES; i++) {
        if (s->handles[i].used) {
            handle_close(&s->handles[i]);
        }
    }

    espix_shell_set_current(NULL);

    espix_klog(ESPIX_KLOG_INFO, TAG, "sftp session for %s ended", c->user);
    free(s);
    return ESP_OK;
}
