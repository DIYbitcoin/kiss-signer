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
static const char WT_DECOR_TAG[]  = "wt_decor";

void wt_mark_decor(lv_obj_t *o)
{
    if (o) lv_obj_set_user_data(o, (void *)WT_DECOR_TAG);
}

bool wt_is_decor(const lv_obj_t *o)
{
    return o && lv_obj_get_user_data((lv_obj_t *)o) == (void *)WT_DECOR_TAG;
}

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

// The fixed pitch faces. No array, no fallback, no per locale variant, and
// none of that is an oversight.
//
// They exist for strings that are never translated: addresses, fingerprints,
// derivation paths, and amounts. A locale cannot change any of those, so there
// is nothing for a font class to select between, and a CJK subset at these
// sizes would cost more than the whole Latin set does.
//
// Deliberately NO fallback chain. Everywhere else a chain is load bearing,
// because a missing glyph should degrade to a smaller face rather than vanish.
// Here the opposite is wanted: if a localised string is ever pointed at one of
// these by mistake, it must be obvious. With CONFIG_LV_USE_FONT_PLACEHOLDER=y
// the lookup ends in a blank box half a line wide, which someone will file a
// bug about. A chain would render it one size small and nobody would notice
// the wiring is wrong. (The renderer does not hang on a missing glyph; the
// comment above about s_font34 predates LV_USE_FONT_PLACEHOLDER being on.)
const lv_font_t *wt_font_mono14(void) { return &font_kiss_mono14; }
const lv_font_t *wt_font_mono23(void) { return &font_kiss_mono23; }
const lv_font_t *wt_font_mono28(void) { return &font_kiss_mono28; }

// The Sign hero, and nothing else. Thirteen glyphs, digits and space and full
// stop, so it cannot represent a letter even if handed one.
const lv_font_t *wt_font_num48(void) { return &font_kiss_num48; }

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
    // CYPHERPINK started at 0xFF3EA5 and went to 0xC45CE8, and that overshot:
    // the only real complaint about the original was that at R=255 it sat about
    // 24 degrees of hue from WT_STOP (0xFF4D5E), so a red warning and ordinary
    // accent chrome read as the same family on a lit panel. Solving that by
    // going purple solved a problem nobody had -- a theme called CYPHERPINK
    // should be pink.
    //
    // 0xE85AB8 is the answer to the actual constraint. The red channel drops
    // from 255 to 232 and the hue moves further round, so it is not in WT_STOP's
    // family; it is plainly pink rather than violet; and its relative luminance
    // sits between the two it replaces, so nothing about contrast changes.
    // Status colours never move, so the accent is the one that has to.
    0xE85AB8,   // CYPHERPINK
    0xFF8A3D,   // ORANGE
};
static const uint32_t ACC_BG_HEX[WT_ACC_N] = {
    0x232E42,   // MONO: cool ink glass, bright enough that "selected" is obvious
    0x102417,   // GREEN
    0x261724,   // CYPHERPINK
    0x2B190D,   // ORANGE
};
// Each row is its accent scaled by the same per-channel ratios the pink pair
// has always used, so a new accent gets a fill and a pressed fill that sit at
// the same depth below it rather than being picked by eye.
static const uint32_t ACC_PRESS_HEX[WT_ACC_N] = {
    0x33405A,
    0x173823,
    0x342135,
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

void wt_lock_565(int *r5, int *g6, int *b5)
{
    // MONO's accent IS the ink, so a white lock would say nothing the brackets
    // going solid and closing on the code does not already say. That theme keeps
    // the green, which is the only place on the device a status colour and an
    // accent trade places -- and it is the honest way round, because in MONO
    // there is no accent to match.
    uint32_t hex = s_accent == WT_ACC_MONO ? 0x35D07F : ACC_HEX[s_accent];
    if (r5) *r5 = (int)((hex >> 19) & 0x1F);
    if (g6) *g6 = (int)((hex >> 10) & 0x3F);
    if (b5) *b5 = (int)((hex >>  3) & 0x1F);
}

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

// The card every wallet screen sits inside. Purely decorative: it is the FIRST
// child, so it draws behind everything, and every screen's absolute coordinates
// are untouched by its arrival. Inset 8 with radius 16 and a WT_EDGE hairline,
// which is what turns a set of objects floating on the panel into one surface
// with a boundary -- the single biggest difference between the shipped screens
// and the design review's drawings.
//
// Not clickable and not scrollable, for the same reason the action bar is not:
// a tap that misses a control must fall through to whatever is behind it.
static void screen_card(lv_obj_t *scr)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 8, 8);
    lv_obj_set_size(card, 784, 464);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, WT_EDGE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
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
    screen_card(scr);

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

lv_obj_t *wt_screen_title(lv_obj_t *scr)
{
    return wt_tagged(scr, WT_TITLE_TAG);
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

    // Inset to the card screen_card draws, not the full panel width: the row is
    // the bottom of one surface, so its hairline has to stop where that surface
    // stops. 9 and 782 sit one pixel inside the card's 8..792 border, and 73
    // takes the fill down to the card's inner bottom edge at 471 rather than
    // painting over its rounded corners.
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_user_data(bar, (void *)WT_BAR_TAG);
    lv_obj_set_pos(bar, 9, WT_CONTENT_BOTTOM);
    lv_obj_set_size(bar, 782, 471 - WT_CONTENT_BOTTOM);
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
    // 10, the same radius wt_card and wt_row_x use. It was 26 -- a lozenge --
    // for as long as buttons were the only boxes on the page. They are not: the
    // device is a list of rounded rectangles now, and a lozenge sitting under a
    // column of them reads as a different family of object rather than as the
    // same family doing a different job.
    //
    // What stays round is anything that is a MARK rather than a control: the "?"
    // chip, the explainer's icon badge, the glossary's grid badges, the diagram
    // tokens in wt_chip. A rectangle is the shape of "this does something"; a
    // circle is the shape of "this is a thing". The word "pill" survives in
    // every name here because renaming forty call sites would say nothing.
    lv_obj_set_style_radius(p, 10, 0);
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
    lv_obj_set_style_radius(f, 10, 0);   // matches the pill it sweeps across
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

// The same fold as wt_addr_short, as plain text: prefix, the four after it, an
// ellipsis, and the last twelve in three blocks. No lit spans, because this is
// for places that take a STRING and not an object -- a row's sub-line -- and
// because the lighting means "compare these", which is a job that belongs on
// RECEIVE where the whole address is on the glass beside its QR.
void wt_addr_fold(const char *addr, char *out, size_t len)
{
    size_t n = addr ? strlen(addr) : 0;
    if (!out || !len) return;
    if (n < 20) { snprintf(out, len, "%s", addr ? addr : ""); return; }
    int pre = !strncmp(addr, "tsp1", 4) ? 5
            : (!strncmp(addr, "bc1", 3) || !strncmp(addr, "tb1", 3) ||
               !strncmp(addr, "sp1", 3)) ? 4 : 0;
    const char *t = addr + n - 12;
    snprintf(out, len, "%.*s %.4s \xE2\x80\xA6 %.4s %.4s %.4s",
             pre, addr, addr + pre, t, t + 4, t + 8);
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

lv_obj_t *wt_row_head(lv_obj_t *scr, const char *txt, int x, int y, int w)
{
    lv_obj_t *h = wt_lbl(scr, txt, x, y, wt_font14(), WT_MUT);
    lv_obj_set_style_text_letter_space(h, 2, 0);
    lv_obj_set_width(h, w);
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    return h;
}

lv_obj_t *wt_row(lv_obj_t *scr, const char *label, const char *sub,
                 const char *val, lv_color_t vcol, int x, int y, int w,
                 lv_event_cb_t cb, void *ud)
{
    return wt_row_f(scr, label, sub, NULL, val, NULL, vcol, x, y, w, cb, ud);
}

lv_obj_t *wt_row_f(lv_obj_t *scr, const char *label, const char *sub,
                   const lv_font_t *sf, const char *val, const lv_font_t *vf,
                   lv_color_t vcol, int x, int y, int w,
                   lv_event_cb_t cb, void *ud)
{
    return wt_row_x(scr, NULL, label, sub, sf, val, vf, vcol, false,
                    x, y, w, 0, cb, ud);
}

lv_obj_t *wt_row_x(lv_obj_t *scr, const char *icon, const char *label,
                   const char *sub, const lv_font_t *sf,
                   const char *val, const lv_font_t *vf, lv_color_t vcol,
                   bool sel, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud)
{
    // A NULL sf means two different things depending on the height, so the
    // answer has to be taken before the default lands on it: on a standard row
    // it is font14, on a tall one it is "measure the box and pick".
    const bool sf_auto = (sf == NULL);
    const int rowh = h > 0 ? h : WT_ROW_H;
    if (!sf) sf = wt_font14();
    if (!vf) vf = wt_font23();
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, w, rowh);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    // A CARD, not a hairline-separated list line. This used to be a bottom rule
    // and no fill, on the argument that a box around every line turns a list
    // into a stack of cards competing for attention. That argument lost to the
    // drawing, and to the device: redraw 05 gives every row a WT_PANEL fill, a
    // WT_HAIR border and a 10px radius, and the reason is legibility rather than
    // decoration. A label sitting on its own fill has an edge to be read
    // against; the same label floating on the page reads as weak type, which is
    // exactly what the flat list looked like on glass.
    //
    // wt_row_sev() then tints the whole card by severity, which is the other
    // thing the flat list could not do: a warning had to rest on the colour of
    // one small note inside it instead of on the box around it.
    lv_obj_set_style_bg_color(row, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, WT_HAIR, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        wt_tap_feedback(row);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    // The chevron first, so everything else can be measured against it.
    //
    // SELECTED rows get a TICK there instead, in the accent, and the accent on
    // the border. That swap is the whole reason this row exists: a chooser is
    // not navigation. A chevron on STORAGE's three options promises each one
    // leads somewhere, when what they actually do is turn on. A tick answers
    // the only question the screen is asking -- which of these is live -- and
    // it answers it in a mark rather than a word, in every locale at once.
    //
    // Not a filled row. A filled card in a list of three reads as "pressed",
    // and these are not momentary; the accent edge says selected without
    // borrowing the language of a button being held.
    int right = w - 12;
    if (sel) {
        lv_obj_set_style_border_color(row, wt_accent(), 0);
        lv_obj_t *ok = wt_lbl(row, LV_SYMBOL_OK, 0, 0, wt_font23(),
                              wt_accent());
        lv_obj_update_layout(ok);
        lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -10, 0);
        right = w - 10 - lv_obj_get_width(ok) - 10;
    } else if (cb) {
        // WT_DIM, not WT_MUT: the drawing's chevrons are rgb(76,86,102), a rung
        // dimmer than its sub-lines. A chevron is an affordance, not content, so
        // it should be the quietest ink on the card.
        lv_obj_t *ch = wt_lbl(row, LV_SYMBOL_RIGHT, 0, 0, wt_font14(), WT_DIM);
        lv_obj_update_layout(ch);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
        right = w - 10 - lv_obj_get_width(ch) - 10;
    }

    // The icon badge, and the lane every text on the row starts from. 14 with
    // no icon, 52 with one: a 30px glyph at 14 plus an 8px gutter.
    //
    // The badge is a bare glyph and not a chip. A chip around it would be a
    // third box inside a box inside a card, and the row's own border is already
    // doing the framing this needs -- the explainer's badge is a chip precisely
    // because it floats on an open page with nothing else to hold it.
    const int lx = (icon && *icon) ? 52 : 14;
    if (icon && *icon) {
        lv_obj_t *ic = wt_lbl(row, icon, 0, 0, wt_font23(),
                              sel ? wt_accent() : WT_MUT);
        lv_obj_update_layout(ic);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    }

    // The LABEL gets the full width on its own line, and the sub-line below
    // shares its line with the value. The obvious arrangement -- label left,
    // value vertically centred on the right -- does not survive this type
    // scale: a row label at font23 is proportionally far wider than the 15px
    // the drawing used, so "Recovery words live in" beside "FLASH" left the
    // label about 150px and it wrapped into its own sub-line. Full width on top
    // means no label in any locale can collide with a value, and the pairing of
    // a small grey fact with the figure it describes on the same line reads
    // better than the figure floating between two rows of text.
    // The VALUE is built and measured BEFORE the label, so the label's box can
    // exclude it. Sizing the label to the chevron alone let a long label run
    // under a wide value: "Recovery words live in" against "SD CARD" collided
    // where the same row against "FLASH" did not, so the defect only appeared
    // once storage had been migrated.
    // The value keeps its natural size: no width is set, so it lays out on one
    // line at its content width. Do NOT give it a long mode -- LONG_CLIP on a
    // label with no explicit size collapses its height to nothing.
    lv_obj_t *v = NULL;
    int vw = 0;
    if (val && *val) {
        v = wt_lbl(row, val, 0, 0, vf, vcol);
        lv_obj_update_layout(v);
        vw = lv_obj_get_width(v);
    }

    // The label takes the full width up to the chevron and is pinned to ONE
    // line. Pinning is what makes it safe to be that wide: a label allowed to
    // wrap grew a second line and that line landed on the value's row, which is
    // how "Recovery words live in" collided with "SD CARD" while the same row
    // against "FLASH" was clean. One line means the row's internal geometry is
    // the same in every locale, and a translation too long to fit ellipsises
    // rather than rearranging the row.
    lv_obj_t *l = wt_lbl(row, label, lx, sub && *sub ? 7 : 18,
                         wt_font23(), WT_INK);
    // Width stops short of the value, and the height is pinned to one line.
    // Both are needed. Pinning alone left the label's BOX spanning to the
    // chevron, and because the value sits only a few pixels below the label's
    // baseline the two boxes still shared a 2px band that the overlap gate
    // rightly called a collision. Excluding the value's width as well means the
    // two never share a pixel in any locale, whatever the translation does to
    // either one.
    // The label and the sub BOTH stop short of the value, and the value sits
    // vertically centred on the row's right. That is what redraw 05 draws: the
    // value's baseline falls between the label's and the sub's, not on either.
    //
    // Bounding the label horizontally is not optional. The overlap gate compares
    // BOXES, and a label box spanning to the chevron contains the value's box
    // whatever their baselines do, so a full-width label reported a collision
    // with every value on the page. The labels are short enough for this to cost
    // nothing: the one that was not, "Recovery words live in", is "Words live
    // in" now.
    int lw = right - vw - (vw ? 12 : 0) - lx;
    lv_obj_set_width(l, lw > 60 ? lw : 60);
    lv_obj_set_height(l, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_update_layout(l);

    int liney = (sub && *sub) ? 7 + lv_obj_get_height(l) + 3 : 0;

    if (v) {
        lv_obj_set_pos(v, right - vw, (rowh - lv_obj_get_height(v)) / 2);
        right -= vw + 12;
    }

    if (sub && *sub) {
        int sw = right - lx > 40 ? right - lx : 40;
        if (rowh > WT_ROW_H) {
            // A TALL row's sub is a paragraph, not a caption, and it wraps. This
            // is the chooser case: the sentence under "FLASH" is the reason
            // somebody picks "SD CARD" instead, and an ellipsis through it takes
            // out the second half, which is where the warning lives. Sized to
            // the box that is actually left so a three line translation drops a
            // font size rather than running out of the card.
            int sh = rowh - liney - 12;
            if (sh < 20) sh = 20;
            lv_obj_t *s = wt_lbl(row, sub, lx, liney,
                                 sf_auto ? wt_body_font(sub, sw, sh) : sf,
                                 WT_MUT);
            lv_obj_set_width(s, sw);
            lv_obj_set_height(s, sh);
            lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
            lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
        } else {
            lv_obj_t *s = wt_lbl(row, sub, lx, liney, sf, WT_MUT);
            lv_obj_set_width(s, sw);
            // ONE line, height pinned. The width left for the sub depends on how
            // wide the VALUE turned out, and a translated value ("DESACTIVADO"
            // for OFF) squeezes it enough to wrap: the second line then fell past
            // the row's bottom edge and was clipped in seven locales while
            // English was clean. Pinning the height makes LONG_DOT truncate
            // instead of wrap, so the row is the same height whatever the
            // translation does.
            lv_obj_set_height(s, lv_font_get_line_height(sf));
            lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
            lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
        }
    }
    return row;
}

// Recolour just a row's sub-line. For the one case where the explanation is a
// warning and the option is not: FLASH storage on a chip with encryption off is
// a legitimate mode somebody may deliberately want, so the CARD stays neutral
// and only the sentence saying what it costs turns amber. wt_row_sev would wash
// the whole row, and a permanently amber option in a list of three reads as
// broken rather than as cautioned.
//
// By tag, not by child index: a row's children depend on which of the icon, the
// value and the chevron it happened to be given.
void wt_row_sub_color(lv_obj_t *row, lv_color_t c)
{
    lv_obj_t *s = wt_tagged(row, WT_SUB_TAG);
    if (s) lv_obj_set_style_text_color(s, c, 0);
}

// Tint a built row by severity. Redraw 05 colours the BOX, not just a note
// inside it: the green card is a state that is satisfied, the amber one a
// warning about where the words live, the red pair the two actions that cannot
// be undone. Measured off the drawing, which uses a 4 to 5 percent fill under a
// 30 percent border of the same hue -- barely a wash, but enough that the group
// reads before any of its words do.
//
// Opacities are LV_OPA values (0..255): 13 is the 5 percent fill, 77 the 30
// percent border. Status hues are never themed, so this takes no accent.
void wt_row_sev(lv_obj_t *row, int sev)
{
    if (!row) return;
    lv_color_t c;
    switch (sev) {
        case WT_SEV_OK:   c = WT_OK;   break;
        case WT_SEV_WARN: c = WT_WARN; break;
        case WT_SEV_STOP: c = WT_STOP; break;
        default:
            lv_obj_set_style_bg_color(row, WT_PANEL, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(row, WT_HAIR, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
            return;
    }
    lv_obj_set_style_bg_color(row, c, 0);
    lv_obj_set_style_bg_opa(row, 13, 0);
    lv_obj_set_style_border_color(row, c, 0);
    lv_obj_set_style_border_opa(row, 77, 0);
}

// The box every Settings row is made of, with nothing in it. Extracted so the
// screens whose content is not a ROW can still wear it: an address block, a
// camera viewport, a status line. Same fill, same hairline, same 10px radius, so
// a card on Receive and a card on Settings are the same object to the eye, and
// wt_row_sev() tints either one.
//
// The fill is what does the work. Type on WT_PANEL has an edge to be read
// against; the same type floating on WT_BG reads as weak, which is the whole
// difference between the settings screen the owner liked and the flat list it
// replaced. Any screen with a block of content and no card is the flat list
// again under a different name.
lv_obj_t *wt_card(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, WT_HAIR, 0);
    lv_obj_set_style_bg_color(card, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// A camera viewport: the card, a WT_EDGE edge, and four bracket corners drawn
// OUTSIDE it. Outside is the whole point. On the device the camera writes
// straight to the panel over exactly this rectangle, so anything inside these
// bounds is gone the moment the first frame lands; the brackets live in LVGL's
// own pixels and survive. They are also what tells an empty preview from a
// rendering fault, which is what both camera screens look like in the simulator
// and on a device whose camera failed to start.
lv_obj_t *wt_viewfinder(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *vp = wt_card(scr, x, y, w, h);
    lv_obj_set_style_border_color(vp, WT_EDGE, 0);

    const int L = 16, T = 2, G = 5;      // arm length, thickness, gap from the box
    const int x0 = x - G, y0 = y - G, x1 = x + w + G, y1 = y + h + G;
    // { x, y, w, h } per arm, two arms per corner, clockwise from top left.
    const int arm[8][4] = {
        { x0,     y0,     L, T }, { x0,     y0,     T, L },
        { x1 - L, y0,     L, T }, { x1 - T, y0,     T, L },
        { x1 - L, y1 - T, L, T }, { x1 - T, y1 - L, T, L },
        { x0,     y1 - T, L, T }, { x0,     y1 - L, T, L },
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *a = lv_obj_create(scr);
        lv_obj_remove_style_all(a);
        lv_obj_set_pos(a, arm[i][0], arm[i][1]);
        lv_obj_set_size(a, arm[i][2], arm[i][3]);
        lv_obj_set_style_bg_color(a, WT_EDGE, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    }
    return vp;
}

lv_obj_t *wt_value_card(lv_obj_t *scr, const char *cap, const char *val,
                        int x, int y, int w, bool big)
{
    lv_obj_t *card = wt_card(scr, x, y, w, 0);

    lv_obj_t *c = wt_lbl(card, cap, 16, 12, wt_font14(), WT_MUT);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_width(c, w - 32);
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(c);

    int vy = 12 + lv_obj_get_height(c) + 8;
    lv_obj_t *v = wt_lbl(card, val, 16, vy,
                         big ? wt_font_mono28() : wt_font_mono23(), WT_INK);
    lv_obj_set_style_text_letter_space(v, 2, 0);
    lv_obj_update_layout(v);
    // Sized to its content, never to a guess: the caption is translated and the
    // value can be four characters or forty.
    lv_obj_set_size(card, w, vy + lv_obj_get_height(v) + 14);
    return card;
}

// Change the value on a card that is already up, without rebuilding it.
//
// The firmware WRITING screen used to delete and recreate its card on every
// percent, which invalidates the card's whole rectangle a hundred times during
// a write that already has the LVGL task blocked. Setting the text dirties
// only the glyphs that changed. The caption and the geometry are the card's
// and do not move: a percent is the same width at 9% and 99% in the mono font
// this draws in.
void wt_value_card_set(lv_obj_t *card, const char *val)
{
    if (!card || lv_obj_get_child_count(card) < 2) return;
    lv_label_set_text(lv_obj_get_child(card, 1), val);
}


// The body font for a PAIR of blocks that must share one size. Taking the
// smaller of the two rungs, because they render side by side and the taller
// half decides whether either of them fits.
//
// strlen is not a substitute for this and was the bug: the passphrase intro
// picked its font from whichever body had more BYTES, and a longer string that
// happens to wrap short chose a size the shorter one could not survive. Italian
// went 7px past WT_CONTENT_BOTTOM the moment a translation changed length.
const lv_font_t *wt_body_font2(const char *a, const char *b, int w, int max_h)
{
    const lv_font_t *fa = wt_body_font(a, w, max_h);
    const lv_font_t *fb = wt_body_font(b, w, max_h);
    if (fa == wt_font14() || fb == wt_font14()) return wt_font14();
    if (fa == wt_font23() || fb == wt_font23()) return wt_font23();
    return fa;
}

lv_obj_t *wt_why_block(lv_obj_t *scr, const char *head, const char *body,
                       int x, int y, int w, int max_h, const lv_font_t *f,
                       lv_color_t col)
{
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // The heading is optional. A block split out of an existing explainer
    // paragraph has no heading to give it, and inventing one would mean a new
    // string in twenty one locales for decoration.
    int by = 0;
    if (head && *head) {
        lv_obj_t *h = wt_lbl(box, head, 14, 0, wt_font14(), WT_INK);
        lv_obj_set_width(h, w - 14);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(h);
        by = lv_obj_get_height(h) + 6;
    }

    // The body takes the largest size that fits the room it was given, unless the
    // caller has already chosen one for a GROUP of blocks. Fixing it at font14
    // would make a card of two short paragraphs render in the smallest type the
    // device owns, which is the mistake this makeover started by making.
    lv_obj_t *b = wt_lbl(box, body, 14, by,
                         f ? f : wt_body_font(body, w - 14, max_h - by), WT_MUT);
    lv_obj_set_width(b, w - 14);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(b);

    int hgt = by + lv_obj_get_height(b);
    lv_obj_set_size(box, w, hgt);
    // The rule last and sized to the measured text, so it always matches the
    // block's real height in whatever locale is rendering.
    lv_obj_t *rule = lv_obj_create(box);
    lv_obj_remove_style_all(rule);
    lv_obj_set_pos(rule, 0, 0);
    lv_obj_set_size(rule, 3, hgt);
    lv_obj_set_style_radius(rule, 2, 0);
    lv_obj_set_style_bg_color(rule, col, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

// ---- the explainer card ----
// Every "?" on the device opens one of these, and they were all the same thing:
// a centred title over a centred paragraph, floating in the middle of a dimmed
// screen. The design handoff draws them as a page like any other -- title top
// left, the value the card is about in a bordered box, and the prose as two
// claims with coloured rules beside them rather than one block nobody finishes.
//
// The two-column split costs NOTHING in translation, which is the only reason it
// is affordable: the explainer bodies were already written as two or three
// paragraphs separated by a blank line, in all twenty one locales, so this
// splits a string that exists rather than asking for a string that does not.
// Anything past the second paragraph joins the second block, so a three
// paragraph body stays two columns instead of inventing a third.
static void explain_close_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

const char *wt_split_colon(const char *line, char *head, size_t head_len)
{
    if (!line || !head || head_len == 0) return NULL;
    const char *c = NULL;
    size_t skip = 0;
    for (const char *p = line; *p; p++) {
        if (*p == ':') { c = p; skip = 1; break; }
        // U+FF1A FULLWIDTH COLON, EF BC 9A
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC &&
            (unsigned char)p[2] == 0x9A) { c = p; skip = 3; break; }
    }
    if (!c) {
        snprintf(head, head_len, "%s", line);
        return NULL;
    }
    size_t n = (size_t)(c - line);
    while (n && line[n - 1] == ' ') n--;     // "locktime :" in French
    if (n >= head_len) n = head_len - 1;
    lv_memcpy(head, line, n);
    head[n] = 0;
    const char *tail = c + skip;
    while (*tail == ' ') tail++;
    return *tail ? tail : NULL;
}

// ---- how the body is laid out ----
// The old rule was: split at the FIRST blank line, paragraph one left, everything
// else right, both measured against a 330px column. Three faults, and they
// compounded. It never considered the full width lane, so a body that would have
// read at font28 across 704 was measured against 330 and dropped a size. It split
// by position rather than by length, so the home fingerprint card -- whose body
// grows a third paragraph for the escape hint -- put four words in the left
// column and two paragraphs in the right. And the pair share one font by design,
// chosen by the taller half, so those four words rendered at font14 beside a
// column that needed it. That is the "tiny text" the device showed.
//
// Now both arrangements are measured at every size and the first that fits wins.
#define EXP_MAX_PARA 6
#define EXP_FULL_W   704
#define EXP_COL_W    344
// Measure against the TEXT lane, not the block. wt_why_block spends 14 on the
// coloured rule and its gutter, so a measurement taken at the block's own width
// comes back short and the last line lands under the OK pill. That is not
// hypothetical: measuring the full lane at 704 instead of 690 put the silent
// payment card 19px past WT_CONTENT_BOTTOM in five locales.
#define EXP_RULE_W   14
#define EXP_FULL_TXT (EXP_FULL_W - EXP_RULE_W)
#define EXP_COL_TXT  (EXP_COL_W - EXP_RULE_W)

typedef struct {
    const char *p[EXP_MAX_PARA];
    int         n[EXP_MAX_PARA];    // byte length of each
    int         count;
} exp_paras_t;

static void exp_split(const char *body, exp_paras_t *o)
{
    o->count = 0;
    const char *s = body;
    while (s && *s && o->count < EXP_MAX_PARA) {
        const char *brk = strstr(s, "\n\n");
        // The last slot swallows whatever is left, so a seven paragraph string
        // still renders whole rather than losing its tail.
        if (!brk || o->count == EXP_MAX_PARA - 1) {
            o->p[o->count] = s;
            o->n[o->count] = (int)strlen(s);
            o->count++;
            return;
        }
        o->p[o->count] = s;
        o->n[o->count] = (int)(brk - s);
        o->count++;
        s = brk + 2;
    }
}

// Height of paragraphs [a, b) joined by blank lines, at font f and width w.
static int exp_height(const exp_paras_t *ps, int a, int b, const lv_font_t *f, int w)
{
    int h = 0;
    for (int i = a; i < b; i++) {
        lv_point_t sz;
        char buf[512];
        int n = ps->n[i];
        if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
        lv_memcpy(buf, ps->p[i], (size_t)n);
        buf[n] = 0;
        lv_text_get_size(&sz, buf, f, 0, 0, w, LV_TEXT_FLAG_NONE);
        h += sz.y;
        if (i + 1 < b) h += lv_font_get_line_height(f);   // the blank line back
    }
    return h;
}

// Copy paragraphs [a, b) back into one string, blank line separated, so
// wt_why_block sees the same shape the locale wrote.
static void exp_join(const exp_paras_t *ps, int a, int b, char *out, size_t len)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = a; i < b && o + 1 < len; i++) {
        int n = ps->n[i];
        if (o + (size_t)n + 3 >= len) n = (int)(len - o - 3);
        if (n < 0) break;
        lv_memcpy(out + o, ps->p[i], (size_t)n);
        o += (size_t)n;
        if (i + 1 < b && o + 2 < len) { out[o++] = '\n'; out[o++] = '\n'; }
    }
    out[o] = 0;
}


// Lay a body out as ruled blocks in the room between `y` and WT_CONTENT_BOTTOM,
// choosing the arrangement and the font size that read best: full width when the
// text can earn the 704 lane, two balanced columns otherwise, dropping a rung
// before it ever overflows. `sev` colours the first block, WT_MUT the second.
//
// This was the private guts of wt_explain_open and it is public now because the
// explainer cards were not the only screens with a wall of grey text in them.
// Nine more screens had one, and giving each its own hand placed layout is how
// the walls got there in the first place. The explainer card calls this too, so
// there is one body layout on the device rather than ten.
//
// It costs NOTHING in translation: every one of these bodies is already written
// as two or three paragraphs separated by a blank line in all 21 locales, so
// this splits a string that exists rather than asking for one that does not.
void wt_why_body(lv_obj_t *par, const char *body, int y, lv_color_t sev,
                 bool two_col)
{
    if (!par || !body || !*body) return;
    int room = WT_CONTENT_BOTTOM - y;
    if (room < 40) room = 40;

exp_paras_t ps;
    exp_split(body, &ps);

    const lv_font_t *ladder[3];
    ladder[0] = wt_font28(); ladder[1] = wt_font23(); ladder[2] = wt_font14();

    const lv_font_t *f = wt_font14();
    int split_at = 0;                 // 0 = one full width block
    int used = room;

    for (int r = 0; r < 3; r++) {
        const lv_font_t *cand = ladder[r];

        // Full width first. It reads better than two columns and it is the
        // only arrangement that can use the whole 704 lane, but a two line
        // answer stretched across the page leaves a hole under itself, so it
        // has to earn the lane by filling at least half the room. A single
        // paragraph takes it regardless: there is nothing to split.
        // two_col is what a full SCREEN asks for and a card does not. The
        // "earn the lane" rule below was tuned for the explainer overlay, which
        // has little room, so a body nearly always fills half of it and takes
        // the full width. A screen has ~280px and three paragraphs fill it
        // easily, so the same rule produced the exact wall of grey text these
        // screens were being rebuilt to stop being. With 2+ paragraphs a screen
        // goes straight to the two columns the reveal screen established.
        int hf = exp_height(&ps, 0, ps.count, cand, EXP_FULL_TXT);
        if (!(two_col && ps.count >= 2) &&
            hf <= room && (ps.count == 1 || hf * 2 >= room)) {
            f = cand; split_at = 0; used = hf;
            break;
        }

        // Otherwise deal the paragraphs into two columns at the boundary
        // that makes them most nearly equal, rather than always after the
        // first. Balanced columns are what let the pair share a bigger font:
        // they share one by design, and one shares badly when one half is
        // four words and the other is two paragraphs.
        if (ps.count >= 2) {
            int best = 1, best_gap = -1, best_tall = 0;
            for (int k = 1; k < ps.count; k++) {
                int hl = exp_height(&ps, 0, k, cand, EXP_COL_TXT);
                int hr = exp_height(&ps, k, ps.count, cand, EXP_COL_TXT);
                int gap = hl > hr ? hl - hr : hr - hl;
                if (best_gap < 0 || gap < best_gap) {
                    best_gap = gap; best = k; best_tall = hl > hr ? hl : hr;
                }
            }
            if (best_tall <= room) {
                f = cand; split_at = best; used = best_tall;
                break;
            }
        }

        // Nothing fits at font14 either: keep it, clamp, and let
        // wt_why_block's own bounds do the rest. Better a full card of the
        // smallest type than a card that silently drops its second half.
        if (r == 2) {
            f = cand;
            split_at = ps.count >= 2 ? 1 : 0;
            used = room;
        }
    }

    // Drop the band by a third of what is left over. Centring it outright
    // floats the text away from the title it answers; hugging the top, which
    // is what this did before, leaves the whole bottom of the card empty.
    int slack = room - used;
    if (slack > 0) { y += slack / 3; room -= slack / 3; }

    if (split_at) {
        char left[640], right[640];
        exp_join(&ps, 0, split_at, left, sizeof left);
        exp_join(&ps, split_at, ps.count, right, sizeof right);
        wt_why_block(par, NULL, left,  48, y, EXP_COL_W, room, f, sev);
        wt_why_block(par, NULL, right, 408, y, EXP_COL_W, room, f, WT_MUT);
    } else {
        wt_why_block(par, NULL, body, 48, y, EXP_FULL_W, room, f, sev);
    }
}

// ---- WT_GRID_ICONS ----
// A glossary is a LIST, and it was being rendered as prose: eight lines of the
// same grey at the same size, so the eight terms it defines had to be read in
// order to find any one of them. Every line is written `TERM: definition` in all
// 21 locales, so an icon badge and a heading can be lifted straight out of the
// string that already exists. Nothing new to translate.
#define GRID_COLS   2
#define GRID_MAXN   12
#define GRID_BADGE  34
#define GRID_GUT    12
// One entry, in bytes. The longest today is the Russian dust attack line at 189
// and Cyrillic runs two bytes a character, so a 192 byte buffer would truncate
// the next translation that grows -- mid codepoint, because the copy below is a
// byte copy. Sized so the line the check gate measures is the line drawn.
#define GRID_LINE_MAX 256

static void grid_badge(lv_obj_t *par, const char *glyph, int x, int y,
                       lv_color_t col)
{
    lv_obj_t *b = lv_obj_create(par);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, GRID_BADGE, GRID_BADGE);
    lv_obj_set_style_radius(b, GRID_BADGE / 2, 0);
    lv_obj_set_style_bg_color(b, WT_KEY, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, WT_EDGE, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(wt_lbl(b, glyph, 0, 0, wt_font14(), col));
}

static void explain_grid(lv_obj_t *ovl, const wt_explain_t *e, int y, int room,
                         lv_color_t sev)
{
    // One line per entry, and NOT a count of eight: a locale is free to ship
    // seven or nine and the grid has to draw what it was handed.
    const char *ln[GRID_MAXN];
    int len[GRID_MAXN], n = 0;
    for (const char *s = e->body; s && *s && n < GRID_MAXN; ) {
        const char *nl = strchr(s, '\n');
        ln[n] = s;
        len[n] = nl ? (int)(nl - s) : (int)strlen(s);
        n++;
        if (!nl) break;
        s = nl + 1;
    }
    if (!n) return;

    int rows = (n + GRID_COLS - 1) / GRID_COLS;
    int cw   = (EXP_FULL_W - GRID_GUT) / GRID_COLS;      // 346
    int tw   = cw - GRID_BADGE - GRID_GUT;               // text lane beside it
    int pitch = room / rows;

    // ONE font for every definition, chosen against the tallest cell, so the
    // eight cells read as one table. Sized per cell they would stagger, which is
    // the fault the two-column prose blocks had.
    const lv_font_t *bf = wt_font23();
    for (int pass = 0; pass < 2; pass++) {
        int tallest = 0;
        for (int i = 0; i < n; i++) {
            char line[GRID_LINE_MAX], head[64];
            int l = len[i] < (int)sizeof line ? len[i] : (int)sizeof line - 1;
            lv_memcpy(line, ln[i], (size_t)l);
            line[l] = 0;
            const char *def = wt_split_colon(line, head, sizeof head);
            if (!def) continue;
            lv_point_t sz;
            lv_text_get_size(&sz, def, bf, 0, 0, tw, LV_TEXT_FLAG_NONE);
            int h = lv_font_get_line_height(wt_font14()) + 2 + sz.y;
            if (h > tallest) tallest = h;
        }
        if (tallest <= pitch - 6) break;
        bf = wt_font14();
    }

    for (int i = 0; i < n; i++) {
        char line[GRID_LINE_MAX], head[64];
        int l = len[i] < (int)sizeof line ? len[i] : (int)sizeof line - 1;
        lv_memcpy(line, ln[i], (size_t)l);
        line[l] = 0;
        const char *def = wt_split_colon(line, head, sizeof head);

        int cx = 48 + (i % GRID_COLS) * (cw + GRID_GUT);
        int cy = y + (i / GRID_COLS) * pitch;

        if (e->icons && e->icons[i])
            grid_badge(ovl, e->icons[i], cx, cy, sev);

        int tx = cx + GRID_BADGE + GRID_GUT;
        lv_obj_t *t = wt_lbl(ovl, head, tx, cy + 2, wt_font14(), sev);
        lv_obj_set_style_text_letter_space(t, 1, 0);
        lv_obj_set_width(t, tw);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

        if (!def) continue;
        lv_obj_t *d = wt_lbl(ovl, def, tx,
                             cy + 2 + lv_font_get_line_height(wt_font14()) + 2,
                             bf, WT_MUT);
        lv_obj_set_width(d, tw);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    }
}

lv_obj_t *wt_explain_open(lv_obj_t *parent, const wt_explain_t *e)
{
    if (!parent || !e) return NULL;

    lv_obj_t *ovl = lv_obj_create(parent);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, WT_BG, 0);
    lv_obj_set_style_bg_opa(ovl, 245, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);          // swallow stray taps
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ovl, explain_close_cb, LV_EVENT_CLICKED, ovl);

    // The title, and the first block's rule, carry the card's severity. A
    // caution explainer opened from an amber row should not arrive wearing the
    // accent: the colour is what says which of the eleven cards you are on
    // before a word of it is read.
    lv_color_t sev = e->sev == WT_SEV_OK   ? WT_OK
                   : e->sev == WT_SEV_WARN ? WT_WARN
                   : e->sev == WT_SEV_STOP ? WT_STOP : wt_accent();

    // The subject badge, right of the title row. An icon is worth more than the
    // word it replaces only if it is the SAME icon the reader met on the screen
    // that sent them here, so callers pass the one their control already wears.
    const int icon_w = 48;
    if (e->icon && *e->icon) {
        lv_obj_t *chip = lv_obj_create(ovl);
        lv_obj_remove_style_all(chip);
        lv_obj_set_pos(chip, 752 - icon_w, 20);
        lv_obj_set_size(chip, icon_w, icon_w);
        lv_obj_set_style_radius(chip, icon_w / 2, 0);
        lv_obj_set_style_bg_color(chip, WT_KEY, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, WT_EDGE, 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *g = wt_lbl(chip, e->icon, 0, 0, wt_font28(), sev);
        lv_obj_center(g);
    }

    // Title and subtitle exactly where wt_screen puts them, because an explainer
    // is a page and should not announce itself as a different kind of object.
    int lane = e->icon && *e->icon ? 704 - icon_w - 16 : 704;
    lv_obj_t *t = wt_lbl(ovl, e->title, 48, 18, wt_font28(), sev);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_set_width(t, lane);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

    int y = 76;
    if (e->sub && *e->sub) {
        lv_obj_t *s = wt_lbl(ovl, e->sub, 48, 64, wt_body_font(e->sub, lane, 29),
                             WT_MUT);
        lv_obj_set_width(s, lane);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        y = 104;
    }

    // Band one: the value this card is about, and whatever diagram the caller
    // draws. Either may be absent; with both, they share the row.
    int band = 0;
    bool has_val = e->val && *e->val;
    if (has_val) {
        lv_obj_t *vc = wt_value_card(ovl, e->cap ? e->cap : "", e->val,
                                     48, y, e->aside ? 340 : 704, true);
        lv_obj_update_layout(vc);
        band = lv_obj_get_height(vc);
    }
    if (e->aside) {
        int ax = has_val ? 408 : 48, aw = has_val ? 344 : 704;
        int ah = e->aside(ovl, ax, y, aw);
        if (ah > band) band = ah;
    }
    if (band) y += band + 20;

    // Band two: the body. WT_CONTENT_BOTTOM is the floor and everything is
    // measured against what is left above it, so a long translation drops a font
    // size instead of running under the OK pill.
    if (e->body && *e->body) {
        int room = WT_CONTENT_BOTTOM - y;
        if (room < 40) room = 40;

        if (e->mode == WT_GRID_ICONS) {
            explain_grid(ovl, e, y, room, sev);
        } else {
        wt_why_body(ovl, e->body, y, sev, false);
        }
    }

    lv_obj_t *ok = wt_pill(ovl, e->ok_txt, 300, WT_ACTION_Y, 200,
                           explain_close_cb, ovl);
    lv_obj_remove_flag(ok, LV_OBJ_FLAG_IGNORE_LAYOUT);
    wt_card_intro(ovl);
    return ovl;
}

// One label carrying its own bordered box, not a container plus a child: LVGL
// labels take border and background styles, so LV_SIZE_CONTENT plus padding
// gives a chip that measures itself against whatever the translation turns out
// to be. A container with a child label needs two layout passes and was the
// shape that segfaulted the first time this was tried.
lv_obj_t *wt_state_chip(lv_obj_t *par, const char *txt, lv_color_t col)
{
    lv_obj_t *c = lv_label_create(par);
    lv_obj_set_style_text_font(c, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_style_radius(c, 100, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_hor(c, 12, 0);
    lv_obj_set_style_pad_ver(c, 5, 0);
    lv_obj_set_style_bg_opa(c, 13, 0);        // ~5 percent
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    wt_state_chip_set(c, txt, col);
    return c;
}

void wt_state_chip_set(lv_obj_t *chip, const char *txt, lv_color_t col)
{
    if (!chip) return;
    lv_label_set_text(chip, txt ? txt : "");
    lv_obj_set_style_text_color(chip, col, 0);
    lv_obj_set_style_border_color(chip, col, 0);
    lv_obj_set_style_bg_color(chip, col, 0);
    lv_obj_update_layout(chip);
}

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
    // The whole stagger fits in a fixed window, however many children there
    // are. It used to be a flat 60ms per child, which was fine for a card of
    // four and became a 900ms drip once the explainer grew an icon badge, a
    // value card, a diagram and two blocks: the OK button arrived a second
    // after the title. Spreading a constant budget keeps the cadence and caps
    // the wait.
    uint32_t n = lv_obj_get_child_count(card);
    uint32_t step = n > 1 ? 300 / (n - 1) : 0;
    if (step > 60) step = 60;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(card, i);
        lv_obj_set_style_opa(ch, 0, 0);
        lv_obj_set_style_translate_y(ch, 14, 0);
        uint32_t delay = 40 + i * step;
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

// Each term carries its own mark, because this equation is read at a glance or
// not at all: three same-shaped word chips are three things to READ before the
// arrow means anything, and the reader is here precisely because words did not
// land the first time. The list, the lock and the key say the whole sentence
// before the labels are parsed, and the labels stay under them because three
// unlabelled icons would be a riddle rather than a shortcut.
//
// All three codepoints are already in SYMS in tools/fonts/gen_fonts.sh at every
// size a chip can take, so this costs nothing in flash and needs no font rebuild
// — and the key is the same mark the card's own badge carries, which is what
// ties the answer to the question the reader tapped.
static void wt_chip_icon(lv_obj_t *row, const char *icon, const char *txt,
                         bool accent)
{
    char buf[WT_ICON_TEXT_MAX];
    snprintf(buf, sizeof buf, "%s %s", icon, txt);
    wt_chip(row, buf, accent);
}

void wt_diagram_fp(lv_obj_t *parent)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip_icon(row, LV_SYMBOL_LIST, tr(STR_D_WORDS), false);
    wt_diagram_op(row, "+");
    wt_chip_icon(row, WT_ICON_LOCK, tr(STR_D_PASSPHRASE), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip_icon(row, WT_ICON_KEY, tr(STR_D_FINGERPRINT), true);
}

// What a backup check actually claims: RECOVERY WORDS -> THIS WALLET. Two chips
// and one arrow, because the whole screen is one assertion and a longer diagram
// would be inventing steps to look busy.
//
// Both labels already exist in every locale (STR_D_WORDS is the fingerprint
// equation's own first chip, so the two diagrams agree on what words are
// called), and both marks are in SYMS at every chip size. The tick is accented
// because the match is the outcome being proven, the same way the fingerprint
// equation accents its result.
//
// STR_I_T and not STR_I_SEC_THIS_WALLET, which reads better in English and is
// the string this was first written with: that key is the literal text "THIS
// WALLET" in all twenty one locales, so on a Russian screen it sat in English
// between two translated chips. It is a section HEADING everywhere else it
// appears, where nobody had noticed; in a diagram beside translated words it is
// obvious. STR_I_T is the WALLET page's own title and is properly localised.
void wt_diagram_verify(lv_obj_t *parent)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip_icon(row, LV_SYMBOL_LIST, tr(STR_D_WORDS), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip_icon(row, LV_SYMBOL_OK, tr(STR_I_T), true);
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
