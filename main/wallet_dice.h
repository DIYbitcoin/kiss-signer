// Source: physical d6 rolls the owner enters by hand. Off-device entropy: the
// seed is BIP39(SHA256(the digit string)), so it can be recomputed on any
// machine and verified against what the device showed. No RNG in this path.
// See docs/superpowers/specs/2026-07-30-dice-entropy-design.md for the threat
// model. Mirrors wallet_tapent: pure, no UI, host-testable.
#pragma once
#include <stdint.h>

#define DICE_MAX        120     // buffer ceiling, comfortably above 99 rolls
#define DICE_FLOOR_128  50      // 12 words / 128 bit  (50 * log2 6 = 129 bit)
#define DICE_FLOOR_256  99      // 24 words / 256 bit  (99 * log2 6 = 256 bit)

// Zero the buffer and the count. Call when the dice screen opens or cancels.
void        wallet_dice_reset(void);

// Append one face, 1..6. Returns 1 if accepted, 0 if the face is out of range
// or the buffer is full.
int         wallet_dice_roll(int face);

// Remove the last accepted roll (backspace). Returns 1 if one was removed,
// 0 if the buffer was already empty.
int         wallet_dice_undo(void);

// Rolls accepted so far.
unsigned    wallet_dice_count(void);

// Read-only, NUL-terminated view of the digit string, for the verify display.
const char *wallet_dice_digits(void);

// SHA256 the digit string and copy the first `len` bytes (16 or 32) into `out`.
// Returns 0 on success; -1 if len is not 16/32, out is NULL, or the count is
// below the floor for that len (50 for 16, 99 for 32).
int         wallet_dice_take(uint8_t *out, unsigned len);
