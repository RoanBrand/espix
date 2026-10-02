/*
 * Text utilities: true/false, wc, head, tail, grep, sort, find, test, sleep.
 *
 * File arguments only, for now: reading standard input is R-P2.4 (redirection
 * and stdin for builtins) and a pipe is R-P2.5, so a utility with no file
 * argument says so rather than blocking on a stream nothing can fill yet.
 *
 * grep matches fixed strings, which is the row's first step; a regex engine is
 * a later item. Its exit status matters as much as its output -- 0 when a line
 * matched, 1 when none did, 2 on error -- because that is what makes
 * `grep x f && ...` mean anything.
 *
 * Lines are read into a fixed 256-byte buffer, so a line longer than that is
 * seen as two; that is the one thing here that is a simplification rather than
 * a decision, and it is why tail and sort copy each line to the heap instead of
 * holding a pointer into it.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_cmds_priv.h"

#define TEXT_LINE_MAX 256

/* Resolve `arg` against the session's cwd and open it for reading. NULL with a
 * message already printed on failure, so every caller's error path is one
 * line. */
static FILE *espix_cmd_open(espix_session_t *s, const char *who, const char *arg,
                            char *abs, size_t abs_len)
{
    if (!espix_cmd_path(s, arg, abs, abs_len)) {
        return NULL;
    }
    FILE *f = fopen(abs, "rb");
    if (f == NULL) {
        espix_eprintf(s, "%s: %s: %s\n", who, arg, strerror(errno));
        return NULL;
    }
    return f;
}

/*
 * The stream a command with no file operands reads from: the shell's `<`
 * target, or NULL when there is none. A builtin has no other standard input --
 * it runs on the session task, not as a process -- so a command with neither a
 * file nor a `<` says so rather than blocking. See R-P2.4.
 */
static FILE *stdin_stream(espix_session_t *s)
{
    return (s != NULL) ? s->redirect_in : NULL;
}

/* ------------------------------------------------------------------ */
/* true, false                                                         */
/* ------------------------------------------------------------------ */

static int cmd_true(espix_session_t *s, int argc, char **argv)
{
    (void)s; (void)argc; (void)argv;
    return 0;
}

static int cmd_false(espix_session_t *s, int argc, char **argv)
{
    (void)s; (void)argc; (void)argv;
    return 1;
}

/* ------------------------------------------------------------------ */
/* wc                                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long lines, words, bytes;
} wc_t;

static void wc_count(espix_session_t *s, FILE *f, wc_t *out)
{
    wc_t  c = { 0, 0, 0 };
    char  buf[256];
    bool  in_word = false;
    size_t n;

    while (!espix_shell_stopping(s) &&
           (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        c.bytes += n;
        for (size_t i = 0; i < n; i++) {
            const char ch = buf[i];
            if (ch == '\n') {
                c.lines++;
            }
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                c.words++;
            }
        }
    }
    *out = c;
}

static void wc_print(espix_session_t *s, const wc_t *c, bool l, bool w, bool b,
                     const char *name)
{
    if (l) { espix_printf(s, "%8lu", c->lines); }
    if (w) { espix_printf(s, "%8lu", c->words); }
    if (b) { espix_printf(s, "%8lu", c->bytes); }
    if (name != NULL) { espix_printf(s, " %s", name); }
    espix_printf(s, "\n");
}

static int cmd_wc(espix_session_t *s, int argc, char **argv)
{
    bool l = false, w = false, b = false;
    int  first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        for (const char *p = argv[first] + 1; *p != '\0'; p++) {
            switch (*p) {
            case 'l': l = true; break;
            case 'w': w = true; break;
            case 'c': b = true; break;
            default:
                espix_eprintf(s, "wc: unknown option -%c\n", *p);
                return 1;
            }
        }
        first++;
    }
    if (!l && !w && !b) { l = w = b = true; }

    if (first >= argc) {
        FILE *f = stdin_stream(s);
        if (f == NULL) {
            espix_eprintf(s, "wc: no input; use <file>\n");
            return 1;
        }
        wc_t c;
        wc_count(s, f, &c);
        wc_print(s, &c, l, w, b, NULL);
        return 0;
    }

    wc_t       total = { 0, 0, 0 };
    const bool many  = (argc - first) > 1;
    int        rc    = 0;

    for (int i = first; i < argc; i++) {
        char  abs[ESPIX_PATH_MAX];
        FILE *f = espix_cmd_open(s, "wc", argv[i], abs, sizeof(abs));
        if (f == NULL) { rc = 1; continue; }

        wc_t c;
        wc_count(s, f, &c);
        fclose(f);

        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
        wc_print(s, &c, l, w, b, argv[i]);
    }

    if (many) { wc_print(s, &total, l, w, b, "total"); }
    return rc;
}

/* ------------------------------------------------------------------ */
/* head, tail                                                           */
/* ------------------------------------------------------------------ */

/* The count from `-n N` or `-nN`, defaulting to 10, and the index of the
 * first operand. -1 on a bad option, with the message already printed. */
static int line_count(espix_session_t *s, const char *who, int argc,
                      char **argv, long *out)
{
    int first = 1;
    *out = 10;

    if (first < argc && strncmp(argv[first], "-n", 2) == 0) {
        const char *v = argv[first] + 2;
        if (*v == '\0' && first + 1 < argc) {
            v = argv[++first];
        }
        if (*v == '\0') {
            espix_eprintf(s, "%s: -n needs a count\n", who);
            return -1;
        }
        char      *end = NULL;
        const long n   = strtol(v, &end, 10);
        if (end == v || *end != '\0' || n < 0) {
            espix_eprintf(s, "%s: bad count '%s'\n", who, v);
            return -1;
        }
        *out = n;
        first++;
    }
    return first;
}

static void head_stream(espix_session_t *s, FILE *f, long n)
{
    char line[TEXT_LINE_MAX];
    long shown = 0;

    while (shown < n && !espix_shell_stopping(s) &&
           fgets(line, sizeof(line), f) != NULL) {
        espix_puts(s, line);
        shown++;
    }
}

static int cmd_head(espix_session_t *s, int argc, char **argv)
{
    long      n     = 0;
    const int first = line_count(s, "head", argc, argv, &n);
    if (first < 0) { return 2; }

    if (first >= argc) {
        FILE *f = stdin_stream(s);
        if (f == NULL) {
            espix_eprintf(s, "head: no input; use <file>\n");
            return 1;
        }
        head_stream(s, f, n);
        return 0;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        char  abs[ESPIX_PATH_MAX];
        FILE *f = espix_cmd_open(s, "head", argv[i], abs, sizeof(abs));
        if (f == NULL) { rc = 1; continue; }
        head_stream(s, f, n);
        fclose(f);
    }
    return rc;
}

/* The last n lines of any stream: a ring, so the input is read once and no
 * seek is needed -- which is what a pipe will require. */
static bool tail_stream(espix_session_t *s, FILE *f, long n)
{
    char **ring  = NULL;
    long   head  = 0;
    long   count = 0;

    if (n > 0) {
        ring = calloc((size_t)n, sizeof(*ring));
        if (ring == NULL) {
            espix_eprintf(s, "tail: out of memory\n");
            return false;
        }
    }

    char line[TEXT_LINE_MAX];
    while (!espix_shell_stopping(s) &&
           fgets(line, sizeof(line), f) != NULL) {
        if (n == 0) { continue; }
        char *copy = strdup(line);
        if (copy == NULL) { break; }
        free(ring[head]);
        ring[head] = copy;
        head = (head + 1) % n;
        if (count < n) { count++; }
    }

    const long start = (count < n) ? 0 : head;
    for (long k = 0; k < count; k++) {
        espix_puts(s, ring[(start + k) % n]);
    }
    for (long k = 0; k < n; k++) { free(ring[k]); }
    free(ring);
    return true;
}

static int cmd_tail(espix_session_t *s, int argc, char **argv)
{
    long      n     = 0;
    const int first = line_count(s, "tail", argc, argv, &n);
    if (first < 0) { return 2; }

    if (first >= argc) {
        FILE *f = stdin_stream(s);
        if (f == NULL) {
            espix_eprintf(s, "tail: no input; use <file>\n");
            return 1;
        }
        return tail_stream(s, f, n) ? 0 : 1;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        char  abs[ESPIX_PATH_MAX];
        FILE *f = espix_cmd_open(s, "tail", argv[i], abs, sizeof(abs));
        if (f == NULL) { rc = 1; continue; }
        if (!tail_stream(s, f, n)) { rc = 1; }
        fclose(f);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* grep                                                                 */
/* ------------------------------------------------------------------ */

static bool contains_case(const char *hay, const char *needle)
{
    if (*needle == '\0') { return true; }

    for (; *hay != '\0'; hay++) {
        const char *h = hay;
        const char *n = needle;

        while (*h != '\0' && *n != '\0' &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (*n == '\0') { return true; }
    }
    return false;
}

/* One stream's matches. `name` is the operand as typed, or NULL for standard
 * input; `many` says whether operands prefix the output. Returns whether any
 * line was selected. */
static bool grep_stream(espix_session_t *s, FILE *f, const char *name,
                        const char *pat, bool ign, bool num, bool inv,
                        bool cnt, bool lname, bool many)
{
    unsigned long matches = 0;
    char          line[TEXT_LINE_MAX];
    long          lineno  = 0;

    while (!espix_shell_stopping(s) &&
           fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        const bool hit = (ign ? contains_case(line, pat)
                              : (strstr(line, pat) != NULL)) != inv;
        if (!hit) { continue; }

        matches++;
        if (!cnt && !lname) {
            if (many && name != NULL) { espix_printf(s, "%s:", name); }
            if (num)  { espix_printf(s, "%ld:", lineno); }
            espix_puts(s, line);
        }
    }

    if (cnt) {
        if (many && name != NULL) { espix_printf(s, "%s:", name); }
        espix_printf(s, "%lu\n", matches);
    }
    if (lname && matches > 0) {
        espix_printf(s, "%s\n", (name != NULL) ? name : "(standard input)");
    }
    return matches > 0;
}

static int cmd_grep(espix_session_t *s, int argc, char **argv)
{
    bool ign = false, num = false, inv = false, cnt = false, lname = false;
    int  first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        for (const char *p = argv[first] + 1; *p != '\0'; p++) {
            switch (*p) {
            case 'i': ign   = true; break;
            case 'n': num   = true; break;
            case 'v': inv   = true; break;
            case 'c': cnt   = true; break;
            case 'l': lname = true; break;
            case 'F': break;            /* fixed strings are all there is */
            default:
                espix_eprintf(s, "grep: unknown option -%c\n", *p);
                return 2;
            }
        }
        first++;
    }
    if (first >= argc) {
        espix_eprintf(s, "usage: grep [-invclF] <pattern> <file>...\n");
        return 2;
    }

    const char *pat = argv[first++];

    if (first >= argc) {
        FILE *f = stdin_stream(s);
        if (f == NULL) {
            espix_eprintf(s, "grep: no input; use <file>\n");
            return 2;
        }
        return grep_stream(s, f, NULL, pat, ign, num, inv, cnt, lname, false)
                   ? 0 : 1;
    }

    const bool many = (argc - first) > 1;
    bool       any  = false;
    bool       err  = false;

    for (int i = first; i < argc; i++) {
        char  abs[ESPIX_PATH_MAX];
        FILE *f = espix_cmd_open(s, "grep", argv[i], abs, sizeof(abs));
        if (f == NULL) { err = true; continue; }
        if (grep_stream(s, f, argv[i], pat, ign, num, inv, cnt, lname, many)) {
            any = true;
        }
        fclose(f);
    }

    if (any) { return 0; }
    return err ? 2 : 1;
}

/* ------------------------------------------------------------------ */
/* sort                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char **v;
    size_t n, cap;
} strlist_t;

static bool strlist_add(strlist_t *l, char *owned)
{
    if (l->n == l->cap) {
        const size_t cap = (l->cap == 0) ? 32 : l->cap * 2;
        char       **nv  = realloc(l->v, cap * sizeof(*nv));
        if (nv == NULL) { return false; }
        l->v   = nv;
        l->cap = cap;
    }
    l->v[l->n++] = owned;
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmp_num(const void *a, const void *b)
{
    const double x = strtod(*(char *const *)a, NULL);
    const double y = strtod(*(char *const *)b, NULL);
    if (x < y) { return -1; }
    return (x > y) ? 1 : 0;
}

/* Whatever the operands were, one list. Separate from the reading loop so
 * standard input can take the same path as a file. */
static bool sort_read(espix_session_t *s, FILE *f, strlist_t *l)
{
    char line[TEXT_LINE_MAX];
    while (!espix_shell_stopping(s) &&
           fgets(line, sizeof(line), f) != NULL) {
        char *copy = strdup(line);
        if (copy == NULL || !strlist_add(l, copy)) {
            free(copy);
            espix_eprintf(s, "sort: out of memory\n");
            return false;
        }
    }
    return true;
}

static void sort_emit(espix_session_t *s, strlist_t *l, bool numeric, bool rev,
                      bool uniq)
{
    int (*cmp)(const void *, const void *) = numeric ? cmp_num : cmp_str;
    qsort(l->v, l->n, sizeof(*l->v), cmp);

    long prev = -1;
    for (size_t k = 0; k < l->n; k++) {
        const size_t i = rev ? (l->n - 1 - k) : k;
        if (uniq && prev >= 0 && cmp(&l->v[prev], &l->v[i]) == 0) { continue; }
        espix_puts(s, l->v[i]);
        prev = (long)i;
    }
}

static void sort_free(strlist_t *l)
{
    for (size_t i = 0; i < l->n; i++) { free(l->v[i]); }
    free(l->v);
}

static int cmd_sort(espix_session_t *s, int argc, char **argv)
{
    bool rev = false, numeric = false, uniq = false;
    int  first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        for (const char *p = argv[first] + 1; *p != '\0'; p++) {
            switch (*p) {
            case 'r': rev     = true; break;
            case 'n': numeric = true; break;
            case 'u': uniq    = true; break;
            default:
                espix_eprintf(s, "sort: unknown option -%c\n", *p);
                return 2;
            }
        }
        first++;
    }

    if (first >= argc) {
        FILE *f = stdin_stream(s);
        if (f == NULL) {
            espix_eprintf(s, "sort: no input; use <file>\n");
            return 1;
        }
        strlist_t  l  = { 0 };
        const bool ok = sort_read(s, f, &l);
        sort_emit(s, &l, numeric, rev, uniq);
        sort_free(&l);
        return ok ? 0 : 1;
    }

    strlist_t l  = { 0 };
    int       rc = 0;

    for (int i = first; i < argc; i++) {
        char  abs[ESPIX_PATH_MAX];
        FILE *f = espix_cmd_open(s, "sort", argv[i], abs, sizeof(abs));
        if (f == NULL) { rc = 1; continue; }
        if (!sort_read(s, f, &l)) { rc = 1; }
        fclose(f);
    }

    sort_emit(s, &l, numeric, rev, uniq);
    sort_free(&l);
    return rc;
}

/* ------------------------------------------------------------------ */
/* find                                                                 */
/* ------------------------------------------------------------------ */

#define FIND_DEPTH_MAX 16

static void find_walk(espix_session_t *s, const char *path, int depth, bool *err)
{
    if (espix_shell_stopping(s)) { return; }

    espix_printf(s, "%s\n", path);

    if (depth >= FIND_DEPTH_MAX) { return; }

    DIR *d = opendir(path);
    if (d == NULL) {
        /* Not a directory, which is the common case, or one that cannot be
         * read. Only the second is an error, so stat tells them apart. */
        struct stat st;
        if (stat(path, &st) != 0) {
            espix_eprintf(s, "find: %s: %s\n", path, strerror(errno));
            *err = true;
        }
        return;
    }

    struct dirent *e;
    while (!espix_shell_stopping(s) && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        /* strlcpy/strlcat rather than snprintf("%s/%s"): the compiler knows
         * the pieces can exceed ESPIX_PATH_MAX and -Wformat-truncation fires,
         * while a path that is too long is simply truncated here -- readdir
         * cannot have produced one that was not already. */
        char child[ESPIX_PATH_MAX];
        const bool slash = (path[0] != '\0' && path[strlen(path) - 1] == '/');
        strlcpy(child, path, sizeof(child));
        if (!slash) { strlcat(child, "/", sizeof(child)); }
        strlcat(child, e->d_name, sizeof(child));
        find_walk(s, child, depth + 1, err);
    }
    closedir(d);
}

static int cmd_find(espix_session_t *s, int argc, char **argv)
{
    bool err = false;

    if (argc < 2) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, ".", abs, sizeof(abs))) { return 1; }
        find_walk(s, abs, 0, &err);
        return err ? 1 : 0;
    }

    for (int i = 1; i < argc; i++) {
        char abs[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], abs, sizeof(abs))) { err = true; continue; }
        find_walk(s, abs, 0, &err);
    }
    return err ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* test, [                                                              */
/* ------------------------------------------------------------------ */

static bool test_unary(const char *op, const char *arg)
{
    struct stat st;

    if (strcmp(op, "-e") == 0) { return stat(arg, &st) == 0; }
    if (strcmp(op, "-f") == 0) { return stat(arg, &st) == 0 && S_ISREG(st.st_mode); }
    if (strcmp(op, "-d") == 0) { return stat(arg, &st) == 0 && S_ISDIR(st.st_mode); }
    if (strcmp(op, "-n") == 0) { return arg[0] != '\0'; }
    if (strcmp(op, "-z") == 0) { return arg[0] == '\0'; }
    return false;
}

static bool test_is_unary(const char *op)
{
    return strcmp(op, "-e") == 0 || strcmp(op, "-f") == 0 ||
           strcmp(op, "-d") == 0 || strcmp(op, "-n") == 0 ||
           strcmp(op, "-z") == 0;
}

static int test_run(espix_session_t *s, const char *who, int argc, char **argv)
{
    if (argc == 0) { return 1; }
    if (argc == 1) { return (argv[0][0] != '\0') ? 0 : 1; }
    if (argc == 2) {
        if (strcmp(argv[0], "!") == 0) {
            return (test_run(s, who, 1, argv + 1) == 0) ? 1 : 0;
        }
        if (!test_is_unary(argv[0])) {
            espix_eprintf(s, "%s: unknown operator %s\n", who, argv[0]);
            return 2;
        }
        return test_unary(argv[0], argv[1]) ? 0 : 1;
    }
    if (argc == 3) {
        const char *a = argv[0], *op = argv[1], *b = argv[2];

        if (strcmp(op, "=") == 0)  { return (strcmp(a, b) == 0) ? 0 : 1; }
        if (strcmp(op, "!=") == 0) { return (strcmp(a, b) != 0) ? 0 : 1; }

        if (strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
            strcmp(op, "-lt") == 0 || strcmp(op, "-gt") == 0 ||
            strcmp(op, "-le") == 0 || strcmp(op, "-ge") == 0) {
            const long x = strtol(a, NULL, 10);
            const long y = strtol(b, NULL, 10);

            if (strcmp(op, "-eq") == 0) { return (x == y) ? 0 : 1; }
            if (strcmp(op, "-ne") == 0) { return (x != y) ? 0 : 1; }
            if (strcmp(op, "-lt") == 0) { return (x <  y) ? 0 : 1; }
            if (strcmp(op, "-gt") == 0) { return (x >  y) ? 0 : 1; }
            if (strcmp(op, "-le") == 0) { return (x <= y) ? 0 : 1; }
            return (x >= y) ? 0 : 1;
        }
        espix_eprintf(s, "%s: unknown operator %s\n", who, op);
        return 2;
    }

    espix_eprintf(s, "%s: too many arguments (-a/-o are not implemented)\n", who);
    return 2;
}

static int cmd_test(espix_session_t *s, int argc, char **argv)
{
    return test_run(s, "test", argc - 1, argv + 1);
}

static int cmd_bracket(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
        espix_eprintf(s, "[: missing ]\n");
        return 2;
    }
    return test_run(s, "[", argc - 2, argv + 1);
}

/* ------------------------------------------------------------------ */
/* sleep                                                                */
/* ------------------------------------------------------------------ */

static int cmd_sleep(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: sleep <seconds>\n");
        return 1;
    }

    char      *end  = NULL;
    long       secs = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || secs < 0) {
        espix_eprintf(s, "sleep: bad seconds '%s'\n", argv[1]);
        return 1;
    }
    if (secs > 86400) { secs = 86400; }

    /*
     * In slices, checking the session's interrupt hook, so a long sleep does
     * not make the terminal unresponsive: a builtin runs on the session task,
     * and there is no signal to cut it short -- see R-P2.8 for the task case.
     */
    const TickType_t end_tick = xTaskGetTickCount() + pdMS_TO_TICKS(secs * 1000);

    while ((int32_t)(end_tick - xTaskGetTickCount()) > 0) {
        /*
         * A background job is asked to stop rather than interrupted, and it
         * must not consume the terminal input doing it: that belongs to the
         * shell, which is still reading.
         */
        if (espix_shell_stopping(s)) {
            return 130;
        }
        if (s->stop == NULL && s->poll_interrupt != NULL &&
            s->poll_interrupt(s)) {
            espix_printf(s, "^C\n");
            return 130;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_text_cmds[] = {
    { .name = "true",  .fn = cmd_true,
      .help = "do nothing, successfully", .usage = "true" },
    { .name = "false", .fn = cmd_false,
      .help = "do nothing, unsuccessfully", .usage = "false" },
    { .name = "wc",    .fn = cmd_wc,
      .help = "count lines, words and bytes", .usage = "wc [-lwc] <file>..." },
    { .name = "head",  .fn = cmd_head,
      .help = "print the first lines of a file", .usage = "head [-n N] <file>..." },
    { .name = "tail",  .fn = cmd_tail,
      .help = "print the last lines of a file", .usage = "tail [-n N] <file>..." },
    { .name = "grep",  .fn = cmd_grep,
      .help = "print lines matching a fixed string",
      .usage = "grep [-invcl] <pattern> <file>..." },
    { .name = "sort",  .fn = cmd_sort,
      .help = "sort lines", .usage = "sort [-rnu] <file>..." },
    { .name = "find",  .fn = cmd_find,
      .help = "walk a directory tree", .usage = "find [path...]" },
    { .name = "test",  .fn = cmd_test,
      .help = "evaluate an expression", .usage = "test <expr>" },
    { .name = "[",     .fn = cmd_bracket,
      .help = "test, with a closing ]", .usage = "[ <expr> ]" },
    { .name = "sleep", .fn = cmd_sleep,
      .help = "pause for a number of seconds", .usage = "sleep <seconds>" },
};

void espix_cmds_register_text(void)
{
    espix_cmds_register_table(s_text_cmds,
                              sizeof(s_text_cmds) / sizeof(s_text_cmds[0]));
}
