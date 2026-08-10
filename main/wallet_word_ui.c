// See wallet_word_ui.h. Three acts: write it, write it again, hold to commit.
//
// The field is BLANK. The duress wizard prints a reference "KISS" for its
// modifier to sit on, because that stroke is measured against the word's box;
// here there is no box and nothing to sit on, so a printed word would teach a
// placement that does not exist in the game. What the owner rehearses here is
// exactly what has to work later: the shape of their own hand, twice.
//
// The second writing is not politeness. wallet_gword stores a picture of a
// movement, and the only question that matters is whether the owner can make
// the same picture again -- so the screen makes them prove it before anything
// is written to flash. A word that cannot survive being written twice in one
// sitting will not survive a cold morning in six months.
#include "wallet_word_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wallet_gword.h"
#include "wallet_theme.h"
#include "i18n.h"

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);

enum { ST_WRITE = 0, ST_AGAIN, ST_CONFIRM, ST_DONE };
static int s_stage;

static gw_template_t s_first;     // what they wrote the first time

static void stage_show(int stage);
static void stage_build(int stage);

// Same reason as the duress wizard: never build a screen inside the event
// callback that asked for it, or LVGL hands the in-flight release to whatever
// is now under the finger and one tap walks two screens.
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
    if (s_pending >= 0) return;
    s_pending = stage;
    lv_async_call(stage_async_cb, NULL);
}

// ---- capture ---------------------------------------------------------------
// Decimated exactly like the game's sampler in main.c, because what is
// rehearsed here has to be the same ink the game will read later.
#define DPTS 512
#define DSTROKES 12
static int     s_dx[DPTS], s_dy[DPTS];
static uint8_t s_did[DPTS];        // stroke id per point, as the game keeps
static int     s_dn;
static int     s_strokes;
static bool    s_down;

static lv_obj_t *s_canvas;
static lv_obj_t *s_hint;
static lv_obj_t *s_line[DSTROKES];
static lv_point_precise_t s_ink[DSTROKES][DPTS];
static int s_inkn[DSTROKES];

static void draw_reset(void)
{
    s_dn = 0;
    s_strokes = 0;
    s_down = false;
    for (int i = 0; i < DSTROKES; i++) {
        s_inkn[i] = 0;
        if (s_line[i]) lv_obj_add_flag(s_line[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void ink_add(int x, int y)
{
    int s = s_strokes - 1;
    if (s < 0 || s >= DSTROKES || !s_line[s]) return;
    if (s_inkn[s] >= DPTS) return;
    s_ink[s][s_inkn[s]].x = x;
    s_ink[s][s_inkn[s]].y = y;
    s_inkn[s]++;
    if (s_inkn[s] >= 2) {
        lv_line_set_points(s_line[s], s_ink[s], s_inkn[s]);
        lv_obj_clear_flag(s_line[s], LV_OBJ_FLAG_HIDDEN);
    }
}

static void press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);

    if (!s_down) {                       // a new stroke begins
        s_down = true;
        if (s_strokes < DSTROKES) s_strokes++;
    }
    if (s_dn >= DPTS) return;
    if (s_dn == 0 ||
        LV_ABS(p.x - s_dx[s_dn - 1]) >= 10 || LV_ABS(p.y - s_dy[s_dn - 1]) >= 10) {
        s_dx[s_dn] = p.x;
        s_dy[s_dn] = p.y;
        s_did[s_dn] = (uint8_t)s_strokes;
        s_dn++;
        ink_add(p.x, p.y);
    }
}

static void release_cb(lv_event_t *e) { (void)e; s_down = false; }

// ---- screens ---------------------------------------------------------------

static void close_all(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_canvas = s_hint = NULL;
    for (int i = 0; i < DSTROKES; i++) s_line[i] = NULL;
}

static void finish(void)
{
    close_all();
    void (*cb)(void) = s_done;
    s_done = NULL;
    if (cb) cb();
}

static void cancel_cb(lv_event_t *e) { (void)e; finish(); }

static void back_to_kiss_cb(lv_event_t *e)
{
    (void)e;
    (void)gw_stored_set(NULL);
    finish();
}

static void say(const char *msg)
{
    if (!s_hint) return;
    lv_label_set_text(s_hint, msg);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

static void done_cb(lv_event_t *e)
{
    (void)e;
    gw_template_t t;
    if (gw_make(s_dx, s_dy, s_did, s_dn, &t) != 0) {
        // Too small or too short to be anybody's word. Say the one thing that
        // fixes it rather than "invalid": on a panel this size, every draw
        // rejected here was rejected for being small.
        draw_reset();
        say(tr(STR_GD_WORD_SMALL));
        return;
    }
    if (s_stage == ST_WRITE) {
        s_first = t;
        stage_show(ST_AGAIN);
        return;
    }
    if (!gw_matches(&s_first, &t)) {
        // Stay on this screen and let them write again. Sending them back to
        // the first would throw away a word they may well be able to reproduce
        // -- and it is the SECOND writing that is usually the sloppy one.
        draw_reset();
        say(tr(STR_GD_DRAW_BAD_T));
        return;
    }
    stage_show(ST_CONFIRM);
}

static void save_cb(void *ud)
{
    (void)ud;
    // Only here, with the word written twice and matched, does anything
    // persist. Everything before this is undone by walking away.
    (void)gw_stored_set(&s_first);
    stage_show(ST_DONE);
}

static void write_screen(bool again)
{
    s_scr = wt_screen(s_parent,
                      tr(again ? STR_GD_DRAW_AGAIN_T : STR_GD_WORD_T),
                      tr(again ? STR_GD_WORD_AGAIN_S : STR_GD_WORD_S));

    for (int i = 0; i < DSTROKES; i++) {
        s_line[i] = lv_line_create(s_scr);
        lv_obj_set_pos(s_line[i], 0, 0);
        lv_obj_set_style_line_color(s_line[i], wt_accent(), 0);
        lv_obj_set_style_line_width(s_line[i], 5, 0);
        lv_obj_set_style_line_rounded(s_line[i], true, 0);
        lv_obj_add_flag(s_line[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_hint = wt_lbl(s_scr, "", 48, 360, wt_font23(), WT_WARN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    // A transparent catcher over the writing field. Deliberately blank: a word
    // has nothing to be placed against, so this is as empty as the game screen
    // the owner will write on later.
    s_canvas = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 110);
    lv_obj_set_size(s_canvas, 800, 240);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, release_cb, LV_EVENT_RELEASED, NULL);

    // DONE, because a written word has no other end. One mark ends when the
    // finger lifts; a word of several letters does not, and guessing at it with
    // a timer would either cut people off mid-word or make them wait.
    // CANCEL leftmost with every other way out on the device, DONE in the
    // corner because it is what this screen is for. BACK TO KISS is optional,
    // so DONE right aligns to 752 either way rather than sliding when the third
    // pill is absent: a control that moves under the finger between visits is
    // the thing this whole pass is removing.
    const bool back_to_kiss = (!again && gw_stored_any());
    wt_pill(s_scr, tr(STR_C_CANCEL), 48, WT_ACTION_Y, 140, cancel_cb, NULL);
    if (back_to_kiss)
        wt_pill(s_scr, tr(STR_GD_WORD_BACK_T), 210, WT_ACTION_Y, 260,
                back_to_kiss_cb, NULL);
    wt_pill(s_scr, tr(STR_GD_WORD_DONE), 492, WT_ACTION_Y, 260, done_cb, NULL);
    draw_reset();
}

// The stop screen. Everything this changes is said before it is held, and held
// rather than tapped, like every other control here that walking back cannot
// undo.
static void confirm_screen(void)
{
    s_scr = wt_screen(s_parent, tr(STR_GD_WORD_C_T), NULL);
    {
        const char *b1 = tr(STR_GD_WORD_C_W1_B), *b2 = tr(STR_GD_WORD_C_W2_B);
        const int BW = 344, BY = 232, BH = WT_CONTENT_BOTTOM - BY;
        const lv_font_t *f = wt_body_font2(b1, b2, BW - 14, BH - 46 - 8);
        wt_why_block(s_scr, tr(STR_GD_WORD_C_W1_H), b1,  48, BY, BW, BH, f,
                     wt_accent());
        wt_why_block(s_scr, tr(STR_GD_WORD_C_W2_H), b2, 408, BY, BW, BH, f,
                     WT_WARN);
    }
    wt_hold_pill(s_scr, tr(STR_GD_WORD_HOLD), 48, WT_ACTION_Y, 330,
                 WT_ACTION_H, 2000, save_cb, NULL);
    wt_pill(s_scr, tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 140, cancel_cb, NULL);
}

static void done_screen(void)
{
    s_scr = wt_screen(s_parent, tr(STR_GD_WORD_OK_T), NULL);
    wt_wraph(s_scr, tr(STR_GD_WORD_OK_B), 48, 180, 704, WT_CONTENT_BOTTOM - 180);
    wt_pill(s_scr, tr(STR_C_OK), 300, WT_ACTION_Y, 200, cancel_cb, NULL);
}

static void stage_build(int stage)
{
    close_all();
    s_stage = stage;
    switch (stage) {
    case ST_WRITE:   write_screen(false); break;
    case ST_AGAIN:   write_screen(true);  break;
    case ST_CONFIRM: confirm_screen();    break;
    case ST_DONE:    done_screen();       break;
    }
}

void wallet_word_ui_open(lv_obj_t *parent, void (*done_cb2)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb2;
    s_pending = -1;
    memset(&s_first, 0, sizeof s_first);
    stage_show(ST_WRITE);
}

bool wallet_word_ui_active(void) { return s_scr != NULL || s_pending >= 0; }
