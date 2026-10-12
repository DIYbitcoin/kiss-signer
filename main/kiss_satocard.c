// Satochip card protocol, the shared part. See kiss_satocard.h.
#include "kiss_satocard.h"

#include <string.h>

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_recovery.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_wipe.h"

#define SC_CLA 0xB0

#define INS_SELECT        0xA4
#define INS_GET_STATUS    0x3C
#define INS_INIT_SC       0x81
#define INS_PROCESS_SC    0x82
#define INS_VERIFY_PIN    0x42
#define INS_EXPORT_CERT   0x93
#define INS_CHALLENGE     0x9A

#define OP_INIT    0x01
#define OP_PROCESS 0x02

#define SW_SETUP_NOT_DONE  0x9C04
#define SW_UNSUPPORTED     0x9C05
#define SW_BLOCKED         0x9C0C
#define SW_SC_REQUIRED     0x9C20
#define SW_SC_UNINIT       0x9C21
#define SW_SC_WRONG_IV     0x9C22
#define SW_SC_WRONG_MAC    0x9C23
#define SW_EXPORT_DENIED   0x9C31
#define SW_INS_UNKNOWN     0x6D00

// Largest response any command here can produce, wrapped: an export chunk is
// 2 + 128 bytes plus a signature, padded and framed, well under this.
#define SC_RESP_MAX 300
#define SC_CERT_MAX 1024

static const uint8_t AID_SATOCHIP[]   = { 'S','a','t','o','C','h','i','p' };
static const uint8_t AID_SEEDKEEPER[] = { 'S','e','e','d','K','e','e','p','e','r' };
static const uint8_t CHALLENGE_MSG[]  = { 'C','h','a','l','l','e','n','g','e',':' };

// The sub CA keys, secp256k1, uncompressed, from the certificates Toporin
// ships in pysatochip (byte-identical to upstream). Both are signed by
// "Toporin Personalization Root CA 1" (P-384, DER sha256 3423ce42 a176ab8d
// 9645b932 3e223026 6106ae8b 198843f9 2a83e4c6 369fdbb5); that link was
// checked with openssl when the keys were copied in. Pinning the sub CA
// rather than the root keeps P-384 off the device.
//
// Satochip Personalization SubCA 1, subca-satochip.cert, DER sha256
// 7886002c 306b2a0e da1998fc b7d08636 5d41d314 bb7cca4f 530ac09a 5e2e4530.
static const uint8_t SUBCA_SATOCHIP[65] = {
    0x04, 0x71, 0x35, 0x71, 0x0c, 0x69, 0x17, 0x2b, 0x25, 0x74, 0x91, 0xa4, 0x57,
    0xbb, 0xef, 0x60, 0x4e, 0xb8, 0x3a, 0xe7, 0x85, 0xda, 0x9b, 0xa9, 0x2d, 0x80,
    0x30, 0x3f, 0x9f, 0xed, 0xbc, 0xf9, 0xe8, 0x23, 0x32, 0x66, 0x43, 0x5a, 0xba,
    0x1a, 0x85, 0xaf, 0xfa, 0xf8, 0xc3, 0xcf, 0xfd, 0x70, 0x46, 0x78, 0x50, 0x1e,
    0x72, 0x63, 0xb4, 0x86, 0x9a, 0x64, 0x82, 0x75, 0x1c, 0xc2, 0x34, 0x5c, 0xc3
};
// SeedKeeper Personalization SubCA 1, subca-seedkeeper.cert, DER sha256
// 308269db dcd1f5e6 a1e4fcb1 bd6a81ed 630de7f2 92b7e3f4 823a04f3 0f60f126.
static const uint8_t SUBCA_SEEDKEEPER[65] = {
    0x04, 0x9a, 0x5f, 0xc4, 0xf7, 0xe9, 0xbc, 0xaa, 0x21, 0x55, 0x2d, 0xcc, 0x69,
    0xaf, 0x27, 0x11, 0xa3, 0x4e, 0x1b, 0x7e, 0x5e, 0xaf, 0x44, 0x16, 0x12, 0x12,
    0xcb, 0xb3, 0x37, 0x58, 0xe2, 0xc7, 0xb5, 0xd6, 0x2d, 0xd1, 0x83, 0x3e, 0x58,
    0x84, 0xac, 0x74, 0x22, 0xf8, 0xb5, 0x35, 0x6f, 0x11, 0x05, 0x3a, 0x73, 0x61,
    0xf8, 0x37, 0xb1, 0x0e, 0xb2, 0xeb, 0x71, 0xa2, 0x13, 0x1a, 0xbc, 0xfa, 0xc5
};

#ifndef ESP_PLATFORM
static uint8_t s_test_anchor[65];
static bool s_use_test_anchor;
void kiss_satocard_test_set_anchor(const uint8_t anchor65[65])
{
    s_use_test_anchor = anchor65 != NULL;
    if (anchor65)
        memcpy(s_test_anchor, anchor65, 65);
}
#endif

static const uint8_t *anchor_for(kiss_sc_applet a)
{
#ifndef ESP_PLATFORM
    if (s_use_test_anchor)
        return s_test_anchor;
#endif
    return a == SC_APPLET_SATOCHIP ? SUBCA_SATOCHIP : SUBCA_SEEDKEEPER;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ---- SHA-1 and HMAC-SHA1 ----
// The channel's key derivation and MAC are HMAC-SHA1, which neither libwally
// nor the host build carries. Small, and pinned by the RFC 2202 vectors.
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
               (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
    kiss_wipe(w, sizeof w);
}

static void sha1_init(sha1_ctx *c)
{
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE,
                                    0x10325476, 0xC3D2E1F0 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t n)
{
    c->len += n;
    while (n) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) {
            sha1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56)
        sha1_update(c, &pad, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4*i]   = (uint8_t)(c->h[i] >> 24);
        out[4*i+1] = (uint8_t)(c->h[i] >> 16);
        out[4*i+2] = (uint8_t)(c->h[i] >> 8);
        out[4*i+3] = (uint8_t)c->h[i];
    }
    kiss_wipe(c, sizeof *c);
}

int kiss_satocard_hmac_sha1(const uint8_t *key, size_t key_len,
                      const uint8_t *msg, size_t msg_len, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, pad[64], inner[20];
    sha1_ctx c;
    if (key_len > 64) {
        sha1_init(&c);
        sha1_update(&c, key, key_len);
        sha1_final(&c, k);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    sha1_update(&c, msg, msg_len);
    sha1_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
    kiss_wipe(k, sizeof k);
    kiss_wipe(pad, sizeof pad);
    kiss_wipe(inner, sizeof inner);
    return 0;
}

// ---- secp256k1 ----
// A context of our own for the two operations on a secret key (the channel
// key and ECDH), blinded once from the session's randomness. Verification
// and recovery use the static context.
static secp256k1_context *sc_ctx(kiss_sc_rng_fn rng)
{
    static secp256k1_context *ctx;
    if (!ctx) {
        ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        if (ctx && rng) {
            uint8_t seed[32];
            rng(seed, sizeof seed);
            // Blinding hardens the key operations against side channels; a
            // context that could not take it still computes correctly, so a
            // failure is not a reason to refuse. GCC will not let a (void)
            // cast drop this result, so it is named and dropped instead.
            int blinded = secp256k1_context_randomize(ctx, seed);
            (void)blinded;
            kiss_wipe(seed, sizeof seed);
        }
    }
    return ctx;
}

// Verify an ECDSA signature over hash32 by pub65. The card's JavaCard signer
// does not normalise S, and libsecp256k1 only accepts low S, so normalise
// first: both forms are the same signature.
bool kiss_satocard_sig_ok(const uint8_t *der, size_t der_len, const uint8_t hash32[32],
                   const uint8_t pub65[65])
{
    secp256k1_ecdsa_signature sig;
    secp256k1_pubkey pk;
    const secp256k1_context *c = secp256k1_context_static;
    if (!secp256k1_ecdsa_signature_parse_der(c, &sig, der, der_len))
        return false;
    secp256k1_ecdsa_signature_normalize(c, &sig, &sig);
    if (!secp256k1_ec_pubkey_parse(c, &pk, pub65, 65))
        return false;
    return secp256k1_ecdsa_verify(c, &sig, hash32, &pk) == 1;
}

// A DER signature as the card makes it (any S) into a verifiable compact one.
static int der_to_compact(const uint8_t *der, size_t len, uint8_t sig64[64])
{
    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_signature_parse_der(secp256k1_context_static, &sig, der, len))
        return -1;
    secp256k1_ecdsa_signature_serialize_compact(secp256k1_context_static, sig64, &sig);
    return 0;
}

// The card names a key by its x coordinate and a signature by it; find the
// key whose x matches among the four the signature could have come from.
// That is also a verification: no other key yields this x for this message.
int kiss_satocard_recover_by_x(const uint8_t *der, size_t der_len,
                        const uint8_t hash32[32], const uint8_t x32[32],
                        uint8_t out65[65])
{
    uint8_t sig64[64];
    if (der_to_compact(der, der_len, sig64) != 0)
        return -1;
    const secp256k1_context *c = secp256k1_context_static;
    for (int recid = 0; recid < 4; recid++) {
        secp256k1_ecdsa_recoverable_signature rs;
        secp256k1_pubkey pk;
        if (!secp256k1_ecdsa_recoverable_signature_parse_compact(c, &rs, sig64, recid))
            continue;
        if (!secp256k1_ecdsa_recover(c, &pk, &rs, hash32))
            continue;
        uint8_t ser[65];
        size_t n = sizeof ser;
        secp256k1_ec_pubkey_serialize(c, ser, &n, &pk, SECP256K1_EC_UNCOMPRESSED);
        if (n == 65 && memcmp(ser + 1, x32, 32) == 0) {
            memcpy(out65, ser, 65);
            return 0;
        }
    }
    return -1;
}

static int ecdh_x(unsigned char *out, const unsigned char *x32,
                  const unsigned char *y32, void *data)
{
    (void)y32; (void)data;
    memcpy(out, x32, 32);
    return 1;
}

// ---- status words ----
static int sw_result(kiss_satocard *c, uint16_t sw)
{
    c->last_sw = sw;
    if (sw == SC_SW_OK)            return SC_OK;
    if (sw == SW_SETUP_NOT_DONE)   return SC_ERR_SETUP;
    if (sw == SW_BLOCKED)          return SC_ERR_BLOCKED;
    if (sw == SW_EXPORT_DENIED)    return SC_ERR_DENIED;
    if (sw == SW_SC_REQUIRED || sw == SW_SC_UNINIT ||
        sw == SW_SC_WRONG_IV || sw == SW_SC_WRONG_MAC)
        return SC_ERR_CHANNEL;
    if ((sw & 0xFFF0) == 0x63C0)   return SC_ERR_PIN;
    return SC_ERR_SW;
}

// ---- transport ----
static int raw(kiss_satocard *c, const uint8_t *cmd, size_t cmd_len,
               uint8_t *resp, size_t cap, size_t *resp_len, uint16_t *sw)
{
    *resp_len = 0;
    *sw = 0;
    if (c->xfer(c->xfer_ctx, cmd, cmd_len, resp, cap, resp_len, sw) != 0)
        return SC_ERR_IO;
    if (*resp_len > cap)
        return SC_ERR_PROTOCOL;
    return SC_OK;
}

// Wrap a plain APDU in the secure channel, send it, unwrap the answer.
// Request: B0 82 00 00 Lc [IV(16) | ct_len(2) | ct | 00 14 | HMAC-SHA1]. The
// IV is 12 random bytes and a counter that must be odd and keep rising. The
// answer is [IV(16) | ct_len(2) | ct] and carries no MAC: what needs to be
// trusted in it (a signature, a public key, an export) is signed by a key the
// caller checks.
static int sc_xfer(kiss_satocard *c, const uint8_t *apdu, size_t apdu_len,
                   uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (!c->channel)
        return SC_ERR_CHANNEL;
    if (apdu_len > 64)
        return SC_ERR_PROTOCOL;

    uint8_t cmd[5 + 16 + 2 + 80 + 2 + 20];
    uint8_t *iv = cmd + 5;
    c->rng(iv, 12);
    iv[12] = (uint8_t)(c->iv_counter >> 24);
    iv[13] = (uint8_t)(c->iv_counter >> 16);
    iv[14] = (uint8_t)(c->iv_counter >> 8);
    iv[15] = (uint8_t)c->iv_counter;
    c->iv_counter += 2;

    size_t ct_len = 0;
    if (wally_aes_cbc(c->enc_key, 16, iv, 16, apdu, apdu_len, AES_FLAG_ENCRYPT,
                      cmd + 5 + 18, 80, &ct_len) != WALLY_OK ||
        ct_len == 0 || ct_len > 80)
        return SC_ERR_PROTOCOL;
    cmd[5 + 16] = (uint8_t)(ct_len >> 8);
    cmd[5 + 17] = (uint8_t)ct_len;
    size_t off = 5 + 18 + ct_len;
    cmd[off++] = 0x00;
    cmd[off++] = 0x14;
    kiss_satocard_hmac_sha1(c->mac_key, 20, cmd + 5, 18 + ct_len, cmd + off);
    off += 20;
    cmd[0] = SC_CLA; cmd[1] = INS_PROCESS_SC; cmd[2] = 0; cmd[3] = 0;
    cmd[4] = (uint8_t)(off - 5);

    uint8_t resp[SC_RESP_MAX];
    size_t rlen;
    uint16_t sw;
    int rc = raw(c, cmd, off, resp, sizeof resp, &rlen, &sw);
    kiss_wipe(cmd, sizeof cmd);
    if (rc != SC_OK)
        goto out;
    rc = sw_result(c, sw);
    if (rc != SC_OK || rlen == 0)
        goto out;
    if (rlen < 18 + 16) { rc = SC_ERR_PROTOCOL; goto out; }
    size_t clen = be16(resp + 16);
    if (clen != rlen - 18 || clen % 16) { rc = SC_ERR_PROTOCOL; goto out; }
    uint8_t plain[SC_RESP_MAX];
    size_t plen = 0;
    if (wally_aes_cbc(c->enc_key, 16, resp, 16, resp + 18, clen,
                      AES_FLAG_DECRYPT, plain, sizeof plain, &plen) != WALLY_OK ||
        plen > clen || plen > cap) {
        kiss_wipe(plain, sizeof plain);
        rc = SC_ERR_PROTOCOL;
        goto out;
    }
    memcpy(out, plain, plen);
    *out_len = plen;
    kiss_wipe(plain, sizeof plain);
out:
    kiss_wipe(resp, sizeof resp);
    return rc;
}

int kiss_satocard_xfer(kiss_satocard *c, const uint8_t *apdu, size_t apdu_len,
                       uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (!c->genuine)
        return SC_ERR_ORDER;
    return sc_xfer(c, apdu, apdu_len, out, cap, out_len);
}

// ---- session ----
void kiss_satocard_init(kiss_satocard *c, kiss_sc_applet applet,
                        kiss_sc_xfer_fn xfer, void *ctx, kiss_sc_rng_fn rng)
{
    memset(c, 0, sizeof *c);
    c->applet = applet;
    c->xfer = xfer;
    c->xfer_ctx = ctx;
    c->rng = rng;
}

void kiss_satocard_close(kiss_satocard *c)
{
    kiss_sc_xfer_fn x = c->xfer;
    void *ctx = c->xfer_ctx;
    kiss_sc_rng_fn r = c->rng;
    kiss_sc_applet a = c->applet;
    kiss_wipe(c, sizeof *c);
    c->xfer = x;
    c->xfer_ctx = ctx;
    c->rng = r;
    c->applet = a;
}

// Response to INIT SECURE CHANNEL:
//   [0020 | eph_x(32) | sig1_len(2) | sig1 | sig2_len(2) | sig2 (| 0020 | auth_x)]
// sig1 is the card's ephemeral key over the first 34 bytes; recovering the
// key from it by its x is also what verifies it. sig2 is the authentikey over
// everything before sig2_len. SeedKeeper appends the authentikey's x and
// Satochip does not, so sig2 is kept and settled by the genuine check, which
// learns the authentikey from the certificate.
static int open_channel(kiss_satocard *c)
{
    secp256k1_context *ctx = sc_ctx(c->rng);
    if (!ctx)
        return SC_ERR_CHANNEL;

    uint8_t priv[32], cmd[5 + 65], resp[SC_RESP_MAX];
    secp256k1_pubkey mine;
    int tries = 0;
    do {
        c->rng(priv, 32);
    } while (!secp256k1_ec_pubkey_create(ctx, &mine, priv) && ++tries < 8);
    if (tries == 8) {
        kiss_wipe(priv, sizeof priv);
        return SC_ERR_CHANNEL;
    }
    size_t n = 65;
    secp256k1_ec_pubkey_serialize(ctx, cmd + 5, &n, &mine, SECP256K1_EC_UNCOMPRESSED);
    cmd[0] = SC_CLA; cmd[1] = INS_INIT_SC; cmd[2] = 0; cmd[3] = 0; cmd[4] = 65;

    size_t rlen;
    uint16_t sw;
    int rc = raw(c, cmd, sizeof cmd, resp, sizeof resp, &rlen, &sw);
    if (rc == SC_OK)
        rc = sw_result(c, sw);
    if (rc != SC_OK)
        goto out;

    rc = SC_ERR_CHANNEL;
    if (rlen < 36 || be16(resp) != 32)
        goto out;
    size_t s1 = be16(resp + 34);
    size_t o2 = 36 + s1;
    if (s1 == 0 || o2 + 2 > rlen)
        goto out;
    size_t s2 = be16(resp + o2);
    if (s2 == 0 || s2 > sizeof c->setup_sig || o2 + 2 + s2 > rlen)
        goto out;

    uint8_t h[32], eph[65];
    (void)wally_sha256(resp, 34, h, 32);
    if (kiss_satocard_recover_by_x(resp + 36, s1, h, resp + 2, eph) != 0)
        goto out;
    (void)wally_sha256(resp, o2, c->setup_hash, 32);
    memcpy(c->setup_sig, resp + o2 + 2, s2);
    c->setup_sig_len = (uint8_t)s2;

    secp256k1_pubkey theirs;
    uint8_t x[32];
    if (!secp256k1_ec_pubkey_parse(ctx, &theirs, eph, 65) ||
        !secp256k1_ecdh(ctx, x, &theirs, priv, ecdh_x, NULL))
        goto out;

    uint8_t k[20];
    kiss_satocard_hmac_sha1(x, 32, (const uint8_t *)"sc_key", 6, k);
    memcpy(c->enc_key, k, 16);
    kiss_satocard_hmac_sha1(x, 32, (const uint8_t *)"sc_mac", 6, c->mac_key);
    kiss_wipe(k, sizeof k);
    kiss_wipe(x, sizeof x);
    c->iv_counter = 1;
    c->channel = true;
    rc = SC_OK;
out:
    kiss_wipe(priv, sizeof priv);
    kiss_wipe(resp, sizeof resp);
    return rc;
}

int kiss_satocard_open(kiss_satocard *c)
{
    const uint8_t *aid = c->applet == SC_APPLET_SATOCHIP ? AID_SATOCHIP : AID_SEEDKEEPER;
    size_t aid_len = c->applet == SC_APPLET_SATOCHIP ? sizeof AID_SATOCHIP
                                                     : sizeof AID_SEEDKEEPER;
    uint8_t cmd[5 + 16], resp[128];
    size_t rlen;
    uint16_t sw;

    cmd[0] = 0x00; cmd[1] = INS_SELECT; cmd[2] = 0x04; cmd[3] = 0x00;
    cmd[4] = (uint8_t)aid_len;
    memcpy(cmd + 5, aid, aid_len);
    int rc = raw(c, cmd, 5 + aid_len, resp, sizeof resp, &rlen, &sw);
    if (rc != SC_OK)
        return rc;
    if (sw != SC_SW_OK) {
        c->last_sw = sw;
        return SC_ERR_NOT_APPLET;
    }

    // [version(4) | PIN0 tries | PUK0 tries | PIN1 | PUK1 | needs 2FA |
    //  seeded | setup done | needs secure channel | ...]
    const uint8_t st[4] = { SC_CLA, INS_GET_STATUS, 0, 0 };
    rc = raw(c, st, sizeof st, resp, sizeof resp, &rlen, &sw);
    if (rc != SC_OK)
        return rc;
    rc = sw_result(c, sw);
    if (rc != SC_OK)
        return rc;
    if (rlen < 12)
        return SC_ERR_PROTOCOL;
    memcpy(c->version, resp, 4);
    c->pin_tries = resp[4];
    c->needs_2fa = resp[8] != 0;
    c->seeded = resp[9] != 0;
    c->setup_done = resp[10] != 0;
    if (!c->setup_done)
        return SC_ERR_SETUP;
    return open_channel(c);
}

int kiss_satocard_verify_pin(kiss_satocard *c, const char *pin, size_t pin_len,
                             int *tries_left)
{
    if (tries_left)
        *tries_left = -1;
    if (!pin || pin_len < 4 || pin_len > 16)
        return SC_ERR_PIN;
    uint8_t apdu[5 + 16], resp[16];
    size_t rlen;
    apdu[0] = SC_CLA; apdu[1] = INS_VERIFY_PIN; apdu[2] = 0; apdu[3] = 0;
    apdu[4] = (uint8_t)pin_len;
    memcpy(apdu + 5, pin, pin_len);
    int rc = sc_xfer(c, apdu, 5 + pin_len, resp, sizeof resp, &rlen);
    kiss_wipe(apdu, sizeof apdu);
    kiss_wipe(resp, sizeof resp);
    if (rc == SC_ERR_PIN) {
        int left = c->last_sw & 0x0F;
        if (tries_left)
            *tries_left = left;
        if (left == 0)
            rc = SC_ERR_BLOCKED;      // that was the last try
    }
    c->pin_ok = rc == SC_OK;
    return rc;
}

// ---- certificate ----
// A minimal DER walk: enough to take an X.509 certificate apart into the
// signed part, its signature, and the key it certifies. Everything is
// bounds-checked against the buffer; nothing here allocates.
typedef struct { const uint8_t *p; size_t len; size_t hdr; } der_t;

static int der_next(const uint8_t *p, size_t avail, uint8_t tag, der_t *out)
{
    if (avail < 2 || p[0] != tag)
        return -1;
    size_t len, hdr;
    if (p[1] < 0x80) {
        len = p[1]; hdr = 2;
    } else if (p[1] == 0x81) {
        if (avail < 3 || p[2] < 0x80) return -1;
        len = p[2]; hdr = 3;
    } else if (p[1] == 0x82) {
        if (avail < 4) return -1;
        len = (size_t)p[2] << 8 | p[3];
        if (len < 0x100) return -1;
        hdr = 4;
    } else {
        return -1;
    }
    if (len > avail - hdr)
        return -1;
    out->p = p + hdr;
    out->len = len;
    out->hdr = hdr;
    return 0;
}

static const uint8_t OID_ECDSA_SHA256[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const uint8_t OID_ECDSA_SHA512[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04 };
static const uint8_t OID_EC_PUBKEY[]    = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
static const uint8_t OID_SECP256K1[]    = { 0x2B,0x81,0x04,0x00,0x0A };

static bool oid_is(const der_t *d, const uint8_t *oid, size_t n)
{
    return d->len == n && memcmp(d->p, oid, n) == 0;
}

int kiss_satocard_check_cert(const uint8_t *der, size_t len,
                       const uint8_t anchor65[65], uint8_t pub65[65])
{
    der_t cert, tbs, alg, oid, bits, f;
    if (der_next(der, len, 0x30, &cert) != 0 || cert.hdr + cert.len != len)
        return SC_ERR_NOT_GENUINE;
    const uint8_t *p = cert.p, *end = cert.p + cert.len;

    // tbsCertificate, kept whole: its encoding is what was signed
    if (der_next(p, (size_t)(end - p), 0x30, &tbs) != 0)
        return SC_ERR_NOT_GENUINE;
    const uint8_t *tbs_raw = p;
    size_t tbs_raw_len = tbs.hdr + tbs.len;
    p += tbs_raw_len;

    // signatureAlgorithm
    if (der_next(p, (size_t)(end - p), 0x30, &alg) != 0 ||
        der_next(alg.p, alg.len, 0x06, &oid) != 0)
        return SC_ERR_NOT_GENUINE;
    bool sha512 = oid_is(&oid, OID_ECDSA_SHA512, sizeof OID_ECDSA_SHA512);
    if (!sha512 && !oid_is(&oid, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256))
        return SC_ERR_NOT_GENUINE;
    p += alg.hdr + alg.len;

    // signatureValue: BIT STRING, no unused bits, wrapping a DER signature
    if (der_next(p, (size_t)(end - p), 0x03, &bits) != 0 || bits.len < 2 ||
        bits.p[0] != 0 || p + bits.hdr + bits.len != end)
        return SC_ERR_NOT_GENUINE;

    // ECDSA over a 256-bit curve uses the leftmost 256 bits of the digest,
    // so a SHA-512 signature is checked against its first 32 bytes.
    uint8_t h[64];
    if (sha512)
        (void)wally_sha512(tbs_raw, tbs_raw_len, h, 64);
    else
        (void)wally_sha256(tbs_raw, tbs_raw_len, h, 32);
    if (!kiss_satocard_sig_ok(bits.p + 1, bits.len - 1, h, anchor65))
        return SC_ERR_NOT_GENUINE;

    // Inside tbs: [0] version, serial, signature, issuer, validity, subject,
    // then subjectPublicKeyInfo. Validity is not checked: this device keeps
    // no trusted clock, and the sub CA itself runs to 2040.
    const uint8_t *q = tbs.p, *qend = tbs.p + tbs.len;
    if (der_next(q, (size_t)(qend - q), 0xA0, &f) == 0)
        q += f.hdr + f.len;
    static const uint8_t skip[] = { 0x02, 0x30, 0x30, 0x30, 0x30 };
    for (size_t i = 0; i < sizeof skip; i++) {
        if (der_next(q, (size_t)(qend - q), skip[i], &f) != 0)
            return SC_ERR_NOT_GENUINE;
        q += f.hdr + f.len;
    }
    der_t spki, kalg, koid, curve, key;
    if (der_next(q, (size_t)(qend - q), 0x30, &spki) != 0 ||
        der_next(spki.p, spki.len, 0x30, &kalg) != 0 ||
        der_next(kalg.p, kalg.len, 0x06, &koid) != 0 ||
        !oid_is(&koid, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY) ||
        der_next(koid.p + koid.len, kalg.len - koid.hdr - koid.len, 0x06, &curve) != 0 ||
        !oid_is(&curve, OID_SECP256K1, sizeof OID_SECP256K1) ||
        der_next(kalg.p + kalg.len, spki.len - kalg.hdr - kalg.len, 0x03, &key) != 0 ||
        key.len != 66 || key.p[0] != 0 || key.p[1] != 0x04)
        return SC_ERR_NOT_GENUINE;
    memcpy(pub65, key.p + 1, 65);
    return SC_OK;
}

int kiss_satocard_genuine(kiss_satocard *c)
{
    c->genuine = false;
    if (!c->channel || !c->pin_ok)
        return SC_ERR_ORDER;

    uint8_t resp[160], cert[SC_CERT_MAX];
    size_t rlen;
    const uint8_t init[4] = { SC_CLA, INS_EXPORT_CERT, 0x00, OP_INIT };
    int rc = sc_xfer(c, init, sizeof init, resp, sizeof resp, &rlen);
    if (rc == SC_ERR_SW && (c->last_sw == SW_INS_UNKNOWN || c->last_sw == SW_UNSUPPORTED))
        return SC_ERR_NO_CERT;
    if (rc != SC_OK)
        return rc;
    if (rlen != 2)
        return SC_ERR_PROTOCOL;
    size_t total = be16(resp);
    if (total == 0)
        return SC_ERR_NO_CERT;
    if (total > sizeof cert)
        return SC_ERR_NOT_GENUINE;

    for (size_t off = 0; off < total; ) {
        size_t chunk = total - off > 128 ? 128 : total - off;
        uint8_t apdu[9] = { SC_CLA, INS_EXPORT_CERT, 0x00, OP_PROCESS, 4,
                            (uint8_t)(off >> 8), (uint8_t)off, 0, (uint8_t)chunk };
        rc = sc_xfer(c, apdu, sizeof apdu, resp, sizeof resp, &rlen);
        if (rc != SC_OK)
            return rc;
        if (rlen != chunk)
            return SC_ERR_PROTOCOL;
        memcpy(cert + off, resp, chunk);
        off += chunk;
    }

    uint8_t key[65];
    rc = kiss_satocard_check_cert(cert, total, anchor_for(c->applet), key);
    if (rc != SC_OK)
        return rc;
    // The key that set up this channel must be the certified one; a reader in
    // the middle signed the setup with a key of its own and is caught here.
    if (!kiss_satocard_sig_ok(c->setup_sig, c->setup_sig_len, c->setup_hash, key))
        return SC_ERR_NOT_GENUINE;

    // And the card must hold that key now: sign a challenge it has never seen.
    uint8_t apdu[5 + 32], chal[32];
    c->rng(chal, 32);
    apdu[0] = SC_CLA; apdu[1] = INS_CHALLENGE; apdu[2] = 0; apdu[3] = 0; apdu[4] = 32;
    memcpy(apdu + 5, chal, 32);
    rc = sc_xfer(c, apdu, sizeof apdu, resp, sizeof resp, &rlen);
    if (rc != SC_OK)
        return rc;
    if (rlen < 34 || be16(resp + 32) + 34u != rlen)
        return SC_ERR_PROTOCOL;
    uint8_t msg[sizeof CHALLENGE_MSG + 64], h[32];
    memcpy(msg, CHALLENGE_MSG, sizeof CHALLENGE_MSG);
    memcpy(msg + sizeof CHALLENGE_MSG, resp, 32);
    memcpy(msg + sizeof CHALLENGE_MSG + 32, chal, 32);
    (void)wally_sha256(msg, sizeof msg, h, 32);
    if (!kiss_satocard_sig_ok(resp + 34, rlen - 34, h, key))
        return SC_ERR_NOT_GENUINE;

    memcpy(c->authentikey, key, 65);
    c->genuine = true;
    return SC_OK;
}
