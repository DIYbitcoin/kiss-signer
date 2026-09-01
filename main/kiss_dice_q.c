// dice roll quality: judge the raw digits BEFORE the hash whitens them.
// See design/specs/2026-07-30-dice-entropy-design.md.
//
// Deliberately a separate file from kiss_dice.c: that one pulls in
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
// through. WD_RATE (kiss_dice.h) is set from the enumeration instead:
// 1 in ~1.1 million honest sessions.
//
// Why the steps use the same threshold. For fair rolls over Z6 the map
// (r1..rN) -> (r1, d1..dN-1) with di = (r_i+1 - r_i) mod 6 is a bijection
// onto equally fair values, so the step histogram is judged by the SAME
// statistic at N-1 — no second theory, no second constant. That is what
// catches 1,2,3,4,5,6 repeating: perfectly level faces, zero level steps.
//
// A coin is the same three tests over Z2 and not a second judge: the parse,
// the step modulus and the bar are the only things that know the base. The
// bijection above holds over any Zb, so the step test needs no new argument,
// and at base 2 it is the one doing the work — with two bins the count test
// only sees a heavy lean, while an alternating string reads as level counts
// and zero level steps. WD_RATE_2 in kiss_dice.h carries the enumeration.
#include "kiss_dice.h"

// log2(n) * 1000, rounded to nearest, n = 0..DICE_MAX. Regenerate with:
//   python3 -c "import math;print([0]+[int(math.log2(n)*1000+0.5) for n in range(1,321)])"
// That one line is the whole table, and the table is the whole log2 — no
// libm, no float, nothing that can round differently on the device than on
// the owner's computer. uint16_t is exact: the largest entry is 8322.
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
    7492, 7500, 7508, 7516, 7524, 7531, 7539, 7547, 7555, 7562,
    7570, 7577, 7585, 7592, 7600, 7607, 7615, 7622, 7629, 7637,
    7644, 7651, 7658, 7665, 7672, 7679, 7687, 7693, 7700, 7707,
    7714, 7721, 7728, 7735, 7741, 7748, 7755, 7762, 7768, 7775,
    7781, 7788, 7794, 7801, 7807, 7814, 7820, 7827, 7833, 7839,
    7845, 7852, 7858, 7864, 7870, 7877, 7883, 7889, 7895, 7901,
    7907, 7913, 7919, 7925, 7931, 7937, 7943, 7948, 7954, 7960,
    7966, 7972, 7977, 7983, 7989, 7994, 8000, 8006, 8011, 8017,
    8022, 8028, 8033, 8039, 8044, 8050, 8055, 8061, 8066, 8071,
    8077, 8082, 8087, 8093, 8098, 8103, 8109, 8114, 8119, 8124,
    8129, 8134, 8140, 8145, 8150, 8155, 8160, 8165, 8170, 8175,
    8180, 8185, 8190, 8195, 8200, 8205, 8209, 8214, 8219, 8224,
    8229, 8234, 8238, 8243, 8248, 8253, 8257, 8262, 8267, 8271,
    8276, 8281, 8285, 8290, 8295, 8299, 8304, 8308, 8313, 8317,
    8322
};

int32_t kiss_dice_bits(const unsigned c[6])
{
    unsigned n = c[0] + c[1] + c[2] + c[3] + c[4] + c[5];
    if (n == 0 || n > DICE_MAX) return 0;
    // Largest intermediate is 320 * 8322 = 2,663,040: int32 with room over.
    int32_t b = (int32_t)n * WD_L2[n];
    for (int i = 0; i < 6; i++) b -= (int32_t)c[i] * WD_L2[c[i]];
    return b;
}

// The whole string is one block typed twice or more: returns the block
// length, or 0. This is the case both entropy tests are blind to — roll ten
// honestly, type them five times, and the faces AND the steps come out level
// while the string carries ~26 real bits. Cost is at most ~51k comparisons at
// N=320; the chance of honest rolls forming any period is ~6^-25 for a die and
// ~2^-63 for 128 flips, never either way.
static unsigned wd_period(const char *d, unsigned n)
{
    for (unsigned p = 1; p * 2 <= n; p++) {
        unsigned i = p;
        while (i < n && d[i] == d[i - p]) i++;
        if (i == n) return p;
    }
    return 0;
}

// The floor lives here, beside the judge, and not in kiss_dice.c: that file is
// stubbed out by the simulator, and a floor the stub owned could drift from the
// one the verdict is measured against.
unsigned kiss_dice_floor(unsigned base, unsigned len)
{
    if (base == 2) return (len == 32) ? COIN_FLOOR_256 : COIN_FLOOR_128;
    return (len == 32) ? DICE_FLOOR_256 : DICE_FLOOR_128;
}

int kiss_dice_blocked(int verdict)
{
    return verdict == WD_Q_UNEVEN || verdict == WD_Q_PATTERN;
}

void kiss_dice_judge(const char *digits, unsigned n, unsigned base,
                       unsigned len, kiss_dice_q_t *out)
{
    if (!out) return;
    *out = (kiss_dice_q_t){0};
    base = (base == 2) ? 2 : 6;
    out->base  = base;
    out->floor = kiss_dice_floor(base, len);
    if (!digits) { out->verdict = WD_Q_SHORT; return; }
    if (n > DICE_MAX) n = DICE_MAX;
    out->n = n;

    const char zero = (base == 2) ? '0' : '1';   // the char face 1 records as
    for (unsigned i = 0; i < n; i++) {
        int f = digits[i] - zero;               // '1'..'6' -> 0..5, '0'/'1' -> 0/1
        if (f < 0 || (unsigned)f >= base) continue;  // roll() stores nothing else
        out->face[f]++;
        if (i) out->step[(digits[i] - digits[i - 1] + base) % base]++;
    }
    out->bits      = kiss_dice_bits(out->face);
    out->step_bits = kiss_dice_bits(out->step);
    out->period    = wd_period(digits, n);

    // Below the floor there is nothing to judge: at small N the estimator's
    // own bias would fire the check about half the time, so the verdict
    // appears exactly when DONE becomes usable and not a roll sooner.
    if (n < out->floor) { out->verdict = WD_Q_SHORT; return; }

    const int32_t rate = (base == 2) ? WD_RATE_2 : WD_RATE;
    if (out->bits < rate * (int32_t)n)
        out->verdict = WD_Q_UNEVEN;
    else if (out->step_bits < rate * (int32_t)(n - 1) || out->period)
        out->verdict = WD_Q_PATTERN;
    else
        out->verdict = WD_Q_OK;
}
