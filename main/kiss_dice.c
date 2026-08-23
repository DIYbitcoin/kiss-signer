// dice and coin entropy: the verifiable path.
// See docs/superpowers/specs/2026-07-30-dice-entropy-design.md
#include "kiss_dice.h"

#include <string.h>

#include "wally_core.h"
#include "wally_crypto.h"

static char     s_digits[DICE_MAX + 1];
static unsigned s_n;
static unsigned s_base = 6;

// The character a face records as. Base 6 keeps '1'..'6', the digits printed on
// a die. Base 2 records '0'/'1' rather than '1'/'2' so the preimage IS the bit
// string: the owner reads the flips off the screen and pipes them to sha256sum
// with nothing to translate on the way.
static char face_char(int face)
{
    return (char)((s_base == 2 ? '0' : '1') + face - 1);
}

void kiss_dice_reset(unsigned base)
{
    wally_bzero(s_digits, sizeof s_digits);
    s_n = 0;
    s_base = (base == 2) ? 2 : 6;
}

unsigned kiss_dice_base(void) { return s_base; }

int kiss_dice_roll(int face)
{
    if (face < 1 || (unsigned)face > s_base) return 0;
    if (s_n >= DICE_MAX) return 0;
    s_digits[s_n++] = face_char(face);
    s_digits[s_n] = '\0';
    return 1;
}

int kiss_dice_undo(void)
{
    if (s_n == 0) return 0;
    s_digits[--s_n] = '\0';
    return 1;
}

unsigned kiss_dice_count(void) { return s_n; }

const char *kiss_dice_digits(void) { return s_digits; }

int kiss_dice_take(uint8_t *out, unsigned len)
{
    if (!out || (len != 16 && len != 32)) return -1;
    if (s_n < kiss_dice_floor(s_base, len)) return -1;

    uint8_t h[32];
    int rc = wally_sha256((const unsigned char *)s_digits, s_n, h, 32);
    if (rc == WALLY_OK) memcpy(out, h, len);
    wally_bzero(h, sizeof h);
    return rc == WALLY_OK ? 0 : -1;
}

int kiss_dice_peek(uint8_t out[32])
{
    if (!out) return -1;
    return wally_sha256((const unsigned char *)s_digits, s_n, out, 32) == WALLY_OK ? 0 : -1;
}
