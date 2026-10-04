/*
 * NFS version 3, read-only: LOOKUP, GETATTR, ACCESS, READ, READDIR, READDIRPLUS,
 * READLINK, FSSTAT, FSINFO, PATHCONF. Everything that would write returns
 * NFS3ERR_ROFS rather than half-working, which is what the export says it is.
 *
 * File handles do not carry the path: a 64-byte handle cannot hold one, and a
 * path changes when a directory is renamed. They name a slot in a small table
 * of paths instead, and a slot that is reused leaves the client with a stale
 * handle it knows how to recover from.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "espix_kernel.h"

#include "nfs3.h"
#include "nfsd_internal.h"

#define TAG "espix:nfs3"

/* nfsstat3 */
#define NFS3_OK          0
#define NFS3ERR_PERM     1
#define NFS3ERR_NOENT    2
#define NFS3ERR_IO       5
#define NFS3ERR_ACCES    13
#define NFS3ERR_EXIST    17
#define NFS3ERR_NOTDIR   20
#define NFS3ERR_ISDIR    21
#define NFS3ERR_INVAL    22
#define NFS3ERR_NOSPC    28
#define NFS3ERR_ROFS     30
#define NFS3ERR_NAMETOOLONG 63
#define NFS3ERR_NOTEMPTY 66
#define NFS3ERR_STALE    70
#define NFS3ERR_BADHANDLE 10001
#define NFS3ERR_NOTSUPP  10004
#define NFS3ERR_SERVERFAULT 10006

#define FH_LEN     8
#define PATH_SLOTS 128
#define PATH_CAP   256

static struct {
    uint16_t id;
    char     path[PATH_CAP];
} s_slots[PATH_SLOTS];
static uint16_t s_next_id = 1;
static int      s_cursor;

static uint32_t nfserr(int err)
{
    switch (err) {
    case 0:          return NFS3_OK;
    case EPERM:      return NFS3ERR_PERM;
    case ENOENT:     return NFS3ERR_NOENT;
    case EACCES:     return NFS3ERR_ACCES;
    case EEXIST:     return NFS3ERR_EXIST;
    case ENOTDIR:    return NFS3ERR_NOTDIR;
    case EISDIR:     return NFS3ERR_ISDIR;
    case EINVAL:     return NFS3ERR_INVAL;
    case ENOSPC:     return NFS3ERR_NOSPC;
    case EROFS:      return NFS3ERR_ROFS;
    case ENAMETOOLONG: return NFS3ERR_NAMETOOLONG;
    case ENOTEMPTY:  return NFS3ERR_NOTEMPTY;
    default:         return NFS3ERR_IO;
    }
}

/* ------------------------------------------------------------- handles --- */

static bool fh_resolve(const uint8_t *fh, size_t len, int *exp,
                       char *path, size_t cap)
{
    if (len != FH_LEN || fh[0] != 'E' || fh[1] != 'S' ||
        fh[2] != 'P' || fh[3] != 'X') {
        return false;
    }

    const int      e  = fh[4];
    const uint16_t id = (uint16_t)(fh[5] | (fh[6] << 8));
    if (e < 0 || e >= nfsd_exports_count()) {
        return false;
    }
    for (int i = 0; i < PATH_SLOTS; i++) {
        if (s_slots[i].id == id) {
            *exp = e;
            strlcpy(path, s_slots[i].path, cap);
            return true;
        }
    }
    return false;
}

static bool fh_make(int exp, const char *path, uint8_t *fh, size_t *len)
{
    int slot = -1;

    for (int i = 0; i < PATH_SLOTS; i++) {
        if (s_slots[i].id != 0 && strcmp(s_slots[i].path, path) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = s_cursor++ % PATH_SLOTS;
        s_slots[slot].id = s_next_id++;
        if (s_next_id == 0) {
            s_next_id = 1;
        }
        strlcpy(s_slots[slot].path, path, PATH_CAP);
    }

    fh[0] = 'E'; fh[1] = 'S'; fh[2] = 'P'; fh[3] = 'X';
    fh[4] = (uint8_t)exp;
    fh[5] = (uint8_t)(s_slots[slot].id & 0xFF);
    fh[6] = (uint8_t)(s_slots[slot].id >> 8);
    fh[7] = 0;
    *len = FH_LEN;
    return true;
}

/* ------------------------------------------------------------ the wire --- */

static bool get_fh(rpc_call_t *c, uint8_t *fh, size_t *len)
{
    uint32_t n;

    if (!rpc_get_u32(c, &n) || n > 64) {
        return false;
    }
    *len = n;
    return rpc_get_opaque(c, fh, n);
}

static void put_fh(xdrw_t *w, const uint8_t *fh, size_t len)
{
    xdrw_u32(w, (uint32_t)len);
    xdrw_opaque(w, fh, len);
}

static void put_time(xdrw_t *w, long secs)
{
    xdrw_u32(w, (uint32_t)secs);
    xdrw_u32(w, 0);
}

static uint32_t ftype(mode_t m)
{
    if (S_ISREG(m))  return 1;
    if (S_ISDIR(m))  return 2;
    if (S_ISBLK(m))  return 3;
    if (S_ISCHR(m))  return 4;
    if (S_ISLNK(m))  return 5;
    if (S_ISSOCK(m)) return 6;
    return 7;
}

static void put_fattr(xdrw_t *w, int exp, const struct stat *st)
{
    xdrw_u32(w, ftype(st->st_mode));
    xdrw_u32(w, (uint32_t)(st->st_mode & 07777));
    xdrw_u32(w, (uint32_t)(st->st_nlink != 0 ? st->st_nlink : 1));
    xdrw_u32(w, (uint32_t)st->st_uid);
    xdrw_u32(w, (uint32_t)st->st_gid);
    xdrw_u64(w, (uint64_t)st->st_size);
    xdrw_u64(w, (uint64_t)((st->st_size + 511) / 512) * 512);
    xdrw_u32(w, 0);                     /* rdev: major, minor */
    xdrw_u32(w, 0);
    xdrw_u64(w, (uint64_t)(exp + 1));   /* fsid */
    xdrw_u64(w, (uint64_t)(st->st_ino != 0 ? st->st_ino : 1));
    put_time(w, st->st_mtime);
    put_time(w, st->st_mtime);          /* ctime: the VFS gives no separate one */
    put_time(w, st->st_mtime);
}

/* post_op_attr: true and the attributes, or false. */
static void put_post_attr(xdrw_t *w, bool have, int exp, const struct stat *st)
{
    xdrw_bool(w, have);
    if (have) {
        put_fattr(w, exp, st);
    }
}

static bool path_of(int exp, const char *rel, char *out, size_t cap,
                    const char **why)
{
    const nfs_export_t *e = nfsd_export(exp);
    if (e == NULL) {
        *why = "no export";
        return false;
    }
    if (rel == NULL || rel[0] == 0) {
        strlcpy(out, e->path, cap);
    } else {
        snprintf(out, cap, "%s%s", e->path, rel);
    }
    return true;
}

/* One component, so a client cannot climb out of the export with ../ */
static bool name_ok(const char *name)
{
    if (name == NULL || name[0] == 0 || strlen(name) > 255) {
        return false;
    }
    return strchr(name, '/') == NULL && strcmp(name, ".") != 0 &&
           strcmp(name, "..") != 0;
}

/* ---------------------------------------------------------- procedures --- */

static size_t proc_getattr(rpc_call_t *c, xdrw_t *w)
{
    uint8_t fh[64];
    size_t  fhlen = 0;
    char    path[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }
    xdrw_u32(w, NFS3_OK);
    put_fattr(w, exp, &st);
    return w->len;
}

static size_t proc_lookup(rpc_call_t *c, xdrw_t *w)
{
    uint8_t fh[64];
    size_t  fhlen = 0;
    char    dir[PATH_CAP], name[256], path[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_string(c, name, sizeof(name))) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, dir, sizeof(dir)) || !name_ok(name)) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }
    strlcpy(path, dir, sizeof(path));
    strlcat(path, "/", sizeof(path));
    strlcat(path, name, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }

    uint8_t child[FH_LEN];
    size_t  clen = 0;
    if (!fh_make(exp, path, child, &clen)) {
        xdrw_u32(w, NFS3ERR_SERVERFAULT);
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_fh(w, child, clen);
    put_post_attr(w, true, exp, &st);

    struct stat dst;
    put_post_attr(w, stat(dir, &dst) == 0, exp, &dst);
    return w->len;
}

static size_t proc_access(rpc_call_t *c, xdrw_t *w)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    uint32_t asked = 0;
    char     path[PATH_CAP];
    int      exp = 0;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_u32(c, &asked)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }

    /* READ and LOOKUP on anything; the rest only if the bits allow it. */
    const uint32_t READ = 0x1, LOOKUP = 0x2, MODIFY = 0x4, EXTEND = 0x8,
                   DELETE = 0x10, EXECUTE = 0x20;
    uint32_t granted = asked & (READ | LOOKUP);
    const nfs_export_t *e = nfsd_export(exp);
    if (e != NULL && !e->ro) {
        if (st.st_mode & S_IWUSR) granted |= asked & (MODIFY | EXTEND | DELETE);
        if (st.st_mode & S_IXUSR) granted |= asked & EXECUTE;
    }
    if (S_ISDIR(st.st_mode)) {
        granted |= asked & (LOOKUP | READ);
    }

    xdrw_u32(w, NFS3_OK);
    put_post_attr(w, true, exp, &st);
    xdrw_u32(w, granted);
    return w->len;
}

static size_t proc_readlink(rpc_call_t *c, xdrw_t *w)
{
    uint8_t fh[64];
    size_t  fhlen = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    /* espix's VFS has no symlink to read, and says so rather than pretending
     * the path is not there. */
    xdrw_u32(w, NFS3ERR_NOTSUPP);
    return w->len;
}

static size_t proc_read(rpc_call_t *c, xdrw_t *w, uint8_t *io, size_t iocap)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    uint64_t off = 0;
    uint32_t want = 0;
    char     path[PATH_CAP];
    int      exp = 0;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_u64(c, &off) ||
        !rpc_get_u32(c, &want)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }
    if (off != 0 && lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        xdrw_u32(w, NFS3ERR_INVAL);
        return w->len;
    }

    const size_t room = (want < iocap) ? want : iocap;
    const int    got  = read(fd, io, room);
    struct stat  st;
    const bool   have = (fstat(fd, &st) == 0);
    close(fd);

    if (got < 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_post_attr(w, have, exp, &st);
    xdrw_u32(w, (uint32_t)got);
    xdrw_bool(w, (size_t)got < want);
    xdrw_u32(w, (uint32_t)got);
    xdrw_opaque(w, io, (size_t)got);
    return w->len;
}

/*
 * A directory is sent one page at a time, with the cookie being how many names
 * have already gone. That costs a re-read of the directory per page and needs
 * no seekdir, which the VFS may not have.
 */
static size_t proc_readdir(rpc_call_t *c, xdrw_t *w, bool plus)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    uint64_t cookie = 0;
    uint32_t verf = 0, dircount = 0, maxcount = 0;
    char     path[PATH_CAP];
    int      exp = 0;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_u64(c, &cookie) ||
        !rpc_get_u32(c, &verf) || !rpc_get_u32(c, &verf)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    if (plus) {
        if (!rpc_get_u32(c, &dircount) || !rpc_get_u32(c, &maxcount)) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
        if (maxcount == 0 || maxcount > NFSD_BUFCAP - 512) {
            maxcount = NFSD_BUFCAP - 512;
        }
    } else {
        if (!rpc_get_u32(c, &maxcount)) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
        dircount = maxcount;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    DIR *d = opendir(path);
    if (d == NULL) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    struct stat dst;
    put_post_attr(w, stat(path, &dst) == 0, exp, &dst);
    xdrw_u32(w, 0);                     /* cookie verifier */
    xdrw_u32(w, 0);

    const size_t reply_at = w->len;     /* where the entry list begins */
    uint64_t     seen     = 0;
    bool         wrote    = false;
    bool         eof      = true;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        seen++;
        if (seen <= cookie) {
            continue;                   /* already sent, in a previous page */
        }

        /* Budget: each entry costs roughly this much. */
        if (wrote && (w->len - reply_at) + 256 > maxcount) {
            eof = false;
            break;
        }

        char child[PATH_CAP];
        strlcpy(child, path, sizeof(child));
        strlcat(child, "/", sizeof(child));
        strlcat(child, de->d_name, sizeof(child));
        struct stat st;
        const bool  have = (stat(child, &st) == 0);

        xdrw_bool(w, true);             /* this entry exists */
        xdrw_u64(w, (uint64_t)(have && st.st_ino != 0 ? st.st_ino : seen));
        xdrw_string(w, de->d_name);
        xdrw_u64(w, seen);
        if (plus) {
            put_post_attr(w, have, exp, &st);
            uint8_t cfh[FH_LEN];
            size_t  clen = 0;
            /* No handle per entry: the client looks up what it wants. */
            (void)cfh;
            (void)clen;
            xdrw_bool(w, false);
        }
        wrote = true;
    }
    closedir(d);

    xdrw_bool(w, false);                /* the end of the entry list */
    xdrw_bool(w, eof);
    return w->len;
}

static size_t proc_fsstat(rpc_call_t *c, xdrw_t *w)
{
    uint8_t fh[64];
    size_t  fhlen = 0;
    char    path[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }

    const uint64_t bsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    struct stat    st;
    xdrw_u32(w, NFS3_OK);
    put_post_attr(w, stat(path, &st) == 0, exp, &st);
    xdrw_u64(w, (uint64_t)vfs.f_blocks * bsize);
    xdrw_u64(w, (uint64_t)vfs.f_bfree * bsize);
    xdrw_u64(w, (uint64_t)vfs.f_bavail * bsize);
    xdrw_u64(w, (uint64_t)vfs.f_files);
    xdrw_u64(w, (uint64_t)vfs.f_ffree);
    xdrw_u64(w, (uint64_t)vfs.f_ffree);
    xdrw_u32(w, 0);                     /* invarsec */
    return w->len;
}

static size_t proc_fsinfo(rpc_call_t *c, xdrw_t *w, size_t iocap)
{
    uint8_t fh[64];
    size_t  fhlen = 0;
    char    path[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    const uint32_t rt = (uint32_t)iocap;
    struct stat    st;
    xdrw_u32(w, NFS3_OK);
    put_post_attr(w, stat(path, &st) == 0, exp, &st);
    xdrw_u32(w, rt);                    /* rtmax */
    xdrw_u32(w, rt);                    /* rtpref */
    xdrw_u32(w, 4096);                  /* rtmult */
    xdrw_u32(w, 4096);                  /* wtmax */
    xdrw_u32(w, 4096);                  /* wtpref */
    xdrw_u32(w, 4096);                  /* wtmult */
    xdrw_u32(w, 4096);                  /* dtpref */
    xdrw_u64(w, 0x7FFFFFFF);            /* maxfilesize */
    xdrw_u32(w, 0); xdrw_u32(w, 1);     /* time_delta: 1 second */
    xdrw_u32(w, 0x0001 | 0x0002 | 0x0008);  /* link, symlink, homogenous */
    return w->len;
}

static size_t proc_pathconf(rpc_call_t *c, xdrw_t *w)
{
    uint8_t fh[64];
    size_t  fhlen = 0;
    char    path[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_post_attr(w, true, exp, &st);
    xdrw_u32(w, 1);                     /* linkmax */
    xdrw_u32(w, 255);                   /* name_max */
    xdrw_bool(w, true);                 /* no_trunc */
    xdrw_bool(w, false);                /* chown_restricted */
    xdrw_bool(w, false);                /* case_insensitive */
    xdrw_bool(w, true);                 /* case_preserving */
    return w->len;
}

bool nfs3_fh_for_export(int exp, uint8_t *fh, size_t *len)
{
    const nfs_export_t *e = nfsd_export(exp);
    if (e == NULL) {
        return false;
    }
    return fh_make(exp, e->path, fh, len);
}

size_t nfs3_handle(const rpc_call_t *c, xdrw_t *w, uint8_t *io, size_t iocap)
{
    rpc_call_t args = *c;

    switch (c->proc) {
    case 0:                             /* NULL */
        xdrw_accept(w, RPC_SUCCESS);
        return w->len;
    case 1:  return proc_getattr(&args, w);
    case 3:  return proc_lookup(&args, w);
    case 4:  return proc_access(&args, w);
    case 5:  return proc_readlink(&args, w);
    case 6:  return proc_read(&args, w, io, iocap);
    case 15: return proc_readdir(&args, w, false);
    case 16: return proc_readdir(&args, w, true);
    case 17: return proc_fsstat(&args, w);
    case 18: return proc_fsinfo(&args, w, iocap);
    case 19: return proc_pathconf(&args, w);

    case 2:   /* SETATTR */
    case 7:   /* WRITE */
    case 8:   /* CREATE */
    case 9:   /* MKDIR */
    case 10:  /* SYMLINK */
    case 11:  /* REMOVE */
    case 12:  /* RMDIR */
    case 13:  /* RENAME */
    case 14:  /* LINK */
    case 20:  /* COMMIT */
        xdrw_accept(w, RPC_SUCCESS);
        xdrw_u32(w, NFS3ERR_ROFS);
        return w->len;

    default:
        xdrw_accept(w, RPC_PROC_UNAVAIL);
        return w->len;
    }
}
