// See kiss_pwgen.h.
#include "kiss_pwgen.h"

#include <string.h>

#include "kiss_crypto.h"   // kiss_trng_fill / kiss_trng_live
#include "kiss_wipe.h"
#include "kiss_seed.h"     // kiss_seed_word: the wordlist already in flash

int kiss_pwgen_draw(const uint8_t *rnd, size_t n, uint16_t idx[PWGEN_WORDS])
{
    if (!rnd || !idx || n < PWGEN_BYTES) return -1;
    // One 56-bit register, filled big end first, read 11 bits at a time from
    // the top. Bit 55 (the last byte's low bit) is never read.
    uint64_t bits = 0;
    for (int i = 0; i < PWGEN_BYTES; i++) bits = (bits << 8) | rnd[i];
    for (int w = 0; w < PWGEN_WORDS; w++) {
        const int shift = 56 - 11 * (w + 1);
        idx[w] = (uint16_t)((bits >> shift) & 0x7FF);
    }
    return 0;
}

int kiss_pwgen_join(const uint16_t idx[PWGEN_WORDS], char *out, size_t out_len)
{
    if (!idx || !out || out_len < PWGEN_MAX) return -1;
    size_t o = 0;
    for (int w = 0; w < PWGEN_WORDS; w++) {
        const char *s = NULL;
        if (kiss_seed_word((int)idx[w], &s) != 0 || !s) { out[0] = 0; return -1; }
        const size_t len = strlen(s);
        // Refuse rather than truncate: half a password is a backup that opens
        // and an owner who cannot tell.
        if (o + len + (w ? 1u : 0u) >= out_len) { out[0] = 0; return -1; }
        if (w) out[o++] = ' ';
        memcpy(out + o, s, len);
        o += len;
    }
    out[o] = 0;
    return 0;
}

int kiss_pwgen_make(char *out, size_t out_len)
{
    if (!out || out_len < PWGEN_MAX) return -1;
    if (!kiss_trng_live()) { if (out_len) out[0] = 0; return -1; }
    uint8_t rnd[PWGEN_BYTES];
    uint16_t idx[PWGEN_WORDS];
    kiss_trng_fill(rnd, sizeof rnd);
    int rc = kiss_pwgen_draw(rnd, sizeof rnd, idx);
    if (rc == 0) rc = kiss_pwgen_join(idx, out, out_len);
    kiss_wipe(rnd, sizeof rnd);
    kiss_wipe(idx, sizeof idx);
    return rc;
}
