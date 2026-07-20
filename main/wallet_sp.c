// Silent Payments crypto (see wallet_sp.h). Split from wallet_crypto.c so the
// BIP352/374/375 math stays reviewable in one place and compiles into both the
// device and /tmp/kisstest.
#include "wallet_sp.h"

#include <string.h>

// ---- bech32m (encode-only) -------------------------------------------------
// libwally's bech32 is segwit-address shaped (90-char cap), while BIP352
// addresses are ~117 chars under the bech32m 1023-char rule, so we carry our
// own tiny encoder. Charset and polymod per BIP173/BIP350.

static const char B32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
#define BECH32M_CONST 0x2bc830a3u

static uint32_t b32_polymod_step(uint32_t chk, uint8_t v)
{
    static const uint32_t GEN[5] = {
        0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u
    };
    uint8_t b = chk >> 25;
    chk = ((chk & 0x1ffffffu) << 5) ^ v;
    for (int i = 0; i < 5; i++)
        if ((b >> i) & 1) chk ^= GEN[i];
    return chk;
}

// data = 5-bit groups (already includes the version). Writes hrp + '1' + data
// + 6-char checksum. Returns 0, or -1 if cap can't hold it all + NUL.
static int b32m_encode(const char *hrp, const uint8_t *data, size_t n_data,
                       char *out, size_t cap)
{
    size_t hrp_len = strlen(hrp);
    if (hrp_len + 1 + n_data + 6 + 1 > cap) return -1;

    uint32_t chk = 1;
    for (size_t i = 0; i < hrp_len; i++) chk = b32_polymod_step(chk, hrp[i] >> 5);
    chk = b32_polymod_step(chk, 0);
    for (size_t i = 0; i < hrp_len; i++) chk = b32_polymod_step(chk, hrp[i] & 0x1f);
    for (size_t i = 0; i < n_data; i++)  chk = b32_polymod_step(chk, data[i]);
    for (int i = 0; i < 6; i++)          chk = b32_polymod_step(chk, 0);
    chk ^= BECH32M_CONST;

    char *w = out;
    memcpy(w, hrp, hrp_len); w += hrp_len;
    *w++ = '1';
    for (size_t i = 0; i < n_data; i++) *w++ = B32_CHARSET[data[i]];
    for (int i = 0; i < 6; i++) *w++ = B32_CHARSET[(chk >> (5 * (5 - i))) & 0x1f];
    *w = 0;
    return 0;
}

// ---- BIP352 derivation (not in the sim build: no secp there) ---------------
#ifndef SIMULATOR
#include <secp256k1.h>
#include <wally_crypto.h>

static secp256k1_context *sp_ctx(void)
{
    static secp256k1_context *ctx;
    if (!ctx) ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN |
                                             SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

// tagged_hash(tag, msg) = sha256(sha256(tag) || sha256(tag) || msg)
static void sp_tagged_hash(const char *tag, const uint8_t *msg, size_t msg_len,
                           uint8_t out32[32])
{
    uint8_t th[32], buf[64 + 128];
    wally_sha256((const uint8_t *)tag, strlen(tag), th, 32);
    memcpy(buf, th, 32);
    memcpy(buf + 32, th, 32);
    memcpy(buf + 64, msg, msg_len);
    wally_sha256(buf, 64 + msg_len, out32, 32);
}

int sp_sum_privkeys(const uint8_t *privs32, const bool *is_xonly, size_t n,
                    uint8_t a_sum32[32], uint8_t a_sum_pub33[33])
{
    secp256k1_context *ctx = sp_ctx();
    if (!n) return -1;
    uint8_t sum[32] = { 0 };
    for (size_t i = 0; i < n; i++) {
        uint8_t k[32];
        memcpy(k, privs32 + i * 32, 32);
        if (!secp256k1_ec_seckey_verify(ctx, k)) return -2;
        if (is_xonly[i]) {
            // BIP352: x-only inputs contribute the even-Y key
            secp256k1_pubkey pub;
            uint8_t ser[33];
            size_t sl = sizeof ser;
            if (!secp256k1_ec_pubkey_create(ctx, &pub, k)) return -2;
            secp256k1_ec_pubkey_serialize(ctx, ser, &sl, &pub, SECP256K1_EC_COMPRESSED);
            if (ser[0] == 0x03 && !secp256k1_ec_seckey_negate(ctx, k)) return -2;
        }
        if (i == 0) {
            memcpy(sum, k, 32);
        } else if (!secp256k1_ec_seckey_tweak_add(ctx, sum, k)) {
            return -3;  // intermediate/final zero: BIP352 says sending fails
        }
    }
    if (!secp256k1_ec_seckey_verify(ctx, sum)) return -3;
    secp256k1_pubkey apub;
    size_t sl = 33;
    if (!secp256k1_ec_pubkey_create(ctx, &apub, sum)) return -3;
    secp256k1_ec_pubkey_serialize(ctx, a_sum_pub33, &sl, &apub, SECP256K1_EC_COMPRESSED);
    memcpy(a_sum32, sum, 32);
    return 0;
}

int sp_input_hash(const uint8_t *outpoints36, size_t n,
                  const uint8_t a_sum_pub33[33], uint8_t out32[32])
{
    if (!n) return -1;
    const uint8_t *lowest = outpoints36;
    for (size_t i = 1; i < n; i++)
        if (memcmp(outpoints36 + i * 36, lowest, 36) < 0)
            lowest = outpoints36 + i * 36;
    uint8_t msg[36 + 33];
    memcpy(msg, lowest, 36);
    memcpy(msg + 36, a_sum_pub33, 33);
    sp_tagged_hash("BIP0352/Inputs", msg, sizeof msg, out32);
    return 0;
}

// out33 = scalar32 * P(in33); shared by share and adjusted-share computation
static int sp_point_mul(const uint8_t in33[33], const uint8_t scalar32[32],
                        uint8_t out33[33])
{
    secp256k1_context *ctx = sp_ctx();
    secp256k1_pubkey p;
    size_t sl = 33;
    if (!secp256k1_ec_pubkey_parse(ctx, &p, in33, 33)) return -1;
    if (!secp256k1_ec_pubkey_tweak_mul(ctx, &p, scalar32)) return -2;
    secp256k1_ec_pubkey_serialize(ctx, out33, &sl, &p, SECP256K1_EC_COMPRESSED);
    return 0;
}

int sp_ecdh_share(const uint8_t a_sum32[32], const uint8_t scan33[33],
                  uint8_t share33[33])
{
    return sp_point_mul(scan33, a_sum32, share33);
}

int sp_derive_group(const uint8_t share33[33], const uint8_t input_hash32[32],
                    sp_recip_t *recips, size_t n)
{
    secp256k1_context *ctx = sp_ctx();
    uint8_t adjusted[33];
    if (sp_point_mul(share33, input_hash32, adjusted) != 0) return -1;
    for (size_t k = 0; k < n; k++) {
        uint8_t msg[33 + 4], t_k[32], ser[33];
        memcpy(msg, adjusted, 33);
        msg[33] = (uint8_t)(k >> 24);
        msg[34] = (uint8_t)(k >> 16);
        msg[35] = (uint8_t)(k >> 8);
        msg[36] = (uint8_t)k;
        sp_tagged_hash("BIP0352/SharedSecret", msg, sizeof msg, t_k);
        secp256k1_pubkey p;
        size_t sl = 33;
        if (!secp256k1_ec_pubkey_parse(ctx, &p, recips[k].spend, 33)) return -2;
        if (!secp256k1_ec_pubkey_tweak_add(ctx, &p, t_k)) return -3;
        secp256k1_ec_pubkey_serialize(ctx, ser, &sl, &p, SECP256K1_EC_COMPRESSED);
        memcpy(recips[k].xonly_out, ser + 1, 32);
    }
    return 0;
}
#endif  // !SIMULATOR

int sp_address_encode(const uint8_t scan33[33], const uint8_t spend33[33],
                      bool testnet, char *out, size_t cap)
{
    // version 0 + convertbits(scan||spend, 8 -> 5, pad): 66 bytes -> 106 groups
    uint8_t data[1 + 106];
    size_t n = 0;
    data[n++] = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < 66; i++) {
        acc = (acc << 8) | (i < 33 ? scan33[i] : spend33[i - 33]);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            data[n++] = (acc >> bits) & 0x1f;
        }
    }
    if (bits) data[n++] = (acc << (5 - bits)) & 0x1f;
    return b32m_encode(testnet ? "tsp" : "sp", data, n, out, cap);
}
