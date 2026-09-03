/*
 * Filesystem commands: ls, cat, cd, pwd, mkdir, rm, cp, mv, touch, df.
 */

#include <dirent.h>
#include <errno.h>
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

static int cmd_ls(espix_session_t *s, int argc, char **argv)
{
    bool        long_form = false;
    const char *target    = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_form = true;
        } else {
            target = argv[i];
        }
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
        if (long_form) {
            char when[20];
            char perms[11];
            ls_time(when, sizeof(when), st.st_mtime);
            espix_fs_mode_str(espix_fs_mode(abs, &st), false,
                              perms, sizeof(perms));
            espix_printf(s, "%s  %8ld  %12s  %s\n",
                         perms, (long)st.st_size, when, abs);
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

    unsigned count = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        count++;

        if (!long_form) {
            espix_printf(s, "%s\n", ent->d_name);
            continue;
        }

        char child[ESPIX_PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s",
                     (strcmp(abs, "/") == 0) ? "" : abs, ent->d_name)
            >= (int)sizeof(child)) {
            espix_printf(s, "?????????? %8s  %12s  %s\n", "-", "-", ent->d_name);
            continue;
        }

        struct stat cst;
        char        when[20];
        char        perms[11];

        if (stat(child, &cst) != 0) {
            espix_printf(s, "?????????? %8s  %12s  %s\n", "-", "-", ent->d_name);
            continue;
        }

        ls_time(when, sizeof(when), cst.st_mtime);

        const bool is_dir = S_ISDIR(cst.st_mode);
        espix_fs_mode_str(espix_fs_mode(child, &cst), is_dir,
                          perms, sizeof(perms));

        if (is_dir) {
            espix_printf(s, "%s  %8s  %12s  %s/\n",
                         perms, "-", when, ent->d_name);
        } else {
            espix_printf(s, "%s  %8ld  %12s  %s\n",
                         perms, (long)cst.st_size, when, ent->d_name);
        }
    }

    closedir(dir);

    if (long_form) {
        espix_printf(s, "%u entr%s\n", count, count == 1 ? "y" : "ies");
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
        } else {
            /* Only on the non-recursive path: espix_fs_rm_rf() drops the
             * overrides for everything it removes, since it is the only caller
             * that knows what those paths were. */
            espix_fs_mode_forget(abs);
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

    espix_fs_mode_rename(src, dst);
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
        if (rc == ESP_ERR_NO_MEM) {
            espix_printf(s, "chmod: %s: no room left in %s\n",
                         abs, ESPIX_MODES_PATH);
            status = 1;
        } else if (rc != ESP_OK) {
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
      .help = "list directory contents",         .usage = "ls [-l] [path]" },
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
