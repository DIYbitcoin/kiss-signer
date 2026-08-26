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
#include "kiss_word_ui.h"
#include "i18n.h"
#include "kiss_wipe.h"

static lv_obj_t *s_scr;
// The rehearsal's three widgets. Declared up here with the screen they
// belong to, so close_all() can null them: they live only on ST_DRAW, and
// rehearse_release_cb reads s_word_box without a guard.
static lv_obj_t *s_word_box, *s_rhint, *s_rink;
static lv_obj_t *s_parent;
static void (*s_done)(void);

// Stage machine. The picker and its rehearsal were deleted once, correctly:
// the chosen stroke was never read on the unlock path, so the wizard stored a
// choice nothing consulted and taught a secret that did not exist.
//
// They are back because kiss_duress_route now reads it. The bare word answers
// with the decoy on every device, configured or not, which is where the leak
// actually lived -- and with that fixed the stroke is free to decide something
// without the device announcing anything. See kiss_duress.h.
//
// The rehearsal is not ceremony. A stroke the owner cannot reproduce is a
// stroke that quietly stops reaching their keys, and the picker alone cannot
// tell a shape they can draw from one they merely liked the name of. Drawing it
// twice, classified by the same code the unlock runs, is the only thing here
// that proves it works.
// ST_ACK sits between learning and configuring on purpose: it is the last
// screen before anything is stored, and what it asks the owner to confirm is
// the one fact the whole feature rests on -- that both signers are the same
// seed words, and the passphrase is the only thing separating them. An owner
// who has not understood that will fund the wrong one.
enum { ST_INTRO = 0, ST_FUND, ST_ACK, ST_PICK, ST_DRAW, ST_DONE, ST_NOPASS };
static int s_stage;

// The stroke being rehearsed and how many clean repeats it has. Nothing is
// stored until the second one lands: kiss_duress_set on the first would leave a
// half-learned stroke routing the device.
static int s_pick = WDG_NONE;
static int s_got;

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
    s_word_box = s_rhint = s_rink = NULL;
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
    void (*done)(void) = s_done;
    close_all();                     // and not a bare delete: the rehearsal's
                                     // widgets are the point of that function
    kiss_login_open_add_later(done);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    // The stroke was committed by the second clean rehearsal, not here: a DONE
    // that persists would store one for an owner who reached this screen by
    // skipping the rehearsal.
    finish();
}

// ---- the rehearsal canvas -------------------------------------------------
//
// One stroke at a time, over a printed reference word. The word is printed
// rather than drawn by the owner because the word is not what is being chosen,
// and a fixed box is the same measurement kiss_duress_classify gets at unlock:
// the bounding box of what came before the final stroke.
#define DR_PTS 128
static int s_rx[DR_PTS], s_ry[DR_PTS];
static int s_rn;
static bool s_rdown;
static lv_point_precise_t s_rpts[DR_PTS];

static void rehearse_reset(void)
{
    s_rn = 0;
    s_rdown = false;
    if (s_rink) lv_obj_add_flag(s_rink, LV_OBJ_FLAG_HIDDEN);
}

static void rehearse_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (!s_rdown) { s_rdown = true; s_rn = 0; }
    if (s_rn >= DR_PTS) return;
    // Same 10px dedupe kiss_word_ui uses. The classifier measures shape, and a
    // hundred samples of a stationary finger is not shape.
    if (s_rn == 0 || LV_ABS(p.x - s_rx[s_rn - 1]) >= 10 ||
                     LV_ABS(p.y - s_ry[s_rn - 1]) >= 10) {
        s_rx[s_rn] = p.x;
        s_ry[s_rn] = p.y;
        s_rpts[s_rn].x = p.x;
        s_rpts[s_rn].y = p.y;
        s_rn++;
        if (s_rn >= 2 && s_rink) {
            lv_line_set_points(s_rink, s_rpts, (uint32_t)s_rn);
            lv_obj_remove_flag(s_rink, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void rehearse_release_cb(lv_event_t *e)
{
    (void)e;
    if (!s_rdown) return;
    s_rdown = false;

    lv_area_t b;
    lv_obj_get_coords(s_word_box, &b);
    // The SAME call the unlock path makes, with the same argument order. A
    // rehearsal that judged the stroke by any other rule would certify a stroke
    // the device then refuses.
    const int got = kiss_duress_classify(s_rx, s_ry, s_rn,
                                         b.x1, b.y1, b.x2, b.y2);
    rehearse_reset();

    if (got != s_pick) {
        // Not a failure worth a screen of its own: the finger slipped. Say so
        // in place and let them go again, because the alternative is an owner
        // who gives up and leaves a stroke set that they cannot draw. The count
        // resets -- two consecutive, or it has not been learned.
        s_got = 0;
        if (s_rhint) lv_label_set_text(s_rhint, tr(STR_GD_DRAW_BAD_T));
        return;
    }

    if (++s_got < 2) {
        // The long form here, not ONCE MORE: this is the moment the owner is
        // told WHY there is a second one, and it is the same sentence that
        // answers "what if I forget it".
        if (s_rhint) lv_label_set_text(s_rhint, tr(STR_GD_DRAW_AGAIN_S));
        return;
    }
    // Twice, cleanly. Only now does anything persist.
    (void)kiss_duress_set(s_pick);
    stage_show(ST_DONE);
}

// Hand the display to the drawing module the way Settings does: drop this
// screen first, then open it with OUR done callback, so it returns to whoever
// opened the wizard rather than back into a stage that no longer exists.
static void word_cb(lv_event_t *e)
{
    (void)e;
    void (*done)(void) = s_done;
    s_done = NULL;                   // finish() must not fire it as well
    s_pending = -1;
    close_all();
    s_stage = ST_INTRO;
    kiss_word_ui_open(s_parent, done);
}

static void pick_cb(lv_event_t *e)
{
    s_pick = (int)(intptr_t)lv_event_get_user_data(e);
    s_got = 0;
    stage_show(ST_DRAW);
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
    // The rehearsal's three widgets belong to ST_DRAW's screen and to no other
    // stage. s_word_box is read UNGUARDED -- lv_obj_get_coords(s_word_box, &b)
    // in rehearse_release_cb -- so leaving it set across a stage change makes
    // that a dereference of freed memory rather than a skipped branch.
    s_word_box = s_rhint = s_rink = NULL;
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
        wt_chrome_head(s_scr);
        diagram_two_ways();
        wt_why_body(s_scr, tr(STR_GD_INTRO_B), 250, wt_primary(), false);
        // Three controls on the standard row. The TALL row and the group fit
        // existed for pill boxes whose labels had to wrap inside fixed widths;
        // the arrow actions are content-sized single lines at chrome23, so
        // the row is the 52px one every other screen uses.
        //
        // "SET UP A SPARE", not OK: on a screen explaining a decoy wallet, an
        // OK button tells the owner nothing about which of the two things is
        // about to happen. This one commits to the second wallet with words.
        wt_arrow_action(s_scr, tr(STR_GD_SET_UP_SPARE), false, true, 48,
                        WT_ACTION_Y, 240, false, next_cb, NULL);
        // The way back to plain behaviour, and it is the ESCAPE HATCH the
        // enforced stroke rests on: forgetting your stroke costs two taps
        // inside the spare, never your keys.
        //
        // UNCONDITIONAL, which it was not. It appeared only when a stroke was
        // set, so in a spare session its presence announced that one was --
        // and now that kiss_duress_route forks on kiss_duress_real() again,
        // that is a coerced owner's confession sitting in an action row. The
        // Settings row above it learned this exact lesson first
        // (kiss_settings.c, "the row's absence was the confession"); a control
        // that clears nothing is the cheapest possible way to say nothing.
        wt_arrow_action(s_scr, tr(STR_GD_TURN_OFF), false, false, 330,
                        WT_ACTION_Y, 220, false, turn_off_cb, NULL);
        wt_arrow_action(s_scr, tr(STR_GD_SKIP), true, false, WT_EXIT_X,
                        WT_ACTION_Y, 140, true, skip_cb, NULL);
        break;
    }
    case ST_FUND: {
        s_scr = wt_screen(s_parent, tr(STR_GD_FUND_T), NULL);
        wt_chrome_head(s_scr);
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
        wt_arrow_action(s_scr, tr(STR_GD_SET_UP_REAL), false, true, 48,
                        WT_ACTION_Y, 420, false, next_cb, NULL);
        wt_arrow_action(s_scr, tr(STR_GD_SKIP), true, false, WT_EXIT_X,
                        WT_ACTION_Y, 140, true, skip_cb, NULL);
        break;
    }
    // The model, confirmed once, before anything is configured. Every string
    // here already ships in 21 locales and every one of them is lifted from a
    // screen that teaches the SAME rule elsewhere -- the keys screen and the
    // write-it-down screen -- so the owner meets one rule twice rather than a
    // second rule once. Nothing new was authored for this screen.
    case ST_ACK: {
        s_scr = wt_screen(s_parent, tr(STR_L_WARN_T), NULL);
        wt_chrome_head(s_scr);
        lv_obj_t *row = wt_diagram_row(diagram_box(112));
        chip_icon(row, WT_ICON_SECRET, tr(STR_D_SPARE), false);
        wt_diagram_op(row, "+");
        chip_icon(row, WT_ICON_LOCK, tr(STR_D_PASSPHRASE), false);
        wt_diagram_op(row, LV_SYMBOL_RIGHT);
        chip_icon(row, WT_ICON_KEY, tr(STR_D_REAL), true);

        const int BY = 208, BW = 344, BH = WT_CONTENT_BOTTOM - BY;
        const char *b1 = tr(STR_L_FP_NOTE_NOPASS);   // seed words alone
        const char *b2 = tr(STR_W_WRITE_S);          // seed words + passphrase
        // _head, never a hand-subtracted budget: this screen has headings, and
        // measuring them is the difference between font23 and font14 here.
        const lv_font_t *f = wt_body_font2_head(tr(STR_D_SPARE), b1,
                                                tr(STR_D_REAL),  b2, BW - 14, BH);
        wt_why_block(s_scr, tr(STR_D_SPARE), b1,  48, BY, BW, BH, f, WT_WARN);
        wt_why_block(s_scr, tr(STR_D_REAL),  b2, 408, BY, BW, BH, f, wt_accent());

        // The same resolve the sign flow's cautions wear: a tick and the
        // words, nothing drawn around them. SKIP leaves, so it points back.
        wt_arrow_action(s_scr, tr(STR_GD_SKIP), true, false, 48, WT_ACTION_Y,
                        190, false, skip_cb, NULL);
        lv_obj_t *ack = wt_word_action(s_scr, LV_SYMBOL_OK,
                                       tr(STR_C_I_UNDERSTAND), true,
                                       wt_accent(), true, next_cb, NULL);
        lv_obj_align(ack, LV_ALIGN_TOP_RIGHT, -48, WT_ACTION_Y + 6);
        break;
    }
    // Six shapes, two rows of three, at the geometry the deleted screen used.
    // They are named rather than drawn because the names are what the rehearsal
    // then asks for, and a picture of an underline beside a picture of a strike
    // is two flat lines.
    case ST_PICK: {
        s_scr = wt_screen(s_parent, tr(STR_GD_PICK_REAL_T),
                          tr(STR_GD_PICK_REAL_S));
        wt_chrome_head(s_scr);
        // Each choice is a destination, so it wears the chevron the settings
        // rows already taught: word and arrow, nothing drawn around them.
        for (int g = WDG_UNDERLINE, i = 0; g < WDG_N; g++, i++) {
            const int key = kiss_duress_label_key(g);
            if (key < 0) continue;
            lv_obj_t *c = wt_word_action(s_scr, WT_ICON_ARR_R,
                                         tr((uint16_t)key), false, WT_INK,
                                         false, pick_cb, (void *)(intptr_t)g);
            lv_obj_set_pos(c, 48 + (i % 3) * 240, 134 + (i / 3) * 72);
        }
        // No body here, and no new key for one. What forgetting costs is said
        // where it is actually earned -- on the rehearsal, by GD_DRAW_AGAIN_S,
        // which is the sentence "so a slip now does not lock you out later" and
        // already ships. A paragraph on this screen would be a third telling of
        // a rule the subtitle and the next screen both make.
        wt_arrow_action(s_scr, tr(STR_GD_SKIP), true, false, WT_EXIT_X,
                        WT_ACTION_Y, 140, true, skip_cb, NULL);
        break;
    }
    case ST_DRAW: {
        s_scr = wt_screen(s_parent, tr(STR_GD_DRAW_T), tr(STR_GD_DRAW_S));
        wt_chrome_head(s_scr);
        // The shape being asked for, named, because by here the owner has left
        // the screen that named it.
        const int key = kiss_duress_label_key(s_pick);
        if (key >= 0)
            wt_lbl(s_scr, tr((uint16_t)key), 48, 104, wt_font23(), wt_accent());

        // The reference word. Its box is what kiss_duress_classify measures
        // against, so it is a real object with real coordinates and not a
        // painted decoration.
        s_word_box = wt_card(s_scr, 250, 150, 300, 96);
        lv_obj_t *w = wt_lbl(s_word_box, "KISS", 0, 0, wt_font34(), WT_INK);
        lv_obj_center(w);

        // The canvas sits OVER the word and takes the whole band, because a
        // stroke that has to start inside a 300px box is a stroke the owner
        // cannot draw. Built after the card so the press lands here.
        lv_obj_t *cv = lv_obj_create(s_scr);
        lv_obj_remove_style_all(cv);
        lv_obj_set_pos(cv, 0, 110);
        lv_obj_set_size(cv, 800, 200);
        lv_obj_add_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cv, rehearse_press_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(cv, rehearse_release_cb, LV_EVENT_RELEASED, NULL);

        // On the SCREEN at (0,0), not on the canvas. lv_line points are relative
        // to the line object's own origin, and the points fed to it come from
        // lv_indev_get_point, which is absolute -- so a line parented to a
        // canvas at y=110 draws every stroke 110px BELOW the finger. Reported
        // from the bench as the ink not following the finger at all. This is the
        // same parenting kiss_word_ui.c uses for exactly the same reason, and
        // the canvas above stays transparent so the ink sits over it.
        s_rink = lv_line_create(s_scr);
        lv_obj_set_pos(s_rink, 0, 0);
        lv_obj_set_style_line_width(s_rink, 6, 0);
        lv_obj_set_style_line_color(s_rink, wt_accent(), 0);
        lv_obj_set_style_line_rounded(s_rink, true, 0);
        lv_obj_add_flag(s_rink, LV_OBJ_FLAG_HIDDEN);

        // Empty until a stroke lands. GD_DRAW_AGAIN_S opens with "the same
        // stroke again", which is a lie before there has been a first one --
        // the subtitle already says what to do, and this line exists to react.
        s_rhint = wt_lbl(s_scr, "", 48, 286, wt_font23(), WT_MUT);
        lv_obj_set_width(s_rhint, 704);
        rehearse_reset();
        wt_arrow_action(s_scr, tr(STR_GD_SKIP), true, false, WT_EXIT_X,
                        WT_ACTION_Y, 140, true, skip_cb, NULL);
        break;
    }
    case ST_NOPASS: {
        s_scr = wt_screen(s_parent, tr(STR_GD_NOPASS_T), NULL);
        wt_chrome_head(s_scr);
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
        // Both widths and both pills are now FIXED, where they used to fork on
        // kiss_duress_real(). This screen is the one a spare session reaches,
        // so a pill that appears only when a stroke is configured tells a
        // prober that one is -- and the row that opens this screen was
        // un-hidden for precisely that reason. A layout that changes shape is
        // the same confession as a pill that comes and goes.
        wt_arrow_action(s_scr, tr(STR_L_CREATE_PASS_BTN), false, true, 48,
                        WT_ACTION_Y, 340, false, add_pass_cb, NULL);
        wt_arrow_action(s_scr, tr(STR_GD_TURN_OFF), false, false, 396,
                        WT_ACTION_Y, 208, false, turn_off_cb, NULL);
        wt_arrow_action(s_scr, tr(STR_C_OK), true, false, WT_BACK_X,
                        WT_ACTION_Y, 140, true, skip_cb, NULL);
        break;
    }
    default: {
        s_scr = wt_screen(s_parent, tr(STR_GD_DONE_T), NULL);
        wt_chrome_head(s_scr);
        // Same shape as ST_INTRO: the mapping is drawn, and the body keeps
        // only what the diagram cannot say — the stroke routes, it does not
        // unlock, and this is the last screen in the flow that says so.
        diagram_two_ways();
        wt_why_body(s_scr, tr(STR_GD_DONE_B), 250, WT_OK, true);
        // The drawing is offered HERE, at the end of the flow, and that is the
        // whole of the discoverability fix. kiss_word_ui_open had exactly one
        // caller in the shipped firmware -- a third pill on a Settings page --
        // so an owner setting up a spare was never once told that the four
        // letters in the diagram they are looking at can be replaced. Reported
        // from the bench as the drawing being missing; it was reachable, and
        // never offered anywhere the decision was being made.
        wt_arrow_action(s_scr, tr(STR_GD_WORD_PILL), false, false, 48,
                        WT_ACTION_Y, 380, false, word_cb, NULL);
        wt_arrow_action(s_scr, tr(STR_C_DONE), false, true, 552, WT_ACTION_Y,
                        200, true, save_cb, NULL);
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
