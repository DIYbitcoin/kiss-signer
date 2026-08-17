// Desktop tests for main/kiss_duress.c -- the duress unlock modifier.
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

#include "kiss_duress.h"

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
    return kiss_duress_classify(g_xs, g_ys, g_n, BX0, BY0, BX1, BY1);
}

// The same ink, with no box to measure it against.
static int freeclassify(void) {
    return kiss_duress_classify_free(g_xs, g_ys, g_n);
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
         kiss_duress_classify(g_xs, g_ys, 0, BX0, BY0, BX1, BY1) == WDG_NONE);
    dchk("degenerate bbox is refused",
         kiss_duress_classify(g_xs, g_ys, g_n, 100, 100, 100, 100) == WDG_NONE);

    // ---- configuration ----
    // One stroke, the owner's. Plain KISS opens the decoy and always will, so a
    // second configurable stroke for it would only be another way to reach
    // something already reachable with no stroke at all.
    kiss_duress_forget();
    dchk("unset", kiss_duress_real() == WDG_NONE);

    dchk("set", kiss_duress_set(WDG_UNDERLINE) == 0);
    dchk("reads back", kiss_duress_real() == WDG_UNDERLINE);

    dchk("out-of-range refused", kiss_duress_set(WDG_N) != 0);
    dchk("negative refused", kiss_duress_set(-1) != 0);
    dchk("refused writes changed nothing", kiss_duress_real() == WDG_UNDERLINE);

    {
        int ok = 1;
        for (int g = WDG_UNDERLINE; g < WDG_N; g++)
            if (kiss_duress_set(g) != 0 || kiss_duress_real() != g) ok = 0;
        dchk("every modifier is settable", ok);
    }

    dchk("turning it off is allowed", kiss_duress_set(WDG_NONE) == 0);
    dchk("off means unset", kiss_duress_real() == WDG_NONE);

    // every modifier has a name the picker can show
    {
        int named = 1;
        for (int g = WDG_UNDERLINE; g < WDG_N; g++)
            if (kiss_duress_label_key(g) < 0) named = 0;
        dchk("every modifier has a label", named);
        dchk("WDG_NONE has no label", kiss_duress_label_key(WDG_NONE) < 0);
    }

    // ---- routing policy (the configured stroke decides; the bare word never) --
    dchk("no word at all opens nothing",
         kiss_duress_route(false, WDG_NONE) == WDR_NONE);
    dchk("a scribble with a stroke still opens nothing",
         kiss_duress_route(false, WDG_CIRCLE) == WDR_NONE);

    // Unconfigured: the rule every device shipped with. An owner who never
    // opened the picker keeps the way in they already had, so enabling this
    // feature cannot shut anybody out of their own keys on upgrade.
    kiss_duress_set(WDG_NONE);
    dchk("unset: word alone opens the decoy",
         kiss_duress_route(true, WDG_NONE) == WDR_DECOY);
    {
        int all_real = 1;
        for (int g = WDG_NONE + 1; g < WDG_N; g++)
            if (kiss_duress_route(true, g) != WDR_REAL) all_real = 0;
        dchk("unset: word plus any stroke reaches the passphrase", all_real);
    }

    // Configured: only the owner's stroke, and every other one lands exactly
    // where no stroke lands. That indistinguishability IS the feature -- a
    // prober who guesses wrong gets a working, funded signer and no sign that
    // they guessed at all.
    {
        int exact = 1, wrong_is_decoy = 1;
        for (int cfg = WDG_NONE + 1; cfg < WDG_N; cfg++) {
            kiss_duress_set(cfg);
            if (kiss_duress_route(true, cfg) != WDR_REAL) exact = 0;
            for (int g = WDG_NONE + 1; g < WDG_N; g++)
                if (g != cfg && kiss_duress_route(true, g) != WDR_DECOY)
                    wrong_is_decoy = 0;
        }
        dchk("set: the chosen stroke reaches the passphrase", exact);
        dchk("set: every other stroke opens the decoy", wrong_is_decoy);
    }

    // The leak the uniform rule was introduced to close, kept closed: it lived
    // on the BARE WORD, where a configured device opened the decoy and an
    // unconfigured one drew a passphrase keyboard. One gesture said which kind
    // of device this was. The bare word now answers DECOY in every
    // configuration, which is what makes it safe for the stroke to decide.
    {
        int bare_stable = 1;
        for (int cfg = WDG_NONE; cfg < WDG_N; cfg++) {
            kiss_duress_set(cfg);
            if (kiss_duress_route(true, WDG_NONE) != WDR_DECOY) bare_stable = 0;
        }
        dchk("the bare word opens the decoy whatever is configured", bare_stable);
    }

    // A custom drawing's final mark is a FREE mark with no word box behind it,
    // so it has no WDG_* identity to compare. That path stays any-mark on
    // purpose, and must not start depending on the stroke: it used to hand
    // kiss_duress_route a fabricated WDG_STRIKE, which now would mean "works
    // only for owners who happened to pick line-through".
    {
        int marked_ok = 1;
        for (int cfg = WDG_NONE; cfg < WDG_N; cfg++) {
            kiss_duress_set(cfg);
            if (kiss_duress_route_marked(true, true) != WDR_REAL) marked_ok = 0;
            if (kiss_duress_route_marked(true, false) != WDR_DECOY) marked_ok = 0;
            if (kiss_duress_route_marked(false, true) != WDR_NONE) marked_ok = 0;
        }
        dchk("a custom drawing routes on any mark, whatever stroke is set",
             marked_ok);
    }
    kiss_duress_set(WDG_NONE);

    // ---- free marks: the same shapes with no word under them --------------
    //
    // A custom way in is a sequence of these, so they are drawn ANYWHERE on
    // the game screen rather than over a word. Coordinates below are picked
    // well away from the KISS box on purpose: a free mark that only classifies
    // where the word used to be is a mark that works in the wizard and fails
    // on the panel.
    {
        stroke_start(); stroke_to(600, 400); stroke_to(760, 404);
        dchk("free: flat stroke is a line", freeclassify() == WDF_LINE);

        // The three framed ids collapse here, and that is the design. Draw the
        // same stroke high, middle and low: all three must answer LINE, because
        // "above" and "below" are properties of a word that is not there.
        int collapsed = 1;
        stroke_start(); stroke_to(200,  60); stroke_to(400,  62);
        if (freeclassify() != WDF_LINE) collapsed = 0;
        stroke_start(); stroke_to(200, 240); stroke_to(400, 242);
        if (freeclassify() != WDF_LINE) collapsed = 0;
        stroke_start(); stroke_to(200, 440); stroke_to(400, 442);
        if (freeclassify() != WDF_LINE) collapsed = 0;
        dchk("free: height on screen changes nothing", collapsed);

        stroke_start(); stroke_to(180, 120); stroke_to(360, 300);
        dchk("free: diagonal down-right", freeclassify() == WDF_SLASH);
        stroke_start(); stroke_to(360, 120); stroke_to(180, 300);
        dchk("free: diagonal down-left", freeclassify() == WDF_SLASH);

        stroke_start();
        stroke_to(300, 140); stroke_to(420, 200); stroke_to(420, 300);
        stroke_to(300, 360); stroke_to(180, 300); stroke_to(180, 200);
        stroke_to(298, 143);
        dchk("free: a loop is a circle", freeclassify() == WDF_CIRCLE);

        stroke_start();
        stroke_to(300, 200); stroke_to(340, 280); stroke_to(430, 130);
        dchk("free: a tick", freeclassify() == WDF_CHECK);

        // ---- the negatives, which are the ones that matter ----
        //
        // A free mark has no word to be measured against, so the ONLY thing
        // standing between an idle finger and a valid mark is the size floor
        // and the refusal to guess. Every one of these is something a hand
        // does on a game screen without meaning anything by it.
        stroke_start(); stroke_to(400, 240); stroke_to(404, 243);
        dchk("free: a tap is nothing", freeclassify() == WDF_NONE);

        stroke_start(); stroke_to(400, 240); stroke_to(460, 246);
        dchk("free: a 60px nudge is under the floor",
             freeclassify() == WDF_NONE);

        stroke_start(); stroke_to(400, 120); stroke_to(404, 300);
        dchk("free: a vertical bar is not a shape", freeclassify() == WDF_NONE);

        // Three sides of a loop the finger abandoned. Wide, tall, and NOT
        // closed -- the framed classifier learned this one the hard way and
        // the free one inherits the lesson rather than the bug.
        stroke_start();
        stroke_to(300, 140); stroke_to(420, 200); stroke_to(420, 300);
        stroke_to(300, 360);
        dchk("free: an abandoned loop is nothing", freeclassify() == WDF_NONE);

        // The K of KISS, stroke by stroke. The game screen is where people
        // draw this word, so its own strokes are the most likely accidental
        // input a sequence reader will ever see.
        stroke_start(); stroke_to(140, 120); stroke_to(140, 300);
        dchk("free: the K spine is nothing", freeclassify() == WDF_NONE);

        // A slice across the fruit: fast, flat, and exactly what this screen is
        // for. It IS a line, and that is correct and safe -- one mark is not a
        // way in, four in the right order is, and the game swallows strokes
        // that are not building a sequence.
        stroke_start(); stroke_to(120, 260); stroke_to(700, 250);
        dchk("free: a fruit slice reads as a line", freeclassify() == WDF_LINE);

        // Every shape has a name to put on a pill, and no shape shares one.
        int named = 1, distinct = 1;
        for (int m = WDF_NONE + 1; m < WDF_N; m++) {
            if (kiss_duress_free_label_key(m) < 0) named = 0;
            for (int o = m + 1; o < WDF_N; o++)
                if (kiss_duress_free_label_key(m) ==
                    kiss_duress_free_label_key(o)) distinct = 0;
        }
        dchk("free: every mark has a distinct label", named && distinct);
        dchk("free: WDF_NONE has no label",
             kiss_duress_free_label_key(WDF_NONE) < 0);
    }

    // ---- the word ---------------------------------------------------------
    //
    // An owner's own way in, replacing KISS. The matcher is a PREFIX test, so
    // the mark after the word is left for the caller exactly as one stroke
    // after KISS is today, and kiss_duress_route still decides the door.
    {
        const uint8_t w[4]  = { WDF_LINE, WDF_SLASH, WDF_CIRCLE, WDF_CHECK };
        const uint8_t alt[4]= { WDF_LINE, WDF_SLASH, WDF_CIRCLE, WDF_CIRCLE };
        uint8_t got[WDW_MAX];

        kiss_duress_forget();
        dchk("word: none set by default", kiss_duress_word_len() == 0);
        // Unset is NOT an empty word that everything begins with. It means
        // KISS still stands, and the caller has to be sent to detect_cover_word.
        dchk("word: unset matches nothing", kiss_duress_word_match(w, 4) == 0);

        dchk("word: set", kiss_duress_word_set(w, 4) == 0);
        dchk("word: length reads back", kiss_duress_word_len() == 4);
        dchk("word: marks read back",
             kiss_duress_word_get(got) == 4 && memcmp(got, w, 4) == 0);

        dchk("word: exact run matches", kiss_duress_word_match(w, 4) == 4);
        dchk("word: one mark wrong does not",
             kiss_duress_word_match(alt, 4) == 0);
        dchk("word: a short run does not",
             kiss_duress_word_match(w, 3) == 0);

        // The whole point of a prefix: the word, then the mark that picks the
        // door. Five marks in, four consumed, one left over for the caller.
        {
            uint8_t plus[5] = { WDF_LINE, WDF_SLASH, WDF_CIRCLE, WDF_CHECK,
                                WDF_SLASH };
            dchk("word: trailing mark is left for the caller",
                 kiss_duress_word_match(plus, 5) == 4);
        }
        // ...and a WRONG word with a trailing mark is still nothing. A mark
        // after a miss must not rescue it.
        {
            uint8_t bad[5] = { WDF_CHECK, WDF_SLASH, WDF_CIRCLE, WDF_CHECK,
                               WDF_SLASH };
            dchk("word: a trailing mark does not rescue a miss",
                 kiss_duress_word_match(bad, 5) == 0);
        }

        // Nothing invalid is storable. A word containing WDF_NONE would be a
        // word with a hole in it, and a stroke the reader refused would then
        // silently match it.
        {
            uint8_t hole[3] = { WDF_LINE, WDF_NONE, WDF_CIRCLE };
            uint8_t oob[2]  = { WDF_LINE, WDF_N };
            uint8_t big[WDW_MAX + 1];
            for (unsigned i = 0; i < sizeof big; i++) big[i] = WDF_LINE;
            dchk("word: a hole is refused", kiss_duress_word_set(hole, 3) < 0);
            dchk("word: an out of range mark is refused",
                 kiss_duress_word_set(oob, 2) < 0);
            dchk("word: longer than WDW_MAX is refused",
                 kiss_duress_word_set(big, WDW_MAX + 1) < 0);
            dchk("word: a refused set changes nothing",
                 kiss_duress_word_len() == 4);
        }

        dchk("word: clearing puts KISS back",
             kiss_duress_word_set(NULL, 0) == 0 &&
             kiss_duress_word_len() == 0);

        // A wipe must take the way in with it, or the next owner of an erased
        // device inherits a door into a seed that is gone. Same reasoning that
        // keeps the gesture out of kiss_seed.c's KEEP_KEYS.
        kiss_duress_word_set(w, 4);
        kiss_duress_set(WDG_CIRCLE);
        kiss_duress_forget();
        dchk("word: a wipe forgets the word",  kiss_duress_word_len() == 0);
        dchk("word: a wipe forgets the mark",
             kiss_duress_real() == WDG_NONE);
    }

    return dfails;
}
