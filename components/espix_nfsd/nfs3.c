/*
 * NFS version 3: LOOKUP, GETATTR, ACCESS, READ, WRITE, CREATE, MKDIR, REMOVE,
 * RMDIR, RENAME, SETATTR, READDIR, READDIRPLUS, READLINK, FSSTAT, FSINFO,
 * PATHCONF, COMMIT. What a volume can hold is the volume's business: a write to
 * a FAT export works, a chmod to one is refused by the filesystem, and an export
 * that does not say rw refuses both with NFS3ERR_ROFS.
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
#include <utime.h>
#include <time.h>
#include <unistd.h>

#include "espix_fs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
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
#define NFS3ERR_XDEV     18
#define NFS3ERR_MLINK    31
#define NFS3ERR_LOOP     62
#define NFS3ERR_BADHANDLE 10001
#define NFS3ERR_NOT_SYNC 10002
#define NFS3ERR_NOTSUPP  10004
#define NFS3ERR_TOOSMALL 10005
#define NFS3ERR_SERVERFAULT 10006
#define NFS3ERR_BADTYPE  10007

/* stable_how: what the client wants done with its data before we answer. */
#define STABLE_UNSTABLE  0
#define STABLE_DATA      1
#define STABLE_FILE      2

/* createmode3 */
#define CREATE_UNCHECKED 0
#define CREATE_GUARDED   1
#define CREATE_EXCLUSIVE 2

#define FH_LEN     28      /* the size nfsd hands out; 8 was unusual enough to be a variable */
#define PATH_SLOTS 1024      /* a listing looks up every entry it lists */
#define PATH_CAP   256

typedef struct {
    uint16_t id;
    char     path[PATH_CAP];
} nfs_slot_t;

/*
 * The handle table: PSRAM, allocated on first use and kept for as long as the
 * daemon runs. A client looks up every entry it lists, so a 500-entry directory
 * needs about 500 handles alive at once. The old fixed 128-slot array wrapped
 * during that and evicted the directory's own handle; the next page came back
 * NFS3ERR_BADHANDLE, which macOS abandons the listing over -- that was the stop
 * at exactly 127 entries.
 *
 * It is never freed while the daemon lives, and that is not an optimisation
 * left undone. A table that ages out would take with it handles the client is
 * still holding: macOS mounted, listed, went quiet for a minute, listed again,
 * and every request came back BADHANDLE -- because the free was timed from the
 * last handle *minted*, and a client using the handles it already has mints
 * none. There is no signal that a client is finished with a handle, so the
 * only safe lifetime is the daemon's. 1024 slots of a 256-byte path is 258 KB
 * of PSRAM (2% of the 13 MB free), and none of it is touched until a client
 * asks for its first handle.
 */
static nfs_slot_t *s_slots;
static uint32_t    s_next_id = 1;
static int         s_cursor;

/*
 * The address the current request came from. The daemon serves one request at a
 * time, so a static holds it exactly, and it costs nothing to check: four bytes
 * against the export's client list, and no I/O.
 */
static uint32_t s_src;

void nfs3_set_source(uint32_t src)
{
    s_src = src;
}

void nfs3_slots_cleanup(void)
{
    free(s_slots);
    s_slots = NULL;
}

static bool slots_ensure(void)
{
    if (s_slots == NULL) {
        s_slots = heap_caps_calloc(PATH_SLOTS, sizeof(*s_slots), MALLOC_CAP_SPIRAM);
        if (s_slots == NULL) {
            s_slots = heap_caps_calloc(PATH_SLOTS, sizeof(*s_slots), MALLOC_CAP_8BIT);
        }
    }
    return s_slots != NULL;
}

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
    case EXDEV:      return NFS3ERR_XDEV;
    case EMLINK:     return NFS3ERR_MLINK;
    case ELOOP:      return NFS3ERR_LOOP;
    case ENOSYS:     return NFS3ERR_NOTSUPP;
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
    const uint32_t id = (uint32_t)fh[5] | ((uint32_t)fh[6] << 8) |
                        ((uint32_t)fh[7] << 16) | ((uint32_t)fh[8] << 24);
    if (e < 0 || e >= nfsd_exports_count() || s_slots == NULL) {
        return false;
    }

    /*
     * The handle was minted for a client this export allowed. A client keeps a
     * handle for as long as it likes and reuses it for every request that
     * follows, so the permission is checked on each of them, not once at mount.
     */
    const nfs_export_t *x = nfsd_export(e);
    if (x == NULL || !nfsd_client_allowed(x, s_src)) {
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
    if (!slots_ensure()) {
        return false;
    }
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

    memset(fh, 0, FH_LEN);
    fh[0] = 'E'; fh[1] = 'S'; fh[2] = 'P'; fh[3] = 'X';
    fh[4] = (uint8_t)exp;
    fh[5] = (uint8_t)(s_slots[slot].id & 0xFF);
    fh[6] = (uint8_t)((s_slots[slot].id >> 8) & 0xFF);
    fh[7] = (uint8_t)((s_slots[slot].id >> 16) & 0xFF);
    fh[8] = (uint8_t)((s_slots[slot].id >> 24) & 0xFF);

    /* The rest is a hash of the path rather than padding: a handle the client
     * carries around should look like the opaque bytes a server hands out, and
     * zeros were the last thing about ours that did not. */
    uint32_t h = 2166136261u;
    for (const char *p = path; p != NULL && *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    for (size_t i = 9; i < FH_LEN; i++) {
        fh[i] = (uint8_t)(h >> ((i % 4) * 8));
        h = h * 16777619u + 0x9e3779b9u;
    }
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

/*
 * A file's attributes, in the shape a server the client accepts sends them:
 * the client decodes uid/gid through the mount's idmap and gives up on a value
 * it cannot map, so an export reports root ownership (what root_squash means)
 * rather than espix's local 1000. A directory gets a size and a link count a
 * directory can have, and a clock that never answered gets the current time
 * rather than 1970 -- both of which are what a real server reports.
 */
static void put_fattr(xdrw_t *w, int exp, const struct stat *st)
{
    const bool dir = S_ISDIR(st->st_mode);
    const uint32_t nlink = (uint32_t)(st->st_nlink != 0 ? st->st_nlink : 1);
    const uint64_t size = (uint64_t)st->st_size;
    const uint64_t used = ((size + 511) / 512) * 512;
    const long now = (long)time(NULL);
    const long mt = (st->st_mtime != 0) ? (long)st->st_mtime : now;

    xdrw_u32(w, ftype(st->st_mode));
    xdrw_u32(w, (uint32_t)(st->st_mode & 07777));
    xdrw_u32(w, dir && nlink < 2 ? 2 : nlink);
    /*
     * The file's own owner. On a FAT mount that is whoever the mount was made
     * as -- there is no squash here, so this is the same owner a Linux client
     * would be shown by a Linux server holding the same files.
     */
    xdrw_u32(w, (uint32_t)st->st_uid);
    xdrw_u32(w, (uint32_t)st->st_gid);
    xdrw_u64(w, (dir && size == 0) ? 4096 : size);
    xdrw_u64(w, (dir && used == 0) ? 4096 : used);
    xdrw_u32(w, 0);                     /* rdev: major, minor */
    xdrw_u32(w, 0);
    /* A device hash and an inode, as a server reports them: 1/1 is the shape
     * that is not. */
    const nfs_export_t *e = nfsd_export(exp);
    uint32_t h = 0x811c9dc5u;
    for (const char *p = (e != NULL) ? e->path : NULL; p != NULL && *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    xdrw_u64(w, (uint64_t)0x4553505800000000ull | h);
    xdrw_u64(w, (uint64_t)(st->st_ino != 0 ? st->st_ino : h));
    put_time(w, mt);
    put_time(w, mt);                    /* ctime: the VFS gives no separate one */
    put_time(w, mt);
}

/* post_op_attr: true and the attributes, or false. */
static void put_post_attr(xdrw_t *w, bool have, int exp, const struct stat *st)
{
    xdrw_bool(w, have);
    if (have) {
        put_fattr(w, exp, st);
    }
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

/* Declared here rather than with the write path below, because LOOKUP uses it
 * too: a path that does not fit is an error, not a name cut short. */
static bool join_path(char *out, size_t cap, const char *dir, const char *name);

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

    /*
     * LOOKUP's failure arm carries the directory's attributes, and the client
     * decodes that arm whenever the status is not OK. Answering with the status
     * alone reaches it as an I/O error -- and the first stat of a file that is
     * not there yet is how every create begins, so leaving this out made writes
     * impossible in a way that looked like a disk fault.
     */
    if (!fh_resolve(fh, fhlen, &exp, dir, sizeof(dir)) || !name_ok(name)) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }
    if (!join_path(path, sizeof(path), dir, name)) {
        xdrw_u32(w, NFS3ERR_NAMETOOLONG);
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        struct stat dst;
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, stat(dir, &dst) == 0, exp, &dst);
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
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, false, exp, NULL);
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
    char    path[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    /* espix's VFS has no symlink, so a path that exists is not one -- which is
     * INVAL, the answer a Linux server gives for readlink on anything else --
     * and a path that is not there says so. */
    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }
    xdrw_u32(w, NFS3ERR_INVAL);
    put_post_attr(w, true, exp, &st);
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
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }
    if (off != 0 && lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        xdrw_u32(w, NFS3ERR_INVAL);
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    const size_t room = (want < iocap) ? want : iocap;
    const int    got  = read(fd, io, room);
    struct stat  st;
    const bool   have = (fstat(fd, &st) == 0);
    close(fd);

    if (got < 0) {
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, have, exp, &st);
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
/* FatFs date/time to Unix seconds, the arithmetic IDF uses for stat. Without
 * it a listing would date every entry 1970. */
static time_t fat_info_time(uint16_t date, uint16_t time)
{
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
    tm.tm_mday = date & 0x1F;
    tm.tm_mon  = ((date >> 5) & 0x0F) - 1;
    tm.tm_year = ((date >> 9) & 0x7F) + 80;
    tm.tm_sec  = (time & 0x1F) * 2;
    tm.tm_min  = (time >> 5) & 0x3F;
    tm.tm_hour = (time >> 11) & 0x1F;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

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
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    DIR *d = opendir(path);
    if (d == NULL) {
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, false, exp, NULL);
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

    /*
     * A mount that keeps no ownership of its own owns everything on it as the
     * one who mounted it, and a worklist entry cannot report that per name.
     * Asked once for the page it is a lookup in the mount table -- asking per
     * entry would be the stat this walk exists to avoid. Zero for a filesystem
     * that stores its own ownership, which is what the entries then report.
     */
    uint16_t ouid = 0;
    uint16_t ogid = 0;
    (void)espix_fs_mount_owner(path, &ouid, &ogid);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        seen++;
        if (seen <= cookie) {
            continue;                   /* already sent, in a previous page */
        }

        /*
         * Reserve what this entry will really cost before writing it, not a
         * guess: a READDIRPLUS entry carries fileid, name, cookie, attributes
         * and a handle slot, so 256 bytes was often short and the reply could
         * pass the maxcount the client sized its buffer for. It then could not
         * decode the page, asked again, and the server re-read the directory
         * every time -- a mount that hung with the disk light on.
         */
        const size_t name_cost = strlen(de->d_name) + 4;
        const size_t entry_cost = plus ? (200 + name_cost) : (28 + name_cost);
        if (wrote && (w->len - reply_at) + entry_cost + 16 > maxcount) {
            eof = false;
            break;
        }

        /*
         * No stat() per entry. A stat on this exFAT volume costs a second or
         * more, so attributes for every name made a listing take minutes and
         * left the client timing out. The client looks up what it needs, and
         * the plus form is allowed to say the attributes are not here.
         */
        xdrw_bool(w, true);             /* this entry exists */
        xdrw_u64(w, (uint64_t)(de->d_ino != 0 ? de->d_ino : seen));
        xdrw_string(w, de->d_name);
        xdrw_u64(w, seen);
        if (plus) {
            /*
             * Attributes and a handle, both from what the walk already read.
             * A stat here would scan the directory, and a missing handle makes
             * the client look every name up itself -- 500 lookups, 500 scans.
             * Supplying both is what makes a listing one walk and nothing else.
             */
            espix_fs_entry_info_t ei;
            struct stat st;
            const bool  have = espix_fs_last_entry(&ei);
            char        child[PATH_CAP];
            uint8_t     cfh[FH_LEN];
            size_t      clen = 0;

            memset(&st, 0, sizeof(st));
            if (have) {
                st.st_mode  = (ei.attr & 0x10) ? (S_IFDIR | 0777)
                                               : (S_IFREG | 0777);
                st.st_size  = ei.size;
                st.st_mtime = fat_info_time(ei.date, ei.time);
                st.st_uid   = ouid;
                st.st_gid   = ogid;
            }
            put_post_attr(w, have, exp, &st);

            strlcpy(child, path, sizeof(child));
            if (child[0] == 0 || child[strlen(child) - 1] != '/') {
                strlcat(child, "/", sizeof(child));
            }
            strlcat(child, de->d_name, sizeof(child));

            if (fh_make(exp, child, cfh, &clen)) {
                xdrw_bool(w, true);
                xdrw_u32(w, (uint32_t)clen);
                xdrw_opaque(w, cfh, clen);
            } else {
                xdrw_bool(w, false);
            }
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
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    /*
     * A server that cannot measure the volume still has to answer: returning
     * an error here failed the whole mount, because FSSTAT is part of looking
     * a filesystem up. Say so in the log and report an empty filesystem.
     */
    /*
     * espix's own space API rather than POSIX statvfs(): statvfs is registered
     * per filesystem and only the ext side registers it, so the FAT volume the
     * stick is on answers nothing. FSSTAT is part of looking a filesystem up --
     * failing it fails the mount -- and df reads the same way.
     */
    uint64_t total = 0, avail = 0;
    if (espix_fs_stat_fat(path, &total, &avail) != ESP_OK) {
        bool got = false;
#if CONFIG_ESPIX_FS_EXT4
        got = (espix_fs_stat_ext(path, &total, &avail) == ESP_OK);
#endif
        if (!got) {
            struct statvfs vfs;
            if (statvfs(path, &vfs) == 0) {
                const uint64_t bs = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
                total = (uint64_t)vfs.f_blocks * bs;
                avail = (uint64_t)vfs.f_bavail * bs;
                got = true;
            }
        }
        if (!got) {
            espix_fs_info_t root;
            if (espix_fs_stat_root(&root) == ESP_OK) {
                total = root.total_bytes;
                avail = root.used_bytes <= root.total_bytes
                            ? root.total_bytes - root.used_bytes : 0;
            } else {
                espix_klog(ESPIX_KLOG_WARN, "nfs3",
                           "fsstat '%s': no volume answered", path);
            }
        }
    }

    struct stat st;
    xdrw_u32(w, NFS3_OK);
    put_post_attr(w, stat(path, &st) == 0, exp, &st);
    xdrw_u64(w, total);
    xdrw_u64(w, avail);
    xdrw_u64(w, avail);
    xdrw_u64(w, 0);                     /* the API counts bytes, not inodes */
    xdrw_u64(w, 0);
    xdrw_u64(w, 0);
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
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    const uint32_t rt = (uint32_t)iocap;
    struct stat    st;
    xdrw_u32(w, NFS3_OK);
    /* Also without attributes, as nfsd sends it: 80 bytes, not 164. */
    put_post_attr(w, false, exp, &st);
    (void)st;
    xdrw_u32(w, rt);                    /* rtmax */
    xdrw_u32(w, rt);                    /* rtpref */
    xdrw_u32(w, 4096);                  /* rtmult */
    xdrw_u32(w, 4096);                  /* wtmax */
    xdrw_u32(w, 4096);                  /* wtpref */
    xdrw_u32(w, 4096);                  /* wtmult */
    xdrw_u32(w, 4096);                  /* dtpref */
    xdrw_u64(w, 0x7FFFFFFF);            /* maxfilesize */
    xdrw_u32(w, 1); xdrw_u32(w, 0);     /* time_delta: 1 second, as nfsd reports it */
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
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
        put_post_attr(w, false, exp, NULL);
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    /* Linux's nfsd answers PATHCONF without attributes (post_op_attr FALSE),
     * and that is the shape a client accepts -- 56 bytes, not 140. */
    put_post_attr(w, false, exp, &st);
    (void)st;
    /* The values a real server reports, which is also the truth here: espix
     * makes no hard links, rejects an over-long name rather than truncating it,
     * and only root may change ownership. */
    xdrw_u32(w, 32000);                 /* linkmax */
    xdrw_u32(w, 255);                   /* name_max */
    xdrw_bool(w, false);                /* no_trunc */
    xdrw_bool(w, true);                 /* chown_restricted */
    xdrw_bool(w, false);                /* case_insensitive */
    xdrw_bool(w, true);                 /* case_preserving */
    return w->len;
}

/* -------------------------------------------------------------- writes --- */

/*
 * Everything below changes something. An export is read-only unless
 * /etc/exports says rw -- "ro" is espix's default rather than its exception,
 * because a stick that becomes writable over the network by omission is the
 * wrong way round -- and a mount made read-only answers EROFS from the
 * filesystem underneath whatever this says, so both gates are real.
 */
static bool may_write(int exp)
{
    const nfs_export_t *e = nfsd_export(exp);
    return (e != NULL) && !e->ro;
}

/* XDR's boolean is a 32-bit word, nonzero for true. */
static bool get_bool(rpc_call_t *c, bool *out)
{
    uint32_t v;

    if (!rpc_get_u32(c, &v)) {
        return false;
    }
    *out = (v != 0);
    return true;
}

/*
 * A name under a directory, or false when it does not fit. The refusal is the
 * point: a path silently cut short names a *different* file, which is worse
 * than an error the client can act on.
 */
static bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    if (strlcpy(out, dir, cap) >= cap || strlcat(out, "/", cap) >= cap) {
        return false;
    }
    return strlcat(out, name, cap) < cap;
}

/* wcc_attr: the size and the timestamps a client checks before it trusts what
 * it has cached. espix has no separate ctime, so it reports mtime twice, as it
 * does everywhere else. */
static void put_wcc_attr(xdrw_t *w, const struct stat *st)
{
    xdrw_u64(w, (uint64_t)st->st_size);
    put_time(w, (long)st->st_mtime);
    put_time(w, (long)st->st_mtime);
}

/*
 * wcc_data: what an object looked like before a change and after it. Both
 * sides are optional in XDR, and neither is optional in the reply -- the client
 * decodes the arm its status chose, so a failure that stops after the status
 * reaches it as an I/O error instead of the errno it was sent.
 */
static void put_wcc(xdrw_t *w, int exp, bool have_before,
                    const struct stat *before, bool have_after,
                    const struct stat *after)
{
    xdrw_bool(w, have_before);
    if (have_before) {
        put_wcc_attr(w, before);
    }
    put_post_attr(w, have_after, exp, after);
}

/* Nothing to say about the object: legal, and what an error raised before the
 * path is even known uses. */
static void put_wcc_none(xdrw_t *w)
{
    xdrw_bool(w, false);
    xdrw_bool(w, false);
}

static void put_wcc_fail(xdrw_t *w, bool have_before,
                         const struct stat *before)
{
    xdrw_bool(w, have_before);
    if (have_before) {
        put_wcc_attr(w, before);
    }
    xdrw_bool(w, false);                /* the operation failed: nothing moved */
}

/* The same, for the directory a name was added to or taken out of. */
static void put_dir_wcc(xdrw_t *w, int exp, const char *dir,
                        bool have_before, const struct stat *before)
{
    struct stat after;
    const bool  have_after = (stat(dir, &after) == 0);

    put_wcc(w, exp, have_before, before, have_after, &after);
}

/*
 * writeverf3: eight bytes that identify this boot. A client holding data it has
 * not committed compares this after a reconnect and sends the data again if it
 * changed, so it has to be one value within a boot and a different one after --
 * which is why it comes from the random source rather than from a fixed tag.
 */
static void put_verf(xdrw_t *w)
{
    static uint32_t s_verf[2];

    if (s_verf[0] == 0 && s_verf[1] == 0) {
        s_verf[0] = esp_random() ^ 0x45535058u;         /* ESPX */
        s_verf[1] = esp_random() ^ 0x4e465333u;         /* NFS3 */
    }
    xdrw_u32(w, s_verf[0]);
    xdrw_u32(w, s_verf[1]);
}

/* sattr3, which every creating and setting procedure carries. */
typedef struct {
    bool     set_mode, set_uid, set_gid, set_size;
    uint16_t mode, uid, gid;
    uint64_t size;
    uint32_t atime_kind, mtime_kind;    /* 0 dont change, 1 server, 2 client */
    long     atime_sec, mtime_sec;
} sattr3_t;

static bool get_sattr(rpc_call_t *c, sattr3_t *a)
{
    bool     has = false;
    uint32_t v = 0;

    memset(a, 0, sizeof(*a));

    if (!get_bool(c, &has)) {
        return false;
    }
    if (has) {
        if (!rpc_get_u32(c, &v)) {
            return false;
        }
        a->set_mode = true;
        a->mode = (uint16_t)(v & 07777);
    }
    if (!get_bool(c, &has)) {
        return false;
    }
    if (has) {
        if (!rpc_get_u32(c, &v)) {
            return false;
        }
        a->set_uid = true;
        a->uid = (uint16_t)v;
    }
    if (!get_bool(c, &has)) {
        return false;
    }
    if (has) {
        if (!rpc_get_u32(c, &v)) {
            return false;
        }
        a->set_gid = true;
        a->gid = (uint16_t)v;
    }
    if (!get_bool(c, &has)) {
        return false;
    }
    if (has) {
        if (!rpc_get_u64(c, &a->size)) {
            return false;
        }
        a->set_size = true;
    }
    /*
     * The two times are not optionals like the four above: sattr3 carries a
     * set_time, which is a discriminant on its own -- 0 leave it, 1 set it to
     * the server's clock, 2 take the time that follows -- so reading a bool
     * here shifts the rest of the request by a word. A client that asks for
     * SET_TO_SERVER_TIME, which is what an ordinary write followed by a
     * setattr does, then fails to parse at all.
     */
    if (!rpc_get_u32(c, &a->atime_kind)) {
        return false;
    }
    if (a->atime_kind == 2) {
        uint32_t sec = 0, nsec = 0;
        if (!rpc_get_u32(c, &sec) || !rpc_get_u32(c, &nsec)) {
            return false;
        }
        a->atime_sec = (long)sec;
    }
    if (!rpc_get_u32(c, &a->mtime_kind)) {
        return false;
    }
    if (a->mtime_kind == 2) {
        uint32_t sec = 0, nsec = 0;
        if (!rpc_get_u32(c, &sec) || !rpc_get_u32(c, &nsec)) {
            return false;
        }
        a->mtime_sec = (long)sec;
    }
    return true;
}

/*
 * Apply what the volume can hold. FAT keeps no modes and no owners, so the two
 * fields it has not got are applied and forgotten: the filesystem refuses them
 * and the refusal stops here, because a client asking for a mode it opened with
 * is doing nothing wrong and must not have its create fail over it.
 *
 * SET_TO_SERVER_TIME needs nothing on its own: the write it accompanies is what
 * sets the time, which is what that mode means.
 *
 * Note that this is the NFS view of a volume, not espix's own: a chmod through
 * the shell or an app still answers EPERM on a filesystem that keeps no modes,
 * while the same chmod over the wire is accepted and forgotten, as it is on a
 * Linux server exporting the same kind of directory.
 */
static int apply_sattr(const char *path, const sattr3_t *a)
{
    int err = 0;

    if (a->set_size && truncate(path, (off_t)a->size) != 0) {
        err = errno;
    }
    /*
     * A mode or an owner is not something a FAT volume has, and a refusal is
     * not the client's business: a Linux server exporting a vfat directory
     * accepts a chmod and forgets it, and refusing one fails the
     * create-then-setattr sequence an ordinary "cp" is made of -- the client
     * asks for the mode it opened with, is told no, and reports the open as
     * failed while the file it created sits there empty. What the volume can
     * genuinely answer -- a size, a time, a read-only mount -- is still
     * reported; this is only about the two fields FAT does not have.
     */
    if (a->set_mode) {
        (void)espix_fs_chmod(path, (mode_t)a->mode);
    }
    if (a->set_uid || a->set_gid) {
        (void)espix_fs_chown(path, a->set_uid ? a->uid : ESPIX_FS_KEEP_ID,
                             a->set_gid ? a->gid : ESPIX_FS_KEEP_ID);
    }
    if (a->atime_kind == 2 || a->mtime_kind == 2) {
        struct stat    st;
        struct utimbuf ut;

        /*
         * utime() names two times and sets both, so a field the client did not
         * ask for is filled in from the file as it stands: the alternative is
         * leaving it to become "now", which is a change nobody requested. FAT
         * keeps no access time, so on a stick only the modification lands.
         */
        if (stat(path, &st) == 0) {
            ut.actime  = (a->atime_kind == 2) ? (time_t)a->atime_sec
                                              : st.st_atime;
            ut.modtime = (a->mtime_kind == 2) ? (time_t)a->mtime_sec
                                              : st.st_mtime;
            if (utime(path, &ut) != 0) {
                err = errno;
            }
        }
    }
    return err;
}

/* The failure half of the replies that report a directory unchanged. */
static void put_dir_wcc_fail(xdrw_t *w, bool have_before,
                             const struct stat *before)
{
    xdrw_bool(w, have_before);
    if (have_before) {
        put_wcc_attr(w, before);
    }
    xdrw_bool(w, false);
}

/* ---------------------------------------------------------- procedures --- */

static size_t proc_setattr(rpc_call_t *c, xdrw_t *w)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    char     path[PATH_CAP];
    int      exp = 0;
    sattr3_t attr;
    bool     guard = false;
    uint32_t gsec = 0, gnsec = 0;

    if (!get_fh(c, fh, &fhlen) || !get_sattr(c, &attr) ||
        !get_bool(c, &guard)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    if (guard && (!rpc_get_u32(c, &gsec) || !rpc_get_u32(c, &gnsec))) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_wcc_none(w);
        return w->len;
    }

    struct stat before;
    const bool  have_before = (stat(path, &before) == 0);

    if (!may_write(exp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_fail(w, have_before, &before);
        return w->len;
    }
    if (!have_before) {
        xdrw_u32(w, nfserr(errno));
        put_wcc_fail(w, false, &before);
        return w->len;
    }
    /* A guard the file has already moved past is not a failure of the request
     * but of its precondition: the client re-reads and asks again. */
    if (guard && (uint32_t)before.st_mtime != gsec) {
        xdrw_u32(w, NFS3ERR_NOT_SYNC);
        put_wcc_fail(w, true, &before);
        return w->len;
    }

    const int err = apply_sattr(path, &attr);
    if (err != 0) {
        xdrw_u32(w, nfserr(err));
        put_wcc_fail(w, true, &before);
        return w->len;
    }

    struct stat after;
    const bool  have_after = (stat(path, &after) == 0);

    xdrw_u32(w, NFS3_OK);
    put_wcc(w, exp, true, &before, have_after, &after);
    return w->len;
}

static size_t proc_write(rpc_call_t *c, xdrw_t *w)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    uint64_t off = 0;
    uint32_t count = 0, stable = 0, dlen = 0;
    char     path[PATH_CAP];
    int      exp = 0;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_u64(c, &off) ||
        !rpc_get_u32(c, &count) || !rpc_get_u32(c, &stable) ||
        !rpc_get_u32(c, &dlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    /*
     * The data is the rest of the request and it is written from where it
     * already is: copying it would be another 4 KB of RAM and a second pass
     * over bytes that are already in the buffer.
     */
    if (dlen > count || c->pos + dlen > c->len) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    const uint8_t *data = c->buf + c->pos;

    xdrw_accept(w, RPC_SUCCESS);
    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_wcc_none(w);
        return w->len;
    }
    if (!may_write(exp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_none(w);
        return w->len;
    }

    const int fd = open(path, O_WRONLY);
    if (fd < 0) {
        xdrw_u32(w, nfserr(errno));
        put_wcc_none(w);
        return w->len;
    }

    struct stat before;
    const bool  have_before = (fstat(fd, &before) == 0);

    if (off != 0 && lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        xdrw_u32(w, NFS3ERR_INVAL);
        put_wcc_fail(w, have_before, &before);
        return w->len;
    }

    size_t done = 0;
    while (done < dlen) {
        const ssize_t n = write(fd, data + done, dlen - done);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    int err = (done < dlen) ? (errno ? errno : EIO) : 0;

    /*
     * FILE_SYNC and DATA_SYNC are flushed here; UNSTABLE is left in the
     * filesystem's cache and the client's COMMIT is what flushes it. That is
     * what the mode means, and doing it the other way round would make every
     * few KB of a copy pay for a FAT and directory sync of its own.
     */
    if (err == 0 && stable != STABLE_UNSTABLE && fsync(fd) != 0) {
        err = errno ? errno : EIO;
    }

    struct stat after;
    const bool  have_after = (fstat(fd, &after) == 0);
    close(fd);

    if (err != 0) {
        xdrw_u32(w, nfserr(err));
        put_wcc_fail(w, have_before, &before);
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_wcc(w, exp, have_before, &before, have_after, &after);
    xdrw_u32(w, (uint32_t)done);
    xdrw_u32(w, (stable == STABLE_UNSTABLE) ? STABLE_UNSTABLE : STABLE_FILE);
    put_verf(w);
    return w->len;
}

static size_t proc_create(rpc_call_t *c, xdrw_t *w)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    char     dir[PATH_CAP], name[256], path[PATH_CAP];
    int      exp = 0;
    uint32_t how = 0;
    sattr3_t attr;
    uint8_t  verifier[8];

    if (!get_fh(c, fh, &fhlen) || !rpc_get_string(c, name, sizeof(name)) ||
        !rpc_get_u32(c, &how)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    memset(&attr, 0, sizeof(attr));
    if (how == CREATE_EXCLUSIVE) {
        /* A verifier the client would have us remember, so that a create it
         * sends twice is recognised. Nothing here is replayed -- the transport
         * is not a retry loop -- so it is read to keep the request in step and
         * then dropped. */
        if (!rpc_get_opaque(c, verifier, sizeof(verifier))) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
    } else if (!get_sattr(c, &attr)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, dir, sizeof(dir)) || !name_ok(name)) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_wcc_none(w);
        return w->len;
    }
    if (!join_path(path, sizeof(path), dir, name)) {
        xdrw_u32(w, NFS3ERR_NAMETOOLONG);
        put_wcc_none(w);
        return w->len;
    }
    if (!may_write(exp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_none(w);
        return w->len;
    }

    struct stat dbefore;
    const bool  have_dbefore = (stat(dir, &dbefore) == 0);

    /* UNCHECKED creates or truncates; GUARDED and EXCLUSIVE only create. */
    const int flags = O_WRONLY | O_CREAT |
                      ((how == CREATE_UNCHECKED) ? O_TRUNC : O_EXCL);
    const int fd = open(path, flags, 0666);
    if (fd < 0) {
        xdrw_u32(w, nfserr(errno));
        put_dir_wcc_fail(w, have_dbefore, &dbefore);
        return w->len;
    }
    close(fd);

    /* The mode and the times are the volume's business on FAT; failing the
     * create over them would fail a copy that has nothing wrong with it. */
    if (how != CREATE_EXCLUSIVE) {
        (void)apply_sattr(path, &attr);
    }

    struct stat st;
    const bool  have_st = (stat(path, &st) == 0);
    uint8_t     cfh[FH_LEN];
    size_t      clen = 0;
    const bool  have_fh = fh_make(exp, path, cfh, &clen);

    xdrw_u32(w, NFS3_OK);
    xdrw_bool(w, have_fh);
    if (have_fh) {
        xdrw_u32(w, (uint32_t)clen);
        xdrw_opaque(w, cfh, clen);
    }
    put_post_attr(w, have_st, exp, &st);
    put_dir_wcc(w, exp, dir, have_dbefore, &dbefore);
    return w->len;
}

static size_t proc_mkdir(rpc_call_t *c, xdrw_t *w)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    char     dir[PATH_CAP], name[256], path[PATH_CAP];
    int      exp = 0;
    sattr3_t attr;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_string(c, name, sizeof(name)) ||
        !get_sattr(c, &attr)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, dir, sizeof(dir)) || !name_ok(name)) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_wcc_none(w);
        return w->len;
    }
    if (!join_path(path, sizeof(path), dir, name)) {
        xdrw_u32(w, NFS3ERR_NAMETOOLONG);
        put_wcc_none(w);
        return w->len;
    }
    if (!may_write(exp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_none(w);
        return w->len;
    }

    struct stat dbefore;
    const bool  have_dbefore = (stat(dir, &dbefore) == 0);

    if (mkdir(path, attr.set_mode ? (mode_t)attr.mode : 0755) != 0) {
        xdrw_u32(w, nfserr(errno));
        put_dir_wcc_fail(w, have_dbefore, &dbefore);
        return w->len;
    }
    if (attr.set_uid || attr.set_gid || attr.atime_kind == 2 ||
        attr.mtime_kind == 2) {
        (void)apply_sattr(path, &attr);
    }

    struct stat st;
    const bool  have_st = (stat(path, &st) == 0);
    uint8_t     cfh[FH_LEN];
    size_t      clen = 0;
    const bool  have_fh = fh_make(exp, path, cfh, &clen);

    xdrw_u32(w, NFS3_OK);
    xdrw_bool(w, have_fh);
    if (have_fh) {
        xdrw_u32(w, (uint32_t)clen);
        xdrw_opaque(w, cfh, clen);
    }
    put_post_attr(w, have_st, exp, &st);
    put_dir_wcc(w, exp, dir, have_dbefore, &dbefore);
    return w->len;
}

/* Is the directory empty, apart from the two names every one has? */
static bool dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) {
        return false;
    }
    bool          empty = true;
    struct dirent *de;

    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(d);
    return empty;
}

static size_t proc_remove(rpc_call_t *c, xdrw_t *w, bool is_dir)
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
        put_wcc_none(w);
        return w->len;
    }
    if (!join_path(path, sizeof(path), dir, name)) {
        xdrw_u32(w, NFS3ERR_NAMETOOLONG);
        put_wcc_none(w);
        return w->len;
    }
    if (!may_write(exp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_none(w);
        return w->len;
    }

    struct stat dbefore;
    const bool  have_dbefore = (stat(dir, &dbefore) == 0);

    /*
     * FatFs answers FR_DENIED for a directory that still has names in it, and
     * IDF turns that into EACCES -- so a client would be told "permission
     * denied" where the truth is "not empty". Asked here instead, where the
     * answer is the one POSIX defines.
     */
    if (is_dir && !dir_is_empty(path)) {
        xdrw_u32(w, NFS3ERR_NOTEMPTY);
        put_dir_wcc_fail(w, have_dbefore, &dbefore);
        return w->len;
    }

    const int rc = is_dir ? rmdir(path) : unlink(path);
    if (rc != 0) {
        xdrw_u32(w, nfserr(errno));
        put_dir_wcc_fail(w, have_dbefore, &dbefore);
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_dir_wcc(w, exp, dir, have_dbefore, &dbefore);
    return w->len;
}

static size_t proc_rename(rpc_call_t *c, xdrw_t *w)
{
    uint8_t ffh[64], tfh[64];
    size_t  ffhlen = 0, tfhlen = 0;
    char    fdir[PATH_CAP], fname[256], fpath[PATH_CAP];
    char    tdir[PATH_CAP], tname[256], tpath[PATH_CAP];
    int     fexp = 0, texp = 0;

    if (!get_fh(c, ffh, &ffhlen) || !rpc_get_string(c, fname, sizeof(fname)) ||
        !get_fh(c, tfh, &tfhlen) || !rpc_get_string(c, tname, sizeof(tname))) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(ffh, ffhlen, &fexp, fdir, sizeof(fdir)) ||
        !fh_resolve(tfh, tfhlen, &texp, tdir, sizeof(tdir)) ||
        !name_ok(fname) || !name_ok(tname)) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_wcc_none(w);
        put_wcc_none(w);
        return w->len;
    }
    if (!join_path(fpath, sizeof(fpath), fdir, fname) ||
        !join_path(tpath, sizeof(tpath), tdir, tname)) {
        xdrw_u32(w, NFS3ERR_NAMETOOLONG);
        put_wcc_none(w);
        put_wcc_none(w);
        return w->len;
    }
    if (!may_write(fexp) || !may_write(texp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_none(w);
        put_wcc_none(w);
        return w->len;
    }

    struct stat fbefore, tbefore;
    const bool  have_f = (stat(fdir, &fbefore) == 0);
    const bool  have_t = (stat(tdir, &tbefore) == 0);

    if (rename(fpath, tpath) != 0) {
        xdrw_u32(w, nfserr(errno));
        put_wcc_fail(w, have_f, &fbefore);
        put_wcc_fail(w, have_t, &tbefore);
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_dir_wcc(w, fexp, fdir, have_f, &fbefore);
    put_dir_wcc(w, texp, tdir, have_t, &tbefore);
    return w->len;
}

static size_t proc_commit(rpc_call_t *c, xdrw_t *w)
{
    uint8_t  fh[64];
    size_t   fhlen = 0;
    uint64_t off = 0;
    uint32_t count = 0;
    char     path[PATH_CAP];
    int      exp = 0;

    if (!get_fh(c, fh, &fhlen) || !rpc_get_u64(c, &off) ||
        !rpc_get_u32(c, &count)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, path, sizeof(path))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        put_wcc_none(w);
        return w->len;
    }
    if (!may_write(exp)) {
        xdrw_u32(w, NFS3ERR_ROFS);
        put_wcc_none(w);
        return w->len;
    }

    /*
     * A commit is a flush of the volume's cache, because that is what f_sync
     * does and the volume is what has to reach the stick. The range in the
     * arguments is a hint a server may narrow; this one keeps no per-range
     * cache to narrow it with.
     */
    const int fd = open(path, O_RDWR);
    if (fd < 0) {
        xdrw_u32(w, nfserr(errno));
        put_wcc_none(w);
        return w->len;
    }

    struct stat before, after;
    const bool  have_before = (fstat(fd, &before) == 0);
    const int   rc = fsync(fd);
    const bool  have_after = (fstat(fd, &after) == 0);
    const int   err = (rc == 0) ? 0 : (errno ? errno : EIO);

    close(fd);

    if (err != 0) {
        xdrw_u32(w, nfserr(err));
        put_wcc_fail(w, have_before, &before);
        return w->len;
    }

    xdrw_u32(w, NFS3_OK);
    put_wcc(w, exp, have_before, &before, have_after, &after);
    put_verf(w);
    return w->len;
}

/*
 * LINK, SYMLINK and MKNOD name things this filesystem has not got: FAT keeps no
 * hard links, and espix's VFS has no symlink to make. Each still answers in the
 * shape its own procedure defines, because the client decodes the arm its
 * status chose -- and a name that was to be created leaves its directory
 * unchanged, which is what the wcc says.
 */
static size_t proc_notsupp(rpc_call_t *c, xdrw_t *w, bool link)
{
    uint8_t fh[64];
    size_t  fhlen = 0;
    char    name[256];
    char    dir[PATH_CAP];
    int     exp = 0;

    if (!get_fh(c, fh, &fhlen)) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    if (link) {
        /* LINK carries the file handle first, then the name to link it to. */
        uint8_t lfh[64];
        size_t  lfhlen = 0;

        if (!get_fh(c, lfh, &lfhlen)) {
            xdrw_accept(w, RPC_GARBAGE_ARGS);
            return w->len;
        }
        fhlen = lfhlen;
        memcpy(fh, lfh, sizeof(fh));
    }
    if (!rpc_get_string(c, name, sizeof(name))) {
        xdrw_accept(w, RPC_GARBAGE_ARGS);
        return w->len;
    }
    xdrw_accept(w, RPC_SUCCESS);

    if (!fh_resolve(fh, fhlen, &exp, dir, sizeof(dir))) {
        xdrw_u32(w, NFS3ERR_BADHANDLE);
        if (link) {
            put_post_attr(w, false, exp, NULL);
        }
        put_wcc_none(w);
        return w->len;
    }

    xdrw_u32(w, NFS3ERR_NOTSUPP);
    if (link) {
        put_post_attr(w, false, exp, NULL);         /* the file side */
    }
    put_dir_wcc(w, exp, dir, false, NULL);
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
    /* RFC 1813: 16 READDIR, 17 READDIRPLUS, 18 FSSTAT, 19 FSINFO, 20 PATHCONF.
     * This table was one low from 15 up, so a client asking FSINFO (19) was
     * answered with a PATHCONF reply and gave up with "RPC struct is bad" --
     * the whole of why no client would mount. */
    case 16: return proc_readdir(&args, w, false);
    case 17: return proc_readdir(&args, w, true);
    case 18: return proc_fsstat(&args, w);
    case 19: return proc_fsinfo(&args, w, iocap);
    case 20: return proc_pathconf(&args, w);

    case 2:  return proc_setattr(&args, w);
    case 7:  return proc_write(&args, w);
    case 8:  return proc_create(&args, w);
    case 9:  return proc_mkdir(&args, w);
    case 10:                                    /* SYMLINK */
    case 11: /* MKNOD */
        return proc_notsupp(&args, w, false);
    case 12: return proc_remove(&args, w, false);
    case 13: return proc_remove(&args, w, true);
    case 14: return proc_rename(&args, w);
    case 15:                                    /* LINK */
        return proc_notsupp(&args, w, true);
    case 21: return proc_commit(&args, w);

    default:
        xdrw_accept(w, RPC_PROC_UNAVAIL);
        return w->len;
    }
}
