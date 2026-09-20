/*
 * espix users: /etc/passwd parsing and PBKDF2 password verification.
 *
 * Record format, one account per line:
 *
 *   name:$pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>:<uid>:<gid>:<home>
 *
 * Close enough to a crypt(3) string to be recognisable, and hex rather than
 * base64 so the file stays readable with `cat` and needs no decoder. Field
 * order follows /etc/passwd as far as espix has fields for.
 *
 * A `!` where the hash goes is a locked account: it has a name, a uid and a
 * home, and no password will ever match it. root is seeded that way.
 *
 * Hashed rather than stored plaintext, unlike the WiFi PSK in /etc/wifi.conf.
 * The reasoning differs: a PSK protects one network the device is already on,
 * while a login password is very likely reused somewhere that matters.
 */

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "mbedtls/platform_util.h"   /* mbedtls_platform_zeroize */
#include "psa/crypto.h"

/*
 * The SHA peripheral's own API, from the mbedtls component's port. The public
 * PSA and Mbed TLS calls reach the same hardware but pay a driver setup --
 * including a heap allocation and a peripheral reset -- on every hash, which
 * for PBKDF2's 40000 tiny hashes a login is the whole cost. See the note above
 * pbkdf2_sha256().
 */
#include "sha/sha_core.h"

#include "espix_auth.h"
#include "espix_fs.h"
#include "espix_kernel.h"

#define TAG "auth"

#define PASSWD_PATH     "/etc/passwd"
#define SUDOERS_PATH    "/etc/sudoers"
#define GROUP_PATH      "/etc/group"
/*
 * Long enough for the widest record the format allows: a 16-character name, the
 * algorithm tag, 20000, a 32-hex salt, a 64-hex hash, uid, gid and a 63-byte
 * home is 213 bytes. The old 192 predated uid and gid and was already one long
 * name away from truncating a hash.
 */
#define LINE_MAX_LEN    256
#define SALT_LEN        16
#define HASH_LEN        32
/*
 * 20000 iterations is 40000 HMACs -- two per iteration, inner and outer -- and
 * that is the number the login actually pays for, so it is the one the number
 * below has to be read against.
 *
 * It has not been reduced and should not be: cutting it would shorten the login
 * by weakening password hashing. What the per-iteration overhead used to be is
 * the story told above pbkdf2_sha256(), and it is no longer paid.
 */
#define PBKDF2_ITERS    20000

#define DEFAULT_USER    ESPIX_AUTH_DEFAULT_USER
#define DEFAULT_PASS    "espix"
#define DEFAULT_HOME    "/home/esp"

#define ROOT_USER       ESPIX_AUTH_ROOT_USER

/*
 * root's home is `/`, not `/root`.
 *
 * It matches what the console already does -- it starts at / and the prompt
 * shows an absolute path there rather than `~` -- and it means no warning at
 * login about a home that the rootfs image does not contain.
 *
 * It also falls out well for ownership: the rule that a file belongs to the
 * account whose home contains it, longest home winning, then makes root the
 * owner of everything that is not inside somebody's home. Which is what a Unix
 * rootfs looks like.
 */
#define ROOT_HOME       "/"

#define ALGO_TAG        "$pbkdf2-sha256$"
#define LOCKED_TAG      "!"

#define MAX_ACCOUNTS    8
#define MAX_GROUPS      12

/* Every member name plus a separator, for the widest membership that fits the
 * account table. */
#define MEMBERS_MAX     (MAX_ACCOUNTS * ESPIX_USER_MAX)

/* The group Debian puts administrators in, at the gid Debian uses. Seeded so a
 * fresh /etc/sudoers can say %sudo and mean something. */
#define SUDO_GROUP      "sudo"
#define SUDO_GID        27

static bool s_is_default;

/*
 * Two caches over /etc/passwd, both cleared by any write to it.
 *
 * They exist because of who asks. espix_fs calls the ownership rule below for
 * every path that carries no stored owner -- which, on a device nobody has
 * chowned, is every path -- and `ls -l` renders a name per directory entry.
 * Re-reading and re-parsing the account file for each of those would make a
 * listing quadratic in flash reads for no gain: the answer only changes when
 * somebody adds a user or moves a home.
 */
static espix_user_t s_accounts[MAX_ACCOUNTS];
static size_t       s_account_count;
static bool         s_accounts_valid;

static char        s_name_cache[ESPIX_USER_MAX];
static espix_uid_t s_name_cache_uid;
static bool        s_name_cache_valid;

static char        s_group_cache[ESPIX_USER_MAX];
static espix_gid_t s_group_cache_gid;
static bool        s_group_cache_valid;

static void cache_invalidate(void)
{
    s_accounts_valid    = false;
    s_name_cache_valid  = false;
    s_group_cache_valid = false;
}

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
 * PBKDF2-HMAC-SHA256 over the SHA peripheral directly.
 *
 * WHY NOT PSA. Measured on an S3 at 240MHz, 20000 iterations (40000 HMACs, two
 * per iteration) cost 1207 ms through psa_hash_clone(). Almost none of that is
 * hashing. Per HMAC the PSA path pays:
 *
 *   - psa_hash_clone() -> esp_sha_hash_clone(), which is
 *     heap_caps_malloc(sizeof(esp_sha256_context), MALLOC_CAP_DMA|INTERNAL)
 *     plus a memset and a memcpy, undone by psa_hash_finish()'s abort. That is
 *     40000 malloc/free pairs a login, not the "no per-iteration allocation"
 *     an earlier version of this comment claimed.
 *   - esp_sha_acquire_hardware(), which takes the SHA/AES lock *and* calls
 *     esp_crypto_sha_enable_periph_clk(true) -- and that function resets the
 *     peripheral on every enable. So the SHA block is reset 40000 times a login
 *     to hash 40000 blocks.
 *
 * WHAT THIS DOES INSTEAD. Absorb ipad and opad once, keep the two resulting
 * SHA-256 states in 32-byte arrays, and per HMAC do only
 * esp_sha_write_digest_state() -> esp_sha_block() -> esp_sha_read_digest_state()
 * (SOC_SHA_SUPPORT_RESUME is what makes the middle of a hash resumable). No
 * allocation, no key schedule, no peripheral reset per hash.
 *
 * The hardware is still shared: SHA shares a lock with AES, and with the crypto
 * DMA, so it must not be held for the ~1s the whole derivation takes or WiFi's
 * AES work would stall behind it. It is therefore taken and released once per
 * PBKDF2_BATCH iterations -- a few hundred microseconds at a time, and a few
 * hundred acquisitions a login instead of 40000.
 *
 * This is the shape SHA needs; the construction is untouched. Same output --
 * pbkdf2_selftest() checks the result against an RFC 7914 vector at init, so a
 * mistake here is caught at boot rather than as a login that will not work.
 *
 * The public APIs were measured and are slower on this target, which is why
 * this reaches for the peripheral. mbedtls_pkcs5_pbkdf2_hmac_ext() reaches the
 * same hardware through the MD layer and pays that driver's setup on all 40000
 * hashes (1675 ms); psa_key_derivation_*() re-derives the key schedule every
 * iteration (2030 ms). The one-shot esp_sha_hash_compute() is allocation-free
 * (its context is on the stack) but must re-absorb ipad or opad for every
 * hash, doubling the blocks for no reason. Reported in docs/UPSTREAM.md.
 */

#define SHA_BLOCK 64        /* SHA-256's block, and the HMAC pad length */

/* Iterations between releasing the SHA/AES lock. Long enough that the lock is
 * not what the loop costs, short enough that AES work waiting behind it is not
 * made to wait for a login. */
#define PBKDF2_BATCH 64

/*
 * The final block of a message whose first SHA_BLOCK bytes are already in
 * `state`: `tail`, then the 0x80, zero fill, and the 64-bit big-endian bit
 * count. Both hashes here absorb exactly one block before the tail -- the ipad
 * or opad -- so the count is (SHA_BLOCK + tail_len) * 8.
 */
static void final_block(uint8_t block[SHA_BLOCK],
                        const uint8_t *tail, size_t tail_len)
{
    memset(block, 0, SHA_BLOCK);
    memcpy(block, tail, tail_len);
    block[tail_len] = 0x80;

    const uint64_t bits = (uint64_t)(SHA_BLOCK + tail_len) * 8;
    for (unsigned i = 0; i < 8; i++) {
        block[SHA_BLOCK - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
}

/*
 * One SHA-256 over `block`, resumed from the prepared `state` and written to
 * `out`. `state` is only read, which is what lets one prepared state serve
 * every iteration. The hardware must already be held.
 */
static void sha_resume(uint8_t state[HASH_LEN],
                       const uint8_t block[SHA_BLOCK], uint8_t out[HASH_LEN])
{
    esp_sha_write_digest_state(SHA2_256, state);
    esp_sha_block(SHA2_256, block, false);
    esp_sha_read_digest_state(SHA2_256, out);
}

/*
 * Exactly one output block, which is all this needs: HASH_LEN is SHA-256's
 * digest size, so T_1 is the whole derived key and the block counter is a
 * constant 1. A second block would need the loop below wrapped in another.
 */
static esp_err_t pbkdf2_sha256(const char *password,
                               const uint8_t *salt, size_t salt_len,
                               uint32_t iters, uint8_t *out, size_t out_len)
{
    if (out_len != HASH_LEN || iters == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* S || INT_BE32(1) must fit the final block beside the 0x80 and the length
     * field, or U_1 would need a second block. SALT_LEN is 16, so this guards
     * against a future caller rather than a case that occurs. */
    if (salt_len + sizeof(uint32_t) > SHA_BLOCK - 9) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t        ipad[SHA_BLOCK];
    uint8_t        opad[SHA_BLOCK];
    uint8_t        key[HASH_LEN];
    const uint8_t *k     = (const uint8_t *)password;
    size_t         k_len = strlen(password);

    /* RFC 2104: a key longer than the block is replaced by its own digest. */
    if (k_len > SHA_BLOCK) {
        size_t got = 0;
        if (psa_hash_compute(PSA_ALG_SHA_256, k, k_len, key, sizeof(key),
                             &got) != PSA_SUCCESS || got != HASH_LEN) {
            return ESP_FAIL;
        }
        k     = key;
        k_len = HASH_LEN;
    }

    memset(ipad, 0x36, sizeof(ipad));
    memset(opad, 0x5c, sizeof(opad));
    for (size_t i = 0; i < k_len; i++) {
        ipad[i] = (uint8_t)(ipad[i] ^ k[i]);
        opad[i] = (uint8_t)(opad[i] ^ k[i]);
    }

    uint8_t       inner_state[HASH_LEN];
    uint8_t       outer_state[HASH_LEN];
    uint8_t       block[SHA_BLOCK];
    uint8_t       u[HASH_LEN];
    uint8_t       acc[HASH_LEN];
    uint8_t       tail[SHA_BLOCK];
    const uint8_t counter[4] = { 0, 0, 0, 1 };

    /* One hold covers both prepared states and U_1. */
    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);

    /* Absorb each pad once; what is read back is the state a hash resumed from
     * it would start with. start=true reloads the IV, so the second block does
     * not disturb the first. */
    esp_sha_block(SHA2_256, ipad, true);
    esp_sha_read_digest_state(SHA2_256, inner_state);
    esp_sha_block(SHA2_256, opad, true);
    esp_sha_read_digest_state(SHA2_256, outer_state);

    /* U_1 = PRF(P, S || INT_BE32(1)) */
    memcpy(tail, salt, salt_len);
    memcpy(tail + salt_len, counter, sizeof(counter));
    final_block(block, tail, salt_len + sizeof(counter));
    sha_resume(inner_state, block, u);
    final_block(block, u, HASH_LEN);
    sha_resume(outer_state, block, u);
    memcpy(acc, u, HASH_LEN);

    esp_sha_release_hardware();

    /* U_i = PRF(P, U_{i-1}); T_1 = U_1 ^ U_2 ^ ... ^ U_c */
    uint32_t i = 1;
    while (i < iters) {
        uint32_t n = iters - i;
        if (n > PBKDF2_BATCH) {
            n = PBKDF2_BATCH;
        }

        esp_sha_acquire_hardware();
        esp_sha_set_mode(SHA2_256);
        for (uint32_t j = 0; j < n; j++, i++) {
            final_block(block, u, HASH_LEN);
            sha_resume(inner_state, block, u);
            final_block(block, u, HASH_LEN);
            sha_resume(outer_state, block, u);
            for (size_t b = 0; b < HASH_LEN; b++) {
                acc[b] = (uint8_t)(acc[b] ^ u[b]);
            }
        }
        esp_sha_release_hardware();
    }

    memcpy(out, acc, HASH_LEN);

    /* Neither the pads nor the intermediates may outlive this call: all are
     * derived from the password. */
    mbedtls_platform_zeroize(ipad, sizeof(ipad));
    mbedtls_platform_zeroize(opad, sizeof(opad));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(inner_state, sizeof(inner_state));
    mbedtls_platform_zeroize(outer_state, sizeof(outer_state));
    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(acc, sizeof(acc));
    mbedtls_platform_zeroize(tail, sizeof(tail));
    return ESP_OK;
}

static esp_err_t derive(const char *password, const uint8_t *salt,
                        uint32_t iters, uint8_t *out, size_t out_len)
{
    return pbkdf2_sha256(password, salt, SALT_LEN, iters, out, out_len);
}

/*
 * Known-answer tests, because this is hand-driven HMAC and "every login still
 * works" would also be true of an implementation that is consistently wrong.
 *
 * RFC 7914 section 11, the first PBKDF2-HMAC-SHA256 vector: P="passwd",
 * S="salt", c=1, dkLen=64. Only the first 32 bytes are checked, which is
 * exactly T_1 -- the one block this produces.
 *
 * The second is the same inputs at c=200, an independently computed vector
 * rather than a published one, and it is here for a different reason: 200
 * crosses PBKDF2_BATCH three times, so releasing and re-taking the hardware in
 * the middle of a derivation is exercised at boot rather than by a login that
 * silently stops working. It costs a few hundred hashes.
 */
static void pbkdf2_selftest(void)
{
    static const uint8_t want1[HASH_LEN] = {
        0x55, 0xac, 0x04, 0x6e, 0x56, 0xe3, 0x08, 0x9f,
        0xec, 0x16, 0x91, 0xc2, 0x25, 0x44, 0xb6, 0x05,
        0xf9, 0x41, 0x85, 0x21, 0x6d, 0xde, 0x04, 0x65,
        0xe6, 0x8b, 0x9d, 0x57, 0xc2, 0x0d, 0xac, 0xbc,
    };
    static const uint8_t want200[HASH_LEN] = {
        0x0f, 0x40, 0x9e, 0xb1, 0xaa, 0x5a, 0xd2, 0x60,
        0x03, 0x3e, 0x44, 0x16, 0xbd, 0xdf, 0xa6, 0xa5,
        0x23, 0x43, 0xb8, 0x83, 0x94, 0x2c, 0x88, 0xec,
        0xcd, 0x24, 0x4e, 0x2d, 0x39, 0x12, 0x4b, 0xa1,
    };
    uint8_t got[HASH_LEN];

    if (pbkdf2_sha256("passwd", (const uint8_t *)"salt", 4, 1,
                      got, sizeof(got)) != ESP_OK ||
        memcmp(got, want1, sizeof(want1)) != 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "PBKDF2 self-test FAILED -- passwords cannot be trusted");
        return;
    }
    if (pbkdf2_sha256("passwd", (const uint8_t *)"salt", 4, 200,
                      got, sizeof(got)) != ESP_OK ||
        memcmp(got, want200, sizeof(want200)) != 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "PBKDF2 batch self-test FAILED -- passwords cannot be trusted");
        return;
    }
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "PBKDF2 self-test ok");
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
    char        name[ESPIX_USER_MAX];
    uint32_t    iters;
    uint8_t     salt[SALT_LEN];
    uint8_t     hash[HASH_LEN];
    char        home[64];
    espix_uid_t uid;
    espix_gid_t gid;
    bool        locked;     /* no password will match; still a real account */
} record_t;

/*
 * Strictly numeric, because a field that fails to parse must not quietly become
 * uid 0. strtoul() alone would read "" and "root" as zero and hand out the
 * superuser to a corrupted line.
 */
static bool parse_id(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char             *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v > UINT16_MAX) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

/* name:$pbkdf2-sha256$iters$salthex$hashhex:uid:gid:home */
static bool parse_line(char *line, record_t *out)
{
    memset(out, 0, sizeof(*out));

    /*
     * Take every colon-separated field before touching the hash's internals.
     * Splitting the hash calls strtok() again, which resets the state this
     * depends on -- the original code was careful about that too, and it is
     * easy to undo by accident when adding a field.
     */
    char *name = strtok(line, ":");
    char *hash = strtok(NULL, ":");
    char *uid  = strtok(NULL, ":\r\n");
    char *gid  = strtok(NULL, ":\r\n");
    char *home = strtok(NULL, ":\r\n");

    if (name == NULL || hash == NULL) {
        return false;
    }
    if (!parse_id(uid, &out->uid) || !parse_id(gid, &out->gid)) {
        return false;
    }

    strlcpy(out->name, name, sizeof(out->name));
    strlcpy(out->home, home != NULL ? home : "/", sizeof(out->home));

    if (strcmp(hash, LOCKED_TAG) == 0) {
        out->locked = true;
        return true;
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

    return true;
}

/* The canonical line for a record, newline included. */
static void format_record(const record_t *rec, char *out, size_t len)
{
    if (rec->locked) {
        snprintf(out, len, "%s:%s:%u:%u:%s\n", rec->name, LOCKED_TAG,
                 (unsigned)rec->uid, (unsigned)rec->gid, rec->home);
        return;
    }

    char salt_hex[SALT_LEN * 2 + 1];
    char hash_hex[HASH_LEN * 2 + 1];
    to_hex(rec->salt, SALT_LEN, salt_hex);
    to_hex(rec->hash, HASH_LEN, hash_hex);

    snprintf(out, len, "%s:%s%u$%s$%s:%u:%u:%s\n", rec->name, ALGO_TAG,
             (unsigned)rec->iters, salt_hex, hash_hex,
             (unsigned)rec->uid, (unsigned)rec->gid, rec->home);
}

static bool find_record(const char *user, record_t *out)
{
    /*
     * Raised for the open, because /etc/passwd is root's and this runs with the
     * credentials of whoever is asking -- a login from an unprivileged session,
     * or `whoami` in a shell. espix_auth is the component that owns this file;
     * see espix_fs_priv_begin() for why that is the whole justification.
     */
    espix_fs_priv_begin();
    FILE *f = fopen(PASSWD_PATH, "r");
    espix_fs_priv_end();

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

/*
 * The whole file, parsed. Returns how many records landed in `recs`.
 *
 * Every write rewrites the file from one of these, which is what lets a record
 * gain fields without any caller having to preserve the ones it does not care
 * about. A handful of accounts makes this cheaper than editing in place, and
 * the file is small enough that the flash write is not worth optimising.
 *
 * A line that will not parse is dropped with a warning rather than carried
 * through: it cannot authenticate anyone, and re-emitting something this code
 * could not read would be pretending to understand it.
 */
static size_t load_all(record_t *recs, size_t max)
{
    espix_fs_priv_begin();
    FILE *f = fopen(PASSWD_PATH, "r");
    espix_fs_priv_end();

    if (f == NULL) {
        return 0;
    }

    char   line[LINE_MAX_LEN];
    size_t n = 0;

    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        char keep[LINE_MAX_LEN];
        strlcpy(keep, line, sizeof(keep));

        if (parse_line(line, &recs[n])) {
            n++;
        } else {
            keep[strcspn(keep, "\r\n")] = '\0';
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: ignoring unreadable line '%s'",
                       PASSWD_PATH, keep);
        }
    }

    fclose(f);
    return n;
}

static esp_err_t store_all(const record_t *recs, size_t count)
{
    /*
     * The write, too: `passwd` is a builtin running as the user changing their
     * own password, and without this it could not rewrite the file that holds
     * it. This is the seam that stands in for setuid, which espix does not have.
     */
    espix_fs_priv_begin();
    FILE *out = fopen(PASSWD_PATH, "w");
    espix_fs_priv_end();

    if (out == NULL) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < count; i++) {
        char line[LINE_MAX_LEN];
        format_record(&recs[i], line, sizeof(line));
        fputs(line, out);
    }

    cache_invalidate();
    return (fclose(out) == 0) ? ESP_OK : ESP_FAIL;
}

/* Replace the record named by `rec`, or append it if there is none. */
static esp_err_t upsert(const record_t *rec)
{
    record_t *recs = calloc(MAX_ACCOUNTS, sizeof(*recs));
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count   = load_all(recs, MAX_ACCOUNTS);
    bool   replaced = false;

    for (size_t i = 0; i < count; i++) {
        if (strcmp(recs[i].name, rec->name) == 0) {
            recs[i] = *rec;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (count == MAX_ACCOUNTS) {
            free(recs);
            return ESP_ERR_NO_MEM;
        }
        recs[count++] = *rec;
    }

    const esp_err_t err = store_all(recs, count);
    free(recs);
    return err;
}

/* Fill in a record's hash from a plaintext password. */
static esp_err_t set_hash(record_t *rec, const char *password)
{
    if (psa_generate_random(rec->salt, SALT_LEN) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    if (derive(password, rec->salt, PBKDF2_ITERS, rec->hash,
               HASH_LEN) != ESP_OK) {
        return ESP_FAIL;
    }
    rec->iters  = PBKDF2_ITERS;
    rec->locked = false;
    return ESP_OK;
}

static esp_err_t write_record(const char *user, const char *password,
                              espix_uid_t uid, espix_gid_t gid,
                              const char *home)
{
    record_t rec = { 0 };

    strlcpy(rec.name, user, sizeof(rec.name));
    strlcpy(rec.home, home != NULL ? home : "/", sizeof(rec.home));
    rec.uid = uid;
    rec.gid = gid;

    if (password == NULL) {
        rec.locked = true;      /* a real account nobody can log into */
    } else {
        const esp_err_t err = set_hash(&rec, password);
        if (err != ESP_OK) {
            return err;
        }
    }

    return upsert(&rec);
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
    if (rec.locked) {
        /* No hash to compare against, so nothing can match. Checked here as
         * well as at parse time because a locked record is a valid record --
         * it is only authentication it is barred from. */
        return false;
    }

    /*
     * The one phase of a login with no I/O in it, so its wall time is its CPU
     * time -- which makes it the number that decides whether a slow login is
     * computation or network. DEBUG, so it costs nothing at the default level.
     *
     * PBKDF2_ITERS carries the comment "~100ms on a 240MHz S3". That has never
     * been measured, and the watchdog backtrace shows a heap_caps_malloc per
     * HMAC iteration underneath it, so the cost may be the allocator rather
     * than the crypto. Those want different fixes and only one of them is free.
     */
    const int64_t t0 = esp_timer_get_time();

    uint8_t candidate[HASH_LEN];
    if (derive(password, rec.salt, rec.iters, candidate,
               sizeof(candidate)) != ESP_OK) {
        return false;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "timing: pbkdf2 %u iters, %lld ms",
               (unsigned)rec.iters,
               (long long)((esp_timer_get_time() - t0) / 1000));

    return equal_ct(candidate, rec.hash, HASH_LEN);
}

esp_err_t espix_auth_set_password(const char *user, const char *password)
{
    if (user == NULL || password == NULL || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Only ever changes an existing account. This used to create one, giving it
     * ESPIX_UID_FIRST -- so a second `passwd bob hunter2` produced an account
     * sharing esp's uid and therefore esp's ownership of everything. Creation
     * belongs to useradd, which allocates a free id; moving it there is what
     * fixes that rather than patching the fallback.
     */
    record_t rec;
    if (!find_record(user, &rec)) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t err = write_record(user, password, rec.uid, rec.gid,
                                       rec.home);
    if (err == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "password changed for %s", user);
        if (strcmp(user, DEFAULT_USER) == 0) {
            s_is_default = false;
        }
    }
    return err;
}

esp_err_t espix_auth_lock(const char *user)
{
    if (user == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    record_t rec;
    if (!find_record(user, &rec)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (rec.locked) {
        return ESP_OK;          /* already shut; not an error */
    }

    /* Drop the hash as well as setting the flag: a locked record should not
     * carry material that would come back if the flag were ever lost. */
    rec.locked = true;
    rec.iters  = 0;
    memset(rec.salt, 0, sizeof(rec.salt));
    memset(rec.hash, 0, sizeof(rec.hash));

    const esp_err_t err = upsert(&rec);
    if (err == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "locked account '%s'", user);
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
    out->uid = rec.uid;
    out->gid = rec.gid;
    return ESP_OK;
}

bool espix_auth_is_default(void)
{
    return s_is_default;
}

bool espix_auth_is_locked(const char *user)
{
    record_t rec;
    return find_record(user, &rec) && rec.locked;
}

/*
 * Refresh the account cache if a write has invalidated it.
 *
 * Silent on failure: an unreadable /etc/passwd leaves the list empty, which
 * makes every file root-owned and every uid nameless. That is the safe way to
 * be wrong -- it denies rather than grants -- and the parse failure is already
 * logged by load_all().
 */
static void accounts_refresh(void)
{
    if (s_accounts_valid) {
        return;
    }

    record_t *recs = calloc(MAX_ACCOUNTS, sizeof(*recs));
    if (recs == NULL) {
        return;
    }

    const size_t count = load_all(recs, MAX_ACCOUNTS);

    s_account_count = 0;
    for (size_t i = 0; i < count; i++) {
        espix_user_t *u = &s_accounts[s_account_count++];
        strlcpy(u->name, recs[i].name, sizeof(u->name));
        strlcpy(u->home, recs[i].home, sizeof(u->home));
        u->uid = recs[i].uid;
        u->gid = recs[i].gid;
    }
    s_accounts_valid = true;

    free(recs);
}

const char *espix_auth_name_for_uid(espix_uid_t uid)
{
    if (s_name_cache_valid && s_name_cache_uid == uid) {
        return s_name_cache;
    }

    accounts_refresh();

    for (size_t i = 0; i < s_account_count; i++) {
        if (s_accounts[i].uid == uid) {
            strlcpy(s_name_cache, s_accounts[i].name, sizeof(s_name_cache));
            s_name_cache_uid   = uid;
            s_name_cache_valid = true;
            return s_name_cache;
        }
    }

    return NULL;
}

/*
 * The espix_fs ownership rule: a file with no stored owner belongs to the
 * account whose home contains it, the longest home winning.
 *
 * root's home is "/", so it matches everything and loses every tie to a more
 * specific home. The effect is that /home/esp and everything under it belongs
 * to esp, and the rest of the rootfs belongs to root -- with nothing written to
 * flash to say so, which is what keeps a freshly imaged device free of
 * attribute data and what makes a storage-flash come back correct.
 */
static bool owner_rule(const char *abs_path, uint16_t *uid, uint16_t *gid)
{
    accounts_refresh();

    size_t best  = 0;
    bool   found = false;

    for (size_t i = 0; i < s_account_count; i++) {
        const size_t len = strlen(s_accounts[i].home);
        if ((!found || len > best) &&
            espix_fs_within(abs_path, s_accounts[i].home)) {
            best   = len;
            *uid   = s_accounts[i].uid;
            *gid   = s_accounts[i].gid;
            found  = true;
        }
    }

    return found;
}

/* ------------------------------------------------------------------ */
/* Groups                                                              */
/* ------------------------------------------------------------------ */

/*
 * name:gid:member,member
 *
 * Linux's format without the vestigial password field, the same trimming
 * /etc/passwd already does here. Members are kept as the raw comma-separated
 * text rather than split on load: the list is short, it is written back
 * verbatim, and testing membership is a walk either way.
 *
 * A group of the account's own name and gid exists for every user -- Debian's
 * user private group scheme, which is what espix already had by accident when
 * every gid equalled its uid. Anything else somebody belongs to is
 * supplementary and lives only here.
 */
typedef struct {
    char        name[ESPIX_USER_MAX];
    espix_gid_t gid;
    char        members[MEMBERS_MAX];
} group_t;

static bool parse_group_line(char *line, group_t *out)
{
    memset(out, 0, sizeof(*out));

    char *name    = strtok(line, ":");
    char *gid     = strtok(NULL, ":\r\n");
    char *members = strtok(NULL, ":\r\n");

    if (name == NULL || !parse_id(gid, &out->gid)) {
        return false;
    }

    strlcpy(out->name, name, sizeof(out->name));
    if (members != NULL) {
        strlcpy(out->members, members, sizeof(out->members));
    }
    return true;
}

static size_t groups_load(group_t *out, size_t max)
{
    espix_fs_priv_begin();
    FILE *f = fopen(GROUP_PATH, "r");
    espix_fs_priv_end();

    if (f == NULL) {
        return 0;
    }

    char   line[LINE_MAX_LEN];
    size_t n = 0;

    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (parse_group_line(line, &out[n])) {
            n++;
        }
    }

    fclose(f);
    return n;
}

static esp_err_t groups_store(const group_t *g, size_t count)
{
    espix_fs_priv_begin();
    FILE *f = fopen(GROUP_PATH, "w");
    espix_fs_priv_end();

    if (f == NULL) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < count; i++) {
        fprintf(f, "%s:%u:%s\n", g[i].name, (unsigned)g[i].gid, g[i].members);
    }

    cache_invalidate();
    return (fclose(f) == 0) ? ESP_OK : ESP_FAIL;
}

/* Is `user` named in a comma-separated member list? */
static bool member_of(const char *members, const char *user)
{
    const size_t len = strlen(user);
    const char  *p   = members;

    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        const size_t seg  = (comma != NULL) ? (size_t)(comma - p) : strlen(p);

        if (seg == len && strncmp(p, user, len) == 0) {
            return true;
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    return false;
}

const char *espix_auth_group_name(espix_gid_t gid)
{
    /*
     * File scope, not function statics, so cache_invalidate() can reach it.
     * As locals this went stale in a way that took a specific sequence to see:
     * next_free_id() hands out the lowest free number, so `groupdel shared`
     * followed by `groupadd other` reuses the same gid, and `ls -l` went on
     * printing the group that no longer existed.
     */
    if (s_group_cache_valid && s_group_cache_gid == gid) {
        return s_group_cache;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }

    const size_t count = groups_load(g, MAX_GROUPS);
    const char  *found = NULL;

    for (size_t i = 0; i < count; i++) {
        if (g[i].gid == gid) {
            strlcpy(s_group_cache, g[i].name, sizeof(s_group_cache));
            s_group_cache_gid   = gid;
            s_group_cache_valid = true;
            found               = s_group_cache;
            break;
        }
    }

    free(g);
    return found;
}

bool espix_auth_group_id(const char *name, espix_gid_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return false;
    }

    const size_t count = groups_load(g, MAX_GROUPS);
    bool         found = false;

    for (size_t i = 0; i < count && !found; i++) {
        if (strcmp(g[i].name, name) == 0) {
            *out  = g[i].gid;
            found = true;
        }
    }

    free(g);
    return found;
}

size_t espix_auth_groups(const char *user, espix_gid_t *out, size_t max)
{
    if (user == NULL || out == NULL || max == 0) {
        return 0;
    }

    size_t n = 0;

    /* The primary group first, from the account record, so a user with no
     * /etc/group at all still has the gid their files are stamped with. */
    espix_user_t account;
    if (espix_auth_lookup(user, &account) == ESP_OK) {
        out[n++] = account.gid;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return n;
    }

    const size_t count = groups_load(g, MAX_GROUPS);

    for (size_t i = 0; i < count && n < max; i++) {
        if (!member_of(g[i].members, user)) {
            continue;
        }
        bool already = false;
        for (size_t j = 0; j < n; j++) {
            already = already || (out[j] == g[i].gid);
        }
        if (!already) {
            out[n++] = g[i].gid;
        }
    }

    free(g);
    return n;
}

bool espix_auth_in_group(const char *user, const char *group)
{
    if (user == NULL || group == NULL) {
        return false;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return false;
    }

    const size_t count = groups_load(g, MAX_GROUPS);
    bool         found = false;

    for (size_t i = 0; i < count && !found; i++) {
        if (strcmp(g[i].name, group) != 0) {
            continue;
        }
        /* Either named in the list, or it is this account's own group. */
        found = member_of(g[i].members, user);
        if (!found) {
            espix_user_t account;
            found = espix_auth_lookup(user, &account) == ESP_OK &&
                    account.gid == g[i].gid;
        }
    }

    free(g);
    return found;
}

/* ------------------------------------------------------------------ */
/* Making and unmaking accounts                                        */
/* ------------------------------------------------------------------ */

/*
 * The lowest free id at or above `from`, searching both files.
 *
 * Both, not one: a user private group means a uid and a gid of the same number,
 * so an id taken by either is taken by both. Allocating them apart is how you
 * end up with a group whose gid belongs to somebody else's account.
 */
static bool next_free_id(uint16_t from, uint16_t to, uint16_t *out)
{
    record_t *recs = calloc(MAX_ACCOUNTS, sizeof(*recs));
    group_t  *grps = calloc(MAX_GROUPS, sizeof(*grps));

    if (recs == NULL || grps == NULL) {
        free(recs);
        free(grps);
        return false;
    }

    const size_t nrec = load_all(recs, MAX_ACCOUNTS);
    const size_t ngrp = groups_load(grps, MAX_GROUPS);
    bool         ok   = false;

    for (uint16_t id = from; id <= to && !ok; id++) {
        bool taken = false;
        for (size_t i = 0; i < nrec && !taken; i++) {
            taken = (recs[i].uid == id) || (recs[i].gid == id);
        }
        for (size_t i = 0; i < ngrp && !taken; i++) {
            taken = (grps[i].gid == id);
        }
        if (!taken) {
            *out = id;
            ok   = true;
        }
    }

    free(recs);
    free(grps);
    return ok;
}

static bool id_range(bool system, uint16_t *from, uint16_t *to)
{
    *from = system ? ESPIX_UID_SYSTEM_FIRST : ESPIX_UID_FIRST;
    *to   = system ? ESPIX_UID_SYSTEM_LAST  : (ESPIX_UID_NOBODY - 1);
    return true;
}

esp_err_t espix_auth_group_add(const char *name, bool system)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= ESPIX_USER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = groups_load(g, MAX_GROUPS);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(g[i].name, name) == 0) {
            free(g);
            return ESP_ERR_INVALID_STATE;   /* already there */
        }
    }
    if (count == MAX_GROUPS) {
        free(g);
        return ESP_ERR_NO_MEM;
    }

    uint16_t from, to, gid;
    id_range(system, &from, &to);
    if (!next_free_id(from, to, &gid)) {
        free(g);
        return ESP_ERR_NOT_FOUND;           /* the range is full */
    }

    strlcpy(g[count].name, name, sizeof(g[count].name));
    g[count].gid        = gid;
    g[count].members[0] = '\0';
    count++;

    const esp_err_t err = groups_store(g, count);
    free(g);

    if (err == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "added group '%s' (gid %u)", name,
                   (unsigned)gid);
    }
    return err;
}

esp_err_t espix_auth_group_del(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count   = groups_load(g, MAX_GROUPS);
    size_t out     = 0;
    bool   removed = false;

    for (size_t i = 0; i < count; i++) {
        if (strcmp(g[i].name, name) == 0) {
            removed = true;
            continue;
        }
        if (out != i) {
            g[out] = g[i];
        }
        out++;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (removed) {
        err = groups_store(g, out);
    }

    free(g);
    return err;
}

/* Add or remove `user` everywhere a member list names them. */
/*
 * Take `user` out of every member list in `g`, in memory only.
 *
 * Separated from storing it deliberately. This used to load, edit and commit in
 * one call, which meant espix_auth_set_groups() replacing somebody's groups had
 * already written the removals to flash before it discovered that one of the
 * groups being asked for did not exist -- so `usermod -G sduo esp` stripped esp
 * of everything, sudo included, and then reported failure. On a device whose
 * root is locked that is the serial console or nothing.
 *
 * A caller that edits then stores once cannot half-do it.
 */
static void membership_strip(group_t *g, size_t count, const char *user)
{
    for (size_t i = 0; i < count; i++) {
        if (!member_of(g[i].members, user)) {
            continue;
        }

        /* Rebuild the list without this name rather than splicing the string,
         * which is where the off-by-one comma bugs live. */
        char   rebuilt[MEMBERS_MAX] = { 0 };
        size_t n                    = 0;
        const char *p               = g[i].members;

        while (*p != '\0') {
            const char  *comma = strchr(p, ',');
            const size_t seg   = (comma != NULL) ? (size_t)(comma - p)
                                                 : strlen(p);

            if (!(seg == strlen(user) && strncmp(p, user, seg) == 0)) {
                n += (size_t)snprintf(rebuilt + n, sizeof(rebuilt) - n,
                                      "%s%.*s", (n > 0) ? "," : "",
                                      (int)seg, p);
            }
            if (comma == NULL) {
                break;
            }
            p = comma + 1;
        }

        strlcpy(g[i].members, rebuilt, sizeof(g[i].members));
    }
}

/* The committing form, for callers with nothing else to change. */
static esp_err_t membership_purge(const char *user)
{
    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t    count = groups_load(g, MAX_GROUPS);
    membership_strip(g, count, user);

    const esp_err_t err = groups_store(g, count);
    free(g);
    return err;
}

esp_err_t espix_auth_set_groups(const char *user, const char *csv, bool append)
{
    if (user == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    espix_user_t account;
    if (espix_auth_lookup(user, &account) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t count = groups_load(g, MAX_GROUPS);
    esp_err_t    err   = ESP_OK;

    /*
     * Everything below happens to the loaded copy, and the file is written once
     * at the end. A name that does not resolve therefore changes nothing at
     * all, which is what a command that reports failure should have done.
     */
    if (!append) {
        membership_strip(g, count, user);
    }

    char list[MEMBERS_MAX];
    strlcpy(list, (csv != NULL) ? csv : "", sizeof(list));

    for (char *name = strtok(list, ","); name != NULL && err == ESP_OK;
         name = strtok(NULL, ",")) {
        bool matched = false;

        for (size_t i = 0; i < count && !matched; i++) {
            if (strcmp(g[i].name, name) != 0) {
                continue;
            }
            matched = true;
            if (member_of(g[i].members, user)) {
                continue;               /* already in it */
            }
            const size_t used = strlen(g[i].members);
            if (snprintf(g[i].members + used, sizeof(g[i].members) - used,
                         "%s%s", (used > 0) ? "," : "", user)
                >= (int)(sizeof(g[i].members) - used)) {
                err = ESP_ERR_NO_MEM;
            }
        }

        if (!matched) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "no group '%s'", name);
            err = ESP_ERR_NOT_FOUND;
        }
    }

    if (err == ESP_OK) {
        err = groups_store(g, count);
    }

    free(g);
    return err;
}

/*
 * Create an account's home directory, owned by the account.
 *
 * Shared by `useradd` and by the seeding of the default account, which is the
 * point: the default account used to be the one exception, and /home/esp existed
 * only because the rootfs image happened to ship an empty directory there. A
 * device whose filesystem was rebuilt from an image without it -- which is now
 * every device, since fsroot/ ships nothing -- had no home for its only user,
 * so SSH started at / and `scp file host:` with no remote path had nowhere to
 * land. Whoever creates an account creates its home.
 *
 * Raised over the mkdir alone: this runs at boot before there is any session to
 * be checked, and from `useradd`, which is root's. EEXIST is the ordinary case
 * on every boot after the first and says nothing worth logging.
 */
static void ensure_home(const char *home, espix_uid_t uid)
{
    if (home == NULL || home[0] == '\0' || strcmp(home, "/") == 0) {
        return;                 /* a service account, or root */
    }

    espix_fs_priv_begin();
    const int rc = mkdir(home, 0755);
    espix_fs_priv_end();

    if (rc == 0) {
        (void)espix_fs_chown(home, uid, uid);
    } else if (errno != EEXIST) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot create %s: %s", home,
                   strerror(errno));
    }
}

esp_err_t espix_auth_user_add(const char *name, bool system, bool make_home)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= ESPIX_USER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    record_t existing;
    if (find_record(name, &existing)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t from, to, uid;
    id_range(system, &from, &to);
    if (!next_free_id(from, to, &uid)) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Home is /home/<name> for a person and / for a service, which is what root
     * already uses. A service account has nowhere of its own to be, and giving
     * it a directory that does not exist would only make apply_account() warn
     * at every login it is not going to have.
     */
    char home[64];
    if (system) {
        strlcpy(home, "/", sizeof(home));
    } else {
        snprintf(home, sizeof(home), "/home/%s", name);
    }

    /*
     * Created locked, with no password, which is what useradd(8) does: the
     * account exists and nothing can authenticate as it until `passwd` gives it
     * one. It also keeps the only command that takes a password on the command
     * line down to that one.
     */
    const esp_err_t err = write_record(name, NULL, uid, uid, home);
    if (err != ESP_OK) {
        return err;
    }

    /* The user's own group, same name and id -- Debian's private groups, and
     * what espix already had when every gid equalled its uid. */
    group_t *g = calloc(MAX_GROUPS, sizeof(*g));
    if (g == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: no memory for its group; it has a gid nothing names",
                   name);
    } else {
        size_t count = groups_load(g, MAX_GROUPS);

        if (count < MAX_GROUPS) {
            strlcpy(g[count].name, name, sizeof(g[count].name));
            g[count].gid = uid;
            count++;
            (void)groups_store(g, count);
        } else {
            /* The account is real and usable; only the name for its gid is
             * missing, so `groups` and `ls -l` will show a number. Said out
             * loud rather than left to be discovered in a listing. */
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: /etc/group is full, so gid %u has no name", name,
                       (unsigned)uid);
        }
        free(g);
    }

    if (make_home && !system) {
        ensure_home(home, uid);
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "added %s '%s' (uid %u)",
               system ? "system account" : "account", name, (unsigned)uid);
    return ESP_OK;
}

esp_err_t espix_auth_user_del(const char *name, bool remove_home)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    record_t rec;
    if (!find_record(name, &rec)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (rec.uid == ESPIX_UID_ROOT) {
        return ESP_ERR_NOT_ALLOWED;     /* uid 0 is not somebody's account */
    }

    record_t *recs = calloc(MAX_ACCOUNTS, sizeof(*recs));
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t count = load_all(recs, MAX_ACCOUNTS);
    size_t       out   = 0;

    for (size_t i = 0; i < count; i++) {
        if (strcmp(recs[i].name, name) == 0) {
            continue;
        }
        if (out != i) {
            recs[out] = recs[i];
        }
        out++;
    }

    const esp_err_t err = store_all(recs, out);
    free(recs);

    if (err != ESP_OK) {
        return err;
    }

    (void)membership_purge(name);
    (void)espix_auth_group_del(name);       /* its private group */

    if (remove_home && rec.home[0] != '\0' && strcmp(rec.home, "/") != 0) {
        espix_fs_priv_begin();
        const esp_err_t rm = espix_fs_rm_rf(rec.home);
        espix_fs_priv_end();

        if (rm != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "cannot remove %s", rec.home);
        }
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "removed account '%s'", name);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Who may become root                                                 */
/* ------------------------------------------------------------------ */

bool espix_auth_may_sudo(const char *user)
{
    if (user == NULL || user[0] == '\0') {
        return false;
    }

    espix_fs_priv_begin();
    FILE *f = fopen(SUDOERS_PATH, "r");
    espix_fs_priv_end();

    if (f == NULL) {
        return false;       /* no list is an empty list, not an open door */
    }

    char line[ESPIX_USER_MAX + 8];
    bool found = false;

    while (!found && fgets(line, sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        line[strcspn(line, " \t\r\n")] = '\0';

        if (line[0] == '\0') {
            continue;
        }

        /* `%name` is a group, as in a real sudoers file; anything else is an
         * account. Debian ships %sudo, RHEL ships %wheel, and espix seeds the
         * former -- but a bare name still works, because granting one account
         * without inventing a group for it is ordinary. */
        found = (line[0] == '%') ? espix_auth_in_group(user, line + 1)
                                 : (strcmp(line, user) == 0);
    }

    fclose(f);
    return found;
}

/*
 * Seed the list with the default account, the way a Linux installer puts the
 * first user in the `sudo` group.
 *
 * Without this a shipped device has a locked root and no way to reach it except
 * the serial console -- which is defensible, but is not what anyone expects
 * from the account the device tells them to log in as.
 */
static void ensure_sudoers(void)
{
    espix_fs_priv_begin();
    FILE *probe = fopen(SUDOERS_PATH, "r");
    espix_fs_priv_end();

    if (probe != NULL) {
        fclose(probe);
        return;
    }

    espix_fs_priv_begin();
    FILE *f = fopen(SUDOERS_PATH, "w");
    espix_fs_priv_end();

    if (f == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot create %s", SUDOERS_PATH);
        return;
    }

    fprintf(f, "# Who may run commands as root: an account name per line,\n");
    fprintf(f, "# or %%name for every member of a group.\n");
    fprintf(f, "%%%s\n", SUDO_GROUP);
    fclose(f);

    const esp_err_t err = espix_fs_chmod(SUDOERS_PATH, 0600);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot restrict %s: %s",
                   SUDOERS_PATH, esp_err_to_name(err));
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "created %s granting group '%s'",
               SUDOERS_PATH, SUDO_GROUP);
}

/*
 * Seed /etc/group, if there is none.
 *
 * root's own group, the sudo group with the default account in it, and the
 * default account's private group. That is the shape a Debian install has after
 * its first user is created, and it is what makes the seeded `%sudo` in
 * /etc/sudoers grant anything.
 */
static void ensure_groups(void)
{
    espix_fs_priv_begin();
    FILE *probe = fopen(GROUP_PATH, "r");
    espix_fs_priv_end();

    if (probe != NULL) {
        fclose(probe);
        return;
    }

    group_t *g = calloc(3, sizeof(*g));
    if (g == NULL) {
        return;
    }

    strlcpy(g[0].name, ROOT_USER, sizeof(g[0].name));
    g[0].gid = ESPIX_UID_ROOT;

    strlcpy(g[1].name, SUDO_GROUP, sizeof(g[1].name));
    g[1].gid = SUDO_GID;
    strlcpy(g[1].members, DEFAULT_USER, sizeof(g[1].members));

    strlcpy(g[2].name, DEFAULT_USER, sizeof(g[2].name));
    g[2].gid = ESPIX_UID_FIRST;

    if (groups_store(g, 3) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "created %s with '%s' in '%s'",
                   GROUP_PATH, DEFAULT_USER, SUDO_GROUP);
    }

    free(g);

    /*
     * Readable by everyone, unlike /etc/passwd. It holds no secrets, and `ls -l`
     * resolving a gid to a name should not need privilege for it.
     */
    (void)espix_fs_chmod(GROUP_PATH, 0644);
}

esp_err_t espix_auth_init(void)
{
    record_t rec;

    /* Before anything verifies a password with it. One iteration, so the cost
     * is a rounding error against the 20000 a login pays. */
    pbkdf2_selftest();

    if (find_record(DEFAULT_USER, &rec)) {
        /* Detect the shipped password so we can nag, without storing a marker:
         * verifying it is cheap and cannot be wrong. */
        s_is_default = espix_auth_verify(DEFAULT_USER, DEFAULT_PASS);
    } else {
        const esp_err_t err = write_record(DEFAULT_USER, DEFAULT_PASS,
                                           ESPIX_UID_FIRST, ESPIX_UID_FIRST,
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

    /* Every boot, not only the one that creates the account: the record and the
     * directory are separate pieces of state, and a filesystem replaced under a
     * surviving /etc/passwd would otherwise leave the account homeless. Cheap --
     * mkdir returning EEXIST is the usual answer. */
    ensure_home(DEFAULT_HOME, ESPIX_UID_FIRST);

    /*
     * root, locked.
     *
     * The console has always called itself root; this is what makes that a
     * lookup rather than a string. Locked means no password can match it, so
     * adding it opens no door -- SSH still has nothing to let in, and the
     * console never authenticated in the first place. `passwd root` from the
     * console would unlock it, which is a deliberate act and reads like one.
     */
    if (!find_record(ROOT_USER, &rec)) {
        const esp_err_t err = write_record(ROOT_USER, NULL, ESPIX_UID_ROOT,
                                           ESPIX_UID_ROOT, ROOT_HOME);
        if (err != ESP_OK) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot add '%s' to %s: %s",
                       ROOT_USER, PASSWD_PATH, esp_err_to_name(err));
            return err;
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "added locked account '%s' (uid %d)",
                   ROOT_USER, ESPIX_UID_ROOT);
    }

    ensure_groups();
    ensure_sudoers();

    /*
     * Nobody but root reads this file directly.
     *
     * The hashes are in it -- espix has no /etc/shadow -- so leaving it
     * world-readable would put every account's PBKDF2 material in reach of any
     * shell user, which is the exact problem the shadow split was invented for.
     * espix does not need the split because nothing outside this component
     * opens the file: lookups, logins and `passwd` all come through here and
     * raise privilege for the open.
     */
    const esp_err_t mode_err = espix_fs_chmod(PASSWD_PATH, 0600);
    if (mode_err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot restrict %s: %s",
                   PASSWD_PATH, esp_err_to_name(mode_err));
    }

    /*
     * Hand espix_fs the ownership rule now that the account file is known good.
     * Until this point every path answers "root", which is what boot needs: the
     * rootfs is root's, and nothing should be owned by an account espix has not
     * finished reading yet.
     */
    espix_fs_set_owner_rule(owner_rule);

    if (s_is_default) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "user '%s' still has the default password; run 'passwd'",
                   DEFAULT_USER);
    }

    return ESP_OK;
}
