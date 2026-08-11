// Host tests for the cards draw judge. Needs no wordlist and no crypto: the
// module under test is pure integer math over wordlist indices, so every
// fixture here is built by hand and the whole suite is bit-identical on any
// compiler.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "kiss_cards_q.h"

static int cfails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); cfails++; }
}

// Deterministic xorshift32: fixture generation must never depend on libc rand.
static uint32_t s_xs;
static uint32_t xs32(void)
{
    s_xs ^= s_xs << 13; s_xs ^= s_xs >> 17; s_xs ^= s_xs << 5;
    return s_xs;
}

// A draw with nothing wrong with it: 613 is prime and coprime to 2048, so the
// indices are distinct and every consecutive gap is 613 or 1435 — far outside
// WC_NEAR — while the wrap makes the sequence non monotone. Every fixture below
// starts here and breaks exactly one thing, so a verdict can only come from the
// break and never from the base.
static void clean(uint16_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; i++) w[i] = (uint16_t)((i * 613 + 41) % 2048);
}

int test_cards_q(void)
{
    cfails = 0;
    printf("\n-- cards draw quality --\n");
    kiss_cards_q_t q, q2;
    uint16_t w[32];

    // ---- the base fixture really is clean ----
    clean(w, 11);
    kiss_cards_judge(w, 11, &q);
    ok("clean 11 -> OK",
       q.verdict == WC_Q_OK && q.flags == 0 && q.distinct == 11 &&
       q.dups == 0 && q.near == 0 && q.sorted == 0 && q.period == 0);
    clean(w, 23);
    kiss_cards_judge(w, 23, &q);
    ok("clean 23 -> OK", q.verdict == WC_Q_OK && q.flags == 0);

    // ---- blocks ----
    for (unsigned i = 0; i < 23; i++) w[i] = 777;
    kiss_cards_judge(w, 11, &q);
    ok("11 identical -> SAME",
       q.verdict == WC_Q_SAME && kiss_cards_blocked(&q) &&
       q.distinct == 1 && q.dups == 10 && q.period == 1 &&
       (q.flags & WC_F_SAME) && (q.flags & WC_F_PERIOD) &&
       !(q.flags & WC_F_SORTED));
    kiss_cards_judge(w, 23, &q);
    ok("23 identical -> SAME", q.verdict == WC_Q_SAME && kiss_cards_blocked(&q));

    // "abandon" is wordlist index 0 (pinned in sim/test_lastword.c). That file
    // uses this exact draw as a PASS: candidates() must keep answering 128 for
    // it forever. This assertion is the other half of that sentence, and the
    // two together are what stop anyone "fixing" either module by breaking the
    // other.
    memset(w, 0, sizeof w);
    kiss_cards_judge(w, 11, &q);
    ok("abandon x11 (the BIP39 zero vector) -> SAME, blocked",
       q.verdict == WC_Q_SAME && kiss_cards_blocked(&q));
    kiss_cards_judge(w, 23, &q);
    ok("abandon x23 -> SAME, blocked",
       q.verdict == WC_Q_SAME && kiss_cards_blocked(&q));

    // A five word run typed over: the partial tail form a hand actually makes.
    clean(w, 11);
    for (unsigned i = 5; i < 11; i++) w[i] = w[i - 5];
    kiss_cards_judge(w, 11, &q);
    ok("5 word block, typed over -> PERIOD",
       q.verdict == WC_Q_PERIOD && kiss_cards_blocked(&q) && q.period == 5);

    // ---- the period boundary is a stated contract ----
    // wc_period only looks at p with 2p <= n, so a smallest repeat of 6 at
    // eleven words is NOT periodic. Asserted rather than implied, exactly as
    // test_dice.c asserts the four/five face boundary.
    clean(w, 11);
    for (unsigned i = 6; i < 11; i++) w[i] = w[i - 6];
    kiss_cards_judge(w, 11, &q);
    ok("smallest repeat 6 at n=11 -> not periodic",
       q.period == 0 && !(q.flags & WC_F_PERIOD) && !kiss_cards_blocked(&q));

    // ---- sorted ----
    clean(w, 11);
    for (unsigned i = 0; i < 11; i++) w[i] = (uint16_t)(40 + i * 137);
    kiss_cards_judge(w, 11, &q);
    ok("ascending, wide gaps -> SORTED",
       q.verdict == WC_Q_SORTED && !kiss_cards_blocked(&q) &&
       q.sorted == 1 && q.near == 0 && q.dups == 0);
    for (unsigned i = 0; i < 11; i++) w[i] = (uint16_t)(1800 - i * 137);
    kiss_cards_judge(w, 11, &q);
    ok("descending, wide gaps -> SORTED", q.verdict == WC_Q_SORTED && q.sorted == -1);
    // Non strict: one repeat inside a sorted run is still a sorted run.
    for (unsigned i = 0; i < 11; i++) w[i] = (uint16_t)(40 + i * 137);
    w[5] = w[4];
    kiss_cards_judge(w, 11, &q);
    ok("sorted with one repeat -> still SORTED",
       q.verdict == WC_Q_SORTED && q.sorted == 1 && q.dups == 1);

    // ---- cluster, and the WC_NEAR boundary as a contract ----
    for (unsigned i = 0; i < 11; i++) w[i] = (uint16_t)(100 + i);
    kiss_cards_judge(w, 11, &q);
    ok("11 consecutive indices -> CLUSTER (over SORTED)",
       q.verdict == WC_Q_CLUSTER && q.near == 10 && (q.flags & WC_F_SORTED));

    // Step 8 is a neighbour, step 9 is not. Both fixtures end on a far index so
    // the sequence is not monotone and only the neighbour rule can speak.
    for (unsigned i = 0; i < 10; i++) w[i] = (uint16_t)(500 + i * 8);
    w[10] = 100;
    kiss_cards_judge(w, 11, &q);
    ok("constant step 8 -> CLUSTER", q.verdict == WC_Q_CLUSTER && q.near == 9);
    for (unsigned i = 0; i < 10; i++) w[i] = (uint16_t)(500 + i * 9);
    w[10] = 100;
    kiss_cards_judge(w, 11, &q);
    ok("constant step 9 -> OK", q.verdict == WC_Q_OK && q.near == 0);

    // ---- thresholds, from both sides, at both sizes ----
    // Duplicates: dups is n - distinct, so k extra copies of one word.
    clean(w, 11);
    w[3] = w[0]; w[7] = w[0];
    kiss_cards_judge(w, 11, &q);
    ok("n=11, dups 2 -> OK", q.dups == 2 && q.verdict == WC_Q_OK);
    w[9] = w[0];
    kiss_cards_judge(w, 11, &q);
    ok("n=11, dups 3 -> DUP", q.dups == 3 && q.verdict == WC_Q_DUP);

    clean(w, 23);
    w[3] = w[0]; w[7] = w[0]; w[11] = w[0]; w[15] = w[0];
    kiss_cards_judge(w, 23, &q);
    ok("n=23, dups 4 -> OK", q.dups == 4 && q.verdict == WC_Q_OK);
    w[19] = w[0];
    kiss_cards_judge(w, 23, &q);
    ok("n=23, dups 5 -> DUP", q.dups == 5 && q.verdict == WC_Q_DUP);

    // Neighbours: disjoint close pairs, so each one adds exactly one near pair.
    clean(w, 11);
    w[1] = (uint16_t)(w[0] + 5); w[3] = (uint16_t)(w[2] + 5); w[5] = (uint16_t)(w[4] + 5);
    kiss_cards_judge(w, 11, &q);
    ok("n=11, near 3 -> OK", q.near == 3 && q.verdict == WC_Q_OK);
    w[7] = (uint16_t)(w[6] + 5);
    kiss_cards_judge(w, 11, &q);
    ok("n=11, near 4 -> CLUSTER", q.near == 4 && q.verdict == WC_Q_CLUSTER);

    clean(w, 23);
    w[1] = (uint16_t)(w[0] + 5); w[3] = (uint16_t)(w[2] + 5);
    w[5] = (uint16_t)(w[4] + 5); w[7] = (uint16_t)(w[6] + 5);
    kiss_cards_judge(w, 23, &q);
    ok("n=23, near 4 -> OK", q.near == 4 && q.verdict == WC_Q_OK);
    w[9] = (uint16_t)(w[8] + 5);
    kiss_cards_judge(w, 23, &q);
    ok("n=23, near 5 -> CLUSTER", q.near == 5 && q.verdict == WC_Q_CLUSTER);

    // ---- nothing to judge yet ----
    for (unsigned i = 0; i < 10; i++) w[i] = 5;
    kiss_cards_judge(w, 10, &q);
    ok("10 words -> SHORT even all identical",
       q.verdict == WC_Q_SHORT && !kiss_cards_blocked(&q));
    kiss_cards_judge(NULL, 11, &q);
    ok("NULL -> SHORT", q.verdict == WC_Q_SHORT);

    // ---- false positive rate, stated as a rate ----
    // 20,000 blind draws at each size from a FIXED seed: integer judge plus
    // integer PRNG means this is byte identical on every host and cannot flake.
    // Expected flags at the published rates are 0.047 and 0.020, so the
    // contract is zero. If a seed ever does flag, publish the draw and confirm
    // it is a legitimate rare event — never loosen a threshold to make this
    // pass.
    {
        int bad11 = 0, bad23 = 0;
        s_xs = 0xB1F39;
        for (int t = 0; t < 20000; t++) {
            for (int i = 0; i < 11; i++) w[i] = (uint16_t)(xs32() % WC_LIST);
            kiss_cards_judge(w, 11, &q);
            if (q.verdict != WC_Q_OK) bad11++;
        }
        for (int t = 0; t < 20000; t++) {
            for (int i = 0; i < 23; i++) w[i] = (uint16_t)(xs32() % WC_LIST);
            kiss_cards_judge(w, 23, &q);
            if (q.verdict != WC_Q_OK) bad23++;
        }
        printf("  fp sweep: %d flagged of 20000 at 11 words, %d of 20000 at 23\n",
               bad11, bad23);
        ok("blind draws: none flagged", bad11 == 0 && bad23 == 0);
    }

    // ---- reproducibility ----
    clean(w, 11);
    kiss_cards_judge(w, 11, &q);
    kiss_cards_judge(w, 11, &q2);
    ok("same draw -> identical judgement", memcmp(&q, &q2, sizeof q) == 0);

    // ---- the ceiling clamps rather than reading past the array ----
    clean(w, 23);
    kiss_cards_judge(w, 99, &q);
    ok("n above WC_MAX is clamped", q.n == WC_MAX);

    return cfails;
}
