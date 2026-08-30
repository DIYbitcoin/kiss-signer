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
#define HIST_X   48
#define HIST_Y   96
#define HIST_W   704
#define HIST_H   200
#define BAR_W    6
#define BAR_PITCH 7
#define BAR_X0   2
#define BAR_TOP  12
#define BAR_H    176

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void    (*s_done)(void);
static lv_timer_t *s_tmr;
static kiss_rngq_t s_q;
static lv_obj_t *s_fill[100];
static lv_obj_t *s_cnt;    // the running tally; the result widgets replace it
static lv_obj_t *s_note;   // the sub lane: carries the retry line on a miss
static lv_obj_t *s_exit;   // BACK while running, DONE once finished

static void intro_screen(void);
static void run_screen(void);

bool kiss_rngaudit_active(void) { return s_scr != NULL; }

static void wipe_widgets(void)
{
    s_cnt = s_note = s_exit = NULL;
    for (int i = 0; i < 100; i++) s_fill[i] = NULL;
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
             WT_CHOICE_X, WT_CHOICE_Y(0), WT_CHOICE_W, WT_CHOICE_H,
             NULL, NULL);

    // What the test scores and what it cannot see, as rows under the row that
    // says where the numbers come from. With no source the second one carries
    // the refusal instead of the caution: a formula passes this test too, so
    // there is nothing here to audit -- the same reason key material refuses.
    wt_fact_t facts[2] = {
        { .cap = tr(STR_W_RNG_WHY1_H), .val = tr(STR_W_RNG_WHY1_B),
          .icon = LV_SYMBOL_SHUFFLE },
        { .cap = tr(STR_W_RNG_WHY2_H),
          .val = live ? tr(STR_W_RNG_WHY2_B) : tr(STR_W_RNG_NOSRC_B),
          .icon = LV_SYMBOL_WARNING, .icon_col = WT_WARN },
    };
    wt_facts(s_scr, 204, facts, 2);

    lv_obj_t *back = wt_arrow_action(s_scr, tr(STR_C_BACK), true, false,
                                     WT_BACK_X, WT_ACTION_Y, 140, true,
                                     exit_cb, NULL);
    lv_obj_set_ext_click_area(back, 10);
    // No START without a source. Absent, not greyed: a disabled control is a
    // shape this product does not draw.
    if (live)
        wt_arrow_action(s_scr, tr(STR_W_RNG_GO), false, true, WT_ACT_X,
                        WT_ACTION_Y, 240, false, go_cb, NULL);
}

// ---- the run ----

static void bars_set(void)
{
    if (!s_q.n) return;
    for (int i = 0; i < s_q.bins; i++) {
        if (!s_fill[i]) continue;
        // Live and honest at draw 300 and draw 5000 alike: the scale is the
        // CURRENT count against the current fair share, so the skyline
        // settles toward the line instead of climbing a fixed axis.
        int h = (int)((uint32_t)s_q.bin[i] * (BAR_H / 2) * s_q.bins / s_q.n);
        if (h > BAR_H) h = BAR_H;
        lv_obj_set_pos(s_fill[i], BAR_X0 + i * BAR_PITCH, BAR_TOP + BAR_H - h);
        lv_obj_set_size(s_fill[i], BAR_W, h);
    }
}

static void finish(void)
{
    uint32_t milli = kiss_rngq_chi2_milli(&s_q);
    int v = kiss_rngq_verdict(&s_q, milli);

    if (s_cnt) { lv_obj_delete(s_cnt); s_cnt = NULL; }

    char val[16];
    snprintf(val, sizeof val, "%u.%03u", (unsigned)(milli / 1000),
             (unsigned)(milli % 1000));
    wt_value_card(s_scr, tr(STR_W_RNG_CHI_CAP), val, 48, 304, 190, false);
    // The interval is a pair of constants, never translated; the intro taught
    // what it means. Same figures as kiss_rngq_verdict, 99 dof. 320 wide
    // because the range is 17 mono23 characters: at 280 it wrapped, and the
    // grown card crossed the content floor into the action bar.
    wt_value_card(s_scr, tr(STR_W_RNG_RANGE_CAP), "61.137 .. 148.230",
                  246, 304, 320, false);

    // UNEVEN is the dice judge's word, reused: same verdict, same 21 locales.
    const char *vs = v == RNGQ_PASS ? tr(STR_W_RNG_EVEN)
                   : v == RNGQ_LOW  ? tr(STR_W_RNG_TOOEVEN)
                                    : tr(STR_W_DICE_UNEVEN);
    // A miss is WARN, never STOP: honest noise lands there once in 500 runs,
    // and the note says exactly that. The chip pair carries both halves of
    // the claim -- the spread, and the source it came from.
    lv_obj_t *chip = wt_state_chip(s_scr, vs, v == RNGQ_PASS ? OK_COL : WARN_COL);
    lv_obj_set_pos(chip, 578, 314);
    bool live = kiss_trng_live();
    lv_obj_t *src = wt_state_chip(s_scr, live ? tr(STR_W_RNG_ON) : tr(STR_W_RNG_OFF),
                                  live ? OK_COL : WARN_COL);
    lv_obj_set_pos(src, 578, 352);

    // The sub lane answers "so is it good?" in a sentence either way: the
    // chips carry the verdict, but a newcomer should not have to decode it
    // from a chip. On a miss, the once-in-500 line; on a pass, the plain one.
    if (s_note)
        wt_note_fit(s_note, v == RNGQ_PASS ? tr(STR_W_RNG_PASS_NOTE)
                                           : tr(STR_W_RNG_RETRY), 704, 34);

    if (s_exit) lv_obj_delete(s_exit);
    s_exit = wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, WT_BACK_X,
                             WT_ACTION_Y, 140, true, exit_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_W_RNG_AGAIN), false, true, WT_ACT_X,
                    WT_ACTION_Y, 240, false, go_cb, NULL);
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
    s_note = wt_note(s_scr, "", 48, 60, 704, 34);

    lv_obj_t *card = wt_card(s_scr, HIST_X, HIST_Y, HIST_W, HIST_H);
    for (int i = 0; i < 100; i++) {
        lv_obj_t *f = lv_obj_create(card);
        lv_obj_remove_style_all(f);
        lv_obj_set_pos(f, BAR_X0 + i * BAR_PITCH, BAR_TOP + BAR_H);
        lv_obj_set_size(f, BAR_W, 0);
        lv_obj_set_style_bg_color(f, wt_accent(), 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
        s_fill[i] = f;
    }
    // The fair share line, drawn LAST so it crosses over the fills -- the
    // dice bars' trick, at 100 columns instead of 6.
    lv_obj_t *tick = lv_obj_create(card);
    lv_obj_remove_style_all(tick);
    lv_obj_set_pos(tick, 0, BAR_TOP + BAR_H / 2);
    lv_obj_set_size(tick, HIST_W, 1);
    lv_obj_set_style_bg_color(tick, WT_DIV, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE);

    s_cnt = wt_lbl(s_scr, "0 / 5000", HIST_X, 316, wt_font_mono28(), MUT_COL);

    s_exit = wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, WT_BACK_X,
                             WT_ACTION_Y, 140, true, run_back_cb, NULL);
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
