// dice roll quality: judge the raw digits BEFORE the hash whitens them.
// See docs/superpowers/specs/2026-07-30-dice-entropy-design.md.
//
// Deliberately a separate file from wallet_dice.c: that one pulls in
// wally_crypto.h and is stubbed out by the simulator, while this one needs no
// crypto at all — so the sim links the REAL judgement and every gate renders
// the true verdicts in all 21 locales.
//
// Why not Krux's numbers. Krux (the reference implementation) warns when the
// plug-in Shannon estimate falls under min_bits - 2, i.e. 126 bits at 50
// rolls. But the plug-in estimator runs ~(k-1)/(2N*ln2) = 3.6 bits LOW at
// N=50, so exact enumeration of all 3,478,761 six bin compositions of 50 puts
// P(estimate < 126) at 0.4953 — their warning fires on HALF of honest
// sessions. (Reproduce: sum multinomial mass over compositions with
// int(H*N) < 126.) A warning that often is a warning users learn to click
// through. WD_RATE (wallet_dice.h) is set from the enumeration instead:
// 1 in ~1.1 million honest sessions.
//
// Why the steps use the same threshold. For fair rolls over Z6 the map
// (r1..rN) -> (r1, d1..dN-1) with di = (r_i+1 - r_i) mod 6 is a bijection
// onto equally fair values, so the step histogram is judged by the SAME
// statistic at N-1 — no second theory, no second constant. That is what
// catches 1,2,3,4,5,6 repeating: perfectly level faces, zero level steps.
#include "wallet_dice.h"

// log2(n) * 1000, rounded to nearest, n = 0..DICE_MAX. Regenerate with:
//   python3 -c "import math;print([0]+[int(math.log2(n)*1000+0.5) for n in range(1,181)])"
// That one line is the whole table, and the table is the whole log2 — no
// libm, no float, nothing that can round differently on the device than on
// the owner's computer. uint16_t is exact: the largest entry is 7492.
static const uint16_t WD_L2[DICE_MAX + 1] = {
    0, 0, 1000, 1585, 2000, 2322, 2585, 2807, 3000, 3170,
    3322, 3459, 3585, 3700, 3807, 3907, 4000, 4087, 4170, 4248,
    4322, 4392, 4459, 4524, 4585, 4644, 4700, 4755, 4807, 4858,
    4907, 4954, 5000, 5044, 5087, 5129, 5170, 5209, 5248, 5285,
    5322, 5358, 5392, 5426, 5459, 5492, 5524, 5555, 5585, 5615,
    5644, 5672, 5700, 5728, 5755, 5781, 5807, 5833, 5858, 5883,
    5907, 5931, 5954, 5977, 6000, 6022, 6044, 6066, 6087, 6109,
    6129, 6150, 6170, 6190, 6209, 6229, 6248, 6267, 6285, 6304,
    6322, 6340, 6358, 6375, 6392, 6409, 6426, 6443, 6459, 6476,
    6492, 6508, 6524, 6539, 6555, 6570, 6585, 6600, 6615, 6629,
    6644, 6658, 6672, 6687, 6700, 6714, 6728, 6741, 6755, 6768,
    6781, 6794, 6807, 6820, 6833, 6845, 6858, 6870, 6883, 6895,
    6907, 6919, 6931, 6943, 6954, 6966, 6977, 6989, 7000, 7011,
    7022, 7033, 7044, 7055, 7066, 7077, 7087, 7098, 7109, 7119,
    7129, 7140, 7150, 7160, 7170, 7180, 7190, 7200, 7209, 7219,
    7229, 7238, 7248, 7257, 7267, 7276, 7285, 7295, 7304, 7313,
    7322, 7331, 7340, 7349, 7358, 7366, 7375, 7384, 7392, 7401,
    7409, 7418, 7426, 7435, 7443, 7451, 7459, 7468, 7476, 7484,
    7492
};

int32_t wallet_dice_bits(const unsigned c[6])
{
    unsigned n = c[0] + c[1] + c[2] + c[3] + c[4] + c[5];
    if (n == 0 || n > DICE_MAX) return 0;
    // Largest intermediate is 180 * 7492 = 1,348,560: int32 with room over.
    int32_t b = (int32_t)n * WD_L2[n];
    for (int i = 0; i < 6; i++) b -= (int32_t)c[i] * WD_L2[c[i]];
    return b;
}

// The whole string is one block typed twice or more: returns the block
// length, or 0. This is the case both entropy tests are blind to — roll ten
// honestly, type them five times, and the faces AND the steps come out level
// while the string carries ~26 real bits. Cost is at most ~16k comparisons at
// N=180; the chance of honest rolls forming any period is ~6^-25, never.
static unsigned wd_period(const char *d, unsigned n)
{
    for (unsigned p = 1; p * 2 <= n; p++) {
        unsigned i = p;
        while (i < n && d[i] == d[i - p]) i++;
        if (i == n) return p;
    }
    return 0;
}

void wallet_dice_judge(const char *digits, unsigned n, unsigned len,
                       wallet_dice_q_t *out)
{
    if (!out) return;
    *out = (wallet_dice_q_t){0};
    out->floor = (len == 32) ? DICE_FLOOR_256 : DICE_FLOOR_128;
    if (!digits) { out->verdict = WD_Q_SHORT; return; }
    if (n > DICE_MAX) n = DICE_MAX;
    out->n = n;

    for (unsigned i = 0; i < n; i++) {
        int f = digits[i] - '1';                // '1'..'6' -> 0..5
        if (f < 0 || f > 5) continue;           // roll() never stores anything else
        out->face[f]++;
        if (i) out->step[(digits[i] - digits[i - 1] + 6) % 6]++;
    }
    out->bits      = wallet_dice_bits(out->face);
    out->step_bits = wallet_dice_bits(out->step);
    out->period    = wd_period(digits, n);

    // Below the floor there is nothing to judge: at small N the estimator's
    // own bias would fire the check about half the time, so the verdict
    // appears exactly when DONE becomes usable and not a roll sooner.
    if (n < out->floor) { out->verdict = WD_Q_SHORT; return; }

    if (out->bits < (int32_t)WD_RATE * (int32_t)n)
        out->verdict = WD_Q_UNEVEN;
    else if (out->step_bits < (int32_t)WD_RATE * (int32_t)(n - 1) || out->period)
        out->verdict = WD_Q_PATTERN;
    else
        out->verdict = WD_Q_OK;
}
