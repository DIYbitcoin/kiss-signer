// Host tests for the randomness audit's counting. The threshold constants are
// the 0.001/0.999 chi square quantiles at 99 and 49 degrees of freedom; the
// golden score pins the sim walk's PASS picture to one seed forever.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "kiss_rngq.h"

static int fails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); fails++; }
}

// The one seed the sim's kiss_trng_fill stub uses. If this constant moves,
// move it there too, or the walk photographs a histogram no test has scored.
#define RNGQ_SIM_SEED 0x4B495353u   // "KISS"

static int feed_all_bytes(kiss_rngq_t *q)
{
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    return (int)kiss_rngq_feed(q, all, sizeof all);
}

// Force a bin vector without pretending it came from bytes: the chi square
// and verdict functions only read bin[] and bins.
static void rig(kiss_rngq_t *q, int bins, uint16_t v)
{
    kiss_rngq_reset(q, bins);
    for (int i = 0; i < bins; i++) q->bin[i] = v;
    q->n = RNGQ_SAMPLES;
}

int test_rngq(void)
{
    kiss_rngq_t q;

    // ---- rejection sampling ----
    kiss_rngq_reset(&q, 100);
    ok("100 bins: 200 of 256 byte values accepted", feed_all_bytes(&q) == 200);
    ok("100 bins: each pile hit exactly twice", q.bin[0] == 2 && q.bin[99] == 2);
    kiss_rngq_reset(&q, 50);
    ok("50 bins: 250 of 256 byte values accepted", feed_all_bytes(&q) == 250);
    ok("50 bins: each pile hit exactly five times", q.bin[0] == 5 && q.bin[49] == 5);

    // ---- the feed stops at RNGQ_SAMPLES ----
    kiss_rngq_reset(&q, 100);
    uint8_t z[64] = {0};
    for (int i = 0; i < 200; i++) kiss_rngq_feed(&q, z, sizeof z);
    ok("feed caps at 5000", q.n == RNGQ_SAMPLES && q.bin[0] == RNGQ_SAMPLES);

    // ---- chi square, exact ----
    rig(&q, 100, 50);
    ok("level piles score 0", kiss_rngq_chi2_milli(&q) == 0);
    ok("score 0 reads TOO EVEN", kiss_rngq_verdict(&q, 0) == RNGQ_LOW);

    rig(&q, 100, 50);
    q.bin[3] = 60; q.bin[7] = 40;      // sum of squares 200 -> 4.000
    ok("one 60/40 swap scores 4.000", kiss_rngq_chi2_milli(&q) == 4000);

    rig(&q, 100, 50);
    q.bin[0] = RNGQ_SAMPLES;           // everything in one pile
    for (int i = 1; i < 100; i++) q.bin[i] = 0;
    ok("worst case computes without wrap",
       kiss_rngq_chi2_milli(&q) == 495000000u);
    ok("worst case reads UNEVEN",
       kiss_rngq_verdict(&q, 495000000u) == RNGQ_HIGH);

    // ---- thresholds, at the nearest representable scores ----
    rig(&q, 100, 50);
    ok("100 bins: 61.120 is LOW",  kiss_rngq_verdict(&q, 61120) == RNGQ_LOW);
    ok("100 bins: 61.140 is PASS", kiss_rngq_verdict(&q, 61140) == RNGQ_PASS);
    ok("100 bins: 148.220 is PASS", kiss_rngq_verdict(&q, 148220) == RNGQ_PASS);
    ok("100 bins: 148.240 is HIGH", kiss_rngq_verdict(&q, 148240) == RNGQ_HIGH);
    rig(&q, 50, 100);
    ok("50 bins: 23.980 is LOW",   kiss_rngq_verdict(&q, 23980) == RNGQ_LOW);
    ok("50 bins: 23.990 is PASS",  kiss_rngq_verdict(&q, 23990) == RNGQ_PASS);
    ok("50 bins: 85.350 is PASS",  kiss_rngq_verdict(&q, 85350) == RNGQ_PASS);
    ok("50 bins: 85.360 is HIGH",  kiss_rngq_verdict(&q, 85360) == RNGQ_HIGH);

    // ---- the sim's fixed seed produces a PASS, and its score is golden ----
    // 5000 accepted needs ~6400 raw bytes at 78% acceptance; take plenty.
    static uint8_t buf[16384];
    kiss_rngq_test_fill(RNGQ_SIM_SEED, buf, sizeof buf);
    kiss_rngq_reset(&q, 100);
    size_t used = 0, got = 0;
    while (q.n < RNGQ_SAMPLES && used < sizeof buf) {
        got = kiss_rngq_feed(&q, buf + used, 256);
        used += 256;
        (void)got;
    }
    ok("sim seed fills 5000 from 16KB", q.n == RNGQ_SAMPLES);
    uint32_t milli = kiss_rngq_chi2_milli(&q);
    printf("info: sim seed chi square %u.%03u\n", milli / 1000, milli % 1000);
    ok("sim seed scores PASS", kiss_rngq_verdict(&q, milli) == RNGQ_PASS);
    // Golden: the walk's PASS picture is this exact histogram. A change here
    // means the generator or the feed changed, and the pictures with it.
    ok("sim seed golden score 105.920", milli == 105920u);

    return fails;
}
