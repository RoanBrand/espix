/*
 * espix users: /etc/passwd parsing and PBKDF2 password verification.
 *
 * Record format, one account per line:
 *
 *   name:$pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>:<home>
 *
 * Close enough to a crypt(3) string to be recognisable, and hex rather than
 * base64 so the file stays readable with `cat` and needs no decoder.
 *
 * Hashed rather than stored plaintext, unlike the WiFi PSK in /etc/wifi.conf.
 * The reasoning differs: a PSK protects one network the device is already on,
 * while a login password is very likely reused somewhere that matters.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psa/crypto.h"

#include "espix_auth.h"
#include "espix_kernel.h"

#define TAG "auth"

#define PASSWD_PATH     "/etc/passwd"
#define LINE_MAX_LEN    192
#define SALT_LEN        16
#define HASH_LEN        32
#define PBKDF2_ITERS    20000   /* ~100ms on a 240MHz S3; tune if login drags */

#define DEFAULT_USER    ESPIX_AUTH_DEFAULT_USER
#define DEFAULT_PASS    "espix"
#define DEFAULT_HOME    "/home/esp"

#define ALGO_TAG        "$pbkdf2-sha256$"

static bool s_is_default;

/* ------------------------------------------------------------------ */
/* Hex                                                                 */
/* ------------------------------------------------------------------ */

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static bool from_hex(const char *in, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        char pair[3] = { in[i * 2], in[i * 2 + 1], '\0' };
        if (pair[0] == '\0' || pair[1] == '\0') {
            return false;
        }
        char *end = NULL;
        const unsigned long v = strtoul(pair, &end, 16);
        if (end != pair + 2) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* PBKDF2 via PSA                                                      */
/* ------------------------------------------------------------------ */

/*
 * Note the PSA API rather than mbedtls_pkcs5_pbkdf2_hmac(): this is Mbed TLS
 * 4.x, where the classic low-level calls moved to private headers and PSA is
 * the public surface.
 */
static esp_err_t derive(const char *password, const uint8_t *salt,
                        uint32_t iters, uint8_t *out, size_t out_len)
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    esp_err_t ret = ESP_FAIL;

    if (psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256))
        != PSA_SUCCESS) {
        goto out;
    }
    if (psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST,
                                         iters) != PSA_SUCCESS) {
        goto out;
    }
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                       salt, SALT_LEN) != PSA_SUCCESS) {
        goto out;
    }
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                       (const uint8_t *)password,
                                       strlen(password)) != PSA_SUCCESS) {
        goto out;
    }
    if (psa_key_derivation_output_bytes(&op, out, out_len) != PSA_SUCCESS) {
        goto out;
    }

    ret = ESP_OK;
out:
    psa_key_derivation_abort(&op);
    return ret;
}

/* Length-independent compare, so a caller cannot time its way to the hash. */
static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* ------------------------------------------------------------------ */
/* Records                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[ESPIX_USER_MAX];
    uint32_t iters;
    uint8_t  salt[SALT_LEN];
    uint8_t  hash[HASH_LEN];
    char     home[64];
} record_t;

/* name:$pbkdf2-sha256$iters$salthex$hashhex:home */
static bool parse_line(char *line, record_t *out)
{
    memset(out, 0, sizeof(*out));

    char *name = strtok(line, ":");
    char *hash = strtok(NULL, ":");
    char *home = strtok(NULL, ":\r\n");

    if (name == NULL || hash == NULL) {
        return false;
    }
    if (strncmp(hash, ALGO_TAG, strlen(ALGO_TAG)) != 0) {
        return false;   /* unknown scheme: refuse rather than guess */
    }

    char *p = hash + strlen(ALGO_TAG);
    char *iters_s = strtok(p, "$");
    char *salt_s  = strtok(NULL, "$");
    char *hash_s  = strtok(NULL, "$");
    if (iters_s == NULL || salt_s == NULL || hash_s == NULL) {
        return false;
    }

    out->iters = (uint32_t)strtoul(iters_s, NULL, 10);
    if (out->iters == 0) {
        return false;
    }
    if (!from_hex(salt_s, out->salt, SALT_LEN) ||
        !from_hex(hash_s, out->hash, HASH_LEN)) {
        return false;
    }

    strlcpy(out->name, name, sizeof(out->name));
    strlcpy(out->home, home ? home : "/", sizeof(out->home));
    return true;
}

static bool find_record(const char *user, record_t *out)
{
    FILE *f = fopen(PASSWD_PATH, "r");
    if (f == NULL) {
        return false;
    }

    char line[LINE_MAX_LEN];
    bool found = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        record_t rec;
        /* parse_line uses strtok, so it consumes `line`; that is fine, we
         * refill it on the next iteration. */
        if (parse_line(line, &rec) && strcmp(rec.name, user) == 0) {
            *out  = rec;
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}

static esp_err_t write_record(const char *user, const char *password,
                              const char *home)
{
    uint8_t salt[SALT_LEN];
    if (psa_generate_random(salt, sizeof(salt)) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    uint8_t hash[HASH_LEN];
    if (derive(password, salt, PBKDF2_ITERS, hash, sizeof(hash)) != ESP_OK) {
        return ESP_FAIL;
    }

    char salt_hex[SALT_LEN * 2 + 1];
    char hash_hex[HASH_LEN * 2 + 1];
    to_hex(salt, sizeof(salt), salt_hex);
    to_hex(hash, sizeof(hash), hash_hex);

    /*
     * Rewrite the whole file, copying every other account. With a handful of
     * users this is simpler and less error-prone than in-place editing, and the
     * file is small enough that the flash write is not worth optimising.
     */
    char (*others)[LINE_MAX_LEN] = NULL;
    size_t other_count = 0;

    FILE *in = fopen(PASSWD_PATH, "r");
    if (in != NULL) {
        others = calloc(8, LINE_MAX_LEN);
        if (others == NULL) {
            fclose(in);
            return ESP_ERR_NO_MEM;
        }
        char line[LINE_MAX_LEN];
        while (fgets(line, sizeof(line), in) != NULL && other_count < 8) {
            char probe[LINE_MAX_LEN];
            strlcpy(probe, line, sizeof(probe));
            const char *name = strtok(probe, ":");
            if (name != NULL && strcmp(name, user) == 0) {
                continue;       /* the account being replaced */
            }
            strlcpy(others[other_count++], line, LINE_MAX_LEN);
        }
        fclose(in);
    }

    FILE *out = fopen(PASSWD_PATH, "w");
    if (out == NULL) {
        free(others);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < other_count; i++) {
        fputs(others[i], out);
    }
    fprintf(out, "%s:%s%u$%s$%s:%s\n", user, ALGO_TAG,
            (unsigned)PBKDF2_ITERS, salt_hex, hash_hex,
            home ? home : "/");

    fclose(out);
    free(others);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

bool espix_auth_verify(const char *user, const char *password)
{
    if (user == NULL || password == NULL) {
        return false;
    }

    record_t rec;
    if (!find_record(user, &rec)) {
        return false;
    }

    uint8_t candidate[HASH_LEN];
    if (derive(password, rec.salt, rec.iters, candidate,
               sizeof(candidate)) != ESP_OK) {
        return false;
    }

    return equal_ct(candidate, rec.hash, HASH_LEN);
}

esp_err_t espix_auth_set_password(const char *user, const char *password)
{
    if (user == NULL || password == NULL || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    record_t rec;
    const char *home = find_record(user, &rec) ? rec.home : "/";

    const esp_err_t err = write_record(user, password, home);
    if (err == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "password changed for %s", user);
        if (strcmp(user, DEFAULT_USER) == 0) {
            s_is_default = false;
        }
    }
    return err;
}

esp_err_t espix_auth_lookup(const char *user, espix_user_t *out)
{
    record_t rec;
    if (out == NULL || !find_record(user, &rec)) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(out->name, rec.name, sizeof(out->name));
    strlcpy(out->home, rec.home, sizeof(out->home));
    return ESP_OK;
}

bool espix_auth_is_default(void)
{
    return s_is_default;
}

esp_err_t espix_auth_init(void)
{
    record_t rec;

    if (find_record(DEFAULT_USER, &rec)) {
        /* Detect the shipped password so we can nag, without storing a marker:
         * verifying it is cheap and cannot be wrong. */
        s_is_default = espix_auth_verify(DEFAULT_USER, DEFAULT_PASS);
    } else {
        const esp_err_t err = write_record(DEFAULT_USER, DEFAULT_PASS,
                                           DEFAULT_HOME);
        if (err != ESP_OK) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create %s: %s",
                       PASSWD_PATH, esp_err_to_name(err));
            return err;
        }
        s_is_default = true;
        espix_klog(ESPIX_KLOG_INFO, TAG, "created %s with user '%s'",
                   PASSWD_PATH, DEFAULT_USER);
    }

    if (s_is_default) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "user '%s' still has the default password; run 'passwd'",
                   DEFAULT_USER);
    }

    return ESP_OK;
}
