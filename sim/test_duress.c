// Desktop tests for main/wallet_duress.c -- the duress unlock modifier.
//
// This suite exists because of one asymmetry. A modifier that fails to be
// recognized is an annoyance: you draw it again. A modifier recognized when
// the owner did NOT draw one takes someone who was opening their decoy under
// coercion and puts a passphrase keyboard on the screen in front of whoever is
// standing over them. So the negatives below matter more than the positives,
// and they are drawn from the shapes this device actually sees: the letter
// strokes of KISS itself, taps, and the flat swipes people make at a panel.
#include <stdio.h>
#include <string.h>

#include "wallet_duress.h"

static int dfails;

static void dchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); dfails++; }
}

// The KISS the sim walk draws (sim_main.c): x 140..560, y 120..290.
#define BX0 140
#define BY0 120
#define BX1 560
#define BY1 290

// Room for a circle drawn right round the word: ~1200px of ink at the touch
// layer's 10px decimation. Sized from the ink, like GEST_MAX in main.c.
#define MAXP 256
static int g_xs[MAXP], g_ys[MAXP], g_n;

static void stroke_start(void) { g_n = 0; }

// Straight segment from the current end to (x,y), sampled like a finger does
// (the touch layer decimates to >=10px moves, so ~12px steps are realistic).
static void stroke_to(int x, int y) {
    int fx = g_n ? g_xs[g_n - 1] : x, fy = g_n ? g_ys[g_n - 1] : y;
    if (!g_n) { g_xs[g_n] = x; g_ys[g_n] = y; g_n++; return; }
    int dx = x - fx, dy = y - fy;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady) / 12;
    if (steps < 1) steps = 1;
    for (int i = 1; i <= steps && g_n < MAXP; i++) {
        g_xs[g_n] = fx + dx * i / steps;
        g_ys[g_n] = fy + dy * i / steps;
        g_n++;
    }
}

static int classify(void) {
    return wallet_duress_classify(g_xs, g_ys, g_n, BX0, BY0, BX1, BY1);
}

int test_duress(void) {
    // ---- the six modifiers, drawn where a hand would put them ----
    stroke_start(); stroke_to(150, 312); stroke_to(552, 318);
    dchk("underline below the word", classify() == WDG_UNDERLINE);

    stroke_start(); stroke_to(148, 100); stroke_to(556, 96);
    dchk("overline above the word", classify() == WDG_OVERLINE);

    stroke_start(); stroke_to(150, 203); stroke_to(550, 209);
    dchk("strike through the middle", classify() == WDG_STRIKE);

    stroke_start(); stroke_to(152, 292); stroke_to(548, 118);
    dchk("slash up across the word", classify() == WDG_SLASH);

    stroke_start(); stroke_to(548, 292); stroke_to(152, 118);
    dchk("slash drawn the other way", classify() == WDG_SLASH);

    stroke_start();
    stroke_to(132, 112); stroke_to(570, 108); stroke_to(566, 302);
    stroke_to(130, 300); stroke_to(133, 118);
    dchk("circle around the word", classify() == WDG_CIRCLE);

    // A BIG loop that closes loosely -- what a hand actually draws when it
    // circles a word quickly. The old closing test scaled with the loop, so the
    // bigger the circle the tighter it had to close, and a board reported
    // having to draw it small on purpose.
    stroke_start();
    stroke_to(120, 96); stroke_to(600, 92); stroke_to(596, 320);
    stroke_to(116, 316); stroke_to(124, 150);
    dchk("big loosely-closed circle still reads", classify() == WDG_CIRCLE);

    // ...but a loop left properly open is still not a circle
    stroke_start();
    stroke_to(132, 112); stroke_to(570, 108); stroke_to(566, 302); stroke_to(300, 300);
    dchk("three-quarter loop is still refused", classify() == WDG_NONE);

    stroke_start(); stroke_to(210, 196); stroke_to(258, 272); stroke_to(372, 146);
    dchk("check mark", classify() == WDG_CHECK);

    // ---- negatives: the shapes this panel sees every day ----
    stroke_start(); stroke_to(300, 200); stroke_to(304, 203);
    dchk("a tap is not a modifier", classify() == WDG_NONE);

    stroke_start(); stroke_to(250, 300); stroke_to(340, 304);
    dchk("short flat swipe is not an underline", classify() == WDG_NONE);

    // the K's spine, upper arm and lower arm, as the sim walk draws them
    stroke_start(); stroke_to(140, 120); stroke_to(140, 300);
    dchk("K spine is not a modifier", classify() == WDG_NONE);

    stroke_start(); stroke_to(140, 210); stroke_to(230, 132);
    dchk("K upper arm is not a slash", classify() == WDG_NONE);

    stroke_start(); stroke_to(140, 210); stroke_to(230, 300);
    dchk("K lower arm is not a slash", classify() == WDG_NONE);

    // an S: wanders in both axes, but nowhere near the width of the word
    stroke_start();
    stroke_to(420, 140); stroke_to(360, 152); stroke_to(345, 188);
    stroke_to(400, 212); stroke_to(422, 250); stroke_to(362, 286);
    dchk("an S letter is not a circle", classify() == WDG_NONE);

    // The bands deliberately leave gaps: a flat stroke that is neither clearly
    // high, middle nor low is REJECTED rather than guessed at. Guessing here is
    // exactly the failure that surfaces a passphrase prompt under duress.
    stroke_start(); stroke_to(150, 166); stroke_to(550, 168);
    dchk("flat stroke between bands is rejected", classify() == WDG_NONE);

    stroke_start(); stroke_to(150, 244); stroke_to(550, 246);
    dchk("flat stroke in the other gap is rejected", classify() == WDG_NONE);

    // a loop that does not close is not a circle
    stroke_start();
    stroke_to(132, 112); stroke_to(570, 108); stroke_to(566, 302); stroke_to(300, 300);
    dchk("open loop is not a circle", classify() == WDG_NONE);

    // degenerate input must never classify
    stroke_start(); stroke_to(300, 200);
    dchk("single point is not a modifier", classify() == WDG_NONE);
    dchk("empty stroke is not a modifier",
         wallet_duress_classify(g_xs, g_ys, 0, BX0, BY0, BX1, BY1) == WDG_NONE);
    dchk("degenerate bbox is refused",
         wallet_duress_classify(g_xs, g_ys, g_n, 100, 100, 100, 100) == WDG_NONE);

    // ---- configuration ----
    // One stroke, the owner's. Plain KISS opens the decoy and always will, so a
    // second configurable stroke for it would only be another way to reach
    // something already reachable with no stroke at all.
    wallet_duress_forget();
    dchk("unset", wallet_duress_real() == WDG_NONE);

    dchk("set", wallet_duress_set(WDG_UNDERLINE) == 0);
    dchk("reads back", wallet_duress_real() == WDG_UNDERLINE);

    dchk("out-of-range refused", wallet_duress_set(WDG_N) != 0);
    dchk("negative refused", wallet_duress_set(-1) != 0);
    dchk("refused writes changed nothing", wallet_duress_real() == WDG_UNDERLINE);

    {
        int ok = 1;
        for (int g = WDG_UNDERLINE; g < WDG_N; g++)
            if (wallet_duress_set(g) != 0 || wallet_duress_real() != g) ok = 0;
        dchk("every modifier is settable", ok);
    }

    dchk("turning it off is allowed", wallet_duress_set(WDG_NONE) == 0);
    dchk("off means unset", wallet_duress_real() == WDG_NONE);

    // every modifier has a name the picker can show
    {
        int named = 1;
        for (int g = WDG_UNDERLINE; g < WDG_N; g++)
            if (wallet_duress_label_key(g) < 0) named = 0;
        dchk("every modifier has a label", named);
        dchk("WDG_NONE has no label", wallet_duress_label_key(WDG_NONE) < 0);
    }

    // ---- routing policy (uniform: the stored stroke must not change it) ----
    dchk("no word at all opens nothing",
         wallet_duress_route(false, WDG_NONE) == WDR_NONE);
    dchk("a scribble with a stroke still opens nothing",
         wallet_duress_route(false, WDG_CIRCLE) == WDR_NONE);
    dchk("word alone opens the decoy",
         wallet_duress_route(true, WDG_NONE) == WDR_DECOY);
    {
        int all_real = 1;
        for (int g = WDG_NONE + 1; g < WDG_N; g++)
            if (wallet_duress_route(true, g) != WDR_REAL) all_real = 0;
        dchk("word plus any stroke reaches the passphrase", all_real);
    }

    // The property the whole change exists for: the answer must not depend on
    // what is stored. Before this, drawing the word once told an attacker
    // whether a stroke was configured, because the device either prompted or
    // did not.
    {
        int stable = 1;
        for (int cfg = WDG_NONE; cfg < WDG_N; cfg++) {
            wallet_duress_set(cfg);
            if (wallet_duress_route(true, WDG_NONE) != WDR_DECOY) stable = 0;
            if (wallet_duress_route(true, WDG_UNDERLINE) != WDR_REAL) stable = 0;
        }
        dchk("routing is identical whatever stroke is configured", stable);
    }
    wallet_duress_set(WDG_NONE);

    return dfails;
}
