// Desktop tests for main/kiss_gword.c -- a word the owner writes, matched by
// shape.
//
// The asymmetry that shapes this suite is the opposite of test_duress.c's. A
// modifier recognised when nobody drew one puts a passphrase keyboard on screen
// in front of whoever is standing over the device, so there the false POSITIVE
// is the dangerous one. Here the false NEGATIVE is: a word the owner really
// wrote and the device refuses is an owner locked out of their own signer, with
// nothing on the device to appeal to. So the positives below are the ones drawn
// sloppily on purpose, and the negatives are only asked to reject things that
// are plainly not the same word.
#include <stdio.h>
#include <string.h>

#include "kiss_gword.h"

static int gfails;

static void gchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); gfails++; }
}

#define MAXP 512
static int g_xs[MAXP], g_ys[MAXP];
static uint8_t g_sid[MAXP];
static int g_n, g_stroke;

static void w_start(void) { g_n = 0; g_stroke = 0; }
static void w_lift(void)  { g_stroke++; }

// Straight segment from the current end, sampled the way the panel does: the
// collector keeps a point only once the finger moved 10px, so ~12px steps are
// what the recogniser actually receives.
static void w_to(int x, int y) {
    int fx = g_n ? g_xs[g_n - 1] : x, fy = g_n ? g_ys[g_n - 1] : y;
    if (!g_n) { g_xs[g_n] = x; g_ys[g_n] = y; g_sid[g_n] = (uint8_t)g_stroke; g_n++; return; }
    int dx = x - fx, dy = y - fy;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady) / 12;
    if (steps < 1) steps = 1;
    for (int i = 1; i <= steps && g_n < MAXP; i++) {
        g_xs[g_n] = fx + dx * i / steps;
        g_ys[g_n] = fy + dy * i / steps;
        g_sid[g_n] = (uint8_t)g_stroke;
        g_n++;
    }
}

static int w_make(gw_template_t *t) {
    return gw_make(g_xs, g_ys, g_sid, g_n, t);
}

// A four stroke word, parameterised so the same hand can write it again a
// little bigger, a little lower and a little wobbly. dx/dy shift it, `s` scales
// it as a percentage, `j` is per-vertex jitter.
static void write_word(int dx, int dy, int s, int j) {
    int k = 0;
    // k++ and k were read in two arguments of the same call, so which jitter
    // each vertex got depended on the compiler's argument order -- undefined
    // behaviour, in the helper that decides whether two drawings of a word
    // are "the same word". Step k once, on its own line, then use it twice.
    #define P(X, Y) do { int k_ = k++; \
        w_to(dx + (X) * s / 100 + ((k_ % 3) - 1) * j, \
             dy + (Y) * s / 100 + (((k_ + 1) % 3) - 1) * j); } while (0)
    w_start();
    P(140, 120); P(140, 300); w_lift();                 // K spine
    P(140, 210); P(215, 120); w_lift();                 // K upper arm
    P(140, 210); P(215, 300); w_lift();                 // K lower arm
    P(285, 130); P(285, 300);                           // I
    #undef P
}

// A visibly different four stroke word in the same box: three verticals and a
// bar, nothing like the shape above.
static void write_other(void) {
    w_start();
    w_to(140, 120); w_to(140, 300); w_lift();
    w_to(210, 120); w_to(210, 300); w_lift();
    w_to(280, 120); w_to(280, 300); w_lift();
    w_to(140, 310); w_to(280, 310);
}

int test_gword(void) {
    gw_template_t a, b, c;

    // ---- the same word, written again by the same hand ----
    write_word(0, 0, 100, 0);
    gchk("a written word makes a template", w_make(&a) == 0);

    write_word(0, 0, 100, 6);                       // same place, shaky
    gchk("shaky second writing makes one", w_make(&b) == 0);
    gchk("a shaky rewrite still matches", gw_matches(&a, &b));

    // Bigger and somewhere else entirely. This is the whole reason the template
    // is normalised: nobody writes twice in the same square inch.
    write_word(220, 60, 150, 4);
    gchk("bigger, and elsewhere on the panel", w_make(&b) == 0 && gw_matches(&a, &b));

    // Smaller, and up in the corner.
    write_word(-60, -40, 70, 3);
    gchk("smaller, and in a corner", w_make(&b) == 0 && gw_matches(&a, &b));

    // ---- words that are not that word ----
    write_other();
    gchk("a different word does not match", w_make(&b) == 0 && !gw_matches(&a, &b));

    // Upside down. Rotation is deliberately NOT normalised away: someone
    // holding the device the wrong way round is not the owner.
    w_start();
    w_to(285, 300); w_to(285, 130); w_lift();
    w_to(215, 300); w_to(140, 210); w_lift();
    w_to(215, 120); w_to(140, 210); w_lift();
    w_to(140, 300); w_to(140, 120);
    gchk("the same word upside down does not match",
         w_make(&b) == 0 && !gw_matches(&a, &b));

    // The same ink with a different number of pen lifts. Lifts are the one
    // feature a shaky hand does not change, so this is a different word however
    // similar the outline.
    w_start();
    w_to(140, 120); w_to(140, 300);
    w_to(140, 210); w_to(215, 120);
    w_to(140, 210); w_to(215, 300);
    w_to(285, 130); w_to(285, 300);
    gchk("one continuous stroke is not the four stroke word",
         w_make(&b) == 0 && !gw_matches(&a, &b));
    gchk("different lift counts are not comparable at all",
         gw_distance(&a, &b) == INT16_MAX);

    // ---- draws that are not words at all ----
    w_start(); w_to(400, 240); w_to(410, 246);
    gchk("a tap makes no template", w_make(&b) < 0);

    w_start(); w_to(400, 240); w_to(460, 250);
    gchk("a 60px flick is under the floor", w_make(&b) < 0);

    gchk("no points, no template", gw_make(NULL, NULL, NULL, 0, &b) < 0);

    // An unset template is not a wildcard. This is the shape of the bug that
    // would open every device with no word set: distance against nothing has to
    // be uncomparable, never zero.
    memset(&c, 0, sizeof c);
    gchk("an unset template matches nothing",
         gw_distance(&a, &c) == INT16_MAX && !gw_matches(&a, &c));
    gchk("two unset templates do not match each other", !gw_matches(&c, &c));

    // The numbers behind the threshold, printed rather than asserted. A future
    // hand tuning GW_MATCH_MAX needs to see the margin, not just that a test
    // was green: the gap between the worst same-word distance and the best
    // different-word distance IS the safety, and a change that narrows it is a
    // change worth arguing about.
    {
        int same_worst = 0, diff_best = INT16_MAX;
        write_word(0, 0, 100, 6);   w_make(&b);
        if (gw_distance(&a, &b) > same_worst) same_worst = gw_distance(&a, &b);
        write_word(220, 60, 150, 4); w_make(&b);
        if (gw_distance(&a, &b) > same_worst) same_worst = gw_distance(&a, &b);
        write_word(-60, -40, 70, 3); w_make(&b);
        if (gw_distance(&a, &b) > same_worst) same_worst = gw_distance(&a, &b);
        write_other(); w_make(&b);
        if (gw_distance(&a, &b) < diff_best) diff_best = gw_distance(&a, &b);
        printf("      gword: same word worst %d, other word best %d, threshold %d\n",
               same_worst, diff_best, GW_MATCH_MAX);
        gchk("the threshold sits between them",
             same_worst <= GW_MATCH_MAX && diff_best > GW_MATCH_MAX);
    }

    // ---- enrolment and unlock must read one draw the same way ----
    //
    // The two collectors owned their own limits and disagreed. Enrolment took
    // 512 points and folded every stroke past its twelfth INTO the twelfth;
    // unlock took 384 and kept every boundary. `strokes` is matched exactly, so
    // a word of thirteen strokes could be written twice, confirmed, saved, and
    // then never open the device again -- with no error anywhere, because both
    // halves believed they had done their job. Both numbers live in
    // kiss_gword.h now, and the excess is DROPPED rather than folded.
    {
        const int EXTRA = 2;                       // a 14 stroke draw
        gw_template_t enrolled, seen, merged;

        // What kiss_word_ui stores: the strokes past the budget never happen.
        w_start();
        for (int s = 0; s < GW_MAX_STROKES + EXTRA; s++) {
            if (s) w_lift();
            if (s >= GW_MAX_STROKES) continue;     // dropped whole, points and all
            w_to(120 + s * 40, 130);
            w_to(120 + s * 40, 300);
        }
        gchk("an over-long draw still makes a template", w_make(&enrolled) == 0);
        gchk("enrolment stores exactly the stroke budget",
             enrolled.strokes == GW_MAX_STROKES);

        // What main.c's written_word_match sees: the whole draw arrives and the
        // word is every point whose stroke id is below the stored count.
        w_start();
        for (int s = 0; s < GW_MAX_STROKES + EXTRA; s++) {
            if (s) w_lift();
            w_to(120 + s * 40, 130);
            w_to(120 + s * 40, 300);
        }
        {
            int xs[MAXP], ys[MAXP]; int n = 0; uint8_t sid[MAXP];
            for (int i = 0; i < g_n; i++)
                if (g_sid[i] < enrolled.strokes) {
                    xs[n] = g_xs[i]; ys[n] = g_ys[i]; sid[n] = g_sid[i]; n++;
                }
            gchk("unlock rebuilds the word from the stored strokes",
                 gw_make(xs, ys, sid, n, &seen) == 0);
        }
        gchk("what was enrolled is what unlock reads",
             gw_matches(&enrolled, &seen));

        // And the shape that shipped, kept as the thing this test is for: the
        // excess folded INTO the twelfth stroke, so the stored template carries
        // points unlock will never hand it.
        //
        // The thirteenth and fourteenth strokes here are an underline, not two
        // more verticals. That is the point rather than a convenience: with the
        // excess drawn like the rest of the word, the folded template still
        // matched, which is precisely why this shipped and why no gate found it.
        // The failure needs the tail to be SHAPED differently from the body --
        // an underline, a crossbar, a flourish -- and that describes most of the
        // words anyone would choose.
        w_start();
        for (int s = 0; s < GW_MAX_STROKES; s++) {
            if (s) w_lift();
            w_to(120 + s * 40, 130);
            w_to(120 + s * 40, 300);
        }
        w_to(120, 390); w_to(560, 390);            // folded in, never lifted
        gchk("the folded draw counts the same strokes",
             w_make(&merged) == 0 && merged.strokes == GW_MAX_STROKES);
        printf("      gword: folded vs unlock distance %d, threshold %d\n",
               gw_distance(&merged, &seen), GW_MATCH_MAX);
        gchk("folding the excess is what unlock could never reproduce",
             !gw_matches(&merged, &seen));
    }

    // ---- storage ----
    gw_stored_set(NULL);
    gchk("nothing stored by default", !gw_stored_any());
    gchk("storing a word", gw_stored_set(&a) == 0 && gw_stored_any());
    gchk("it reads back identical",
         gw_stored_get(&b) && memcmp(&a, &b, sizeof a) == 0);
    gchk("an unset template cannot be stored", gw_stored_set(&c) < 0);
    gchk("clearing puts KISS back",
         gw_stored_set(NULL) == 0 && !gw_stored_any());
    gchk("a cleared slot fills nothing in", !gw_stored_get(&b));

    return gfails;
}
