/*
 * The libc names for espix's own users and groups.
 *
 * espix_auth owns the account model -- /etc/passwd, /etc/group, uid, gid, home
 * and each account's group list -- and every espix command that touches it
 * (useradd, userdel, usermod, groupadd, groupdel, passwd, id, groups, chown,
 * chgrp) already goes through it. What was missing was the other door: a
 * *loaded app* that asks for getpwnam() or getgrgid() found neither, because no
 * table named them. That was the whole of the "there is no user to become" gap
 * -- the accounts were already here, and unnameable from an app.
 *
 * getpwnam and getpwuid return a static, as glibc's do: not reentrant, and not
 * to be held across a call that might rewrite /etc/passwd. The _r forms put the
 * name and home in the caller's buffer and hand back a pointer to it.
 *
 * pw_shell is empty rather than a path. espix has one shell and it is not a file
 * an account names; a program that tries to exec the field should fail on "no
 * program" rather than on a path that exists and is not what it thinks. Nothing
 * in espix reads it.
 *
 * What is deliberately NOT here: setuid, setgid and setgroups. Those need the
 * process's credentials to be *writable*, and espix_proc exposes only
 * espix_proc_cred_of_task() -- getters. That is a priv check and a proc API,
 * not a lookup, and it arrives when something needs it rather than before.
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stddef.h>
#include <string.h>

#include "esp_elf.h"

#include "espix_auth.h"
#include "espix_kernel.h"

#define TAG "abi"

/* The home field's size, from the account record the file itself declares. */
#define HOME_MAX sizeof(((espix_user_t *)0)->home)

/* The one static answer getpwnam()/getpwuid() return. */
static struct passwd s_pw;
static char          s_pw_name[ESPIX_USER_MAX];
static char          s_pw_dir[HOME_MAX];

static void pw_fill(const espix_user_t *u)
{
    memcpy(s_pw_name, u->name, sizeof(u->name));
    memcpy(s_pw_dir, u->home, sizeof(u->home));

    s_pw.pw_name   = s_pw_name;
    s_pw.pw_passwd = (char *)"x";   /* never the hash: /etc/passwd is 0600 root */
    s_pw.pw_uid    = (uid_t)u->uid;
    s_pw.pw_gid    = (gid_t)u->gid;
    s_pw.pw_gecos  = (char *)"";
    s_pw.pw_dir    = s_pw_dir;
    s_pw.pw_shell  = (char *)"";
}

struct passwd *getpwnam(const char *name)
{
    espix_user_t u;

    if (name == NULL || espix_auth_lookup(name, &u) != ESP_OK) {
        return NULL;
    }
    pw_fill(&u);
    return &s_pw;
}

struct passwd *getpwuid(uid_t uid)
{
    const char *name = espix_auth_name_for_uid((espix_uid_t)uid);

    return (name != NULL) ? getpwnam(name) : NULL;
}

/* Shared body of the two reentrant forms: name and home packed into buf. */
static int pw_fill_r(const espix_user_t *u, struct passwd *pwd, char *buf,
                     size_t buflen, struct passwd **result)
{
    if (result == NULL) {
        return EINVAL;
    }
    *result = NULL;

    const size_t nlen = strlen(u->name) + 1;
    const size_t hlen = strlen(u->home) + 1;
    if (nlen + hlen > buflen) {
        return ERANGE;
    }

    memcpy(buf, u->name, nlen);
    memcpy(buf + nlen, u->home, hlen);

    *pwd = (struct passwd){
        .pw_name   = buf,
        .pw_passwd = (char *)"x",
        .pw_uid    = (uid_t)u->uid,
        .pw_gid    = (gid_t)u->gid,
        .pw_gecos  = (char *)"",
        .pw_dir    = buf + nlen,
        .pw_shell  = (char *)"",
    };
    *result = pwd;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result)
{
    espix_user_t u;

    if (name == NULL || pwd == NULL || buf == NULL || result == NULL) {
        return EINVAL;
    }
    *result = NULL;

    /* "Not found" is a zero return with *result NULL, not an error. */
    if (espix_auth_lookup(name, &u) != ESP_OK) {
        return 0;
    }
    return pw_fill_r(&u, pwd, buf, buflen, result);
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result)
{
    espix_user_t u;

    if (pwd == NULL || buf == NULL || result == NULL) {
        return EINVAL;
    }
    *result = NULL;

    const char *name = espix_auth_name_for_uid((espix_uid_t)uid);
    if (name == NULL || espix_auth_lookup(name, &u) != ESP_OK) {
        return 0;
    }
    return pw_fill_r(&u, pwd, buf, buflen, result);
}

/* The same for groups, which are a name and a number and nothing else. */
static struct group s_gr;
static char         s_gr_name[ESPIX_USER_MAX];

struct group *getgrnam(const char *name)
{
    espix_gid_t gid;

    if (name == NULL || !espix_auth_group_id(name, &gid)) {
        return NULL;
    }

    strncpy(s_gr_name, name, sizeof(s_gr_name) - 1);
    s_gr_name[sizeof(s_gr_name) - 1] = '\0';

    s_gr.gr_name   = s_gr_name;
    s_gr.gr_passwd = (char *)"x";
    s_gr.gr_gid    = (gid_t)gid;
    s_gr.gr_mem    = NULL;
    return &s_gr;
}

struct group *getgrgid(gid_t gid)
{
    const char *name = espix_auth_group_name((espix_gid_t)gid);

    return (name != NULL) ? getgrnam(name) : NULL;
}

static int gr_fill_r(const char *name, espix_gid_t gid, struct group *grp,
                     char *buf, size_t buflen, struct group **result)
{
    if (result == NULL) {
        return EINVAL;
    }
    *result = NULL;

    const size_t nlen = strlen(name) + 1;
    if (nlen > buflen) {
        return ERANGE;
    }
    memcpy(buf, name, nlen);

    *grp = (struct group){
        .gr_name   = buf,
        .gr_passwd = (char *)"x",
        .gr_gid    = (gid_t)gid,
        .gr_mem    = NULL,
    };
    *result = grp;
    return 0;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result)
{
    espix_gid_t gid;

    if (name == NULL || grp == NULL || buf == NULL || result == NULL) {
        return EINVAL;
    }
    *result = NULL;

    if (!espix_auth_group_id(name, &gid)) {
        return 0;
    }
    return gr_fill_r(name, gid, grp, buf, buflen, result);
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result)
{
    if (grp == NULL || buf == NULL || result == NULL) {
        return EINVAL;
    }
    *result = NULL;

    const char *name = espix_auth_group_name((espix_gid_t)gid);
    if (name == NULL) {
        return 0;
    }
    return gr_fill_r(name, (espix_gid_t)gid, grp, buf, buflen, result);
}

static const struct esp_elfsym s_user_syms[] = {
    ESP_ELFSYM_EXPORT(getpwnam),
    ESP_ELFSYM_EXPORT(getpwuid),
    ESP_ELFSYM_EXPORT(getpwnam_r),
    ESP_ELFSYM_EXPORT(getpwuid_r),
    ESP_ELFSYM_EXPORT(getgrnam),
    ESP_ELFSYM_EXPORT(getgrgid),
    ESP_ELFSYM_EXPORT(getgrnam_r),
    ESP_ELFSYM_EXPORT(getgrgid_r),
    ESP_ELFSYM_END
};

void espix_auth_abi_register(void)
{
    if (esp_elf_register_symbol(s_user_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish users to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "users and groups published to apps");
}
