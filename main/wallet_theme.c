// Shared wallet UI kit. See wallet_theme.h. Every builder here matches the
// house style the screens shipped with, so porting a screen to the kit must
// not change a rendered pixel while the accent is MONO.
#include "wallet_theme.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "kiss_fonts.h"

// Object identity, stamped into user_data. The addresses are what matter, not
// the strings: they let action_bar_ensure tell a screen built by wt_screen from
// an explainer card that happens to be the same size, and find the one bar it
// already made without keeping a static pointer that a screen teardown would
// leave dangling.
static const char WT_SCREEN_TAG[] = "wt_screen";
static const char WT_BAR_TAG[]    = "wt_action_bar";
static const char WT_TITLE_TAG[]  = "wt_title";
static const char WT_SUB_TAG[]    = "wt_subtitle";

// Find one of wt_screen's own children by its tag. By tag and not by index:
// the subtitle is only child 1 on the screens that HAVE a subtitle, and on the
// ones that do not, child 1 is whatever the screen built first.
static lv_obj_t *wt_tagged(lv_obj_t *scr, const char *tag)
{
    if (!scr) return NULL;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (lv_obj_get_user_data(c) == (void *)tag) return c;
    }
    return NULL;
}

// Each composite starts with Montserrat for ASCII, symbols, and Latin text,
// then falls back directly to the active locale's regional CJK font. A single
// ja -> ko -> zh chain would render shared Han codepoints with whichever font
// appeared first, mixing Japanese glyph forms into Simplified Chinese.
static lv_font_t s_font14[I18N_FC_ZH + 1];
static lv_font_t s_font23[I18N_FC_ZH + 1];
static lv_font_t s_font28[I18N_FC_ZH + 1];
// 34 exists for Latin/Cyrillic ONLY -- the CJK subsets at this size would add
// ~4.5MB to an app already using 8.9MB of a 12MB partition, and CJK glyphs read
// considerably larger than Latin at the same pixel size anyway. Hence a single
// face, not an array: there is deliberately no per-class variant to pick.
static lv_font_t s_font34;
static bool s_fonts_ready;

static void fonts_init(void)
{
    if (s_fonts_ready) return;
    for (int i = 0; i <= I18N_FC_ZH; i++) {
        s_font14[i] = font_kiss_lat14;
        s_font23[i] = font_kiss_lat23;
        s_font28[i] = font_kiss_lat28;
    }
    s_font14[I18N_FC_JA].fallback = &font_kiss_ja14;
    s_font14[I18N_FC_KO].fallback = &font_kiss_ko14;
    s_font14[I18N_FC_ZH].fallback = &font_kiss_zh14;
    s_font23[I18N_FC_JA].fallback = &font_kiss_ja23;
    s_font23[I18N_FC_KO].fallback = &font_kiss_ko23;
    s_font23[I18N_FC_ZH].fallback = &font_kiss_zh23;
    s_font28[I18N_FC_JA].fallback = &font_kiss_ja28;
    s_font28[I18N_FC_KO].fallback = &font_kiss_ko28;
    s_font28[I18N_FC_ZH].fallback = &font_kiss_zh28;
    // Chains to the 28px Japanese face, the largest CJK size that exists. A
    // glyph missing from an LVGL font is an infinite loop in the renderer, not
    // a tofu box, so this must never dead-end -- even though wt_font34() is
    // supposed to keep CJK locales away from this face entirely. Belt and
    // braces, because the failure mode is a hung device.
    s_font34 = font_kiss_lat34;
    s_font34.fallback = &font_kiss_ja28;
    s_fonts_ready = true;
}

static int font_class_for_lang(int lang)
{
    int fc = i18n_lang_info(lang)->font_class;
    return (fc >= I18N_FC_LAT && fc <= I18N_FC_ZH) ? fc : I18N_FC_LAT;
}

const lv_font_t *wt_font14_for_lang(int lang)
{
    fonts_init();
    return &s_font14[font_class_for_lang(lang)];
}

const lv_font_t *wt_font14(void) { return wt_font14_for_lang(i18n_get_lang()); }
const lv_font_t *wt_font23(void)
{
    fonts_init();
    return &s_font23[font_class_for_lang(i18n_get_lang())];
}
const lv_font_t *wt_font28(void)
{
    fonts_init();
    return &s_font28[font_class_for_lang(i18n_get_lang())];
}

// The top rung, for page titles and primary buttons. Latin/Cyrillic locales
// get the real 34px face; every CJK locale gets 28 instead, because no CJK
// face exists at 34 and handing a Latin-only font to a locale whose every
// string is CJK would put the renderer on the fallback path for the whole
// screen. 28 is not a downgrade there: Han and Kana fill their em box far more
// than Latin does, so a 28px CJK title already reads about as large as a 34px
// Latin one.
const lv_font_t *wt_font34(void)
{
    fonts_init();
    return font_class_for_lang(i18n_get_lang()) == I18N_FC_LAT
             ? &s_font34
             : &s_font28[font_class_for_lang(i18n_get_lang())];
}

const lv_font_t *wt_body_font(const char *txt, int w, int max_h)
{
    if (!txt || !*txt)
        return wt_font28();
    lv_point_t sz;
    // Three rungs, not two. This used to fall straight from 28 to 14, so one
    // row of overflow cost a reader 64% of the glyph size for nothing. 23 is
    // the same face at line height 29 and takes most of what 28 cannot.
    // Measured per locale: the same sentence is far taller in ja/ko/zh, and a
    // Cyrillic or Vietnamese translation often runs 40% longer than the English.
    lv_text_get_size(&sz, txt, wt_font28(), 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y <= max_h)
        return wt_font28();
    lv_text_get_size(&sz, txt, wt_font23(), 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y <= max_h)
        return wt_font23();
    return wt_font14();
}

static int s_accent = WT_ACC_MONO;

static const uint32_t ACC_HEX[WT_ACC_N] = {
    0xE8EEF7,   // MONO: same as WT_INK, the shipped look
    0x35D07F,   // GREEN (matches the home art dot)
    0xFF3EA5,   // CYPHERPINK
    0xFF8A3D,   // ORANGE
};
static const uint32_t ACC_BG_HEX[WT_ACC_N] = {
    0x232E42,   // MONO: cool ink glass, bright enough that "selected" is obvious
    0x102417,   // GREEN
    0x2A1020,   // CYPHERPINK
    0x2B190D,   // ORANGE
};
static const uint32_t ACC_PRESS_HEX[WT_ACC_N] = {
    0x33405A,
    0x173823,
    0x3A1730,
    0x3A2513,
};

void wt_accent_set(int id) { s_accent = (id >= 0 && id < WT_ACC_N) ? id : WT_ACC_MONO; }
int  wt_accent_get(void)   { return s_accent; }
lv_color_t wt_accent(void) { return lv_color_hex(ACC_HEX[s_accent]); }
lv_color_t wt_primary(void) { return wt_accent(); }
lv_color_t wt_accent_bg(void) { return lv_color_hex(ACC_BG_HEX[s_accent]); }
const char *wt_accent_name(void)
{
    static const char *NM[WT_ACC_N] = {"MONO", "GREEN", "CYPHERPINK", "ORANGE"};
    return NM[s_accent];
}
lv_color_t wt_accent_pressed(void) { return lv_color_hex(ACC_PRESS_HEX[s_accent]); }

// largest of {23, 14} that fits (defined with wt_note); used by the subtitle too
static const lv_font_t *note_font(const char *txt, int w, int max_h);

#define SUB_ROW_H 22        // font14 line + breathing room, for two-line pills
#define SUB_ROW_H23 35      // the same row when the second line is a readable 23

// ---- pill labels ----
// A control's label is never smaller than the prose that explains it. Notes cap
// at 23, so pills start there: at font14 a button sat below its own caption and
// read as an afterthought, which was worst exactly where it mattered most (HOLD
// TO SIGN under a 40px amount). The screen's ONE primary action goes to 28.
//
// Letter spacing shrinks as the font grows: 2px of tracking is a third of a
// word's width at 14 and just noise at 28, and it is width the label needs.
// Does txt fit the box at font f with this tracking, on one line or wrapped?
// max_w = LV_COORD_MAX measures the text unwrapped; passing bw measures it
// wrapped, in which case sz.x comes back as the WIDEST LINE -- so a single word
// too long for the box still reports a miss instead of silently overhanging.
static bool pill_fits(const char *txt, const lv_font_t *f, int space,
                      int bw, int bh, bool wrap)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, f, space, 0, wrap ? bw : LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return sz.x <= bw && sz.y <= bh;
}

// The rungs a pill label descends, cheapest concession first:
//
//   1. the size, with its normal tracking
//   2. the same size with tracking closed to 0. "CREATE NEW WALLET" is 20
//      characters, so 1px of tracking is 20px of width -- and nobody has ever
//      noticed a missing pixel between letters, while everybody notices a
//      button rendered in the smallest type on the screen.
//   3. a SECOND LINE at the same size. "MAINTENIR POUR SIGNER" has no one-line
//      size above 14 on any pill this layout can afford.
//   4. only then, a smaller font.
//
// Wrapping is never tried before both single-line attempts, so a label that
// already fits on one line will not start breaking in two.
wt_pill_fit_t wt_pill_fit(const char *txt, int w, int h, bool primary)
{
    int bw = w - 28, bh = h - 8;   // rounded ends eat the corners
    wt_pill_fit_t r = { wt_font14(), 2, false };

    if (primary) {
        if (pill_fits(txt, wt_font28(), 1, bw, bh, false))
            return (wt_pill_fit_t){ wt_font28(), 1, false };
        if (pill_fits(txt, wt_font28(), 0, bw, bh, false))
            return (wt_pill_fit_t){ wt_font28(), 0, false };
    }
    if (pill_fits(txt, wt_font23(), 1, bw, bh, false))
        return (wt_pill_fit_t){ wt_font23(), 1, false };
    if (pill_fits(txt, wt_font23(), 0, bw, bh, false))
        return (wt_pill_fit_t){ wt_font23(), 0, false };
    if (pill_fits(txt, wt_font23(), 0, bw, bh, true))
        return (wt_pill_fit_t){ wt_font23(), 0, true };
    if (pill_fits(txt, wt_font14(), 2, bw, bh, false))
        return r;
    if (pill_fits(txt, wt_font14(), 1, bw, bh, false))
        return (wt_pill_fit_t){ wt_font14(), 1, false };
    return (wt_pill_fit_t){ wt_font14(), 1, true };   // out of rungs: wrap
}

// One rung for a whole group of pills: the SMALLEST that every label needs.
//
// wt_pill_fit sizes one label in isolation, and `primary` lets that one label
// reach 28 while its neighbours start at 23. On the SIGN chooser that put
// "SCAN QR" at 28 directly above "FROM SD CARD" at 23 -- two buttons doing the
// same job, one visibly shouting. Per-label fitting is right for a lone pill
// and wrong for a set, because a set reads as a set.
//
// Callers pass every label that shares a visual row or column. The result is
// applied to all of them via wt_pill_apply_fit, so they land on one size.
wt_pill_fit_t wt_pill_group_fit(const char *const *txts, int n, int w, int h,
                                bool primary)
{
    wt_pill_fit_t worst = wt_pill_fit(txts && n > 0 ? txts[0] : "", w, h, primary);
    for (int i = 1; i < n; i++) {
        wt_pill_fit_t f = wt_pill_fit(txts[i], w, h, primary);
        // Rank by glyph height first, then by whether the label had to wrap:
        // a wrapped 23 is a worse fit than a one-line 23 and must win, or the
        // group settles on a size one of its members cannot actually use.
        int rank_f = (f.font == wt_font14() ? 0 : f.font == wt_font23() ? 1 : 2) * 2
                     + (f.wrap ? 0 : 1);
        int rank_w = (worst.font == wt_font14() ? 0 : worst.font == wt_font23() ? 1 : 2) * 2
                     + (worst.wrap ? 0 : 1);
        if (rank_f < rank_w)
            worst = f;
    }
    return worst;
}

void wt_pill_apply_fit(lv_obj_t *pill, wt_pill_fit_t f, int w)
{
    if (!pill) return;
    lv_obj_t *l = lv_obj_get_child(pill, 0);
    if (!l) return;
    lv_obj_set_style_text_font(l, f.font, 0);
    lv_obj_set_style_text_letter_space(l, f.space, 0);
    if (f.wrap) {
        lv_obj_set_width(l, w - 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_set_width(l, LV_SIZE_CONTENT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }
    lv_obj_center(l);
}

static void pill_label_fit(lv_obj_t *l, const char *txt, int w, int h, bool primary)
{
    wt_pill_fit_t f = wt_pill_fit(txt, w, h, primary);
    lv_obj_set_style_text_font(l, f.font, 0);
    lv_obj_set_style_text_letter_space(l, f.space, 0);
    if (f.wrap) {
        lv_obj_set_width(l, w - 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        // a re-fit can turn wrapping back off (wt_pill_row drops a rung)
        lv_obj_set_width(l, LV_SIZE_CONTENT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }
}

lv_obj_t *wt_screen(lv_obj_t *parent, const char *title, const char *sub)
{
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, WT_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(scr);

    // The page title is the one label on every screen, so it sets the tone for
    // how big the device "feels". wt_font34 gives Latin/Cyrillic a real 34px
    // face and hands CJK locales 28, which is why this can grow without a CJK
    // font at 34 existing.
    //
    // Header geometry is tight and deliberate: 34 has a ~45px line box, so the
    // title moved up to y=18 to keep its descenders off the subtitle, and the
    // subtitle moved to y=66 with a 29px budget -- exactly one line at font23,
    // landing on 95, one pixel clear of the y=96 content line every screen
    // builds against. Loosen any of those three numbers and the subtitle either
    // drops to font14 or collides with the first row of content.
    lv_obj_t *cap = lv_label_create(scr);   // note_font: defined with wt_note below
    lv_label_set_text(cap, title);
    lv_obj_set_style_text_color(cap, wt_accent(), 0);
    lv_obj_set_style_text_font(cap, wt_font34(), 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 48, 18);
    lv_obj_set_user_data(cap, (void *)WT_TITLE_TAG);

    if (sub) {
        // The subtitle gets ONE line, between the title and content at y=96.
        // Sized to fit, not assumed to fit: at a fixed font23 the longer
        // subtitles ran straight off the right edge of the panel, and a label
        // with no width clips silently instead of wrapping.
        lv_obj_t *s = lv_label_create(scr);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_color(s, WT_MUT, 0);
        lv_obj_set_style_text_font(s, note_font(sub, 704, 29), 0);
        lv_obj_set_width(s, 704);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s, 48, 66);
        lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
    }
    lv_obj_set_user_data(scr, (void *)WT_SCREEN_TAG);
    wt_title_fit(scr, 704);   // 48..752, the page margins
    return scr;
}

void wt_title_fit(lv_obj_t *scr, int w)
{
    lv_obj_t *cap = wt_tagged(scr, WT_TITLE_TAG);
    if (!cap) return;
    const char *txt = lv_label_get_text(cap);
    if (!txt || !*txt) return;

    // 34 -> 28 -> 23, one line the whole way. A title is the one label that
    // must not wrap: wt_screen puts the subtitle 3px under its 45px box, so a
    // second line lands on top of the subtitle rather than pushing it down.
    // Measured unwrapped (LV_COORD_MAX) so the answer is the real width the
    // words need, not the widest line of a wrap that already went wrong.
    //
    // The tracking shrinks with the size for the same reason pill labels do:
    // 3px between letters is presence at 34 and just lost width at 23.
    static const int space[3] = { 3, 2, 2 };
    const lv_font_t *f[3] = { wt_font34(), wt_font28(), wt_font23() };
    int pick = 2;
    for (int i = 0; i < 3; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, txt, f[i], space[i], 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (sz.x <= w) { pick = i; break; }
    }
    lv_obj_set_style_text_font(cap, f[pick], 0);
    lv_obj_set_style_text_letter_space(cap, space[pick], 0);
}

void wt_sub_fit(lv_obj_t *scr, int w)
{
    lv_obj_t *s = wt_tagged(scr, WT_SUB_TAG);
    if (!s) return;
    const char *txt = lv_label_get_text(s);
    if (!txt) return;
    // Re-fit as well as re-width: the subtitle gets ONE line inside a 29px
    // budget, and narrowing the lane without re-measuring would simply wrap it
    // onto the y=96 content line.
    lv_obj_set_style_text_font(s, note_font(txt, w, 29), 0);
    lv_obj_set_width(s, w);
}

// ---- the action bar (see wallet_theme.h) ----
// Built on demand by wt_pillh, so it exists exactly on the screens that have an
// action row and never has to be remembered.
//
// Created ONCE and never re-raised. LVGL paints in tree order, so the bar lands
// above everything built before the first action pill and below every pill
// built after it, which is the stacking this wants. Raising it again on the
// second pill would put it over the first one. It also means anything a screen
// deliberately draws INSIDE the band after its buttons, such as the build
// identity line along the bottom edge of Settings, still draws on top of the
// bar rather than being swallowed by it.
static void action_bar_ensure(lv_obj_t *scr)
{
    if (!scr || lv_obj_get_user_data(scr) != (void *)WT_SCREEN_TAG) return;

    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++)
        if (lv_obj_get_user_data(lv_obj_get_child(scr, i)) == (void *)WT_BAR_TAG)
            return;

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_user_data(bar, (void *)WT_BAR_TAG);
    lv_obj_set_pos(bar, 0, WT_CONTENT_BOTTOM);
    lv_obj_set_size(bar, 800, 480 - WT_CONTENT_BOTTOM);
    lv_obj_set_style_bg_color(bar, WT_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, WT_HAIR, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    // Not clickable and not scrollable: it is a surface, and a tap that misses a
    // button must fall through to whatever is behind rather than being eaten.
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- tap feedback ----
// The device has no haptics, so a press can only be answered optically, and
// until now the whole answer was the background changing colour instantly.
//
// Two more style properties carry the rest. The pill translates 2px down while
// held, so it reads as pushed in; and an outline ring travels between the pill
// edge and 10px outside it, opaque at the pressed end and invisible at the
// resting end. Pressing pulls the ring in as it appears, releasing pushes it
// back out as it fades -- an optical tick at each end of the tap.
//
// The ring needs no press/release handlers because BOTH ends are ordinary
// states: the resting state simply owns the wide invisible outline and the
// pressed state owns the narrow opaque one, and the transition below animates
// whichever direction the finger goes.
//
// Deliberately NOT transform_scale. A scaled object forces LVGL to allocate a
// draw layer, the pool on this panel cannot hold one that size, and
// lv_draw_dispatch then retries the allocation forever -- a hard hang, not a
// dropped frame. calculate_layer_type() in lv_obj_style.c triggers only on
// rotation, scale, skew, layered opacity, bitmap masks and blend modes; every
// property used here is a plain paint value with no layer behind it.
//
// One descriptor on the DEFAULT state rather than a fast-in/slow-out pair,
// because a transition is looked up on the state being entered: a descriptor
// living only on the pressed style would animate the press and then sit out
// the release, which is the half that matters most.
#define WT_TAP_RING 10          // how far outside the edge the ring travels
#define WT_TAP_MS   160

static lv_style_transition_dsc_t s_tap_tr;
static bool s_tap_tr_ready;

void wt_tap_feedback(lv_obj_t *p)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR, LV_STYLE_TRANSLATE_Y,
        LV_STYLE_OUTLINE_WIDTH, LV_STYLE_OUTLINE_OPA,
        LV_STYLE_PROP_INV                       // terminator
    };
    if (!s_tap_tr_ready) {
        lv_style_transition_dsc_init(&s_tap_tr, props, lv_anim_path_ease_out,
                                     WT_TAP_MS, 0, NULL);
        s_tap_tr_ready = true;
    }
    lv_obj_set_style_outline_color(p, wt_primary(), 0);
    lv_obj_set_style_outline_width(p, WT_TAP_RING, 0);
    lv_obj_set_style_outline_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(p, 0, 0);
    lv_obj_set_style_transition(p, &s_tap_tr, 0);

    lv_obj_set_style_outline_width(p, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_opa(p, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(p, 2, LV_STATE_PRESSED);
}

static lv_obj_t *round_chip(lv_obj_t *parent, const char *symbol,
                            int x, int y, lv_color_t color,
                            lv_event_cb_t cb, void *ud)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 30, 30);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_style_radius(chip, 15, 0);
    lv_obj_set_style_bg_color(chip, WT_KEY, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, color, 0);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(chip, 12);       // 54px effective target
    wt_tap_feedback(chip);
    if (cb) lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, wt_font14(), 0);
    lv_obj_center(label);
    return chip;
}

lv_obj_t *wt_help_chip(lv_obj_t *parent, int x, int y, lv_color_t color,
                       lv_event_cb_t cb, void *ud)
{
    return round_chip(parent, "?", x, y, color, cb, ud);
}

lv_obj_t *wt_pillh(lv_obj_t *scr, const char *txt, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud)
{
    // A pill at the action line means this screen has an action row, so it
    // gets the floor to stand on. No-op for a pill that is content (a chooser
    // row, a keyboard key) and for one built inside a card rather than on a
    // screen, which is why the test is the y AND the tag, not either alone.
    if (y >= WT_CONTENT_BOTTOM) action_bar_ensure(scr);

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_radius(p, 26, 0);
    lv_obj_set_style_bg_color(p, WT_KEY, 0);
    lv_obj_set_style_bg_color(p, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    wt_tap_feedback(p);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, WT_MUT, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, WT_INK, 0);
    pill_label_fit(l, txt, w, h, false);
    lv_obj_center(l);
    return p;
}

lv_obj_t *wt_pill(lv_obj_t *scr, const char *txt, int x, int y, int w,
                  lv_event_cb_t cb, void *ud)
{
    return wt_pillh(scr, txt, x, y, w, 52, cb, ud);
}

// Two spaces, not one: at a pill's tracking a single space let the icon crowd
// the first letter and the pair read as one damaged glyph.
void wt_icon_text(char *out, size_t out_len, const char *icon, const char *txt)
{
    snprintf(out, out_len, "%s  %s", icon, txt);
}

lv_obj_t *wt_pill_icon(lv_obj_t *scr, const char *icon, const char *txt,
                       int x, int y, int w, int h, lv_event_cb_t cb, void *ud)
{
    char buf[WT_ICON_TEXT_MAX];
    wt_icon_text(buf, sizeof buf, icon, txt);
    return wt_pillh(scr, buf, x, y, w, h, cb, ud);
}

// ---- hold to confirm (see wallet_theme.h) ----
// One press cannot fire it and neither can two: the finger has to stay down.
// State hangs off the pill so several could coexist, and the timer is deleted
// on release AND on delete, so a screen torn down mid-hold leaves nothing.
typedef struct {
    lv_obj_t *pill, *fill;
    lv_timer_t *tmr;
    uint32_t t0;
    int ms, w;
    void (*done)(void *);
    void *ud;
} wt_hold_t;

static void hold_reset(wt_hold_t *h)
{
    if (h->tmr) { lv_timer_delete(h->tmr); h->tmr = NULL; }
    if (h->fill) lv_obj_set_width(h->fill, 0);
}

static void hold_tick_cb(lv_timer_t *t)
{
    wt_hold_t *h = lv_timer_get_user_data(t);
    uint32_t el = lv_tick_elaps(h->t0);
    if (el >= (uint32_t)h->ms) {
        void (*done)(void *) = h->done;
        void *ud = h->ud;
        hold_reset(h);
        if (done) done(ud);            // may delete the pill: touch nothing after
        return;
    }
    lv_obj_set_width(h->fill, (int32_t)(el * (uint32_t)h->w / (uint32_t)h->ms));
}

static void hold_press_cb(lv_event_t *e)
{
    wt_hold_t *h = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        h->t0 = lv_tick_get();
        if (!h->tmr) h->tmr = lv_timer_create(hold_tick_cb, 30, h);
    } else {                           // RELEASED, PRESS_LOST, or DELETE
        hold_reset(h);
        if (c == LV_EVENT_DELETE) lv_free(h);
    }
}

lv_obj_t *wt_hold_pill(lv_obj_t *scr, const char *txt, int x, int y, int w, int h_,
                       int ms, void (*done)(void *), void *ud)
{
    wt_hold_t *h = lv_malloc(sizeof *h);
    if (!h) return NULL;
    lv_memzero(h, sizeof *h);
    h->ms = ms > 0 ? ms : 1200;
    h->w = w;
    h->done = done;
    h->ud = ud;

    lv_obj_t *p = wt_pillh(scr, txt, x, y, w, h_, NULL, NULL);
    lv_obj_set_style_border_color(p, WT_STOP, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    h->pill = p;

    // the sweep sits UNDER the label (added first would be behind the text
    // LVGL already made, so move it back explicitly)
    lv_obj_t *f = lv_obj_create(p);
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, 0, h_);
    lv_obj_set_pos(f, 0, 0);
    lv_obj_set_style_radius(f, 26, 0);
    lv_obj_set_style_bg_color(f, WT_STOP, 0);
    lv_obj_set_style_bg_opa(f, 90, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(f);
    h->fill = f;

    lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_PRESSED, h);
    lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_RELEASED, h);
    lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_PRESS_LOST, h);
    lv_obj_add_event_cb(p, hold_press_cb, LV_EVENT_DELETE, h);
    return p;
}

void wt_pill_select(lv_obj_t *pill, bool on)
{
    // active chooser = filled glass + 2px accent ring + bright text; inactive
    // recedes. The fill is what makes MONO's selection readable (a white ring
    // alone disappears next to white text).
    lv_obj_set_style_bg_color(pill, on ? wt_accent_bg() : WT_KEY, 0);
    lv_obj_set_style_border_color(pill, on ? wt_primary() : WT_MUT, 0);
    lv_obj_set_style_border_width(pill, on ? 2 : 1, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(pill, 0), on ? WT_INK : WT_MUT, 0);
}

void wt_pill_primary(lv_obj_t *pill)
{
    lv_obj_set_style_bg_color(pill, wt_accent_bg(), 0);
    lv_obj_set_style_bg_color(pill, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(pill, wt_primary(), 0);
    lv_obj_set_style_border_width(pill, 2, 0);
    // this marker is already "the one action this screen wants" everywhere it
    // is used, so it is also where the label earns the top rung
    wt_pill_label_max(pill);
}

// Pills that sit in one row share a label size. The fit is per pill, so one
// long word drops only that pill a rung: BACK came out at 23 next to "silent
// payment" at 14 and the row read as a rendering mistake rather than a choice.
// Smallest wins, which is also the only size guaranteed to fit all of them.
void wt_pill_row(lv_obj_t **pills, int n)
{
    const lv_font_t *lo = wt_font28();
    for (int i = 0; i < n; i++) {
        lv_obj_t *l = pills[i] ? lv_obj_get_child(pills[i], 0) : NULL;
        if (!l || !lv_obj_check_type(l, &lv_label_class)) continue;
        const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
        if (f == wt_font14()) lo = f;
        else if (f == wt_font23() && lo == wt_font28()) lo = f;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *l = pills[i] ? lv_obj_get_child(pills[i], 0) : NULL;
        if (!l || !lv_obj_check_type(l, &lv_label_class)) continue;
        lv_obj_set_style_text_font(l, lo, 0);
        lv_obj_set_style_text_letter_space(l, lo == wt_font14() ? 2 : 1, 0);
    }
}

void wt_pill_label_max(lv_obj_t *pill)
{
    lv_obj_t *l = lv_obj_get_child(pill, 0);
    if (!l || !lv_obj_check_type(l, &lv_label_class)) return;
    lv_obj_update_layout(pill);
    pill_label_fit(l, lv_label_get_text(l),
                   lv_obj_get_width(pill), lv_obj_get_height(pill), true);
}

// A pill that carries a second line: a category over the app that fits it, a
// type over its example prefix. The main label owns the top of the box and the
// sub-label the bottom, so the fit has to exclude the sub's row. Centralised
// because three screens had hand-tuned offsets that no longer agreed once the
// main label could change size.
static void pill_sub_line(lv_obj_t *pill, const char *sub,
                          const lv_font_t *f, int row_h)
{
    lv_obj_t *main_l = lv_obj_get_child(pill, 0);
    if (!main_l) return;
    lv_obj_update_layout(pill);
    int w = lv_obj_get_width(pill), h = lv_obj_get_height(pill);
    pill_label_fit(main_l, lv_label_get_text(main_l), w, h - row_h, false);
    lv_obj_align(main_l, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *s = lv_label_create(pill);
    lv_label_set_text(s, sub);
    // The sub-line is centred and never wraps, so a font too wide for the pill
    // does not clip -- it hangs out past both edges, over whatever is beside
    // the button. Drop it a rung instead. Only the locale that cannot make the
    // requested size pays, rather than every locale being held to the longest.
    if (f != wt_font14() && !pill_fits(sub, f, 0, w - 28, row_h, false))
        f = wt_font14();
    lv_obj_set_style_text_font(s, f, 0);
    lv_obj_set_style_text_color(s, WT_MUT, 0);
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
}

void wt_pill_two_line(lv_obj_t *pill, const char *sub)
{
    pill_sub_line(pill, sub, wt_font14(), SUB_ROW_H);
}

// Same pill, but the second line is a VALUE and not an eyebrow: the example
// address under ADDRESS TYPE is what actually tells you what your addresses
// look like ("bc1..." vs "1..."), and at 14 under a 23px name it read as a
// footnote on its own button. Needs 43px of pill above the main label's line,
// so the caller has to give the pill ~72px of height for the name to stay 23.
void wt_pill_two_line_val(lv_obj_t *pill, const char *sub)
{
    pill_sub_line(pill, sub, wt_font23(), SUB_ROW_H23);
}

lv_obj_t *wt_lbl(lv_obj_t *scr, const char *txt, int x, int y,
                 const lv_font_t *f, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Side note with a KNOWN vertical budget: picks the largest of the three fonts
// that fits, exactly like an explainer card. A blanket font bump here does not
// work -- these sit in gaps between other controls, so the size has to be
// derived from the gap, and short copy is what earns the big one.
lv_obj_t *wt_wraph(lv_obj_t *scr, const char *txt, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, x, y);
    wt_wrap_fit(l, txt, w, h);
    return l;
}

// Re-fit a wt_wraph label whose text changes after it is built (the settings
// chooser captions swap on every tap). The font has to be recomputed with the
// text: leaving the old one is how a longer translation silently overflows.
void wt_wrap_fit(lv_obj_t *l, const char *txt, int w, int h)
{
    if (!l) return;
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, wt_body_font(txt, w, h), 0);
}

// A note that BELONGS to a control: same auto-fit, capped at 23. Left uncapped,
// a three-word note under a button renders at 28 and ends up shouting louder
// than the button itself. Explainer cards keep the full ladder; these do not.
static const lv_font_t *note_font(const char *txt, int w, int max_h)
{
    if (!txt || !*txt) return wt_font23();
    lv_point_t sz;
    lv_text_get_size(&sz, txt, wt_font23(), 0, 0, w, LV_TEXT_FLAG_NONE);
    return sz.y <= max_h ? wt_font23() : wt_font14();
}

void wt_note_fit(lv_obj_t *l, const char *txt, int w, int h)
{
    if (!l) return;
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, note_font(txt, w, h), 0);
}

lv_obj_t *wt_note(lv_obj_t *scr, const char *txt, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, x, y);
    wt_note_fit(l, txt, w, h);
    return l;
}

lv_obj_t *wt_wrap(lv_obj_t *scr, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_obj_set_style_text_font(l, wt_font14(), 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Column captions ("NETWORK", "WALLET"). Deliberately SMALL: these are
// eyebrows, not content. They were briefly font23 and it inverted the whole
// hierarchy, the label shouting while the value under it whispered. The size
// belongs to the thing you actually read.
//
// Small and WT_MUT together was the mistake: the caption lost twice, once on
// scale and again on contrast, and on the device panel ADDRESS TYPE was
// reported as barely visible. The desktop simulator never showed it, because a
// monitor renders #7A869C on #070A10 far more generously than the panel does.
// So the rank is carried by SIZE alone and the ink stays full strength. A
// caption you cannot read is not a subtle caption, it is a missing one.
lv_obj_t *wt_section(lv_obj_t *scr, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, WT_INK, 0);
    lv_obj_set_style_text_font(l, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *qr_card_raw(lv_obj_t *scr, lv_obj_t **qr,
                             int x, int y, int card_px, int qr_px)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_px, card_px);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, WT_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *q = lv_qrcode_create(card);
    if (q) {
        lv_qrcode_set_size(q, qr_px);
        lv_qrcode_set_dark_color(q, lv_color_hex(0x0B0E14));
        lv_qrcode_set_light_color(q, WT_CARD);
        lv_obj_remove_flag(q, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(q);
    }
    if (qr) *qr = q;
    return card;
}

typedef struct {
    lv_obj_t *card;
    lv_obj_t *qr;
    lv_obj_t *zoom;
    lv_obj_t *zoom_qr;
    uint8_t *data;
    uint32_t data_len;
    uint32_t data_cap;
} wt_qr_state_t;

// QR payloads are usually public, but the same component also renders the
// private Silent Payments scan key. Keep the cache generic without weakening
// key hygiene: volatile writes prevent the compiler from eliding the wipe.
static void qr_payload_zero(void *data, uint32_t len)
{
    volatile uint8_t *p = data;
    while (p && len--) *p++ = 0;
}

static void qr_payload_free(wt_qr_state_t *s)
{
    if (!s || !s->data) return;
    qr_payload_zero(s->data, s->data_cap);
    lv_free(s->data);
    s->data = NULL;
    s->data_len = 0;
    s->data_cap = 0;
}

static void qr_zoom_close_cb(lv_event_t *e)
{
    wt_qr_state_t *s = lv_event_get_user_data(e);
    if (!s || !s->zoom) return;
    lv_obj_t *zoom = s->zoom;
    s->zoom = NULL;
    s->zoom_qr = NULL;
    // Animated output updates only the enlarged QR while it is visible. Put
    // the latest cached frame back on the underlying card before returning.
    if (s->qr && s->data && s->data_len)
        lv_qrcode_update(s->qr, s->data, s->data_len);
    lv_obj_delete_async(zoom);
}

static void qr_zoom_open_cb(lv_event_t *e)
{
    wt_qr_state_t *s = lv_event_get_user_data(e);
    if (!s || s->zoom || !s->data || !s->data_len) return;

    lv_obj_t *parent = lv_obj_get_parent(s->card);
    lv_obj_t *ovl = lv_obj_create(parent);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, WT_BG, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ovl, qr_zoom_close_cb, LV_EVENT_CLICKED, s);
    lv_obj_move_foreground(ovl);
    s->zoom = ovl;

    // 392px gives even a 117-character Silent Payment code materially larger
    // modules while leaving a full white quiet zone and a visible close target.
    lv_obj_t *zoom_card = qr_card_raw(ovl, &s->zoom_qr, 188, 28, 424, 392);
    lv_obj_add_flag(zoom_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zoom_card, qr_zoom_close_cb, LV_EVENT_CLICKED, s);
    if (s->zoom_qr)
        lv_qrcode_update(s->zoom_qr, s->data, s->data_len);
    round_chip(ovl, LV_SYMBOL_CLOSE, 748, 20, WT_MUT,
               qr_zoom_close_cb, s);
}

static void qr_state_delete_cb(lv_event_t *e)
{
    wt_qr_state_t *s = lv_event_get_user_data(e);
    if (!s) return;
    qr_payload_free(s);
    lv_free(s);
}

lv_obj_t *wt_qr_card(lv_obj_t *scr, lv_obj_t **qr,
                     int x, int y, int card_px, int qr_px)
{
    lv_obj_t *q = NULL;
    lv_obj_t *card = qr_card_raw(scr, &q, x, y, card_px, qr_px);
    if (!q) {
        if (qr) *qr = NULL;
        return card;
    }

    wt_qr_state_t *s = lv_malloc_zeroed(sizeof *s);
    if (!s) {
        if (qr) *qr = q;
        return card;                         // QR still works; only zoom is absent
    }
    s->card = card;
    s->qr = q;
    lv_obj_set_user_data(q, s);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    wt_tap_feedback(card);
    lv_obj_add_event_cb(card, qr_zoom_open_cb, LV_EVENT_CLICKED, s);
    lv_obj_add_event_cb(card, qr_state_delete_cb, LV_EVENT_DELETE, s);
    // Outside the white card, so the cue never damages the QR quiet zone.
    round_chip(scr, LV_SYMBOL_PLUS, x - 36, y + 8, WT_MUT,
               qr_zoom_open_cb, s);

    if (qr) *qr = q;
    return card;
}

lv_result_t wt_qr_update(lv_obj_t *qr, const void *data, uint32_t data_len)
{
    if (!qr) return LV_RESULT_INVALID;
    wt_qr_state_t *s = lv_obj_get_user_data(qr);
    if (!s) return lv_qrcode_update(qr, data, data_len);

    uint32_t old_len = s->data_len;
    if (data_len > s->data_cap) {
        uint32_t cap = (data_len + 127u) & ~127u;
        // Do not use realloc here: if it moves the block, the allocator frees
        // the old secret-bearing buffer before we have a chance to wipe it.
        uint8_t *next = lv_malloc(cap);
        if (!next) return lv_qrcode_update(qr, data, data_len);
        qr_payload_free(s);
        s->data = next;
        s->data_cap = cap;
        old_len = 0;
    }
    lv_memcpy(s->data, data, data_len);
    if (old_len > data_len)
        qr_payload_zero(s->data + data_len, old_len - data_len);
    s->data_len = data_len;

    // The underlying card is completely covered while zoomed. Updating just
    // the visible QR avoids encoding every animated fragment twice.
    return lv_qrcode_update(s->zoom_qr ? s->zoom_qr : qr, data, data_len);
}

// Only the TAIL is lit. The first characters of a bech32 address are the human
// readable part and the witness version: every Native SegWit mainnet address
// starts bc1q and every testnet/signet one tb1q. Highlighting them taught
// people to compare a constant, which is worse than useless -- it feels like
// checking while confirming nothing, and an address swapped by malware matches
// there for free. The last 8 (two groups) carry real entropy and include the
// bech32 checksum, so any altered address differs in them.
#define ADDR_TAIL_CHARS 8

// A spangroup is clickable out of the box, and an address is text, not a
// button. Left alone it silently eats every press that lands on it: put one
// inside a tappable row and the row goes dead exactly where the address is
// printed -- which is the middle, and the first place a finger goes.
static void addr_spans_no_click(lv_obj_t *sg)
{
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
}

static lv_span_t *addr_span(lv_obj_t *sg, const char *txt, bool lit)
{
    lv_span_t *s = lv_spangroup_new_span(sg);
    lv_span_set_text(s, txt);
    // Lit groups stand out by brightness alone (accent/ink vs muted grey). No
    // underline: it read as a link or a spelling error on the address, and the
    // contrast already carries the distinction.
    lv_style_set_text_color(lv_span_get_style(s), lit ? wt_accent() : WT_MUT);
    return s;
}

lv_obj_t *wt_addr_short(lv_obj_t *par, const char *addr, const lv_font_t *f)
{
    size_t n = strlen(addr);
    // Not an address at all -- a locked-session message, an error string. Show
    // it as plain words rather than slicing arbitrary text into fake blocks.
    if (n < 20)
        return wt_lbl(par, addr, 0, 0, f, WT_MUT);

    // bech32 opens with a constant prefix through the first data character:
    // bc1q/tb1q for SegWit and sp1q/tsp1q for silent payments. Skip it and
    // light the four AFTER it. Base58 has no such constant, so its first four
    // are the lit ones.
    int pre = !strncmp(addr, "tsp1", 4) ? 5
            : (!strncmp(addr, "bc1", 3) || !strncmp(addr, "tb1", 3) ||
               !strncmp(addr, "sp1", 3)) ? 4 : 0;
    char head[8] = {0}, key[8] = {0}, mid[32] = {0}, last[8] = {0};
    lv_memcpy(head, addr, (size_t)pre);
    lv_memcpy(key, addr + pre, 4);
    // Twelve from the end, in three blocks of four. Chunking from the RIGHT is
    // the point: 42 characters do not divide by four, so grouping from the left
    // would leave the final block short and the lit four would straddle a gap.
    const char *t = addr + n - 12;
    snprintf(mid, sizeof mid, "  \xE2\x80\xA6  %.4s %.4s ", t, t + 4);
    lv_memcpy(last, t + 8, 4);

    lv_obj_t *sg = lv_spangroup_create(par);
    addr_spans_no_click(sg);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);   // one line, sized to fit
    lv_obj_set_style_text_font(sg, f, 0);
    if (pre) {
        char pfx[8];
        snprintf(pfx, sizeof pfx, "%s ", head);
        addr_span(sg, pfx, false);
    }
    addr_span(sg, key, true);
    addr_span(sg, mid, false);
    addr_span(sg, last, true);
    lv_spangroup_refresh(sg);
    return sg;
}

lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f)
{
    int len = (int)strlen(grouped);
    int t = 0, raw = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (grouped[i] != ' ' && ++raw == ADDR_TAIL_CHARS) { t = i; break; }
    }
    // Snap to a GROUP boundary. An address is rarely a multiple of 4, so a
    // fixed 8-character tail starts mid-group and the highlight breaks a block
    // in half -- which then wraps, orphaning two characters on their own line.
    // Whole groups only: still "the last few", but always readable as blocks.
    while (t > 0 && grouped[t - 1] != ' ') t--;
    char head[256];           // fits a grouped silent-payment addr (~146 chars)
    snprintf(head, sizeof head, "%.*s", t, grouped);

    lv_obj_t *sg = lv_spangroup_create(par);
    addr_spans_no_click(sg);
    lv_obj_set_width(sg, w);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, f, 0);
    lv_span_t *s1 = lv_spangroup_new_span(sg);
    lv_span_set_text(s1, head);
    lv_style_set_text_color(lv_span_get_style(s1), WT_MUT);
    lv_span_t *s2 = lv_spangroup_new_span(sg);
    lv_span_set_text(s2, grouped + t);
    lv_style_set_text_color(lv_span_get_style(s2), wt_accent());   // brightness, no underline
    // The tail is the part you are actually asked to compare, so when the body
    // is too small to compare comfortably the tail renders one rung ABOVE it.
    // Blowing up the whole string instead would push the other outputs off a
    // scrolling list -- this buys the legibility where it counts for one extra
    // line of height.
    //
    // The bump only applies to font14, because it only exists to rescue font14.
    // At 23 and 28 the body is already readable at arm's length and the accent
    // colour alone marks the tail: that is what the main receive screen has
    // always done at 28, and a 23 that jumped to 28 would cost two more lines
    // on a 117-character silent-payment address for no gain.
    lv_style_set_text_font(lv_span_get_style(s2),
                           f == wt_font14() ? wt_font23() : f);
    lv_spangroup_refresh(sg);
    return sg;
}

// ---- explainer-card entrance animation (shared by every "?" card) ----
static void ci_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void ci_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void ci_bg(void *o, int32_t v)  { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

void wt_card_intro(lv_obj_t *card)
{
    // the dim backdrop eases in first
    lv_opa_t bg = lv_obj_get_style_bg_opa(card, 0);
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_anim_t d;
    lv_anim_init(&d);
    lv_anim_set_var(&d, card);
    lv_anim_set_values(&d, 0, bg);
    lv_anim_set_duration(&d, 140);
    lv_anim_set_path_cb(&d, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&d, ci_bg);
    lv_anim_start(&d);

    // then the content settles in, one element after the next. translate_y is a
    // render offset, so it composes cleanly with lv_obj_align and reverts to 0.
    uint32_t n = lv_obj_get_child_count(card);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(card, i);
        lv_obj_set_style_opa(ch, 0, 0);
        lv_obj_set_style_translate_y(ch, 14, 0);
        uint32_t delay = 40 + i * 60;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ch);
        lv_anim_set_values(&a, 0, LV_OPA_COVER);
        lv_anim_set_duration(&a, 220);
        lv_anim_set_delay(&a, delay);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, ci_opa);
        lv_anim_start(&a);
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, ch);
        lv_anim_set_values(&b, 14, 0);
        lv_anim_set_duration(&b, 240);
        lv_anim_set_delay(&b, delay);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&b, ci_ty);
        lv_anim_start(&b);
    }
}

// ---- chip diagrams (shared by the "?" cards) ----
lv_obj_t *wt_diagram_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *wt_chip(lv_obj_t *row, const char *txt, bool accent)
{
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 12, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_bg_color(c, accent ? wt_accent_bg() : WT_KEY, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, accent ? 2 : 1, 0);
    lv_obj_set_style_border_color(c, accent ? wt_primary() : WT_MUT, 0);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, accent ? WT_INK : WT_MUT, 0);
    lv_obj_set_style_text_font(l, wt_font14(), 0);
    lv_obj_center(l);
    return c;
}

lv_obj_t *wt_diagram_op(lv_obj_t *row, const char *txt)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_obj_set_style_text_font(l, wt_font14(), 0);
    return l;
}

void wt_diagram_fp(lv_obj_t *parent)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip(row, tr(STR_D_WORDS), false);
    wt_diagram_op(row, "+");
    wt_chip(row, tr(STR_D_PASSPHRASE), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, tr(STR_D_FINGERPRINT), true);
}

void wt_diagram_pair(lv_obj_t *parent)
{
    // the airgap: an online app and the offline signer, bridged only by QR
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip(row, tr(STR_D_ONLINE_APP), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT " QR " LV_SYMBOL_LEFT);
    wt_chip(row, tr(STR_D_KISS_OFFLINE), true);
}

void wt_group4(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
        if (i && i % 4 == 0) out[o++] = ' ';
        out[o++] = in[i];
    }
    out[o] = 0;
}

void wt_fmt_btc(uint64_t sats, char *out, size_t out_len)
{
    // full 8 decimals, never abbreviated: this string exists to be compared
    // digit-by-digit against a coordinator that displays BTC
    snprintf(out, out_len, "%llu.%08llu",
             (unsigned long long)(sats / 100000000ULL),
             (unsigned long long)(sats % 100000000ULL));
}

void wt_fmt_sats(uint64_t v, char *out, size_t out_len)
{
    char raw[24];
    int n = snprintf(raw, sizeof raw, "%llu", (unsigned long long)v);
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < out_len; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = 0;
}
