// Source: a physical d6 or a coin, entered by hand. Off-device entropy: the
// seed is BIP39(SHA256(the digit string)), so it can be recomputed on any
// machine and verified against what the device showed. No RNG in this path.
// See design/specs/2026-07-30-dice-entropy-design.md for the threat
// model. Mirrors kiss_tapent: pure, no UI, host-testable.
//
// Base 2 is the same path, not a second one. A coin records '0' and '1', so
// the SHA256 preimage is the bit string the owner already thinks in and
// `printf '0110…' | sha256sum` reproduces it. Which physical face is which is
// the owner's choice: any fixed map carries the same bit.
#pragma once
#include <stdint.h>

#define DICE_MAX        320     // buffer ceiling. Sized so KEEP GOING can
                                // rescue a flagged session: the largest floor
                                // is 256 flips, and 64 spare cannot be shaved
                                // much further without the rescue being unable
                                // to move the quality statistic.
#define DICE_FLOOR_128  50      // 12 words / 128 bit  (50 * log2 6 = 129 bit)
#define DICE_FLOOR_256  99      // 24 words / 256 bit  (99 * log2 6 = 256 bit)
#define COIN_FLOOR_128  128     // a flip is exactly one bit, so the floor is
#define COIN_FLOOR_256  256     // the bit count. 2.6x the taps of a die.

// The floor for `base` (2 or 6) at `len` bytes (16 or 32). A table, not a
// computed ceiling: 99 is deliberately 255.9 bits, so a ceil() would disagree
// with the constant that has shipped since the path was written.
unsigned    kiss_dice_floor(unsigned base, unsigned len);

// Zero the buffer and the count, and set the base for the run: 2 for a coin,
// 6 for a die. Anything else is taken as 6. The base is a parameter on the one
// call that STARTS a run, so no path can open a screen having forgotten it,
// and a run can never mix the two.
void        kiss_dice_reset(unsigned base);

// The base this run is recording.
unsigned    kiss_dice_base(void);

// Append one face, 1..base. Returns 1 if accepted, 0 if the face is out of
// range or the buffer is full. Base 6 stores '1'..'6', base 2 stores '0'/'1'.
int         kiss_dice_roll(int face);

// Remove the last accepted roll (backspace). Returns 1 if one was removed,
// 0 if the buffer was already empty.
int         kiss_dice_undo(void);

// Rolls accepted so far.
unsigned    kiss_dice_count(void);

// Read-only, NUL-terminated view of the digit string, for the verify display.
const char *kiss_dice_digits(void);

// SHA256 the digit string and copy the first `len` bytes (16 or 32) into `out`.
// Returns 0 on success; -1 if len is not 16/32, out is NULL, or the count is
// below kiss_dice_floor() for this run's base.
int         kiss_dice_take(uint8_t *out, unsigned len);

// SHA256 the current digit string into out[32] regardless of count, for the
// live verification fingerprint. Returns 0, or -1 on NULL/hash failure. Unlike
// take(), this has no floor and is not seed material past the display.
int         kiss_dice_peek(uint8_t out[32]);

// ---- roll quality (kiss_dice_q.c: no crypto, links real in the sim) ----
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

// The same bar for a coin, derived the same way. A flip carries 1000, and only
// 129 histograms exist at 128 flips, so the binomial sum is exact rather than
// estimated: 860 fires at |heads - 64| >= 28, which is 1 in 1,268,778 honest
// sessions — the dice path's own rate. (870 would be 1 in 498,472; 900 is 1 in
// 37,552, and a warning that often is one owners learn to click through.) The
// step check at 127 steps is 1 in 2.29 million under the same bar, so the two
// together are about 1 in 820,000.
//
// What this bar can and cannot see, stated because two bins see less than a
// reader assumes: it refuses one key mashed, an alternating string, or a block
// typed twice. It does NOT refuse a biased coin — 60/40 measures 970 and
// passes, correctly, because it still carries 124 of the promised 128 bits.
// Same scope as WD_RATE, which refuses "four or fewer faces" and not a
// slightly loaded die.
#define WD_RATE_2  860

enum { WD_Q_SHORT = 0,  // below the floor: nothing to judge yet
       WD_Q_OK,
       WD_Q_UNEVEN,     // the faces did not come up like a die's or a coin's
       WD_Q_PATTERN };  // the ORDER is predictable, however level the faces

typedef struct {
    int      verdict;     // WD_Q_*
    unsigned n;           // rolls judged
    unsigned floor;       // the roll count this len needs
    unsigned base;        // 2 or 6: how many of the bins below are in use
    unsigned face[6];     // index 0 = face 1 (base 2: 0 = '0', 1 = '1')
    unsigned step[6];     // (face - previous face) mod base
    int32_t  bits;        // milli-bits carried by the faces
    int32_t  step_bits;   // milli-bits carried by the steps
    unsigned period;      // 0, or the block length the string repeats at
} kiss_dice_q_t;

// Entropy of a six bin histogram, in milli-bits TOTAL (not per roll):
// N*log2(N) - sum(c*log2(c)), from an integer lookup table. No float
// anywhere, so the number is bit-identical on the device and on any
// computer the owner recomputes it on. Base 2 passes the same six bins with
// the last four zero: the table maps 0 to 0, so nothing needs a second form.
int32_t kiss_dice_bits(const unsigned counts[6]);

// Judge a roll string in `base` against the floor for `len` (16 or 32). Pure:
// no globals, no clock, no crypto. Fills every field of `out`.
void    kiss_dice_judge(const char *digits, unsigned n, unsigned base,
                          unsigned len, kiss_dice_q_t *out);

// 1 when the verdict refuses. Both flagged verdicts refuse: a run this judge
// does not believe was rolled does not become a seed, and KEEP GOING is the way
// through — it keeps every roll already banked, which is what DICE_MAX is for.
// Named rather than spelled out at each call site so DONE, the tests and any
// future caller cannot drift apart about what a block is.
int     kiss_dice_blocked(int verdict);
