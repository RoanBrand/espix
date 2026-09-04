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
        espix_printf(s, "cd: %s: no such file or directory\n", abs);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        espix_printf(s, "cd: %s: not a directory\n", abs);
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
 * coreutils rounds up, and drops to one decimal only below 10: 1412 bytes is
 * "1.4K" and 20796 is "21K", not "20.3K". Matching that exactly matters more
 * than being arithmetically neat, because the point of -h is that the number
 * looks like the one every other tool would have printed.
 *
 * Integer arithmetic throughout. The obvious version wants doubles and ceil(),
 * which drags in libm for a column of a listing.
 */
static void ls_size(char *out, size_t len, off_t bytes, bool human)
{
    if (!human || bytes < 1024) {
        snprintf(out, len, "%ld", (long)bytes);
        return;
    }

    static const char units[] = { 'K', 'M', 'G' };
    uint64_t          div     = 1024;
    int               u       = 0;

    while ((uint64_t)bytes >= div * 1024 && u < 2) {
        div *= 1024;
        u++;
    }

    /* Tenths, rounded up -- never report less than the file holds. */
    const uint64_t tenths = ((uint64_t)bytes * 10 + div - 1) / div;

    if (tenths < 100) {
        snprintf(out, len, "%u.%u%c", (unsigned)(tenths / 10),
                 (unsigned)(tenths % 10), units[u]);
    } else {
        snprintf(out, len, "%u%c", (unsigned)((tenths + 9) / 10), units[u]);
    }
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
    bool   is_dir;
    bool   statted;    /* false renders the ?????????? row */
} ls_entry_t;

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
                espix_printf(s, "ls: unknown option '-%c'\n" LS_USAGE, *p);
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
        espix_printf(s, "ls: %s: no such file or directory\n", abs);
        return 1;
    }

    /* A plain file argument just describes itself. */
    if (!S_ISDIR(st.st_mode)) {
        if (f.long_form) {
            char when[20];
            char perms[11];
            char size[16];
            ls_time(when, sizeof(when), st.st_mtime);
            ls_size(size, sizeof(size), st.st_size, f.human);
            espix_fs_mode_str(espix_fs_mode(abs, &st), false,
                              perms, sizeof(perms));
            espix_printf(s, "%s  %8s  %12s  %s\n", perms, size, when, abs);
        } else {
            espix_printf(s, "%s\n", abs);
        }
        return 0;
    }

    DIR *dir = opendir(abs);
    if (dir == NULL) {
        espix_printf(s, "ls: %s: cannot open\n", abs);
        return 1;
    }

    /*
     * A stat costs a metadata read and the mode may open the file to sniff the
     * ELF magic, so neither is done for a plain listing of names. -t needs the
     * mtime to sort on even without -l.
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

            if (f.long_form) {
                e->mode = espix_fs_mode(child, &cst);
            }
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

    for (size_t i = 0; i < count; i++) {
        const ls_entry_t *e = &ents[i];

        if (!f.long_form) {
            espix_printf(s, "%s\n", e->name);
            continue;
        }
        if (!e->statted) {
            espix_printf(s, "?????????? %8s  %12s  %s\n", "-", "-", e->name);
            continue;
        }

        char when[20];
        char perms[11];
        char size[16];

        ls_time(when, sizeof(when), e->mtime);
        espix_fs_mode_str(e->mode, e->is_dir, perms, sizeof(perms));

        if (e->is_dir) {
            espix_printf(s, "%s  %8s  %12s  %s/\n", perms, "-", when, e->name);
        } else {
            ls_size(size, sizeof(size), e->size, f.human);
            espix_printf(s, "%s  %8s  %12s  %s\n", perms, size, when, e->name);
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(ents[i].name);
    }
    free(ents);

    if (truncated) {
        espix_printf(s, "ls: stopped at %u entries\n", (unsigned)count);
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
        espix_printf(s, "usage: cat <file>...\n");
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
            espix_printf(s, "cat: %s: %s\n", abs, strerror(errno));
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
        espix_printf(s, "usage: mkdir <dir>...\n");
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
            espix_printf(s, "mkdir: %s: %s\n", abs, strerror(errno));
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
        espix_printf(s, "usage: rm [-r] <path>...\n");
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
            espix_printf(s, "rm: refusing to remove /\n");
            status = 1;
            continue;
        }

        if (recursive) {
            const esp_err_t err = espix_fs_rm_rf(abs);
            if (err != ESP_OK) {
                espix_printf(s, "rm: %s: %s\n", abs, esp_err_to_name(err));
                status = 1;
            }
        } else if (unlink(abs) != 0) {
            espix_printf(s, "rm: %s: %s\n", abs, strerror(errno));
            status = 1;
        }
    }

    return status;
}

static int cmd_cp(espix_session_t *s, int argc, char **argv)
{
    if (argc != 3) {
        espix_printf(s, "usage: cp <src> <dst>\n");
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
        espix_printf(s, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }

    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        espix_printf(s, "cp: %s: %s\n", dst, strerror(errno));
        fclose(in);
        return 1;
    }

    char   chunk[COPY_CHUNK];
    size_t n;
    int    status = 0;

    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) {
            espix_printf(s, "cp: %s: write failed\n", dst);
            status = 1;
            break;
        }
    }

    fclose(in);
    fclose(out);
    return status;
}

static int cmd_mv(espix_session_t *s, int argc, char **argv)
{
    if (argc != 3) {
        espix_printf(s, "usage: mv <src> <dst>\n");
        return 1;
    }

    char src[ESPIX_PATH_MAX];
    char dst[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[1], src, sizeof(src)) ||
        !espix_cmd_path(s, argv[2], dst, sizeof(dst))) {
        return 1;
    }

    if (rename(src, dst) != 0) {
        espix_printf(s, "mv: %s -> %s: %s\n", src, dst, strerror(errno));
        return 1;
    }

    return 0;
}

static int cmd_touch(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_printf(s, "usage: touch <file>...\n");
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
            espix_printf(s, "touch: %s: %s\n", abs, strerror(errno));
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
            *err = "setuid, setgid and sticky are not supported";
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
        for (; *p != '\0' && *p != ','; p++) {
            switch (*p) {
            case 'r': bits |= 0444; break;
            case 'w': bits |= 0222; break;
            case 'x': bits |= 0111; break;
            case 's':
                *err = "setuid and setgid are not supported";
                return false;
            case 't':
                *err = "the sticky bit is not supported";
                return false;
            default:
                *err = "expected r, w or x";
                return false;
            }
        }

        bits &= who;

        if (op == '+') {
            mode |= bits;
        } else if (op == '-') {
            mode &= ~bits;
        } else {
            mode = (mode & ~who) | bits;
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
        espix_printf(s, "usage: chmod <mode> <path>...\n");
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
            espix_printf(s, "chmod: %s: %s\n", abs, strerror(errno));
            status = 1;
            continue;
        }

        mode_t      mode = 0;
        const char *err  = NULL;

        if (!chmod_parse(argv[1], espix_fs_mode(abs, &st), &mode, &err)) {
            espix_printf(s, "chmod: %s: %s\n", argv[1], err);
            return 1;           /* the mode is wrong for every path, not one */
        }

        const esp_err_t rc = espix_fs_chmod(abs, mode);
        if (rc != ESP_OK) {
            espix_printf(s, "chmod: %s: %s\n", abs, esp_err_to_name(rc));
            status = 1;
        }
    }

    return status;
}

static int cmd_df(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_fs_info_t info;
    if (espix_fs_stat_root(&info) != ESP_OK) {
        espix_printf(s, "df: rootfs not mounted\n");
        return 1;
    }

    const unsigned total_k = (unsigned)(info.total_bytes / 1024);
    const unsigned used_k  = (unsigned)(info.used_bytes / 1024);
    const unsigned pct     = (info.total_bytes > 0)
                             ? (unsigned)((info.used_bytes * 100) / info.total_bytes)
                             : 0;

    espix_printf(s, "%-12s %9s %9s %9s %5s %s\n",
                 "Filesystem", "1K-blocks", "Used", "Available", "Use%",
                 "Mounted on");
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
    espix_printf(s, "%-12s %9u %9u %9u %4u%% %s\n",
                 "littlefs", total_k, used_k, total_k - used_k, pct, "/");
    return 0;
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
    { .name = "df",    .fn = cmd_df,
      .help = "report filesystem usage",         .usage = "df" },
};

void espix_cmds_register_fs(void)
{
    espix_cmds_register_table(s_fs_cmds,
                             sizeof(s_fs_cmds) / sizeof(s_fs_cmds[0]));
}
