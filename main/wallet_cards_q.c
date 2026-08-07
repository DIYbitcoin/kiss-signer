// cards draw quality: judge the owner's own words BEFORE the checksum word
// joins them. See wallet_cards_q.h for what this does and does not promise.
//
// Why there is no entropy estimate here. The dice judge measures a six bin
// histogram over fifty rolls, where the counts mean something. Eleven draws
// from a 2048 word list have no histogram worth reading: the plug-in estimate
// tops out at log2(11) = 3.46 bits per word and reads the same for a blind
// draw and for a careful fake. So every rule below is a COUNT of structure,
// never a score, and the module carries no log table and no float at all.
//
// The null model. The owner is told to cut the list into cards, shuffle and
// draw blind, which is drawing WITHOUT replacement — under which duplicates are
// impossible. Every rate below is computed for drawing WITH replacement (return
// each card, reshuffle), because a threshold that is safe there is safe under
// both and the device cannot know which the owner did.
//
// Rates, exact enumeration rather than approximation. Reproduce with the
// fractions module: Stirling numbers of the second kind for the duplicate rows,
// a 2048 state transfer matrix over the draws for the neighbour rows. A Poisson
// approximation on pairs overcounts the duplicate rows, because a triple
// contributes three pairs but only two excess copies.
//
//                       11 words     23 words
//   all identical       7.7e-34      1.4e-73     BLOCK
//   period, 2p <= n     1.4e-20      1.8e-40     BLOCK
//   sorted              5.1e-8       8.8e-23     warn
//   dups >= 3           1.4e-6       2.0e-4      warn at 11 only
//   dups >= 5           4.9e-12      6.2e-8      warn at 23
//   near >= 4, K=8      9.5e-7       3.1e-5      warn at 11
//   near >= 5, K=8      9.5e-9       9.1e-7      warn at 23
//   ------------------------------------------------------------
//   combined warn       1 in 422k    1 in 1.03M
//
// The duplicate rule needs two constants because one does not fit: dups >= 3
// costs 1.4e-6 at eleven words and 2.0e-4 at twenty three, a factor of 150. At
// twenty three the next threshold down is 4.2e-6, four times looser than the
// dice line, so the rule takes 5 and lands twenty times tighter instead. That
// is the honest cost of holding one budget across two sizes.
//
// Dice holds 1 in 1.1 million. Twenty three words matches it; eleven words is
// 2.6x looser on purpose, because a false WARN costs one tap and a false BLOCK
// costs a redraw.
//
// Sorting is the one rule where the rate is not the argument. Typing a
// genuinely blind draw in list order discards log2(n!) — 25.2 bits at eleven
// words, taking a 128 bit seed to about 102.7 — because only the multiset
// survives the sort. That is a real loss and the screen can state it exactly,
// and it is also why sorting warns rather than blocks: 102.7 bits is not
// brute forceable, and a warning that overstates is one people learn to click
// through.
#include "wallet_cards_q.h"

// The whole sequence is one block typed over and over: returns the block
// length, or 0. Same shape and same reason as wd_period in wallet_dice_q.c —
// it is the case the counting rules are blind to. Both 11 and 23 are prime, so
// an EXACT repetition can only be p = 1 (all identical); p from 2 up catches
// the partial tail form, "a b c d e a b c d e a", which is the shape a hand
// actually produces. Cost is at most ~264 comparisons at n = 23.
static unsigned wc_period(const uint16_t *w, unsigned n)
{
    for (unsigned p = 1; p * 2 <= n; p++) {
        unsigned i = p;
        while (i < n && w[i] == w[i - p]) i++;
        if (i == n) return p;
    }
    return 0;
}

int wallet_cards_blocked(const wallet_cards_q_t *q)
{
    return q && (q->flags & WC_F_BLOCK) != 0;
}

void wallet_cards_judge(const uint16_t *idx, unsigned n, wallet_cards_q_t *out)
{
    if (!out) return;
    *out = (wallet_cards_q_t){0};
    if (!idx) { out->verdict = WC_Q_SHORT; return; }
    if (n > WC_MAX) n = WC_MAX;
    out->n = n;
    if (n < WC_MIN) { out->verdict = WC_Q_SHORT; return; }

    // n is at most 23, so the pairwise scan is at most 253 comparisons and
    // needs no scratch buffer, no sort and no allocation.
    for (unsigned i = 0; i < n; i++) {
        unsigned seen = 0;
        for (unsigned j = 0; j < i; j++)
            if (idx[j] == idx[i]) { seen = 1; break; }
        if (!seen) out->distinct++;
    }
    out->dups = n - out->distinct;

    int up = 1, down = 1;
    for (unsigned i = 1; i < n; i++) {
        if (idx[i] < idx[i - 1]) up = 0;
        if (idx[i] > idx[i - 1]) down = 0;
        int d = (int)idx[i] - (int)idx[i - 1];
        if (d < 0) d = -d;
        if (d <= WC_NEAR) out->near++;
    }
    // Non strict on purpose, so a sorted set with one repeat still counts as
    // sorted. All identical satisfies both directions, which is why the flag
    // below is guarded on distinct > 1 and left to WC_F_SAME instead.
    out->sorted = up ? 1 : down ? -1 : 0;
    out->period = wc_period(idx, n);

    if (out->distinct == 1)                out->flags |= WC_F_SAME;
    if (out->period)                       out->flags |= WC_F_PERIOD;
    if (out->near >= WC_NEAR_MIN(n))       out->flags |= WC_F_CLUSTER;
    if (out->sorted && out->distinct > 1)  out->flags |= WC_F_SORTED;
    if (out->dups >= WC_DUP_MIN(n))        out->flags |= WC_F_DUP;

    // Precedence is severity, and severity is how much is provably gone. SAME
    // and PERIOD are everything. CLUSTER is next because a draw off a narrow
    // slice of the list can be down to about 11 bits, where SORTED is a known
    // and bounded loss of log2(n!), and DUP is the weakest of the three: a
    // draw WITH replacement is allowed to repeat, and does.
    out->verdict = (out->flags & WC_F_SAME)    ? WC_Q_SAME
                 : (out->flags & WC_F_PERIOD)  ? WC_Q_PERIOD
                 : (out->flags & WC_F_CLUSTER) ? WC_Q_CLUSTER
                 : (out->flags & WC_F_SORTED)  ? WC_Q_SORTED
                 : (out->flags & WC_F_DUP)     ? WC_Q_DUP
                 : WC_Q_OK;
}
