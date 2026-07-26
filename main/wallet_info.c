// The WALLET tile. Three jobs, one section:
//   facts   — fingerprint / network / address type / first address, each with a
//             small "?" chip that opens a plain-words explainer (new users learn,
//             experienced users ignore).
//   pair    — the coordinator export: descriptor for Sparrow-family apps, key
//             origin + SLIP-132 zpub for BlueWallet (it doesn't read descriptors).
//   words   — re-view the backup words after a be-alone warning. Words on paper
//             only: no seed-as-QR export in any form (owner's rule).
// Compiled in BOTH device and sim builds; sim stubs the crypto seams.
#include "wallet_info.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_seed.h"
#include "wallet_setup.h"   // wallet_setup_open_verify: check the paper backup
#include "wallet_theme.h"
#include "wallet_ui.h"   // wallet_ui_last_fp

static lv_obj_t *s_scr;                 // whichever wallet-section screen is up
static lv_obj_t *s_parent;
static int s_pair_fmt;                  // 0 = descriptor (Sparrow), 1 = BlueWallet
static lv_obj_t *s_pair_pill[2], *s_pair_app[2], *s_pair_txt, *s_pair_note, *s_pair_qr;

static void info_screen(void);

bool wallet_info_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void wallet_info_close(void) { close_cb(NULL); }

static void swap_screen(void)           // replace the current section screen
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

// ---- "?" explainers: dim overlay + card, fades in and settles (opa/translate
// only — transform_scale hangs LVGL) ----
static void help_ok_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

enum { DIAG_NONE = 0, DIAG_FP, DIAG_PAIR };   // optional chip diagram under the body

static void help_open_d(const char *title, const char *body, int diagram)
{
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, WT_BG, 0);
    lv_obj_set_style_bg_opa(ovl, 245, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);          // swallow stray taps
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ovl, help_ok_cb, LV_EVENT_CLICKED, ovl);  // tap anywhere = ok

    lv_obj_t *t = wt_lbl(ovl, title, 0, 0, wt_font28(), wt_accent());
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);

    // body sizes itself: short copy reads big, a long translation stays inside
    // the card. Width-capped + wrapping, so the hard newlines written for the
    // small font can never run off the edge at the big one.
    // body runs from y=160 to the diagram (y=320) or to the OK pill (y=392)
    int bw = 720, bh = diagram == DIAG_NONE ? 225 : 155;
    lv_obj_t *b = wt_lbl(ovl, body, 0, 0, wt_body_font(body, bw, bh), WT_MUT);
    lv_obj_set_width(b, bw);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 160);

    if (diagram == DIAG_FP)   wt_diagram_fp(ovl, 330);
    if (diagram == DIAG_PAIR) wt_diagram_pair(ovl, 330);

    wt_pill(ovl, tr(STR_C_OK), 300, 392, 200, help_ok_cb, ovl);
    wt_card_intro(ovl);                       // staggered fade + rise (shared kit)
}

static void help_open(const char *title, const char *body)
{
    help_open_d(title, body, DIAG_NONE);
}

static void help_cb(lv_event_t *e)
{
    const char *key = (const char *)lv_event_get_user_data(e);
    if (!strcmp(key, "fp"))
        help_open_d(tr(STR_D_FINGERPRINT), tr(STR_I_H_FP_B), DIAG_FP);
    else if (!strcmp(key, "type"))
        help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B));
    else if (!strcmp(key, "pair"))
        help_open_d(tr(STR_I_H_PAIR_T), tr(STR_I_H_PAIR_B), DIAG_PAIR);
    else
        help_open(tr(STR_I_SEC_FIRST), tr(STR_I_H_ADDR_B));
}

#ifdef SIMULATOR
void wallet_info_sim_open_type_help(void)
{
    if (s_scr) help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B));
}
#endif

static lv_obj_t *mk_help_chip(int x, int y, const char *key)
{
    lv_obj_t *hc = lv_obj_create(s_scr);
    lv_obj_remove_style_all(hc);
    lv_obj_set_size(hc, 30, 30);
    lv_obj_set_pos(hc, x, y);
    lv_obj_set_style_radius(hc, 15, 0);
    lv_obj_set_style_bg_color(hc, WT_KEY, 0);
    lv_obj_set_style_bg_opa(hc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hc, 1, 0);
    lv_obj_set_style_border_color(hc, WT_MUT, 0);
    lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hc, 12);
    lv_obj_add_event_cb(hc, help_cb, LV_EVENT_CLICKED, (void *)key);
    lv_obj_t *hl = lv_label_create(hc);
    lv_label_set_text(hl, "?");
    lv_obj_set_style_text_color(hl, WT_MUT, 0);
    lv_obj_set_style_text_font(hl, wt_font14(), 0);
    lv_obj_center(hl);
    return hc;
}

// ---- PAIR COORDINATOR ----
static void pair_refresh(void)
{
    char txt[256];
    int rc = s_pair_fmt ? wallet_session_bw_export(txt, sizeof txt)
                        : wallet_session_descriptor(txt, sizeof txt);
    if (rc != 0)
        snprintf(txt, sizeof txt, "%s", tr(STR_C_SESSION_LOCKED));
    if (s_pair_qr)
        lv_qrcode_update(s_pair_qr, txt, (uint32_t)strlen(txt));
    lv_label_set_text(s_pair_txt, txt);
    wt_note_fit(s_pair_note, s_pair_fmt ? tr(STR_I_NOTE_BW) : tr(STR_I_NOTE_SPARROW),
                360, 86);
    for (int i = 0; i < 2; i++) {
        bool on = (s_pair_fmt == i);
        wt_pill_select(s_pair_pill[i], on);
        lv_obj_set_style_text_color(s_pair_app[i],   // app name under the category
                                    on ? wt_accent() : lv_color_hex(0x525C6E), 0);
    }
}

static void pair_fmt_cb(lv_event_t *e)
{
    s_pair_fmt = (int)(intptr_t)lv_event_get_user_data(e);
    pair_refresh();
}

static void pair_back_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    info_screen();
}

static void sp_key_warn_cb(lv_event_t *e);   // scan-key export, warning first

static void pair_screen(void)
{
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_I_PAIR_T),
                      tr(STR_I_PAIR_S));
    wt_qr_card(s_scr, &s_pair_qr, 48, 96, 300, 264);

    // where does the coordinator live? two parallel choices, side by side like
    // the Settings ADDRESS TYPE picker (a dropdown would hide one of only two)
    wt_section(s_scr, tr(STR_I_SHOW_TO), 400, 96);
    mk_help_chip(526, 90, "pair");
    const char *CAT[2] = {tr(STR_I_DESKTOP), tr(STR_I_MOBILE)};
    const char *APP[2] = {tr(STR_I_APP_DESKTOP), tr(STR_I_APP_MOBILE)};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *p = wt_pillh(s_scr, CAT[i], 400 + i * 185, 120, 175, 60,
                               pair_fmt_cb, (void *)(intptr_t)i);
        wt_pill_two_line(p, APP[i]);
        s_pair_app[i] = lv_obj_get_child(p, 1);   // pair_refresh() recolors it
        s_pair_pill[i] = p;
    }

    s_pair_txt = wt_lbl(s_scr, "", 400, 200, wt_font14(), WT_INK);
    lv_obj_set_width(s_pair_txt, 360);
    lv_label_set_long_mode(s_pair_txt, LV_LABEL_LONG_WRAP);

    s_pair_note = wt_note(s_scr, "", 400, 312, 360, 86);   // to the note at 404

    // pairing ends with proof, not hope: point at the address check, then at a
    // tiny dress rehearsal before real money rides on it
    lv_obj_t *pv = wt_note(s_scr, tr(STR_I_PROVE), 400, 404, 360, 72);
    lv_obj_set_style_text_color(pv, WT_INK, 0);

    wt_pill(s_scr, tr(STR_C_BACK), 48, 404, 140, pair_back_cb, NULL);
    // The silent-payment SCAN KEY is a coordinator export too, but it is a
    // PRIVATE key, unlike the xpub/zpub above: kept a separate, warned action so
    // it never reads as just another thing you hand out. Two-line like the
    // category pills above: "SCAN KEY" alone doesn't say WHICH key, and the
    // full phrase won't fit one line in the longer languages.
    lv_obj_t *skp = wt_pillh(s_scr, tr(STR_R_SP_SCAN_BTN), 200, 400, 190, 60,
                             sp_key_warn_cb, NULL);
    wt_pill_two_line(skp, tr(STR_S_SP_BADGE));
    pair_refresh();
}

// ---- silent-payment SCAN KEY export (BIP-392 sp(spscan...)), warning first ----
// Hands a coordinator the scan PRIVATE key so it can DETECT payments to this
// wallet's silent-payment address. It can never spend. Deliberate two-step
// behind an honest warning, not bundled silently into a wallet import.
static void sp_key_back_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    pair_screen();
}

static void sp_key_show_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_R_SP_SCAN_BTN), tr(STR_R_SP_EXPORT_S));
    lv_obj_t *qr = NULL;
    wt_qr_card(s_scr, &qr, 48, 96, 300, 264);

    char key[256];
    if (wallet_session_sp_scan_export(key, sizeof key) != 0)
        snprintf(key, sizeof key, "%s", tr(STR_C_SESSION_LOCKED));
    if (qr)
        lv_qrcode_update(qr, key, (uint32_t)strlen(key));

    // machine-import string: wrapped whole, not grouped like an address
    lv_obj_t *k = wt_lbl(s_scr, key, 400, 100, wt_font14(), WT_INK);
    lv_obj_set_width(k, 360);
    lv_label_set_long_mode(k, LV_LABEL_LONG_WRAP);

    lv_obj_t *note = wt_lbl(s_scr, tr(STR_R_SP_EXPORT_NOTE), 400, 250, wt_font14(), WT_MUT);
    lv_obj_set_width(note, 360);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    wt_pill(s_scr, tr(STR_C_DONE), 48, 404, 160, sp_key_back_cb, NULL);
}

static void sp_key_warn_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_R_SP_SCAN_BTN), tr(STR_R_SP_WARN_S));
    lv_obj_t *wb = wt_lbl(s_scr, tr(STR_R_SP_WARN_B), 48, 108,
                          wt_body_font(tr(STR_R_SP_WARN_B), 700, 280), WT_MUT);
    lv_obj_set_width(wb, 700);
    lv_label_set_long_mode(wb, LV_LABEL_LONG_WRAP);
    lv_obj_t *sp = wt_pill(s_scr, tr(STR_R_SP_SHOW), 48, 404, 300, sp_key_show_cb, NULL);
    wt_pill_primary(sp);
    wt_pill(s_scr, tr(STR_C_BACK), 610, 404, 140, sp_key_back_cb, NULL);
}

// ---- BACKUP WORDS (warning first, then the grid) ----
static void words_back_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    info_screen();
}

static void words_show_cb(lv_event_t *e)
{
    (void)e;
    char words[WSEED_MAX_MNEMONIC];
    if (wallet_seed_load(words, sizeof words) != 0)
        return;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_I_WORDS_BTN),
                      tr(STR_I_WORDS_S));
    // count + split, grid like the setup reveal (2 cols for 12, 4 for 24)
    int n = 1;
    for (const char *p = words; *p; p++) if (*p == ' ') n++;
    int cols = n > 12 ? 4 : 2;
    int rows = (n + cols - 1) / cols;
    const char *p = words;
    for (int i = 0; i < n && *p; i++) {
        char w[12], buf[32];   // BIP39 words are <= 8 chars; sized like the wizard
        int wl = 0;
        while (p[wl] && p[wl] != ' ' && wl < 11) wl++;
        memcpy(w, p, (size_t)wl); w[wl] = 0;
        p += wl; while (*p == ' ') p++;
        snprintf(buf, sizeof buf, "%2d. %s", i + 1, w);
        int c = i / rows, r = i % rows;
        wt_lbl(s_scr, buf, 48 + c * (cols == 4 ? 184 : 300), 108 + r * (cols == 4 ? 42 : 28),
               wt_font14(), WT_INK);
    }
    memset(words, 0, sizeof words);
    wt_pill(s_scr, tr(STR_C_DONE), 610, 404, 140, words_back_cb, NULL);
}

// VERIFY MY COPY: hand off to the setup module's paper-check flow, then reopen
// this section when it returns.
static void winfo_after_verify(void) { info_screen(); }

static void verify_copy_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();                       // drop this screen (async: safe mid-event)
    wallet_setup_open_verify(s_parent, winfo_after_verify);
}

static void words_warn_screen(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_I_WORDS_BTN), tr(STR_I_WARN_S));
    lv_obj_t *b = wt_lbl(s_scr, tr(STR_I_WARN_B), 48, 116,
                         wt_body_font(tr(STR_I_WARN_B), 700, 270), WT_MUT);
    lv_obj_set_width(b, 700);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_t *sp = wt_pill(s_scr, tr(STR_I_SHOW_WORDS), 48, 404, 240, words_show_cb, NULL);
    wt_pill_primary(sp);
    wt_pill(s_scr, tr(STR_I_VERIFY_COPY), 300, 404, 240, verify_copy_cb, NULL);
    wt_pill(s_scr, tr(STR_C_BACK), 610, 404, 140, words_back_cb, NULL);
}

// ---- the section home: facts + actions ----
static void pair_open_cb(lv_event_t *e)  { (void)e; pair_screen(); }

static void info_screen(void)
{
    s_pair_qr = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_T), tr(STR_I_S));

    // facts, left column
    uint8_t fp[4];
    char buf[128], grouped[120];
    wallet_ui_last_fp(fp);

    // help chips sit AFTER the section text: titles vary wildly in width
    // across 19 languages, a fixed x overlaps the longer ones (pt, ru)
    // Caption small, VALUE big. These four values are the whole point of the
    // screen -- the fingerprint you check, the network you are on, the address
    // you read out loud -- so they get the size, and their labels stay eyebrows.
    lv_obj_t *sec = wt_section(s_scr, tr(STR_D_FINGERPRINT), 48, 96);
    lv_obj_update_layout(sec);
    mk_help_chip(48 + lv_obj_get_width(sec) + 12, 90, "fp");
    snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    lv_obj_t *f = wt_lbl(s_scr, buf, 48, 118, wt_font28(), WT_INK);
    lv_obj_set_style_text_letter_space(f, 2, 0);

    wt_section(s_scr, tr(STR_I_SEC_NET), 48, 168);
    wt_lbl(s_scr, wallet_testnet() ? tr(STR_I_NET_TEST) : tr(STR_I_NET_MAIN),
           48, 188, wt_font23(), wallet_testnet() ? WT_WARN : WT_INK);

    sec = wt_section(s_scr, tr(STR_I_SEC_TYPE), 48, 226);
    lv_obj_update_layout(sec);
    mk_help_chip(48 + lv_obj_get_width(sec) + 12, 220, "type");
    int sc = wallet_script();
    int purpose = sc == WSCRIPT_LEGACY ? 44 : sc == WSCRIPT_NESTED ? 49 : 84;
    wt_lbl(s_scr, sc == WSCRIPT_LEGACY ? "Legacy (1...)"
               : sc == WSCRIPT_NESTED ? "Nested SegWit (3...)"
                                      : "Native SegWit (bc1...)",
           48, 246, wt_font23(), WT_INK);
    snprintf(buf, sizeof buf, "m/%d'/%d'/0'", purpose, wallet_testnet() ? 1 : 0);
    wt_lbl(s_scr, buf, 48, 276, wt_font23(), WT_MUT);

    sec = wt_section(s_scr, tr(STR_I_SEC_FIRST), 48, 316);
    lv_obj_update_layout(sec);
    mk_help_chip(48 + lv_obj_get_width(sec) + 12, 310, "addr");
    if (wallet_session_address(0, 0, buf, sizeof buf) != 0)
        snprintf(buf, sizeof buf, "%s", tr(STR_C_SESSION_LOCKED));
    wt_group4(buf, grouped, sizeof grouped);
    lv_obj_t *a = wt_lbl(s_scr, grouped, 48, 336, wt_font23(), WT_INK);
    lv_obj_set_width(a, 360);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);

    // actions, right column
    // Gaps here are 6px, not 12: the column has room for exactly three lines of
    // font23 under each pill, and at 12px both notes came out one pixel short
    // and dropped to font14.
    lv_obj_t *pp = wt_pill(s_scr, tr(STR_I_PAIR_T), 430, 100, 340, pair_open_cb, NULL);
    wt_pill_primary(pp);
    wt_note(s_scr, tr(STR_I_PAIR_BTN_NOTE), 430, 158, 340, 89);    // to WORDS at 250

    wt_pill(s_scr, tr(STR_I_WORDS_BTN), 430, 250, 340, words_warn_screen, NULL);
    wt_note(s_scr, tr(STR_I_WORDS_BTN_NOTE), 430, 308, 340, 90);   // to BACK at 404

    wt_pill(s_scr, tr(STR_C_BACK), 610, 404, 140, close_cb, NULL);
}

void wallet_info_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_pair_fmt = 0;
    info_screen();
}
