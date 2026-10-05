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
#include <time.h>
#include <unistd.h>

#include "espix_fs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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

#define FH_LEN     28      /* the size nfsd hands out; 8 was unusual enough to be a variable */
#define PATH_SLOTS 1024      /* a listing looks up every entry it lists */
#define PATH_CAP   256

typedef struct {
    uint16_t id;
    char     path[PATH_CAP];
} nfs_slot_t;

/*
 * The handle table: PSRAM, allocated on first use, freed once nobody has asked
 * for a handle for a while. A client looks up every entry it lists, so a
 * 500-entry directory needs about 500 handles alive at once. The old fixed
 * 128-slot array wrapped during that and evicted the directory own handle; the
 * next page came back NFS3ERR_BADHANDLE, which macOS abandons the listing
 * over -- that was the stop at exactly 127 entries.
 */
static nfs_slot_t *s_slots;
static uint16_t    s_next_id = 1;
static int         s_cursor;
static uint32_t    s_last_ms;
#define SLOTS_IDLE_MS 30000

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
        s_last_ms = (uint32_t)esp_log_timestamp();
    }
    if (s_slots != NULL && s_last_ms != 0 &&
        (uint32_t)((uint32_t)esp_log_timestamp() - s_last_ms) > SLOTS_IDLE_MS) {
        free(s_slots);              /* nobody is holding a handle any more */
        s_slots = NULL;
        return slots_ensure();
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
    s_last_ms = (uint32_t)esp_log_timestamp();
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
    fh[6] = (uint8_t)(s_slots[slot].id >> 8);
    fh[7] = 0;

    /* The rest is a hash of the path rather than padding: a handle the client
     * carries around should look like the opaque bytes a server hands out, and
     * zeros were the last thing about ours that did not. */
    uint32_t h = 2166136261u;
    for (const char *p = path; p != NULL && *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    for (size_t i = 8; i < FH_LEN; i++) {
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
        return w->len;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        xdrw_u32(w, nfserr(errno));
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

    case 2:   /* SETATTR */
    case 7:   /* WRITE */
    case 8:   /* CREATE */
    case 9:   /* MKDIR */
    case 10:  /* SYMLINK */
    case 11:  /* MKNOD */
    case 12:  /* REMOVE */
    case 13:  /* RMDIR */
    case 14:  /* RENAME */
    case 15:  /* LINK */
    case 21:  /* COMMIT */
        xdrw_accept(w, RPC_SUCCESS);
        xdrw_u32(w, NFS3ERR_ROFS);
        return w->len;

    default:
        xdrw_accept(w, RPC_PROC_UNAVAIL);
        return w->len;
    }
}
