// KEF envelope, cipher half: PBKDF2 key derivation and AES-256-GCM built on
// libwally primitives. GCM is CTR (single-block wally_aes ECB encrypts of a
// counter) plus GHASH, implemented here bit-serially with no data-dependent
// lookups, because libwally ships no GCM and mbedtls has no host build — the
// house rule is that the exact bytes that run on the device also run in
// /tmp/kisstest, where this file is proven against the GCM spec vectors and
// against envelopes produced by the reference implementation.
//
// Decrypt verifies the tag BEFORE decrypting (the tag depends only on the
// ciphertext), so tampered bytes never reach the output buffer, and every
// failure leaves that buffer zeroed. Krux's older ECB, CBC and CTR versions
// are read too (see "the versions KISS only reads" below); those carry their
// check inside or beside the ciphertext, so they decrypt first and check
// after, and a failure still zeroes the buffer. One failure code for everything — a
// wrong password, a wrong version and a truncated envelope must be
// indistinguishable to the caller (kiss_kef.h: encrypt strict, decrypt
// vague).
#include "kiss_kef.h"
#include "kiss_wipe.h"

#include <stdio.h>
#include <string.h>

#include "wally_bip39.h"
#include "wally_crypto.h"

#include "kiss_crypto.h"   // fingerprint id; jitter + trng gate for the IV
#include "kiss_pbkdf2.h"   // the key derivation, on the chip's SHA engine

#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

// ---- randomness ---------------------------------------------------------
// The IV of every envelope comes through here. Same shape as the sealed-blob
// writer (kiss_seed_sd.c): chip entropy folded with timing jitter, behind
// the TRNG provenance gate on device, /dev/urandom on the host.
static int fill_random(uint8_t *out, size_t len)
{
    if (!out || len > 32) return -1;
    uint8_t chip[32], jit[32], mixed[32];
    int rc;

#ifdef ESP_PLATFORM
    if (!kiss_trng_live()) return -1;
    esp_fill_random(chip, sizeof chip);
    rc = 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    rc = fread(chip, 1, sizeof chip, f) == sizeof chip ? 0 : -1;
    fclose(f);
#endif

    if (rc == 0) rc = kiss_jitter(jit);
    if (rc == 0) rc = kiss_entropy_mix(chip, jit, mixed);
    if (rc == 0) memcpy(out, mixed, len);

    kiss_wipe(chip, sizeof chip);
    kiss_wipe(jit, sizeof jit);
    kiss_wipe(mixed, sizeof mixed);
    return rc;
}

#ifndef ESP_PLATFORM
static uint8_t s_fix_iv[KEF_IV_LEN];
static int s_fix_iv_set;
void kiss_kef_test_fix_iv(const uint8_t iv[KEF_IV_LEN])
{
    memcpy(s_fix_iv, iv, KEF_IV_LEN);
    s_fix_iv_set = 1;
}
#endif

// Length-independent compare, same shape as the sealed-blob opener.
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// ---- GF(2^128) for GHASH ------------------------------------------------
// A block as two big-endian halves. Multiplication is the NIST SP 800-38D
// bit-serial algorithm; both operands are secret-derived, so every branch
// is replaced by a mask.
typedef struct { uint64_t hi, lo; } be128_t;

static be128_t be128_load(const uint8_t b[16])
{
    be128_t x = { 0, 0 };
    for (int i = 0; i < 8; i++) x.hi = (x.hi << 8) | b[i];
    for (int i = 8; i < 16; i++) x.lo = (x.lo << 8) | b[i];
    return x;
}

static void be128_store(be128_t x, uint8_t b[16])
{
    for (int i = 7; i >= 0; i--) { b[i] = (uint8_t)x.hi; x.hi >>= 8; }
    for (int i = 15; i >= 8; i--) { b[i] = (uint8_t)x.lo; x.lo >>= 8; }
}

static be128_t gf_mult(be128_t x, be128_t y)
{
    be128_t z = { 0, 0 }, v = y;
    for (int i = 0; i < 128; i++) {
        uint64_t bit = i < 64 ? (x.hi >> (63 - i)) & 1u
                              : (x.lo >> (127 - i)) & 1u;
        uint64_t keep = 0u - bit;
        z.hi ^= v.hi & keep;
        z.lo ^= v.lo & keep;
        uint64_t lsb = v.lo & 1u;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        v.hi ^= (0u - lsb) & 0xe100000000000000ULL;
    }
    return z;
}

// ---- AES-256-GCM, no AAD ------------------------------------------------
static int aes_block(const uint8_t key[32], const uint8_t in[16],
                     uint8_t out[16])
{
    return wally_aes(key, 32, in, 16, AES_FLAG_ENCRYPT, out, 16) == WALLY_OK
               ? 0 : -1;
}

static void inc32(uint8_t ctr[16])
{
    for (int i = 15; i >= 12; i--)
        if (++ctr[i]) break;
}

// Encrypt-or-decrypt (XOR keystream) `n` bytes and compute the full 16-byte
// tag over the CIPHERTEXT `ct`. For sealing, call with dir_out = ct; for
// opening, the caller verifies the tag before asking for the plaintext.
static int gcm_keystream(const uint8_t key[32], const uint8_t iv[KEF_IV_LEN],
                         const uint8_t *in, uint8_t *out, size_t n)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, KEF_IV_LEN);
    memset(ctr + KEF_IV_LEN, 0, 4);
    ctr[15] = 1;                                  // J0
    int rc = 0;
    for (size_t off = 0; off < n && rc == 0; off += 16) {
        inc32(ctr);
        rc = aes_block(key, ctr, ks);
        size_t take = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < take; i++) out[off + i] = in[off + i] ^ ks[i];
    }
    kiss_wipe(ctr, sizeof ctr);
    kiss_wipe(ks, sizeof ks);
    return rc;
}

static int gcm_tag(const uint8_t key[32], const uint8_t iv[KEF_IV_LEN],
                   const uint8_t *ct, size_t n, uint8_t tag[16])
{
    uint8_t hb[16] = { 0 }, block[16], j0[16];
    if (aes_block(key, hb, hb) != 0) return -1;
    be128_t h = be128_load(hb), y = { 0, 0 };

    for (size_t off = 0; off < n; off += 16) {
        size_t take = n - off < 16 ? n - off : 16;
        memset(block, 0, sizeof block);
        memcpy(block, ct + off, take);
        be128_t c = be128_load(block);
        y.hi ^= c.hi;
        y.lo ^= c.lo;
        y = gf_mult(y, h);
    }
    // lengths block: 64 bits of AAD length (always 0 here) then of C, in bits
    be128_t lens = { 0, (uint64_t)n * 8 };
    y.hi ^= lens.hi;
    y.lo ^= lens.lo;
    y = gf_mult(y, h);

    memcpy(j0, iv, KEF_IV_LEN);
    memset(j0 + KEF_IV_LEN, 0, 4);
    j0[15] = 1;
    int rc = aes_block(key, j0, j0);              // E(K, J0)
    if (rc == 0) {
        be128_store(y, block);
        for (int i = 0; i < 16; i++) tag[i] = block[i] ^ j0[i];
    }
    kiss_wipe(hb, sizeof hb);
    kiss_wipe(block, sizeof block);
    kiss_wipe(j0, sizeof j0);
    kiss_wipe(&h, sizeof h);
    kiss_wipe(&y, sizeof y);
    return rc;
}

#ifndef ESP_PLATFORM
int kiss_kef_test_gcm(const uint8_t key[32], const uint8_t iv[KEF_IV_LEN],
                      const uint8_t *pt, size_t n, uint8_t *ct,
                      uint8_t tag[16])
{
    int rc = gcm_keystream(key, iv, pt, ct, n);
    if (rc == 0) rc = gcm_tag(key, iv, ct, n, tag);
    return rc;
}
#endif

// ---- the versions KISS only reads --------------------------------------
// KISS writes version 20 and nothing else, but Krux's default mode was ECB
// until Krux 25.09, and its owner can still pick ECB, CBC or CTR, so a Krux
// backup in someone's drawer is often one of these. The rules are Krux's
// Cipher.decrypt and Cipher._authenticate (src/krux/kef.py), step for step,
// and sim/test_kef.c holds envelopes made by that code, including Krux's own
// stored test vectors.
//
// auth < 0: sha256(plaintext) truncated, appended to the plaintext before
// encrypting. auth > 0: sha256(version | iv | plaintext | key) truncated,
// appended after the ciphertext. NUL padding cannot tell padding from a
// plaintext or check that ends in 0x00, so, as Krux does, a failed check is
// retried a bounded number of times with those zeros put back.
enum { M_ECB, M_CBC, M_CTR };
enum { PAD_NUL, PAD_PKCS, PAD_NONE };

typedef struct {
    uint8_t version, mode, iv_len, pad;
    int8_t  auth;
} kef_legacy_t;

static const kef_legacy_t LEGACY[] = {
    {  0, M_ECB,  0, PAD_NUL,  -16 },   // AES-ECB v1
    {  1, M_CBC, 16, PAD_NUL,  -16 },   // AES-CBC v1
    {  5, M_ECB,  0, PAD_NUL,    3 },   // AES-ECB
    {  6, M_ECB,  0, PAD_PKCS,  -4 },   // AES-ECB +p
    { 10, M_CBC, 16, PAD_NUL,    4 },   // AES-CBC
    { 11, M_CBC, 16, PAD_PKCS,  -4 },   // AES-CBC +p
    { 15, M_CTR, 12, PAD_NONE,  -4 },   // AES-CTR
};

// The largest ciphertext read here: a 24 word text mnemonic with its check
// fits, and it bounds the one stack buffer the public check hashes from.
#define LEGACY_MAX_CT 256

static const kef_legacy_t *legacy(uint8_t version)
{
    for (size_t i = 0; i < sizeof LEGACY / sizeof LEGACY[0]; i++)
        if (LEGACY[i].version == version) return &LEGACY[i];
    return NULL;
}

static int legacy_decrypt(const kef_legacy_t *L, const uint8_t key[32],
                          const uint8_t *iv, const uint8_t *ct, size_t n,
                          uint8_t *out)
{
    uint8_t ctr[16], ks[16];
    int rc = 0;
    if (L->mode == M_CTR) {                       // nonce | 32 bit count from 0
        memcpy(ctr, iv, 12);
        memset(ctr + 12, 0, 4);
    }
    for (size_t off = 0; off < n && rc == 0; off += 16) {
        size_t take = n - off < 16 ? n - off : 16;
        if (L->mode == M_CTR) {
            rc = aes_block(key, ctr, ks);
            for (size_t i = 0; i < take; i++) out[off + i] = ct[off + i] ^ ks[i];
            inc32(ctr);
            continue;
        }
        rc = wally_aes(key, 32, ct + off, 16, AES_FLAG_DECRYPT, out + off,
                       16) == WALLY_OK ? 0 : -1;
        if (rc == 0 && L->mode == M_CBC) {
            const uint8_t *prev = off ? ct + off - 16 : iv;
            for (int i = 0; i < 16; i++) out[off + i] ^= prev[i];
        }
    }
    kiss_wipe(ctr, sizeof ctr);
    kiss_wipe(ks, sizeof ks);
    return rc;
}

static int legacy_open(const kef_legacy_t *L, const uint8_t key[32],
                       const kef_env_t *e, uint8_t *plain, size_t plain_cap,
                       size_t *plain_len)
{
    size_t alen = (size_t)(L->auth < 0 ? -L->auth : L->auth);
    const uint8_t *iv = e->payload;
    const uint8_t *ct = e->payload + L->iv_len;
    // kef_parse already holds the payload to its version's floor and, for
    // ECB and CBC, to whole blocks after the iv and any appended check.
    size_t n = e->payload_len - L->iv_len - (L->auth > 0 ? alen : 0);
    if (n == 0 || n > plain_cap || n > LEGACY_MAX_CT) return -1;
    if (L->mode != M_CTR && n % 16) return -1;

    uint8_t a[16], sum[32];
    uint8_t hin[1 + 16 + LEGACY_MAX_CT + 32];
    int rc = legacy_decrypt(L, key, iv, ct, n, plain);

    size_t len = n;
    if (rc == 0 && L->pad == PAD_NUL) {
        while (len && plain[len - 1] == 0) len--;
    } else if (rc == 0 && L->pad == PAD_PKCS) {
        uint8_t p = plain[len - 1];
        if (p == 0 || p > len) rc = -1;           // Krux's unpad leaves nothing
        else len -= p;
    }
    if (rc == 0 && L->auth < 0) {
        if (len < alen) rc = -1;
        else {
            len -= alen;
            memcpy(a, plain + len, alen);
        }
    } else if (rc == 0) {
        memcpy(a, ct + n, alen);
    }

    int ok = 0;
    if (rc == 0) {
        size_t tries = 1;
        if (L->pad == PAD_NUL) {
            size_t gone = n - len;               // padding, plus the check if inside
            tries = (gone < alen + 1 ? gone : alen + 1) + 1;
        }
        for (size_t t = 0; t < tries && !ok && rc == 0; t++) {
            if (L->auth > 0) {
                size_t h = 0;
                hin[h++] = L->version;
                memcpy(hin + h, iv, L->iv_len);
                h += L->iv_len;
                memcpy(hin + h, plain, len);
                h += len;
                memcpy(hin + h, key, 32);
                h += 32;
                rc = wally_sha256(hin, h, sum, sizeof sum) == WALLY_OK ? 0 : -1;
            } else {
                rc = wally_sha256(plain, len, sum, sizeof sum) == WALLY_OK
                         ? 0 : -1;
            }
            if (rc == 0 && ct_equal(sum, a, alen)) ok = 1;
            else if (rc == 0 && t + 1 < tries) {
                // Put one stripped zero back and look again. Inside the
                // plaintext, the check's first byte was really plaintext and
                // a zero belongs at the check's end; outside, the zero was
                // plaintext. Either way len stays within the n decrypted.
                if (L->auth < 0) {
                    plain[len++] = a[0];
                    memmove(a, a + 1, alen - 1);
                    a[alen - 1] = 0;
                } else {
                    plain[len++] = 0;
                }
            }
        }
    }
    if (ok) {
        memset(plain + len, 0, plain_cap - len);  // the check and the padding
        *plain_len = len;
    }
    kiss_wipe(a, sizeof a);
    kiss_wipe(sum, sizeof sum);
    kiss_wipe(hin, sizeof hin);
    return ok ? 0 : -1;
}

static int derive_key(const char *password, size_t pass_len,
                      const uint8_t *id, size_t id_len, uint32_t iters,
                      uint8_t key[32])
{
    // The password's bytes go in exactly as typed (UTF-8, no normalization,
    // per the format). An empty id is a valid salt; the pointer just must
    // not be NULL for libwally.
    static const uint8_t none;
    if (!id) id = &none;
    // On the device this is 200,000 SHA-256 compressions on the accelerator
    // instead of in portable C, and it is the whole cost of opening or making
    // a locked backup. See kiss_pbkdf2.h.
    return kiss_pbkdf2_sha256((const uint8_t *)password, pass_len,
                              id, id_len, iters, key);
}

// ---- the two envelope operations ---------------------------------------
int kiss_kef_seal(const uint8_t *id, size_t id_len,
                  const char *password, size_t pass_len,
                  const uint8_t *plain, size_t plain_len,
                  uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out && out_cap) memset(out, 0, out_cap);
    if (out_len) *out_len = 0;
    if (!out || !out_len || !password || !pass_len || !plain || !plain_len)
        return -1;
    if (id_len > KEF_ID_MAX) return -1;
    // Bound plain_len BEFORE the size arithmetic: every real caller passes
    // tens of bytes, and this is what keeps a hostile length from wrapping
    // `need` around zero and turning the cap check into a lie.
    if (plain_len > KEF_MAX_ENV) return -1;

    size_t need = 5 + id_len + KEF_IV_LEN + plain_len + KEF_TAG_LEN;
    if (need > out_cap || need > KEF_MAX_ENV) return -1;

    size_t hdr = kef_emit_header(out, out_cap, id, id_len,
                                 KEF_VERSION_AES_GCM, KEF_ITER_STORED);
    if (!hdr) return -1;

    uint8_t *iv = out + hdr;
    uint8_t *ct = iv + KEF_IV_LEN;
    uint8_t key[32], tag[16];
    int rc;

#ifndef ESP_PLATFORM
    if (s_fix_iv_set) {
        memcpy(iv, s_fix_iv, KEF_IV_LEN);
        s_fix_iv_set = 0;
        rc = 0;
    } else
#endif
        rc = fill_random(iv, KEF_IV_LEN);

    if (rc == 0) rc = derive_key(password, pass_len, id, id_len,
                                 KEF_ITER_STORED * 10000u, key);
    if (rc == 0) rc = gcm_keystream(key, iv, plain, ct, plain_len);
    if (rc == 0) rc = gcm_tag(key, iv, ct, plain_len, tag);
    if (rc == 0) {
        memcpy(ct + plain_len, tag, KEF_TAG_LEN);
        *out_len = need;
    } else {
        memset(out, 0, out_cap);
    }
    kiss_wipe(key, sizeof key);
    kiss_wipe(tag, sizeof tag);
    return rc == 0 ? 0 : -1;
}

int kiss_kef_open(const char *password, size_t pass_len,
                  const uint8_t *env, size_t env_len,
                  uint8_t *plain, size_t plain_cap, size_t *plain_len)
{
    if (plain && plain_cap) memset(plain, 0, plain_cap);
    if (plain_len) *plain_len = 0;
    if (!plain || !plain_len || !password || !pass_len || !env) return -1;

    kef_env_t e;
    uint8_t key[32], tag[16];
    int rc = kef_parse(env, env_len, &e);
    if (rc == 0 && !kef_can_open(e.version)) rc = -1;
    if (rc == 0 && e.iter_eff > KEF_MAX_EFF_ITER) rc = -1;

    if (rc == 0 && e.version != KEF_VERSION_AES_GCM) {
        const kef_legacy_t *L = legacy(e.version);
        rc = L ? derive_key(password, pass_len, e.id, e.id_len, e.iter_eff,
                            key) : -1;
        if (rc == 0) rc = legacy_open(L, key, &e, plain, plain_cap, plain_len);
        if (rc != 0) {
            memset(plain, 0, plain_cap);
            *plain_len = 0;
        }
        kiss_wipe(key, sizeof key);
        return rc == 0 ? 0 : -1;
    }

    size_t ct_len = 0;
    const uint8_t *iv = NULL, *ct = NULL;
    if (rc == 0) {
        iv = e.payload;
        ct = e.payload + KEF_IV_LEN;
        ct_len = e.payload_len - KEF_IV_LEN - KEF_TAG_LEN;
        if (ct_len > plain_cap) rc = -1;
    }
    if (rc == 0) rc = derive_key(password, pass_len, e.id, e.id_len,
                                 e.iter_eff, key);
    if (rc == 0) rc = gcm_tag(key, iv, ct, ct_len, tag);
    if (rc == 0 && !ct_equal(tag, ct + ct_len, KEF_TAG_LEN)) rc = -1;
    if (rc == 0) rc = gcm_keystream(key, iv, ct, plain, ct_len);
    if (rc == 0) {
        *plain_len = ct_len;
    } else if (plain_cap) {
        memset(plain, 0, plain_cap);
    }
    kiss_wipe(key, sizeof key);
    kiss_wipe(tag, sizeof tag);
    return rc == 0 ? 0 : -1;
}

int kiss_kef_seal_seed(const char *mnemonic, const char *password,
                       size_t pass_len, uint8_t *out, size_t out_cap,
                       size_t *out_len, char id_hex_out[9])
{
    if (id_hex_out) id_hex_out[0] = 0;
    if (!mnemonic || !id_hex_out) return -1;

    uint8_t entropy[32], fp[4];
    size_t elen = 0;
    int rc = bip39_mnemonic_to_bytes(NULL, mnemonic, entropy, sizeof entropy,
                                     &elen) == WALLY_OK && elen ? 0 : -1;
    if (rc == 0) rc = kiss_fingerprint(NULL, fp);
    if (rc == 0) {
        snprintf(id_hex_out, 9, "%02X%02X%02X%02X",
                 fp[0], fp[1], fp[2], fp[3]);
        rc = kiss_kef_seal((const uint8_t *)id_hex_out, 8, password, pass_len,
                           entropy, elen, out, out_cap, out_len);
    }
    kiss_wipe(entropy, sizeof entropy);
    kiss_wipe(fp, sizeof fp);
    if (rc != 0 && id_hex_out) id_hex_out[0] = 0;
    return rc == 0 ? 0 : -1;
}
