// Source 3: the timing of the user's own taps.
//
// The taps are not the entropy. The jitter between them is, sampled at CPU
// cycle resolution: at 240MHz a human's tens of milliseconds of variation
// spans tens of millions of cycles, so the low bits are unpredictable even to
// someone standing over the user with a stopwatch. See docs/specs/tap-entropy.md
// for the budget, and for why this counts events rather than scoring them.
//
// Sources 1 and 2 are the camera and the chip's own TRNG. Both come from one
// vendor and are audited by this firmware; this one is neither, which is the
// whole reason it exists. A hash is as strong as its best input, so the only
// fatal shape is depending on a single source.
//
// The clock is a parameter, not a call: every decision here is exercised on
// the host by sim/test_tapent.c with injected timestamps.
#pragma once
#include <stdint.h>
#include <stddef.h>

// 64 taps counted at a deliberately underclaimed 2 bits each = 128 bits. A tap
// whose arrival varies by even one millisecond carries roughly eighteen bits at
// cycle resolution, so this is a floor chosen to survive being wrong, not an
// estimate.
#define WTAP_TARGET       64

// A tap closer than this to the last ACCEPTED one is a drag or bounce artifact
// of the touch panel, not a decision by a hand.
#define WTAP_DEBOUNCE_US  30000

// Begin a session. Zeroes the chain, the count and the debounce clock.
void wallet_tapent_reset(void);

// Offer one tap. `us` is a microsecond timestamp, `cycles` the CPU cycle
// counter, `x`/`y` the touch point. Returns 1 if it counted, 0 if debounced.
// Counting folds the record straight into the chain: taps are never buffered,
// so there is no array of timestamps in RAM for anyone to find later.
int wallet_tapent_tap(uint64_t us, uint32_t cycles, int16_t x, int16_t y);

// Taps accepted so far, capped at WTAP_TARGET.
unsigned wallet_tapent_count(void);

// Copy the finished chain into out[32]. Returns 0 only once the target is
// reached, nonzero otherwise — a partial chain is never handed out.
int wallet_tapent_take(uint8_t out[32]);
