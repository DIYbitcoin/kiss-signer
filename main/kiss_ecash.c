// Cashu ecash keys. See kiss_ecash.h.
#include "kiss_ecash.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <secp256k1.h>
#include <wally_bip32.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_crypto.h"
#include "kiss_sp.h"
#include "kiss_wipe.h"

int kiss_ecash_account(const struct ext_key *master, struct ext_key *out)
{
    if (!master || !out)
        return 1;
    const uint32_t path[4] = {
        BIP32_INITIAL_HARDENED_CHILD + KISS_ECASH_PURPOSE,
        BIP32_INITIAL_HARDENED_CHILD + KISS_ECASH_ACCOUNT,
        BIP32_INITIAL_HARDENED_CHILD + 0,
        BIP32_INITIAL_HARDENED_CHILD + 0,
    };
    if (bip32_key_from_parent_path(master, path, 4, BIP32_FLAG_KEY_PRIVATE,
                                   out) != WALLY_OK) {
        wally_bzero(out, sizeof *out);
        return 2;
    }
    return 0;
}

int kiss_ecash_pubkey(const struct ext_key *account, uint32_t counter,
                      uint8_t pub33[33])
{
    // A hardened counter is not NUT-13's key, and from a public account it
    // could not be derived at all; refuse it rather than hand back a key no
    // watch only wallet would ever lock to.
    if (!account || !pub33 || counter >= BIP32_INITIAL_HARDENED_CHILD)
        return 1;
    struct ext_key child;
    int rc = bip32_key_from_parent(account, counter, BIP32_FLAG_KEY_PUBLIC,
                                   &child) == WALLY_OK ? 0 : 2;
    if (rc == 0)
        memcpy(pub33, child.pub_key, 33);
    wally_bzero(&child, sizeof child);
    return rc;
}

int kiss_session_ecash_export(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return 1;
    out[0] = 0;
    const struct ext_key *master = kiss_session_master();
    if (!master)
        return 2;   // locked, or the keys are on a card
    struct ext_key acct;
    if (kiss_ecash_account(master, &acct))
        return 3;
    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    int rc = bip32_key_get_fingerprint((struct ext_key *)master, fp,
                                       sizeof fp) == WALLY_OK ? 0 : 4;
    char *xpub = NULL;
    if (rc == 0) {
        acct.version = BIP32_VER_MAIN_PRIVATE;   // network free: always xpub
        if (bip32_key_to_base58(&acct, BIP32_FLAG_KEY_PUBLIC, &xpub) != WALLY_OK)
            rc = 5;
    }
    if (rc == 0) {
        int n = snprintf(out, out_len, "[%02x%02x%02x%02x/%uh/%uh/0h/0h]%s",
                         fp[0], fp[1], fp[2], fp[3],
                         KISS_ECASH_PURPOSE, KISS_ECASH_ACCOUNT, xpub);
        if (n < 0 || (size_t)n >= out_len) {
            out[0] = 0;
            rc = 6;
        }
    }
    if (xpub)
        wally_free_string(xpub);
    wally_bzero(&acct, sizeof acct);
    return rc;
}

// ---- NUT-00 and NUT-11 primitives ----

static const char HTC_DOMAIN[] = "Secp256k1_HashToCurve_Cashu_";

int kiss_ecash_hash_to_curve(const uint8_t *x, size_t x_len, uint8_t y33[33])
{
    if (!x || !y33 || x_len > KISS_ECASH_SECRET_MAX)
        return 1;
    const secp256k1_context *ctx = wally_get_secp_context();
    uint8_t buf[sizeof HTC_DOMAIN - 1 + KISS_ECASH_SECRET_MAX];
    memcpy(buf, HTC_DOMAIN, sizeof HTC_DOMAIN - 1);
    memcpy(buf + sizeof HTC_DOMAIN - 1, x, x_len);
    uint8_t m[32 + 4];
    if (wally_sha256(buf, sizeof HTC_DOMAIN - 1 + x_len, m, 32) != WALLY_OK)
        return 2;
    // The spec's counter is a uint32; a valid point turns up within a few
    // tries, so the loop is bounded far below that.
    for (uint32_t c = 0; c < 65536; c++) {
        m[32] = (uint8_t)c;
        m[33] = (uint8_t)(c >> 8);
        m[34] = (uint8_t)(c >> 16);
        m[35] = (uint8_t)(c >> 24);
        uint8_t cand[33];
        cand[0] = 0x02;
        if (wally_sha256(m, sizeof m, cand + 1, 32) != WALLY_OK)
            return 2;
        secp256k1_pubkey pk;
        if (secp256k1_ec_pubkey_parse(ctx, &pk, cand, 33)) {
            memcpy(y33, cand, 33);
            return 0;
        }
    }
    return 3;
}

int kiss_ecash_blind(const uint8_t *x, size_t x_len, const uint8_t r32[32],
                     uint8_t b33[33])
{
    if (!r32 || !b33)
        return 1;
    uint8_t y[33];
    if (kiss_ecash_hash_to_curve(x, x_len, y))
        return 2;
    const secp256k1_context *ctx = wally_get_secp_context();
    secp256k1_pubkey pk;
    size_t len = 33;
    // r must be a scalar in [1, n). r >= n is refused by tweak_add, but r = 0
    // is not: it gives B_ = Y, the unblinded point, which shows the mint the
    // secret's point and links the output to its later spend. seckey_verify
    // refuses both.
    if (!secp256k1_ec_seckey_verify(ctx, r32) ||
        !secp256k1_ec_pubkey_parse(ctx, &pk, y, 33) ||
        !secp256k1_ec_pubkey_tweak_add(ctx, &pk, r32) ||
        !secp256k1_ec_pubkey_serialize(ctx, b33, &len, &pk,
                                       SECP256K1_EC_COMPRESSED))
        return 3;
    return 0;
}

// 66 lowercase hex characters of a compressed point. Uppercase is refused,
// not folded: the mint hashes the string it receives, so a signature over
// a folded copy would not verify there.
static int point_hex_ok(const char *h)
{
    if (!h || strlen(h) != 66 || h[0] != '0' || (h[1] != '2' && h[1] != '3'))
        return 0;
    for (int i = 0; i < 66; i++) {
        char c = h[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

// A secret is a string the mint stores and hashes; control characters and
// NULs have no place in one, and a NUL would end it early here.
static size_t secret_len_ok(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n == 0 || n > KISS_ECASH_SECRET_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)s[i] < 0x20 || s[i] == 0x7f)
            return 0;
    return n;
}

static int put(uint8_t *buf, size_t *at, const void *src, size_t n)
{
    if (n > KISS_ECASH_MSG_MAX - *at)
        return 1;
    memcpy(buf + *at, src, n);
    *at += n;
    return 0;
}

int kiss_ecash_sigall_hash(const kiss_ecash_in_t *in, size_t n_in,
                           const kiss_ecash_out_t *out, size_t n_out,
                           const char *quote_id, uint8_t hash32[32])
{
    if (!in || n_in == 0 || (n_out && !out) || !hash32)
        return 1;
    uint8_t *msg = malloc(KISS_ECASH_MSG_MAX);
    if (!msg)
        return 5;
    size_t at = 0;
    int rc = 0;
    for (size_t i = 0; i < n_in && rc == 0; i++) {
        size_t sl = secret_len_ok(in[i].secret);
        if (!sl || !point_hex_ok(in[i].c_hex))
            rc = 2;
        else if (put(msg, &at, in[i].secret, sl) ||
                 put(msg, &at, in[i].c_hex, 66))
            rc = 3;
    }
    for (size_t i = 0; i < n_out && rc == 0; i++) {
        char amt[24];
        int n = snprintf(amt, sizeof amt, "%llu",
                         (unsigned long long)out[i].amount);
        if (!point_hex_ok(out[i].b_hex))
            rc = 2;
        else if (n <= 0 || put(msg, &at, amt, (size_t)n) ||
                 put(msg, &at, out[i].b_hex, 66))
            rc = 3;
    }
    if (rc == 0 && quote_id) {
        size_t ql = secret_len_ok(quote_id);
        if (!ql)
            rc = 2;
        else if (put(msg, &at, quote_id, ql))
            rc = 3;
    }
    if (rc == 0 && wally_sha256(msg, at, hash32, 32) != WALLY_OK)
        rc = 4;
    kiss_wipe(msg, at);
    free(msg);
    return rc;
}

// BIP340 over hash32 with the key at {counter}. Static on purpose: a public
// call that signs any hash would be a signing oracle beside the review.
static int ecash_sign(const struct ext_key *account, uint32_t counter,
                      const uint8_t hash32[32], uint8_t sig64[64])
{
    if (!account || !hash32 || !sig64 ||
        counter >= BIP32_INITIAL_HARDENED_CHILD ||
        account->priv_key[0] != BIP32_FLAG_KEY_PRIVATE)
        return 1;
    struct ext_key child;
    int rc = bip32_key_from_parent(account, counter, BIP32_FLAG_KEY_PRIVATE,
                                   &child) == WALLY_OK ? 0 : 2;
    if (rc == 0 && sp_schnorr_sign(child.priv_key + 1, hash32, sig64) != 0)
        rc = 3;
    if (rc != 0)
        kiss_wipe(sig64, 64);
    wally_bzero(&child, sizeof child);
    return rc;
}

// ---- spend request (docs/specs/ecash-spend-request.md) ----

// A strict reader for the one JSON shape the spec allows. No floats, no
// negatives, no escapes but \" and \\, no duplicate or unknown keys: a string
// this signer reads one way must not be readable another way by the mint.
typedef struct {
    const char *s;
    size_t n, i;
} jr_t;

static void jr_ws(jr_t *j)
{
    while (j->i < j->n && (j->s[j->i] == ' ' || j->s[j->i] == '\t' ||
                           j->s[j->i] == '\n' || j->s[j->i] == '\r'))
        j->i++;
}

static bool jr_eat(jr_t *j, char c)
{
    jr_ws(j);
    if (j->i < j->n && j->s[j->i] == c) {
        j->i++;
        return true;
    }
    return false;
}

// A string into out (NUL terminated, at most cap - 1 bytes). allow_esc lets
// \" and \\ through, decoded; every other escape, a control byte or a string
// that does not fit is refused.
static bool jr_str(jr_t *j, char *out, size_t cap, size_t *len, bool allow_esc)
{
    if (!jr_eat(j, '"'))
        return false;
    size_t o = 0;
    while (j->i < j->n) {
        char c = j->s[j->i++];
        if (c == '"') {
            out[o] = 0;
            if (len)
                *len = o;
            return true;
        }
        if ((uint8_t)c < 0x20 || c == 0x7f)
            return false;
        if (c == '\\') {
            if (!allow_esc || j->i >= j->n)
                return false;
            c = j->s[j->i++];
            if (c != '"' && c != '\\')
                return false;
        }
        if (o + 1 >= cap)
            return false;
        out[o++] = c;
    }
    return false;
}

// A non negative integer at most 2^53, no leading zeros, no sign, no fraction.
static bool jr_uint(jr_t *j, uint64_t *out)
{
    jr_ws(j);
    size_t st = j->i;
    uint64_t v = 0;
    while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') {
        v = v * 10 + (uint64_t)(j->s[j->i] - '0');
        if (v > (1ull << 53))
            return false;
        j->i++;
    }
    size_t d = j->i - st;
    if (d == 0 || (d > 1 && j->s[st] == '0'))
        return false;
    // a fraction or exponent right after the digits is a float, not an int
    if (j->i < j->n && (j->s[j->i] == '.' || j->s[j->i] == 'e' ||
                        j->s[j->i] == 'E'))
        return false;
    *out = v;
    return true;
}

static bool jr_end(jr_t *j)
{
    jr_ws(j);
    return j->i == j->n;
}

static bool hex_lower(const char *h, size_t n)
{
    if (strlen(h) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
            return false;
    return true;
}

static void hex_of(const uint8_t *b, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
}

static bool unhex(const char *h, uint8_t *out, size_t n)
{
    if (!hex_lower(h, 2 * n))
        return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t hi = (uint8_t)(h[2 * i] <= '9' ? h[2 * i] - '0' : h[2 * i] - 'a' + 10);
        uint8_t lo = (uint8_t)(h[2 * i + 1] <= '9' ? h[2 * i + 1] - '0' : h[2 * i + 1] - 'a' + 10);
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

// The input lock: ["P2PK",{"nonce":"..","data":"..","tags":[["sigflag","SIG_ALL"]]}]
// in any key order, each key once, nothing else. No escapes inside: none of
// these values needs one.
static int parse_lock(const char *sec, size_t len, const char *our_hex)
{
    jr_t j = {sec, len, 0};
    char kind[8], key[8], val[132], data[67] = {0};
    bool seen_n = false, seen_d = false, seen_t = false;
    if (!jr_eat(&j, '[') || !jr_str(&j, kind, sizeof kind, NULL, false) ||
        !jr_eat(&j, ','))
        return KISS_ECASH_E_FORMAT;
    if (strcmp(kind, "P2PK") != 0)
        return KISS_ECASH_E_LOCK;
    if (!jr_eat(&j, '{'))
        return KISS_ECASH_E_FORMAT;
    do {
        if (!jr_str(&j, key, sizeof key, NULL, false) || !jr_eat(&j, ':'))
            return KISS_ECASH_E_FORMAT;
        if (!strcmp(key, "nonce") && !seen_n) {
            size_t vl;
            if (!jr_str(&j, val, sizeof val, &vl, false) || vl == 0)
                return KISS_ECASH_E_FORMAT;
            seen_n = true;
        } else if (!strcmp(key, "data") && !seen_d) {
            if (!jr_str(&j, data, sizeof data, NULL, false))
                return KISS_ECASH_E_FORMAT;
            seen_d = true;
        } else if (!strcmp(key, "tags") && !seen_t) {
            // exactly one tag, ["sigflag","SIG_ALL"]: a locktime, a refund
            // path, extra keys or SIG_INPUTS each change who can spend this
            // or what the signature covers
            char a[16], b[16];
            if (!jr_eat(&j, '[') || !jr_eat(&j, '[') ||
                !jr_str(&j, a, sizeof a, NULL, false) || !jr_eat(&j, ',') ||
                !jr_str(&j, b, sizeof b, NULL, false) || !jr_eat(&j, ']') ||
                !jr_eat(&j, ']'))
                return KISS_ECASH_E_LOCK;
            if (strcmp(a, "sigflag") != 0 || strcmp(b, "SIG_ALL") != 0)
                return KISS_ECASH_E_LOCK;
            seen_t = true;
        } else {
            return KISS_ECASH_E_FORMAT;   // unknown or repeated key
        }
    } while (jr_eat(&j, ','));
    if (!jr_eat(&j, '}') || !jr_eat(&j, ']') || !jr_end(&j))
        return KISS_ECASH_E_FORMAT;
    if (!seen_n || !seen_d)
        return KISS_ECASH_E_FORMAT;
    if (!seen_t)
        return KISS_ECASH_E_LOCK;     // no tags is SIG_INPUTS
    if (strcmp(data, our_hex) != 0)
        return KISS_ECASH_E_NOT_OURS;
    return KISS_ECASH_OK;
}

typedef struct {
    uint16_t off, len;          // the secret, in the pool
    char c[67];
} req_in_t;

typedef struct {
    uint64_t amount;
    char b[67];
    bool change;
    uint32_t counter;
    char nonce[65];
} req_out_t;

typedef struct {
    bool kept;
    uint32_t counter;
    uint8_t our_pub[33];        // the key every input is locked to, at review
    int n_in, n_out;
    req_in_t in[KISS_ECASH_MAX_IN];
    req_out_t out[KISS_ECASH_MAX_OUT];
    char pool[KISS_ECASH_REQ_MAX];
    size_t pool_used;
} req_t;

static req_t *s_req;

void kiss_ecash_review_clear(void)
{
    if (s_req) {
        kiss_wipe(s_req, sizeof *s_req);
        free(s_req);
        s_req = NULL;
    }
}

// The change secret this signer will accept, built here and never read from
// the phone: the only lock on a change output is one this code wrote.
static int change_secret(const struct ext_key *acct, uint32_t counter,
                         const char *nonce, char *out, size_t cap)
{
    uint8_t pub[33];
    char ph[67];
    if (kiss_ecash_pubkey(acct, counter, pub))
        return 1;
    hex_of(pub, 33, ph);
    int n = snprintf(out, cap,
                     "[\"P2PK\",{\"nonce\":\"%s\",\"data\":\"%s\","
                     "\"tags\":[[\"sigflag\",\"SIG_ALL\"]]}]", nonce, ph);
    return n > 0 && (size_t)n < cap ? 0 : 1;
}

static int parse_inputs(jr_t *j, req_t *r)
{
    if (!jr_eat(j, '['))
        return KISS_ECASH_E_FORMAT;
    do {
        if (r->n_in >= KISS_ECASH_MAX_IN || !jr_eat(j, '{'))
            return KISS_ECASH_E_FORMAT;
        req_in_t *in = &r->in[r->n_in];
        bool seen_s = false, seen_c = false;
        do {
            char key[8];
            if (!jr_str(j, key, sizeof key, NULL, false) || !jr_eat(j, ':'))
                return KISS_ECASH_E_FORMAT;
            if (!strcmp(key, "secret") && !seen_s) {
                size_t sl;
                char *dst = r->pool + r->pool_used;
                size_t room = sizeof r->pool - r->pool_used;
                if (room > KISS_ECASH_SECRET_MAX + 1)
                    room = KISS_ECASH_SECRET_MAX + 1;
                if (!jr_str(j, dst, room, &sl, true) || sl == 0)
                    return KISS_ECASH_E_FORMAT;
                in->off = (uint16_t)r->pool_used;
                in->len = (uint16_t)sl;
                r->pool_used += sl + 1;
                seen_s = true;
            } else if (!strcmp(key, "C") && !seen_c) {
                if (!jr_str(j, in->c, sizeof in->c, NULL, false))
                    return KISS_ECASH_E_FORMAT;
                seen_c = true;
            } else {
                return KISS_ECASH_E_FORMAT;
            }
        } while (jr_eat(j, ','));
        if (!jr_eat(j, '}') || !seen_s || !seen_c)
            return KISS_ECASH_E_FORMAT;
        r->n_in++;
    } while (jr_eat(j, ','));
    return jr_eat(j, ']') ? KISS_ECASH_OK : KISS_ECASH_E_FORMAT;
}

static int parse_change(jr_t *j, req_out_t *o, uint8_t r32[32])
{
    if (!jr_eat(j, '{'))
        return KISS_ECASH_E_FORMAT;
    bool seen_c = false, seen_n = false, seen_r = false;
    do {
        char key[8];
        if (!jr_str(j, key, sizeof key, NULL, false) || !jr_eat(j, ':'))
            return KISS_ECASH_E_FORMAT;
        if (!strcmp(key, "counter") && !seen_c) {
            uint64_t v;
            if (!jr_uint(j, &v) || v >= BIP32_INITIAL_HARDENED_CHILD)
                return KISS_ECASH_E_FORMAT;
            o->counter = (uint32_t)v;
            seen_c = true;
        } else if (!strcmp(key, "nonce") && !seen_n) {
            if (!jr_str(j, o->nonce, sizeof o->nonce, NULL, false) ||
                !hex_lower(o->nonce, 64))
                return KISS_ECASH_E_FORMAT;
            seen_n = true;
        } else if (!strcmp(key, "r") && !seen_r) {
            char rh[65];
            if (!jr_str(j, rh, sizeof rh, NULL, false) || !unhex(rh, r32, 32))
                return KISS_ECASH_E_FORMAT;
            seen_r = true;
        } else {
            return KISS_ECASH_E_FORMAT;
        }
    } while (jr_eat(j, ','));
    if (!jr_eat(j, '}') || !seen_c || !seen_n || !seen_r)
        return KISS_ECASH_E_FORMAT;
    o->change = true;
    return KISS_ECASH_OK;
}

static int parse_outputs(jr_t *j, req_t *r, const struct ext_key *acct)
{
    if (!jr_eat(j, '['))
        return KISS_ECASH_E_FORMAT;
    do {
        if (r->n_out >= KISS_ECASH_MAX_OUT || !jr_eat(j, '{'))
            return KISS_ECASH_E_FORMAT;
        req_out_t *o = &r->out[r->n_out];
        uint8_t rr[32];
        bool seen_a = false, seen_b = false, seen_ch = false;
        int rc = KISS_ECASH_OK;
        do {
            char key[8];
            if (!jr_str(j, key, sizeof key, NULL, false) || !jr_eat(j, ':')) {
                rc = KISS_ECASH_E_FORMAT;
            } else if (!strcmp(key, "amount") && !seen_a) {
                if (!jr_uint(j, &o->amount) || o->amount == 0)
                    rc = KISS_ECASH_E_FORMAT;
                seen_a = true;
            } else if (!strcmp(key, "B_") && !seen_b) {
                if (!jr_str(j, o->b, sizeof o->b, NULL, false))
                    rc = KISS_ECASH_E_FORMAT;
                seen_b = true;
            } else if (!strcmp(key, "change") && !seen_ch) {
                rc = parse_change(j, o, rr);
                seen_ch = true;
            } else {
                rc = KISS_ECASH_E_FORMAT;
            }
        } while (rc == KISS_ECASH_OK && jr_eat(j, ','));
        if (rc == KISS_ECASH_OK && (!jr_eat(j, '}') || !seen_a || !seen_b))
            rc = KISS_ECASH_E_FORMAT;
        if (rc == KISS_ECASH_OK && o->change) {
            // Rebuild the secret, recompute B_, and compare with the B_ that
            // will be signed. A claimed change output that does not match is
            // somebody else's output wearing a label.
            char sec[256], bh[67];
            uint8_t b[33];
            if (change_secret(acct, o->counter, o->nonce, sec, sizeof sec) ||
                kiss_ecash_blind((const uint8_t *)sec, strlen(sec), rr, b))
                rc = KISS_ECASH_E_CHANGE;
            else {
                hex_of(b, 33, bh);
                if (strcmp(bh, o->b) != 0)
                    rc = KISS_ECASH_E_CHANGE;
            }
            kiss_wipe(sec, sizeof sec);
        }
        kiss_wipe(rr, sizeof rr);
        if (rc != KISS_ECASH_OK)
            return rc;
        r->n_out++;
    } while (jr_eat(j, ','));
    return jr_eat(j, ']') ? KISS_ECASH_OK : KISS_ECASH_E_FORMAT;
}

static int review(const char *req, size_t len, kiss_ecash_summary_t *sum,
                  req_t *r, const struct ext_key *acct)
{
    jr_t j = {req, len, 0};
    bool seen_v = false, seen_m = false, seen_u = false, seen_c = false,
         seen_i = false, seen_o = false;
    if (!jr_eat(&j, '{'))
        return KISS_ECASH_E_FORMAT;
    int rc = KISS_ECASH_OK;
    do {
        char key[8];
        if (!jr_str(&j, key, sizeof key, NULL, false) || !jr_eat(&j, ':'))
            return KISS_ECASH_E_FORMAT;
        if (!strcmp(key, "v") && !seen_v) {
            uint64_t v;
            if (!jr_uint(&j, &v))
                return KISS_ECASH_E_FORMAT;
            if (v != 1)
                return KISS_ECASH_E_VERSION;
            seen_v = true;
        } else if (!strcmp(key, "mint") && !seen_m) {
            if (!jr_str(&j, sum->mint, sizeof sum->mint, NULL, false))
                return KISS_ECASH_E_FORMAT;
            seen_m = true;
        } else if (!strcmp(key, "unit") && !seen_u) {
            if (!jr_str(&j, sum->unit, sizeof sum->unit, NULL, false))
                return KISS_ECASH_E_FORMAT;
            seen_u = true;
        } else if (!strcmp(key, "counter") && !seen_c) {
            uint64_t v;
            if (!jr_uint(&j, &v) || v >= BIP32_INITIAL_HARDENED_CHILD)
                return KISS_ECASH_E_FORMAT;
            r->counter = (uint32_t)v;
            seen_c = true;
        } else if (!strcmp(key, "inputs") && !seen_i) {
            if ((rc = parse_inputs(&j, r)) != KISS_ECASH_OK)
                return rc;
            seen_i = true;
        } else if (!strcmp(key, "outputs") && !seen_o) {
            if ((rc = parse_outputs(&j, r, acct)) != KISS_ECASH_OK)
                return rc;
            seen_o = true;
        } else {
            return KISS_ECASH_E_FORMAT;
        }
    } while (jr_eat(&j, ','));
    if (!jr_eat(&j, '}') || !jr_end(&j) || !seen_v || !seen_m || !seen_u ||
        !seen_c || !seen_i || !seen_o)
        return KISS_ECASH_E_FORMAT;

    // Every input locked to the one key at counter, with only SIG_ALL.
    char our_hex[67];
    if (kiss_ecash_pubkey(acct, r->counter, r->our_pub))
        return KISS_ECASH_E_INTERNAL;
    hex_of(r->our_pub, 33, our_hex);
    for (int i = 0; i < r->n_in; i++) {
        rc = parse_lock(r->pool + r->in[i].off, r->in[i].len, our_hex);
        if (rc != KISS_ECASH_OK)
            return rc;
        for (int k = 0; k < i; k++)
            if (!strcmp(r->pool + r->in[i].off, r->pool + r->in[k].off))
                return KISS_ECASH_E_DUP;
    }

    // No change secret may repeat an input's or another change output's: the
    // mint would sign it, and the change could then never be redeemed.
    for (int i = 0; i < r->n_out; i++) {
        const req_out_t *o = &r->out[i];
        for (int k = 0; k < i; k++)
            if (!strcmp(o->b, r->out[k].b))
                return KISS_ECASH_E_DUP;
        if (!o->change)
            continue;
        char sec[256];
        if (change_secret(acct, o->counter, o->nonce, sec, sizeof sec))
            return KISS_ECASH_E_INTERNAL;
        for (int k = 0; k < r->n_in; k++)
            if (!strcmp(sec, r->pool + r->in[k].off))
                return KISS_ECASH_E_DUP;
        for (int k = 0; k < i; k++)
            if (r->out[k].change && r->out[k].counter == o->counter &&
                !strcmp(r->out[k].nonce, o->nonce))
                return KISS_ECASH_E_DUP;
    }

    // The message must build: every C and B_ is the exact hex the mint hashes.
    kiss_ecash_in_t ins[KISS_ECASH_MAX_IN];
    kiss_ecash_out_t outs[KISS_ECASH_MAX_OUT];
    for (int i = 0; i < r->n_in; i++)
        ins[i] = (kiss_ecash_in_t){r->pool + r->in[i].off, r->in[i].c};
    for (int i = 0; i < r->n_out; i++)
        outs[i] = (kiss_ecash_out_t){r->out[i].amount, r->out[i].b};
    uint8_t h[32];
    if (kiss_ecash_sigall_hash(ins, (size_t)r->n_in, outs, (size_t)r->n_out,
                               NULL, h))
        return KISS_ECASH_E_FORMAT;

    sum->counter = r->counter;
    sum->n_in = r->n_in;
    sum->n_out = r->n_out;
    for (int i = 0; i < r->n_out; i++) {
        if (r->out[i].change) {
            sum->n_change++;
            sum->change += r->out[i].amount;
        } else {
            sum->leaving += r->out[i].amount;
        }
    }
    return KISS_ECASH_OK;
}

int kiss_ecash_review(const char *req, size_t len, kiss_ecash_summary_t *sum)
{
    kiss_ecash_review_clear();
    if (!sum)
        return KISS_ECASH_E_INTERNAL;
    memset(sum, 0, sizeof *sum);
    const struct ext_key *master = kiss_session_master();
    if (!master)
        return KISS_ECASH_E_LOCKED;
    if (!req || len == 0 || len > KISS_ECASH_REQ_MAX)
        return KISS_ECASH_E_FORMAT;
    req_t *r = calloc(1, sizeof *r);
    struct ext_key acct;
    if (!r)
        return KISS_ECASH_E_INTERNAL;
    int rc = kiss_ecash_account(master, &acct) ? KISS_ECASH_E_INTERNAL
                                                : review(req, len, sum, r, &acct);
    wally_bzero(&acct, sizeof acct);
    if (rc != KISS_ECASH_OK) {
        kiss_wipe(r, sizeof *r);
        free(r);
        memset(sum, 0, sizeof *sum);
        return rc;
    }
    r->kept = true;
    s_req = r;
    return KISS_ECASH_OK;
}

int kiss_ecash_sign_reviewed(char *reply, size_t cap)
{
    if (reply && cap)
        reply[0] = 0;
    req_t *r = s_req;
    if (!r || !r->kept) {
        kiss_ecash_review_clear();
        return KISS_ECASH_E_NOTHING;
    }
    const struct ext_key *master = kiss_session_master();
    if (!master || !reply) {
        kiss_ecash_review_clear();
        return KISS_ECASH_E_LOCKED;
    }
    struct ext_key acct;
    uint8_t pub[33], h[32], sig[64];
    int rc = KISS_ECASH_OK;
    if (kiss_ecash_account(master, &acct) ||
        kiss_ecash_pubkey(&acct, r->counter, pub))
        rc = KISS_ECASH_E_INTERNAL;
    // The session can have been locked and reopened under another passphrase
    // since the review: the request was checked against those keys, not these.
    else if (memcmp(pub, r->our_pub, 33) != 0)
        rc = KISS_ECASH_E_SESSION;
    if (rc == KISS_ECASH_OK) {
        kiss_ecash_in_t ins[KISS_ECASH_MAX_IN];
        kiss_ecash_out_t outs[KISS_ECASH_MAX_OUT];
        for (int i = 0; i < r->n_in; i++)
            ins[i] = (kiss_ecash_in_t){r->pool + r->in[i].off, r->in[i].c};
        for (int i = 0; i < r->n_out; i++)
            outs[i] = (kiss_ecash_out_t){r->out[i].amount, r->out[i].b};
        if (kiss_ecash_sigall_hash(ins, (size_t)r->n_in, outs,
                                   (size_t)r->n_out, NULL, h) ||
            ecash_sign(&acct, r->counter, h, sig))
            rc = KISS_ECASH_E_INTERNAL;
    }
    if (rc == KISS_ECASH_OK) {
        char hh[65], sh[129];
        hex_of(h, 32, hh);
        hex_of(sig, 64, sh);
        int n = snprintf(reply, cap, "{\"v\":1,\"hash\":\"%s\",\"sig\":\"%s\"}",
                         hh, sh);
        if (n < 0 || (size_t)n >= cap) {
            reply[0] = 0;
            rc = KISS_ECASH_E_INTERNAL;
        }
    }
    kiss_wipe(sig, sizeof sig);
    wally_bzero(&acct, sizeof acct);
    kiss_ecash_review_clear();
    return rc;
}
