// See wallet_duress_ui.h. Four acts: teach it, say to fund the spare, pick your
// stroke, draw it twice.
//
// The draw screens ask for the MODIFIER only, over a printed reference word,
// rather than the whole "KISS + stroke". Two reasons. The word is not the part
// being chosen, so rehearsing it teaches nothing new; and a printed reference
// gives the classifier a known box to measure against, which is the same thing
// it will measure against in the game (the bounding box of what was drawn).
// What the owner rehearses here is exactly what has to work later: the stroke,
// placed relative to the word.
#include "wallet_duress_ui.h"

#include <stdint.h>
#include <string.h>

#include "wallet_duress.h"
#include "wallet_theme.h"
#include "i18n.h"

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);

// The reference word's box on the draw screens. The label is centred on this,
// and it is what wallet_duress_classify measures the stroke against.
#define REF_X0 250
#define REF_Y0 170
#define REF_X1 550
#define REF_Y1 268

// Stage machine. ST_PICK chooses the stroke, the two ST_DRAW stages rehearse
// it; the second is the confirmation.
enum { ST_INTRO = 0, ST_FUND, ST_PICK, ST_DRAW1, ST_DRAW2, ST_DONE, ST_NOPASS };
static int s_stage;
static int s_pick;

// ---- stroke capture ------------------------------------------------------
// Same decimation rule as the game's unlock sampler in main.c: store a point
// only once the finger has actually moved. Bounding the buffer by ink rather
// than by time is what keeps a slow, careful draw from filling it before the
// stroke is finished.
#define DPTS 256
static int s_dx[DPTS], s_dy[DPTS];
static int s_dn;
static lv_obj_t *s_canvas;        // full-screen catcher, owns the touch
static lv_obj_t *s_hint;          // "that was not it" line, hidden until needed
// Live ink. Drawing a gesture and seeing nothing happen is indistinguishable
// from a dead touch panel, and this screen asks people to do it twice.
static lv_obj_t *s_ink_line;
static lv_point_precise_t s_ink[DPTS];

static void stage_show(int stage);
static void stage_build(int stage);

// A stage change must NOT build its screen inside the event callback that
// triggered it. LVGL keeps dispatching the in-flight press to whatever is now
// under the finger, so a screen created mid-event immediately receives the
// same release -- and because every screen here has a pill in the same corner,
// one tap walked the entire chooser end to end. Deferring the build to
// lv_async_call ends the event pass first, which is the only reliable fix.
static int s_pending = -1;

static void stage_async_cb(void *ud)
{
    (void)ud;
    int st = s_pending;
    s_pending = -1;
    if (st >= 0) stage_build(st);
}

static void stage_show(int stage)
{
    if (s_pending >= 0) return;      // one transition per input event, never two
    s_pending = stage;
    lv_async_call(stage_async_cb, NULL);
}

static void draw_reset(void)
{
    s_dn = 0;
    if (s_ink_line) lv_obj_add_flag(s_ink_line, LV_OBJ_FLAG_HIDDEN);
}

static void ink_update(void)
{
    if (!s_ink_line || s_dn < 2) return;
    for (int i = 0; i < s_dn; i++) {
        s_ink[i].x = s_dx[i];
        s_ink[i].y = s_dy[i];
    }
    lv_line_set_points(s_ink_line, s_ink, s_dn);
    lv_obj_clear_flag(s_ink_line, LV_OBJ_FLAG_HIDDEN);
}

static void draw_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (s_dn >= DPTS) return;
    if (s_dn == 0 ||
        LV_ABS(p.x - s_dx[s_dn - 1]) >= 10 || LV_ABS(p.y - s_dy[s_dn - 1]) >= 10) {
        s_dx[s_dn] = p.x;
        s_dy[s_dn] = p.y;
        s_dn++;
        ink_update();
    }
}

static void draw_release_cb(lv_event_t *e)
{
    (void)e;
    const int got = wallet_duress_classify(s_dx, s_dy, s_dn,
                                           REF_X0, REF_Y0, REF_X1, REF_Y1);
    draw_reset();

    if (got == s_pick) {
        stage_show(s_stage + 1);
        return;
    }
    // Wrong, or not recognized at all. Say so and let them draw again on the
    // same screen: sending them back to the picker on a wobble would teach
    // people that the stroke they chose "does not work", when it does.
    if (s_hint) {
        lv_label_set_text(s_hint, tr(STR_GD_DRAW_BAD_T));
        lv_obj_set_style_text_color(s_hint, WT_WARN, 0);
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- screens -------------------------------------------------------------

static void close_all(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_canvas = NULL;
    s_hint = NULL;
    s_ink_line = NULL;
    s_dn = 0;
}

static void finish(void)
{
    void (*cb)(void) = s_done;
    s_pending = -1;                  // drop any queued build; we are leaving
    close_all();
    s_stage = ST_INTRO;
    if (cb) cb();
}

static void skip_cb(lv_event_t *e) { (void)e; finish(); }

static void save_cb(lv_event_t *e)
{
    (void)e;
    // Only now, with the stroke drawn twice and matched twice, does anything
    // persist.
    (void)wallet_duress_set(s_pick);
    finish();
}

static void turn_off_cb(lv_event_t *e)
{
    (void)e;
    (void)wallet_duress_set(WDG_NONE);
    finish();
}

static void pick_cb(lv_event_t *e)
{
    s_pick = (int)(intptr_t)lv_event_get_user_data(e);
    stage_show(s_stage + 1);
}

static void next_cb(lv_event_t *e) { (void)e; stage_show(s_stage + 1); }

// Six pills in two rows of three. Laid out on the 800x480 grid the rest of the
// wallet uses, with the row heights the pill kit expects.
static void pick_screen(void)
{
    s_scr = wt_screen(s_parent, tr(STR_GD_PICK_REAL_T), tr(STR_GD_PICK_REAL_S));
    for (int g = WDG_UNDERLINE; g < WDG_N; g++) {
        int slot = g - WDG_UNDERLINE, col = slot % 3, row = slot / 3;
        wt_pill(s_scr, tr(wallet_duress_label_key(g)),
                48 + col * 240, 150 + row * 104, 220, pick_cb, (void *)(intptr_t)g);
    }
    wt_pill(s_scr, tr(STR_GD_SKIP), 610, WT_ACTION_Y, 140, skip_cb, NULL);
}

static void draw_screen(bool again)
{
    s_scr = wt_screen(s_parent, tr(again ? STR_GD_DRAW_AGAIN_T : STR_GD_DRAW_T),
                      tr(again ? STR_GD_DRAW_AGAIN_S : STR_GD_DRAW_S));

    // the stroke being rehearsed, named, so a mis-tap on the picker is obvious
    // here rather than two screens later
    wt_lbl(s_scr, tr(wallet_duress_label_key(s_pick)), 48, 110, wt_font23(), wt_accent());

    // The reference word sits inside a faint box, and the box is not decoration:
    // it IS what wallet_duress_classify measures against, so "above" and "below"
    // have to be visible or they are a guess.
    lv_obj_t *box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, REF_X0, REF_Y0);
    lv_obj_set_size(box, REF_X1 - REF_X0, REF_Y1 - REF_Y0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, WT_MUT, 0);
    lv_obj_set_style_border_opa(box, 90, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *w = wt_lbl(s_scr, "KISS", 0, 0, wt_font34(), WT_INK);
    lv_obj_update_layout(w);
    lv_obj_set_pos(w, REF_X0 + (REF_X1 - REF_X0 - lv_obj_get_width(w)) / 2,
                      REF_Y0 + (REF_Y1 - REF_Y0 - lv_obj_get_height(w)) / 2);

    s_ink_line = lv_line_create(s_scr);
    lv_obj_set_pos(s_ink_line, 0, 0);
    lv_obj_set_style_line_color(s_ink_line, wt_accent(), 0);
    lv_obj_set_style_line_width(s_ink_line, 5, 0);
    lv_obj_set_style_line_rounded(s_ink_line, true, 0);
    lv_obj_add_flag(s_ink_line, LV_OBJ_FLAG_HIDDEN);

    s_hint = wt_lbl(s_scr, "", 48, 300, wt_font23(), WT_WARN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    // A transparent catcher over the whole page collects the stroke. It is
    // added LAST so it sits above the labels; the BACK pill below is added
    // after it and therefore stays reachable.
    s_canvas = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 96);
    lv_obj_set_size(s_canvas, 800, 300);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, draw_press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, draw_release_cb, LV_EVENT_RELEASED, NULL);

    wt_pill(s_scr, tr(STR_GD_SKIP), 610, WT_ACTION_Y, 140, skip_cb, NULL);
    draw_reset();
}

static void stage_build(int stage)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_canvas = NULL;
    s_hint = NULL;
    s_ink_line = NULL;
    s_stage = stage;

    switch (stage) {
    // Two teaching screens, each the full 704px width. They were one
    // two-column screen first, and it rendered badly in every language: the
    // right-hand heading ran off the panel and the body wrapped mid-sentence,
    // because 250px cannot hold copy written in lines sized for 430.
    //
    // Each body sits in a card now, which is the house look and also what makes
    // three paragraphs of teaching copy read as one block rather than as loose
    // text on the page. The body gives up 16px of width to the card's padding and
    // has it to spare: the height budget is 260 against about 200 of English.
    case ST_INTRO: {
        s_scr = wt_screen(s_parent, tr(STR_GD_INTRO_T), tr(STR_GD_INTRO_S));
        wt_card(s_scr, 36, 104, 716, 288);
        lv_obj_t *b = wt_wraph(s_scr, tr(STR_GD_INTRO_B), 52, 118, 688, 260);
        lv_obj_set_style_text_color(b, WT_INK, 0);
        // "SET UP A SPARE", not OK: on a screen explaining a decoy wallet, an
        // OK button tells the owner nothing about which of the two things is
        // about to happen. This one commits to the second wallet with words.
        wt_pill(s_scr, tr(STR_GD_SET_UP_SPARE), 48, WT_ACTION_Y, 240, next_cb, NULL);
        // Reached from Settings with a configuration already in place, this is
        // the only way back to plain behaviour. Absent during setup, where
        // there is nothing yet to turn off.
        if (wallet_duress_real() != WDG_NONE)
            wt_pill(s_scr, tr(STR_GD_TURN_OFF), 280, WT_ACTION_Y, 260, turn_off_cb, NULL);
        wt_pill(s_scr, tr(STR_GD_SKIP), 610, WT_ACTION_Y, 140, skip_cb, NULL);
        break;
    }
    case ST_FUND: {
        s_scr = wt_screen(s_parent, tr(STR_GD_FUND_T), NULL);
        wt_card(s_scr, 36, 104, 716, 288);
        lv_obj_t *b = wt_wraph(s_scr, tr(STR_GD_FUND_B), 52, 118, 688, 260);
        lv_obj_set_style_text_color(b, WT_INK, 0);
        // Same rationale as ST_INTRO: name the action.
        wt_pill(s_scr, tr(STR_GD_SET_UP_SPARE), 48, WT_ACTION_Y, 240, next_cb, NULL);
        wt_pill(s_scr, tr(STR_GD_SKIP), 610, WT_ACTION_Y, 140, skip_cb, NULL);
        break;
    }
    case ST_PICK:  pick_screen();        break;
    case ST_DRAW1: draw_screen(false);   break;
    case ST_DRAW2: draw_screen(true);    break;
    case ST_NOPASS: {
        s_scr = wt_screen(s_parent, tr(STR_GD_NOPASS_T), NULL);
        wt_card(s_scr, 36, 104, 716, 288);
        lv_obj_t *b = wt_wraph(s_scr, tr(STR_GD_NOPASS_B), 52, 118, 688, 260);
        lv_obj_set_style_text_color(b, WT_INK, 0);
        // The only way back to plain behaviour for a signer that was allowed to
        // configure a stroke before this case was handled.
        if (wallet_duress_real() != WDG_NONE)
            wt_pill(s_scr, tr(STR_GD_TURN_OFF), 48, WT_ACTION_Y, 260, turn_off_cb, NULL);
        wt_pill(s_scr, tr(STR_C_OK), 610, WT_ACTION_Y, 140, skip_cb, NULL);
        break;
    }
    default: {
        s_scr = wt_screen(s_parent, tr(STR_GD_DONE_T), NULL);
        wt_why_body(s_scr, tr(STR_GD_DONE_B), 118, WT_OK, true);
        wt_pill(s_scr, tr(STR_C_DONE), 48, WT_ACTION_Y, 200, save_cb, NULL);
        break;
    }
    }
}

void wallet_duress_ui_open(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb;
    s_pick = WDG_NONE;
    s_pending = -1;
    stage_show(ST_INTRO);
}

void wallet_duress_ui_open_nopass(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb;
    s_pick = WDG_NONE;
    s_pending = -1;
    stage_show(ST_NOPASS);
}

bool wallet_duress_ui_active(void) { return s_scr != NULL || s_pending >= 0; }
