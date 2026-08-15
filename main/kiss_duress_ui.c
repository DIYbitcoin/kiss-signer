// See kiss_duress_ui.h. Four acts: teach it, say to fund the spare, pick your
// stroke, draw it twice.
//
// The draw screens ask for the MODIFIER only, over a printed reference word,
// rather than the whole "KISS + stroke". Two reasons. The word is not the part
// being chosen, so rehearsing it teaches nothing new; and a printed reference
// gives the classifier a known box to measure against, which is the same thing
// it will measure against in the game (the bounding box of what was drawn).
// What the owner rehearses here is exactly what has to work later: the stroke,
// placed relative to the word.
#include "kiss_duress_ui.h"

#include <stdint.h>
#include <string.h>

#include "kiss_duress.h"
#include "kiss_duress_ui.h"
#include "kiss_theme.h"
#include "kiss_ui.h"
#include "i18n.h"
#include "kiss_wipe.h"

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);

// Stage machine. The stroke picker and its two rehearsal stages are GONE,
// deliberately: the chosen stroke was never read on the unlock path -- any
// single extra swipe after the word routed to the passphrase keyboard, and
// the passphrase was always the real gate. A wizard that stores a choice
// nothing consults is worse than no wizard: it teaches a secret that does
// not exist and adds a lockout the device never imposed. What remains is
// the truth: teach the two-signer idea, say to fund the spare, and state
// the rule -- one extra swipe, any swipe, asks for your passphrase.
enum { ST_INTRO = 0, ST_FUND, ST_DONE, ST_NOPASS };
static int s_stage;

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

// ---- screens -------------------------------------------------------------

static void close_all(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

// The lock's close, kiss_fw_ui_close's shape: drop the screen AND the done
// callback. One caller of this wizard is the last step of setup, with the
// staged seed live behind it -- a done fired by a teardown must never run.
void kiss_duress_ui_lock_close(void)
{
    s_done = NULL;
    s_pending = -1;
    close_all();
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

static void add_pass_cb(lv_event_t *e)
{
    (void)e;
    // The add-later login owns the display while it runs and returns through
    // the same channel the rest of this flow uses: its final screens chain
    // into the stroke chooser (setup_warn_ok_cb), whose DONE closes back to
    // whoever opened the ways in page.
    lv_obj_t *parent = s_parent;
    void (*done)(void) = s_done;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_login_open_add_later(done);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    // Nothing persists: there is no chosen stroke any more, only the rule.
    finish();
}

static void turn_off_cb(lv_event_t *e)
{
    (void)e;
    (void)kiss_duress_set(WDG_NONE);
    finish();
}

static void next_cb(lv_event_t *e) { (void)e; stage_show(s_stage + 1); }

// ---- the teaching diagrams ------------------------------------------------
// A centred flex column to hang diagram rows off, because wt_diagram_row sizes
// itself to its content and a screen is not a flex container. Same shape as the
// asides in kiss_info.c; kept local because only this file stacks two rows.
static lv_obj_t *diagram_box(int y)
{
    lv_obj_t *col = lv_obj_create(s_scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 48, y);
    lv_obj_set_width(col, 704);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

static void chip_icon(lv_obj_t *row, const char *icon, const char *txt,
                      bool accent)
{
    char buf[WT_ICON_TEXT_MAX];
    wt_icon_text(buf, sizeof buf, icon, txt);
    wt_chip(row, buf, accent);
}

// The two ways in, drawn. This was three paragraphs saying which gesture opens
// which wallet, and a gesture mapping is a picture: the letters, what is added
// to them, and what opens. The spare is plain and the real one is accented,
// which is the same accent-is-the-outcome rule the fingerprint equation uses.
//
// KISS is not translated because it is not a word here, it is the four letters
// the finger draws. The stroke's name and both wallet labels are.
static void diagram_two_ways(void)
{
    lv_obj_t *box = diagram_box(112);

    lv_obj_t *r1 = wt_diagram_row(box);
    wt_chip(r1, "KISS", false);
    wt_diagram_op(r1, LV_SYMBOL_RIGHT);
    chip_icon(r1, WT_ICON_SECRET, tr(STR_D_SPARE), false);

    lv_obj_t *r2 = wt_diagram_row(box);
    wt_chip(r2, "KISS", false);
    wt_diagram_op(r2, "+");
    wt_chip(r2, tr(STR_GD_PICK_REAL_T), false);
    wt_diagram_op(r2, LV_SYMBOL_RIGHT);
    chip_icon(r2, WT_ICON_KEY, tr(STR_D_REAL), true);
}

static void stage_build(int stage)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_stage = stage;

    switch (stage) {
    // The two teaching screens were a card with three paragraphs of prose in
    // it. The card counts as chrome, so the BARE gate passed them, and the
    // reader still met forty words of grey where the subject is a mapping from
    // gestures to wallets -- which is a picture, not a paragraph.
    //
    // Each one now leads with the diagram and keeps only the claim the diagram
    // cannot make. wt_why_body splits on blank lines and places itself, so the
    // body no longer needs a height budget guessed against English.
    case ST_INTRO: {
        s_scr = wt_screen(s_parent, tr(STR_GD_INTRO_T), tr(STR_GD_INTRO_S));
        diagram_two_ways();
        wt_why_body(s_scr, tr(STR_GD_INTRO_B), 250, wt_primary(), false);
        // The only TALL action row in this flow, and the only one that needs to
        // be. It carries THREE pills, and "SET UP A SPARE" is 232 to 317px at
        // font23 in twelve locales against a 212px budget, so on a 52px row
        // wt_pill_fit ran out of rungs and drew the screen's primary action in
        // the smallest type on it. At 66 the same label takes a SECOND LINE at
        // 23 instead -- which is exactly what WT_ACTION_Y_TALL is for, and the
        // reason it already exists is the same one: HOLD TO SIGN.
        //
        // Widening was not available. At the widths font23 needs on one line
        // (345 + 260 + 190, plus gaps) the row wants 819px of the 702 between
        // 48 and 750. Wrapping buys the height instead of the width.
        //
        // The three x positions are also a fix. SET UP A SPARE ran to 288 and
        // TURN THIS OFF started at 280, so the two overlapped by 8px whenever a
        // configured wallet arrived here from Settings -- the one path no walk
        // stop visits, which is why it survived. 48..288, 300..520, 560..750.
        lv_obj_t *row[3];
        int nrow = 0;
        // "SET UP A SPARE", not OK: on a screen explaining a decoy wallet, an
        // OK button tells the owner nothing about which of the two things is
        // about to happen. This one commits to the second wallet with words.
        row[nrow++] = wt_pillh(s_scr, tr(STR_GD_SET_UP_SPARE), 48,
                               WT_ACTION_Y_TALL, 240, WT_ACTION_H_TALL,
                               next_cb, NULL);
        // Reached from Settings with a configuration already in place, this is
        // the only way back to plain behaviour. Absent during setup, where
        // there is nothing yet to turn off.
        if (kiss_duress_real() != WDG_NONE)
            row[nrow++] = wt_pillh(s_scr, tr(STR_GD_TURN_OFF), 300,
                                   WT_ACTION_Y_TALL, 220, WT_ACTION_H_TALL,
                                   turn_off_cb, NULL);
        row[nrow++] = wt_pillh(s_scr, tr(STR_GD_SKIP), 560, WT_ACTION_Y_TALL,
                               190, WT_ACTION_H_TALL, skip_cb, NULL);
        // One rung for the row. Without this the three fit independently and
        // the screen can draw a 23 beside a 14, which reads as one button
        // mattering more than the one that leaves.
        wt_pill_row(row, nrow);
        break;
    }
    case ST_FUND: {
        s_scr = wt_screen(s_parent, tr(STR_GD_FUND_T), NULL);
        // The spare has an identity of its own, which is the half of this
        // screen that is a fact rather than an instruction -- and the diagram
        // now says SO. It drew SPARE -> FINGERPRINT, which reads as though the
        // spare produced THE fingerprint, the one the owner already wrote down.
        // It does not: it is a different keyset with a different code, and a
        // reader who misses that will compare the spare against their paper and
        // conclude the device is broken.
        lv_obj_t *row = wt_diagram_row(diagram_box(112));
        chip_icon(row, WT_ICON_SECRET, tr(STR_D_SPARE), false);
        wt_diagram_op(row, LV_SYMBOL_RIGHT);
        chip_icon(row, WT_ICON_KEY, tr(STR_D_OWN_FP), true);
        // Two claims, two columns: it really works, and an empty one is a tell.
        wt_why_body(s_scr, tr(STR_GD_FUND_B), 190, WT_WARN, true);
        // Same rationale as ST_INTRO, opposite wallet -- and that is the whole
        // point of the pair. ST_INTRO's pill opens THIS screen, which is about
        // the spare, so it says SPARE. This pill opens ST_DONE, which
        // states the real signer's rule. It said SET UP A
        // SPARE for both, which put the word SPARE on the door to the real
        // wallet's only setting, and readers concluded the stroke belonged to
        // the decoy. The two CTAs name different wallets on purpose.
        // 420, not the 240 the other CTAs take. This screen's action row holds
        // only this pill and NOT NOW at 610, so there is nothing to crowd, and
        // 240 was not enough: "NOW THE REAL ONE" needs 261px at font23 and cs,
        // pl and ru need up to 289, so every one of them dropped to font14 --
        // the rung wt_pill_fit reaches only after tracking and a second line
        // have both failed. A routing button rendered in the smallest type on
        // the screen is the one that gets skimmed, which is how the stroke ends
        // up on the wrong wallet.
        wt_pill(s_scr, tr(STR_GD_SET_UP_REAL), 48, WT_ACTION_Y, 420, next_cb, NULL);
        wt_pill(s_scr, tr(STR_GD_SKIP), 560, WT_ACTION_Y, 190, skip_cb, NULL);
        break;
    }
    case ST_NOPASS: {
        s_scr = wt_screen(s_parent, tr(STR_GD_NOPASS_T), NULL);
        // Why there is nothing to hide behind, in two chips: the layer this
        // feature stands on is missing. GD_OFF is the same "NOT SET" the ways
        // in row on Settings shows, so the reader has met it already.
        lv_obj_t *row = wt_diagram_row(diagram_box(112));
        chip_icon(row, WT_ICON_LOCK, tr(STR_D_PASSPHRASE), false);
        wt_diagram_op(row, LV_SYMBOL_RIGHT);
        wt_chip(row, tr(STR_GD_OFF), false);
        wt_why_body(s_scr, tr(STR_GD_NOPASS_B), 190, WT_WARN, true);
        // The missing layer is ADDABLE, and this is the room for it. The
        // body above already says the rest: add a passphrase and the current
        // keys become the spare. The add-later login runs the wizard's
        // type-twice + fingerprint reveal (kiss_login_open_add_later) and
        // falls out through setup_warn_ok_cb the same way the wizard does,
        // so the stroke chooser is the very next screen -- setting a stroke
        // is exactly why most owners will be doing this.
        wt_pill(s_scr, tr(STR_L_CREATE_PASS_BTN), 48, WT_ACTION_Y,
                kiss_duress_real() != WDG_NONE ? 340 : 556,
                add_pass_cb, NULL);
        // The only way back to plain behaviour for a signer that was allowed to
        // configure a stroke before this case was handled.
        if (kiss_duress_real() != WDG_NONE)
            wt_pill(s_scr, tr(STR_GD_TURN_OFF), 396, WT_ACTION_Y, 208, turn_off_cb, NULL);
        wt_pill(s_scr, tr(STR_C_OK), WT_BACK_X, WT_ACTION_Y, 140, skip_cb, NULL);
        break;
    }
    default: {
        s_scr = wt_screen(s_parent, tr(STR_GD_DONE_T), NULL);
        // Same shape as ST_INTRO: the mapping is drawn, and the body keeps
        // only what the diagram cannot say — the stroke routes, it does not
        // unlock, and this is the last screen in the flow that says so.
        diagram_two_ways();
        wt_why_body(s_scr, tr(STR_GD_DONE_B), 250, WT_OK, true);
        // 552..752: 200 wide, so it cannot use WT_BACK_X and still sit flush.
        wt_pill(s_scr, tr(STR_C_DONE), 552, WT_ACTION_Y, 200, save_cb, NULL);
        break;
    }
    }
}

void kiss_duress_ui_open(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb;
    s_pending = -1;
    stage_show(ST_INTRO);
}

void kiss_duress_ui_open_nopass(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb;
    s_pending = -1;
    stage_show(ST_NOPASS);
}

bool kiss_duress_ui_active(void) { return s_scr != NULL || s_pending >= 0; }
