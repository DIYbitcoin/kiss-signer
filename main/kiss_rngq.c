#include "kiss_rngq.h"
#include <string.h>

void kiss_rngq_reset(kiss_rngq_t *q, int bins)
{
    memset(q, 0, sizeof *q);
    q->bins = (bins == 50) ? 50 : 100;
}

size_t kiss_rngq_feed(kiss_rngq_t *q, const uint8_t *b, size_t len)
{
    unsigned limit = 256u - (256u % q->bins);   // 200 for 100 bins, 250 for 50
    size_t took = 0;
    for (size_t i = 0; i < len && q->n < RNGQ_SAMPLES; i++) {
        if (b[i] >= limit) continue;
        q->bin[b[i] % q->bins]++;
        q->n++;
        took++;
    }
    return took;
}

uint32_t kiss_rngq_chi2_milli(const kiss_rngq_t *q)
{
    // Worst case is every sample in one pile: at 100 bins that sum of squares
    // is (5000-50)^2 + 99*50^2 = 24,750,000, and x20 = 495,000,000 sits inside
    // uint32 with room to spare.
    uint32_t e = RNGQ_SAMPLES / q->bins;   // 50 or 100
    uint32_t mul = 1000u / e;              // 20 or 10, both exact
    uint32_t ss = 0;
    for (unsigned i = 0; i < q->bins; i++) {
        int32_t d = (int32_t)q->bin[i] - (int32_t)e;
        ss += (uint32_t)(d * d);
    }
    return ss * mul;
}

int kiss_rngq_verdict(const kiss_rngq_t *q, uint32_t chi2_milli)
{
    // The 0.001 and 0.999 chi square quantiles: the central 99.8% interval,
    // the same figures the lnbits hardware-wallet /trng tool prints at 99
    // degrees of freedom, recomputed for 49. The score is always a multiple
    // of 20 (or 10), so the boundaries themselves are never landed on.
    uint32_t lo = (q->bins == 50) ? 23983u : 61137u;
    uint32_t hi = (q->bins == 50) ? 85351u : 148230u;
    return chi2_milli < lo ? RNGQ_LOW : chi2_milli > hi ? RNGQ_HIGH : RNGQ_PASS;
}

void kiss_rngq_test_fill(uint64_t seed, uint8_t *out, size_t n)
{
    // splitmix64, one byte per step. Statistical quality is beside the point
    // here; sameness is the point.
    for (size_t i = 0; i < n; i++) {
        seed += 0x9E3779B97F4A7C15ull;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        out[i] = (uint8_t)(z ^ (z >> 31));
    }
}
