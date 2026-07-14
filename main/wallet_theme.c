// Shared wallet UI kit. See wallet_theme.h. Every builder here matches the
// house style the screens shipped with, so porting a screen to the kit must
// not change a rendered pixel while the accent is MONO.
#include "wallet_theme.h"

#include <stdio.h>
#include <string.h>

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

lv_obj_t *wt_screen(lv_obj_t *parent, const char *title, const char *sub)
{
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, WT_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(scr);

    lv_obj_t *cap = lv_label_create(scr);
    lv_label_set_text(cap, title);
    lv_obj_set_style_text_color(cap, wt_accent(), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 48, 30);

    if (sub) {
        lv_obj_t *s = lv_label_create(scr);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_color(s, WT_MUT, 0);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(s, 48, 68);
    }
    return scr;
}

lv_obj_t *wt_pillh(lv_obj_t *scr, const char *txt, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_radius(p, 26, 0);
    lv_obj_set_style_bg_color(p, WT_KEY, 0);
    lv_obj_set_style_bg_color(p, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, WT_MUT, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, WT_INK, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    return p;
}

lv_obj_t *wt_pill(lv_obj_t *scr, const char *txt, int x, int y, int w,
                  lv_event_cb_t cb, void *ud)
{
    return wt_pillh(scr, txt, x, y, w, 52, cb, ud);
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

lv_obj_t *wt_wrap(lv_obj_t *scr, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *wt_section(lv_obj_t *scr, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, WT_MUT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *wt_qr_card(lv_obj_t *scr, lv_obj_t **qr, int x, int y, int card_px, int qr_px)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_px, card_px);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, WT_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_t *q = lv_qrcode_create(card);
    if (q) {
        lv_qrcode_set_size(q, qr_px);
        lv_qrcode_set_dark_color(q, lv_color_hex(0x0B0E14));
        lv_qrcode_set_light_color(q, WT_CARD);
        lv_obj_center(q);
    }
    if (qr) *qr = q;
    return card;
}

lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f)
{
    int len = (int)strlen(grouped);
    int h = len, t = len, raw = 0;
    for (int i = 0; i < len; i++) {
        if (grouped[i] != ' ' && ++raw == 4) { h = i + 1; break; }
    }
    raw = 0;
    for (int i = len - 1; i > h; i--) {
        if (grouped[i] != ' ' && ++raw == 4) { t = i; break; }
    }
    char head[8], mid[120];
    snprintf(head, sizeof head, "%.*s", h, grouped);
    snprintf(mid, sizeof mid, "%.*s", t - h, grouped + h);

    lv_obj_t *sg = lv_spangroup_create(par);
    lv_obj_set_width(sg, w);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, f, 0);
    lv_span_t *s1 = lv_spangroup_new_span(sg);
    lv_span_set_text(s1, head);
    lv_style_set_text_color(lv_span_get_style(s1), wt_accent());
    lv_span_t *s2 = lv_spangroup_new_span(sg);
    lv_span_set_text(s2, mid);
    lv_style_set_text_color(lv_span_get_style(s2), WT_MUT);
    lv_span_t *s3 = lv_spangroup_new_span(sg);
    lv_span_set_text(s3, grouped + t);
    lv_style_set_text_color(lv_span_get_style(s3), wt_accent());
    lv_spangroup_refresh(sg);
    return sg;
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
