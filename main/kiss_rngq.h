// The randomness audit's arithmetic: chip numbers counted into piles, scored
// with chi square against the interval honest noise leaves 499 times in 500.
// Pure counting, no LVGL and no ESP headers, so /tmp/kisstest runs the same
// sums the screen shows. The screens live in kiss_rngaudit.c; why no spread
// score can ever identify a SOURCE is kiss_crypto.h's block comment.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { RNGQ_SAMPLES = 5000 };

// Verdicts. LOW is real: honest noise is lumpy, and a spread flatter than
// chance is the signature of something arranging the numbers, not of a good
// chip. Both misses read as "run it again" to the owner, never as an alarm --
// an honest source lands outside the interval about once in 500 runs.
enum { RNGQ_LOW = -1, RNGQ_PASS = 0, RNGQ_HIGH = 1 };

typedef struct {
    uint16_t bin[100];
    uint32_t n;       // samples accepted so far, stops at RNGQ_SAMPLES
    uint8_t  bins;    // 100, or 50 if the P4 renderer says the bars must halve
} kiss_rngq_t;

void kiss_rngq_reset(kiss_rngq_t *q, int bins);

// Feed raw bytes. Rejection keeps the piles fair: a byte at or above the
// largest multiple of `bins` would hand the low piles one extra path each,
// and the tool would be measuring its own bias. Returns how many bytes this
// call accepted.
size_t kiss_rngq_feed(kiss_rngq_t *q, const uint8_t *b, size_t len);

// chi square in thousandths, exact: the expected count RNGQ_SAMPLES/bins
// divides 1000 for both bin counts, so there is no float anywhere in the
// verdict path. Meaningful once q->n == RNGQ_SAMPLES.
uint32_t kiss_rngq_chi2_milli(const kiss_rngq_t *q);

int kiss_rngq_verdict(const kiss_rngq_t *q, uint32_t chi2_milli);

// Deterministic byte stream (splitmix64) for the sim walk and the tests: one
// seed renders one histogram and pins one golden score, so the walk's PASS
// picture can never drift from what test_rngq asserts. Never runs on the
// device.
void kiss_rngq_test_fill(uint64_t seed, uint8_t *out, size_t n);
