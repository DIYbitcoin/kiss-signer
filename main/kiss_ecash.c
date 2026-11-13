// Cashu ecash keys. See kiss_ecash.h.
#include "kiss_ecash.h"

#include <stdio.h>
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
    static uint8_t msg[KISS_ECASH_MSG_MAX];
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
    return rc;
}

int kiss_ecash_sign(const struct ext_key *account, uint32_t counter,
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
