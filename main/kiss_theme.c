// Shared wallet UI kit. See kiss_theme.h. Every builder here matches the
// house style the screens shipped with, so porting a screen to the kit must
// not change a rendered pixel while the accent is MONO.
#include "kiss_theme.h"

#include <stdio.h>
#include <stdlib.h>
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
static const char WT_ROW_ICON_TAG[] = "wt_row_icon";
// The wide row's label, so the "?" chip can be measured against the TEXT
// rather than the 250px box the label is capped to. Asking the object how
// wide it is answers 250 and puts the chip on top of the words.
static const char WT_ROW_LABEL_TAG[] = "wt_row_label";
// The wide row's CONTROL -- the chip a value sits in, or the value itself on a
// row that only opens a screen. Tagged so the page can land it a beat after
// the row it belongs to without knowing which of the four kinds built it.
static const char WT_ROW_CTRL_TAG[] = "wt_row_ctrl";

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

// The sink from kiss_theme.h. NULL on device and in any host build that has not
// asked, so this costs a null check on a path that already measured text.
#ifndef ESP_PLATFORM
static wt_fit_sink_t s_fit_sink;
void wt_fit_set_sink(wt_fit_sink_t fn) { s_fit_sink = fn; }
#define WT_FIT_GAVE_UP(kind_, txt_, w_, h_) \
    do { if (s_fit_sink) s_fit_sink((kind_), (txt_), (w_), (h_)); } while (0)

static wt_cut_sink_t s_cut_sink;
void wt_cut_set_sink(wt_cut_sink_t fn) { s_cut_sink = fn; }
// Measured before the label is handed the string, because LVGL replaces the
// text with the dotted form and the original is unrecoverable afterwards.
static void wt_sub_measure(const char *txt, const lv_font_t *f, int lane)
{
    if (!s_cut_sink || !txt || !*txt) return;
    lv_point_t sz;
    lv_text_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > lane) s_cut_sink(txt, (int)sz.x, lane);
}
#else
#define WT_FIT_GAVE_UP(kind_, txt_, w_, h_) ((void)0)
#define wt_sub_measure(txt_, f_, lane_) ((void)0)
#endif

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
    // Past here the label is going on a button in the smallest type the device
    // owns, which is the thing the comment at the top of this function says
    // nobody should ever see. Say so.
    WT_FIT_GAVE_UP("pill", txt, bw, bh);
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
//
// The radius is named because the action bar has to reproduce it: the bar fills
// the bottom of this card, so anything that changes the curve here has to change
// the curve there in the same edit or the boundary breaks at the two corners.
#define WT_CARD_R 16
static void screen_card(lv_obj_t *scr)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 8, 8);
    lv_obj_set_size(card, 784, 464);
    lv_obj_set_style_radius(card, WT_CARD_R, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, WT_EDGE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

#ifdef SIMULATOR
// ---- screen coverage, simulator only ----
//
// A layout gate that never draws a screen cannot have an opinion about it.
// whatseed_open was a title, a subtitle and one 704x232 paragraph -- BARE by
// rule 1, on the screen a newcomer opens to find out what a seed is -- for its
// entire life, and every gate reported clean the whole time, because no walk
// stop ever rendered it. It was found by accident, the day it got a stop.
//
// So the walk records both halves: which screen titles it BUILT, and which of
// those a save() actually captured for the gate to question. The difference is
// the list of screens nothing has ever checked. Titles are the key because
// they are what wt_screen already has and what identifies a screen to a
// reader; a screen with no translated title is skipped rather than guessed at.
static uint8_t s_wt_built[STR_N];
static uint8_t s_wt_captured[STR_N];
static int     s_wt_cur = -1;

// Pointer identity, not strcmp: tr() hands back the table entry itself, so the
// match is exact and costs no string compares. English is checked second
// because tr() falls back to it for a key a locale has not filled in.
static int wt_title_id(const char *title)
{
    if (!title || !*title) return -1;
    const char *const *tbl = i18n_tables[i18n_get_lang()];
    for (int i = 0; i < STR_N; i++) if (tbl[i] == title) return i;
    const char *const *en = i18n_tables[I18N_EN];
    for (int i = 0; i < STR_N; i++) if (en[i] == title) return i;
    return -1;   // a literal title: nothing to name it by, so not tracked
}

// Called by the walk's save(). The active screen is the most recently built
// one -- mk_screen deletes its predecessor -- and an overlay saved on top of a
// screen still means that screen was on the panel, which is what is being
// claimed.
void wt_sim_capture(void)
{
    if (s_wt_cur >= 0) s_wt_captured[s_wt_cur] = 1;
}

// The English title, which names the screen to a reader better than a key
// would. The generated header carries no key-name table and this needs no new
// one: the title IS how anyone refers to the screen.
const char *wt_sim_title_key(int id)
{
    if (id < 0 || id >= STR_N) return "?";
    const char *s = i18n_tables[I18N_EN][id];
    return s ? s : "?";
}

// Fills `out` with the ids of screens built but never captured. Returns how
// many there were, which may exceed max.
int wt_sim_uncaptured(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < STR_N; i++) {
        if (s_wt_built[i] && !s_wt_captured[i]) {
            if (n < max) out[n] = i;
            n++;
        }
    }
    return n;
}

// Every title the walk built at all. This is the other half of the question:
// "built but never captured" cannot see a screen the walk never opens, and
// that is exactly the state whatseed was in. tools/check_screen_coverage.py
// diffs this against the title keys in the source.
int wt_sim_built(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < STR_N; i++) {
        if (s_wt_built[i]) {
            if (n < max) out[n] = i;
            n++;
        }
    }
    return n;
}
#endif

lv_obj_t *wt_screen(lv_obj_t *parent, const char *title, const char *sub)
{
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, WT_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    // NOT scrollable, which lv_obj_create makes it by default. No page in this
    // app scrolls -- every screen is laid out absolutely and overlapcheck fails
    // anything below WT_CONTENT_BOTTOM, so a scroll offset can only ever be
    // damage. What it actually cost: LVGL hands a press to the nearest
    // SCROLLABLE ancestor as soon as the finger moves past its scroll limit,
    // and sends PRESS_LOST to whatever was under it. Both screens where the
    // owner DRAWS -- the duress stroke and their own letters -- are a
    // transparent catcher on a wt_screen, so every stroke was being stolen a
    // few pixels in and the ink stopped following the finger. Neither screen
    // had a walk stop, so no gate had ever drawn on either of them.
    // Scrolling lists (kiss_recv.c's address list) set the flag on their own
    // container and are untouched by this.
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(scr);
    screen_card(scr);
#ifdef SIMULATOR
    s_wt_cur = wt_title_id(title);
    if (s_wt_cur >= 0) s_wt_built[s_wt_cur] = 1;
#endif

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

// ---- the action bar (see kiss_theme.h) ----
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
    // takes the fill down to the card's inner bottom edge at 471.
    //
    // Rounded to WT_CARD_R - 1. The card's border is 1px, so the hole it encloses
    // has radius 15 and its bottom corners curve from (9,455) to (24,470); a
    // square fill covering that band paints a solid shoulder OVER the arc, which
    // is why the screen border used to run down both sides, stop dead, and pick
    // up again 11px along the bottom. The corner was never missing, it was
    // buried. Matching the hole's radius exactly puts the fill inside the curve
    // instead of across it.
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_user_data(bar, (void *)WT_BAR_TAG);
    lv_obj_set_pos(bar, 9, WT_CONTENT_BOTTOM);
    lv_obj_set_size(bar, 782, 471 - WT_CONTENT_BOTTOM);
    lv_obj_set_style_radius(bar, WT_CARD_R - 1, 0);
    lv_obj_set_style_bg_color(bar, WT_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Not clickable and not scrollable: it is a surface, and a tap that misses a
    // button must fall through to whatever is behind rather than being eaten.
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // A radius rounds all four corners, and the bar's TOP corners are in open
    // content, not on the card edge -- rounding them bites two notches of page
    // background out of the band and bends the ends of its hairline. So the top
    // rung of the fill is squared off by a second slab drawn over it, which also
    // carries the hairline. Two flat objects rather than clip_corner on the card:
    // clip_corner refreshes the children into an ARGB8888 layer 784px wide, and
    // this panel's draw pool cannot hand out a buffer that size -- lv_draw_dispatch
    // would retry the allocation forever, the same hang transform_scale causes.
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_pos(top, 9, WT_CONTENT_BOTTOM);
    lv_obj_set_size(top, 782, WT_CARD_R - 1);
    lv_obj_set_style_bg_color(top, WT_BAR, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(top, WT_HAIR, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_TOP, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
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
    // A chip painted in the accent has to say so on BOTH channels, or the rim
    // and the glyph drift apart at the next theme change. Guarded on the
    // colour rather than a parameter: callers pass a status colour here too,
    // and a status colour must never be repainted by a theme.
    const bool acc = lv_color_eq(color, wt_accent());
    if (acc) lv_obj_add_flag(chip, WT_FLAG_ACCENT_BORDER);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(chip, 12);       // 54px effective target
    wt_tap_feedback(chip);
    if (cb) lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, color, 0);
    if (acc) lv_obj_add_flag(label, WT_FLAG_ACCENT);
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

// ---- hold to confirm (see kiss_theme.h) ----
// One press cannot fire it and neither can two: the finger has to stay down.
// State hangs off the pill so several could coexist, and the timer is deleted
// on release AND on delete, so a screen torn down mid-hold leaves nothing.
typedef struct {
    lv_obj_t *pill, *fill;
    lv_timer_t *tmr;
    uint32_t t0;
    int ms, w;
    // wt_hold_rule only: the label to swap, the two words to swap between, and
    // how long the fill takes to run back. Zero release_ms is wt_hold_pill,
    // which clears its sweep in a frame because a pill already looks pressed.
    lv_obj_t *lbl, *arrow;
    const char *txt, *held;
    int release_ms;
    void (*done)(void *);
    void *ud;
} wt_hold_t;

static void an_w(void *v, int32_t w) { lv_obj_set_width(v, w); }

// The label and its arrow are two objects, not one formatted string. The walk
// finds a control by the WORDS on it -- exactly, or as the "icon  LABEL" form
// wt_pill_icon builds -- so a trailing arrow baked into the text makes the
// control unfindable, and every hold on this screen would silently do nothing.
// wt_arrow_action splits them for the same reason.
static void hold_rule_say(wt_hold_t *h, const char *txt)
{
    lv_label_set_text(h->lbl, txt);
    if (!h->arrow) return;
    lv_obj_update_layout(h->lbl);
    lv_obj_set_pos(h->arrow, lv_obj_get_width(h->lbl) + 12, 2);
}

// `animate` is false on the two paths where the object is going away or the
// screen is being replaced under it -- DELETE, and the tick that fires done().
// Starting an animation on either is the use-after-free this whole idiom has
// to avoid, and neither would ever be seen.
static void hold_reset(wt_hold_t *h, bool animate)
{
    if (h->tmr) { lv_timer_delete(h->tmr); h->tmr = NULL; }
    if (h->lbl && h->txt) hold_rule_say(h, h->txt);
    if (!h->fill) return;
    lv_anim_delete(h->fill, an_w);
    int32_t at = lv_obj_get_width(h->fill);
    if (!animate || h->release_ms <= 0 || at <= 0) {
        lv_obj_set_width(h->fill, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, h->fill);
    lv_anim_set_exec_cb(&a, an_w);
    lv_anim_set_values(&a, at, 0);
    lv_anim_set_duration(&a, h->release_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void hold_tick_cb(lv_timer_t *t)
{
    wt_hold_t *h = lv_timer_get_user_data(t);
    uint32_t el = lv_tick_elaps(h->t0);
    if (el >= (uint32_t)h->ms) {
        void (*done)(void *) = h->done;
        void *ud = h->ud;
        hold_reset(h, false);
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
        if (h->lbl && h->held) hold_rule_say(h, h->held);
        if (!h->tmr) h->tmr = lv_timer_create(hold_tick_cb, 30, h);
    } else {                           // RELEASED, PRESS_LOST, or DELETE
        hold_reset(h, c != LV_EVENT_DELETE);
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

lv_obj_t *wt_hold_rule(lv_obj_t *scr, const char *txt, const char *held,
                       int x, int y, int w, int ms,
                       void (*done)(void *), void *ud)
{
    if (y >= WT_CONTENT_BOTTOM) action_bar_ensure(scr);

    wt_hold_t *h = lv_malloc(sizeof *h);
    if (!h) return NULL;
    lv_memzero(h, sizeof *h);
    h->ms = ms > 0 ? ms : 1200;
    h->w = w;
    h->txt = txt;
    h->held = held;
    h->release_ms = 180;
    h->done = done;
    h->ud = ud;

    // The hit box is the rule's whole width and the action row's height. No
    // fill, no border, no radius: the control IS the label and the bar, and
    // anything drawn around them would be the pill this replaces.
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, WT_ACTION_H);
    lv_obj_set_pos(p, x, y);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    h->pill = p;

    lv_obj_t *l = wt_lbl(p, "", 0, 0, wt_font23(), wt_accent());
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_add_flag(l, WT_FLAG_ACCENT);
    h->lbl = l;
    lv_obj_t *ar = wt_lbl(p, LV_SYMBOL_RIGHT, 0, 2, wt_font23(), wt_accent());
    lv_obj_add_flag(ar, WT_FLAG_ACCENT);
    h->arrow = ar;
    hold_rule_say(h, txt);
    lv_obj_update_layout(l);

    const int ty = lv_obj_get_height(l) + 8;
    lv_obj_t *track = lv_obj_create(p);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, w, 2);
    lv_obj_set_pos(track, 0, ty);
    lv_obj_set_style_bg_color(track, WT_DIV, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *f = lv_obj_create(p);
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, 0, 2);
    lv_obj_set_pos(f, 0, ty);
    lv_obj_set_style_bg_color(f, wt_accent(), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_add_flag(f, WT_FLAG_ACCENT_FILL);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
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
    // The rim and the fill are the accent, so they are flagged as the accent.
    // wt_pill's ORDINARY border stays WT_MUT and is not flagged: the accent
    // marks the suggested action, and if every pill wore it it would mark
    // nothing -- ADDENDUM-02 rule 3, and docs/device-ux-test.md task 6 is the
    // acceptance test for exactly that.
    lv_obj_add_flag(pill, WT_FLAG_ACCENT_BORDER);
    lv_obj_add_flag(pill, WT_FLAG_ACCENT_BG);
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
    if (sz.y <= max_h) return wt_font23();
    WT_FIT_GAVE_UP("note", txt, w, max_h);
    return wt_font14();
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

lv_obj_t *wt_wrap(lv_obj_t *scr, const char *txt, int x, int y, int w, int max_h)
{
    // The TEXT comes in, so the size can be chosen. It used to hard-code
    // font14 and return an empty label for the caller to fill, which is the
    // font14-is-a-bug shape with the ladder missing rather than overruled:
    // its three callers are the sentences that explain why an address did not
    // match, on the screen where an owner decides whether to trust one.
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, wt_body_font(txt ? txt : "", w, max_h), 0);
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

static void qr_zoom_open_cb(lv_event_t *e);

// The overlay itself, reachable without an event: RECEIVE's "TAP TO ENLARGE"
// line is a second way in, and lv_event_t is opaque outside LVGL's private
// header, so a synthesised event is not an option.
static void qr_zoom_open(wt_qr_state_t *s)
{
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

// A QR whose payload could not be derived must VANISH, not encode the failure.
// Five screens fed tr(STR_C_SESSION_LOCKED) straight into wt_qr_update when a
// locked session refused them, so the device showed a scannable code whose
// content was the words "SESSION LOCKED" -- on RECEIVE that is the square a
// sender is invited to scan as a payment address. Hiding the CARD is the
// contract: a blank white card reads as a broken render, and the qr alone is
// not the tap target, its card is.
void wt_qr_refusal(lv_obj_t *qr, bool locked)
{
    if (!qr) return;
    lv_obj_t *card = lv_obj_get_parent(qr);
    if (!card) return;
    if (locked) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
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

static void qr_zoom_open_cb(lv_event_t *e)
{
    qr_zoom_open(lv_event_get_user_data(e));
}

void wt_qr_zoom(lv_obj_t *qr)
{
    // The zoom, from a control that is not the card. RECEIVE's "TAP TO
    // ENLARGE" line is a second way into the same overlay, and the state the
    // opener needs is already hanging off the QR -- so this is one lookup, not
    // a second implementation of the overlay.
    if (!qr) return;
    wt_qr_state_t *s = lv_obj_get_user_data(qr);
    if (!s) return;
    qr_zoom_open(s);
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
    // bc1q/tb1q for SegWit and sp1q/tsp1q for silent payments. It is shown as
    // its own block so the address still reads as one, but it is not marked and
    // neither is the block after it.
    //
    // This line used to light the four after the prefix AND the last four,
    // while wt_addr_spans -- the full address, directly above it on the verify
    // and receive screens -- lights the last EIGHT. Two renderings of one
    // address, marking two different runs, under a caption that says "compare
    // these 8". The runs overlapped in only their final four, so an owner who
    // learned this line's rule was checking characters the line above left
    // grey, and vice versa.
    //
    // The last eight wins, for the reason written at ADDR_TAIL_CHARS: the tail
    // carries real entropy and the bech32 checksum, so any altered address
    // differs there, while a mark near the front is the part an attacker gets
    // to match cheaply. It is also the only rule that can be taught on a
    // MULTI-recipient panel, which draws no elided line at all -- so choosing
    // the other one would leave the rule unavailable exactly where there are
    // most addresses to get wrong.
    //
    // The rendered string is unchanged, character for character. Only which
    // span carries the accent moved.
    int pre = !strncmp(addr, "tsp1", 4) ? 5
            : (!strncmp(addr, "bc1", 3) || !strncmp(addr, "tb1", 3) ||
               !strncmp(addr, "sp1", 3)) ? 4 : 0;
    char head[8] = {0}, key[8] = {0}, mid[32] = {0}, tail[16] = {0};
    lv_memcpy(head, addr, (size_t)pre);
    lv_memcpy(key, addr + pre, 4);
    // Twelve from the end, in three blocks of four. Chunking from the RIGHT is
    // the point: 42 characters do not divide by four, so grouping from the left
    // would leave the final block short and the lit run would straddle a gap.
    // The first of the three stays grey; the last two ARE the eight.
    const char *t = addr + n - 12;
    snprintf(mid, sizeof mid, "  \xE2\x80\xA6  %.4s ", t);
    snprintf(tail, sizeof tail, "%.4s %.4s", t + 4, t + 8);

    lv_obj_t *sg = lv_spangroup_create(par);
    addr_spans_no_click(sg);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);   // one line, sized to fit
    lv_obj_set_style_text_font(sg, f, 0);
    if (pre) {
        char pfx[8];
        snprintf(pfx, sizeof pfx, "%s ", head);
        addr_span(sg, pfx, false);
    }
    addr_span(sg, key, false);
    addr_span(sg, mid, false);
    addr_span(sg, tail, true);
    lv_spangroup_refresh(sg);
    return sg;
}

static lv_obj_t *addr_spans(lv_obj_t *par, const char *grouped, int w,
                            const lv_font_t *f, bool lift)
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
    //
    // ONLY when there ARE groups. Every caller passed a wt_group4 string until
    // the verify screen passed a raw address -- deliberately, because grouped it
    // measures 437px against a 438px box and wraps onto the line the comparison
    // belongs on. With no space to stop at, this walk ran t down to 0: the muted
    // head became empty and the accent span became the WHOLE address, so the one
    // screen that asks you to compare the last eight characters drew all
    // forty-two in one flat colour with nothing marked at all. Inverted, not
    // missing -- and with two or more recipients there is no second lit line
    // under it to fall back on.
    if (strchr(grouped, ' '))
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
    // The tail is the part you are actually asked to compare, so where the body
    // is too small to compare comfortably the tail renders one rung ABOVE it.
    // Blowing up the whole string instead would push the other outputs off a
    // scrolling list -- this buys the legibility where it counts for 7px.
    //
    // Only at the 14 rung: at 23 and 28 the body already reads at arm's length
    // and the accent alone marks the tail, which is what the receive screen has
    // always done at 28.
    //
    // This used to test `f == wt_font14()` -- the PROPORTIONAL face -- while all
    // four callers pass a mono one, so the branch could not be taken by anything
    // and the tail never grew. Measured both ways at mono14: +7px, and the tail
    // stays on the same line in every box it is drawn in (438 and 722, a 42
    // character bech32 and a 117 character silent payment). It costs height, not
    // a wrap.
    //
    // And it is the CALLER's call, not a rule the font can carry. Whether the
    // tail must carry legibility on its own depends on what else is on the
    // screen: the single-recipient verify panel draws the compared runs again
    // beneath, blocked and at mono23, so lifting the body tail there renders the
    // same eight characters at the same size twice and pushed that panel into a
    // scrollbar with the caption at the fold. A multi-recipient list draws no
    // such line, so the body tail is the only marking there is, and at 14 it was
    // being carried by colour alone.
    if (lift) {
        if (f == wt_font_mono14())      lv_style_set_text_font(lv_span_get_style(s2), wt_font_mono23());
        else if (f == wt_font14())      lv_style_set_text_font(lv_span_get_style(s2), wt_font23());
    }
    // The last span is the lit one, and the flag is what makes accent_walk
    // repaint it when the theme moves. Every address on this device comes
    // through here, so this is the one place it needs saying.
    lv_obj_add_flag(sg, WT_FLAG_ACCENT);
    lv_spangroup_refresh(sg);
    return sg;
}

// The tail is marked by colour, at the body's own size. Use this wherever a
// larger copy of the compared run is drawn elsewhere on the screen.
lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f)
{
    return addr_spans(par, grouped, w, f, false);
}

// The tail is ALSO lifted a rung, for the screens where this line is the only
// place the compared run appears. A no-op above the 14 rung.
lv_obj_t *wt_addr_spans_lift(lv_obj_t *par, const char *grouped, int w,
                             const lv_font_t *f)
{
    return addr_spans(par, grouped, w, f, true);
}

// Section eyebrows carry the ACCENT. They were WT_MUT, which made the theme
// almost invisible outside the page title and one selected dot -- the owner
// could change it and struggle to tell.
//
// An eyebrow is the safest place on the device to spend the accent, and that
// matters because GREEN is byte identical to WT_OK and ORANGE is a near match
// for WT_WARN. Anywhere those can be confused, the accent has to stand aside:
// a green bordered ERASE reads as safe. An eyebrow names a GROUP, never a
// state, so it can never be mistaken for one -- and the one eyebrow that IS a
// state, NO UNDO, overrides this colour to STOP_COL right after the call and
// keeps doing so.
// Repaint everything wearing the accent under scr. The theme can be changed
// while a screen is up, and eyebrows and chevrons are built by shared helpers
// scattered across six files -- keeping a static list of them in every screen
// that has some is how they get missed. A flag on the object and one walk finds
// them wherever they were made.
//
// Colour only. The chevron's opacity is set once when it is built and has to
// survive this, or every theme change makes the arrows a little louder.
static void accent_walk(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT)) {
        // Text first and unconditionally, which is what this flag has always
        // done and what every label under it still needs. Then the two classes
        // that carry their ink somewhere else: setting a text colour on a line
        // is not wrong, it is simply invisible, and that is exactly how the
        // change strand wore a stale accent with the flag correctly set.
        lv_obj_set_style_text_color(o, wt_accent(), 0);
        if (lv_obj_check_type(o, &lv_line_class))
            lv_obj_set_style_line_color(o, wt_accent(), 0);
        else if (lv_obj_check_type(o, &lv_arc_class))
            lv_obj_set_style_arc_color(o, wt_accent(), LV_PART_INDICATOR);
        else if (lv_obj_check_type(o, &lv_spangroup_class)) {
            // A span carries its own style and a text colour on the group is
            // invisible, exactly like the line above. Every lit address on
            // this device is built the same way -- a grey head and the last
            // eight as the FINAL span -- so the last span is the accented one
            // by construction, and repainting it is the whole job. Without
            // this the address tails were the one accent-painted thing on the
            // device that a theme change left behind, on the screens that show
            // the most of them.
            uint32_t sn = lv_spangroup_get_span_count(o);
            if (sn) {
                lv_span_t *last = lv_spangroup_get_child(o, (int32_t)sn - 1);
                if (last) {
                    lv_style_set_text_color(lv_span_get_style(last),
                                            wt_accent());
                    lv_spangroup_refresh(o);
                }
            }
        }
    }
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_BORDER))
        lv_obj_set_style_border_color(o, wt_accent(), 0);
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_BG)) {
        lv_obj_set_style_bg_color(o, wt_accent_bg(), 0);
        lv_obj_set_style_bg_color(o, wt_accent_pressed(), LV_STATE_PRESSED);
    }
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_FILL))
        lv_obj_set_style_bg_color(o, wt_accent(), 0);
    if (lv_obj_has_flag(o, WT_FLAG_ACCENT_SCROLL))
        lv_obj_set_style_bg_color(o, wt_accent(), LV_PART_SCROLLBAR);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) accent_walk(lv_obj_get_child(o, i));
}

void wt_accent_restyle(lv_obj_t *scr)
{
    if (scr) accent_walk(scr);
}

lv_obj_t *wt_row_head(lv_obj_t *scr, const char *txt, int x, int y, int w)
{
    lv_obj_t *h = wt_lbl(scr, txt, x, y, wt_font14(), wt_accent());
    lv_obj_add_flag(h, WT_FLAG_ACCENT);
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
        // Flagged as well as painted, all three accents: a selected row built
        // before a theme change would otherwise keep the old accent on its
        // border, tick and icon while everything around it moved -- the
        // chevron branch below has carried its flag from the start, and paint
        // without the flag is exactly the stale-strand defect accent_walk's
        // own comment describes.
        lv_obj_set_style_border_color(row, wt_accent(), 0);
        lv_obj_add_flag(row, WT_FLAG_ACCENT_BORDER);
        lv_obj_t *ok = wt_lbl(row, LV_SYMBOL_OK, 0, 0, wt_font23(),
                              wt_accent());
        lv_obj_add_flag(ok, WT_FLAG_ACCENT);
        lv_obj_update_layout(ok);
        lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -10, 0);
        right = w - 10 - lv_obj_get_width(ok) - 10;
    } else if (cb) {
        // The accent, at 150 opacity. It was WT_DIM on the argument that a
        // chevron is an affordance rather than content and should be the
        // quietest ink on the card. The first half of that is exactly why it
        // should be TINTED: the accent is the device's "this is yours to touch"
        // colour, and the chevron is the mark that says a row is touchable.
        //
        // The opacity is what keeps the second half true. At full strength a
        // page of six rows becomes six bright arrows and the accent stops
        // meaning anything; at 150 it reads as a tint at arm's length and as a
        // colour up close, which is the job.
        //
        // Safe from the status collision for the same reason the eyebrows are:
        // a chevron says a row OPENS, never how it is doing.
        lv_obj_t *ch = wt_lbl(row, LV_SYMBOL_RIGHT, 0, 0, wt_font14(), wt_accent());
        lv_obj_set_style_text_opa(ch, 150, 0);
        lv_obj_add_flag(ch, WT_FLAG_ACCENT);
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
        // The badge takes the ACCENT, always. It used to be accent only when
        // the row was SELECTED, which meant every list without a current item
        // -- the sign chooser, the file list, the SD screen, the setup pickers
        // -- painted its marks the colour this kit uses for a row that cannot
        // be tapped. Three call sites had already reached for
        // wt_row_icon_accent by hand to undo it.
        //
        // Unselected sits at the CHEVRON's opacity, not full: a page of six
        // rows at full strength is six bright marks and the accent stops
        // meaning anything, which is the argument written above the chevron
        // and it applies here for the same reason. Selected stays at full, so
        // "this is the one you are on" is still louder than "this is a row".
        lv_obj_t *ic = wt_lbl(row, icon, 0, 0, wt_font23(), wt_accent());
        if (!sel) lv_obj_set_style_text_opa(ic, 150, 0);
        lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        lv_obj_set_user_data(ic, (void *)WT_ROW_ICON_TAG);
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

// Recolour just a row's icon badge from WT_MUT to the accent. For the one case
// where the mark is an identity and not decoration: the secret glyph on the
// KEYS card is drawn in the accent, and the same glyph on a row in another
// screen has to match, or one mark reads as two different things.
//
// By tag, not by child index, for the same reason wt_row_sub_color is: which
// children a row has depends on whether it was given a chevron, a tick, a
// value or a sub. The flag hands the label to accent_walk, so a theme change
// repaints it along with every other accent-inked label.
void wt_row_icon_accent(lv_obj_t *row)
{
    lv_obj_t *ic = wt_tagged(row, WT_ROW_ICON_TAG);
    if (!ic) return;
    lv_obj_set_style_text_color(ic, wt_accent(), 0);
    // FULL strength, which is what this call means now that every badge is
    // already accent at the page tint: it lifts one row's mark above the rest.
    lv_obj_set_style_text_opa(ic, LV_OPA_COVER, 0);
    lv_obj_add_flag(ic, WT_FLAG_ACCENT);
}

// The opt-out, and it is not decoration: a row wearing WT_SEV_OK or WT_SEV_WARN
// has already spent its colour, and on the GREEN theme the accent is WT_OK to
// the byte -- an accent badge on a green card is a second verification tick
// nobody wrote. Three rows need this and they are named where they are called.
void wt_row_icon_mute(lv_obj_t *row)
{
    lv_obj_t *ic = wt_tagged(row, WT_ROW_ICON_TAG);
    if (!ic) return;
    lv_obj_remove_flag(ic, WT_FLAG_ACCENT);
    lv_obj_set_style_text_color(ic, WT_MUT, 0);
    lv_obj_set_style_text_opa(ic, LV_OPA_COVER, 0);
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
    // The RAIL, and it sets its own width. This painted a border colour and an
    // opacity and left the width to whoever built the row -- which was fine
    // while every row was a card with a 1px edge, and became nothing at all
    // the moment rows went borderless. A tappable row still showed it, because
    // wt_line_press happens to put a 3px left border there for the pressed
    // state; an INERT row showed a 13-opacity wash and no edge whatever, which
    // is the state the NO UNDO comment already measured as invisible.
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, 3, 0);
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

// ---- SETTINGS: the section tabs (see kiss_theme.h) ----
//
// A designated initialiser that leaves a colour out gives you {0,0,0}, and
// pure black is the one value nothing on this device is painted in -- no ink,
// no border, no fill. So it reads as "unset" and the field takes its default,
// which keeps the wt_wide_t and wt_pop_item_t literals in kiss_settings.c
// short instead of charging every one of them a WT_MUT it did not want to
// think about. Cheaper than a parallel bool per colour, and impossible to get
// half right the way a bool can be.
static lv_color_t col_or(lv_color_t c, lv_color_t dflt)
{
    return (c.red || c.green || c.blue) ? c : dflt;
}

#define WT_TAB_GAP    9    // icon to label, and label to dot
#define WT_TAB_DOT    7
#define WT_TAB_SPACE  2    // the tracking a font14 label wears at this size

// The highlight's two skins as ONE number: 0 is the ordinary group's panel
// fill and edge, 255 is the destructive group's stop tint. A move into or out
// of NO UNDO changes the fill, the border and both opacities at once, so the
// slide carries a single mix rather than swapping four styles at whichever
// moment happens to look least wrong.
#define WT_TAB_STOP_BG_OPA 13
#define WT_TAB_STOP_BD_OPA 77

static void tab_hl_skin(lv_obj_t *hl, int32_t t)
{
    lv_obj_set_style_bg_color(hl, lv_color_mix(WT_STOP, WT_PANEL, (uint8_t)t), 0);
    lv_obj_set_style_bg_opa(hl, (lv_opa_t)(LV_OPA_COVER
        + (WT_TAB_STOP_BG_OPA - LV_OPA_COVER) * t / 255), 0);
    lv_obj_set_style_border_color(hl, lv_color_mix(WT_STOP, WT_EDGE, (uint8_t)t), 0);
    lv_obj_set_style_border_opa(hl, (lv_opa_t)(LV_OPA_COVER
        + (WT_TAB_STOP_BD_OPA - LV_OPA_COVER) * t / 255), 0);
}

static void tab_hl_x(void *v, int32_t x)   { lv_obj_set_x((lv_obj_t *)v, x); }
static void tab_hl_mix(void *v, int32_t t) { tab_hl_skin((lv_obj_t *)v, t); }

lv_obj_t *wt_tabs(lv_obj_t *scr, const wt_tab_t *tabs, int n, int sel,
                  int x, int y, lv_event_cb_t cb)
{
    const lv_font_t *f = wt_font14();

    // FIRST, so every button draws over it. It is the only part of the strip
    // that moves, and the buttons above it are identical to each other.
    lv_obj_t *hl = lv_obj_create(scr);
    lv_obj_remove_style_all(hl);
    lv_obj_set_pos(hl, x + sel * WT_TAB_PITCH, y);
    lv_obj_set_size(hl, WT_TAB_W, WT_TAB_H);
    lv_obj_set_style_radius(hl, 10, 0);
    lv_obj_set_style_border_width(hl, 1, 0);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);   // the button over it takes the tap
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
    // The strip's own origin, which wt_tabs_select needs to place tab `to` and
    // cannot recover from a highlight caught mid slide.
    lv_obj_set_user_data(hl, (void *)(intptr_t)x);
    tab_hl_skin(hl, (sel >= 0 && sel < n && tabs[sel].stop) ? 255 : 0);

    for (int i = 0; i < n; i++) {
        const wt_tab_t *t = &tabs[i];

        lv_obj_t *b = lv_obj_create(scr);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, x + i * WT_TAB_PITCH, y);
        lv_obj_set_size(b, WT_TAB_W, WT_TAB_H);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // The same press answer every row on the device gives, so a tab reads
        // as the same family of object as the rows it switches between.
        lv_obj_set_style_bg_color(b, wt_accent_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        wt_tap_feedback(b);
        if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // The highlight is a FILL and an edge, never the accent, and it is the
        // object created above rather than anything set here. Which also puts
        // the labels back in line: LV_ALIGN_LEFT_MID aligns to the CONTENT
        // area, so the one tab wearing a 1px border used to hold its label a
        // pixel right of the other four, and the word jumped when you selected
        // it. Every settings frame in the walk moved by exactly that pixel.
        //
        // A tab strip is
        // navigation: it says where you are, which is not a status and not an
        // action, so it stays in the surface colours and leaves the accent to
        // the chevrons that say a row opens.
        lv_color_t ink  = t->stop ? WT_STOP_INK : WT_INK;
        lv_color_t mark = t->stop ? WT_STOP : WT_MUT;

        // Measured and centred as a group, because the icon, the label and the
        // dot are three objects and only their TOTAL can be centred. Letter
        // spacing is measured with the label, or the centring is off by two
        // pixels per character in every locale.
        lv_point_t is = { 0, 0 }, ls;
        if (t->icon && *t->icon)
            lv_text_get_size(&is, t->icon, f, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
        lv_text_get_size(&ls, t->label, f, WT_TAB_SPACE, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        int iw = is.x ? is.x + WT_TAB_GAP : 0;
        int dw = t->dot ? WT_TAB_GAP + WT_TAB_DOT : 0;
        int lw = ls.x;
        // A locale whose word does not fit LOSES LETTERS. The tab does not
        // widen: 144 on a 152 pitch is what puts five groups in the 752 lane,
        // and one long translation may not move the other four.
        int room = WT_TAB_W - 8 - iw - dw;
        if (lw > room) lw = room;
        int px = (WT_TAB_W - (iw + lw + dw)) / 2;
        if (px < 4) px = 4;

        if (iw) {
            lv_obj_t *ic = wt_lbl(b, t->icon, 0, 0, f, mark);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, px, 0);
        }
        lv_obj_t *l = wt_lbl(b, t->label, 0, 0, f, ink);
        lv_obj_set_style_text_letter_space(l, WT_TAB_SPACE, 0);
        lv_obj_set_width(l, lw);
        lv_obj_set_height(l, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, px + iw, 0);

        if (t->dot) {
            lv_obj_t *d = lv_obj_create(b);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, WT_TAB_DOT, WT_TAB_DOT);
            lv_obj_set_style_radius(d, WT_TAB_DOT, 0);
            lv_obj_set_style_bg_color(d, WT_WARN, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);   // the tab takes the tap
            lv_obj_align(d, LV_ALIGN_LEFT_MID, px + iw + lw + WT_TAB_GAP, 0);
        }
    }
    return hl;
}

void wt_tabs_select(lv_obj_t *hl, int from, int to, bool stop)
{
    (void)from;
    if (!hl || !lv_obj_is_valid(hl)) return;
    const int x0 = (int)(intptr_t)lv_obj_get_user_data(hl);

    // Both start from where the highlight IS, not from where the last tap
    // meant it to end up. Tapping across the strip faster than 200ms is one
    // continuous slide rather than five jumps to the left edge.
    lv_anim_delete(hl, tab_hl_x);
    lv_anim_delete(hl, tab_hl_mix);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, hl);
    lv_anim_set_duration(&a, WT_TAB_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, tab_hl_x);
    lv_anim_set_values(&a, lv_obj_get_x(hl), x0 + to * WT_TAB_PITCH);
    lv_anim_start(&a);

    // The mix is read back off the fill rather than remembered, so an
    // interrupted cross fade resumes from the colour on the glass. Skipped
    // when there is nowhere to travel: every pair of ordinary tabs wears the
    // same skin, and animating 255 -> 0 between two of them flashes red.
    const int32_t cur = lv_obj_get_style_bg_opa(hl, LV_PART_MAIN);
    const int32_t t0 = (LV_OPA_COVER - cur) * 255
                     / (LV_OPA_COVER - WT_TAB_STOP_BG_OPA);
    const int32_t t1 = stop ? 255 : 0;
    if (t0 != t1) {
        lv_anim_set_exec_cb(&a, tab_hl_mix);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_values(&a, t0, t1);
        lv_anim_start(&a);
    }
}

// ---- KEYS / RECEIVE: the borderless idioms (see kiss_theme.h) ----
// The kit's three animation exec callbacks live with the pane code below;
// the bracket strip is the one caller above them.
static void an_tx(void *v, int32_t x);
static void an_opa(void *v, int32_t o);

void wt_line_press(lv_obj_t *row)
{
    // The rail. A left border already present at opacity 0 costs one style
    // property and no second object; the pressed state flips its opa and
    // lights a 3px accent edge down the row under the finger. That plus the
    // wash is the only feedback a borderless row can give, so it is not
    // decoration -- without it the row is an unmarked target.
    lv_obj_set_style_radius(row, 6, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, wt_accent(), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, wt_accent_pressed(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, WT_FLAG_ACCENT_BORDER);
    lv_obj_add_flag(row, WT_FLAG_ACCENT_BG);
    // A style transition, not an lv_anim: LVGL runs it on the state change
    // itself, so a press released mid-fade reverses rather than finishing and
    // snapping back.
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_OPA, LV_STYLE_BORDER_OPA, LV_STYLE_PROP_INV
    };
    static lv_style_transition_dsc_t tr;
    static bool tr_ready;
    if (!tr_ready) {
        lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out,
                                     160, 0, NULL);
        tr_ready = true;
    }
    lv_obj_set_style_transition(row, &tr, 0);
}

int wt_line_val_y(void)
{
    return WT_LINE_CAP_Y + lv_font_get_line_height(wt_font23()) - 2;
}

lv_obj_t *wt_line_rule(lv_obj_t *par, int x, int y, int w)
{
    lv_obj_t *r = lv_obj_create(par);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_style_bg_color(r, WT_DIV, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    // The pivot the entry animation needs, set HERE rather than at the call
    // site. transform_scale_x with the default centre pivot draws the rule
    // from its middle outward, which inverts the whole effect -- and it does
    // it silently, because a rule at half scale still measures as a rule.
    lv_obj_set_style_transform_pivot_x(r, 0, 0);
    return r;
}

lv_obj_t *wt_help_mark(lv_obj_t *par, int x, int y)
{
    lv_obj_t *m = lv_obj_create(par);
    lv_obj_remove_style_all(m);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 19, 19);
    lv_obj_set_style_radius(m, 10, 0);
    lv_obj_set_style_border_width(m, 1, 0);
    lv_obj_set_style_border_color(m, wt_accent(), 0);
    lv_obj_set_style_border_opa(m, 115, 0);
    lv_obj_add_flag(m, WT_FLAG_ACCENT_BORDER);
    // Takes no taps: the row under it is the target and a 19px circle inside a
    // 468px row that already opens the same explainer would only ever steal
    // presses from it.
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *q = wt_lbl(m, "?", 0, 0, wt_font14(), wt_accent());
    lv_obj_add_flag(q, WT_FLAG_ACCENT);
    lv_obj_center(q);
    return m;
}

lv_obj_t *wt_title_cursor(lv_obj_t *scr)
{
    lv_obj_t *t = wt_screen_title(scr);
    if (!t) return NULL;
    lv_obj_update_layout(t);
    lv_obj_t *cur = lv_obj_create(scr);
    lv_obj_remove_style_all(cur);
    lv_obj_set_size(cur, 10, 22);
    // Centred on the title's cap height rather than its box: a font34 line box
    // carries descender room no capital reaches into, so centring on the box
    // sits the block visibly low against KEYS and RECEIVE, which have no
    // descenders at all.
    lv_obj_set_pos(cur, lv_obj_get_x(t) + lv_obj_get_width(t) + 12,
                   lv_obj_get_y(t) + (lv_obj_get_height(t) - 22) / 2 - 2);
    lv_obj_set_style_bg_color(cur, wt_accent(), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_obj_add_flag(cur, WT_FLAG_ACCENT_FILL);
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_SCROLLABLE);

    // step, not a fade: a cursor BLINKS. An eased opacity ramp reads as a
    // pulse, which is the device's "something is happening" language and this
    // is not that -- it is the page saying it is waiting for you.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cur);
    lv_anim_set_exec_cb(&a, an_opa);
    // 850 out and 850 back is the handoff's 1700ms cycle: lv_anim_path_step
    // holds the start value for the whole duration and only then jumps, so one
    // 1700ms leg would be 1700ms lit and one frame dim.
    lv_anim_set_values(&a, LV_OPA_COVER, 31);
    lv_anim_set_duration(&a, 850);
    lv_anim_set_playback_duration(&a, 850);
    lv_anim_set_path_cb(&a, lv_anim_path_step);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    return cur;
}

lv_obj_t *wt_line_row(lv_obj_t *par, int x, int y, int w, int h,
                      const char *cap, const char *val, const lv_font_t *vf,
                      lv_color_t vcol, const char *sub, const lv_font_t *sf,
                      lv_event_cb_t cb, void *ud)
{
    if (!sf) sf = wt_font23();
    lv_obj_t *row = lv_obj_create(par);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, w, h);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
        wt_line_press(row);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    // The arrow FIRST, so the sub can be measured against the lane it leaves.
    // Absent, not dimmed, on a row that opens nothing: a mark at low opacity
    // still says "there is something here", which is the opposite of true.
    int right = w - WT_LINE_PAD;
    if (cb) {
        lv_obj_t *ar = wt_lbl(row, LV_SYMBOL_RIGHT, 0, 0, wt_font23(),
                              wt_accent());
        lv_obj_add_flag(ar, WT_FLAG_ACCENT);
        lv_obj_update_layout(ar);
        lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -4 - (24 - lv_obj_get_width(ar)) / 2,
                     0);
        right = w - 4 - 24;
    }

    // The page font, not the mono one. The mono face is for DATA -- a version,
    // an address, a fingerprint, a path -- where fixed pitch is what lets an
    // owner compare two of them character by character. A caption is a WORD,
    // and a word set in a second typeface next to a page of Montserrat reads
    // as a rendering fault rather than as a distinction. Reported from the
    // bench as exactly that: "idk why smaller fonts are different fonts".
    // Letter space 2, which is what wt_row_head has always used for the same
    // kind of label.
    lv_obj_t *c = wt_lbl(row, cap, WT_LINE_PAD, WT_LINE_CAP_Y,
                         wt_font23(), WT_MUT);
    lv_obj_set_style_text_letter_space(c, 1, 0);

    // font23, and NOT font14. A sub-line here is a SENTENCE -- "names these
    // keys", "compare the last 8", "so it finds these payments" -- and the
    // house rule about tiny type is about exactly these. It shipped at 14 on
    // the argument that a sub beside a font23 value is metadata; that argument
    // came back off the bench as "its fucking tiny", which is the fifth time
    // the same rule has been reported. There is no version of this where the
    // reader is wrong.
    //
    // The lane grows to 300 to pay for it and the VALUE gives up the width,
    // which is the right way round: a value is one short string and a sub is
    // the sentence explaining it.
    //
    // The SUB is measured before either it or the value is placed, and its box
    // is sized to the text rather than to the lane. A label pinned to a fixed
    // 200 and right-aligned inside it leaves an empty box reaching back across
    // the row, and overlapcheck compares BOXES -- so a short sub beside a long
    // value read as an overlap that nothing on the glass could show.
    // 320, not 300. STR_S_CMP_8 -- "compare the lit characters", the one
    // sentence three screens share -- wants 308 at font23, and CUT reported it
    // ellipsised. The lane is a constant chosen here, not a geometry the page
    // is stuck with like the 196px tab, so it gives the eight pixels and the
    // value gives them up. A 21 locale string does not get cut to save a
    // number that was picked round.
    const int lane = 320;
    int subw = 0;
    if (sub && *sub) {
        lv_point_t ss;
        lv_text_get_size(&ss, sub, sf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        subw = ss.x > lane ? lane : ss.x;
    }
    // 46 in from the row's right, ALWAYS: 4 of lane inset, the arrow's 24, and
    // 18 of gap. Measured off the arrow instead, the one row with no arrow
    // would hang its sub 14px right of every other and the column would not
    // read straight down.
    const int sub_x = w - 46 - subw;

    if (val) {
        // Stops 18 clear of whatever is to its right -- the sub if there is
        // one, the arrow's lane if there is not. The value is the row's
        // subject and is never ellipsised by choice, but a locale that
        // overruns has to lose letters rather than run under the sub.
        const int vw = (subw ? sub_x : right) - 18 - WT_LINE_PAD;
        lv_obj_t *v = wt_lbl(row, val, WT_LINE_PAD, wt_line_val_y(),
                             vf ? vf : wt_font23(), vcol);
        lv_obj_set_width(v, vw);
        lv_obj_set_height(v, lv_font_get_line_height(vf ? vf : wt_font23()));
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    }

    if (subw) {
        lv_obj_t *sl = wt_lbl(row, sub, 0, 0, sf, WT_DIM);
        wt_sub_measure(sub, sf, lane);
        lv_obj_set_width(sl, subw);
        lv_obj_set_height(sl, lv_font_get_line_height(sf));
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -46, 0);
    }
    return row;
}

// ---- the bracket tab strip ----
// The tracking a tab label wears. 1, not 2, since the label moved off the mono
// face: Montserrat's caps are wider than Ioskeley's at the same pixel size, and
// at 2 the product's own English "SILENT PAYMENT" lost its last three letters
// to the ellipsis. The tab cannot widen -- 196 on a 200 pitch is what puts
// three groups in the 704 lane -- so the tracking is what gives the letters
// back.
#define WT_BR_SPACE 1
#define WT_BR_GAP   7    // icon to label, and bracket to either

// Every tab is built with both brackets and they are never created or
// destroyed -- only their opacity moves. Building them on selection instead
// would reflow the strip on every tap, because a bracket appearing changes the
// width of the group the tab centres.
typedef struct { lv_obj_t *l, *r, *ic, *lbl; } br_tab_t;

static void br_paint(lv_obj_t *tab, bool sel)
{
    // Found, not indexed. This walked children by position until the unread
    // dot was added and shifted every one of them -- and the symptom of that
    // is a strip that paints the wrong object, silently, on the one tab that
    // has something to say.
    const bool stop = lv_obj_get_user_data(tab) != NULL;
    lv_obj_t *lb = NULL, *rb = NULL, *lbl = NULL, *ic = NULL;
    const uint32_t n = lv_obj_get_child_count(tab);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(tab, i);
        if (!lv_obj_check_type(c, &lv_label_class)) continue;   // the dot
        const char *t = lv_label_get_text(c);
        if (t && !strcmp(t, "["))      lb = c;
        else if (t && !strcmp(t, "]")) rb = c;
        else if (!ic && lb && !lbl)    ic = c;   // the mark, between [ and the word
        else                           lbl = c;
    }
    if (!lbl && ic) { lbl = ic; ic = NULL; }     // a tab with no mark
    if (lb) lv_obj_set_style_text_opa(lb, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (rb) lv_obj_set_style_text_opa(rb, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    // The destructive group keeps its own ink whether or not it is the one you
    // are on: NO UNDO is a warning before it is a location.
    if (lbl) lv_obj_set_style_text_color(lbl, stop ? WT_STOP_INK
                                                   : (sel ? WT_INK : WT_MUT), 0);
    if (ic) {
        lv_obj_set_style_text_color(ic, stop ? WT_STOP
                                             : (sel ? wt_accent() : WT_DIM), 0);
        // Only the SELECTED icon is accent-painted, so only it may carry the
        // flag: a restyle that repainted every icon would put the accent on
        // every tab at once and the marker would stop marking anything.
        if (sel && !stop) lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        else              lv_obj_remove_flag(ic, WT_FLAG_ACCENT);
    }
}

lv_obj_t *wt_brackets(lv_obj_t *scr, const wt_tab_t *tabs, int n, int sel,
                      int x, int y, int w, lv_event_cb_t cb)
{
    // One face across the strip. The MARK already comes off the Latin face --
    // the mono one carries no FontAwesome and drew a blank box for every tab
    // icon until that was fixed -- so a mono label meant every tab was set in
    // two typefaces at once.
    const lv_font_t *f = wt_font14();

    lv_obj_t *strip = lv_obj_create(scr);
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, x, y);
    lv_obj_set_size(strip, w, WT_BR_STRIP_H);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    // The drawing's 196-on-200 wherever it FITS, and only then the lane
    // divided by the count. Dividing unconditionally looked harmless and
    // silently re-laid KEYS: two tabs in 704 became 352 apart, so every tap
    // the walk aimed at the second one landed on the first.
    const int pitch = (n > 0 && n * WT_BR_PITCH <= w) ? WT_BR_PITCH
                    : (n > 0 ? w / n : w);
    const int tw = pitch - (pitch > WT_BR_W ? pitch - WT_BR_W : 0);

    for (int i = 0; i < n; i++) {
        const wt_tab_t *t = &tabs[i];
        lv_obj_t *b = lv_obj_create(strip);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, i * pitch, 0);
        lv_obj_set_size(b, tw, WT_BR_H);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // No pressed fill. A wash here would be a box appearing on the one
        // strip built to have none; the sink from wt_tap_feedback is the whole
        // press answer.
        wt_tap_feedback(b);
        if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Measured as a group, brackets included, or the label sits off centre
        // by the width of a bracket on every unselected tab.
        // The MARK comes off the Latin face, not the mono one. IoskeleyMono is
        // built from 0x20-0x7E plus three punctuation marks and carries no
        // FontAwesome at all, so a WT_ICON_* asked of it draws LVGL's
        // missing-glyph box -- at the right size, in the right place, on every
        // tab, which is exactly the failure the wt_tabs comment warns about
        // and exactly as invisible to every gate.
        const lv_font_t *icf = wt_font14();
        lv_point_t is = { 0, 0 }, ls, bs;
        if (t->icon && *t->icon)
            lv_text_get_size(&is, t->icon, icf, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
        lv_text_get_size(&ls, t->label, f, WT_BR_SPACE, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        lv_text_get_size(&bs, "[", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int iw = is.x ? is.x + WT_BR_GAP : 0;
        int bw = bs.x + WT_BR_GAP;
        int dw = t->dot ? WT_BR_GAP + 7 : 0;
        // A locale whose word does not fit LOSES LETTERS. The tab does not
        // widen: 196 on a 200 pitch is what puts three groups in the 704 lane.
        int room = tw - 8 - iw - 2 * bw - dw;
        int lw = ls.x > room ? room : ls.x;
        int px = (tw - (2 * bw + iw + lw + dw)) / 2;
        if (px < 2) px = 2;

        lv_obj_t *lb = wt_lbl(b, "[", 0, 0, f, wt_accent());
        lv_obj_add_flag(lb, WT_FLAG_ACCENT);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, px, 0);
        if (iw) {
            lv_obj_t *ic = wt_lbl(b, t->icon, 0, 0, icf, WT_DIM);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, px + bw, 0);
        }
        lv_obj_t *l = wt_lbl(b, t->label, 0, 0, f, WT_MUT);
        lv_obj_set_style_text_letter_space(l, WT_BR_SPACE, 0);
        lv_obj_set_width(l, lw);
        lv_obj_set_height(l, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, px + bw + iw, 0);
        lv_obj_t *rb = wt_lbl(b, "]", 0, 0, f, wt_accent());
        lv_obj_add_flag(rb, WT_FLAG_ACCENT);
        lv_obj_align(rb, LV_ALIGN_LEFT_MID, px + bw + iw + lw + dw + WT_BR_GAP, 0);

        // The unread mark, and the destructive group's ink. wt_tabs carries
        // both and this strip dropped them on the floor -- SETTINGS' SECURITY
        // and BACKUP tabs say something wants reading, and NO UNDO says what
        // it is, and neither survived the move to brackets.
        if (t->dot) {
            lv_obj_t *d = lv_obj_create(b);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, 7, 7);
            lv_obj_set_style_radius(d, 4, 0);
            lv_obj_set_style_bg_color(d, WT_WARN, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(d, LV_ALIGN_LEFT_MID, px + bw + iw + lw + WT_BR_GAP, 0);
        }
        lv_obj_set_user_data(b, (void *)(intptr_t)(t->stop ? 1 : 0));
        br_paint(b, i == sel);
    }

    // The rule under the strip: full lane, static, and NOT a line row's rule.
    // It does not move with the selection and it does not draw on a tab change
    // -- it is the floor the strip stands on, and a floor that redrew itself
    // every tap would be the loudest thing on the page.
    wt_line_rule(strip, 0, WT_BR_STRIP_H - 1, w);
    return strip;
}

void wt_brackets_select(lv_obj_t *strip, int from, int to, bool stop)
{
    (void)stop;
    if (!strip) return;
    const uint32_t n = lv_obj_get_child_count(strip);
    // The last child is the rule, so the tabs are 0..n-2.
    if (from >= 0 && (uint32_t)from < n - 1)
        br_paint(lv_obj_get_child(strip, from), false);
    if (to < 0 || (uint32_t)to >= n - 1) return;
    lv_obj_t *tab = lv_obj_get_child(strip, to);
    br_paint(tab, true);

    // Both brackets arrive from OUTSIDE the label, which is what makes the
    // marker read as a pair closing on the word rather than as two glyphs
    // fading up. One lv_anim per property, per the kit's rule.
    lv_obj_t *lb = lv_obj_get_child(tab, 0);
    lv_obj_t *rb = lv_obj_get_child(tab, lv_obj_get_child_count(tab) - 1);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *o = i ? rb : lb;
        lv_anim_del(o, NULL);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_duration(&a, 220);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_values(&a, i ? -7 : 7, 0);
        lv_anim_set_exec_cb(&a, an_tx);
        lv_anim_start(&a);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_start(&a);
    }
}

// ---- the arrow action ----
void wt_arrow_action_set_text(lv_obj_t *ctrl, const char *txt)
{
    if (!ctrl || !txt) return;
    lv_obj_t *a = NULL, *l = NULL;
    const uint32_t n = lv_obj_get_child_count(ctrl);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(ctrl, i);
        if (!lv_obj_check_type(c, &lv_label_class)) continue;
        const char *t = lv_label_get_text(c);
        // The arrow is one glyph from the symbol range; the word is not.
        if (!a && t && (unsigned char)t[0] == 0xEF) a = c;
        else l = c;
    }
    if (!l || !a) return;
    const lv_font_t *f = wt_font23();
    lv_point_t ls, as;
    lv_text_get_size(&ls, txt, f, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&as, lv_label_get_text(a), f, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const bool back = lv_obj_get_x(a) <= lv_obj_get_x(l);
    lv_label_set_text(l, txt);
    lv_obj_set_width(ctrl, ls.x + 12 + as.x);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, back ? 0 : ls.x + 12, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, back ? as.x + 12 : 0, 0);
}

lv_obj_t *wt_arrow_action(lv_obj_t *scr, const char *txt, bool back,
                          bool primary, int x, int y, int w, bool right,
                          lv_event_cb_t cb, void *ud)
{
    if (y >= WT_CONTENT_BOTTOM) action_bar_ensure(scr);

    const lv_font_t *f = wt_font23();
    lv_point_t ls, as;
    const char *arrow = back ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT;
    lv_text_get_size(&ls, txt, f, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&as, arrow, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int cw = ls.x + 12 + as.x;

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    // The hit box is the action row's full height and the content's width. No
    // fill, no border, no radius: the box IS the two labels, and anything
    // drawn around them would be the pill this replaces.
    lv_obj_set_size(p, cw, WT_ACTION_H);
    lv_obj_set_pos(p, right ? x + w - cw : x, y);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, ud);

    // The whole control shifts 5px the way its arrow points. Not a sink: the
    // pill's 2px drop reads as a button being pushed into the page, and this
    // is not a button -- it is a direction, so the feedback is movement along
    // it. A style transition so a released press reverses rather than snaps.
    {
        static const lv_style_prop_t props[] = {
            LV_STYLE_TRANSLATE_X, LV_STYLE_PROP_INV
        };
        static lv_style_transition_dsc_t tr;
        static bool tr_ready;
        if (!tr_ready) {
            lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out,
                                         170, 0, NULL);
            tr_ready = true;
        }
        lv_obj_set_style_translate_x(p, 0, 0);
        lv_obj_set_style_translate_x(p, back ? -5 : 5, LV_STATE_PRESSED);
        lv_obj_set_style_transition(p, &tr, 0);
    }

    // The arrow points WHERE THE TAP TAKES YOU: leading the label on the way
    // out, trailing it on the way in.
    lv_obj_t *a = wt_lbl(p, arrow, 0, 0, f, wt_accent());
    lv_obj_add_flag(a, WT_FLAG_ACCENT);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, back ? 0 : ls.x + 12, 0);

    // The primary takes the accent on its LABEL as well, and takes nothing
    // else: there is no fill left to give it.
    lv_obj_t *l = wt_lbl(p, txt, 0, 0, f, primary ? wt_accent() : WT_INK);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    if (primary) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, back ? as.x + 12 : 0, 0);
    return p;
}

// ---- SETTINGS: the full-lane row (see kiss_theme.h) ----
#define WT_WIDE_LX      18    // the label's lane, row local
#define WT_WIDE_LW     250    // ...and its cap. See the header: the gate
                              // compares boxes, not ink.
#define WT_WIDE_CHIP_W 190    // the chip's MINIMUM; a long value widens it
#define WT_WIDE_CHIP_H  40
#define WT_WIDE_SWATCH  16
#define WT_HELP_CHIP_W  30    // wt_help_chip's own size

lv_obj_t *wt_row_wide(lv_obj_t *scr, int y, const wt_wide_t *r)
{
    const bool inert = r->kind == WT_WIDE_INERT;
    const lv_font_t *lf = wt_font23();
    // The sub-line is the page's TEACHING copy -- "not real bitcoin", "what
    // opens your real keys", "amount in sats or BTC" -- and it sat at font14
    // on every row of the settings page because the house rules listed row
    // sublines as metadata and the FIT gate carved them out on the strength of
    // that line. Both were wrong, and it came off the bench as text nobody
    // could read. Metadata is a unit suffix or a chevron. A sentence is not.
    //
    // font23, the next rung up: there is no 18, and adding one is a font
    // rebuild across four scripts. The lane is narrower than the copy at this
    // size, which is a reason to cut words, never to go back down.
    const lv_font_t *sf = wt_font23();
    const lv_font_t *vf = r->vf ? r->vf : wt_font23();
    const lv_font_t *cf = wt_font14();       // chevrons, at the size every row
                                             // on the device already wears

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, WT_WIDE_X, y);
    lv_obj_set_size(row, WT_WIDE_W, WT_WIDE_H);
    // BORDERLESS, like every other row on the device now. This was a card --
    // a WT_PANEL fill, a WT_HAIR edge and a 10px radius -- and a page of six
    // cards is six boxes competing before a word is read. What replaces the
    // edge is a 1px rule UNDER the row and, under a finger, the accent rail
    // and wash wt_line_row wears. Same information, same one-line geometry
    // that lets the page be read straight down the value column; the boxes go.
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (r->cb && !inert) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        wt_line_press(row);
        lv_obj_add_event_cb(row, r->cb, LV_EVENT_CLICKED, r->ud);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    // An inert row is present, stated and dead. With no fill left to keep, what
    // says so is its ink, which the colours below already handle -- and it
    // still gets the rule, because a missing rule would read as a missing row.
    wt_line_rule(scr, WT_WIDE_X, y + WT_WIDE_H, WT_WIDE_W);
    if (r->sev) wt_row_sev(row, r->sev);

    lv_color_t ink  = inert ? WT_DIM : WT_INK;
    lv_color_t subc = inert ? WT_DIM : col_or(r->sub_col, WT_MUT);
    lv_color_t vcol = inert ? WT_DIM : col_or(r->vcol, WT_INK);

    // THE CONTROL FIRST, so the sub-line's lane can be measured against what
    // is actually there. Sizing the sub to the row and hoping is how a
    // translated value ("DESACTIVADO" for OFF) ends up sitting on the words
    // that explain it.
    int lane_end = WT_WIDE_W - 12;
    lv_point_t vs = { 0, 0 };
    if (r->val && *r->val)
        lv_text_get_size(&vs, r->val, vf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    if (r->kind == WT_WIDE_CYCLE || r->kind == WT_WIDE_CHIP) {
        // The mark is the promise: LOOP advances the value where it stands,
        // RIGHT opens a screen. Measured before the chip is sized, because the
        // two glyphs are not the same width.
        const char *mark = r->kind == WT_WIDE_CYCLE
                         ? LV_SYMBOL_LOOP : LV_SYMBOL_RIGHT;
        lv_point_t cs;
        lv_text_get_size(&cs, mark, cf, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        int sw = r->swatch ? WT_WIDE_SWATCH + 10 : 0;
        // The chip grows LEFTWARDS out of its minimum, taking the room from
        // the sub-line rather than from the page margin: the right edge is
        // where the eye reads the value column down, so it does not move.
        int cw = sw + vs.x + 12 + cs.x + 12;
        if (cw < WT_WIDE_CHIP_W) cw = WT_WIDE_CHIP_W;
        int cx = WT_WIDE_W - 12 - cw;

        lv_obj_t *chip = lv_obj_create(row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_pos(chip, cx, (WT_WIDE_H - WT_WIDE_CHIP_H) / 2);
        lv_obj_set_size(chip, cw, WT_WIDE_CHIP_H);
        // No box. The WHOLE ROW is the control -- it carries the callback and
        // the pressed rail -- so an edge drawn around the value was a second
        // control drawn inside the first, and the page read as boxes inside
        // boxes. What says "this one changes" is the mark beside the value,
        // which is what it always was; the border was only ever holding it.
        lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);   // the row takes the tap
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(chip, (void *)WT_ROW_CTRL_TAG);

        int vx = 0;
        if (r->swatch) {
            lv_obj_t *d = lv_obj_create(chip);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, WT_WIDE_SWATCH, WT_WIDE_SWATCH);
            lv_obj_set_style_radius(d, WT_WIDE_SWATCH, 0);
            lv_obj_set_style_bg_color(d, wt_accent(), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            // The swatch IS the value: it has to follow a theme change, and a
            // fill needs the FILL flag -- the plain accent flag only ever
            // repaints text and would fail here in silence.
            lv_obj_add_flag(d, WT_FLAG_ACCENT_FILL);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(d, LV_ALIGN_LEFT_MID, vx, 0);
            vx += WT_WIDE_SWATCH + 10;
        }
        if (r->val && *r->val) {
            lv_obj_t *v = wt_lbl(chip, r->val, 0, 0, vf, vcol);
            lv_obj_align(v, LV_ALIGN_LEFT_MID, vx, 0);
        }
        lv_obj_t *ch = wt_lbl(chip, mark, 0, 0, cf, wt_accent());
        lv_obj_set_style_text_opa(ch, 150, 0);
        lv_obj_add_flag(ch, WT_FLAG_ACCENT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -12, 0);

        lane_end = cx - 12;
    } else if (r->kind == WT_WIDE_OPEN) {
        int right = WT_WIDE_W - 12;
        if (r->cb) {
            lv_obj_t *ch = wt_lbl(row, LV_SYMBOL_RIGHT, 0, 0, cf, wt_accent());
            lv_obj_set_style_text_opa(ch, 150, 0);
            lv_obj_add_flag(ch, WT_FLAG_ACCENT);
            lv_obj_update_layout(ch);
            lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -12, 0);
            right -= lv_obj_get_width(ch) + 12;
        }
        if (r->val && *r->val) {
            lv_obj_t *v = wt_lbl(row, r->val, 0, 0, vf, vcol);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, right - WT_WIDE_W, 0);
            lv_obj_set_user_data(v, (void *)WT_ROW_CTRL_TAG);
            right -= vs.x + 12;
        }
        lane_end = right;
    } else {
        // Inert: no chip and no chevron, because there is nothing to open and
        // nothing to pick. The value sits where a chip's text would have.
        if (r->val && *r->val) {
            lv_obj_t *v = wt_lbl(row, r->val, 0, 0, vf, vcol);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -18, 0);
            lane_end = WT_WIDE_W - 18 - vs.x - 12;
        } else {
            lane_end = WT_WIDE_W - 18;
        }
    }

    // The label, capped and pinned to one line. Both matter: the cap keeps the
    // label's BOX off the value's, and the pin stops a long translation
    // growing a second line into the sub-line beside it.
    lv_obj_t *l = wt_lbl(row, r->label, WT_WIDE_LX, 0, lf, ink);
    lv_obj_set_width(l, WT_WIDE_LW);
    lv_obj_set_height(l, lv_font_get_line_height(lf));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_y(l, (WT_WIDE_H - lv_font_get_line_height(lf)) / 2);
    lv_obj_set_user_data(l, (void *)WT_ROW_LABEL_TAG);

    if (r->sub && *r->sub) {
        int sx = WT_WIDE_LX + WT_WIDE_LW;    // 268: where the label's box ends
        int sw = lane_end - sx;
        if (sw < 40) sw = 40;
        wt_sub_measure(r->sub, sf, sw);
        lv_obj_t *s = wt_lbl(row, r->sub, sx, 0, sf, subc);
        lv_obj_set_width(s, sw);
        lv_obj_set_height(s, lv_font_get_line_height(sf));
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_y(s, (WT_WIDE_H - lv_font_get_line_height(sf)) / 2);
        lv_obj_set_user_data(s, (void *)WT_SUB_TAG);
    }
    return row;
}

lv_obj_t *wt_row_wide_ctrl(lv_obj_t *row)
{
    return wt_tagged(row, WT_ROW_CTRL_TAG);
}

lv_obj_t *wt_row_wide_help(lv_obj_t *row, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *l = wt_tagged(row, WT_ROW_LABEL_TAG);
    if (!l) return NULL;
    const char *txt = lv_label_get_text(l);
    if (!txt) return NULL;

    // Measure the TEXT, never the label. wt_row_wide caps the box at 250 so a
    // long translation ellipsises, so asking the object how wide it is answers
    // 250 for every row in every locale and puts the chip on top of the words.
    lv_point_t sz;
    lv_text_get_size(&sz, txt, wt_font23(), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    int lw = sz.x;
    int cap = WT_WIDE_LW - WT_HELP_CHIP_W - 10;
    if (lw > cap) lw = cap;
    // The label gives up the rest of its lane, so the chip lands after the
    // words rather than inside the label's box -- which the overlap gate reads
    // as the "?" and the label sharing pixels, because by box they do.
    lv_obj_set_width(l, lw);

    // The "?" is a TAP TARGET, so it wears the accent like every other one on
    // the device rather than the colour of a row that cannot be pressed. The
    // guard in round_chip flags both its rim and its glyph.
    lv_obj_t *chip = wt_help_chip(row, 0, 0, wt_accent(), cb, ud);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, WT_WIDE_LX + lw + 10, 0);
    return chip;
}


// The explainer under a group: ONE line, at font23, in the page's own margin.
// Never two, and never smaller: a translation that does not fit on one line at
// this size is copy to shorten, not a paragraph to wrap. This is the shape that
// replaced RECOVERY WORDS' three paragraph body -- one muted line per group
// says what the group is for, and the group itself says the rest.
void wt_group_note(lv_obj_t *pane, int rows, const char *txt)
{
    lv_obj_t *l = wt_lbl(pane, txt, WT_WIDE_X, WT_WIDE_EXPL_Y(rows),
                         wt_font23(), WT_MUT);
    lv_obj_set_width(l, WT_WIDE_W);
    lv_obj_set_height(l, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

// ---- the group that MOVES ----------------------------------------------
// Lifted out of kiss_settings.c when a second page wanted the same chrome.
// Everything here was already page-agnostic -- it only ever reached four
// statics and one tag -- so the move is those five things becoming a context
// the caller owns. Nothing about the timing or the curves changed.
//
// Three jobs, in the order they matter. If any of this ever has to be cut,
// cut from the bottom:
//
//   1. the caution on a flagged row is pointed AT, once, after the row lands.
//      This is the reason a page animates rather than decorating it.
//   2. the destructive group arrives unlike its neighbours -- it rises rather
//      than sliding, slower, without the overshoot, and the pane reddens. The
//      owner knows which group they are in before reading a word.
//   3. the value lands a beat after its label, so the eye reads the setting's
//      NAME and then what it is set to, instead of a grid arriving at once.
//
// Confined to a TAB CHANGE. Walking in from elsewhere, and coming back from
// any screen a row opens, paint settled: a value chip rebuilds the whole page
// on every tap, and three taps to reach SIGNET replaying the entry under the
// owner's finger is not a design, it is a flicker.
#define MO_IN_MS      260   // a row arriving
#define MO_IN_DX       56
#define MO_IN_STEP     38   // and the beat between rows down the group
#define MO_UP_MS      320   // ...except in the stop group, which rises
#define MO_UP_DY       14
#define MO_UP_STEP     44
#define MO_OUT_MS     200   // a row leaving
#define MO_OUT_DX      44
#define MO_OUT_STEP    26
#define MO_FADE_IN     200
#define MO_FADE_UP     240
#define MO_FADE_OUT    160
#define MO_CTRL_MS    220   // the value, a beat behind its label
#define MO_CTRL_DX      7
#define MO_CTRL_LAG    90
#define MO_FLARE_UP   220   // the caution, pointed at once and let go
#define MO_FLARE_DOWN 200   // 200 and not the 420 the handoff drew: at 420 the
                            // page is still moving at 976ms, past the 800 the
                            // kit allows a page change
#define MO_FLARE_LAG  260
#define MO_WASH_MS    280

// A child of a pane that is scenery rather than a row -- the stop group's
// wash. It fades on its own schedule and must not be dealt a row's slide.
static const char WT_PANE_SCENERY[] = "wt_pane_scenery";

void wt_pane_scenery(lv_obj_t *child)
{
    if (child) lv_obj_set_user_data(child, (void *)WT_PANE_SCENERY);
}

static void an_tx(void *v, int32_t x)  { lv_obj_set_style_translate_x(v, x, 0); }
static void an_ty(void *v, int32_t y)  { lv_obj_set_style_translate_y(v, y, 0); }
static void an_opa(void *v, int32_t o) { lv_obj_set_style_opa(v, (lv_opa_t)o, 0); }

// The prototype's ease, cubic-bezier(.17,.84,.32,1.05), typed in as itself.
//
// The handoff calls this "about 5% past the mark, then back" and spends a
// paragraph on how to reproduce the overshoot without LVGL's stock one, which
// is far stronger and reads as bouncy on a page of settings. There is no
// overshoot to reproduce: 1.05 is a CONTROL POINT, not the curve's maximum,
// and the curve it controls peaks at 1.0069 -- four tenths of a pixel on a
// 56px travel, in the browser as much as here. Measured off the frames, the
// row arrives at 25 and stays at 25.
//
// The curve is still not ease_out. It is front loaded: most of the distance is
// gone in the first third, so a row reads as arriving rather than as being
// slid. That is what it is here for, and the overshoot never existed.
static void an_path_settle(lv_anim_t *a)
{
    lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(a, LV_BEZIER_VAL_FLOAT(0.17),
                                 LV_BEZIER_VAL_FLOAT(0.84),
                                 LV_BEZIER_VAL_FLOAT(0.32),
                                 LV_BEZIER_VAL_FLOAT(1.05));
}

// The caution, and it is a RING rather than the dot the prototype draws. The
// rows already carry a warning glyph and an amber rim; a 7px dot growing to 14
// beside them is a detail nobody at the bench would see, and it would be a
// fifth mark on a row that has four. The ring is the row's own edge,
// brightened once. It is a separate object so an interrupted pulse is deleted
// rather than unwound -- there is no half-restored border colour to put back.
static void flare_del(lv_anim_t *a) { lv_obj_delete(a->var); }

static void flare_down(lv_anim_t *a)
{
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, a->var);
    lv_anim_set_exec_cb(&b, an_opa);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&b, MO_FLARE_DOWN);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&b, flare_del);
    lv_anim_start(&b);
}

static void flare(lv_obj_t *row, int delay)
{
    lv_obj_t *ring = lv_obj_create(row);
    lv_obj_remove_style_all(ring);
    lv_obj_set_pos(ring, 0, 0);
    lv_obj_set_size(ring, WT_WIDE_W, WT_WIDE_H);
    lv_obj_set_style_radius(ring, 10, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, WT_WARN, 0);
    lv_obj_set_style_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, an_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, MO_FLARE_UP);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, flare_down);
    lv_anim_start(&a);
}

// The two callbacks that reach past their own object. They go through the
// animation's user_data and never a captured pointer: by the time either
// fires, the pane it meant may already have been deleted, by a second tab tap
// or by the screen closing over it. The CONTEXT outlives both -- it belongs to
// the page's module, not to the objects -- so it is the safe thing to hold.
static void enter_done(lv_anim_t *a)
{
    wt_pane_t *p = lv_anim_get_user_data(a);
    if (p) p->entering = false;
}

static void pane_out_done(lv_anim_t *a)
{
    wt_pane_t *p = a ? lv_anim_get_user_data(a) : NULL;
    if (p && p->pane_out) { lv_obj_delete(p->pane_out); p->pane_out = NULL; }
}

// Asked of the ROW rather than of the page's state, so the pulse and the amber
// card can never disagree about which row it is: WT_SEV_WARN is the page's own
// answer to "does this want reading", and this reads the answer back off the
// object it was written on.
static bool row_wants_reading(lv_obj_t *c)
{
    return lv_obj_get_style_border_opa(c, LV_PART_MAIN) == 77
        && lv_color_eq(lv_obj_get_style_border_color(c, LV_PART_MAIN), WT_WARN);
}

void wt_pane_point(const wt_pane_t *p)
{
    if (!p || !p->pane) return;
    uint32_t n = lv_obj_get_child_count(p->pane);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(p->pane, i);
        if (row_wants_reading(c)) flare(c, 0);
    }
}

void wt_pane_enter(wt_pane_t *p, int dir, bool rise)
{
    if (!p || !p->pane) return;
    uint32_t n = lv_obj_get_child_count(p->pane);
    int k = 0;
    lv_obj_t *last = NULL;       // the last row DEALT, which the latch hangs on
    p->entering = true;

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(p->pane, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);

        // The wash is not a row. It has no lane to come in from: it is the
        // colour of the page changing, so it only deepens.
        if (lv_obj_get_user_data(c) == (void *)WT_PANE_SCENERY) {
            lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
            lv_anim_set_exec_cb(&a, an_opa);
            lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&a, MO_WASH_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
            continue;
        }

        const int delay = k * (rise ? MO_UP_STEP : MO_IN_STEP);
        lv_anim_set_delay(&a, delay);
        if (rise) {
            lv_anim_set_exec_cb(&a, an_ty);
            lv_anim_set_values(&a, MO_UP_DY, 0);
            lv_anim_set_duration(&a, MO_UP_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);   // never overshoot
        } else {
            lv_anim_set_exec_cb(&a, an_tx);
            lv_anim_set_values(&a, dir > 0 ? MO_IN_DX : -MO_IN_DX, 0);
            lv_anim_set_duration(&a, MO_IN_MS);
            an_path_settle(&a);
        }
        // The flag comes off the LAST ROW DEALT, which is not the same as the
        // last child: the wash `continue`s above without an entry animation,
        // and a group whose scenery happened to be built last would leave
        // p->entering true for ever. The symptom of that is silent -- every
        // later tab change would drop its outgoing group instead of sliding
        // it -- so it is guarded here rather than by remembering the order a
        // group builds in.
        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_start(&a);
        last = c;

        lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, rise ? MO_FADE_UP : MO_FADE_IN);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);

        // The value, 90ms behind the label it belongs to. Its own opacity, on
        // top of the row's, so it is still climbing after the row has arrived.
        lv_obj_t *ctrl = wt_row_wide_ctrl(c);
        if (ctrl) {
            lv_obj_set_style_opa(ctrl, LV_OPA_TRANSP, 0);
            lv_anim_set_var(&a, ctrl);
            lv_anim_set_delay(&a, delay + MO_CTRL_LAG);
            lv_anim_set_duration(&a, MO_CTRL_MS);
            lv_anim_start(&a);
            lv_anim_set_exec_cb(&a, an_tx);
            lv_anim_set_values(&a, MO_CTRL_DX, 0);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }

        if (row_wants_reading(c)) flare(c, delay + MO_FLARE_LAG);

        k++;
    }

    // A group of nothing but scenery never settles, so it is already settled.
    if (!last) { p->entering = false; return; }

    // Re-armed on the row that actually finishes last, over its own travel:
    // restarting the same (var, exec_cb) pair replaces the animation LVGL is
    // already running for it rather than adding a second one.
    lv_anim_t z;
    lv_anim_init(&z);
    lv_anim_set_var(&z, last);
    lv_anim_set_exec_cb(&z, rise ? an_ty : an_tx);
    lv_anim_set_values(&z, rise ? MO_UP_DY : (dir > 0 ? MO_IN_DX : -MO_IN_DX), 0);
    lv_anim_set_duration(&z, rise ? MO_UP_MS : MO_IN_MS);
    lv_anim_set_delay(&z, (k - 1) * (rise ? MO_UP_STEP : MO_IN_STEP));
    if (rise) lv_anim_set_path_cb(&z, lv_anim_path_ease_out);
    else      an_path_settle(&z);
    lv_anim_set_user_data(&z, p);
    lv_anim_set_completed_cb(&z, enter_done);
    lv_anim_start(&z);
}

void wt_pane_exit(wt_pane_t *p, int dir)
{
    if (!p) return;
    lv_obj_t *pane = p->pane_out;
    if (!pane) return;
    uint32_t n = lv_obj_get_child_count(pane);
    if (!n) { lv_obj_delete(pane); p->pane_out = NULL; return; }

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(pane, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_delay(&a, i * MO_OUT_STEP);
        lv_anim_set_exec_cb(&a, an_tx);
        lv_anim_set_values(&a, 0, dir > 0 ? -MO_OUT_DX : MO_OUT_DX);
        lv_anim_set_duration(&a, MO_OUT_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        // The slide outlasts the fade, so the pane goes when the LAST row has
        // finished travelling and not when it stopped being visible.
        if (i + 1 == n) {
            lv_anim_set_user_data(&a, p);
            lv_anim_set_completed_cb(&a, pane_out_done);
        }
        lv_anim_start(&a);

        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, MO_FADE_OUT);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    }
}

// Everything moving, stopped, and both lanes accounted for. Deleting a pane
// takes its animations with it -- lv_obj's destructor calls lv_anim_delete --
// which is what makes the callback that would have deleted it never fire.
void wt_pane_stop(wt_pane_t *p)
{
    if (!p) return;
    if (p->pane_out) { lv_obj_delete(p->pane_out); p->pane_out = NULL; }
    p->entering = false;
}

// Every route off a tabbed page drops the screen, and a dozen of them do it
// without a word to the context: the exit, the idle lock, and every row that
// opens a screen of its own. Each deletes the screen and builds its own into
// the same parent, and the pane goes with it while the context still names it.
// The next reopen then deletes a freed object.
//
// So the OBJECT says when it is gone, rather than ten call sites remembering
// to. Comparing against the context is what makes it safe when a page has
// already been replaced: an older pane's delete arrives after the new one has
// been named, matches nothing, and does nothing.
static void pane_gone(lv_event_t *e)
{
    wt_pane_t *p = lv_event_get_user_data(e);
    lv_obj_t  *o = lv_event_get_target(e);
    if (!p) return;
    if (o == p->pane)     p->pane = NULL;
    if (o == p->pane_out) p->pane_out = NULL;
}

static void tabs_gone(lv_event_t *e)
{
    wt_pane_t *p = lv_event_get_user_data(e);
    if (p && lv_event_get_target(e) == p->tabs) p->tabs = NULL;
}

lv_obj_t *wt_pane_new(wt_pane_t *p)
{
    lv_obj_t *o = lv_obj_create(p->scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    // The whole page, and never clipped: a row leaving travels 44px past the
    // lane, and a container sized to the rows would cut it in half. It takes
    // no taps of its own, so the strip and the exit under it stay reachable --
    // LVGL only ever hands a press to a CLICKABLE object and walks past this
    // one to the siblings beneath.
    lv_obj_set_size(o, 800, 480);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, pane_gone, LV_EVENT_DELETE, p);
    return o;
}

void wt_pane_tabs_watch(wt_pane_t *p)
{
    if (p && p->tabs) lv_obj_add_event_cb(p->tabs, tabs_gone, LV_EVENT_DELETE, p);
}

// The whole tab change, which both pages were going to write identically:
// close what the old group owned, drop a group that never finished arriving,
// slide the highlight, build the new one, and send the old one out. `build` is
// the page's own switch over p->tab.
void wt_pane_go(wt_pane_t *p, int tab, bool stop, void (*build)(void))
{
    if (!p || tab == p->tab) return;
    const int dir  = tab > p->tab ? 1 : -1;
    const int from = p->tab;
    p->tab = tab;

    const bool was_moving = p->entering;
    wt_pane_stop(p);
    if (was_moving && p->pane) {
        // Tapping faster than the page settles: the group that never finished
        // arriving is dropped outright rather than sent back out. Sliding a
        // row that has not appeared yet is a flicker, not a transition.
        lv_obj_delete(p->pane);
        p->pane = NULL;
    }

    p->pane_out = p->pane;
    p->pane = wt_pane_new(p);
    build();
    // The new rows were built after the page's own restyle() had already run,
    // so the accent flags on them have never been walked.
    wt_accent_restyle(p->pane);

    if (p->select) p->select(p->tabs, from, tab, stop);
    else           wt_tabs_select(p->tabs, from, tab, stop);
    wt_pane_enter(p, dir, stop);
    wt_pane_exit(p, dir);
}

// The bar on a scrolling list. Four lists wrote this by hand and a fifth wrote
// nothing at all -- and the one that had already been accented was stale on
// every theme change, because accent_walk repaints LV_PART_MAIN and a
// scrollbar is a PART. WT_FLAG_ACCENT_SCROLL is what reaches it.
//
// OPA_50 so it is a tint rather than a stripe, and the caller still decides ON
// or OFF: a bar that appears only after you have scrolled answers the wrong
// question, which is the argument written out in kiss_recv.c.
void wt_list_scrollbar(lv_obj_t *list)
{
    if (!list) return;
    lv_obj_set_style_bg_color(list, wt_accent(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
    lv_obj_add_flag(list, WT_FLAG_ACCENT_SCROLL);
}

// ---- overlays (see kiss_theme.h) ----
lv_obj_t *wt_overlay_box(lv_obj_t *scr, lv_obj_t **scrim_out, int x, int y,
                         int w, int h, int radius, lv_event_cb_t close_cb)
{
    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_70, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    if (close_cb) lv_obj_add_event_cb(scrim, close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(scrim);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, WT_BAR, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, WT_EDGE, 0);
    lv_obj_set_style_shadow_width(box, 40, 0);
    lv_obj_set_style_shadow_offset_y(box, 18, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(box, 140, 0);
    // Clickable with no callback: a tap on the box's own padding is aimed at
    // the box, not past it, so it must not fall through to the scrim's close.
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    if (scrim_out) *scrim_out = scrim;
    return box;
}


lv_obj_t *wt_alert_chip(lv_obj_t *scr, const char *txt,
                        lv_event_cb_t cb, void *ud)
{
    const lv_font_t *lf = wt_font23(), *mf = wt_font14();
    // It stands on the action bar, so make sure there is one. Every screen
    // that grows this chip has an exit too, but the order the two are built in
    // belongs to the caller and this may not depend on it.
    action_bar_ensure(scr);

    lv_point_t is, ls, cs;
    lv_text_get_size(&is, LV_SYMBOL_WARNING, mf, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_text_get_size(&ls, txt, lf, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&cs, LV_SYMBOL_RIGHT, mf, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    int w = is.x + 12 + ls.x + 12 + cs.x;

    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, WT_ACT_X, WT_ACTION_Y);
    lv_obj_set_size(c, w, WT_ACTION_H);
    // No box, like everything else in an action bar now. It was a tinted fill
    // and an amber edge, which is a BOX -- the one shape this look removes --
    // and it was the last one left on the settings page. The mark carries the
    // caution; amber ink says the rest. It still shifts under a finger the way
    // an arrow action does, because it is still a control.
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_translate_x(c, 0, 0);
    lv_obj_set_style_translate_x(c, 5, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *ic = wt_lbl(c, LV_SYMBOL_WARNING, 0, 0, mf, WT_WARN);
    // Placed from ZERO, not from the 18px inset the border used to hold. The
    // inset went with the box and the three parts have to close up behind it,
    // or the chevron sits on top of the last letter of the label.
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l = wt_lbl(c, txt, 0, 0, lf, WT_WARN);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, is.x + 12, 0);
    lv_obj_t *ch = wt_lbl(c, LV_SYMBOL_RIGHT, 0, 0, mf, WT_WARN);
    lv_obj_set_style_text_opa(ch, 180, 0);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
    return c;
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

    // CENTRED, both of them. The fingerprint reveal builds its own box by hand
    // and centres (kiss_ui.c), this one pinned everything at x=16, and the same
    // eight characters therefore sat in two different places depending on which
    // screen asked -- on the passphrase warning it read as a form field with a
    // wide empty right half. The value needs a width before it can be centred:
    // wt_lbl leaves it content sized, which is its own bounding box, so an
    // alignment inside it would mean nothing.
    // The caption takes the accent, the same fix wt_section already had: it is
    // the eyebrow over a figure, which is furniture, and at font14 in WT_MUT it
    // came off the bench as barely visible. Nine call sites in four files draw
    // FINGERPRINT through here, so this has to be the theme's decision or the
    // same caption reads as four different marks.
    lv_obj_t *c = wt_lbl(card, cap, 16, 12, wt_font14(), wt_accent());
    lv_obj_add_flag(c, WT_FLAG_ACCENT);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_width(c, w - 32);
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(c);

    int vy = 12 + lv_obj_get_height(c) + 8;
    lv_obj_t *v = wt_lbl(card, val, 16, vy,
                         big ? wt_font_mono28() : wt_font_mono23(), WT_INK);
    lv_obj_set_style_text_letter_space(v, 2, 0);
    lv_obj_set_width(v, w - 32);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(v);
    // Sized to its content, never to a guess: the caption is translated and the
    // value can be four characters or forty.
    lv_obj_set_size(card, w, vy + lv_obj_get_height(v) + 14);
    return card;
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

// The same, for a pair of why-blocks that HAVE headings: measure the headings
// instead of guessing at them.
//
// Callers were subtracting a constant 46 -- a heading wrapped to two lines --
// plus another 8, from a 166px budget. That is 54px, a third of the room,
// surrendered in every one of 21 locales because one of them MIGHT wrap. On
// the passphrase intro it cost a whole rung: two short English headings that
// occupy 20px were charged 54, and the bodies came out at font23 in a box that
// had room for font28.
//
// wt_why_block already measures its own heading and offsets the body by it, so
// the guess was never load bearing -- it only ever made the font smaller than
// the block would have allowed. This measures the same thing the same way, at
// the same font, so the two agree by construction.
// A why-block heading is HALF THE CLAIM, not a unit suffix. It was pinned at
// font14 whatever the body did, so a pair set at font28 wore labels less than
// half the size of the sentence under them -- reported from the bench as small
// text more than once, on screens whose bodies were already correct. The rung
// now follows the body: only a font14 body keeps a font14 heading.
const lv_font_t *wt_why_head_font(const lv_font_t *body)
{
    return body == wt_font14() ? wt_font14() : wt_font23();
}

// Two passes, because the sizes depend on each other: a bigger heading eats the
// room the body is measured against, and the heading's size is decided BY that
// body. Assume the taller heading first; if the body still lands on font14,
// remeasure with the font14 heading it will actually get, which can only give
// room back. It terminates -- there are two heading rungs and the second is
// strictly smaller.
const lv_font_t *wt_body_font2_head(const char *h1, const char *b1,
                                    const char *h2, const char *b2,
                                    int w, int max_h)
{
    const lv_font_t *hf = wt_font23();
    const lv_font_t *f  = NULL;
    for (int pass = 0; pass < 2; pass++) {
        lv_point_t s1 = {0, 0}, s2 = {0, 0};
        if (h1 && *h1)
            lv_text_get_size(&s1, h1, hf, 0, 0, w, LV_TEXT_FLAG_NONE);
        if (h2 && *h2)
            lv_text_get_size(&s2, h2, hf, 0, 0, w, LV_TEXT_FLAG_NONE);
        int head = s1.y > s2.y ? s1.y : s2.y;
        if (head) head += 6;              // wt_why_block's own heading gap
        int room = max_h - head;
        if (room < 40) room = 40;
        f = wt_body_font2(b1, b2, w, room);
        const lv_font_t *want = wt_why_head_font(f);
        if (want == hf) break;
        hf = want;
    }
    return f;
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
        lv_obj_t *h = wt_lbl(box, head, 14, 0,
                             wt_why_head_font(f ? f : wt_body_font(body, w - 14, max_h)),
                             WT_INK);
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
    // Nine callers pass wt_accent() here, and without the flag every one of
    // them kept the OLD accent after a theme change -- a pink rule beside
    // orange chrome until the screen was rebuilt. accent_walk needs telling,
    // and a fill needs the FILL flag: the plain one only repaints text.
    //
    // Only when the colour IS the accent. The status colours never move, so
    // flagging a WT_WARN rule would repaint a caution the theme's colour.
    if (lv_color_eq(col, wt_accent()))
        lv_obj_add_flag(rule, WT_FLAG_ACCENT_FILL);
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

    // Two columns is what a LIST needs. One entry is not a list, and putting it
    // in a 346px lane leaves the right half of the card empty while wrapping
    // one sentence over five short lines. A single caution is the common case
    // on this card -- most flagged transactions trip exactly one -- and no walk
    // stop ever opened it, so it drew that way for its whole life.
    const int cols = (n == 1) ? 1 : GRID_COLS;
    int rows = (n + cols - 1) / cols;
    int cw   = (EXP_FULL_W - (cols - 1) * GRID_GUT) / cols;   // 346 at two
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

        int cx = 48 + (i % cols) * (cw + GRID_GUT);
        int cy = y + (i / cols) * pitch;

        if (e->icons && (size_t)i < e->icons_count && e->icons[i])
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
    // Opaque, not 245. Ten parts in 255 of a light grey word list on a near
    // black ground is still legible: the seed words behind the explainer read
    // straight through it, competing with the card for the same eye. The card
    // has a title, a rule and its own frame -- it does not need a ghost of the
    // page under it to say it is a layer.
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
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

    // 552..752: the corner, like every other way off a screen. It was centred
    // at 300, which matched neither the old rule nor the new one -- and this is
    // the most opened bar in the app, behind all ten explainers and every "?".
    lv_obj_t *ok = wt_pill(ovl, e->ok_txt, 552, WT_ACTION_Y, 200,
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
    // The operator is the only part of an equation that is pure grammar -- the
    // plus and the arrow between chips that carry the terms -- so it is the
    // part the theme should own. Three callers pass a STATUS glyph through
    // here instead of an operator; they clear the flag at the call site.
    lv_obj_set_style_text_color(l, wt_accent(), 0);
    lv_obj_add_flag(l, WT_FLAG_ACCENT);
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

// What words and a passphrase MAKE. The outcome used to be FINGERPRINT, and
// that was true and useless: a newcomer meeting this on the seed explainer, the
// passphrase intro and the fingerprint help card learned that two things they
// had just been told to protect add up to an eight character code, which is not
// what they add up to. They add up to KEYS. The code is what those keys are
// CALLED, which is a different sentence and now has its own diagram below.
//
// Not "YOUR SIGNER" either, though it was asked for: the signer is this box and
// it does not change when a different passphrase is typed. Teaching that would
// have to be untaught the first time the owner read anything else about bitcoin.
void wt_diagram_fp(lv_obj_t *parent)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip_icon(row, LV_SYMBOL_LIST, tr(STR_D_WORDS), false);
    wt_diagram_op(row, "+");
    wt_chip_icon(row, WT_ICON_LOCK, tr(STR_D_PASSPHRASE), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip_icon(row, WT_ICON_KEY, tr(STR_D_KEYS), true);
}

// The fingerprint explainer's OWN picture, and the reason it exists: the "?" on
// the fingerprint screen used to open a card drawing wt_diagram_fp, which is the
// diagram already on the screen behind it. Tapping for help repeated the answer
// the reader had just decided was not enough.
//
// Two chips, so unlike wt_diagram_fp (about 600px in English, wider in half the
// locales -- see the note in kiss_ui.c's show_fingerprint) this one fits a 344
// column and can go anywhere the pair geometry goes.
//
// The code is passed in rather than read from a seam, because the same card is
// opened for the live wallet from three places and for nothing at all before
// setup. Empty falls back to the word, which is what the title does too.
void wt_diagram_fpid(lv_obj_t *parent, const char *code)
{
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip_icon(row, WT_ICON_KEY, tr(STR_D_KEYS), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, code && code[0] ? code : tr(STR_D_FINGERPRINT), true);
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
    // D_KEYS, not I_T. This chip used to borrow the WALLET page's title because
    // that title was the localised word for a key set. It is not any more -- it
    // named the box for a while and now it says KEYS -- and a diagram that
    // depends on a page title is one rename away from claiming that recovery
    // words rebuild a signer. They rebuild keys, which is what D_KEYS is for.
    wt_chip_icon(row, LV_SYMBOL_OK, tr(STR_D_KEYS), true);
}

void wt_diagram_pair(lv_obj_t *parent)
{
    // the airgap: an online app and the offline signer, bridged only by QR
    lv_obj_t *row = wt_diagram_row(parent);
    wt_chip(row, tr(STR_D_ONLINE_APP), false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT " QR " LV_SYMBOL_LEFT);
    wt_chip(row, tr(STR_D_KISS_OFFLINE), true);
}

// ---- the bundle graph ----------------------------------------------------
//
// Geometry, all of it, in box coordinates. The drawing's path data is written
// in the same box at 1:1, so these ARE the numbers in the appendix rather than
// a reading of them.
//
//   junction      (BJ_X, h/2)          2c h=118 -> 59, 3a h=110 -> 55, 3b h=90 -> 45
//   outputs end   BO_X
//   labels start  BL_X
//   rows          evenly spaced from BMARG to h - BMARG
//
// The row rule is worth stating because one of its consequences is load
// bearing. pitch = (h - 2*BMARG) / (n - 1) reproduces frame 2c exactly (three
// rows at 12 / 59 / 106) and lands within 2px of 3a and 3b -- but the part that
// matters is that with an ODD row count the middle row falls exactly on the
// junction. The elided group strand is that middle row, it is therefore
// perfectly horizontal, and LVGL's software renderer dashes horizontal and
// vertical segments ONLY (draw_line_skew has no dash path at all). A group
// strand one pixel off the junction is a group strand drawn solid, which reads
// as one coin -- the exact thing the dash exists to deny.
#define BMARG   12
#define BJ_X   330
#define BO_X   430
#define BL_X   440
#define BJ_R     5
// The lane the input amounts are right aligned in, measured from the widest of
// them and clamped. 104 is frame 2c's lane, 206 is frame 3a's, where the group
// row carries a count and a total on one line.
#define BLANE_MIN 104
#define BLANE_MAX 206
#define BLANE_GAP  16
// Points per curve. The longest strand spans 210px, so 16 puts a vertex every
// ~14px; with line_rounded the joins disappear at this stroke width.
#define BSEG 16
// The junction at rest and at the end of a hold. Rest is not this animation's
// to move: the resting frame is drawn and approved, so growth is what carries
// the hold and the accent that arrives afterwards means the signature.
#define BJ_HOLD_R 8
// Vertical slack around the box so a row's LABEL, which is centred on its
// strand and therefore reaches above the top row, is not clipped by the
// container. LVGL clips children to their parent.
#define BPAD 16

typedef struct {
    lv_point_precise_t *pts;      // one block for every strand; lv_line borrows it
    uint16_t            n_line;
    lv_obj_t           *line[WT_BUNDLE_MAX];
    lv_obj_t           *amount[WT_BUNDLE_MAX];
    lv_obj_t           *note[WT_BUNDLE_MAX];
    uint8_t             role[WT_BUNDLE_MAX];
    // The hold overlay: one accent line per INPUT strand, drawn over its
    // resting one and truncated to how far the hold has got. Its own point
    // block, because the resting strand's is what the truncation is measured
    // FROM and both are live at once.
    lv_point_precise_t *hpts;
    lv_obj_t           *hline[WT_BUNDLE_MAX];
    uint8_t             nseg[WT_BUNDLE_MAX];  // 2 on a flat strand, BSEG on a curve
    uint16_t            n_in;
    lv_obj_t           *dot;      // the junction, which grows with the hold
    // The output side, and what it takes to redraw its strands when it moves.
    lv_obj_t           *col;      // the scrolling output column, NULL if fixed
    lv_obj_t           *sbox;     // clips the output strands to the graph band
    lv_obj_t           *row[WT_BUNDLE_MAX];   // one per output, in column order
    uint16_t            n_out;
    uint16_t            out0;     // index in line[] where the outputs start
    int16_t             jy;       // the junction, in box coordinates
} wt_bundle_t;

// lv_line_set_points stores the POINTER, not a copy (see kiss_word_ui.c:82 for
// the bug that taught this file the same lesson). The block outlives every
// strand and dies with the container.
static void bundle_delete_cb(lv_event_t *e)
{
    wt_bundle_t *b = lv_event_get_user_data(e);
    if (!b) return;
    lv_free(b->pts);
    lv_free(b->hpts);
    lv_free(b);
}

int wt_strand_px(uint64_t sats, uint64_t max_sats)
{
    if (!max_sats) return 2;
    int px = (int)((sats * 11) / max_sats);       // 11px is the widest strand
    return px < 2 ? 2 : px;                       // 2px floor, or dust vanishes
}

// A cubic sampled into `out`. Both control points share a y with the end they
// belong to, which is what makes these read as one strand bending rather than
// two lines meeting: the curve leaves its row horizontally and arrives at the
// junction horizontally.
static void bundle_curve(lv_point_precise_t *out, int x0, int y0, int x1, int y1,
                         int c0_num, int c1_num)
{
    const int span = x1 - x0;
    const int cx0 = x0 + (span * c0_num) / 100;
    const int cx1 = x0 + (span * c1_num) / 100;
    for (int i = 0; i < BSEG; i++) {
        // Fixed point at 1/1024: this runs on a chip with no FPU worth using
        // and the answer is rounded to a pixel either way.
        const int32_t t  = (int32_t)i * 1024 / (BSEG - 1);
        const int32_t it = 1024 - t;
        const int64_t a = (int64_t)it * it * it;          // (1-t)^3
        const int64_t b = 3LL * it * it * t;              // 3(1-t)^2 t
        const int64_t c = 3LL * it * t * t;               // 3(1-t) t^2
        const int64_t d = (int64_t)t * t * t;             // t^3
        const int64_t den = 1024LL * 1024 * 1024;
        out[i].x = (lv_value_precise_t)((a * x0 + b * cx0 + c * cx1 + d * x1) / den);
        out[i].y = (lv_value_precise_t)((a * y0 + b * y0  + c * y1  + d * y1) / den);
    }
}

static lv_color_t bundle_col(uint8_t role, bool signed_ok)
{
    if (signed_ok) return wt_accent();
    switch (role) {
    case WT_STRAND_SEND:   return WT_INK;
    case WT_STRAND_FEE:    return WT_DIM;
    case WT_STRAND_CHANGE: return wt_accent();
    case WT_STRAND_LINKED: return WT_WARN;
    default:               return WT_MUT;
    }
}

static lv_obj_t *bundle_strand(lv_obj_t *par, const wt_strand_t *s,
                               lv_point_precise_t *pts, int npts, int px)
{
    lv_obj_t *l = lv_line_create(par);
    lv_obj_set_pos(l, 0, 0);
    lv_line_set_points(l, pts, (uint32_t)npts);
    lv_obj_set_style_line_width(l, px, 0);
    lv_obj_set_style_line_color(l, bundle_col(s->role, s->signed_ok), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    if (s->is_group) {
        // Many coins must never read as one coin. This is the only dashed line
        // on the device, and it only renders because the row it sits on is the
        // junction row -- see the geometry note above.
        lv_obj_set_style_line_dash_width(l, 3, 0);
        lv_obj_set_style_line_dash_gap(l, 5, 0);
    }
    if (s->role == WT_STRAND_CHANGE || s->signed_ok)
        lv_obj_add_flag(l, WT_FLAG_ACCENT);
    return l;
}

// One row of text beside a strand, as a flex line so a label and an amount can
// be two different faces on the same baseline: the group row is proportional
// words plus a monospaced total, and the output rows are a monospaced amount
// plus proportional words. `end` right aligns the row in its lane, which is
// what the input side needs and the output side must not have.
static lv_obj_t *bundle_row(lv_obj_t *box, int x, int y, int w, bool end)
{
    lv_obj_t *r = lv_obj_create(box);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, LV_SIZE_CONTENT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, end ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    // Content sized, so it never actually clips anything -- and saying so
    // matters beyond drawing. A clip is what decides whether a cut off label is
    // "below a fold the reader can scroll" or "text nobody can ever read", and
    // the answer is taken from the nearest clipping ancestor. Left unsaid, a
    // row that clips nothing still answers that question, with its own
    // unscrollable self, for a column that scrolls perfectly well.
    lv_obj_add_flag(r, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_pos(r, x, y);
    return r;
}

static lv_obj_t *bundle_txt(lv_obj_t *row, const char *s, const lv_font_t *f,
                            lv_color_t col, bool accent)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    if (accent) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    return l;
}

// Point every output strand at where its row actually IS, measured rather than
// assumed, and let the box clip whatever leaves the band.
//
// This is the rule the scrolling column has to obey and the reason the strands
// are recomputed instead of scrolled: the junction end is fixed and the row end
// is not, so the two halves of one line move differently. Scrolling the strands
// with the column would carry the junction off with them. Clamping the row end
// to the edge would be worse than either -- the strand would appear to arrive
// somewhere its row is not, which is the one thing a line between two facts may
// never do. So it is drawn to the true position and cut where it leaves.
static void bundle_relink(wt_bundle_t *b)
{
    if (!b->col) return;
    const int sy = lv_obj_get_scroll_y(b->col);
    // Row positions are box coordinates; the strands live in sbox, which is the
    // graph BAND with no padding, so everything crossing over loses BPAD.
    const int cy = lv_obj_get_y(b->col) - BPAD;
    for (uint16_t i = 0; i < b->n_out; i++) {
        lv_obj_t *ln = b->line[b->out0 + i];
        lv_obj_t *rw = b->row[i];
        if (!ln || !rw) continue;
        const int ry = cy - sy + lv_obj_get_y(rw) + lv_obj_get_height(rw) / 2;
        lv_point_precise_t *pp = b->pts + (size_t)(b->out0 + i) * BSEG;
        int npts = BSEG;
        if (ry == b->jy) {
            pp[0].x = BJ_X; pp[0].y = b->jy;
            pp[1].x = BO_X; pp[1].y = ry;
            npts = 2;
        } else {
            bundle_curve(pp, BJ_X, b->jy, BO_X, ry, 80, 20);
        }
        lv_line_set_points(ln, pp, (uint32_t)npts);
    }
}

static void bundle_scroll_cb(lv_event_t *e)
{
    bundle_relink(lv_event_get_user_data(e));
}

lv_obj_t *wt_bundle(lv_obj_t *scr, int x, int y, int w, int h,
                    const wt_strand_t *in,  size_t n_in,
                    const wt_strand_t *out, size_t n_out,
                    uint64_t max_sats)
{
    if (n_in > WT_BUNDLE_MAX)  n_in  = WT_BUNDLE_MAX;
    if (n_out > WT_BUNDLE_MAX) n_out = WT_BUNDLE_MAX;

    wt_bundle_t *b = lv_malloc(sizeof *b);
    if (!b) return NULL;
    lv_memzero(b, sizeof *b);
    b->pts = lv_malloc(sizeof(lv_point_precise_t) * BSEG * (n_in + n_out));
    if (!b->pts) { lv_free(b); return NULL; }
    // Inputs only: the hold is about the coins being committed, and the output
    // side is already standing down by the time any of this moves.
    b->hpts = lv_malloc(sizeof(lv_point_precise_t) * BSEG * (n_in ? n_in : 1));
    if (!b->hpts) { lv_free(b->pts); lv_free(b); return NULL; }

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y - BPAD);
    lv_obj_set_size(box, w, h + 2 * BPAD);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(box, bundle_delete_cb, LV_EVENT_DELETE, b);

    const int jy = BPAD + h / 2;
    char amt[32];

    // ONE input row is the input TOTAL, and the total is half the arithmetic a
    // reader does on this screen: what came in, less what goes out, is the fee.
    // So it takes the same rung as the amounts on the other side of the
    // junction rather than the rung of a breakdown line. Reported from the
    // bench as the input total being the smallest text on the screen, which it
    // was: mono14, against mono23 outputs 300px to its right.
    //
    // More than one row and they are a BREAKDOWN -- these coins, this size
    // each -- so they stay at mono14 and the caller puts the total on the
    // caption line above them, where there is room for it at mono23 and where
    // no coin row can be mistaken for the sum.
    const lv_font_t *in_f = (n_in == 1) ? wt_font_mono23() : wt_font_mono14();

    // The input lane, measured rather than assumed: the group row carries a
    // count AND a total on one line, which is why frame 3a's lane is twice
    // frame 2c's.
    int lane = BLANE_MIN;
    for (size_t i = 0; i < n_in; i++) {
        lv_point_t ts;
        int wid = 0;
        wt_fmt_amount(in[i].sats, amt, sizeof amt);
        lv_text_get_size(&ts, amt, in_f, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        wid = ts.x;
        if (in[i].label) {
            lv_text_get_size(&ts, in[i].label, wt_font14(), 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            wid += ts.x + 8;                  // + bundle_row's pad_column
        }
        if (wid > lane) lane = wid;
    }
    if (lane > BLANE_MAX) lane = BLANE_MAX;
    const int ix0 = lane + BLANE_GAP;

    lv_point_precise_t *pp = b->pts;
    const int in_lh  = lv_font_get_line_height(in_f);
    const int out_lh = lv_font_get_line_height(wt_font_mono23());

    for (size_t i = 0; i < n_in; i++) {
        const int ry = BPAD + (n_in < 2 ? h / 2
                        : BMARG + (int)i * (h - 2 * BMARG) / (int)(n_in - 1));
        int npts = BSEG;
        if (ry == jy) {                       // flat: two points, so it dashes
            pp[0].x = ix0;  pp[0].y = ry;
            pp[1].x = BJ_X; pp[1].y = jy;
            npts = 2;
        } else {
            bundle_curve(pp, ix0, ry, BJ_X, jy, 43, 62);
        }
        const int k = b->n_line;
        const int px = wt_strand_px(in[i].sats, max_sats);
        b->line[k] = bundle_strand(box, &in[i], pp, npts, px);
        b->role[k] = in[i].role;
        b->nseg[k] = (uint8_t)npts;
        // The overlay, built now and hidden, so a tick allocates nothing and
        // creates nothing. Same width and same dash as the strand underneath:
        // a dashed strand means MANY COINS, and a solid accent line drawn over
        // it during the hold would say one coin was committing.
        b->hline[k] = bundle_strand(box, &in[i], pp, npts, px);
        lv_obj_set_style_line_color(b->hline[k], wt_accent(), 0);
        lv_obj_add_flag(b->hline[k], WT_FLAG_ACCENT);
        lv_obj_add_flag(b->hline[k], LV_OBJ_FLAG_HIDDEN);
        pp += BSEG;

        const bool acc = in[i].signed_ok;
        lv_obj_t *row = bundle_row(box, 0, ry - in_lh / 2, lane, true);
        wt_fmt_amount(in[i].sats, amt, sizeof amt);
        if (in[i].label)                      // the group row: words, then the total
            b->note[k] = bundle_txt(row, in[i].label, wt_font14(),
                                    acc ? wt_accent() : WT_MUT, acc);
        // WT_INK when this row IS the total, WT_MUT when it is one coin of
        // several: the number a reader has to READ is never the dimmest thing
        // on the screen, and the breakdown under a total is not that number.
        // The signature still lands on it -- SIGNING already paints inputs
        // WT_INK and the reveal crosses from there to the accent, so a total
        // that starts at WT_INK loses a step it never used and keeps the one
        // that says a signature exists.
        b->amount[k] = bundle_txt(row, amt, in_f,
                                  acc ? wt_accent()
                                      : (n_in == 1 ? WT_INK : WT_MUT), acc);
        wt_denom_bind(b->amount[k]);   // every figure is the switch, not one
        b->n_line++;
    }

    b->n_in = b->n_line;

    // ---- the output column ----
    //
    // Outputs are NEVER elided, at any count. Bundling inputs is safe -- they
    // are all yours and their total is the fact -- but each output is a place
    // your money goes, and one folded into a group would be a destination
    // visible nowhere. The column scrolls instead, exactly as the panel it
    // replaces did, and the caller keeps its read-to-the-end gate.
    //
    // Rows are content-sized inside a flex column rather than placed at
    // computed y's, because one of them may be a paragraph: a silent payment
    // says a second thing about itself. Their real positions are read back
    // after layout, which is also what makes the strands correct while
    // scrolling.
    const int row_h = out_lh;
    int pitch = (n_out < 2) ? row_h : (h - 2 * BMARG) / (int)(n_out - 1);
    if (pitch < row_h + 4) pitch = row_h + 4;      // uniform, and it will scroll
    b->out0 = b->n_line;
    b->n_out = (uint16_t)n_out;
    b->jy    = (int16_t)(jy - BPAD);      // sbox coordinates

    // The output strands get their own clip, and it is not the box. The box
    // carries BPAD of slack top and bottom so a label centred on the first row
    // is not cut in half -- but a strand aimed at a row that has scrolled out
    // of view must stop at the GRAPH, not BPAD above it, or it runs through the
    // caption sitting on that line. Same reason the strand is clipped rather
    // than clamped: where it stops has to be a boundary of the drawing, not a
    // number chosen to make it look tidy.
    lv_obj_t *sbox = lv_obj_create(box);
    lv_obj_remove_style_all(sbox);
    lv_obj_set_pos(sbox, 0, BPAD);
    lv_obj_set_size(sbox, w, h);
    lv_obj_remove_flag(sbox, LV_OBJ_FLAG_SCROLLABLE);
    b->sbox = sbox;

    // The column's own box is the clip, and where it starts matters twice over.
    //
    // It may not reach the box's top edge: the box carries BPAD of slack, and a
    // row scrolled to the top of a full column would rise into it and share
    // pixels with the caption sitting on that line -- "WHERE IT GOES" and a
    // recipient's amount, overlapping, on the screen that says where the money
    // goes. And it may not end on the box's edge either: a row cut by the BOX
    // is a row cut by something that does not scroll, which reads as text
    // clipped rather than text below a fold, and the gate says so.
    //
    // So the column clips itself, strictly inside the box, and starts 2px above
    // the first row's band so the rows still land on the spread the appendix
    // draws rather than half a line height below it.
    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, BL_X, BPAD + BMARG - row_h / 2);
    lv_obj_set_size(col, w - BL_X, h + row_h - 2 * BMARG);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, pitch - row_h, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_style_width(col, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(col, WT_MUT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(col, LV_OPA_50, LV_PART_SCROLLBAR);
    b->col = col;

    for (size_t i = 0; i < n_out; i++) {
        const int k = b->n_line;
        // Placed at the junction for now; bundle_relink puts every one of them
        // on its row once the column has been laid out.
        bundle_curve(pp, BJ_X, jy - BPAD, BO_X, jy - BPAD, 80, 20);
        if (!out[i].note_only) {
            b->line[k] = bundle_strand(sbox, &out[i], pp, BSEG,
                                       wt_strand_px(out[i].sats, max_sats));
            b->role[k] = out[i].role;
        }
        pp += BSEG;

        // A COLUMN, because a row may be more than one line: an amount and its
        // word, then the address it pays. The strand aims at the whole row's
        // middle, so a row that grows stays joined to its line.
        const int roww = w - BL_X - 8;             // 8 clear of the scrollbar
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, roww);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   // see bundle_row
        b->row[i] = row;

        if (out[i].note_only) {           // words only: the row without an output
            lv_obj_t *n = bundle_txt(row, out[i].label ? out[i].label : "",
                                     wt_font14(), WT_MUT, false);
            lv_obj_set_width(n, roww);
            lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
            b->note[k] = n;
            b->n_line++;
            continue;
        }

        lv_obj_t *line = lv_obj_create(row);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, roww);
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(line, 8, 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // see bundle_row

        const bool acc = (out[i].role == WT_STRAND_CHANGE);
        // The STRAND is DIM for a fee and INK for the send -- that is the
        // thickness reading, and it is in the appendix. The AMOUNT is ink
        // whatever the row, because WT_DIM is the colour of something present
        // but inert, and a fee is neither: it is the number an owner is most
        // likely to be checking. Only change takes the accent, and it is the
        // only accent text in the graph.
        const lv_color_t oc = acc ? wt_accent() : WT_INK;
        wt_fmt_amount(out[i].sats, amt, sizeof amt);
        b->amount[k] = bundle_txt(line, amt, wt_font_mono23(), oc, acc);
        wt_denom_bind(b->amount[k]);
        if (out[i].label)
            b->note[k] = bundle_txt(line, out[i].label, wt_font14(),
                                    acc ? oc : WT_MUT, acc);
        // A destination these keys have paid before. A bare mark, no word: the
        // row already carries an amount, a label and an address, and the one
        // thing being added is "you have been here". The words for it are on
        // the address card behind the "?" -- see kiss_payee.h for why nothing
        // is drawn on a first payment.
        if (out[i].known)
            bundle_txt(line, LV_SYMBOL_REFRESH, wt_font14(), WT_MUT, false);
        // THE FOLD, the same one RECEIVE and the single-recipient card draw:
        // prefix, the four after it, an ellipsis, the last twelve with the
        // final eight lit. One line per destination, whatever its length.
        //
        // It used to be the whole address wrapped in a 304px lane, which took
        // two lines for a bech32 and four for a silent payment -- so a column
        // sized for three rows held one and a half destinations, and the row at
        // the fold was cut through the middle of an address rather than at a
        // line. Folding is what makes a row a row here.
        //
        // Same habit on every screen that shows a destination, which is the
        // whole reason the single-recipient card folds too.
        if (out[i].addr)
            wt_addr_short(row, out[i].addr, wt_font_mono14());
        b->n_line++;
    }

    // Measured, not counted: whether this column overflows depends on the
    // locale and on whether any row is a paragraph, which no output count can
    // answer. MODE_ON rather than AUTO for the same reason the panel used it --
    // a list with more below the fold must not look identical to one that ends
    // there.
    lv_obj_update_layout(col);
    const bool overflows = lv_obj_get_scroll_bottom(col) > 0;
    lv_obj_set_scrollbar_mode(col, overflows ? LV_SCROLLBAR_MODE_ON
                                             : LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(col, bundle_scroll_cb, LV_EVENT_SCROLL, b);
    bundle_relink(b);

    // The junction, last, so it sits over every strand that reaches it. A dot
    // rather than a joint: the strands genuinely meet here, and a gap where
    // eleven pixels of stroke cross two of stroke reads as a rendering fault.
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, BJ_R * 2, BJ_R * 2);
    lv_obj_set_pos(dot, BJ_X - BJ_R, jy - BJ_R);
    lv_obj_set_style_radius(dot, BJ_R, 0);
    lv_obj_set_style_bg_color(dot, WT_INK, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    b->dot = dot;
    return box;
}

// The state block is hung off the delete callback rather than user_data, which
// wt_screen already spends on its own tag. Reading it back through the same
// event is the one place that is not a layering violation.
static wt_bundle_t *bundle_state(lv_obj_t *bundle)
{
    uint32_t n = lv_obj_get_event_count(bundle);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(bundle, i);
        if (lv_event_dsc_get_cb(d) == bundle_delete_cb)
            return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

static void bundle_repaint(wt_bundle_t *b, int k, lv_color_t line_col,
                           lv_color_t txt_col, bool accent)
{
    if (b->line[k]) {
        lv_obj_set_style_line_color(b->line[k], line_col, 0);
        if (accent) lv_obj_add_flag(b->line[k], WT_FLAG_ACCENT);
        else        lv_obj_remove_flag(b->line[k], WT_FLAG_ACCENT);
    }
    lv_obj_t *t[2] = { b->amount[k], b->note[k] };
    for (int i = 0; i < 2; i++) {
        if (!t[i]) continue;
        lv_obj_set_style_text_color(t[i], txt_col, 0);
        if (accent) lv_obj_add_flag(t[i], WT_FLAG_ACCENT);
        else        lv_obj_remove_flag(t[i], WT_FLAG_ACCENT);
    }
}

// The junction, sized and placed from one radius. Its centre never moves; only
// the radius does, so the grow and the retract are the same line of arithmetic.
static void bundle_dot_r(wt_bundle_t *b, int r)
{
    if (!b->dot) return;
    lv_obj_set_size(b->dot, r * 2, r * 2);
    lv_obj_set_pos(b->dot, BJ_X - r, b->jy + BPAD - r);
    lv_obj_set_style_radius(b->dot, r, 0);
}

void wt_bundle_hold(lv_obj_t *bundle, uint8_t progress)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    if (!b) return;

    for (uint16_t k = 0; k < b->n_in; k++) {
        lv_obj_t *ov = b->hline[k];
        if (!ov) continue;
        if (!progress) { lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN); continue; }

        // Walk the table the strand was already sampled into at build time.
        // The cubic is not re-evaluated here: sixteen points per strand exist
        // precisely so a tick every 30ms is a copy and one interpolation, not
        // four multiplies per point on a chip with no FPU worth using.
        //
        // n is not always BSEG. An input whose row lands ON the junction row is
        // written as two points so it can dash, and at twenty inputs that flat
        // row is the grouped strand -- the common case, not the corner.
        const int n = b->nseg[k];
        const lv_point_precise_t *src = b->pts  + (size_t)k * BSEG;
        lv_point_precise_t       *dst = b->hpts + (size_t)k * BSEG;
        const int32_t t   = (int32_t)progress * (n - 1);
        const int     q   = t / 255;
        const int32_t rem = t - (int32_t)q * 255;
        int npts = q + 1;
        for (int i = 0; i <= q; i++) dst[i] = src[i];
        if (q < n - 1) {
            // The head of the line, between two sampled points. Without it the
            // strand would advance a vertex at a time and read as sixteen steps
            // rather than one continuous reach.
            const int32_t ax = (int32_t)src[q].x,     ay = (int32_t)src[q].y;
            const int32_t bx = (int32_t)src[q + 1].x, by = (int32_t)src[q + 1].y;
            dst[q + 1].x = (lv_value_precise_t)(ax + (bx - ax) * rem / 255);
            dst[q + 1].y = (lv_value_precise_t)(ay + (by - ay) * rem / 255);
            npts = q + 2;
        }
        lv_line_set_points(ov, dst, (uint32_t)npts);
        lv_obj_remove_flag(ov, LV_OBJ_FLAG_HIDDEN);
    }

    bundle_dot_r(b, BJ_R + (BJ_HOLD_R - BJ_R) * progress / 255);
}

void wt_bundle_state(lv_obj_t *bundle, int state)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    if (!b) return;
    // SIGNING keeps the overlay exactly where the hold left it -- full length,
    // in the accent. Hiding it there would paint the inputs back down to INK
    // for the few hundred milliseconds libwally is busy and then up to the
    // accent again, and a strand that dims at the moment of commitment says the
    // opposite of what happened. LIVE and SIGNED both take it away, and SIGNED
    // does it in the same call that paints the strands underneath, so the
    // pixels do not change.
    if (state != WT_BUNDLE_SIGNING && state != WT_BUNDLE_HOLDING)
        for (uint16_t k = 0; k < b->n_in; k++)
            if (b->hline[k]) lv_obj_add_flag(b->hline[k], LV_OBJ_FLAG_HIDDEN);
    if (state == WT_BUNDLE_LIVE) bundle_dot_r(b, BJ_R);
    if (b->dot) {
        // Every input strand arrives in the accent, so the thing they arrive AT
        // wears it too. Eleven pixels of accent stroke ending on a white disc
        // is the seam this dot exists to prevent, and it would appear at the
        // one moment the drawing is being read hardest.
        const bool acc = (state == WT_BUNDLE_SIGNED);
        lv_obj_set_style_bg_color(b->dot, acc ? wt_accent() : WT_INK, 0);
        if (acc) lv_obj_add_flag(b->dot, WT_FLAG_ACCENT_FILL);
        else     lv_obj_remove_flag(b->dot, WT_FLAG_ACCENT_FILL);
    }
    for (int k = 0; k < (int)b->n_line; k++) {
        const bool is_in = (k < (int)b->out0);
        if (state == WT_BUNDLE_HOLDING) {
            // Outputs stand down, and so does a LINKED input's warn colour --
            // to the plain mute, not up to WT_INK. Both directions matter and
            // for the same reason: the fill drawn over these is the accent, and
            // it needs something to be visible against. WT_INK is too close to
            // the accent in MONO, and WT_WARN's amber is too close to it in
            // ORANGE -- on a merge, in that theme, the whole animation
            // disappeared into a strand that was already orange.
            //
            // The caution is not being retracted: the bar below still says it,
            // it has already been acknowledged to get here, and the strands
            // wear it again the moment the hold is let go. What the screen is
            // about for these 1200ms is the commitment, not the warning.
            bundle_repaint(b, k, is_in ? WT_MUT : WT_EDGE,
                           is_in ? (b->n_in == 1 ? WT_INK : WT_MUT) : WT_EDGE,
                           false);
        } else if (state == WT_BUNDLE_SIGNING) {
            // Inputs at full strength, outputs stood down. The note rows go with
            // their side: a silent payment's claim is about an output, so it
            // dims with the output it belongs to.
            bundle_repaint(b, k, is_in ? WT_INK : WT_EDGE,
                           is_in ? WT_INK : WT_EDGE, false);
        } else if (state == WT_BUNDLE_SIGNED && is_in) {
            bundle_repaint(b, k, wt_accent(), wt_accent(), true);
        } else if (!is_in) {
            // Back to what the row means, taken from its role rather than
            // remembered: the destinations are readable again the moment there
            // is a signature over them.
            const bool acc = (b->role[k] == WT_STRAND_CHANGE);
            bundle_repaint(b, k, bundle_col(b->role[k], false),
                           acc ? wt_accent() : (b->amount[k] ? WT_INK : WT_MUT),
                           acc);
            if (b->note[k] && !acc)
                lv_obj_set_style_text_color(b->note[k], WT_MUT, 0);
        } else {
            // An input at rest, taken from its role rather than assumed to be
            // muted: a linked one wears WT_WARN and has to come back to it
            // after a hold is let go. The LABEL stays muted either way -- the
            // strand is what the caution is about, and an amount in WT_WARN
            // would read as something wrong with that number.
            //
            // One input row is the input total and rests at WT_INK; several
            // are a breakdown and rest muted. Same test wt_bundle built them
            // under, so a hold let go puts back what was drawn.
            bundle_repaint(b, k, bundle_col(b->role[k], false),
                           b->n_in == 1 ? WT_INK : WT_MUT, false);
        }
    }
}

// ---- the signature landing (see kiss_theme.h) ----
// One value, every input strand, and no per input timing anywhere in it. The
// end state is exactly what wt_bundle_state(SIGNED) paints, and it is that call
// that lands it -- this only fills in the frames between SIGNING and there.
static void bundle_reveal_exec(void *var, int32_t v)
{
    wt_bundle_t *b = bundle_state((lv_obj_t *)var);
    if (!b) return;
    const lv_color_t acc = wt_accent();
    for (int k = 0; k < (int)b->out0; k++) {         // inputs only
        // WT_INK is where SIGNING left them, which is what makes this a
        // crossing rather than a jump from whatever each strand happened to be.
        lv_color_t c = lv_color_mix(acc, WT_INK, (uint8_t)v);
        if (b->line[k])   lv_obj_set_style_line_color(b->line[k], c, 0);
        if (b->amount[k]) lv_obj_set_style_text_color(b->amount[k], c, 0);
    }
    // The junction takes the same value, so the discs and the strokes meeting
    // at it are never two different colours -- the seam wt_bundle_state's dot
    // branch exists to prevent, in the frames it does not cover.
    if (b->dot)
        lv_obj_set_style_bg_color(b->dot, lv_color_mix(acc, WT_INK, (uint8_t)v), 0);
}

static void bundle_reveal_done(lv_anim_t *a)
{
    wt_bundle_state((lv_obj_t *)a->var, WT_BUNDLE_SIGNED);
}

void wt_bundle_signed_reveal(lv_obj_t *bundle, uint32_t ms)
{
    if (!bundle || !bundle_state(bundle)) return;
    if (!ms) { wt_bundle_state(bundle, WT_BUNDLE_SIGNED); return; }
    // The hold overlay goes FIRST and on its own. It is the accent already, at
    // full length, so leaving it up would put the finished colour over strands
    // still crossing to it and the crossing would be invisible under its own
    // answer.
    wt_bundle_t *b = bundle_state(bundle);
    for (uint16_t k = 0; k < b->n_in; k++)
        if (b->hline[k]) lv_obj_add_flag(b->hline[k], LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bundle);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, ms);
    // ease_out, the same curve the explainer card enters on: fast where the
    // answer arrives, slow where it settles.
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, bundle_reveal_exec);
    lv_anim_set_completed_cb(&a, bundle_reveal_done);
    lv_anim_start(&a);
}

lv_obj_t *wt_bundle_outputs(lv_obj_t *bundle)
{
    wt_bundle_t *b = bundle ? bundle_state(bundle) : NULL;
    return b ? b->col : NULL;
}

// Reserve exactly what this iteration writes, which is a character and, only
// on a group boundary, a space before it.
//
// The old guard reserved two every time, so a caller whose buffer was sized to
// the exact answer lost its final character. That is not a hypothetical: a
// txid is 64 hex characters, blocks to 79, and gt[80] on the DETAILS page is
// 79 plus the NUL -- correct, and the one size the over-reservation bit. The
// id rendered 63 characters long with no ellipsis and no gap, its last block
// three wide instead of four, which is what plenty of honest addresses look
// like. An owner comparing it against a coordinator matched every leading
// block and had nothing to tell them the end was missing; an owner who copied
// it down to look the transaction up later wrote down an id that matches
// nothing on chain.
//
// Every other caller had slack and is unaffected, byte for byte. fitcheck
// keeps it that way.
void wt_group4(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!out || !out_len) return;
    // A trailing group of one or two characters JOINS the group before it. An
    // address is rarely a multiple of four -- a 42 character bech32 leaves two
    // -- and that stub is a separate token, so a line that is one group too
    // long wraps it alone: the verify screen showed forty characters on one
    // line and "kz" on the next, which reads as the address being cut off.
    // Merged, the last token is five or six characters and there is no orphan
    // to strand. Nothing is dropped and no other group changes.
    size_t n = strlen(in);
    size_t rem = n % 4;
    size_t last = (rem == 1 || rem == 2) && n > 4 ? n - rem - 4 : n;
    for (size_t i = 0; in[i]; i++) {
        size_t need = (i && i % 4 == 0 && i <= last) ? 2u : 1u;
        if (o + need + 1 > out_len) break;          // +1 keeps room for the NUL
        if (need == 2) out[o++] = ' ';
        out[o++] = in[i];
    }
    out[o] = 0;
}

void wt_fmt_bytes(uint64_t bytes, char *out, size_t out_len)
{
    // One decimal, unit picked by size: a firmware image reads in MB, a card
    // in GB -- "31981.6 MB" is a number nobody can compare with the sticker on
    // the card. uint64 arithmetic throughout: the old MB-only helper multiplied
    // a size_t by ten, which overflows 32 bits past ~400 MB on the device.
    if (bytes >= 1073741824ull) {
        unsigned gb10 = (unsigned)((bytes * 10 + 536870912ull) / 1073741824ull);
        snprintf(out, out_len, "%u.%u GB", gb10 / 10, gb10 % 10);
    } else {
        unsigned mb10 = (unsigned)((bytes * 10 + 524288) / 1048576);
        snprintf(out, out_len, "%u.%u MB", mb10 / 10, mb10 % 10);
    }
}

void wt_fmt_btc(uint64_t sats, char *out, size_t out_len)
{
    // full 8 decimals, never abbreviated: this string exists to be compared
    // digit-by-digit against a coordinator that displays BTC
    snprintf(out, out_len, "%llu.%08llu",
             (unsigned long long)(sats / 100000000ULL),
             (unsigned long long)(sats % 100000000ULL));
}

// The owner's unit. Sats is the default because this is a signer and the
// numbers it shows are compared against a coordinator's transaction view,
// which counts in sats far more often than not; BTC is one row away for anyone
// whose coordinator counts the other way. It is a display preference and
// nothing else -- every amount on this device is a uint64 of satoshis, and the
// setting never reaches storage, a PSBT, or a signature.
static int s_denom = WT_DENOM_SATS;
int  wt_denom(void)        { return s_denom; }
void wt_denom_set(int d)   { s_denom = d == WT_DENOM_BTC ? WT_DENOM_BTC
                                                         : WT_DENOM_SATS; }
const char *wt_denom_unit(void)
{
    return s_denom == WT_DENOM_BTC ? "BTC" : "sats";
}

// The same amount in the OTHER unit, for the places that show both: the sign
// screen prints the total large in the chosen one and small in the other, so
// whichever way a coordinator counts, the number is on the glass without a
// trip to Settings.
// Any label carrying an amount becomes the switch. The preference belongs to
// the number, not to a settings page: wherever a figure is being compared
// against a coordinator that counts the other way, the fix is a tap on the
// figure. wt_denom_bind is what every screen calls after drawing one.
static void (*s_denom_tap)(void);
void wt_denom_on_tap(void (*fn)(void)) { s_denom_tap = fn; }

static void denom_lbl_cb(lv_event_t *e)
{
    (void)e;
    if (s_denom_tap) s_denom_tap();
}

void wt_denom_bind(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    // 12 in every direction: an amount is type, not a button, and a bare
    // label's box is exactly its glyphs. This is what makes a 14px figure
    // reachable without giving it a border it should not have.
    lv_obj_set_ext_click_area(o, 12);
    lv_obj_add_event_cb(o, denom_lbl_cb, LV_EVENT_CLICKED, NULL);
}

void wt_fmt_amount(uint64_t sats, char *out, size_t out_len)
{
    if (s_denom == WT_DENOM_BTC) wt_fmt_btc(sats, out, out_len);
    else                         wt_fmt_sats(sats, out, out_len);
}

void wt_fmt_amount_alt(uint64_t sats, char *out, size_t out_len)
{
    if (s_denom == WT_DENOM_BTC) wt_fmt_sats(sats, out, out_len);
    else                         wt_fmt_btc(sats, out, out_len);
}

const char *wt_denom_unit_alt(void)
{
    return s_denom == WT_DENOM_BTC ? "sats" : "BTC";
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
