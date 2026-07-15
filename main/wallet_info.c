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

    lv_obj_t *t = wt_lbl(ovl, title, 0, 0, &lv_font_montserrat_28, wt_accent());
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t *b = wt_lbl(ovl, body, 0, 0, &lv_font_montserrat_14, WT_MUT);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 160);

    if (diagram == DIAG_FP)   wt_diagram_fp(ovl, 320);
    if (diagram == DIAG_PAIR) wt_diagram_pair(ovl, 320);

    wt_pill(ovl, "OK", 300, 392, 200, help_ok_cb, ovl);
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
        help_open_d("FINGERPRINT",
            "a short code that identifies this wallet without\n"
            "revealing anything about it.\n\n"
            "you saw it at login: same fingerprint = same wallet,\n"
            "same coins. a paired app shows it too, so you can\n"
            "check you both mean the same wallet.", DIAG_FP);
    else if (!strcmp(key, "type"))
        help_open("ADDRESS TYPE",
            "the style of address this wallet hands out. Native\n"
            "SegWit (bc1...) is the modern kind with the lowest fees.\n\n"
            "the m/... line is the derivation path: the standard\n"
            "shelf inside the seed where these keys live. apps use\n"
            "it to find the same addresses this device does.");
    else if (!strcmp(key, "pair"))
        help_open_d("THE COORDINATOR APP",
            "an online app that watches this wallet using only a\n"
            "public key, so it can never spend. it sees balances,\n"
            "builds transactions and broadcasts them.\n\n"
            "DESKTOP shares a descriptor (Sparrow-style), MOBILE a\n"
            "zpub (BlueWallet-style) - same wallet, two dialects.\n"
            "'watch-only' is correct: every spend signs HERE.", DIAG_PAIR);
    else
        help_open("FIRST ADDRESS",
            "address #0, shown so you can recognize this wallet\n"
            "at a glance. a paired app should show exactly this\n"
            "same address first.\n\n"
            "addresses are safe to share: they can receive coins,\n"
            "they can never spend them.");
}

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
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0);
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
        snprintf(txt, sizeof txt, "SESSION LOCKED");
    if (s_pair_qr)
        lv_qrcode_update(s_pair_qr, txt, (uint32_t)strlen(txt));
    lv_label_set_text(s_pair_txt, txt);
    lv_label_set_text(s_pair_note, s_pair_fmt
        ? "phone apps that read a key (zpub): BlueWallet\n"
          "(add wallet > import > scan), Nunchuk, Ibis...\n"
          "the app may say 'watch-only' - correct: it only\n"
          "watches + broadcasts. every send signs HERE."
        : "computer apps that read a descriptor: Sparrow\n"
          "(File > New Wallet > Airgapped Hardware Wallet),\n"
          "Specter, Nunchuk, Fully Noded...");
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

static void pair_screen(void)
{
    swap_screen();
    s_scr = wt_screen(s_parent, "PAIR COORDINATOR",
                      "show this QR to the app that will watch this wallet");
    wt_qr_card(s_scr, &s_pair_qr, 48, 96, 300, 264);

    // where does the coordinator live? two parallel choices, side by side like
    // the Settings ADDRESS TYPE picker (a dropdown would hide one of only two)
    wt_section(s_scr, "SHOW IT TO", 400, 96);
    mk_help_chip(526, 90, "pair");
    static const char *CAT[2] = {"DESKTOP", "MOBILE"};
    static const char *APP[2] = {"Sparrow + more", "BlueWallet + more"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *p = wt_pillh(s_scr, CAT[i], 400 + i * 185, 120, 175, 60,
                               pair_fmt_cb, (void *)(intptr_t)i);
        lv_obj_align(lv_obj_get_child(p, 0), LV_ALIGN_TOP_MID, 0, 9);
        s_pair_app[i] = lv_label_create(p);
        lv_label_set_text(s_pair_app[i], APP[i]);
        lv_obj_set_style_text_font(s_pair_app[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_pair_app[i], LV_ALIGN_BOTTOM_MID, 0, -8);
        s_pair_pill[i] = p;
    }

    s_pair_txt = wt_lbl(s_scr, "", 400, 200, &lv_font_montserrat_14, WT_INK);
    lv_obj_set_width(s_pair_txt, 360);
    lv_label_set_long_mode(s_pair_txt, LV_LABEL_LONG_WRAP);

    s_pair_note = wt_lbl(s_scr, "", 400, 316, &lv_font_montserrat_14, WT_MUT);

    // pairing ends with proof, not hope: point at the address check, then at a
    // tiny dress rehearsal before real money rides on it
    wt_lbl(s_scr, "then prove it: RECEIVE > VERIFY - scan the\n"
                  "app's first address to confirm it's yours.\n"
                  "first time? practice with a tiny send first.",
           400, 404, &lv_font_montserrat_14, WT_INK);

    wt_pill(s_scr, "BACK", 48, 404, 140, pair_back_cb, NULL);
    pair_refresh();
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
    s_scr = wt_screen(s_parent, "BACKUP WORDS",
                      "copy them onto paper, in order. never a photo, never a file.");
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
               &lv_font_montserrat_14, WT_INK);
    }
    memset(words, 0, sizeof words);
    wt_pill(s_scr, "DONE", 610, 404, 140, words_back_cb, NULL);
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
    s_scr = wt_screen(s_parent, "BACKUP WORDS", "the words that rebuild this wallet");
    lv_obj_t *b = wt_lbl(s_scr,
        "make sure nobody can see the screen.\n\n"
        "these words + your passphrase open THIS wallet.\n"
        "the words alone open a different, empty-looking\n"
        "wallet - that's the design, so a forced look\n"
        "reveals nothing.\n\n"
        "never type them into a phone or computer.\n"
        "never photograph them. paper only.\n\n"
        "they are a STANDARD backup: lose this device and\n"
        "words + passphrase restore your coins on any other\n"
        "signer - or, in a true emergency, a phone/desktop\n"
        "wallet. after an emergency like that, sweep the\n"
        "coins to a fresh offline wallet and back IT up.",
        48, 116, &lv_font_montserrat_14, WT_MUT);
    (void)b;
    lv_obj_t *sp = wt_pill(s_scr, "SHOW THE WORDS", 48, 404, 240, words_show_cb, NULL);
    wt_pill_primary(sp);
    wt_pill(s_scr, "VERIFY MY COPY", 300, 404, 240, verify_copy_cb, NULL);
    wt_pill(s_scr, "BACK", 610, 404, 140, words_back_cb, NULL);
}

// ---- the section home: facts + actions ----
static void pair_open_cb(lv_event_t *e)  { (void)e; pair_screen(); }

static void info_screen(void)
{
    s_pair_qr = NULL;
    s_scr = wt_screen(s_parent, "WALLET", "the wallet you are logged into right now");

    // facts, left column
    uint8_t fp[4];
    char buf[128], grouped[120];
    wallet_ui_last_fp(fp);

    wt_section(s_scr, "FINGERPRINT", 48, 100);
    mk_help_chip(196, 94, "fp");
    snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    lv_obj_t *f = wt_lbl(s_scr, buf, 48, 124, &lv_font_montserrat_28, WT_INK);
    lv_obj_set_style_text_letter_space(f, 2, 0);

    wt_section(s_scr, "NETWORK", 48, 182);
    wt_lbl(s_scr, wallet_testnet() ? "TESTNET, practice coins" : "MAINNET, real bitcoin",
           48, 206, &lv_font_montserrat_14, wallet_testnet() ? WT_WARN : WT_INK);

    wt_section(s_scr, "ADDRESS TYPE", 48, 244);
    mk_help_chip(210, 238, "type");
    int sc = wallet_script();
    int purpose = sc == WSCRIPT_LEGACY ? 44 : sc == WSCRIPT_NESTED ? 49 : 84;
    wt_lbl(s_scr, sc == WSCRIPT_LEGACY ? "Legacy (1...)"
               : sc == WSCRIPT_NESTED ? "Nested SegWit (3...)"
                                      : "Native SegWit (bc1...)",
           48, 268, &lv_font_montserrat_14, WT_INK);
    snprintf(buf, sizeof buf, "m/%d'/%d'/0'", purpose, wallet_testnet() ? 1 : 0);
    wt_lbl(s_scr, buf, 48, 290, &lv_font_montserrat_14, WT_MUT);

    wt_section(s_scr, "FIRST ADDRESS", 48, 328);
    mk_help_chip(216, 322, "addr");
    if (wallet_session_address(0, 0, buf, sizeof buf) != 0)
        snprintf(buf, sizeof buf, "SESSION LOCKED");
    wt_group4(buf, grouped, sizeof grouped);
    lv_obj_t *a = wt_lbl(s_scr, grouped, 48, 352, &lv_font_montserrat_14, WT_INK);
    lv_obj_set_width(a, 340);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);

    // actions, right column
    lv_obj_t *pp = wt_pill(s_scr, "PAIR COORDINATOR", 430, 104, 320, pair_open_cb, NULL);
    wt_pill_primary(pp);
    lv_obj_t *pn = wt_wrap(s_scr, 430, 166, 340);
    lv_label_set_text(pn, "connect the app that watches your balance\n"
                          "and prepares transactions. it can never\n"
                          "spend - only this device signs.");

    wt_pill(s_scr, "BACKUP WORDS", 430, 252, 320, words_warn_screen, NULL);
    lv_obj_t *wn = wt_wrap(s_scr, 430, 314, 340);
    lv_label_set_text(wn, "see the words that rebuild this wallet,\n"
                          "or prove your written copy is correct.");

    wt_pill(s_scr, "BACK", 610, 404, 140, close_cb, NULL);
}

void wallet_info_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_pair_fmt = 0;
    info_screen();
}
