// KEF envelope, structural half: parse, sniff, header emit. No crypto here —
// this file compiles into every build including the UI sim, so the restore
// path's format routing is the same code everywhere. See kiss_kef.h for the
// format and kiss_kef_crypto.c for the cipher.
//
// The version table and validation rules are ported from the Krux project's
// reference implementation (src/krux/kef.py):
//
//   The MIT License (MIT)
//   Copyright (c) 2021-2025 Krux contributors
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions: The above copyright notice and this permission
//   notice shall be included in all copies or substantial portions of the
//   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
//   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "kiss_kef.h"

#include <string.h>

// Every version the reference knows, with the shape its payload must have.
// auth_pos is the tag length when the tag is APPENDED to the payload
// (versions with embedded auth carry it inside the ciphertext instead, so it
// costs no envelope bytes here). block versions must align to 16 after iv
// and appended tag are removed; stream versions carry a minimum instead.
typedef struct {
    uint8_t version;
    uint8_t iv_len;
    uint8_t auth_pos;      // appended tag bytes (0 when embedded)
    uint8_t block;         // 1 = ECB/CBC alignment rules apply
    uint8_t min_payload;
} kef_vinfo_t;

static const kef_vinfo_t VINFO[] = {
    { 0,  0, 0, 1, 16 },   // AES-ECB v1, embedded 16B auth
    { 1, 16, 0, 1, 32 },   // AES-CBC v1, embedded 16B auth
    { 5,  0, 3, 1, 19 },   // AES-ECB, appended 3B auth
    { 6,  0, 0, 1, 16 },   // AES-ECB +p, embedded 4B auth
    { 7,  0, 0, 1, 16 },   // AES-ECB +c
    { 10, 16, 4, 1, 36 },  // AES-CBC, appended 4B auth
    { 11, 16, 0, 1, 32 },  // AES-CBC +p
    { 12, 16, 0, 1, 32 },  // AES-CBC +c
    { 15, 12, 0, 0, 17 },  // AES-CTR, embedded 4B auth
    { 16, 12, 0, 0, 17 },  // AES-CTR +c
    { 20, 12, 4, 0, 17 },  // AES-GCM: iv(12) | ct(>=1) | tag(4)
    { 21, 12, 4, 0, 17 },  // AES-GCM +c
};

static const kef_vinfo_t *vinfo(uint8_t version)
{
    for (size_t i = 0; i < sizeof VINFO / sizeof VINFO[0]; i++)
        if (VINFO[i].version == version) return &VINFO[i];
    return NULL;
}

int kef_parse(const uint8_t *buf, size_t len, kef_env_t *out)
{
    if (!buf || !out) return -1;
    if (len < 5) return -1;                      // 1 + 0 id + 1 + 3, nothing left
    size_t id_len = buf[0];
    if (id_len > KEF_ID_MAX) return -1;
    if (len < 1 + id_len + 4 + 1) return -1;     // header + at least 1 payload byte

    uint8_t version = buf[1 + id_len];
    const kef_vinfo_t *vi = vinfo(version);
    if (!vi) return -1;

    uint32_t raw = ((uint32_t)buf[2 + id_len] << 16)
                 | ((uint32_t)buf[3 + id_len] << 8)
                 | (uint32_t)buf[4 + id_len];
    if (raw == 0) return -1;
    uint32_t eff = raw <= 10000 ? raw * 10000u : raw;
    if (eff < KEF_MIN_EFF_ITER) return -1;       // the hostile-envelope guard

    size_t payload_len = len - (5 + id_len);
    if (payload_len < vi->min_payload) return -1;
    if (vi->block &&
        (payload_len - vi->iv_len - vi->auth_pos) % 16 != 0) return -1;

    out->id = buf + 1;
    out->id_len = (uint8_t)id_len;
    out->version = version;
    out->iter_raw = raw;
    out->iter_eff = eff;
    out->payload = buf + 5 + id_len;
    out->payload_len = payload_len;
    return 0;
}

int kef_sniff(const uint8_t *buf, size_t len)
{
    // 16 and 32 bytes are an envelope's own plaintext, and no envelope that
    // small can hold a seed. A text mnemonic cannot collide either: its bytes
    // are all >= 0x20 and every known version byte is < 0x20.
    if (len == 16 || len == 32) return 0;
    kef_env_t e;
    return kef_parse(buf, len, &e) == 0 ? 1 : 0;
}

size_t kef_emit_header(uint8_t *out, size_t cap, const uint8_t *id,
                       size_t id_len, uint8_t version, uint32_t iter_raw)
{
    if (!out || id_len > KEF_ID_MAX || (id_len && !id)) return 0;
    if (iter_raw == 0 || iter_raw >> 24) return 0;
    size_t need = 5 + id_len;
    if (cap < need) return 0;
    out[0] = (uint8_t)id_len;
    if (id_len) memcpy(out + 1, id, id_len);
    out[1 + id_len] = version;
    out[2 + id_len] = (uint8_t)(iter_raw >> 16);
    out[3 + id_len] = (uint8_t)(iter_raw >> 8);
    out[4 + id_len] = (uint8_t)iter_raw;
    return need;
}

// ---- armor -------------------------------------------------------------
// The same envelope reaches this device three ways and they are all the same
// bytes. KISS writes the raw bytes into its QR and into its .kef file, and
// Krux accepts raw, so a KISS backup has always opened there. The other
// direction did not: Krux armors an envelope as base43 for a QR (a subset of
// QR alphanumeric mode, so the square stays small) and as base64 for a file,
// and this reader took raw only. The camera saw the square, the reader said
// nothing was there, and the owner got the one vague failure a wrong password
// gets, with nothing to act on. kiss_kef.h promises a Krux backup opens here,
// so it had to be true in both directions.
//
// Base64 is tried before base43 because the two alphabets overlap and a
// base43 string can be accidentally valid base64. What settles it is that
// nothing is accepted unless what falls out is an envelope kef_sniff claims:
// a descriptor, a text mnemonic or a stray QR decodes to bytes that are not
// one, and falls through to whoever else wants the payload.
#define B43_ALPHABET "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$*+-./:"

static int b43_digit(uint8_t c)
{
    static const char A[] = B43_ALPHABET;
    for (int i = 0; i < 43; i++) if ((uint8_t)A[i] == c) return i;
    return -1;
}

// Big endian base conversion, the same one Electrum and Krux use: the string
// is one number in base 43, and leading '0' characters are leading zero
// bytes rather than digits. Little endian while accumulating, reversed at the
// end, so the carry appends instead of shifting the whole number every digit.
static int b43_decode(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t num[KEF_MAX_ENV];
    size_t n = 0;

    for (size_t i = 0; i < in_len; i++) {
        int d = b43_digit(in[i]);
        if (d < 0) return -1;
        uint32_t carry = (uint32_t)d;
        for (size_t j = 0; j < n; j++) {
            uint32_t v = (uint32_t)num[j] * 43u + carry;
            num[j] = (uint8_t)v;
            carry = v >> 8;
        }
        while (carry) {
            if (n >= sizeof num) return -1;
            num[n++] = (uint8_t)carry;
            carry >>= 8;
        }
    }

    size_t pad = 0;
    while (pad < in_len && in[pad] == (uint8_t)'0') pad++;
    if (pad + n > out_cap) return -1;
    memset(out, 0, pad);
    for (size_t i = 0; i < n; i++) out[pad + i] = num[n - 1 - i];
    *out_len = pad + n;
    return 0;
}

static int b64_digit(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (in_len == 0 || in_len % 4) return -1;
    size_t pad = 0;
    while (pad < 2 && in_len && in[in_len - 1 - pad] == (uint8_t)'=') pad++;
    size_t need = in_len / 4 * 3 - pad;
    if (need > out_cap) return -1;

    size_t o = 0;
    for (size_t i = 0; i + 4 <= in_len; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            uint8_t c = in[i + k];
            v[k] = (c == '=' && i + 4 == in_len) ? 0 : b64_digit(c);
            if (v[k] < 0) return -1;
        }
        uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12)
                   | ((uint32_t)v[2] << 6)  | (uint32_t)v[3];
        for (int k = 0; k < 3 && o < need; k++) out[o++] = (uint8_t)(w >> (16 - 8 * k));
    }
    *out_len = o;
    return 0;
}

int kef_unarmor(const uint8_t *in, size_t in_len,
                uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!in || !out || !out_len || out_cap == 0) return -1;
    *out_len = 0;
    // Base43 grows by about half, so anything this long cannot shrink into an
    // envelope we handle. It also bounds the O(n^2) conversion above.
    if (in_len == 0 || in_len > KEF_MAX_ENV * 2) return -1;

    // A file an editor has touched carries a trailing newline; a QR does not.
    while (in_len && (in[in_len - 1] == '\n' || in[in_len - 1] == '\r' ||
                      in[in_len - 1] == '\t' || in[in_len - 1] == ' '))
        in_len--;
    if (in_len == 0) return -1;

    size_t n = 0;
    if (b64_decode(in, in_len, out, out_cap, &n) == 0 && kef_sniff(out, n)) {
        *out_len = n;
        return 0;
    }
    if (b43_decode(in, in_len, out, out_cap, &n) == 0 && kef_sniff(out, n)) {
        *out_len = n;
        return 0;
    }
    memset(out, 0, out_cap);
    return -1;
}
