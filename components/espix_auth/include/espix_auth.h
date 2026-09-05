/*
 * espix users and password verification.
 *
 * Small on purpose: enough for SSH to decide whether someone may have a shell,
 * for a `passwd` command to change that, and to answer "who is uid 1000" for
 * `ls -l`. No PAM, no shells other than the one espix has.
 *
 * Groups exist only as a number. Every account's gid equals its uid and there
 * is no /etc/group, but the gid is stored and the group mode bits are checked,
 * so the on-disk format and the permission check are already the right shape
 * when real groups arrive.
 *
 * Accounts live in /etc/passwd with the hash inline rather than in a separate
 * /etc/shadow. That used to be justified by espix having no file permissions,
 * which is no longer true. What replaces it: /etc/passwd is 0600 root:root, and
 * espix's own commands read it as the kernel rather than as a process, so a
 * loaded app cannot reach the hashes at all. That closes most of what a split
 * would buy; see docs/ROADMAP.md for what is left of the case for one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The account seeded on first boot, named here so the login greeting can say
 * which one still carries the shipped password. */
#define ESPIX_AUTH_DEFAULT_USER "esp"

/* The superuser, by the only name anything should have to know it by. */
#define ESPIX_AUTH_ROOT_USER    "root"

#define ESPIX_USER_MAX      17      /* 16 + NUL */
#define ESPIX_PASSWORD_MAX  65      /* 64 + NUL */

/*
 * root is 0 because everything that checks for privilege checks for zero, here
 * as everywhere else. The first ordinary account is 1000, following the
 * convention every Linux distribution uses, so a uid seen in `ls -l` means what
 * someone coming from Linux expects it to.
 */
#define ESPIX_UID_ROOT      0
#define ESPIX_UID_FIRST     1000

/*
 * Service accounts live below the people, as they do on Debian: an app that
 * wants its own identity so its files are not another app's gets an id here,
 * a locked password and no home directory.
 */
#define ESPIX_UID_SYSTEM_FIRST  100
#define ESPIX_UID_SYSTEM_LAST   999

/*
 * The identity to fall back to when an account cannot be resolved.
 *
 * Never granted deliberately and owns nothing, so a session that ends up here
 * can read what is world-readable and touch almost nothing. The point is that
 * the failure path is not uid 0: a zero-initialised credential would make
 * "espix could not find your account" mean "you are root", which is the classic
 * way this goes wrong.
 */
#define ESPIX_UID_NOBODY    65534

typedef uint16_t espix_uid_t;
typedef uint16_t espix_gid_t;

typedef struct {
    char        name[ESPIX_USER_MAX];
    char        home[64];
    espix_uid_t uid;
    espix_gid_t gid;
} espix_user_t;

/*
 * Parse /etc/passwd, creating it with the default account if absent. Safe to
 * call before anything needs authentication; logs a warning while the default
 * password is still in place.
 */
esp_err_t espix_auth_init(void);

/*
 * Constant-time-ish comparison against the stored hash. Returns false for an
 * unknown user, a malformed record, or a wrong password — deliberately without
 * distinguishing them to a caller that might report the difference.
 */
bool espix_auth_verify(const char *user, const char *password);

/*
 * Take away a user's password, so nothing can authenticate as them again.
 *
 * The reverse of espix_auth_set_password(), and the only way back after root
 * has been given one: unlocking is just setting a password, but without this
 * that door could never be shut again.
 */
esp_err_t espix_auth_lock(const char *user);

/* Replace a user's password, rewriting /etc/passwd. */
esp_err_t espix_auth_set_password(const char *user, const char *password);

esp_err_t espix_auth_lookup(const char *user, espix_user_t *out);

/*
 * The account owning `uid`, or NULL if no record claims it.
 *
 * Returns a pointer into a small internal cache rather than a copy, because the
 * caller is `ls -l` rendering a column per entry and re-reading /etc/passwd for
 * every line of a directory listing would be absurd. Not reentrant, and not to
 * be held across anything that might change the file.
 */
const char *espix_auth_name_for_uid(espix_uid_t uid);

/*
 * Every group `user` belongs to: the primary from their account record, then
 * anything in /etc/group that names them. Returns how many landed in `out`.
 *
 * Resolved once when a session is set up, for the same reason the uid is: the
 * file is the authority but not something to re-read on the path of every
 * open().
 */
size_t espix_auth_groups(const char *user, espix_gid_t *out, size_t max);

/* A group's id by name -- the reverse of espix_auth_group_name(), and what
 * `chgrp somegroup` needs. */
bool espix_auth_group_id(const char *name, espix_gid_t *out);

/* The name of a group id, or NULL if no group claims it. Cached, and not
 * reentrant, like espix_auth_name_for_uid(). */
const char *espix_auth_group_name(espix_gid_t gid);

/* Membership by name, which is what a `%group` line in /etc/sudoers needs. */
bool espix_auth_in_group(const char *user, const char *group);

/*
 * Create an account, locked, with the next free id in the range `system`
 * selects -- useradd(8) semantics, so nothing can log in as it until `passwd`
 * gives it a password, and no home directory exists unless `make_home`.
 *
 * A group of the same name and id is created with it.
 */
esp_err_t espix_auth_user_add(const char *name, bool system, bool make_home);

/* Remove an account, its private group and every membership naming it. Refuses
 * uid 0. */
esp_err_t espix_auth_user_del(const char *name, bool remove_home);

esp_err_t espix_auth_group_add(const char *name, bool system);
esp_err_t espix_auth_group_del(const char *name);

/*
 * Put `user` in the comma-separated `csv` groups. `append` keeps the
 * memberships they already have; otherwise those are dropped first, which is
 * the difference between usermod -aG and usermod -G.
 */
esp_err_t espix_auth_set_groups(const char *user, const char *csv, bool append);

/*
 * May `user` run a command as root?
 *
 * The list is /etc/sudoers, one account name per line, `#` to end of line. It
 * is the espix equivalent of Debian putting the first account in the `sudo`
 * group: root is seeded locked, so this is the only way up to uid 0 that does
 * not need the serial console.
 *
 * Read here rather than by whoever asks, because the file is 0600 root and this
 * component already holds the privilege seam for reaching such a file.
 */
bool espix_auth_may_sudo(const char *user);

/*
 * True if `user` has no usable password and so can never authenticate. root is
 * seeded this way: it is a real account with a home and a uid, reachable by
 * `su` or the console, but not by anyone holding a password.
 */
bool espix_auth_is_locked(const char *user);

/* True while the seeded default password is unchanged, so callers can nag. */
bool espix_auth_is_default(void);

#ifdef __cplusplus
}
#endif
