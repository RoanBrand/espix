/*
 * Filesystem commands: ls, cat, cd, pwd, mkdir, rm, cp, mv, touch, df.
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "espix_auth.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_shell.h"

#define COPY_CHUNK 512

static int cmd_pwd(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    espix_printf(s, "%s\n", s->cwd[0] != '\0' ? s->cwd : "/");
    return 0;
}

static int cmd_cd(espix_session_t *s, int argc, char **argv)
{
    char abs[ESPIX_PATH_MAX];

    /* Bare `cd` goes home, as everywhere else; `/` only for a session that has
     * no home, which is the console. */
    const char *target = (argc > 1)          ? argv[1]
                       : (s->home[0] != '\0') ? s->home
                                              : "/";

    if (!espix_cmd_path(s, target, abs, sizeof(abs))) {
        return 1;
    }

    struct stat st;
    if (stat(abs, &st) != 0) {
        espix_eprintf(s, "cd: %s: no such file or directory\n", abs);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        espix_eprintf(s, "cd: %s: not a directory\n", abs);
        return 1;
    }

    strlcpy(s->cwd, abs, sizeof(s->cwd));
    return 0;
}

/*
 * The date column for `ls -l`.
 *
 * LittleFS has stored an mtime on every write all along (it keeps one in a
 * custom attribute); espix simply never read it back until now. Which means
 * files written before the clock was set carry 1970 dates, and that is the
 * truth about them rather than a rendering bug -- see `timedatectl`.
 *
 * Recent files get "Aug 31 16:05" and older ones "Aug 31  2025", the same
 * six-month switch coreutils makes, because a time of day is what you want for
 * something touched today and a year for something that was not.
 */
static void ls_time(char *out, size_t len, time_t t)
{
    const time_t now = time(NULL);
    struct tm    tm;

    /*
     * A file with no mtime attribute at all -- everything in the flashed rootfs
     * image, because the image builder writes none. esp_littlefs reports that
     * as -1, which rendered as "Dec 31 1969" and read like a bug rather than
     * like the absence it is.
     */
    if (t <= 0) {
        strlcpy(out, "-", len);
        return;
    }

    localtime_r(&t, &tm);

    const bool recent = (t <= now) && (now - t < 180L * 24 * 3600);
    strftime(out, len, recent ? "%b %e %H:%M" : "%b %e  %Y", &tm);
}

/*
 * The size column, plain or -h.
 *
 * The formatting itself lives in espix_cmd_size(), beside every other command's
 * shared helpers, because `lsblk` reports sizes too and two implementations of
 * "1.5M" drift.
 */
static void ls_size(char *out, size_t len, off_t bytes, bool human)
{
    espix_cmd_size(out, len, (uint64_t)(bytes < 0 ? 0 : bytes), human);
}

/*
 * One directory entry, held rather than printed, because sorting needs the
 * whole directory before any of it can be shown.
 *
 * The name is strdup'd rather than a fixed array: `struct dirent` carries
 * d_name[256] while LittleFS caps a name at 64, so an inline copy would spend
 * four times what the names actually cost. Same reasoning as the override table
 * in espix_fs/mode.c.
 */
typedef struct {
    char  *name;
    time_t mtime;
    off_t  size;
    mode_t mode;
    uint16_t uid;
    uint16_t gid;
    bool   is_dir;
    bool   statted;    /* false renders the ?????????? row */
} ls_entry_t;

/*
 * The name for `id`, or the number when nothing claims it.
 *
 * `is_group` picks which file to look in. It matters: a gid and a uid are
 * different namespaces, and resolving a group through the account table was
 * only ever right while every gid equalled its uid.
 *
 * Copied out rather than returned by pointer: espix_auth answers from a
 * single-entry cache, so asking about the group would move what the owner
 * pointer refers to.
 */
static void ls_id_name(uint16_t id, bool is_group, char *out, size_t len)
{
    const char *name = is_group ? espix_auth_group_name(id)
                                : espix_auth_name_for_uid(id);
    if (name != NULL) {
        strlcpy(out, name, len);
    } else {
        snprintf(out, len, "%u", (unsigned)id);
    }
}

/*
 * The four variable-width columns of a long listing, for one entry.
 *
 * One function because the listing is measured before it is printed and the
 * two passes must agree exactly: a column measured from one rendering and
 * filled from another is a misalignment waiting for the first entry where they
 * differ.
 *
 * An unstatted entry answers "-" in all four rather than being special-cased at
 * the call sites. It has no owner, size or date to report, and "-" is what the
 * listing already showed for a directory's size.
 */
static void ls_cols(const ls_entry_t *e, bool human,
                    char *owner, size_t owner_len,
                    char *group, size_t group_len,
                    char *size,  size_t size_len,
                    char *when,  size_t when_len)
{
    if (!e->statted) {
        strlcpy(owner, "-", owner_len);
        strlcpy(group, "-", group_len);
        strlcpy(size,  "-", size_len);
        strlcpy(when,  "-", when_len);
        return;
    }

    ls_id_name(e->uid, false, owner, owner_len);
    ls_id_name(e->gid, true,  group, group_len);
    ls_time(when, when_len, e->mtime);

    /* A directory's size is whatever littlefs spends on the entry itself, which
     * is not what anyone reading `ls -l` is asking about. */
    if (e->is_dir) {
        strlcpy(size, "-", size_len);
    } else {
        ls_size(size, size_len, e->size, human);
    }
}

/* Grow a column to fit `text`. */
static void ls_widen(int *width, const char *text)
{
    const int n = (int)strlen(text);

    if (n > *width) {
        *width = n;
    }
}

/* Ceiling on entries held at once. Said out loud when reached rather than
 * dropped quietly -- `ps` silently losing its ninth process is already a
 * known issue and is not worth repeating here. */
#define LS_ENTRIES_MAX 512

/*
 * Two comparators rather than one that reads the flags, and -r reverses the
 * sorted array instead of inverting the comparison.
 *
 * qsort() passes no context, so a flag-aware comparator needs file-scope state
 * -- and the console and an SSH session can both be inside `ls` at once, which
 * makes that a data race. qsort_r() would solve it and is not dependably in
 * picolibc. Two context-free comparators and a reverse loop need neither.
 */
static int ls_cmp_name(const void *a, const void *b)
{
    return strcmp(((const ls_entry_t *)a)->name, ((const ls_entry_t *)b)->name);
}

static int ls_cmp_mtime(const void *a, const void *b)
{
    const ls_entry_t *x = a;
    const ls_entry_t *y = b;

    if (x->mtime != y->mtime) {
        return (y->mtime > x->mtime) ? 1 : -1;      /* newest first */
    }
    return strcmp(x->name, y->name);                /* stable, and readable */
}

typedef struct {
    bool long_form;
    bool all;
    bool human;
    bool by_time;
    bool reverse;
} ls_flags_t;

#define LS_USAGE "usage: ls [-1ahltr] [path]\n"

/*
 * Flags, bundled ("-lah") or separate, in any order. Anything that is not a
 * flag is the path.
 *
 * An unknown letter is an error rather than a path. It used to be the latter:
 * every argument except "-l" was taken as the operand, so `ls -z` reported
 * "no such file or directory: /-z" and left you looking at the filesystem.
 */
static bool ls_parse(espix_session_t *s, int argc, char **argv,
                     ls_flags_t *f, const char **target)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            *target = argv[i];
            continue;
        }

        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            switch (*p) {
            case 'l': f->long_form = true; break;
            case 'a': f->all       = true; break;
            case 'h': f->human     = true; break;
            case 't': f->by_time   = true; break;
            case 'r': f->reverse   = true; break;

            /* Already the only behaviour: output is one entry per line, and
             * there is no terminal width in espix_session_t to columnate
             * against. Accepted so a script that says -1 works. */
            case '1': break;

            default:
                espix_eprintf(s, "ls: unknown option '-%c'\n" LS_USAGE, *p);
                return false;
            }
        }
    }
    return true;
}

static int cmd_ls(espix_session_t *s, int argc, char **argv)
{
    ls_flags_t  f      = { 0 };
    const char *target = NULL;

    if (!ls_parse(s, argc, argv, &f, &target)) {
        return 1;
    }

    char abs[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, target, abs, sizeof(abs))) {
        return 1;
    }

    struct stat st;
    if (stat(abs, &st) != 0) {
        espix_eprintf(s, "ls: %s: no such file or directory\n", abs);
        return 1;
    }

    /* A plain file argument just describes itself.
     *
     * With owner and group, which this form used to leave out -- so `ls -l
     * /etc/passwd` and `ls -l /etc` described the same file differently, and
     * the one you reach for when you care about a single file was the one
     * missing who owns it. Widths are the strings' own here: there is one row,
     * so there is nothing to line it up against. */
    if (!S_ISDIR(st.st_mode)) {
        if (f.long_form) {
            char when[20];
            char perms[11];
            char size[16];
            char owner[ESPIX_USER_MAX];
            char group[ESPIX_USER_MAX];
            uint16_t uid = 0;
            uint16_t gid = 0;

            espix_fs_owner(abs, &st, &uid, &gid);
            ls_id_name(uid, false, owner, sizeof(owner));
            ls_id_name(gid, true,  group, sizeof(group));
            ls_time(when, sizeof(when), st.st_mtime);
            ls_size(size, sizeof(size), st.st_size, f.human);
            /* st.st_mode, not espix_fs_mode(): vfs_stat() has already folded
             * the rule's permission bits in, and espix_fs_mode() answers the
             * permissions alone -- passing it here would drop S_ISCHR and draw
             * every device as an ordinary file. */
            espix_fs_mode_str(st.st_mode, false, perms, sizeof(perms));
            espix_printf(s, "%s %s %s %s %s %s\n",
                         perms, owner, group, size, when, abs);
        } else {
            espix_printf(s, "%s\n", abs);
        }
        return 0;
    }

    DIR *dir = opendir(abs);
    if (dir == NULL) {
        espix_eprintf(s, "ls: %s: cannot open\n", abs);
        return 1;
    }

    /*
     * A stat costs a metadata read, and it is where the mode comes from too --
     * espix's VFS fills st_mode in, so there is nothing extra to ask. Skipped
     * entirely for a plain listing of names; -t needs the mtime to sort on even
     * without -l.
     */
    const bool need_stat = f.long_form || f.by_time;

    ls_entry_t          *ents      = NULL;
    size_t               count     = 0;
    size_t               cap       = 0;
    bool                 truncated = false;
    const struct dirent *ent;

    /*
     * No check for "." and ".." here, and none is possible: esp_littlefs's
     * readdir skips them itself, in a loop that reads until it gets what it
     * calls "a real object name". The guard this code used to carry could never
     * fire. It is also why -a cannot show them -- see KNOWN-ISSUES.md.
     */
    while ((ent = readdir(dir)) != NULL) {
        if (!f.all && ent->d_name[0] == '.') {
            continue;
        }
        if (count == LS_ENTRIES_MAX) {
            truncated = true;
            break;
        }

        if (count == cap) {
            const size_t want  = (cap == 0) ? 16 : cap * 2;
            ls_entry_t  *grown = realloc(ents, want * sizeof(*ents));

            if (grown == NULL) {
                truncated = true;
                break;
            }
            ents = grown;
            cap  = want;
        }

        ls_entry_t *e = &ents[count];
        memset(e, 0, sizeof(*e));

        e->name = strdup(ent->d_name);
        if (e->name == NULL) {
            truncated = true;
            break;
        }
        e->is_dir = (ent->d_type == DT_DIR);
        count++;

        if (!need_stat) {
            continue;
        }

        char        child[ESPIX_PATH_MAX];
        struct stat cst;

        if (snprintf(child, sizeof(child), "%s/%s",
                     (strcmp(abs, "/") == 0) ? "" : abs, ent->d_name)
                < (int)sizeof(child)
            && stat(child, &cst) == 0) {
            e->statted = true;
            e->mtime   = cst.st_mtime;
            e->size    = cst.st_size;
            e->is_dir  = S_ISDIR(cst.st_mode);

            e->mode = cst.st_mode;
            espix_fs_owner(child, &cst, &e->uid, &e->gid);
        }
    }

    closedir(dir);

    qsort(ents, count, sizeof(*ents),
          f.by_time ? ls_cmp_mtime : ls_cmp_name);

    if (f.reverse && count > 0) {
        for (size_t i = 0, j = count - 1; i < j; i++, j--) {
            const ls_entry_t tmp = ents[i];
            ents[i] = ents[j];
            ents[j] = tmp;
        }
    }

    /*
     * Column widths measured from the listing, then one pass to print it.
     *
     * They used to be fixed -- owner and group padded to ESPIX_USER_MAX
     * whatever they held, size to 8 when the widest entry was two digits, and
     * two spaces between several columns. On a directory of root-owned files
     * that is a third of the line spent on padding, and it is worst exactly
     * where there is least to say: a subdirectory prints "-" for both size and
     * date, right-aligned in eight and twelve columns, so the eye has to cross
     * a blank gap to reach the name.
     *
     * The old comment justified the fixed width as keeping columns aligned
     * "whether an id resolves to a name or prints as a number". Measuring does
     * that better -- it aligns on what is there rather than on what espix
     * allows -- and it is what ls has always done.
     *
     * ls_cols() is called twice per entry, once to measure and once to print,
     * rather than caching four strings per entry. The entry array is already
     * the memory ceiling here (LS_ENTRIES_MAX), and formatting a size and a
     * date twice is cheaper than carrying 50 bytes an entry to avoid it.
     */
    int w_owner = 1;
    int w_group = 1;
    int w_size  = 1;
    int w_when  = 1;

    if (f.long_form) {
        for (size_t i = 0; i < count; i++) {
            char owner[ESPIX_USER_MAX];
            char group[ESPIX_USER_MAX];
            char size[16];
            char when[20];

            ls_cols(&ents[i], f.human, owner, sizeof(owner), group,
                    sizeof(group), size, sizeof(size), when, sizeof(when));

            ls_widen(&w_owner, owner);
            ls_widen(&w_group, group);
            ls_widen(&w_size,  size);
            ls_widen(&w_when,  when);
        }
    }

    for (size_t i = 0; i < count; i++) {
        const ls_entry_t *e = &ents[i];

        if (!f.long_form) {
            espix_printf(s, "%s\n", e->name);
            continue;
        }

        char owner[ESPIX_USER_MAX];
        char group[ESPIX_USER_MAX];
        char size[16];
        char when[20];
        char perms[11];

        ls_cols(e, f.human, owner, sizeof(owner), group, sizeof(group),
                size, sizeof(size), when, sizeof(when));

        if (e->statted) {
            espix_fs_mode_str(e->mode, e->is_dir, perms, sizeof(perms));
        } else {
            strlcpy(perms, "??????????", sizeof(perms));
        }

        espix_printf(s, "%s %-*s %-*s %*s %*s %s%s\n",
                     perms, w_owner, owner, w_group, group,
                     w_size, size, w_when, when,
                     e->name, (e->statted && e->is_dir) ? "/" : "");
    }

    for (size_t i = 0; i < count; i++) {
        free(ents[i].name);
    }
    free(ents);

    if (truncated) {
        espix_eprintf(s, "ls: stopped at %u entries\n", (unsigned)count);
    }
    if (f.long_form) {
        espix_printf(s, "%u entr%s\n",
                     (unsigned)count, count == 1 ? "y" : "ies");
    }
    return 0;
}

static int cmd_cat(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: cat <file>...\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) {
            status = 1;
            continue;
        }

        FILE *f = fopen(abs, "rb");
        if (f == NULL) {
            espix_eprintf(s, "cat: %s: %s\n", abs, strerror(errno));
            status = 1;
            continue;
        }

        char   chunk[COPY_CHUNK + 1];
        size_t n;
        while ((n = fread(chunk, 1, COPY_CHUNK, f)) > 0) {
            chunk[n] = '\0';
            espix_puts(s, chunk);
        }
        fclose(f);
    }

    return status;
}

static int cmd_mkdir(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: mkdir <dir>...\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) {
            status = 1;
            continue;
        }
        if (mkdir(abs, 0755) != 0) {
            espix_eprintf(s, "mkdir: %s: %s\n", abs, strerror(errno));
            status = 1;
        }
    }

    return status;
}

static int cmd_rm(espix_session_t *s, int argc, char **argv)
{
    bool recursive = false;
    int  first     = 1;

    if (argc > 1 && (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "-rf") == 0)) {
        recursive = true;
        first = 2;
    }

    if (first >= argc) {
        espix_eprintf(s, "usage: rm [-r] <path>...\n");
        return 1;
    }

    int status = 0;

    for (int i = first; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) {
            status = 1;
            continue;
        }
        if (strcmp(abs, "/") == 0) {
            espix_eprintf(s, "rm: refusing to remove /\n");
            status = 1;
            continue;
        }

        if (recursive) {
            const esp_err_t err = espix_fs_rm_rf(abs);
            if (err != ESP_OK) {
                espix_eprintf(s, "rm: %s: %s\n", abs, esp_err_to_name(err));
                status = 1;
            }
        } else if (unlink(abs) != 0) {
            espix_eprintf(s, "rm: %s: %s\n", abs, strerror(errno));
            status = 1;
        }
    }

    return status;
}

/*
 * Read both files back and compare them, chunk for chunk.
 *
 * Not paranoia. A write into a mounted FAT volume has been measured arriving as
 * an empty file -- once in seven copies -- with every layer reporting success,
 * this command's fflush() and fclose() included: the transfer is lost somewhere
 * under FatFs without an error coming back up (KNOWN-ISSUES.md has the numbers).
 * Reading the copy back is therefore the only way to know it arrived.
 *
 * Buffers from the heap rather than the stack, because this runs on the SSH
 * connection task and that stack is the one that overflowed once already. An
 * allocation failure means "not verified", which is not the same as "wrong", so
 * it is said out loud and does not fail the copy.
 */
static bool copy_arrived(espix_session_t *s, const char *src, const char *dst)
{
    FILE *a = fopen(src, "rb");
    if (a == NULL) {
        return false;
    }
    FILE *b = fopen(dst, "rb");
    if (b == NULL) {
        fclose(a);
        return false;
    }

    char *ca = malloc(COPY_CHUNK);
    char *cb = malloc(COPY_CHUNK);
    if (ca == NULL || cb == NULL) {
        free(ca);
        free(cb);
        fclose(a);
        fclose(b);
        espix_eprintf(s, "cp: %s: cannot verify (out of memory)\n", dst);
        return true;
    }

    bool same = true;
    while (same) {
        const size_t na = fread(ca, 1, COPY_CHUNK, a);
        const size_t nb = fread(cb, 1, COPY_CHUNK, b);

        if (na != nb || (na > 0 && memcmp(ca, cb, na) != 0)) {
            same = false;
        } else if (na < COPY_CHUNK) {
            break;                      /* both ended, at the same place */
        }
    }

    free(ca);
    free(cb);
    fclose(a);
    fclose(b);
    return same;
}

static int cmd_cp(espix_session_t *s, int argc, char **argv)
{
    if (argc != 3) {
        espix_eprintf(s, "usage: cp <src> <dst>\n");
        return 1;
    }

    char src[ESPIX_PATH_MAX];
    char dst[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[1], src, sizeof(src)) ||
        !espix_cmd_path(s, argv[2], dst, sizeof(dst))) {
        return 1;
    }

    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        espix_eprintf(s, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }

    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        espix_eprintf(s, "cp: %s: %s\n", dst, strerror(errno));
        fclose(in);
        return 1;
    }

    char   chunk[COPY_CHUNK];
    size_t n;
    int    status = 0;

    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) {
            espix_eprintf(s, "cp: %s: write failed: %s\n", dst, strerror(errno));
            status = 1;
            break;
        }
    }

    fclose(in);

    /*
     * The flush is where the bytes actually go: stdio buffers them, so on a
     * mounted volume the real write happens here and not in fwrite(). Checking
     * fwrite() alone therefore reports success for a copy that fails on close --
     * which is how fifteen bytes became a 0-byte file with nothing said.
     */
    if (status == 0 && fflush(out) != 0) {
        espix_eprintf(s, "cp: %s: write failed: %s\n", dst, strerror(errno));
        status = 1;
    }
    if (status == 0 && fclose(out) != 0) {
        espix_eprintf(s, "cp: %s: close failed: %s\n", dst, strerror(errno));
        status = 1;
    }

    /*
     * Then look at what arrived. Every layer above has now reported success, and
     * a copy has been measured arriving as an empty file anyway -- so this is the
     * check that would have caught it, rather than the operator noticing later.
     */
    if (status == 0 && !copy_arrived(s, src, dst)) {
        espix_eprintf(s, "cp: %s: the copy did not arrive intact\n", dst);
        status = 1;
    }

    return status;
}

static int cmd_mv(espix_session_t *s, int argc, char **argv)
{
    if (argc != 3) {
        espix_eprintf(s, "usage: mv <src> <dst>\n");
        return 1;
    }

    char src[ESPIX_PATH_MAX];
    char dst[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[1], src, sizeof(src)) ||
        !espix_cmd_path(s, argv[2], dst, sizeof(dst))) {
        return 1;
    }

    if (rename(src, dst) != 0) {
        espix_eprintf(s, "mv: %s -> %s: %s\n", src, dst, strerror(errno));
        return 1;
    }

    return 0;
}

static int cmd_touch(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: touch <file>...\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) {
            status = 1;
            continue;
        }

        FILE *f = fopen(abs, "ab");
        if (f == NULL) {
            espix_eprintf(s, "touch: %s: %s\n", abs, strerror(errno));
            status = 1;
            continue;
        }
        fclose(f);
    }

    return status;
}

/*
 * Parse one chmod spec against the current mode.
 *
 * Octal ("644") or symbolic ("+x", "u-w", "go=rx", and comma-separated clauses
 * of those). On failure *err names the problem, because "chmod: invalid mode"
 * for four different mistakes is how you end up debugging your own shell.
 */
/* The words strerror() would use, for the espix_fs codes these commands see. */
static const char *fs_err(esp_err_t rc)
{
    switch (rc) {
    case ESP_ERR_NOT_FOUND:   return "no such file";
    case ESP_ERR_NOT_ALLOWED: return "operation not permitted";
    case ESP_ERR_INVALID_ARG: return "invalid argument";
    default:                  return esp_err_to_name(rc);
    }
}

static bool chmod_parse(const char *spec, mode_t cur, mode_t *out,
                        const char **err)
{
    if (spec[0] == '\0') {
        *err = "empty mode";
        return false;
    }

    /*
     * Anything starting with a digit is meant as octal, so it is diagnosed as
     * octal even when it is not valid -- "chmod 999" reporting a symbolic
     * syntax error would send you looking in the wrong place.
     */
    if (spec[0] >= '0' && spec[0] <= '9') {
        char      *end = NULL;
        const long v   = strtol(spec, &end, 8);

        if (*end != '\0' || v < 0) {
            *err = "not an octal mode (digits 0-7 only)";
            return false;
        }
        if (v > ESPIX_MODE_BITS) {
            *err = "mode out of range (at most four octal digits)";
            return false;
        }
        *out = (mode_t)v;
        return true;
    }

    mode_t mode = cur;
    const char *p = spec;

    for (;;) {
        mode_t who = 0;

        for (; *p != '\0' && strchr("ugoa", *p) != NULL; p++) {
            switch (*p) {
            case 'u': who |= 0700; break;
            case 'g': who |= 0070; break;
            case 'o': who |= 0007; break;
            default:  who |= 0777; break;
            }
        }
        if (who == 0) {
            who = 0777;          /* a bare "+x" means all three classes */
        }

        const char op = *p;
        if (op != '+' && op != '-' && op != '=') {
            *err = "expected +, - or = after u/g/o/a";
            return false;
        }
        p++;

        mode_t bits = 0;
        mode_t high = 0;        /* setuid/setgid/sticky: outside the triads */

        for (; *p != '\0' && *p != ','; p++) {
            switch (*p) {
            case 'r': bits |= 0444; break;
            case 'w': bits |= 0222; break;
            case 'x': bits |= 0111; break;
            case 's':
                /* Which of the two `s` means is decided by the class named:
                 * `u+s` is setuid, `g+s` is setgid, `a+s` is both. */
                if (who & 0700) high |= S_ISUID;
                if (who & 0070) high |= S_ISGID;
                break;
            case 't':
                /* Written `+t` far more often than `o+t`, and it belongs to the
                 * directory rather than to a class, so the class is ignored. */
                high |= S_ISVTX;
                break;
            default:
                *err = "expected r, w, x, s or t";
                return false;
            }
        }

        bits &= who;

        /* `=` replaces the named classes outright, high bits included, or
         * `u=rw` would leave a setuid behind that nobody asked to keep. */
        mode_t who_high = 0;
        if (who & 0700) who_high |= S_ISUID;
        if (who & 0070) who_high |= S_ISGID;
        if (who & 0007) who_high |= S_ISVTX;

        if (op == '+') {
            mode |= bits | high;
        } else if (op == '-') {
            mode &= ~(bits | high);
        } else {
            mode = (mode & ~(who | who_high)) | bits | high;
        }

        if (*p != ',') {
            break;
        }
        p++;
    }

    *out = mode & ESPIX_MODE_BITS;
    return true;
}

static int cmd_chmod(espix_session_t *s, int argc, char **argv)
{
    if (argc < 3) {
        espix_eprintf(s, "usage: chmod <mode> <path>...\n");
        return 1;
    }

    int status = 0;

    for (int i = 2; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) {
            status = 1;
            continue;
        }

        struct stat st;
        if (stat(abs, &st) != 0) {
            espix_eprintf(s, "chmod: %s: %s\n", abs, strerror(errno));
            status = 1;
            continue;
        }

        mode_t      mode = 0;
        const char *err  = NULL;

        if (!chmod_parse(argv[1], st.st_mode & ESPIX_MODE_BITS, &mode, &err)) {
            espix_eprintf(s, "chmod: %s: %s\n", argv[1], err);
            return 1;           /* the mode is wrong for every path, not one */
        }

        /*
         * espix acts on all twelve bits, but not on every bit for every kind of
         * file. The combinations it would have to store and ignore are refused
         * by name here rather than silently masked, which is the same call the
         * octal range makes -- being told beats reading the mode back later and
         * finding it did nothing.
         *
         * This is checked per path, not once, because the same `chmod +t` is
         * right for a directory and meaningless for a file beside it.
         */
        const bool is_dir = S_ISDIR(st.st_mode);

        if (is_dir && (mode & (S_ISUID | S_ISGID))) {
            espix_eprintf(s, "chmod: %s: setuid and setgid on a directory mean "
                            "group inheritance, which espix does not "
                            "implement\n", abs);
            status = 1;
            continue;
        }
        if (!is_dir && (mode & S_ISVTX)) {
            espix_eprintf(s, "chmod: %s: the sticky bit only applies to a "
                            "directory\n", abs);
            status = 1;
            continue;
        }

        const esp_err_t rc = espix_fs_chmod(abs, mode);
        if (rc != ESP_OK) {
            espix_eprintf(s, "chmod: %s: %s\n", abs, fs_err(rc));
            status = 1;
        }
    }

    return status;
}

#define DF_USAGE "usage: df [-h]\n"

/*
 * Flags parsed rather than ignored, which is the whole of the change here.
 *
 * This used to be `(void)argc; (void)argv;`, so `df -h` printed 1K blocks and
 * exited 0 -- and so did `df -Z`. Silently accepting a flag you do not
 * implement is worse than rejecting it: the caller reads the answer believing
 * it is the answer to what they asked. ls_parse() above already had the right
 * shape for this; the only difference is that df takes no operand.
 */
static int cmd_df(espix_session_t *s, int argc, char **argv)
{
    bool human = false;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            /* There is one filesystem and df already names it. An operand is a
             * misunderstanding worth correcting rather than ignoring. */
            espix_eprintf(s, "df: %s: df reports the root filesystem and takes "
                             "no operand\n" DF_USAGE, argv[i]);
            return 1;
        }
        for (const char *c = argv[i] + 1; *c != '\0'; c++) {
            switch (*c) {
            case 'h': human = true; break;
            default:
                espix_eprintf(s, "df: unknown option '-%c'\n" DF_USAGE, *c);
                return 1;
            }
        }
    }

    espix_fs_info_t info;
    if (espix_fs_stat_root(&info) != ESP_OK) {
        espix_eprintf(s, "df: rootfs not mounted\n");
        return 1;
    }

    const unsigned total_k = (unsigned)(info.total_bytes / 1024);
    const unsigned used_k  = (unsigned)(info.used_bytes / 1024);
    const unsigned pct     = (info.total_bytes > 0)
                             ? (unsigned)((info.used_bytes * 100) / info.total_bytes)
                             : 0;

    /* ls_size() rather than a second formatter: -h means the same thing in both
     * commands, and two implementations of "1.5M" drift. */
    char c_total[16];
    char c_used[16];
    char c_avail[16];

    if (human) {
        ls_size(c_total, sizeof(c_total), (off_t)info.total_bytes, true);
        ls_size(c_used,  sizeof(c_used),  (off_t)info.used_bytes,  true);
        ls_size(c_avail, sizeof(c_avail),
                (off_t)(info.total_bytes - info.used_bytes), true);
    } else {
        snprintf(c_total, sizeof(c_total), "%u", total_k);
        snprintf(c_used,  sizeof(c_used),  "%u", used_k);
        snprintf(c_avail, sizeof(c_avail), "%u", total_k - used_k);
    }

    /* "Size" under -h, as GNU df does: the numbers are no longer 1K blocks and
     * a header that says they are would be the same lie in a smaller place. */
    espix_printf(s, "%-12s %9s %9s %9s %5s %s\n",
                 "Filesystem", human ? "Size" : "1K-blocks", "Used",
                 "Available", "Use%", "Mounted on");
    /*
     * Still "littlefs" on "/", and both halves are still true even though `/`
     * is served by espix's own VFS now: that layer holds no storage, it holds
     * the name and the policy. The filesystem underneath really is LittleFS.
     * (Linux shows `overlay` for a stacking filesystem, but overlayfs has an
     * upper layer with storage of its own; espix's does not.)
     *
     * espix_fs_stat_root() resolves by partition label rather than by path, so
     * it never noticed the change.
     */
    espix_printf(s, "%-12s %9s %9s %9s %4u%% %s\n",
                 "littlefs", c_total, c_used, c_avail, pct, "/");
    return 0;
}

/*
 * Resolve "esp", "1000" or "" into a uid.
 *
 * A bare number is taken as an id even when no account claims it, which is what
 * lets ownership be handed to a user that does not exist yet -- the same
 * latitude chown(1) gives, and the reason an unknown *name* is still an error:
 * a typo should not silently become "leave it alone".
 */
static bool id_of(espix_session_t *s, const char *who, const char *cmd,
                  bool is_group, uint16_t *out)
{
    if (who[0] == '\0') {
        *out = ESPIX_FS_KEEP_ID;
        return true;
    }

    char *end = NULL;
    const unsigned long n = strtoul(who, &end, 10);
    if (*end == '\0' && end != who) {
        if (n > UINT16_MAX) {
            espix_eprintf(s, "%s: %s: id out of range\n", cmd, who);
            return false;
        }
        *out = (uint16_t)n;
        return true;
    }

    /* The two are different namespaces. Resolving a group through the account
     * table was only ever right while every gid equalled its uid. */
    if (is_group) {
        if (!espix_auth_group_id(who, out)) {
            espix_eprintf(s, "%s: %s: no such group\n", cmd, who);
            return false;
        }
        return true;
    }

    espix_user_t account;
    if (espix_auth_lookup(who, &account) != ESP_OK) {
        espix_eprintf(s, "%s: %s: no such user\n", cmd, who);
        return false;
    }
    *out = account.uid;
    return true;
}

/*
 * chown [user][:group] <path>..., and chgrp through the same function.
 *
 * Nothing here checks for root. It does not have to: espix_fs_chown() writes
 * through the same filesystem as everything else, so the permission check on
 * the path is what refuses an unprivileged caller. One place to be right.
 */
static int cmd_chown(espix_session_t *s, int argc, char **argv)
{
    const bool is_chgrp = (strcmp(argv[0], "chgrp") == 0);

    if (argc < 3) {
        espix_eprintf(s, "usage: %s\n",
                     is_chgrp ? "chgrp <group> <path>..."
                              : "chown <user>[:<group>] <path>...");
        return 1;
    }

    uint16_t uid = ESPIX_FS_KEEP_ID;
    uint16_t gid = ESPIX_FS_KEEP_ID;

    if (is_chgrp) {
        if (!id_of(s, argv[1], argv[0], true, &gid)) {
            return 1;
        }
    } else {
        char spec[ESPIX_USER_MAX * 2 + 2];
        strlcpy(spec, argv[1], sizeof(spec));

        char *colon = strchr(spec, ':');
        if (colon != NULL) {
            *colon = '\0';
            if (!id_of(s, colon + 1, argv[0], true, &gid)) {
                return 1;
            }
        }
        if (!id_of(s, spec, argv[0], false, &uid)) {
            return 1;
        }
        /*
         * `chown user file` moves the group to that user's *primary* group, the
         * way chown(1) does. Taking the uid worked only while every account's
         * gid equalled it; now that groups are real, the account record is the
         * only thing that knows.
         */
        if (colon == NULL && uid != ESPIX_FS_KEEP_ID) {
            espix_user_t account;
            gid = (espix_auth_lookup(spec, &account) == ESP_OK) ? account.gid
                                                                : uid;
        }
    }

    int status = 0;

    for (int i = 2; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) {
            status = 1;
            continue;
        }

        const esp_err_t rc = espix_fs_chown(abs, uid, gid);
        if (rc != ESP_OK) {
            espix_printf(s, "%s: %s: %s\n", argv[0], abs, fs_err(rc));
            status = 1;
        }
    }

    return status;
}

static espix_cmd_t s_fs_cmds[] = {
    { .name = "pwd",   .fn = cmd_pwd,
      .help = "print the working directory",     .usage = "pwd" },
    { .name = "cd",    .fn = cmd_cd,
      .help = "change the working directory",    .usage = "cd [dir]" },
    { .name = "ls",    .fn = cmd_ls,
      .help = "list directory contents",         .usage = "ls [-1ahltr] [path]" },
    { .name = "cat",   .fn = cmd_cat,
      .help = "print files",                     .usage = "cat <file>..." },
    { .name = "mkdir", .fn = cmd_mkdir,
      .help = "create directories",              .usage = "mkdir <dir>..." },
    { .name = "rm",    .fn = cmd_rm,
      .help = "remove files or directories",     .usage = "rm [-r] <path>..." },
    { .name = "cp",    .fn = cmd_cp,
      .help = "copy a file",                     .usage = "cp <src> <dst>" },
    { .name = "mv",    .fn = cmd_mv,
      .help = "move or rename a file",           .usage = "mv <src> <dst>" },
    { .name = "touch", .fn = cmd_touch,
      .help = "create empty files",              .usage = "touch <file>..." },
    { .name = "chmod", .fn = cmd_chmod,
      .help = "change file mode bits",           .usage = "chmod <mode> <path>..." },
    { .name = "chown", .fn = cmd_chown,
      .help = "change file owner",               .usage = "chown <user>[:<group>] <path>..." },
    { .name = "chgrp", .fn = cmd_chown,
      .help = "change file group",               .usage = "chgrp <group> <path>..." },
    { .name = "df",    .fn = cmd_df,
      .help = "report filesystem usage",         .usage = "df [-h]" },
};

void espix_cmds_register_fs(void)
{
    espix_cmds_register_table(s_fs_cmds,
                             sizeof(s_fs_cmds) / sizeof(s_fs_cmds[0]));
}
