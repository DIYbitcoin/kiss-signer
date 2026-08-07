// Source: physical d6 rolls the owner enters by hand. Off-device entropy: the
// seed is BIP39(SHA256(the digit string)), so it can be recomputed on any
// machine and verified against what the device showed. No RNG in this path.
// See docs/superpowers/specs/2026-07-30-dice-entropy-design.md for the threat
// model. Mirrors wallet_tapent: pure, no UI, host-testable.
#pragma once
#include <stdint.h>

#define DICE_MAX        180     // buffer ceiling. Sized so ROLL MORE can rescue
                                // a flagged session: at the 24 word floor of 99
                                // the old ceiling of 120 left 21 rolls, which
                                // cannot move the quality statistic.
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

// SHA256 the current digit string into out[32] regardless of count, for the
// live verification fingerprint. Returns 0, or -1 on NULL/hash failure. Unlike
// take(), this has no floor and is not seed material past the display.
int         wallet_dice_peek(uint8_t out[32]);

// ---- roll quality (wallet_dice_q.c: no crypto, links real in the sim) ----
// SHA256 whitens the rolls, so the words always look perfect and nothing
// downstream can ever notice that the input was fifty presses of one key.
// The judgement has to happen here, on the raw digits, before the hash.

// Milli-bits a roll must be worth to count as rolled. A fair d6 carries
// log2(6) = 2585. The bar sits between log2(4) = 2000 and log2(5) = 2322 on
// purpose, which gives the rule a meaning the owner can check by hand: a
// string using four or fewer of the six faces ALWAYS warns, five or six is
// judged on how level the counts are. Exact enumeration of every 50 roll
// histogram puts the false alarm rate at 1 in 1.1 million honest sessions.
#define WD_RATE  2050

enum { WD_Q_SHORT = 0,  // below the floor: nothing to judge yet
       WD_Q_OK,
       WD_Q_UNEVEN,     // the six faces did not come up like a die's
       WD_Q_PATTERN };  // the ORDER is predictable, however level the faces

typedef struct {
    int      verdict;     // WD_Q_*
    unsigned n;           // rolls judged
    unsigned floor;       // the roll count this len needs
    unsigned face[6];     // index 0 = face 1
    unsigned step[6];     // (face - previous face) mod 6
    int32_t  bits;        // milli-bits carried by the faces
    int32_t  step_bits;   // milli-bits carried by the steps
    unsigned period;      // 0, or the block length the string repeats at
} wallet_dice_q_t;

// Entropy of a six bin histogram, in milli-bits TOTAL (not per roll):
// N*log2(N) - sum(c*log2(c)), from an integer lookup table. No float
// anywhere, so the number is bit-identical on the device and on any
// computer the owner recomputes it on.
int32_t wallet_dice_bits(const unsigned counts[6]);

// Judge a roll string against the floor for `len` (16 or 32). Pure: no
// globals, no clock, no crypto. Fills every field of `out`.
void    wallet_dice_judge(const char *digits, unsigned n, unsigned len,
                          wallet_dice_q_t *out);

// 1 when the verdict refuses. Both flagged verdicts refuse: a run this judge
// does not believe was rolled does not become a seed, and ROLL MORE is the way
// through — it keeps every roll already banked, which is what DICE_MAX = 180 is
// for. Named rather than spelled out at each call site so DONE, the tests and
// any future caller cannot drift apart about what a block is.
int     wallet_dice_blocked(int verdict);
