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
 * An older three-field line -- name:hash:home, from before there were uids --
 * still parses. It is given a uid by name and the file is rewritten once, on
 * the next boot; see migrate().
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
#include "espix_fs.h"
#include "espix_kernel.h"

#define TAG "auth"

#define PASSWD_PATH     "/etc/passwd"
/*
 * Long enough for the widest record the format allows: a 16-character name, the
 * algorithm tag, 20000, a 32-hex salt, a 64-hex hash, uid, gid and a 63-byte
 * home is 213 bytes. The old 192 predated uid and gid and was already one long
 * name away from truncating a hash.
 */
#define LINE_MAX_LEN    256
#define SALT_LEN        16
#define HASH_LEN        32
#define PBKDF2_ITERS    20000   /* ~100ms on a 240MHz S3; tune if login drags */

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

static void cache_invalidate(void)
{
    s_accounts_valid   = false;
    s_name_cache_valid = false;
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
    char        name[ESPIX_USER_MAX];
    uint32_t    iters;
    uint8_t     salt[SALT_LEN];
    uint8_t     hash[HASH_LEN];
    char        home[64];
    espix_uid_t uid;
    espix_gid_t gid;
    bool        locked;     /* no password will match; still a real account */
    bool        legacy;     /* parsed from a pre-uid line, so needs rewriting */
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

/* name:$pbkdf2-sha256$iters$salthex$hashhex:uid:gid:home, or the older
 * name:hash:home. */
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
    char *f3   = strtok(NULL, ":\r\n");
    char *f4   = strtok(NULL, ":\r\n");
    char *f5   = strtok(NULL, ":\r\n");

    if (name == NULL || hash == NULL) {
        return false;
    }
    strlcpy(out->name, name, sizeof(out->name));

    if (f4 == NULL) {
        /*
         * Pre-uid: name:hash:home. Give it what it lacks rather than refusing
         * it, or an upgrade would lock the only account out of its own device.
         */
        out->legacy = true;
        out->uid    = (strcmp(name, ROOT_USER) == 0) ? ESPIX_UID_ROOT
                                                     : ESPIX_UID_FIRST;
        out->gid    = out->uid;
        strlcpy(out->home, f3 != NULL ? f3 : "/", sizeof(out->home));
    } else {
        if (!parse_id(f3, &out->uid) || !parse_id(f4, &out->gid)) {
            return false;
        }
        strlcpy(out->home, f5 != NULL ? f5 : "/", sizeof(out->home));
    }

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

    /*
     * Keep the account's existing identity; only the password changes. An
     * unknown user gets the next free uid rather than inheriting root's.
     */
    record_t    rec;
    const bool  known = find_record(user, &rec);
    const char *home  = known ? rec.home : "/";
    const espix_uid_t uid = known ? rec.uid : ESPIX_UID_FIRST;
    const espix_gid_t gid = known ? rec.gid : ESPIX_UID_FIRST;

    const esp_err_t err = write_record(user, password, uid, gid, home);
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
 * True if `abs_path` is `dir` or something underneath it.
 *
 * The trailing check is what stops /home/esp claiming /home/espix: a prefix
 * match alone would hand one account's files to another whose name it happens
 * to begin with.
 */
static bool path_within(const char *abs_path, const char *dir)
{
    if (dir[0] == '\0') {
        return false;
    }
    if (strcmp(dir, "/") == 0) {
        return true;            /* root's home; contains everything */
    }

    const size_t len = strlen(dir);
    if (strncmp(abs_path, dir, len) != 0) {
        return false;
    }
    return abs_path[len] == '\0' || abs_path[len] == '/';
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
        if ((!found || len > best) && path_within(abs_path, s_accounts[i].home)) {
            best   = len;
            *uid   = s_accounts[i].uid;
            *gid   = s_accounts[i].gid;
            found  = true;
        }
    }

    return found;
}

/*
 * Rewrite /etc/passwd once, if anything in it predates uids.
 *
 * parse_line() already invented a uid for such a record, so the system works
 * either way; this is what stops it inventing the same one on every boot and
 * makes `cat /etc/passwd` agree with what espix believes.
 */
static void migrate(void)
{
    record_t *recs = calloc(MAX_ACCOUNTS, sizeof(*recs));
    if (recs == NULL) {
        return;
    }

    const size_t count = load_all(recs, MAX_ACCOUNTS);
    size_t       stale = 0;

    for (size_t i = 0; i < count; i++) {
        if (recs[i].legacy) {
            stale++;
        }
    }

    if (stale > 0 && store_all(recs, count) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "%s: gave %u account%s a uid", PASSWD_PATH,
                   (unsigned)stale, stale == 1 ? "" : "s");
    }

    free(recs);
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

    migrate();

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
