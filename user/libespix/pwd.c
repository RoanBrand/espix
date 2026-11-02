/* The user and group database, from the kernel's (GETPW, GETGR): espix keeps no world-readable /etc/passwd. */
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <string.h>

#include "sys.h"

/* The record for name, or for id when name is NULL: 0 or an errno. */
static int fetch(long nr, const char *name, uint32_t id, struct espix_pwent *e, size_t len)
{
    const long r = espix_sys6(nr, (long)name, (long)id, (long)e, (long)len, 0, 0);
    return (r < 0) ? (int)-r : 0;
}

/* The record's n strings into buf; their starts at s, or ERANGE. */
static int lay_out(const struct espix_pwent *e, int n, char *buf, size_t buflen, char **s)
{
    const char *p = e->strs;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        const size_t l = strlen(p) + 1;
        if (used + l > buflen) {
            return ERANGE;
        }
        s[i] = memcpy(buf + used, p, l);
        used += l;
        p += l;
    }
    return 0;
}

static int pw_r(const char *name, uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    union {
        struct espix_pwent e;
        char               raw[512];
    } rec;
    char *s[3];
    *result = NULL;
    int r = fetch(ESPIX_SYS_GETPW, name, uid, &rec.e, sizeof(rec));
    if (r == ENOENT) {
        return 0;
    }
    if (r == 0) {
        r = lay_out(&rec.e, 3, buf, buflen, s);
    }
    if (r != 0) {
        return r;
    }
    *pwd = (struct passwd){ .pw_name = s[0], .pw_passwd = (char *)"x", .pw_uid = rec.e.id, .pw_gid = rec.e.gid,
                            .pw_gecos = (char *)"", .pw_dir = s[1], .pw_shell = s[2] };
    *result = pwd;
    return 0;
}

static int gr_r(const char *name, gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    static char *no_members[] = { NULL };
    union {
        struct espix_pwent e;
        char               raw[256];
    } rec;
    char *s[1];
    *result = NULL;
    int r = fetch(ESPIX_SYS_GETGR, name, gid, &rec.e, sizeof(rec));
    if (r == ENOENT) {
        return 0;
    }
    if (r == 0) {
        r = lay_out(&rec.e, 1, buf, buflen, s);
    }
    if (r != 0) {
        return r;
    }
    *grp = (struct group){ .gr_name = s[0], .gr_passwd = (char *)"x", .gr_gid = rec.e.id, .gr_mem = no_members };
    *result = grp;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    return pw_r(name, 0, pwd, buf, buflen, result);
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    return pw_r(NULL, uid, pwd, buf, buflen, result);
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    return gr_r(name, 0, grp, buf, buflen, result);
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    return gr_r(NULL, gid, grp, buf, buflen, result);
}

static struct passwd s_pw;
static char          s_pwbuf[512];
static struct group  s_gr;
static char          s_grbuf[256];

struct passwd *getpwnam(const char *name)
{
    struct passwd *r;
    errno = pw_r(name, 0, &s_pw, s_pwbuf, sizeof(s_pwbuf), &r);
    return r;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *r;
    errno = pw_r(NULL, uid, &s_pw, s_pwbuf, sizeof(s_pwbuf), &r);
    return r;
}

struct group *getgrnam(const char *name)
{
    struct group *r;
    errno = gr_r(name, 0, &s_gr, s_grbuf, sizeof(s_grbuf), &r);
    return r;
}

struct group *getgrgid(gid_t gid)
{
    struct group *r;
    errno = gr_r(NULL, gid, &s_gr, s_grbuf, sizeof(s_grbuf), &r);
    return r;
}
