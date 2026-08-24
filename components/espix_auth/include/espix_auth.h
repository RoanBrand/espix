/*
 * espix users and password verification.
 *
 * Small on purpose: enough for SSH to decide whether someone may have a shell,
 * and for a `passwd` command to change that. No groups, no PAM, no shells other
 * than the one espix has.
 *
 * Accounts live in /etc/passwd, hash inline rather than in a separate
 * /etc/shadow. That is not laziness dressed up: espix has no file permissions,
 * so any process can read any file, and a second file would imply a protection
 * boundary that does not exist. One file, and the honest reason recorded.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_USER_MAX      17      /* 16 + NUL */
#define ESPIX_PASSWORD_MAX  65      /* 64 + NUL */

typedef struct {
    char name[ESPIX_USER_MAX];
    char home[64];
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

/* Replace a user's password, rewriting /etc/passwd. */
esp_err_t espix_auth_set_password(const char *user, const char *password);

esp_err_t espix_auth_lookup(const char *user, espix_user_t *out);

/* True while the seeded default password is unchanged, so callers can nag. */
bool espix_auth_is_default(void);

#ifdef __cplusplus
}
#endif
