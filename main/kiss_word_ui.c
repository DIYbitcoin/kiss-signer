// See kiss_word_ui.h. Three acts: write it, write it again, hold to commit.
//
// The field is BLANK. The duress wizard prints a reference "KISS" for its
// modifier to sit on, because that stroke is measured against the word's box;
// here there is no box and nothing to sit on, so a printed word would teach a
// placement that does not exist in the game. What the owner rehearses here is
// exactly what has to work later: the shape of their own hand, twice.
//
// The second writing is not politeness. kiss_gword stores a picture of a
// movement, and the only question that matters is whether the owner can make
// the same picture again -- so the screen makes them prove it before anything
// is written to flash. A word that cannot survive being written twice in one
// sitting will not survive a cold morning in six months.
#include "kiss_word_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kiss_crypto.h"   // kiss_session_decoy: is there a passphrase at all
#include "kiss_gword.h"
#include "kiss_wipe.h"
#include "kiss_theme.h"
#include "i18n.h"

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);

enum { ST_WRITE = 0, ST_AGAIN, ST_CONFIRM, ST_DONE, ST_FAIL };
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
// Both numbers come from kiss_gword.h now. They used to be written here, and
// unlock's were written in main.c, and they did not agree: see the note beside
// GW_MAX_PTS for the word that could be saved and could never open anything.
#define DPTS     GW_MAX_PTS
#define DSTROKES GW_MAX_STROKES
static int     s_dx[GW_MAX_PTS], s_dy[GW_MAX_PTS];
static uint8_t s_did[GW_MAX_PTS];  // stroke id per point, as the game keeps
static int     s_dn;
static int     s_strokes;          // strokes STORED, never more than DSTROKES
static int     s_strokes_seen;     // strokes DRAWN, including the ones dropped
static bool    s_down;

static lv_obj_t *s_canvas;
static lv_obj_t *s_hint;
static lv_obj_t *s_line[DSTROKES];
// One pool, not a rectangle. s_ink was [12][384] -- 36KB of static internal
// SRAM permanently reserved so that every stroke could be the longest stroke,
// on a device where the whole word is bounded by GW_MAX_PTS points anyway.
// One pool of that size with a per-stroke offset holds exactly the same
// drawings in 3KB.
//
// Safe with lv_line because points are only ever APPENDED to the current
// (last) stroke: lv_line_set_points stores the POINTER, not a copy, and no
// earlier stroke's slice ever moves. draw_reset is the one place that has to
// be careful -- see the note there.
static lv_point_precise_t s_ink[DPTS];
static int s_inkoff[DSTROKES];     // where each stroke starts in the pool
static int s_inkn[DSTROKES];       // and how many points it has

static void draw_reset(void)
{
    s_dn = 0;
    s_strokes = 0;
    s_strokes_seen = 0;
    s_down = false;
    for (int i = 0; i < DSTROKES; i++) {
        s_inkn[i] = 0;
        s_inkoff[i] = 0;
        // Point the line at the pool with a count of zero, do not merely
        // clear the counter. lv_line KEEPS the pointer it was given, and with
        // one shared pool a stale pointer aims at bytes the next stroke is
        // about to write -- a hidden line is not drawn, but anything that
        // measures it (LV_SIZE_CONTENT self-sizing) reads another stroke's
        // points. With the old per-stroke rows the stale pointer was
        // harmless, which is exactly why this is easy to miss.
        if (s_line[i]) {
            lv_line_set_points(s_line[i], s_ink, 0);
            lv_obj_add_flag(s_line[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    // The raw gesture is an unlock credential. It used to sit in .bss until
    // the next word overwrote it, which on a device that enrols once is
    // forever.
    kiss_wipe(s_ink, sizeof s_ink);
    kiss_wipe(s_dx, sizeof s_dx);
    kiss_wipe(s_dy, sizeof s_dy);
    kiss_wipe(s_did, sizeof s_did);
}

static void ink_add(int x, int y)
{
    int s = s_strokes - 1;
    if (s < 0 || s >= DSTROKES || !s_line[s]) return;
    // A new stroke starts where the previous one ended. Only the last stroke
    // ever grows, so no earlier slice can be disturbed by this.
    if (s_inkn[s] == 0)
        s_inkoff[s] = s > 0 ? s_inkoff[s - 1] + s_inkn[s - 1] : 0;
    int at = s_inkoff[s] + s_inkn[s];
    if (at >= DPTS) return;              // the whole word's point budget
    s_ink[at].x = x;
    s_ink[at].y = y;
    s_inkn[s]++;
    if (s_inkn[s] >= 2) {
        lv_line_set_points(s_line[s], &s_ink[s_inkoff[s]], s_inkn[s]);
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
        s_strokes_seen++;
        if (s_strokes < DSTROKES) s_strokes++;
    }
    // Past the stroke budget the whole stroke is DROPPED, not folded into the
    // last one. Unlock reads the word as "every point whose stroke id is <= the
    // stored count" (written_word_match), so a thirteenth stroke merged into the
    // twelfth here becomes points unlock will never include -- the templates
    // differ and the word never opens the device again. Dropping it instead
    // leaves both sides holding the same first twelve strokes.
    if (s_strokes_seen > DSTROKES) return;
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
    kiss_wipe(s_ink, sizeof s_ink);
    kiss_wipe(s_dx, sizeof s_dx);
    kiss_wipe(s_dy, sizeof s_dy);
    kiss_wipe(s_did, sizeof s_did);
}

static void finish(void)
{
    close_all();
    void (*cb)(void) = s_done;
    s_done = NULL;
    if (cb) cb();
}

static void cancel_cb(lv_event_t *e) { (void)e; finish(); }

// Write it, then READ IT BACK. gw_stored_set returns 0 only once the write is
// committed, and both callers here threw that away: a full NVS partition, a bad
// sector or a cut between set and commit ended on a screen saying the change had
// been made. That is the one failure an owner cannot see for themselves -- the
// device keeps working, on the OLD word, until the day they need the new one.
//
// The read-back is what makes this worth doing. A return code says the write was
// accepted; only reading the slot says what is in it.
static bool store_word(const gw_template_t *t)
{
    if (gw_stored_set(t) != 0)
        return false;
    if (!t)
        return !gw_stored_any();
    gw_template_t back;
    return gw_stored_get(&back) && memcmp(&back, t, sizeof back) == 0;
}

static void back_to_cover_cb(lv_event_t *e)
{
    (void)e;
    // A clear that failed leaves the owner's letters opening the device while
    // they walk away believing KISS is back. Say so instead of finishing.
    if (!store_word(NULL)) {
        stage_show(ST_FAIL);
        return;
    }
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
    stage_show(store_word(&s_first) ? ST_DONE : ST_FAIL);
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
    const bool back_to_cover = (!again && gw_stored_any());
    wt_pill(s_scr, tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 140, cancel_cb, NULL);
    if (back_to_cover)
        wt_pill(s_scr, tr(STR_GD_WORD_BACK_T), 330, WT_ACTION_Y, 260,
                back_to_cover_cb, NULL);
    wt_pill(s_scr, tr(STR_GD_WORD_DONE), WT_ACT_X, WT_ACTION_Y, 260, done_cb, NULL);
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
    wt_pill(s_scr, tr(STR_C_CANCEL), WT_EXIT_X, WT_ACTION_Y, 140, cancel_cb, NULL);
    wt_hold_pill(s_scr, tr(STR_GD_WORD_HOLD), WT_ACT_X, WT_ACTION_Y, 330,
                 WT_ACTION_H, 2000, save_cb, NULL);
}

// The write did not take.
//
// The obvious borrow was the stroke wizard's rejection -- THAT WAS NOT IT, "try
// the stroke again, or go back and pick a different one" -- and rendering it
// killed it. Every word of that blames the owner for a draw they made
// correctly, on a screen that exists because the DEVICE refused a write. The
// storage failure's copy is the honest one: it names no culprit and its whole
// content is the fact that matters, which is that nothing changed.
//
// Reached from two places with opposite meanings -- a word that would not
// store, and a word that would not clear -- so the body has to be true of both.
// "nothing moved" is, for both. No new key: this is a rare hardware fault, and
// a screen an owner may never see is a poor reason to spend 21 locales.
static void fail_screen(void)
{
    s_scr = wt_screen(s_parent, tr(STR_C_TRY_AGAIN), NULL);
    wt_why_body(s_scr, tr(STR_G_STORAGE_FAIL_GENERIC_B), 150, WT_WARN, true);
    wt_pill(s_scr, tr(STR_C_OK), 552, WT_ACTION_Y, 200, cancel_cb, NULL);
}

// The two ways in, redrawn with the owner's own letters where KISS used to be.
// This screen was a title over a 704px paragraph -- BARE, and never rendered by
// anything: no walk stop reached it, so no gate ever had an opinion on it. The
// body says which gesture opens which wallet, and a gesture mapping is a
// picture. It is deliberately the same two rows kiss_duress_ui.c draws on the
// intro, so the fact the owner learned there survives the change of letters.
//
// The pencil, not a word, for what they just wrote: their letters are a shape,
// not a string this device can print, and the screen title is the antecedent.
// Costs no locale a single character.
static void done_screen(void)
{
    s_scr = wt_screen(s_parent, tr(STR_GD_WORD_OK_T), NULL);

    // No passphrase on this session means there is no second wallet for the
    // mark to route to: the letters alone open the funded one. The two rows
    // below would show them a SPARE they do not have, which is the promise an
    // owner would repeat to whoever is standing over them. The stroke wizard
    // already refuses to describe this signer that way (ST_NOPASS); this is the
    // same refusal on the other half of the same question.
    //
    // The letters still WORK -- that is what the title says and why this flow
    // is not blocked the way the stroke's is. Only the spare-and-real split is
    // untrue here, so the passphrase is named as the thing that is missing,
    // borrowing the chips and the body the stroke's own screen ships.
    if (kiss_session_decoy()) {
        // wt_diagram_row carries no position of its own, so it needs a placed
        // parent or it lands at 0,0 and sits on the title. Same column the
        // stroke's ST_NOPASS builds, at the same y, so the two screens an owner
        // meets from one Settings row are the same screen twice.
        lv_obj_t *col = lv_obj_create(s_scr);
        lv_obj_remove_style_all(col);
        lv_obj_set_pos(col, 48, 112);
        lv_obj_set_width(col, 704);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *row = wt_diagram_row(col);
        char nb[WT_ICON_TEXT_MAX];
        wt_icon_text(nb, sizeof nb, WT_ICON_LOCK, tr(STR_D_PASSPHRASE));
        wt_chip(row, nb, false);
        wt_diagram_op(row, LV_SYMBOL_RIGHT);
        wt_chip(row, tr(STR_GD_OFF), false);
        wt_why_body(s_scr, tr(STR_GD_NOPASS_B), 190, WT_WARN, true);
        wt_pill(s_scr, tr(STR_C_OK), 552, WT_ACTION_Y, 200, cancel_cb, NULL);
        return;
    }

    // The rows sit in a card, not loose on the page. Two chip rows are chrome
    // to a reader and were not to the BARE gate: oc_is_frame wants 100x30 and a
    // chip is about 40x28, so English passed only because its body wrapped to
    // two lines and never became a wall. de, fr, pl and ru wrapped to three and
    // the screen was reported bare with the diagram right there on it.
    lv_obj_t *card = wt_card(s_scr, 48, 118, 704, 96);
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_size(box, 704, 96);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    char buf[WT_ICON_TEXT_MAX];
    lv_obj_t *r1 = wt_diagram_row(box);
    wt_chip(r1, LV_SYMBOL_EDIT, false);
    wt_diagram_op(r1, LV_SYMBOL_RIGHT);
    wt_icon_text(buf, sizeof buf, WT_ICON_SECRET, tr(STR_D_SPARE));
    wt_chip(r1, buf, false);

    lv_obj_t *r2 = wt_diagram_row(box);
    wt_chip(r2, LV_SYMBOL_EDIT, false);
    wt_diagram_op(r2, "+");
    wt_chip(r2, tr(STR_GD_PICK_REAL_T), false);
    wt_diagram_op(r2, LV_SYMBOL_RIGHT);
    wt_icon_text(buf, sizeof buf, WT_ICON_KEY, tr(STR_D_REAL));
    wt_chip(r2, buf, true);

    wt_wraph(s_scr, tr(STR_GD_WORD_OK_B), 48, 228, 704, WT_CONTENT_BOTTOM - 228);
    // The corner, not centred at 300: one pill, and it is the way out.
    wt_pill(s_scr, tr(STR_C_OK), 552, WT_ACTION_Y, 200, cancel_cb, NULL);
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
    case ST_FAIL:    fail_screen();       break;
    }
}

void kiss_word_ui_open(lv_obj_t *parent, void (*done_cb2)(void))
{
    s_parent = parent ? parent : lv_screen_active();
    s_done = done_cb2;
    s_pending = -1;
    memset(&s_first, 0, sizeof s_first);
    stage_show(ST_WRITE);
}

bool kiss_word_ui_active(void) { return s_scr != NULL || s_pending >= 0; }
