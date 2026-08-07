// Host tests for the dice-entropy module. The SHA256 vectors are the proof of
// verifiability: they must equal `printf '<rolls>' | sha256sum`.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <math.h>    // ONLY for proving WD_L2 equals its one-liner; the module
#include <stdio.h>   // under test must never include it.
#include <string.h>
#include "wally_core.h"
#include "wally_crypto.h"
#include "wallet_dice.h"
#include "wallet_seed.h"

static int fails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); fails++; }
}

static void hex(const uint8_t *b, unsigned n, char *out)
{
    for (unsigned i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
}

static void roll_str(const char *s)
{
    wallet_dice_reset();
    for (const char *p = s; *p; p++) wallet_dice_roll(*p - '0');
}

// printf '12345'*10 (50 chars) | sha256sum
static const char *KAT50 =
    "5eca9288344f8143aa96673f67faf41314a3157e2b4defca2acfafb6f6c29fbd";
// printf '123456'*16 '123' (99 chars) | sha256sum
static const char *KAT99 =
    "5588d3630bd19f6375b7bd922457af34ea9c74f00807566a1cf808e445dc8c20";
static const char *R50 =
    "12345123451234512345123451234512345123451234512345";
static const char *R99 =
    "123456123456123456123456123456123456123456123456"
    "123456123456123456123456123456123456123456123456123";

// ---- roll quality (wallet_dice_q.c) ----
//
// Why the thresholds are NOT Krux's: Krux warns when the plug-in Shannon
// estimate falls under min_bits - 2 (126 bits at 50 rolls). The estimator is
// biased ~3.6 bits low at N=50, so exact enumeration of all 3,478,761 six bin
// compositions of 50 gives P(estimate < 126) = 0.4953 — their warning fires
// on HALF of honest sessions. Reproduce: enumerate compositions c of 50 over
// 6 bins, sum the multinomial mass of those with int(H(c)*50) < 126.
// WD_RATE = 2050 was chosen from the same enumeration: 1 in ~1.1 million.

static void judge_str(const char *s, unsigned need, wallet_dice_q_t *q)
{
    wallet_dice_judge(s, (unsigned)strlen(s), need, q);
}

// Deterministic xorshift32: fixture generation must never depend on libc rand.
static uint32_t s_xs;
static uint32_t xs32(void)
{
    s_xs ^= s_xs << 13; s_xs ^= s_xs >> 17; s_xs ^= s_xs << 5;
    return s_xs;
}

static int test_dice_q(void)
{
    int qfails = fails; (void)qfails;
    fails = 0;
    printf("\n-- dice roll quality --\n");
    wallet_dice_q_t q, q2;

    // ---- the table equals its one-liner ----
    // WD_L2 is private, but bits({1, n-1, 0..}) = n*L2[n] - (n-1)*L2[n-1]
    // (L2[1] = 0), so the prefix sum telescopes to n*L2[n] exactly. Checking
    // that against host log2 for every n proves all 181 entries. The ONLY
    // float allowed near this feature lives right here.
    {
        int bad = 0;
        int32_t acc = 0;
        for (unsigned n = 2; n <= 180; n++) {
            unsigned c[6] = { 1, n - 1, 0, 0, 0, 0 };
            acc += wallet_dice_bits(c);
            int32_t want = (int32_t)n * (int32_t)(log2((double)n) * 1000.0 + 0.5);
            if (acc != want) bad++;
        }
        ok("WD_L2 table == round(log2(n)*1000) for all n", bad == 0);
    }

    // ---- statistic known answers on hand built histograms ----
    { unsigned c[6] = {50,0,0,0,0,0};   ok("bits: one face = 0", wallet_dice_bits(c) == 0); }
    { unsigned c[6] = {25,25,0,0,0,0};  ok("bits: 25/25 = 50000 exactly", wallet_dice_bits(c) == 50000); }
    { unsigned c[6] = {9,9,8,8,8,8};    ok("bits: 9,9,8,8,8,8 = 129140", wallet_dice_bits(c) == 129140); }
    { unsigned c[6] = {0,0,0,0,0,0};    ok("bits: empty = 0", wallet_dice_bits(c) == 0); }

    // ---- true positives, exact verdicts ----
    judge_str("11111111111111111111111111111111111111111111111111", 16, &q);
    ok("all same x50 -> UNEVEN", q.verdict == WD_Q_UNEVEN && q.bits == 0);
    {
        char s[100]; memset(s, '4', 99); s[99] = 0;
        judge_str(s, 32, &q);
        ok("all same x99 -> UNEVEN", q.verdict == WD_Q_UNEVEN);
    }
    judge_str("65432165432165432165432165432165432165432165432165", 16, &q);
    ok("descending ramp -> PATTERN", q.verdict == WD_Q_PATTERN && q.step_bits == 0);
    judge_str("12121212121212121212121212121212121212121212121212", 16, &q);
    ok("12 x25 -> UNEVEN", q.verdict == WD_Q_UNEVEN && q.bits == 50000);
    judge_str("22122222212221112122111221211121112211212222122112", 16, &q);
    ok("two faces, shuffled -> UNEVEN", q.verdict == WD_Q_UNEVEN);
    judge_str("12313331113113122123311332231212131113333121223123", 16, &q);
    ok("three faces, shuffled -> UNEVEN", q.verdict == WD_Q_UNEVEN);
    judge_str("44244242324341424334221444444421131243314421113241", 16, &q);
    ok("four faces, shuffled -> UNEVEN", q.verdict == WD_Q_UNEVEN);

    // The case both entropy tests are blind to: ten honest rolls typed five
    // times. Faces pass (126100), steps pass (114763) — only the period
    // check sees it. This one line is why wd_period exists.
    judge_str("14362615531436261553143626155314362615531436261553", 16, &q);
    ok("10 roll block x5 -> PATTERN by period alone",
       q.verdict == WD_Q_PATTERN && q.period == 10 &&
       q.bits >= 2050 * 50 && q.step_bits >= 2050 * 49);

    // ---- the KAT fixtures double as pattern fixtures ----
    judge_str(R50, 16, &q);
    ok("R50 -> PATTERN (period 5)", q.verdict == WD_Q_PATTERN && q.period == 5);
    judge_str(R99, 32, &q);
    ok("R99 -> PATTERN (steps all 1)", q.verdict == WD_Q_PATTERN && q.step_bits == 0);

    // ---- the four/five face boundary is a stated contract ----
    // The most level four face histogram possible still fires; five faces
    // dead level passes. That is what WD_RATE between log2(4) and log2(5)
    // MEANS, so it is asserted, not implied.
    { unsigned c[6] = {13,13,12,12,0,0}; ok("four faces, most level, 50: under the bar", wallet_dice_bits(c) <  2050 * 50); }
    { unsigned c[6] = {25,25,25,24,0,0}; ok("four faces, most level, 99: under the bar", wallet_dice_bits(c) <  2050 * 99); }
    { unsigned c[6] = {10,10,10,10,10,0}; ok("five faces, level, 50: over the bar",      wallet_dice_bits(c) >= 2050 * 50); }
    { unsigned c[6] = {20,20,20,20,19,0}; ok("five faces, level, 99: over the bar",      wallet_dice_bits(c) >= 2050 * 99); }
    judge_str("32242143134455423244323113221421221154241112323431", 16, &q);
    ok("five faces, shuffled -> OK", q.verdict == WD_Q_OK);

    // The healthy run the sim walk types (SIM_DICE_OK in sim/sim_main.c). The
    // walk's comment quotes these numbers; this is where they are enforced.
    judge_str("14464111145452332224636431261353544615153616323265", 16, &q);
    ok("sim walk healthy string -> OK",
       q.verdict == WD_Q_OK && q.bits == 128622 && q.step_bits == 126605 &&
       q.period == 0);

    // ---- below the floor: never judged ----
    judge_str("1111111111111111111111111111111111111111111111111", 16, &q);   // 49
    ok("49 rolls -> SHORT even all same", q.verdict == WD_Q_SHORT);
    {
        char s[99]; memset(s, '1', 98); s[98] = 0;
        judge_str(s, 32, &q);
        ok("98 rolls -> SHORT even all same", q.verdict == WD_Q_SHORT);
    }

    // ---- false positive rate, stated as a rate ----
    // 20,000 honest strings at each floor from a FIXED seed: deterministic,
    // cannot flake, and catches a threshold set an order of magnitude too
    // tight. The contract is under 0.1%; the measured value is 0.
    {
        int bad = 0; int32_t worst = 0x7FFFFFFF;
        char s[100];
        s_xs = 0xDECAF;
        for (int t = 0; t < 20000; t++) {
            for (int i = 0; i < 50; i++) s[i] = (char)('1' + xs32() % 6);
            s[50] = 0;
            wallet_dice_judge(s, 50, 16, &q);
            if (q.verdict != WD_Q_OK) bad++;
            int32_t m = q.bits - 2050 * 50;
            if (m < worst) worst = m;
        }
        for (int t = 0; t < 20000; t++) {
            for (int i = 0; i < 99; i++) s[i] = (char)('1' + xs32() % 6);
            s[99] = 0;
            wallet_dice_judge(s, 99, 32, &q);
            if (q.verdict != WD_Q_OK) bad++;
        }
        printf("  fp sweep: %d flagged of 40000, worst 50-roll face margin %d milli-bits\n",
               bad, (int)worst);
        // Zero, not a loose bound. A flag used to cost one tap on USE ANYWAY
        // and now costs the session, so "under 1 in 1000" no longer describes
        // what is being promised. The measured value has always been 0.
        ok("honest rolls: none flagged", bad == 0);
    }

    // ---- what a block is, in one place ----
    ok("blocked: UNEVEN refuses",  wallet_dice_blocked(WD_Q_UNEVEN) == 1);
    ok("blocked: PATTERN refuses", wallet_dice_blocked(WD_Q_PATTERN) == 1);
    ok("blocked: OK does not",     wallet_dice_blocked(WD_Q_OK) == 0);
    ok("blocked: SHORT does not",  wallet_dice_blocked(WD_Q_SHORT) == 0);

    // ---- reproducibility ----
    judge_str("32242143134455423244323113221421221154241112323431", 16, &q);
    judge_str("32242143134455423244323113221421221154241112323431", 16, &q2);
    ok("same string -> identical judgement", memcmp(&q, &q2, sizeof q) == 0);

    // ---- the raised ceiling ----
    wallet_dice_reset();
    { int accepted = 0;
      for (int i = 0; i < 200; i++) accepted += wallet_dice_roll(1 + i % 6);
      ok("ceiling: exactly DICE_MAX rolls accepted", accepted == DICE_MAX && DICE_MAX == 180); }
    ok("ceiling: roll 181 refused", wallet_dice_roll(3) == 0);
    wallet_dice_reset();

    return fails;
}

int test_dice(void)
{
    fails = 0;
    printf("\n-- dice entropy --\n");

    // roll / count / invalid / undo
    wallet_dice_reset();
    ok("starts empty", wallet_dice_count() == 0);
    ok("valid face accepted", wallet_dice_roll(4) == 1);
    ok("count is 1", wallet_dice_count() == 1);
    ok("face 0 rejected", wallet_dice_roll(0) == 0);
    ok("face 7 rejected", wallet_dice_roll(7) == 0);
    ok("count unchanged after invalid", wallet_dice_count() == 1);
    ok("undo removes it", wallet_dice_undo() == 1);
    ok("empty again", wallet_dice_count() == 0);
    ok("undo on empty is 0", wallet_dice_undo() == 0);
    wallet_dice_roll(1); wallet_dice_roll(2);
    ok("digits reflect rolls", strcmp(wallet_dice_digits(), "12") == 0);

    // ---- floor gate ----
    uint8_t e[32]; char got[65];
    roll_str(R50);                                  // 50 rolls
    ok("50 rolls: 12-word take ok", wallet_dice_take(e, 16) == 0);
    ok("50 rolls: 24-word take refused", wallet_dice_take(e, 32) == -1);
    wallet_dice_undo();                             // 49 rolls
    ok("49 rolls: 12-word take refused", wallet_dice_take(e, 16) == -1);

    // ---- KAT: 12-word entropy == first 16 bytes of SHA256(R50) ----
    roll_str(R50);
    wallet_dice_take(e, 16); hex(e, 16, got);
    ok("12-word entropy == SHA256(R50)[0..16]", strncmp(got, KAT50, 32) == 0);
    if (strncmp(got, KAT50, 32) != 0) printf("  got %s\n  want %.32s\n", got, KAT50);

    // ---- KAT: 24-word entropy == SHA256(R99) ----
    roll_str(R99);
    ok("99 rolls: 24-word take ok", wallet_dice_take(e, 32) == 0);
    hex(e, 32, got);
    ok("24-word entropy == SHA256(R99)", strcmp(got, KAT99) == 0);
    if (strcmp(got, KAT99) != 0) printf("  got %s\n  want %s\n", got, KAT99);

    // ---- entropy -> a real, deterministic BIP39 mnemonic ----
    // (self-consistency here; the human off-device cross-check against an
    // external BIP39 tool is the device-acceptance step in the spec.)
    char words[256], words2[256];
    ok("entropy -> mnemonic rc",
       wallet_seed_from_entropy(e, 32, words, sizeof words) == 0);
    roll_str(R99); wallet_dice_take(e, 32);
    ok("same rolls -> same mnemonic",
       wallet_seed_from_entropy(e, 32, words2, sizeof words2) == 0 &&
       strcmp(words, words2) == 0);

    // ---- length validation + wipe ----
    ok("bad len rejected", wallet_dice_take(e, 20) == -1);
    ok("NULL out rejected", wallet_dice_take(NULL, 32) == -1);
    wallet_dice_reset();
    ok("reset clears count", wallet_dice_count() == 0);
    ok("reset clears digits", wallet_dice_digits()[0] == '\0');

    // ---- peek: full SHA256 of the current digits, no floor ----
    roll_str(R50);
    ok("peek rc", wallet_dice_peek(e) == 0);
    hex(e, 32, got);
    ok("peek == SHA256(R50) full", strcmp(got, KAT50) == 0);
    if (strcmp(got, KAT50) != 0) printf("  got %s\n  want %s\n", got, KAT50);
    ok("peek under the floor still works", (wallet_dice_undo(), wallet_dice_peek(e) == 0));
    ok("peek NULL rejected", wallet_dice_peek(NULL) == -1);

    return fails + test_dice_q();
}
