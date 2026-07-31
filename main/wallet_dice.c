// dice entropy: the verifiable path.
// See docs/superpowers/specs/2026-07-30-dice-entropy-design.md
#include "wallet_dice.h"

#include <string.h>

#include "wally_core.h"
#include "wally_crypto.h"

static char     s_digits[DICE_MAX + 1];
static unsigned s_n;

void wallet_dice_reset(void)
{
    wally_bzero(s_digits, sizeof s_digits);
    s_n = 0;
}

int wallet_dice_roll(int face)
{
    if (face < 1 || face > 6) return 0;
    if (s_n >= DICE_MAX) return 0;
    s_digits[s_n++] = (char)('0' + face);
    s_digits[s_n] = '\0';
    return 1;
}

int wallet_dice_undo(void)
{
    if (s_n == 0) return 0;
    s_digits[--s_n] = '\0';
    return 1;
}

unsigned wallet_dice_count(void) { return s_n; }

const char *wallet_dice_digits(void) { return s_digits; }

int wallet_dice_take(uint8_t *out, unsigned len)
{
    if (!out || (len != 16 && len != 32)) return -1;
    unsigned floor = (len == 32) ? DICE_FLOOR_256 : DICE_FLOOR_128;
    if (s_n < floor) return -1;

    uint8_t h[32];
    int rc = wally_sha256((const unsigned char *)s_digits, s_n, h, 32);
    if (rc == WALLY_OK) memcpy(out, h, len);
    wally_bzero(h, sizeof h);
    return rc == WALLY_OK ? 0 : -1;
}
