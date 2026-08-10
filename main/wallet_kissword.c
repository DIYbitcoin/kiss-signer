// See wallet_kissword.h for why this is not in main.c any more.
#include "wallet_kissword.h"

// Recognise a "K": a left vertical spine, and anything reaching to the right.
// Deliberately LENIENT -- this is cover, not the lock. A plain tap or a flat
// swipe still will not match, which is all it has to refuse.
static bool kw_is_k(const int *xs, const int *ys, int n)
{
    if (n < 8) return false;
    int minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int w = maxx - minx, h = maxy - miny;
    if (w < 40 || h < 50) return false;      // needs a bit of size, easy to meet
    int spine_top = 0, spine_bot = 0;
    bool arm = false;
    // Integer comparisons against the bbox rather than float normalisation:
    // same decision, no libm, and it rounds identically on the device and on
    // the desktop runner. nx < 0.50 becomes 2*(x-minx) < w, and so on.
    for (int i = 0; i < n; i++) {
        int dx = xs[i] - minx, dy = ys[i] - miny;
        if (2 * dx < w) { if (2 * dy < h) spine_top++; else spine_bot++; }
        if (20 * dx > 9 * w) arm = true;     // nx > 0.45
    }
    return spine_top >= 1 && spine_bot >= 1 && arm;
}

bool kw_is_kiss(const int *xs, const int *ys, const uint8_t *sid,
                int n, int strokes)
{
    if (!xs || !ys || !sid) return false;
    // Deliberateness comes from PEN LIFTS, not x-gaps: writing K I S S means at
    // least four separate strokes (K may take two or three). Stroke count is
    // immune to fat-finger blur, so it can be strict where the x-clustering
    // below stays forgiving -- one wide stroke or a casual zigzag never fires.
    if (strokes < 4) return false;
    if (n < 10) return false;
    if (n > KW_MAX_PTS) n = KW_MAX_PTS;

    int minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int w = maxx - minx, h = maxy - miny;
    // LOOSENED, and the reason is worth writing down: this used to be the gate
    // in front of the passphrase keyboard, so a false positive was a tell and
    // strictness was cheap. It now opens the DECOY, so a fumbled shape costs
    // nothing at all -- someone lands in a spare wallet.
    if (w < 120 || h < 35) return false;     // still a word, just a smaller one
    if (w < h) return false;                 // wider than tall

    // Letter clusters along x. Letters are continuous in x; a 1-bin empty gap
    // marks a letter break. Bins are filled along same-stroke segments (not
    // just at touch samples): a fast stroke leaves >1-bin gaps between samples,
    // which used to split one letter into two phantom clusters.
    enum { KB = 40 };
    bool occ[KB];
    for (int b = 0; b < KB; b++) occ[b] = false;
    for (int i = 0; i < n; i++) {
        int b1 = (xs[i] - minx) * KB / (w + 1);
        occ[b1] = true;
        if (i > 0 && sid[i] == sid[i - 1]) {
            int b0 = (xs[i - 1] - minx) * KB / (w + 1);
            for (int b = b0 < b1 ? b0 : b1; b <= (b0 < b1 ? b1 : b0); b++)
                occ[b] = true;
        }
    }
    int clusters = 0, gap = 0, k_hi = 0;
    bool in = false;
    for (int b = 0; b < KB; b++) {
        if (occ[b]) {
            if (!in) { clusters++; in = true; }
            if (clusters == 1) k_hi = b;     // right edge of the leftmost letter
            gap = 0;
        } else if (in && ++gap >= 1) {
            in = false;
        }
    }
    int kcut = minx + (k_hi + 1) * (w + 1) / KB;   // isolate the leftmost letter

    // THE FIX. Clusters alone cannot tell KISS from KIS: a finger-drawn "SS"
    // usually merges into one blob in x, so this was loosened to 3 -- and 3 is
    // exactly what KIS produces. The comment here claimed four clusters were
    // required and the code asked for three, for months.
    //
    // The stroke count could not supply the missing strictness either, whatever
    // the old comment said: a K costs two or three lifts by this file's own
    // admission, so K+I+S already clears strokes >= 4.
    //
    // So count the letters AFTER the K by pen lift, which x-blur cannot touch.
    // KISS has three of them (I, S, S) however badly the S's merge; KIS has
    // two. Stroke ids are monotone within a draw, so one pass counts each
    // stroke once even though an S loops back and forth across the cut.
    int right_strokes = 0, last = -1;
    for (int i = 0; i < n; i++) {
        if (xs[i] <= kcut) continue;
        if ((int)sid[i] != last) { right_strokes++; last = (int)sid[i]; }
    }
    // Either reading is enough, because the two blur in opposite directions:
    // the S's merging costs a cluster and leaves the lifts intact, while the K
    // and I merging costs a right-hand lift and leaves the clusters intact.
    // Demanding both would refuse ordinary handwriting; demanding neither is
    // what let KIS through.
    if (clusters < 4 && right_strokes < 3) return false;

    // ...and the leftmost letter must be a (lenient) K.
    int lx[KW_MAX_PTS], ly[KW_MAX_PTS];
    int ln = 0;
    for (int i = 0; i < n && ln < KW_MAX_PTS; i++)
        if (xs[i] <= kcut) { lx[ln] = xs[i]; ly[ln] = ys[i]; ln++; }
    return kw_is_k(lx, ly, ln);
}
