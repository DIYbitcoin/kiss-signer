// Satochip: public keys and signatures from the card. See kiss_satochip.h.
#include "kiss_satochip.h"

#include <string.h>

#include <secp256k1.h>
#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_wipe.h"

#define SATO_CLA 0xB0
#define INS_GET_EXTENDED_KEY 0x6D
#define INS_SIGN_HASH        0x7A

#define OPT_RESET_CACHE 0x80
#define SW_NO_MEMORY    0x9C01
#define KEY_LAST_DERIVED 0xFF

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Derive on the card. Response:
//   [chaincode(32) | coordx_size(2) | coordx(32) | sig1_len(2) | sig1 |
//    sig2_len(2) | sig2]
// sig1 is the derived key over everything before sig1_len, so recovering the
// key by its x also verifies it; sig2 is the authentikey over everything
// before sig2_len, which ties the key to this certified card. The top bit of
// coordx_size is an old caching hint and is ignored.
static int derive(kiss_satocard *c, const uint32_t *path, int depth,
                  uint8_t pub65[65], uint8_t chaincode[32])
{
    if (depth < 0 || depth > SATO_MAX_DEPTH)
        return SC_ERR_FORMAT;
    uint8_t apdu[5 + 4 * SATO_MAX_DEPTH];
    apdu[0] = SATO_CLA; apdu[1] = INS_GET_EXTENDED_KEY;
    apdu[2] = (uint8_t)depth; apdu[3] = 0x00;
    apdu[4] = (uint8_t)(4 * depth);
    for (int i = 0; i < depth; i++) {
        apdu[5 + 4*i]     = (uint8_t)(path[i] >> 24);
        apdu[5 + 4*i + 1] = (uint8_t)(path[i] >> 16);
        apdu[5 + 4*i + 2] = (uint8_t)(path[i] >> 8);
        apdu[5 + 4*i + 3] = (uint8_t)path[i];
    }
    size_t len = 5 + 4 * (size_t)depth;

    uint8_t resp[256];
    size_t rlen;
    int rc = kiss_satocard_xfer(c, apdu, len, resp, sizeof resp, &rlen);
    if (rc == SC_ERR_SW && c->last_sw == SW_NO_MEMORY) {
        // The card's derivation cache is full: ask it to start over, once.
        apdu[3] = OPT_RESET_CACHE;
        rc = kiss_satocard_xfer(c, apdu, len, resp, sizeof resp, &rlen);
    }
    if (rc != SC_OK)
        goto out;

    rc = SC_ERR_PROTOCOL;
    if (rlen < 68 || (be16(resp + 32) & 0x7FFF) != 32)
        goto out;
    size_t s1 = be16(resp + 66);
    size_t o2 = 68 + s1;
    if (s1 == 0 || o2 + 2 > rlen)
        goto out;
    size_t s2 = be16(resp + o2);
    if (s2 == 0 || o2 + 2 + s2 != rlen)
        goto out;

    uint8_t h[32];
    (void)wally_sha256(resp, 66, h, 32);
    rc = SC_ERR_NOT_GENUINE;
    if (kiss_satocard_recover_by_x(resp + 68, s1, h, resp + 34, pub65) != 0)
        goto out;
    (void)wally_sha256(resp, o2, h, 32);
    if (!kiss_satocard_sig_ok(resp + o2 + 2, s2, h, c->authentikey))
        goto out;
    memcpy(chaincode, resp, 32);
    rc = SC_OK;
out:
    kiss_wipe(resp, sizeof resp);
    return rc;
}

int kiss_satochip_pubkey(kiss_satocard *c, const uint32_t *path, int depth,
                         uint8_t pub33[33], uint8_t chaincode[32])
{
    uint8_t pub65[65];
    int rc = derive(c, path, depth, pub65, chaincode);
    if (rc != SC_OK)
        return rc;
    pub33[0] = (pub65[64] & 1) ? 0x03 : 0x02;
    memcpy(pub33 + 1, pub65 + 1, 32);
    return SC_OK;
}

int kiss_satochip_sign(kiss_satocard *c, const uint32_t *path, int depth,
                       const uint8_t hash32[32], uint8_t *der, size_t *der_len)
{
    *der_len = 0;
    // A card with 2FA wants a second device to approve each signature, which
    // an offline signer has no way to reach.
    if (c->needs_2fa)
        return SC_ERR_DENIED;

    uint8_t pub65[65], cc[32];
    int rc = derive(c, path, depth, pub65, cc);
    kiss_wipe(cc, sizeof cc);
    if (rc != SC_OK)
        return rc;

    // P1 0xFF: sign with the key the derivation above just loaded.
    uint8_t apdu[5 + 32] = { SATO_CLA, INS_SIGN_HASH, KEY_LAST_DERIVED, 0x00, 32 };
    memcpy(apdu + 5, hash32, 32);
    uint8_t resp[80];
    size_t rlen;
    rc = kiss_satocard_xfer(c, apdu, sizeof apdu, resp, sizeof resp, &rlen);
    if (rc != SC_OK)
        return rc;

    // The card's JavaCard signer leaves S as it falls; relay policy wants it
    // low. Both forms are the same signature, so normalise, then check it
    // really is a signature by this key over this hash before handing it on.
    const secp256k1_context *ctx = secp256k1_context_static;
    secp256k1_ecdsa_signature sig;
    secp256k1_pubkey pk;
    if (!secp256k1_ecdsa_signature_parse_der(ctx, &sig, resp, rlen))
        return SC_ERR_PROTOCOL;
    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);
    if (!secp256k1_ec_pubkey_parse(ctx, &pk, pub65, 65) ||
        secp256k1_ecdsa_verify(ctx, &sig, hash32, &pk) != 1)
        return SC_ERR_NOT_GENUINE;
    size_t n = 72;
    if (!secp256k1_ecdsa_signature_serialize_der(ctx, der, &n, &sig))
        return SC_ERR_PROTOCOL;
    *der_len = n;
    return SC_OK;
}
