// See kiss_fw_ui.h. Five screens: what is on the card, why there is nothing to
// take, what it costs to take it, the write, and how it ended.
//
// The subject of the chain is WHICH VERSION REPLACES WHICH, and it is drawn
// once -- fw_trade -- then reused on the offer, on the confirm and on the
// verdict. It used to be assembled by the reader out of a value card, a row
// labelled Version and the word "newer", which is three objects saying one
// thing and none of them saying it.
//
// Borderless, on the idioms KEYS and RECEIVE were rebuilt on: a fact is a line
// with a rule under it, an action is an arrow with no box, and a state is a
// lamp. There is no wt_card and no wt_value_card left on any of the five.
#include "kiss_fw_ui.h"

#include <stdio.h>
#include <string.h>

#include "kiss_fw.h"
#include "kiss_theme.h"
#include "kiss_panel.h"
#include "i18n.h"

#ifndef SIMULATOR
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);
static wfw_image_t s_img;
// The one object on this chain whose animation repeats for ever and is NOT
// freed by the screen going away in the ordinary path: writing_apply leaves it
// breathing, and install_now then blacks the framebuffers and blocks the LVGL
// task inside the flash write with the screen still up. Deleted before that,
// not after. The title cursor needs no such handle -- LVGL's destructor calls
// lv_anim_delete on every object it frees, so that one goes with the screen.
static lv_obj_t *s_band;

// The page under the header. x=48 like every action bar; the text lane is
// WT_LINE_PAD further in, so a trade block and a line row start on the same
// vertical.
#define FW_X       48
#define FW_W      704
#define FW_TXT_X  (FW_X + WT_LINE_PAD)
#define FW_BAND_Y  76
#define FW_HRULE_Y 105
#define FW_PANE_Y 120

// A line row is a font23 caption over a font28 value now, and 58 does not hold
// one: the kit measures the value at wt_line_val_y() + the value's own line
// height, which lands 4px past the old row's floor and clips the descenders.
// 76 is what KEYS and RECEIVE use, and matching it matters more than the
// drawing's 58 -- three screens whose rows are different heights read as three
// different components.
//
// Two of them plus the closing rule is 152px, so the last rule sits at 396 and
// the block starts at 240. That is 20px higher than the drawing puts it, and
// the caution line above moves the same 16 to keep its gap.
#define FW_ROW_H    76
#define FW_ROW1_Y   240
#define FW_ROW2_Y   (FW_ROW1_Y + FW_ROW_H + 4)
#define FW_CAUTION_Y 192

static void fw_screen(void);

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_band = NULL;
    if (s_done) s_done();
}

bool kiss_fw_ui_active(void) { return s_scr != NULL; }

// Deliberately NOT close_cb. That one hands control back to Settings, which is
// the right answer for BACK and exactly the wrong one here: the lock is taking
// the screen away, not returning from it, and rebuilding Settings under a
// locked device is the whole bug. s_parent is left alone so a write already
// committed can still draw its result.
void kiss_fw_ui_close(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_band = NULL;
    s_done = NULL;
}

// ---- motion ----------------------------------------------------------------

static void an_ty(void *v, int32_t y)  { lv_obj_set_style_translate_y(v, y, 0); }
static void an_opa(void *v, int32_t o) { lv_obj_set_style_opa(v, (lv_opa_t)o, 0); }
static void an_bgopa(void *v, int32_t o) { lv_obj_set_style_bg_opa(v, (lv_opa_t)o, 0); }
static void an_w(void *v, int32_t w)   { lv_obj_set_width(v, w); }

// Everything on these screens arrives the same way: up nine pixels and in.
static void fw_enter(lv_obj_t *o, int ms, int delay)
{
    if (!o) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, an_ty);
    lv_anim_set_values(&a, 9, 0);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_set_exec_cb(&a, an_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&a);
}

// A rule DRAWS rather than fades, left to right.
//
// By WIDTH, and not by the transform_scale_x the pivot in wt_line_rule was set
// up for. A transform makes LVGL render the object through an intermediate
// LAYER, and this build's LVGL heap is 128 KB -- the same as the device's.
// Four transformed rules on one screen took the free heap from 80 KB to 24 KB
// with the largest free block down to 3.5 KB, and the language picker two
// stops later then spun for ever inside a failed allocation, which is what
// LV_ASSERT_MALLOC does when it fails. Measured with lv_mem_monitor either
// side of the screen; with the transform gone the same screen gives the heap
// back in full.
//
// Growing the real width is safe for the gate that pivot was protecting:
// overlapcheck reads real positions, and every intermediate width of a rule is
// a SUBSET of its final one, which is already clear of everything.
//
// The target width is passed in rather than read back off the object. A rule
// is created and animated in the same breath, and lv_obj_get_width before the
// first layout pass returns 0 -- which animated every rule on the chain from
// nothing to nothing. Silent, and invisible to every gate: a rule that is
// never drawn is a rule nothing can collide with.
static void fw_rule_draw(lv_obj_t *r, int w, int delay)
{
    if (!r) return;
    lv_obj_set_width(r, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r);
    lv_anim_set_exec_cb(&a, an_w);
    lv_anim_set_values(&a, 0, w);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// A rule of the pane's own width, at the pane's own x. wt_line_rule owns the
// colour and the height, so a section break and a row's underline are the same
// object and cannot drift apart.
static lv_obj_t *fw_rule(int y) { return wt_line_rule(s_scr, FW_X, y, FW_W); }

// The same, drawing itself in.
static void fw_rule_in(int y, int delay)
{
    fw_rule_draw(fw_rule(y), FW_W, delay);
}

// ---- the header ------------------------------------------------------------

// Title, blinking cursor, and one mono band where KEYS and RECEIVE put their
// tab strip. FIRMWARE gets no tabs: it has one job, and a tab strip over a
// single task is ceremony.
//
// `accent` is the writing screen and nothing else. Everywhere else the band is
// a background fact -- what this device is running -- and a background fact in
// the accent would be competing with the thing the screen is about.
static void fw_head(const char *title, const char *band, bool accent)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_band = NULL;
    s_scr = wt_screen(s_parent, title, NULL);
    // The chrome head is the fit AND the cursor now: INK mono28 at 14, the
    // one header identity the whole device wears. The band and rule below
    // keep this chain's shipped rhythm.
    wt_chrome_head(s_scr);

    // wt_font14, not the mono face beside it. IoskeleyMono is built from
    // 0x20-0x7E and carries no FontAwesome at all, so a WT_ICON_* asked of it
    // draws LVGL's missing-glyph rectangle -- at the right size, in the right
    // place, on every screen. Same failure the tab marks hit on KEYS, and
    // invisible to every gate.
    lv_obj_t *m = wt_lbl(s_scr, WT_ICON_REPLACE, FW_X, FW_BAND_Y - 1,
                         wt_font14(), accent ? wt_accent() : WT_DIM);
    if (accent) lv_obj_add_flag(m, WT_FLAG_ACCENT);
    lv_obj_update_layout(m);

    lv_obj_t *l = wt_lbl(s_scr, band, FW_X + lv_obj_get_width(m) + 8, FW_BAND_Y,
                         wt_font14(), accent ? wt_accent() : WT_MUT);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    if (accent) lv_obj_add_flag(l, WT_FLAG_ACCENT);

    fw_rule(FW_HRULE_Y);
}

// The band on every screen that is not writing: what this device runs today.
static void fw_head_running(const char *title)
{
    // 224, not 64. The formatted result of a translated _FMT key is capped at
    // about 190 bytes by the generator, and the 64 byte version of this line
    // is the truncation the device compiler caught after a push.
    char band[224];
    snprintf(band, sizeof band, tr(STR_G_FW_RUNNING_FMT),
             kiss_fw_running_version());
    fw_head(title, band, false);
}

// ---- the trade -------------------------------------------------------------

// Which version replaces which, on one baseline. The subject of four of the
// five screens, so it is one object with a size and a verdict rather than
// three near-copies.
typedef struct {
    const char *from, *to;
    const lv_font_t *ff, *tf;
    lv_color_t fc, tc;
    bool strike;              // the version left behind, once it has been
    const char *joiner;       // an arrow, or the cross on a refusal
    lv_color_t jc;
    bool accent;              // the joiner and `to` follow the accent
    const char *lamp;         // the direction, as a state. NULL to omit
    lv_color_t lampc;
} fw_trade_t;

static lv_obj_t *fw_trade(int y, const fw_trade_t *t)
{
    lv_obj_t *box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, FW_TXT_X, y);
    lv_obj_set_size(box, FW_W - WT_LINE_PAD * 2,
                    lv_font_get_line_height(t->tf));
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    // Bottom aligned rather than top: the three faces have different line
    // boxes and it is the BASELINE they have to share, which is what a reader
    // sees as "these are the same kind of thing".
    int x = 0;
    lv_obj_t *f = wt_lbl(box, t->from, 0, 0, t->ff, t->fc);
    if (t->strike) lv_obj_set_style_text_decor(f, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    lv_obj_align(f, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_update_layout(f);
    x += lv_obj_get_width(f) + 18;

    lv_obj_t *j = wt_lbl(box, t->joiner, 0, 0, wt_font23(), t->jc);
    if (t->accent) lv_obj_add_flag(j, WT_FLAG_ACCENT);
    lv_obj_align(j, LV_ALIGN_BOTTOM_LEFT, x, -6);
    lv_obj_update_layout(j);
    x += lv_obj_get_width(j) + 18;

    lv_obj_t *to = wt_lbl(box, t->to, 0, 0, t->tf, t->tc);
    if (t->accent) lv_obj_add_flag(to, WT_FLAG_ACCENT);
    lv_obj_align(to, LV_ALIGN_BOTTOM_LEFT, x, 0);
    lv_obj_update_layout(to);
    x += lv_obj_get_width(to) + 24;

    // RECEIVE's lamp, unchanged: a direction is a STATE, and this device has
    // one object for a state. It replaces the row whose value was the word
    // "newer", which is an adjective pretending to be a fact.
    if (t->lamp) {
        lv_obj_t *dot = lv_obj_create(box);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, t->lampc, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(dot, 10, 0);
        lv_obj_set_style_shadow_color(dot, t->lampc, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_LEFT, x, -10);

        // The DOT above keeps the lamp colour; its word takes the accent.
        lv_obj_t *w = wt_lbl(box, t->lamp, 0, 0, wt_font14(),
                             wt_ink_for(t->lampc));
        lv_obj_set_style_text_letter_space(w, 2, 0);
        lv_obj_align(w, LV_ALIGN_BOTTOM_LEFT, x + 16, -4);
    }

    fw_enter(box, 280, 40);
    return box;
}

// ---- the claim pair --------------------------------------------------------

// The claim about THIS install on the first row, the one about power on the
// second. `bottom` is gone with the two wt_why_blocks it used to budget: a
// fact row is one line at a fixed rung, so there is no budget to get wrong,
// and the pair used to be the exact shape this file's history kept landing at
// font14 by hand-subtracting a heading's height from it.
//
// No fw_enter on the rows. The stagger existed to bring two blocks in behind
// the trade above them; two lines of text under a rule that already draws
// itself in do not need a second entrance, and wt_facts hands back a y rather
// than the labels.
static void fw_claims(int y, const char *lcap, const char *lval,
                      const char *lmark, lv_color_t lcol)
{
    wt_fact_t facts[2] = {
        { .cap = lcap, .val = lval, .icon = lmark, .icon_col = lcol },
        { .cap = tr(STR_G_FW_RISK_H), .val = tr(STR_G_FW_RISK_B),
          .icon = LV_SYMBOL_WARNING,
          .icon_col = WT_WARN },
    };
    wt_facts(s_scr, y, facts, 2);
}

// What the signature row's "?" answers: what a signature buys.
//
// ONE paragraph, and the title is the WORD. This card used to compose the
// signature claim with the power caution -- two paragraphs from the confirm
// screen, glued at runtime -- and titled itself "ecdsa + post quantum", which
// is a heading, not a name. When the caution shrank to a fact row's value the
// card inherited the fragment: "a minute, keep it plugged", alone, under a
// lower case title, on a card about cryptography.
//
// The caution belongs to the screen where the decision is; the standard's own
// names belong on the TECHNICAL line, which is where every other definition
// on this device puts them.
static void sig_row_help_cb(lv_event_t *e)
{
    (void)e;
    wt_explain_t x = {
        .title      = tr(STR_G_FW_ROW_SIG),
        .icon       = WT_ICON_LOCK,
        .cap        = tr(STR_G_FW_ON_CARD),
        .val        = s_img.version,
        .body       = tr(STR_G_FW_WHY_B),
        .term       = tr(STR_G_FW_WHY_H),
        .term_label = tr(STR_G_TECHNICAL),
        .ok_txt     = tr(STR_C_OK),
        .mode       = WT_BODY_PROSE,
    };
    wt_explain_open(s_scr, &x);
}

static void where_help_cb(lv_event_t *e)
{
    (void)e;
    wt_explain_t x = {
        .title  = tr(STR_G_FW_WHERE_H),
        .icon   = WT_ICON_SD,
        .body   = tr(STR_G_FW_WHERE_B),
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_BODY_PROSE,
    };
    wt_explain_open(s_scr, &x);
}

// The row's tap, without the arrow wt_line_row draws for a cb. Same press
// rail, same handler; only the second mark goes.
static void fw_row_tap(lv_obj_t *row, lv_event_cb_t cb)
{
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    wt_line_press(row);
}

// The "?" that says the row under it is a question with an answer. Placed off
// the CAPTION's measured width, so it stays beside the words in every locale
// instead of at a hand-picked x that only English lands on.
//
// A REAL CHIP, and the row's arrow is gone. It was a 19px wt_help_mark -- a
// sign, by its own contract, that takes no taps and leaves them to the row --
// beside a full-size arrow that opened the same explainer. Two controls, one
// action, and the bench read it exactly that way: "there is a tiny as question
// mark to tap but when seemingly tapped it just goes to the same place the
// arrow goes too". So there is one mark now, it is the 30px chip the rest of
// the device uses for "this is a question", and the whole row still opens it.
static void fw_mark_after_cap(lv_obj_t *row, lv_event_cb_t cb)
{
    lv_obj_t *cap = NULL;
    lv_obj_update_layout(row);
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, i);
        if (lv_obj_check_type(c, &lv_label_class) &&
            lv_obj_get_y(c) == WT_LINE_CAP_Y) { cap = c; break; }
    }
    if (!cap) return;
    lv_obj_update_layout(row);
    // Centred on the caption, measured rather than nudged. The chip is 30px
    // and the caption is whatever rung the kit is on this week -- it was
    // font14 when this was written and is font23 now, and a hand offset that
    // looked right against the first sat 8px high against the second.
    lv_obj_t *chip = wt_help_chip(row, 0, 0, wt_accent(), cb, NULL);
    lv_obj_set_pos(chip, WT_LINE_PAD + lv_obj_get_width(cap) + 10,
                   WT_LINE_CAP_Y + (lv_obj_get_height(cap) - 30) / 2);
}

// ---- 5. the verdict --------------------------------------------------------

#ifndef SIMULATOR
static void restart_cb(lv_event_t *e) { (void)e; esp_restart(); }
#else
static void restart_cb(lv_event_t *e) { close_cb(e); }
#endif

static void result_back_cb(lv_event_t *e)
{
    (void)e;
    fw_screen();
}

// READ THE CARD AGAIN, from a screen that is only there because reading it
// answered badly.
//
// Four of the six refusals below are about the card and not about the image on
// it: no card, no image, an image too big for the slot kept for it, a file that
// is not one of ours. Every one of them is fixed at the slot -- reseat it, put
// the release on it, swap it -- and the screen offered BACK to the section home
// and nothing else, with "open this screen again" written in the body as the
// route. A rescan IS reopening the screen: fw_screen re-runs kiss_fw_scan and
// rebuilds from the answer, which is exactly what the post-install failure
// screen has always done under the word BACK.
//
// Not on the other two. ALREADY RUNNING is not a fault and has nothing to try;
// a build with no key to check a signature with cannot be talked into having
// one by looking at the card a second time. A control that cannot change the
// answer is a control that teaches the owner their taps do nothing.
static void fw_rescan_cb(lv_event_t *e)
{
    (void)e;
    fw_screen();
}

static void result_screen(int rc)
{
    const bool ok = rc == WFW_OK;
    const char *body;

    if (ok) {
        body = tr(STR_G_FW_OK_B);
    } else {
        // Each failure gets its own sentence. "It did not work" on a screen
        // that just spent a minute writing to flash tells the owner nothing
        // about whether the device still boots, which is the only thing they
        // want to know.
        body = rc == WFW_ERR_REJECTED    ? tr(STR_G_FW_FAIL_SIG_B)
             : rc == WFW_ERR_PQ_REJECTED ? tr(STR_G_FW_FAIL_PQ_B)
             : rc == WFW_ERR_CARD_GONE   ? tr(STR_G_FW_FAIL_CARD_B)
             : rc == WFW_ERR_UNSIGNED    ? tr(STR_G_FW_UNSIGNED_B)
             :                             tr(STR_G_FW_FAIL_WRITE_B);
    }

    // The band says what is RUNNING, and on both paths that is still the old
    // firmware: a successful write makes the new slot bootable, it does not
    // start it. The handoff drawing put the new version here on success, which
    // would have the band contradicting the body's own "restart to run it" --
    // and this file already carries the account of what it cost the last time
    // a firmware screen asserted something that had not happened yet.
    fw_head_running(tr(ok ? STR_G_FW_OK_T : STR_G_FW_FAIL_T));
    lv_obj_set_style_text_color(wt_screen_title(s_scr),
                                ok ? wt_accent() : wt_ink_for(WT_WARN), 0);

    // The picture carries the verdict before the sentence does. On a failure
    // the version still in charge comes back to full ink and the one that did
    // not land greys out, so an owner who reads nothing still knows where they
    // stand.
    fw_trade_t t = {
        .from   = kiss_fw_running_version(),
        .ff     = wt_font_mono23(),
        .fc     = ok ? WT_DIM : WT_INK,
        .strike = ok,
        .joiner = ok ? LV_SYMBOL_RIGHT : LV_SYMBOL_CLOSE,
        .jc     = ok ? wt_accent() : wt_ink_for(WT_WARN),
        .to     = s_img.version,
        .tf     = wt_font_mono28(),
        .tc     = ok ? wt_accent() : WT_DIM,
        .accent = ok,
    };
    fw_trade(FW_PANE_Y + 4, &t);
    // 172, not 192. The claims below lost 62px to the slide's band, and the
    // 56px of dead glass between the trade row and the rule is where it comes
    // back from -- the pair is what this screen is for, and the air above it
    // was never carrying anything.
    fw_rule_in(164, 150);

    // The strings already carry a blank line between the verdict and what
    // becomes of the old firmware, so the split costs no key. A body with no
    // break simply has no tail.
    char head[256];
    const char *tail = strstr(body, "\n\n");
    if (tail) {
        size_t n = (size_t)(tail - body);
        if (n >= sizeof head) n = sizeof head - 1;
        memcpy(head, body, n);
        head[n] = 0;
        tail += 2;
    }
    // THE ONE FAILURE WITH A REMEDY, and it is not the card.
    //
    // The other three are about this device or this moment: the write stopped,
    // the card left, this build has no key. A signature that does not check out
    // is about the FILE, and it will not check out the tenth time either -- so
    // a screen saying only what happened invites the owner to reseat the card
    // and hold the slide again over the same bytes.
    //
    // ITS OWN LABEL, under the refusal and never inside it. Appended to the
    // paragraph above it made one 620x125 block, and a wrapping label that wide
    // and that tall on a screen with nothing framed is what the BARE gate is
    // for -- it caught this in English on the first run. Two claims in two
    // boxes is also the right shape regardless: what happened is not what to
    // do, and the second is the only line here the owner can act on.
    const bool remedy = rc == WFW_ERR_REJECTED || rc == WFW_ERR_PQ_REJECTED;
    const int tail_h = remedy ? 84 : WT_CONTENT_BOTTOM - 268;
    lv_obj_t *b = wt_note(s_scr, tail ? head : body, FW_TXT_X, 216, 620,
                          268 - 216 - 8);
    fw_enter(b, 240, 190);
    if (tail) {
        lv_obj_t *tl = wt_note(s_scr, tail, FW_TXT_X, 268, 620, tail_h);
        lv_obj_set_style_text_color(tl, WT_DIM, 0);
        fw_enter(tl, 240, 232);
    }
    if (remedy) {
        // MUT, not DIM. The sentence above it is a footnote about what did not
        // happen; this one is the step, and the two cannot read alike.
        lv_obj_t *fx = wt_note(s_scr, tr(STR_G_FW_FAIL_SIG_FIX), FW_TXT_X,
                               268 + tail_h + 8, 620,
                               WT_CONTENT_BOTTOM - (268 + tail_h + 8));
        lv_obj_set_style_text_color(fx, WT_MUT, 0);
        fw_enter(fx, 240, 262);
    }

    if (ok) {
        char act[WT_ICON_TEXT_MAX];
        wt_icon_text(act, sizeof act, LV_SYMBOL_REFRESH, tr(STR_G_FW_RESTART));
        wt_arrow_action(s_scr, act, false, true, WT_ACT_X, WT_ACTION_Y, 0,
                        false, restart_cb, NULL);
    } else {
        // The rescan the six refusals now carry, on the screen that has been
        // doing it all along under the word BACK. Naming it leaves the corner
        // free to be an exit, which is what the corner is everywhere else.
        wt_arrow_action(s_scr, tr(STR_C_TRY_AGAIN), false, true, WT_ACT_X,
                        WT_ACTION_Y, 0, false, result_back_cb, NULL);
    }
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
}

// ---- 4. writing ------------------------------------------------------------

// How long WRITING stays lit before the panel goes dark. One hold length, so
// the screen announcing the dark is on the glass for about as long as the hold
// that asked for it.
#define FW_LIT_MS 1200

// The write has exactly one channel out, and it is not this screen.
//
// This used to set a percent card and force a refresh for it. Neither did
// anything an owner could see: install_now blacks the framebuffers and then
// blocks inside kiss_fw_install for the whole write, so the card was dark
// from the first chunk to the last. Only the simulator, which writes no flash
// and has no framebuffer to starve, ever watched it count. The card is gone;
// the light band says what the light is going to do instead.
//
// What is left runs BETWEEN esp_ota_write calls, with the cache back, and
// touches a PWM register. Keeping this function to exactly that makes it
// structurally impossible for LVGL work to land inside a cache off window.
static void progress_cb(int pct, void *ud)
{
    (void)ud;
    kiss_backlight_level(pct);
#ifdef ESP_PLATFORM
    // The light is the indicator the owner sees; this line is the indicator
    // the bench sees. The task is blocked but the UART is not, so the next
    // device test reports numbers instead of adjectives.
    static int last_decade = -1;
    if (pct / 10 != last_decade) {
        last_decade = pct / 10;
        ESP_LOGI("fw", "write %d%%", pct);
    }
#endif
}

// A firmware write starves the DSI panel's framebuffer read, so install_now
// blacks the glass and drives the backlight instead: a dark slab whose light
// climbs back to full as the bytes land. That is the right behaviour and it
// looks exactly like a device that died. This band is the difference, and it
// is the sentence STR_G_FW_DARK_B makes, drawn: a wash that breathes, with the
// claim inside it.
//
// Two stops, not the three the drawing shows. LV_GRADIENT_MAX_STOPS is 2 in
// this build and raising it is a memory decision for every gradient on the
// device, which is not a trade a wash on one screen gets to make.
static void fw_light_band(int y)
{
    lv_obj_t *b = lv_obj_create(s_scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, FW_X, y);
    lv_obj_set_size(b, FW_W, 58);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, WT_INK, 0);
    lv_obj_set_style_bg_grad_color(b, WT_INK, 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
    // bg_opa, and not only the two stop opacities. remove_style_all leaves
    // bg_opa at TRANSP, and LVGL skips the whole background draw before it
    // ever looks at a gradient -- so this band was an invisible rectangle
    // breathing invisibly for as long as it has existed, and the frame proved
    // it: every pixel of the band's 704x58 read exactly the page background.
    // Nothing in the source looked wrong, which is the argument for the
    // picture rule.
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(b, 26, 0);
    lv_obj_set_style_bg_grad_opa(b, 5, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);

    // font23 in a 58px band, which had room for it the whole time. This is a
    // SENTENCE -- it is the one instruction on a screen that goes dark for a
    // minute, and it was set at font14 with 4px of letter spacing, which is
    // the treatment a caption lane gets. Nothing an owner has to read is
    // font14. Spacing drops to 2 because font23 does not need the width and
    // the string has to stay inside the band.
    lv_obj_t *l = wt_lbl(b, tr(STR_G_FW_BACKLIGHT), WT_LINE_PAD, 16,
                         wt_font23(), WT_MUT);
    lv_obj_set_style_text_letter_space(l, 2, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, b);
    // The BACKGROUND's opacity, not the object's. Object opa scales the
    // children too, and in the drawing the caption is the band's SIBLING and
    // holds still while the wash moves -- a line of type fading in and out is
    // the one thing on this screen an owner might try to read. It also keeps
    // LVGL off the layer path: a 704x58 layer is 163KB against a 128KB heap,
    // which is the same wall the rules hit when they animated by transform.
    lv_anim_set_exec_cb(&a, an_bgopa);
    lv_anim_set_values(&a, 64, 191);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    s_band = b;
}

// The write itself, one LVGL tick after the screen that announces it.
//
// Deferred rather than called inline, for two reasons that happen to be the
// same reason. On the device, an inline call blocks the LVGL task from the
// moment the hold completes, so the writing screen would only appear because
// lv_refr_now forces it out mid erase -- the screen would be painted by the
// work rather than before it. And the screen walk saves a frame between
// pump() calls, so a screen that is drawn and replaced inside one callback is
// a screen no gate can ever measure: WRITING was the one stop overlapcheck
// could not reach.
static void install_now(lv_timer_t *t)
{
    lv_timer_delete(t);

    // Before anything below touches the panel. The band's animation repeats
    // for ever and the next few hundred milliseconds are spent with the
    // framebuffers black and the LVGL task blocked inside a flash write; a
    // repeating lv_anim outliving its object is the only use-after-free this
    // design can create.
    if (s_band) { lv_anim_delete(s_band, NULL); s_band = NULL; }

    // The write cannot be drawn, so the light is the whole indicator.
    //
    // Black both framebuffers first: the DMA still starves for every
    // esp_ota_write, and this is what it starves ON. Black where the panel
    // expected black is the difference between a torn screen and a dark one.
    // Then the backlight climbs from a floor to full as the bytes land, and a
    // slab that comes back to light IS the progress bar.
    //
    // Order on the way out matters. Build the result and paint it while the
    // panel is still dim, THEN raise the light, or the owner gets a full
    // brightness flash of the black we just wrote.
    kiss_panel_black();
#ifdef ESP_PLATFORM
    // Dim as one deliberate motion, not a cut to black: a screen that dies
    // in a frame reads as a crash, a light that runs down reads as a start.
    // ~200ms, blocking the LVGL task we own anyway.
    for (int p = 100; p >= 0; p -= 5) {
        kiss_backlight_level(p);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    kiss_backlight_level(0);
#endif
    int rc = kiss_fw_install(&s_img, progress_cb, NULL);
    result_screen(rc);
    lv_refr_now(NULL);
    kiss_backlight_set(1);
}

static void writing_apply(void *ud)
{
    (void)ud;
    // The one screen where the band is not a background fact: it names the
    // version being written, in the accent, because that is what is happening
    // rather than what is true.
    char band[224];
    snprintf(band, sizeof band, tr(STR_G_FW_WRITING_FMT), s_img.version);
    fw_head(tr(STR_G_FW_WRITING_T), band, true);
    // No BACK and no CANCEL: past this point the receiving slot is being
    // erased, and the honest options are finish or lose power, neither of
    // which is a button.
    fw_light_band(156);
    fw_claims(230, tr(STR_G_FW_DARK_H), tr(STR_G_FW_DARK_B),
              LV_SYMBOL_EYE_CLOSE, wt_accent());

    // Lit long enough to be read, then dark. This was 30 ms, which is two
    // frames: the screen that tells an owner the panel is about to go dark was
    // itself never on the panel long enough to see. One hold length is the
    // right unit, since a hold is what they just did to get here.
    lv_timer_t *t = lv_timer_create(install_now, FW_LIT_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// ---- 3. confirm ------------------------------------------------------------

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    fw_screen();
}

static void confirm_screen(void)
{
    bool down = s_img.cmp < 0;
    fw_head_running(tr(STR_G_FW_CONFIRM_T));

    // The trade again, smaller. No caption and no lamp: the lamp's job was
    // done on the offer, and repeating a state the owner has already read and
    // acted on is noise on the screen where they are about to commit.
    fw_trade_t t = {
        .from   = kiss_fw_running_version(),
        .ff     = wt_font_mono23(),
        .fc     = WT_DIM,
        .joiner = LV_SYMBOL_RIGHT,
        .jc     = wt_accent(),
        .to     = s_img.version,
        .tf     = wt_font_mono28(),
        .tc     = wt_accent(),
        .accent = !down,
    };
    fw_trade(FW_PANE_Y, &t);
    fw_rule_in(192, 150);

    // Two claims, not one paragraph. A downgrade swaps the left block for the
    // one that says so and turns its rule amber: on that path the interesting
    // claim is not how the check works but that this goes backwards.
    // The signature row says WHAT was checked; its long form is behind the
    // "?" on the offer, in the card sig_row_help_cb builds. A downgrade swaps
    // it for the claim that this goes backwards, and takes the amber mark.
    // 210, not the 186 the blocks started at: the rule fw_rule_in draws at
    // 192 ran straight through the first row's caption. A block began with a
    // heading and its body cleared the rule; a row IS its first line.
    fw_claims(210,
              tr(down ? STR_G_FW_DOWN_H : STR_G_FW_ROW_SIG),
              tr(down ? STR_G_FW_DOWN_B : STR_G_FW_WHY_H),
              down ? LV_SYMBOL_WARNING : LV_SYMBOL_OK,
              down ? WT_WARN : wt_accent());

    // 1500 ms, the same as the storage move. The pill is gone and the progress
    // runs along a rule under the label instead, which is why this sits on
    // WT_ACTION_Y rather than the tall bar: the tall bar existed to give a fat
    // pill room.
    wt_slide_rule(s_scr, tr(STR_G_FW_HOLD), tr(STR_G_FW_KEEP_HOLDING),
                  WT_ACT_X, WT_ACTION_Y_SLIDE, 330, writing_apply, NULL);
    wt_arrow_action(s_scr, tr(STR_C_CANCEL), true, false, 592, WT_ACTION_Y, 160,
                    true, confirm_cancel_cb, NULL);
}

static void install_cb(lv_event_t *e)
{
    (void)e;
    confirm_screen();
}

// ---- 2. nothing to install -------------------------------------------------

// All six refusals share one shape, so the page never rearranges itself around
// which thing went wrong.
//
// Exactly one of them is not a fault. "Already running" means the owner did
// everything right and there is nothing to do, and it wears the accent and a
// tick where the other five wear WT_WARN and a warning triangle. Painting a
// non-problem in the caution colour is the thing this redesign exists to fix.
static void nothing_to_install(int rc)
{
    const char *h, *b, *glyph;
    lv_color_t col;
    switch (rc) {
    case WFW_ERR_NO_CARD:  h = tr(STR_G_FW_NOCARD_H);   b = tr(STR_G_FW_NOCARD_B);   break;
    case WFW_ERR_NO_FILE:  h = tr(STR_G_FW_NOFILE_H);   b = tr(STR_G_FW_NOFILE_B);   break;
    case WFW_ERR_TOO_BIG:  h = tr(STR_G_FW_BIG_H);      b = tr(STR_G_FW_BIG_B);      break;
    case WFW_ERR_UNSIGNED: h = tr(STR_G_FW_UNSIGNED_H); b = tr(STR_G_FW_UNSIGNED_B); break;
    case WFW_ERR_SAME:     h = tr(STR_G_FW_SAME_H);     b = tr(STR_G_FW_SAME_B);     break;
    default:               h = tr(STR_G_FW_BAD_H);      b = tr(STR_G_FW_BAD_B);      break;
    }
    if (rc == WFW_ERR_SAME) {
        // LV_SYMBOL_OK, and not the info disc the drawing asks for: U+F05A is
        // not in SYMS and the fonts are not being rebuilt for it. A tick is
        // honest here in a way it is not on the offer -- the scan HAS run and
        // its answer is "you already have this" -- and it is the one mark on
        // the six that is not a warning.
        glyph = LV_SYMBOL_OK;
        col   = wt_accent();
    } else {
        glyph = LV_SYMBOL_WARNING;
        col   = WT_WARN;
    }

    // The GLYPH keeps the warning colour and the HEADLINE takes the accent:
    // amber is a mark colour, and these six screens are read.
    lv_obj_t *g = wt_lbl(s_scr, glyph, FW_TXT_X, 124, wt_font23(), col);
    if (rc == WFW_ERR_SAME) lv_obj_add_flag(g, WT_FLAG_ACCENT);
    lv_obj_update_layout(g);
    lv_obj_t *hd = wt_lbl(s_scr, h, FW_TXT_X + lv_obj_get_width(g) + 14, 118,
                          wt_font28(), wt_ink_for(col));
    lv_obj_add_flag(hd, WT_FLAG_ACCENT);
    fw_enter(g, 280, 40);
    fw_enter(hd, 280, 40);

    lv_obj_t *bd = wt_note(s_scr, b, FW_TXT_X, 176, 640, 270 - 176 - 8);
    lv_obj_set_style_text_color(bd, WT_MUT, 0);
    fw_enter(bd, 260, 120);

    fw_rule_in(270, 150);

    // The remedy, as a line you can tap rather than a second paragraph. The
    // screen this replaces printed the whole "where the file goes" explainer
    // beside the refusal, which is the wall of text its own comment said it
    // was trying to avoid.
    // No cb to wt_line_row: the arrow it would draw says the same thing the
    // "?" beside the caption already says, and one action never gets two
    // marks. The row still takes the tap -- wt_line_press and the handler are
    // added by hand below the chip.
    lv_obj_t *row = wt_line_row(s_scr, FW_X, 282, FW_W, FW_ROW_H,
                                tr(STR_G_FW_WHERE_CAP),
                                tr(STR_G_FW_WHERE_SHORT), wt_font23(), WT_INK,
                                NULL, NULL, NULL, NULL);
    fw_row_tap(row, where_help_cb);
    fw_mark_after_cap(row, where_help_cb);
    fw_enter(row, 260, 190);
    fw_rule_in(282 + FW_ROW_H, 300);
}

// ---- 1. the offer ----------------------------------------------------------

static void fw_screen(void)
{
    int rc = kiss_fw_scan(&s_img);
    // A build that cannot check a signature has nothing to say about whatever
    // is on the card, so that answer outranks the scan's.
    if (kiss_fw_available() != WFW_OK)
        rc = WFW_ERR_UNSIGNED;

    fw_head_running(tr(STR_G_FW_T));

    const bool installable = (rc == WFW_OK || rc == WFW_ERR_OLDER);
    if (!installable) {
        nothing_to_install(rc);
        if (rc != WFW_ERR_SAME && rc != WFW_ERR_UNSIGNED)
            wt_arrow_action(s_scr, tr(STR_C_TRY_AGAIN), false, true, WT_ACT_X,
                            WT_ACTION_Y, 0, false, fw_rescan_cb, NULL);
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y,
                        160, true, close_cb, NULL);
        return;
    }

    const bool down = s_img.cmp < 0;

    // The caption over the trade, and the only thing above it on the screen.
    lv_obj_t *cap = wt_lbl(s_scr, tr(STR_G_FW_ON_CARD), FW_TXT_X, FW_PANE_Y,
                           wt_font14(), wt_accent());
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_add_flag(cap, WT_FLAG_ACCENT);
    fw_enter(cap, 280, 40);

    fw_trade_t t = {
        .from   = kiss_fw_running_version(),
        .ff     = wt_font_mono28(),
        .fc     = WT_DIM,
        .joiner = LV_SYMBOL_RIGHT,
        .jc     = wt_accent(),
        .to     = s_img.version,
        .tf     = wt_font_mono28(),
        .tc     = wt_accent(),
        .accent = !down,
        .lamp   = tr(down ? STR_G_FW_OLDER_LAMP : STR_G_FW_NEWER_LAMP),
        .lampc  = down ? WT_WARN : WT_OK,
    };
    fw_trade(144, &t);

    // The one thing wrong with this offer, on one full width line.
    //
    // Two facts compete for it and only one can be true at a time in practice:
    // that the card is behind what is running, and that the scan only opened
    // some of the .bin files on the card. The narrowed one wins when both are
    // there, because the lamp above already says OLDER in amber and nothing
    // else on the screen can say the answer came from a SUBSET.
    //
    // The narrowed sentence is the one the PSBT list uses when the card
    // outruns ITS window -- identical fact, identical remedy, and already in
    // 21 locales. It needs the whole lane: at 300px, which is what a line
    // row's sub gets, it ellipsises after "showing 24 of 31 files. r" and
    // loses the half an owner can act on. That was a fixed bug once already.
    //
    // The downgrade sentence is only the SECOND clause of STR_G_FW_DOWN_B --
    // the first ("this card is older than what is running") is what the lamp
    // says. Pinned to one line either way: at two it reaches the section rule,
    // and the German string runs to three and lands inside the first row.
    char more[224];
    const bool narrowed = s_img.on_card > s_img.examined;
    if (narrowed)
        snprintf(more, sizeof more, tr(STR_S_FILES_MORE_FMT),
                 s_img.examined, s_img.on_card);
    if (narrowed || down) {
        lv_obj_t *n = wt_lbl(s_scr, narrowed ? more : tr(STR_G_FW_DOWN_SHORT),
                             FW_TXT_X, FW_CAUTION_Y, wt_font23(),
                             wt_ink_for(WT_WARN));
        lv_obj_set_width(n, FW_W - WT_LINE_PAD * 2);
        lv_obj_set_height(n, lv_font_get_line_height(wt_font23()));
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        fw_enter(n, 240, 40);
    }

    fw_rule_in(FW_ROW1_Y - 12, 150);

    // The file, and how big it is. No arrow: this line opens nothing, and an
    // arrow on it would promise a screen that does not exist.
    char sz[24];
    wt_fmt_bytes(s_img.size, sz, sizeof sz);
    lv_obj_t *r1 = wt_line_row(s_scr, FW_X, FW_ROW1_Y, FW_W, FW_ROW_H,
                               tr(STR_G_FW_ROW_FILE), s_img.name,
                               wt_font_mono28(), WT_INK, sz, NULL, NULL, NULL);
    fw_enter(r1, 260, 190);
    fw_rule_in(FW_ROW1_Y + FW_ROW_H, 300);

    // A PROMISE, not a verdict, because at this point nothing has checked
    // anything. rc comes from kiss_fw_scan, which reads the descriptor --
    // version, size, name -- and from kiss_fw_available, which asks only
    // whether this BUILD holds a key at all. The image's own signature is
    // checked by esp_ota_end at WRITE time, a screen and a hold-to-install
    // later, and the code for "it did not check out" is WFW_ERR_REJECTED,
    // which cannot exist yet at this point in the flow.
    //
    // It used to read "Signature: checked here" under a tick in WT_SEV_OK
    // green, and every translator took that for a completed pass:
    // "verifie ici", "verificado aqui", and Japanese, which is explicitly
    // "already verified". Twenty-one locales asserting a check that had not
    // run, on the screen where the owner decides whether to replace the
    // firmware that holds their keys. So: no tick, no green, no severity
    // colour, and a value that says WHEN rather than whether.
    //
    // The "?" is the answer to the row's own question. A new user meeting
    // "checked before anything is written" here has nowhere else to learn what
    // a signature buys them, and the two paragraphs that explain it are
    // already translated on the confirm screen.
    lv_obj_t *r2 = wt_line_row(s_scr, FW_X, FW_ROW2_Y, FW_W, FW_ROW_H,
                               tr(STR_G_FW_ROW_SIG), tr(STR_G_FW_SIG_PROMISE),
                               wt_font28(), WT_INK, NULL, NULL, NULL, NULL);
    fw_row_tap(r2, sig_row_help_cb);
    fw_mark_after_cap(r2, sig_row_help_cb);
    fw_enter(r2, 260, 232);
    fw_rule_in(FW_ROW2_Y + FW_ROW_H, 342);

    // INSTALL is safe in the primary slot because install_cb only opens the
    // confirm. BACK keeps the corner in both branches: whether there is an
    // image on the card is not something the owner decides on the way in, so a
    // BACK that moved between visits to the same screen would be the fault
    // this whole pass removes.
    wt_arrow_action(s_scr, tr(STR_G_FW_INSTALL), false, true, WT_ACT_X,
                    WT_ACTION_Y, 0, false, install_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
}

void kiss_fw_ui_open(lv_obj_t *parent, void (*done_cb)(void))
{
    // Drop a screen still up rather than forgetting the pointer to it. In the
    // product this is always NULL -- close_cb deletes on the way out -- but
    // the walk opens this screen ten times in a row without closing it, and
    // every one of those used to stay parented, invisible, under the next.
    // Two symptoms, both real: the LVGL heap ran down about 10 KB per open,
    // and act_for started reporting two controls saying INSTALL, which is a
    // walk one tap away from pressing the wrong screen's button.
    if (s_scr) lv_obj_delete(s_scr);
    s_scr = NULL;
    s_band = NULL;
    s_parent = parent;
    s_done = done_cb;
    fw_screen();
}
