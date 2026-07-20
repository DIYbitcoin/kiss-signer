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
    // largest caller: the DLEQ challenge at 6*33+32 = 230 bytes
    uint8_t th[32], buf[64 + 256];
    if (msg_len > 256) { memset(out32, 0, 32); return; }
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
// ---- BIP374 DLEQ ----------------------------------------------------------
// Port of the BIP374 reference (via the embit fork's dleq.py, mirrored
// operation-for-operation). Scalar arithmetic mod n rides on libsecp's seckey
// tweak calls plus one conditional subtract for hash outputs >= n.

static const uint8_t SP_N[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
    0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
    0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41
};

static bool sp_is_zero32(const uint8_t x[32])
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= x[i];
    return acc == 0;
}

// x mod n for x < 2^256: n > 2^255, so at most one subtraction is needed
static void sp_mod_n(uint8_t x[32])
{
    if (memcmp(x, SP_N, 32) < 0) return;
    int borrow = 0;
    for (int i = 31; i >= 0; i--) {
        int d = (int)x[i] - (int)SP_N[i] - borrow;
        borrow = d < 0;
        x[i] = (uint8_t)(d + (borrow ? 256 : 0));
    }
}

// out33 = scalar*base (base NULL = standard G). Scalar 0 = point at infinity,
// reported as rc 1 with out untouched; rc < 0 = invalid input.
static int sp_mul_base(const uint8_t *base33, const uint8_t scalar32[32],
                       uint8_t out33[33])
{
    secp256k1_context *ctx = sp_ctx();
    if (sp_is_zero32(scalar32)) return 1;
    size_t sl = 33;
    secp256k1_pubkey p;
    if (!base33) {
        if (!secp256k1_ec_pubkey_create(ctx, &p, scalar32)) return -1;
    } else {
        if (!secp256k1_ec_pubkey_parse(ctx, &p, base33, 33)) return -1;
        if (!secp256k1_ec_pubkey_tweak_mul(ctx, &p, scalar32)) return -2;
    }
    secp256k1_ec_pubkey_serialize(ctx, out33, &sl, &p, SECP256K1_EC_COMPRESSED);
    return 0;
}

// out33 = p1 + p2 where either may be the point at infinity (rc-1 semantics
// from sp_mul_base): inf + inf or a sum landing on infinity fails.
static int sp_add_points(const uint8_t *p1_33, int p1_inf,
                         const uint8_t *p2_33, int p2_inf, uint8_t out33[33])
{
    secp256k1_context *ctx = sp_ctx();
    if (p1_inf && p2_inf) return -1;
    if (p1_inf) { memcpy(out33, p2_33, 33); return 0; }
    if (p2_inf) { memcpy(out33, p1_33, 33); return 0; }
    secp256k1_pubkey a, b;
    const secp256k1_pubkey *both[2] = { &a, &b };
    secp256k1_pubkey sum;
    size_t sl = 33;
    if (!secp256k1_ec_pubkey_parse(ctx, &a, p1_33, 33) ||
        !secp256k1_ec_pubkey_parse(ctx, &b, p2_33, 33))
        return -2;
    if (!secp256k1_ec_pubkey_combine(ctx, &sum, both, 2)) return -3;  // infinity
    secp256k1_ec_pubkey_serialize(ctx, out33, &sl, &sum, SECP256K1_EC_COMPRESSED);
    return 0;
}

static const uint8_t SP_G33[33] = {
    0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0,
    0x62, 0x95, 0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d,
    0xce, 0x28, 0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98
};

// e' = tagged("BIP0374/challenge", A||B||C||G||R1||R2||m') as a 32B scalar
static void sp_dleq_challenge(const uint8_t A[33], const uint8_t B[33],
                              const uint8_t C[33], const uint8_t G[33],
                              const uint8_t R1[33], const uint8_t R2[33],
                              const uint8_t *m32, uint8_t out32[32])
{
    uint8_t msg[33 * 6 + 32];
    size_t n = 0;
    memcpy(msg + n, A, 33); n += 33;
    memcpy(msg + n, B, 33); n += 33;
    memcpy(msg + n, C, 33); n += 33;
    memcpy(msg + n, G, 33); n += 33;
    memcpy(msg + n, R1, 33); n += 33;
    memcpy(msg + n, R2, 33); n += 33;
    if (m32) { memcpy(msg + n, m32, 32); n += 32; }
    sp_tagged_hash("BIP0374/challenge", msg, n, out32);
}

int sp_dleq_prove(const uint8_t a32[32], const uint8_t b33[33],
                  const uint8_t aux32[32], const uint8_t *m32,
                  const uint8_t *g33, uint8_t proof64[64])
{
    secp256k1_context *ctx = sp_ctx();
    const uint8_t *G = g33 ? g33 : SP_G33;
    if (!secp256k1_ec_seckey_verify(ctx, a32)) return -1;

    uint8_t A[33], C[33];
    if (sp_mul_base(g33, a32, A) != 0) return -2;   // A = a*G
    if (sp_mul_base(b33, a32, C) != 0) return -3;   // C = a*B

    // t = a XOR H_aux(r)
    uint8_t aux_h[32], t[32];
    sp_tagged_hash("BIP0374/aux", aux32, 32, aux_h);
    for (int i = 0; i < 32; i++) t[i] = a32[i] ^ aux_h[i];

    // k = H_nonce(t || A || C || m') mod n
    uint8_t nmsg[32 + 33 + 33 + 32], k[32];
    size_t nl = 0;
    memcpy(nmsg + nl, t, 32); nl += 32;
    memcpy(nmsg + nl, A, 33); nl += 33;
    memcpy(nmsg + nl, C, 33); nl += 33;
    if (m32) { memcpy(nmsg + nl, m32, 32); nl += 32; }
    sp_tagged_hash("BIP0374/nonce", nmsg, nl, k);
    sp_mod_n(k);
    if (sp_is_zero32(k)) return -4;

    uint8_t R1[33], R2[33];
    if (sp_mul_base(g33, k, R1) != 0) return -5;    // R1 = k*G
    if (sp_mul_base(b33, k, R2) != 0) return -6;    // R2 = k*B

    // e = H_challenge(A||B||C||G||R1||R2||m'); s = k + e*a mod n
    uint8_t e[32], s[32], ea[32];
    sp_dleq_challenge(A, b33, C, G, R1, R2, m32, e);
    memcpy(ea, e, 32);
    sp_mod_n(ea);
    if (!sp_is_zero32(ea)) {
        memcpy(s, a32, 32);
        if (!secp256k1_ec_seckey_tweak_mul(ctx, s, ea)) return -7;  // e*a
        if (!secp256k1_ec_seckey_tweak_add(ctx, s, k)) return -7;   // + k
    } else {
        memcpy(s, k, 32);
    }
    memcpy(proof64, e, 32);
    memcpy(proof64 + 32, s, 32);

    // spec: self-verify before returning
    if (sp_dleq_verify(A, b33, C, proof64, m32, g33) != 0) return -8;
    return 0;
}

int sp_dleq_verify(const uint8_t a_pub33[33], const uint8_t b33[33],
                   const uint8_t share33[33], const uint8_t proof64[64],
                   const uint8_t *m32, const uint8_t *g33)
{
    secp256k1_context *ctx = sp_ctx();
    const uint8_t *G = g33 ? g33 : SP_G33;

    // parse checks double as the spec's is_infinite(A/B/C) checks
    secp256k1_pubkey tmp;
    if (!secp256k1_ec_pubkey_parse(ctx, &tmp, a_pub33, 33) ||
        !secp256k1_ec_pubkey_parse(ctx, &tmp, b33, 33) ||
        !secp256k1_ec_pubkey_parse(ctx, &tmp, share33, 33))
        return -1;

    uint8_t e[32], s[32];
    memcpy(e, proof64, 32);
    memcpy(s, proof64 + 32, 32);
    if (memcmp(s, SP_N, 32) >= 0) return -2;  // s must be < n

    // neg_e = (-e) mod n
    uint8_t neg_e[32];
    memcpy(neg_e, e, 32);
    sp_mod_n(neg_e);
    if (!sp_is_zero32(neg_e) && !secp256k1_ec_seckey_negate(ctx, neg_e))
        return -3;

    // R1 = s*G + (-e)*A ; R2 = s*B + (-e)*C (0-scalars = point at infinity)
    uint8_t sG[33], eA[33], R1[33], sB[33], eC[33], R2[33];
    int rc_sG = sp_mul_base(g33, s, sG);
    int rc_eA = sp_mul_base(a_pub33, neg_e, eA);
    if (rc_sG < 0 || rc_eA < 0) return -4;
    if (sp_add_points(sG, rc_sG, eA, rc_eA, R1) != 0) return -5;
    int rc_sB = sp_mul_base(b33, s, sB);
    int rc_eC = sp_mul_base(share33, neg_e, eC);
    if (rc_sB < 0 || rc_eC < 0) return -6;
    if (sp_add_points(sB, rc_sB, eC, rc_eC, R2) != 0) return -7;

    uint8_t e_check[32];
    sp_dleq_challenge(a_pub33, b33, share33, G, R1, R2, m32, e_check);
    return memcmp(e, e_check, 32) == 0 ? 0 : -8;
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
