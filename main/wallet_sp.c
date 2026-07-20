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
