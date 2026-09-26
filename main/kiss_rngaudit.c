// RANDOMNESS AUDIT screens. Two pages: an intro that teaches what a spread
// score can and cannot say, then the live histogram. See kiss_rngaudit.h for
// what is claimed and kiss_crypto.h for why provenance outranks every
// statistic on this board.
#include "kiss_rngaudit.h"
#include "kiss_rngq.h"
#include "kiss_crypto.h"
#include "kiss_theme.h"
#include "i18n.h"
#include <stdio.h>

#define MUT_COL  WT_MUT
#define OK_COL   WT_OK
#define WARN_COL WT_WARN

// The histogram card fills the 704 lane: 100 bars on a 7px pitch is 699, so
// 2px in-card margin on the left and 3 on the right. The bar lane leaves the
// card 12px of head and foot; full height is TWICE the fair share, the dice
// bars' rule, so the fair line crosses at half and a level skyline against it
// says "noise did this" with no words in any locale.
#define HIST_X   SX(48)
#define HIST_Y   SY(96)
#define HIST_W   SX(704)
#define BAR_W    SX(6)
#define BAR_PITCH SX(7)
#define BAR_TOP  SY(12)
#if KISS_NARROW
// The 3.5in card is 118 tall, not the scaled 133: the stat row under it is 57
// and has to end above the band, where the scaled card left it 57 past it. The
// skyline is 399 of a 422 card, so it sits centred rather than 2px off the left
// edge with 21 empty on the right.
#define HIST_H   118
#define BAR_X0   ((HIST_W - (99 * BAR_PITCH + BAR_W)) / 2)
#define BAR_H    (HIST_H - 2 * BAR_TOP)
#else
#define HIST_H   SY(200)
#define BAR_X0   2
#define BAR_H    SY(176)
#endif

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void    (*s_done)(void);
static lv_timer_t *s_tmr;
static kiss_rngq_t s_q;
// ONE object, not a hundred and one.
//
// The skyline used to be an lv_obj per column plus one for the fair share
// line. That is 101 objects on a screen, and this screen turned out to hold
// the whole device's LVGL peak: 108KB of a 126KB pool, 26KB clear of the next
// worst, with the pool at 57% fragmentation. Past that edge LVGL does not
// report anything -- lv_obj_create hands back NULL and the next create
// segfaults, or lv_refr_now spins forever -- so the headroom every other
// screen has was being spent here, on rectangles.
//
// A hundred rectangles is what a draw callback is for. The tree already
// draws rather than builds in one place -- the unlock panel strokes the
// owner's word with lv_line and a point array (kiss_word_ui.c) -- and that
// widget is the wrong one here: it strokes a polyline, and this is a hundred
// FILLED columns with gaps between them. Traced as an outline it would be a
// different picture, and the picture is the lesson.
//
// So: a draw callback, confined to this one widget. Everything else on the
// screen is still the kit.
static lv_obj_t *s_hist;
#if KISS_NARROW
// The skyline's lane on the 3.5in. A result row that a long caption wraps taller
// takes its pixels from the card rather than from the band, and the bars and
// the fair line follow the card down with it.
static int s_bar_lane = BAR_H;
#define BAR_LANE s_bar_lane
#else
#define BAR_LANE BAR_H
#endif
static lv_obj_t *s_cnt;    // the running tally; the result widgets replace it
static lv_obj_t *s_note;   // the sub lane: carries the retry line on a miss
static lv_obj_t *s_exit;   // BACK while running, DONE once finished

static void intro_screen(void);
static void run_screen(void);

bool kiss_rngaudit_active(void) { return s_scr != NULL; }

static void wipe_widgets(void)
{
    s_cnt = s_note = s_exit = NULL;
    s_hist = NULL;
}

void kiss_rngaudit_close(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wipe_widgets();
}

static void exit_cb(lv_event_t *e)
{
    (void)e;
    kiss_rngaudit_close();
    if (s_done) s_done();
}

static void go_cb(lv_event_t *e)      { (void)e; run_screen(); }
static void run_back_cb(lv_event_t *e)
{
    (void)e;
    // Cancel mid run: the timer dies with the screen, nothing is lost and
    // nothing is kept -- a fresh START refills from zero.
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    intro_screen();
}

// ---- intro ----

static void intro_screen(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wipe_widgets();
    // No subtitle: the why blocks below carry the question the subtitle
    // asked, and the audit row that opens this page still wears it as a sub.
    s_scr = wt_screen(s_parent, tr(STR_W_RNG_T), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_W_AUD_T));
        wt_trail(s_scr, LV_SYMBOL_SHUFFLE, trail, false);
    }

    // The card clears the trail strip (70..100) with a gap of its own.
#if KISS_NARROW
    // 16 on the 3.5in: 24 put the third fact row through the band, and the
    // pixels the rows need come from above the card as well as below it.
    const int RNG_CARD_Y = WT_CHROME_STRIP_Y + WT_BR_H + 16;
#else
    const int RNG_CARD_Y = WT_CHROME_STRIP_Y + WT_BR_H + 24;
#endif

    bool live = kiss_trng_live();

    // The provenance row leads, because it answers the one question the bars
    // cannot. POWER, not a bolt: the fact stated is that the noise circuit is
    // switched on. Value in a status colour -- this is a state, not a theme.
    // font23 on the sub, stated rather than fitted. The ladder had one line
    // of a 96px row to work with and the copy was three, so it landed on
    // font14 -- "noise source text is very tiny". The copy is one line now.
    wt_row_x(s_scr, LV_SYMBOL_POWER, tr(STR_W_RNG_SRC), tr(STR_W_RNG_SRC_SUB),
             wt_font23(), live ? tr(STR_W_RNG_ON) : tr(STR_W_RNG_OFF),
             // ON and OFF are words, not a reading. The mono face on this page
             // belongs to the counter under the bars.
             wt_font23(), live ? OK_COL : WARN_COL, false,
             // NOT WT_CHOICE_Y(0). That constant is 96 and the trail strip
             // runs 70..100, so this card's top edge was drawn four pixels
             // INSIDE the row naming the page -- the chooser screens the
             // constant was borrowed from have no trail above them.
             WT_CHOICE_X, RNG_CARD_Y, WT_CHOICE_W, WT_CHOICE_H,
             NULL, NULL);

    // DECIDED: the pair read "SPREAD / 5000 numbers, 100 groups" and
    // "CANNOT PROVE / software passes it too", and came off the bench as
    // "what are you trying to say". Both were the middle of a sentence: one
    // named the method without saying what it measures, the other named a
    // limit without saying what a pass would have meant. Three rows say the
    // whole thing in order -- what runs, what a good result looks like, what
    // it still cannot tell you -- and the third is where "spread" is earned,
    // so SPREAD SCORE on the result page arrives with a meaning attached.
    //
    // The value lane is 438px at font28, about 25 characters, so none of
    // these can grow into a sentence: that is the shape doing its job, and
    // anything longer belongs in a paragraph, not a fact row.
    wt_fact_t facts[3] = {
        { .cap = tr(STR_W_RNG_WHY1_H), .val = tr(STR_W_RNG_WHY1_B),
          .icon = LV_SYMBOL_SHUFFLE },
        { .cap = tr(STR_W_RNG_FAIR_H), .val = tr(STR_W_RNG_FAIR_B),
          .icon = LV_SYMBOL_OK },
        // With no source the caution stops being a caveat and becomes the
        // refusal: there is nothing to score, so the caption changes too --
        // "THE LIMIT / nothing to audit" is two halves of different claims.
        { .cap = live ? tr(STR_W_RNG_WHY2_H) : tr(STR_W_RNG_OFF),
          .val = live ? tr(STR_W_RNG_WHY2_B) : tr(STR_W_RNG_NOSRC_B),
          .icon = LV_SYMBOL_WARNING, .icon_col = WT_WARN },
    };
    // Under the card with a gap that reads as one, rather than pinned at a
    // number chosen when the card sat higher.
#if KISS_NARROW
    // Hung from the band the way every explainer's rows are: resting on the
    // card, THE LIMIT ended 4 px under the floor on the 3.5in. The card's
    // bottom edge still wins if a taller row ever needs the room.
    {
        int fy = WT_CONTENT_BOTTOM - WT_FACT_BAND_GAP - wt_facts_height(facts, 3);
        const int under = RNG_CARD_Y + WT_CHOICE_H + SY(14);
        wt_facts(s_scr, fy > under ? fy : under, facts, 3);
    }
#else
    wt_facts(s_scr, RNG_CARD_Y + WT_CHOICE_H + SY(34), facts, 3);
#endif

    lv_obj_t *back = wt_arrow_action(s_scr, tr(STR_C_BACK), true, false,
                                     WT_BACK_X, WT_ACTION_Y, SX(140), true,
                                     exit_cb, NULL);
    lv_obj_set_ext_click_area(back, 10);
    // No START without a source. Absent, not greyed: a disabled control is a
    // shape this product does not draw.
    if (live)
        wt_arrow_action(s_scr, tr(STR_W_RNG_GO), false, true, WT_ACT_X,
                        WT_ACTION_Y, SX(240), false, go_cb, NULL);
}

// ---- the run ----

// One bar's height, the same arithmetic the objects used: live and honest at
// draw 300 and draw 5000 alike, because the scale is the CURRENT count
// against the CURRENT fair share. The skyline settles toward the line rather
// than climbing a fixed axis.
static int bar_h(int i)
{
    if (!s_q.n) return 0;
    int h = (int)((uint32_t)s_q.bin[i] * (BAR_LANE / 2) * s_q.bins / s_q.n);
    return h > BAR_LANE ? BAR_LANE : h;
}

// The skyline and the fair share line, in one pass over the widget's own
// area. Drawn in this order for the reason the objects were created in it:
// the line crosses OVER the fills, which is what makes a level skyline read
// as "noise did this" without a word in any locale.
static void hist_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = wt_accent();
    for (int i = 0; i < s_q.bins && i < 100; i++) {
        const int h = bar_h(i);
        if (h <= 0) continue;
        lv_area_t a;
        a.x1 = c.x1 + BAR_X0 + i * BAR_PITCH;
        a.x2 = a.x1 + BAR_W - 1;
        a.y2 = c.y1 + BAR_TOP + BAR_LANE - 1;
        a.y1 = a.y2 - h + 1;
        lv_draw_rect(layer, &d, &a);
    }

    d.bg_color = WT_DIV;
    lv_area_t t;
    t.x1 = c.x1;
    t.x2 = c.x2;
    t.y1 = c.y1 + BAR_TOP + BAR_LANE / 2;
    t.y2 = t.y1;
    lv_draw_rect(layer, &d, &t);
}

static void bars_set(void)
{
    if (s_hist) lv_obj_invalidate(s_hist);
}

static void finish(void)
{
    uint32_t milli = kiss_rngq_chi2_milli(&s_q);
    int v = kiss_rngq_verdict(&s_q, milli);

    if (s_cnt) { lv_obj_delete(s_cnt); s_cnt = NULL; }

    char val[16];
    snprintf(val, sizeof val, "%u.%03u", (unsigned)(milli / 1000),
             (unsigned)(milli % 1000));
#if KISS_NARROW
    // ONE row against the band on the 3.5in. The wide positions scaled put all
    // three blocks through the floor, SPREAD SCORE wrapped in its 114 px card,
    // and the two chips stacked 2 px apart. The chips take a column as wide as
    // the widest word either can wear, so nothing moves between a pass and a
    // miss; the range keeps the 192 its 17 characters need, and the score card
    // takes the rest. A locale whose caption would wrap in what is left (the
    // German chip column leaves it 71 px) gets two rows instead: the chips side
    // by side over the cards, and the skyline gives up the height.
    {
        const char *vs = v == RNGQ_PASS ? tr(STR_W_RNG_EVEN)
                       : v == RNGQ_LOW  ? tr(STR_W_RNG_TOOEVEN)
                                        : tr(STR_W_DICE_UNEVEN);
        const bool live = kiss_trng_live();
        const char *const words[] = {
            tr(STR_W_RNG_EVEN), tr(STR_W_RNG_TOOEVEN), tr(STR_W_DICE_UNEVEN),
            tr(STR_W_RNG_ON), tr(STR_W_RNG_OFF),
        };
        int colw = 0;
        for (int i = 0; i < 5; i++) {
            lv_point_t sz;
            lv_text_get_size(&sz, words[i], wt_font14(), 1, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            if (sz.x > colw) colw = sz.x;
        }
        colw += 2 * SX(10);                  // the chip's own side pads
        const int gap = 6, air = 8;
        lv_point_t cap;
        lv_text_get_size(&cap, tr(STR_W_RNG_CHI_CAP), wt_font14(), 1, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const bool one_row = SX(48) + cap.x + SX(32) + gap + SX(320) + gap + colw
                             <= SX(752);
        const int rng_x = one_row ? SX(752) - colw - gap - SX(320)
                                  : SX(752) - SX(320);
        lv_obj_t *chi = wt_value_card(s_scr, tr(STR_W_RNG_CHI_CAP), val, SX(48),
                                      0, rng_x - gap - SX(48), false);
        lv_obj_t *rng = wt_value_card(s_scr, tr(STR_W_RNG_RANGE_CAP),
                                      "61.137 .. 148.230", rng_x, 0, SX(320), false);
        lv_obj_t *chip = wt_state_chip(s_scr, vs, v == RNGQ_PASS ? OK_COL : WARN_COL);
        lv_obj_t *src = wt_state_chip(s_scr, live ? tr(STR_W_RNG_ON) : tr(STR_W_RNG_OFF),
                                      live ? OK_COL : WARN_COL);
        lv_obj_update_layout(s_scr);
        const int card_h = LV_MAX(lv_obj_get_height(chi), lv_obj_get_height(rng));
        const int ch1 = lv_obj_get_height(chip), ch2 = lv_obj_get_height(src);
        int top;
        if (one_row) {
            const int chips_h = ch1 + gap + ch2;
            const int row_h = LV_MAX(card_h, chips_h);
            const int row_y = WT_CONTENT_BOTTOM - air - row_h;
            lv_obj_set_y(chi, row_y);
            lv_obj_set_y(rng, row_y);
            const int cy = row_y + (row_h - chips_h) / 2;
            lv_obj_set_pos(chip, SX(752) - colw, cy);
            lv_obj_set_pos(src, SX(752) - colw, cy + ch1 + gap);
            top = row_y;
        } else {
            const int row_y = WT_CONTENT_BOTTOM - air - card_h;
            const int chips_y = row_y - gap - LV_MAX(ch1, ch2);
            lv_obj_set_y(chi, row_y);
            lv_obj_set_y(rng, row_y);
            const int src_x = SX(752) - lv_obj_get_width(src);
            lv_obj_set_pos(src, src_x, chips_y);
            lv_obj_set_pos(chip, src_x - gap - lv_obj_get_width(chip), chips_y);
            top = chips_y;
        }
        lv_obj_set_height(chi, card_h);
        lv_obj_set_height(rng, card_h);
        if (s_hist && top - gap < HIST_Y + HIST_H) {
            const int h = top - gap - HIST_Y;
            lv_obj_set_height(lv_obj_get_parent(s_hist), h);
            lv_obj_set_height(s_hist, h);
            s_bar_lane = h - 2 * BAR_TOP;
        }
    }
#else
    wt_value_card(s_scr, tr(STR_W_RNG_CHI_CAP), val, SX(48), SY(304), SX(190), false);
    // The interval is a pair of constants, never translated; the intro taught
    // what it means. Same figures as kiss_rngq_verdict, 99 dof. 320 wide
    // because the range is 17 mono23 characters: at 280 it wrapped, and the
    // grown card crossed the content floor into the action bar.
    wt_value_card(s_scr, tr(STR_W_RNG_RANGE_CAP), "61.137 .. 148.230",
                  SX(246), SY(304), SX(320), false);

    // UNEVEN is the dice judge's word, reused: same verdict, same 21 locales.
    const char *vs = v == RNGQ_PASS ? tr(STR_W_RNG_EVEN)
                   : v == RNGQ_LOW  ? tr(STR_W_RNG_TOOEVEN)
                                    : tr(STR_W_DICE_UNEVEN);
    // A miss is WARN, never STOP: honest noise lands there once in 500 runs,
    // and the note says exactly that. The chip pair carries both halves of
    // the claim -- the spread, and the source it came from.
    lv_obj_t *chip = wt_state_chip(s_scr, vs, v == RNGQ_PASS ? OK_COL : WARN_COL);
    lv_obj_set_pos(chip, SX(578), SY(314));
    bool live = kiss_trng_live();
    lv_obj_t *src = wt_state_chip(s_scr, live ? tr(STR_W_RNG_ON) : tr(STR_W_RNG_OFF),
                                  live ? OK_COL : WARN_COL);
    lv_obj_set_pos(src, SX(578), SY(352));
#endif

    // The sub lane answers "so is it good?" in a sentence either way: the
    // chips carry the verdict, but a newcomer should not have to decode it
    // from a chip. On a miss, the once-in-500 line; on a pass, the plain one.
    if (s_note)
        wt_note_fit(s_note, v == RNGQ_PASS ? tr(STR_W_RNG_PASS_NOTE)
                                           : tr(STR_W_RNG_RETRY), SX(704), SY(34));

    if (s_exit) lv_obj_delete(s_exit);
    s_exit = wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, WT_BACK_X,
                             WT_ACTION_Y, SX(140), true, exit_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_W_RNG_AGAIN), false, true, WT_ACT_X,
                    WT_ACTION_Y, SX(240), false, go_cb, NULL);
}

static void tick_cb(lv_timer_t *t)
{
    // ~160 accepted samples per 80ms tick: 5000 in about 2.6 seconds -- long
    // enough to watch the piles level out, short enough to never want a bar.
    uint8_t buf[208];
    kiss_trng_fill(buf, sizeof buf);
    kiss_rngq_feed(&s_q, buf, sizeof buf);
    bars_set();
    if (s_cnt) {
        char b[24];
        snprintf(b, sizeof b, "%u / %u", (unsigned)s_q.n, (unsigned)RNGQ_SAMPLES);
        lv_label_set_text(s_cnt, b);
    }
    if (s_q.n >= RNGQ_SAMPLES) {
        lv_timer_delete(t);
        s_tmr = NULL;
        finish();
    }
}

static void run_screen(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wipe_widgets();
    s_scr = wt_screen(s_parent, tr(STR_W_RNG_T), NULL);
    wt_chrome_head(s_scr);
    // The sub lane, empty until a miss puts the once-in-500 line there.
    s_note = wt_note(s_scr, "", SX(48), SY(60), SX(704), SY(34));

    lv_obj_t *card = wt_card(s_scr, HIST_X, HIST_Y, HIST_W, HIST_H);
    // The skyline and its fair share line, in one widget with a draw
    // callback. It fills the card and owns nothing else -- see s_hist.
    s_hist = lv_obj_create(card);
    lv_obj_remove_style_all(s_hist);
    lv_obj_set_pos(s_hist, 0, 0);
    lv_obj_set_size(s_hist, HIST_W, HIST_H);
    lv_obj_remove_flag(s_hist, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_hist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hist, hist_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

#if KISS_NARROW
    // Under the shorter card, not at the scaled 210 the taller one needed.
    s_bar_lane = BAR_H;
    s_cnt = wt_lbl(s_scr, "0 / 5000", HIST_X, HIST_Y + HIST_H + SY(12),
                   wt_font_mono28(), MUT_COL);
#else
    s_cnt = wt_lbl(s_scr, "0 / 5000", HIST_X, SY(316), wt_font_mono28(), MUT_COL);
#endif

    s_exit = wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, WT_BACK_X,
                             WT_ACTION_Y, SX(140), true, run_back_cb, NULL);
    lv_obj_set_ext_click_area(s_exit, 10);

    kiss_rngq_reset(&s_q, 100);
    s_tmr = lv_timer_create(tick_cb, 80, NULL);
}

void kiss_rngaudit_open(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb;
    intro_screen();
}

#ifdef SIMULATOR
void kiss_rngaudit_sim_result(int verdict)
{
    // Rig the piles and render the finished screen. TOO EVEN is every pile
    // at exactly 50 -- the flat comb IS the lesson -- and UNEVEN alternates
    // 30/70, which scores 800.000.
    run_screen();
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    kiss_rngq_reset(&s_q, 100);
    for (int i = 0; i < 100; i++)
        s_q.bin[i] = (verdict < 0) ? 50 : ((i & 1) ? 70 : 30);
    s_q.n = RNGQ_SAMPLES;
    bars_set();
    finish();
}
#endif
