// Desktop tests for main/kiss_coverword.c -- the cover word, matched by shape.
//
// This suite exists because of a bug it would have caught on day one. The
// recogniser asked for four pen lifts and THREE letter clusters while its own
// comment claimed four clusters, and a K costs two or three lifts -- so "KIS"
// cleared both gates and opened the decoy. It shipped that way, with every gate
// green, because detect_cover_word lived in main.c and main.c links into no test
// binary. Nothing on the host could draw a shape at it and ask what it said.
//
// The asymmetry here is the mirror of test_gword.c's. A false NEGATIVE costs an
// owner one more attempt at a word that is only cover anyway. A false POSITIVE
// is a stranger reaching the decoy with three letters, or -- worse -- the
// fourth S landing inside the 500ms arming window and being read as the duress
// MODIFIER, which surfaces a passphrase prompt in front of them. So the
// negatives below are the ones drawn carefully, and the positives are drawn
// badly on purpose.
#include <stdio.h>
#include <string.h>

#include "kiss_coverword.h"

static int kfails;

static void kchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); kfails++; }
}

#define MAXP 512
static int k_xs[MAXP], k_ys[MAXP];
static uint8_t k_sid[MAXP];
static int k_n, k_stroke;

static void s_start(void) { k_n = 0; k_stroke = 0; }
static void s_lift(void)  { k_stroke++; }

// Straight segment from the current end, sampled the way the panel does: the
// collector keeps a point only once the finger moved 10px, so ~12px steps are
// what the recogniser actually receives. Same helper shape as test_gword.c.
static void s_to(int x, int y) {
    int fx = k_n ? k_xs[k_n - 1] : x, fy = k_n ? k_ys[k_n - 1] : y;
    if (!k_n) { k_xs[0] = x; k_ys[0] = y; k_sid[0] = (uint8_t)k_stroke; k_n = 1; return; }
    int dx = x - fx, dy = y - fy;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady) / 12;
    if (steps < 1) steps = 1;
    for (int i = 1; i <= steps && k_n < MAXP; i++) {
        k_xs[k_n] = fx + dx * i / steps;
        k_ys[k_n] = fy + dy * i / steps;
        k_sid[k_n] = (uint8_t)k_stroke;
        k_n++;
    }
}

// Start a fresh stroke at a point without drawing into it from the last one.
static void s_move(int x, int y) {
    s_lift();
    if (k_n < MAXP) { k_xs[k_n] = x; k_ys[k_n] = y; k_sid[k_n] = (uint8_t)k_stroke; k_n++; }
}

static bool ask(void) { return cw_match(k_xs, k_ys, k_sid, k_n, k_stroke + 1); }

// ---- the letters, each starting its own stroke ----

// K in three strokes: spine, upper arm, lower arm. This is how the simulator
// draws it and how most people do.
static void letter_k3(int x) {
    s_move(x, 120);       s_to(x, 300);
    s_move(x, 210);       s_to(x + 90, 130);
    s_move(x, 210);       s_to(x + 90, 300);
}
// K in two strokes: spine, then one arm folded back over itself.
static void letter_k2(int x) {
    s_move(x, 120);       s_to(x, 300);
    s_move(x + 90, 130);  s_to(x, 210); s_to(x + 90, 300);
}
// K in one stroke, drawn without lifting.
static void letter_k1(int x) {
    s_move(x, 120); s_to(x, 300); s_to(x, 210); s_to(x + 90, 130);
    s_to(x, 210); s_to(x + 90, 300);
}
static void letter_i(int x) { s_move(x, 130); s_to(x, 290); }
static void letter_s(int x) {
    s_move(x + 78, 140); s_to(x + 18, 152); s_to(x + 3, 188);
    s_to(x + 58, 212);   s_to(x + 80, 250); s_to(x + 20, 286); s_to(x, 272);
}

int test_coverword(void) {
    printf("\n-- cover word --\n");

    // ---- THE REGRESSION. Three letters is not the word. ----
    s_start(); letter_k3(140); letter_i(285); letter_s(342);
    kchk("KIS, K in three strokes -> refused", !ask());

    s_start(); letter_k2(140); letter_i(285); letter_s(342);
    kchk("KIS, K in two strokes -> refused", !ask());

    s_start(); letter_k1(140); letter_i(285); letter_s(342);
    kchk("KIS, K in one stroke -> refused", !ask());

    // ---- the word itself, written three ways ----
    s_start(); letter_k1(140); letter_i(285); letter_s(342); letter_s(462);
    kchk("KISS, K in one stroke", ask());

    s_start(); letter_k2(140); letter_i(285); letter_s(342); letter_s(462);
    kchk("KISS, K in two strokes", ask());

    s_start(); letter_k3(140); letter_i(285); letter_s(342); letter_s(462);
    kchk("KISS, K in three strokes", ask());

    // ---- drawn badly, which is the normal case on glass ----
    // The two S's overlapping in x: one blob, three clusters. This is the
    // device finding that loosened the cluster rule to 3 in the first place,
    // so it must still pass -- the pen lifts are what carry it now.
    s_start(); letter_k3(140); letter_i(285); letter_s(342); letter_s(372);
    kchk("KISS with the S's merged in x", ask());

    // The K and I close together. This is the one direction the fix is NOT
    // free in, so the boundary is pinned rather than left to be discovered on
    // glass: the arms of the K end at x=230, and the I is accepted from a 14px
    // gap outward at this ~400px word width.
    s_start(); letter_k3(140); letter_i(244); letter_s(342); letter_s(462);
    kchk("KISS with the I close after the K (14px)", ask());

    // Closer than that and the two letters are one blob of ink AND one blob of
    // lifts -- the I falls left of the K cut, so there are two letters to the
    // right and three clusters, which is what KIS looks like. Refusing is the
    // honest answer: nothing in the drawing distinguishes them. It costs a
    // redraw of a COVER gesture, which is the cheap direction to be wrong in,
    // and the case the panel actually reported -- the two S's merging -- is
    // carried by the pen lifts above.
    s_start(); letter_k3(140); letter_i(236); letter_s(342); letter_s(462);
    kchk("KISS with the I touching the K -> refused, and that is the trade", !ask());

    // ---- things that are not a word ----
    s_start(); letter_k3(140);
    kchk("a K alone -> refused", !ask());

    s_start(); letter_k3(140); letter_i(285);
    kchk("KI -> refused", !ask());

    s_start(); s_move(140, 200); s_to(600, 210);
    kchk("one flat swipe -> refused", !ask());

    s_start(); s_move(300, 200); s_to(304, 204);
    kchk("a tap -> refused", !ask());

    // Six strokes of zigzag with no K on the left: strokes and width both
    // clear, so the K check is the only thing refusing it.
    s_start();
    for (int i = 0; i < 6; i++) { s_move(140 + i * 70, 150); s_to(200 + i * 70, 290); }
    kchk("a six stroke zigzag -> refused", !ask());

    // ---- the shape the simulator actually draws ----
    // sim/sim_main.c draw_cover_word() is the walk's idea of the word. If it
    // and the recogniser ever disagree, the walk proves nothing about the
    // unlock -- which is exactly how the KIS bug survived. Same coordinates.
    s_start();
    s_move(140, 120); for (int i = 1; i <= 9; i++) s_to(140, 120 + i * 20);
    s_move(140, 210); for (int i = 1; i <= 6; i++) s_to(140 + i * 15, 210 - i * 13);
    s_move(140, 210); for (int i = 1; i <= 6; i++) s_to(140 + i * 15, 210 + i * 15);
    s_move(285, 130); for (int i = 1; i <= 8; i++) s_to(285, 130 + i * 21);
    s_move(420, 140); s_to(360, 152); s_to(345, 188); s_to(400, 212);
    s_to(422, 250); s_to(362, 286); s_to(342, 272);
    s_move(540, 140); s_to(480, 152); s_to(465, 188); s_to(520, 212);
    s_to(542, 250); s_to(482, 286); s_to(462, 272);
    kchk("the exact stroke stream draw_cover_word() emits", ask());

    // And the same stream with the last S left off, which is what a hand that
    // pauses mid word produces.
    s_start();
    s_move(140, 120); for (int i = 1; i <= 9; i++) s_to(140, 120 + i * 20);
    s_move(140, 210); for (int i = 1; i <= 6; i++) s_to(140 + i * 15, 210 - i * 13);
    s_move(140, 210); for (int i = 1; i <= 6; i++) s_to(140 + i * 15, 210 + i * 15);
    s_move(285, 130); for (int i = 1; i <= 8; i++) s_to(285, 130 + i * 21);
    s_move(420, 140); s_to(360, 152); s_to(345, 188); s_to(400, 212);
    s_to(422, 250); s_to(362, 286); s_to(342, 272);
    kchk("...minus the last S -> refused", !ask());

    // ---- the two tap way in ----
    // Same asymmetry as the word above, and stronger. A false negative costs a
    // tester one more tap. A false positive is the corner opening on a SINGLE
    // tap, which on a device that has been switched to mainnet and back is a
    // way in nobody drew.
    cw_quick_reset();
    kchk("corner box", cw_quick_zone(0, 0) && cw_quick_zone(119, 119));
    kchk("...and what is outside it",
         !cw_quick_zone(120, 60) && !cw_quick_zone(60, 120) && !cw_quick_zone(-1, 0));

    kchk("one tap alone opens nothing", !cw_quick_tap(40, 40, 1000));
    kchk("the second closes the pair", cw_quick_tap(40, 40, 1400));
    kchk("a third does not re-fire it", !cw_quick_tap(40, 40, 1500));
    kchk("...but a fourth pairs with the third", cw_quick_tap(40, 40, 1900));

    // Late: the second tap becomes the first of a fresh pair rather than
    // nothing, so the tap AFTER it must not be the one that opens the signer.
    cw_quick_reset();
    kchk("arm", !cw_quick_tap(40, 40, 5000));
    kchk("900ms later -> too late", !cw_quick_tap(40, 40, 5900));
    kchk("...and it re-armed rather than pairing", cw_quick_tap(40, 40, 6100));

    // Exactly on the window is in; one past it is not.
    cw_quick_reset();
    (void)cw_quick_tap(40, 40, 100);
    kchk("the window is inclusive", cw_quick_tap(40, 40, 100 + CW_QT_MS));
    cw_quick_reset();
    (void)cw_quick_tap(40, 40, 100);
    kchk("one ms past it is not", !cw_quick_tap(40, 40, 101 + CW_QT_MS));

    // A tap outside the corner disarms: the pair is two taps in the SAME place,
    // not two taps of which one happened to be in the corner.
    cw_quick_reset();
    (void)cw_quick_tap(40, 40, 200);
    kchk("a tap elsewhere is not a pair", !cw_quick_tap(400, 240, 300));
    kchk("...and it disarmed the first", !cw_quick_tap(40, 40, 400));

    // The lock clears it. Without this a tap left armed before a lock pairs
    // with the first tap of the next session.
    cw_quick_reset();
    (void)cw_quick_tap(40, 40, 700);
    cw_quick_reset();
    kchk("the lock disarms", !cw_quick_tap(40, 40, 800));

    // The tick wraps at ~49 days. Unsigned subtraction, so the wrap is neither
    // a way in (a huge elapsed reading) nor a way out.
    cw_quick_reset();
    (void)cw_quick_tap(40, 40, 0xFFFFFF00u);
    kchk("across the tick wrap", cw_quick_tap(40, 40, 0x00000100u));

    return kfails;
}
