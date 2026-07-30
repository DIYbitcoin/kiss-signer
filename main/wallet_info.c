// The WALLET tile. Two jobs, one section:
//   facts   — fingerprint / network / address type / first address, each with a
//             small "?" chip that opens a plain-words explainer (new users learn,
//             experienced users ignore).
//   pair    — the coordinator export: descriptor for Sparrow-family apps, key
//             origin + SLIP-132 zpub for BlueWallet (it doesn't read descriptors).
// Settings owns the RECOVERY WORDS entry; its warning/reveal implementation
// remains in this module. Words stay paper-only: no seed-as-QR export.
// Compiled in BOTH device and sim builds; sim stubs the crypto seams.
#include "wallet_info.h"

#include <stdint.h>   // intptr_t: page step smuggled through the callback's user data
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
static void (*s_words_done)(void);
static int s_pair_fmt;                  // 0 = descriptor (Sparrow), 1 = BlueWallet
static lv_obj_t *s_pair_pill[2], *s_pair_app[2], *s_pair_note, *s_pair_qr;

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

enum { DIAG_NONE = 0, DIAG_FP, DIAG_PAIR, DIAG_SCAN };

static void sp_permission_fact(lv_obj_t *parent, const char *icon,
                               int key, lv_color_t icon_color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 560, 38);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, WT_KEY, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, WT_MUT, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Icon and words centre as ONE group, which is why this is a flex row and
    // not two absolute positions. Left aligned, the three rows had their icons
    // pinned at x=16 and their text at x=58 while the words themselves ran to
    // wildly different lengths, so the block read as a ragged list inside three
    // centred pills. The group centres; the pill centres; they agree.
    //
    // The text is content sized with a 482 ceiling rather than a fixed 482 box.
    // Content sized is what lets a short row centre tightly around its own
    // words; the ceiling is what keeps a long translation wrapping inside the
    // pill instead of running out of it. Every locale fits one line today, so
    // the ceiling has never yet had to do anything.
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);

    lv_obj_t *ic = wt_lbl(row, icon, 0, 0, wt_font23(), icon_color);
    lv_obj_set_width(ic, LV_SIZE_CONTENT);
    lv_obj_t *fact = wt_note(row, tr(key), 0, 0, 482, 29);
    lv_obj_set_width(fact, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(fact, 482, 0);
    lv_obj_set_style_text_align(fact, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(fact, WT_INK, 0);
}

// A private scan key is unusual enough that prose alone makes users hunt for
// the actual permission boundary. These three rows are reused on the explainer
// and the consent screen, and remain meaningful in MONO through shape + words.
static void sp_permission_model(lv_obj_t *parent)
{
    // A flex column, so the three rows keep their 6px gap wherever the parent
    // decides to put the block. They used to be placed at y, y+44 and y+88 from
    // a number the caller had to keep in step with how tall the body above them
    // rendered, which is the same fault as the diagram row below.
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    // Three rows, three jobs, and only TWO of them are coloured.
    //
    // The scheme was capability / limit / consequence: accent for what the key
    // does, WT_OK for what it cannot do, WT_WARN for the part you cannot take
    // back. That is a real scale, and in GREEN theme it collapsed, because
    // ACC_HEX[WT_ACC_GREEN] is 0x35D07F and WT_OK is 0x35D07F. Byte identical.
    // The first two rows rendered the same colour, so a reader saw two greens
    // and an amber and reasonably asked what the greens were supposed to mean.
    // ORANGE has the same problem one row down: 0xFF8A3D beside WT_WARN's
    // 0xF2B84B is a distinction nobody is going to make on a lit panel.
    //
    // Status colours are never themed, so the accent is the one that steps
    // aside. FINDS PAYMENTS is not a status at all, it is the plain statement
    // of what the thing does, and it takes WT_INK. What is left is two colours
    // that each mean exactly one thing: green is the boundary that holds, amber
    // is the cost that does not expire.
    //
    // In MONO this changes NOTHING: ACC_HEX[WT_ACC_MONO] is 0xE8EEF7 and WT_INK
    // is 0xE8EEF7, so the shipped look is preserved to the byte. Every pixel
    // that moves here moves in a theme where two rows used to be the same
    // colour and now are not.
    sp_permission_fact(col, LV_SYMBOL_EYE_OPEN,
                       STR_R_SP_FACT_FIND, WT_INK);
    sp_permission_fact(col, WT_ICON_LOCK,
                       STR_R_SP_FACT_NO_SPEND, WT_OK);
    sp_permission_fact(col, LV_SYMBOL_LOOP,
                       STR_R_SP_FACT_FOREVER, WT_WARN);
}

// The three diagrams, as wt_explain_open asides: draw into the box you are
// given, report the height you used. Each of the underlying helpers appends into
// a flex column, so each wrapper supplies one.
static int aside_col(lv_obj_t *par, int x, int y, int w, void (*fill)(lv_obj_t *))
{
    lv_obj_t *col = lv_obj_create(par);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, x, y);
    lv_obj_set_width(col, w);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    fill(col);
    lv_obj_update_layout(col);
    return lv_obj_get_height(col);
}
static int aside_fp(lv_obj_t *p, int x, int y, int w)
{ return aside_col(p, x, y, w, wt_diagram_fp); }
static int aside_pair(lv_obj_t *p, int x, int y, int w)
{ return aside_col(p, x, y, w, wt_diagram_pair); }
static int aside_scan(lv_obj_t *p, int x, int y, int w)
{ return aside_col(p, x, y, w, sp_permission_model); }

static lv_obj_t *help_open_on(lv_obj_t *parent, const char *title,
                              const char *body, int diagram, bool fp_exit_hint)
{
    // The escape-gesture hint belongs to the HOME fingerprint card and nowhere
    // else, so it rides on the end of the body rather than being a fourth kind
    // of block. It is one sentence and it is the last thing to read.
    char with_hint[768];
    if (fp_exit_hint) {
        snprintf(with_hint, sizeof with_hint, "%s\n\n%s", body,
                 tr(STR_H_EXIT_HINT));
        body = with_hint;
    }
    wt_explain_t e = {
        .title  = title,
        .icon   = diagram == DIAG_FP   ? WT_ICON_KEY
                : diagram == DIAG_PAIR ? WT_ICON_QR
                : diagram == DIAG_SCAN ? WT_ICON_SECRET : NULL,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .aside  = diagram == DIAG_FP   ? aside_fp
                : diagram == DIAG_PAIR ? aside_pair
                : diagram == DIAG_SCAN ? aside_scan : NULL,
    };
    return wt_explain_open(parent, &e);
}

static lv_obj_t *help_open_d(const char *title, const char *body, int diagram)
{
    return help_open_on(s_scr, title, body, diagram, false);
}

static void help_open(const char *title, const char *body)
{
    help_open_d(title, body, DIAG_NONE);
}

lv_obj_t *wallet_info_fp_card_open(lv_obj_t *parent, const char *fingerprint)
{
    char title[64];
    if (fingerprint && fingerprint[0])
        snprintf(title, sizeof title, tr(STR_H_FP_CARD_FMT), fingerprint);
    else
        snprintf(title, sizeof title, "%s", tr(STR_D_FINGERPRINT));
    return help_open_on(parent, title, tr(STR_I_H_FP_B), DIAG_FP,
                        fingerprint != NULL);
}

lv_obj_t *wallet_info_help_card_open(lv_obj_t *parent, const char *title,
                                     const char *body)
{
    return help_open_on(parent, title, body, DIAG_NONE, false);
}

static void help_cb(lv_event_t *e)
{
    const char *key = (const char *)lv_event_get_user_data(e);
    if (!strcmp(key, "fp"))
        wallet_info_fp_card_open(s_scr, NULL);
    else if (!strcmp(key, "type"))
        help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B));
    else if (!strcmp(key, "pair"))
        help_open_d(tr(STR_I_H_PAIR_T), tr(STR_I_H_PAIR_B), DIAG_PAIR);
    else if (!strcmp(key, "scan"))
        help_open_d(tr(STR_R_SP_SCAN_BTN), tr(STR_R_SP_WARN_B), DIAG_SCAN);
    else
        help_open(tr(STR_I_SEC_FIRST), tr(STR_I_H_ADDR_B));
}

#ifdef SIMULATOR
void wallet_info_sim_open_type_help(void)
{
    if (s_scr) help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B));
}

void wallet_info_sim_open_fp_help(void)
{
    if (s_scr) wallet_info_fp_card_open(s_scr, NULL);
}
#endif

static lv_obj_t *mk_help_chip(int x, int y, const char *key)
{
    return wt_help_chip(s_scr, x, y, WT_MUT, help_cb, (void *)key);
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
        wt_qr_update(s_pair_qr, txt, (uint32_t)strlen(txt));
    wt_note_fit(s_pair_note, s_pair_fmt ? tr(STR_I_NOTE_BW) : tr(STR_I_NOTE_SPARROW),
                360, 190);
    for (int i = 0; i < 2; i++) {
        bool on = (s_pair_fmt == i);
        wt_pill_select(s_pair_pill[i], on);
        lv_obj_set_style_text_color(s_pair_app[i],   // app name above the category
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

static void pair_screen(void);

static void pair_qr_back_cb(lv_event_t *e)
{
    (void)e;
    pair_screen();
}

static void pair_instructions_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_pair_qr = s_pair_note = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_PAIR_T),
                      s_pair_fmt ? tr(STR_I_APP_MOBILE)
                                 : tr(STR_I_APP_DESKTOP));

    // Page two is intentionally static: first the exact import steps, then
    // the independent address proof. The raw descriptor is already encoded in
    // the QR and no longer crowds out the instructions people must read.
    // A card per step, eyebrow inside it. Two paragraphs of instructions with
    // two small captions above them, all floating on the page, left the reader to
    // work out which caption owned which paragraph. Both bodies keep a three line
    // budget at font23, which is what they had.
    lv_obj_t *c1 = wt_card(s_scr, 36, 96, 716, 162);
    wt_section(c1, tr(STR_I_SHOW_TO), 16, 10);
    lv_obj_t *steps = wt_note(c1,
        s_pair_fmt ? tr(STR_I_NOTE_BW) : tr(STR_I_NOTE_SPARROW),
        16, 34, 688, 116);
    lv_obj_set_style_text_color(steps, WT_INK, 0);

    lv_obj_t *c2 = wt_card(s_scr, 36, 264, 716, 132);
    wt_section(c2, tr(STR_R_VERIFY), 16, 8);
    lv_obj_t *prove = wt_note(c2, tr(STR_I_PROVE), 16, 30, 688, 96);
    lv_obj_set_style_text_color(prove, WT_INK, 0);

    wt_pill(s_scr, tr(STR_C_BACK), 48, WT_ACTION_Y, 140, pair_qr_back_cb, NULL);
    wt_pill(s_scr, tr(STR_C_DONE), 610, WT_ACTION_Y, 140, pair_back_cb, NULL);
}

static void sp_key_warn_cb(lv_event_t *e);   // scan-key export, warning first

static void pair_screen(void)
{
    swap_screen();
    s_pair_qr = s_pair_note = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_PAIR_T),
                      tr(STR_I_PAIR_S));
    if (wallet_testnet()) {
        lv_obj_t *net = wt_lbl(s_scr, "TESTNET", 672, 30, wt_font14(), WT_WARN);
        lv_obj_set_style_bg_color(net, lv_color_hex(0x2A2113), 0);
        lv_obj_set_style_bg_opa(net, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(net, WT_WARN, 0);
        lv_obj_set_style_border_width(net, 1, 0);
        lv_obj_set_style_radius(net, 10, 0);
        lv_obj_set_style_pad_hor(net, 8, 0);
        lv_obj_set_style_pad_ver(net, 3, 0);
        lv_obj_set_style_text_letter_space(net, 1, 0);
    }
    wt_qr_card(s_scr, &s_pair_qr, 48, 96, 300, 264);

    // where does the coordinator live? two parallel choices, side by side like
    // the Settings ADDRESS TYPE picker (a dropdown would hide one of only two)
    // The chip goes after the caption's MEASURED width, the way every other
    // one on this screen does. Pinned at 526 it assumed the caption was 126px,
    // which "GÖSTERİLECEK YER" is not.
    lv_obj_t *show_sec = wt_section(s_scr, tr(STR_I_SHOW_TO), 400, 96);
    lv_obj_update_layout(show_sec);
    mk_help_chip(400 + lv_obj_get_width(show_sec) + 12, 90, "pair");
    const char *CAT[2] = {tr(STR_I_DESKTOP), tr(STR_I_MOBILE)};
    const char *APP[2] = {tr(STR_I_APP_DESKTOP), tr(STR_I_APP_MOBILE)};
    for (int i = 0; i < 2; i++) {
        // The app is the decision, so it owns the readable 23px line; the
        // desktop/mobile category is the small eyebrow underneath.
        lv_obj_t *p = wt_pillh(s_scr, APP[i], 400 + i * 185, 120, 175, 60,
                               pair_fmt_cb, (void *)(intptr_t)i);
        wt_pill_two_line(p, CAT[i]);
        s_pair_app[i] = lv_obj_get_child(p, 0);   // pair_refresh() recolors it
        s_pair_pill[i] = p;
    }

    // The QR is primary on page one; the selected app's import directions are
    // readable here and repeated with the proof step on the static NEXT page.
    s_pair_note = wt_note(s_scr, "", 400, 204, 360, 190);

    // This BACK escapes pairing altogether, so it takes the corner and NEXT
    // moves to the left. The pairing QR page one step further in keeps ITS back
    // on the left, because that one only steps back to this page: same word,
    // different job, and WT_BACK_X says which job earns the corner.
    wt_pill(s_scr, tr(STR_R_NEXT), 48, WT_ACTION_Y, 140,
            pair_instructions_cb, NULL);
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, pair_back_cb, NULL);
    // The silent-payment SCAN KEY used to live HERE, buried one tap inside PAIR
    // COORDINATOR. It is its own export with its own consent warning, and
    // hiding it behind the descriptor flow implied the two were one action.
    // It is a top-level pill on the WALLET screen now; see info_screen().
    pair_refresh();
}

// ---- silent-payment SCAN KEY export (BIP-392 sp(spscan...)), warning first ----
// Hands a coordinator the scan PRIVATE key so it can DETECT payments to this
// wallet's silent-payment address. It can never spend. Deliberate two-step
// behind an honest warning, not bundled silently into a wallet import.
// Back to the WALLET screen, not the pair screen. SCAN KEY is launched from
// info_screen() now; while it lived inside PAIR COORDINATOR this returned to
// pair_screen(), and leaving it that way would drop the user somewhere they
// never came from -- the kind of navigation bug that reads as the device
// having done something unexpected with a key export.
static void sp_key_back_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    info_screen();
}

static void sp_key_show(void *ud)
{
    (void)ud;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_R_SP_SCAN_BTN), tr(STR_R_SP_EXPORT_S));
    lv_obj_t *qr = NULL;
    wt_qr_card(s_scr, &qr, 48, 96, 300, 264);

    char key[256];
    if (wallet_session_sp_scan_export(key, sizeof key) != 0)
        snprintf(key, sizeof key, "%s", tr(STR_C_SESSION_LOCKED));
    if (qr)
        wt_qr_update(qr, key, (uint32_t)strlen(key));

    // machine-import string: wrapped whole, not grouped like an address
    lv_obj_t *k = wt_lbl(s_scr, key, 400, 100, wt_font14(), WT_INK);
    lv_obj_set_width(k, 360);
    lv_label_set_long_mode(k, LV_LABEL_LONG_WRAP);

    // 250 down to the DONE pill at 404 is 154px, so this reads at 23.
    wt_note(s_scr, tr(STR_R_SP_EXPORT_NOTE), 400, 250, 360, 140);

    wt_pill(s_scr, tr(STR_C_DONE), 48, WT_ACTION_Y, 160, sp_key_back_cb, NULL);
}

static void sp_key_warn_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_R_SP_SCAN_BTN),
                      tr_sym(LV_SYMBOL_WARNING, STR_R_SP_WARN_S));
    // Warning paragraph then the three permission rows, in a flex column: the
    // paragraph is wt_body_font-sized, so the block under it cannot be placed
    // at a y decided in advance. wt_body_font floors at font14 rather than
    // guaranteeing the budget, so "it fits in 145" was an assumption and not a
    // fact even before a translation was involved.
    lv_obj_t *col = lv_obj_create(s_scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 48, 104);
    lv_obj_set_size(col, 704, WT_CONTENT_BOTTOM - 104);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wb = wt_lbl(col, tr(STR_R_SP_WARN_B), 0, 0,
                          wt_body_font(tr(STR_R_SP_WARN_B), 700, 145), WT_MUT);
    lv_obj_set_width(wb, 700);
    lv_label_set_long_mode(wb, LV_LABEL_LONG_WRAP);
    sp_permission_model(col);
    // Revealing a reusable private scan key should not be one stray tap away.
    // A short hold is deliberate without adding the friction of signing.
    wt_hold_pill(s_scr, tr(STR_R_SP_SHOW), 48, WT_ACTION_Y_TALL, 330, WT_ACTION_H_TALL,
                 900, sp_key_show, NULL);
    wt_pillh(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y_TALL, 140, WT_ACTION_H_TALL,
             sp_key_back_cb, NULL);
}

// ---- BACKUP WORDS (warning first, then the grid) ----
static void words_finish(void)
{
    swap_screen();
    void (*done)(void) = s_words_done;
    s_words_done = NULL;
    if (done) done();
    else info_screen();
}

static void words_back_cb(lv_event_t *e)
{
    (void)e;
    words_finish();
}

// These twelve words ARE the wallet. They were rendered at font14 in a grid
// that stopped at y=248 with 150px of empty screen below it -- the smallest
// type on the device on the one screen where a misread character costs the
// coins. They are now font28, and a 24-word seed pages rather than shrinking:
// 12 per page, 2 columns of 6 at a 46px pitch from y=108, so the last row
// bottoms at 375 and clears the action row at 404.
//
// The page counter is deliberately digits-only ("13-24 / 24"). A translated
// "WORDS 13 TO 24 OF 24" would have to reach 21 locale tables to ship, and the
// numerals carry the whole meaning on their own.
#define WORDS_PER_PAGE 12

static int s_words_page;

static void words_render_page(int page);

static void words_page_cb(lv_event_t *e)
{
    words_render_page(s_words_page + (int)(intptr_t)lv_event_get_user_data(e));
}

static void words_render_page(int page)
{
    // Re-read per page and wipe on the way out: paging must not leave a
    // decrypted mnemonic parked in a static between screens.
    char words[WSEED_MAX_MNEMONIC];
    if (wallet_seed_load(words, sizeof words) != 0)
        return;
    int n = 1;
    for (const char *p = words; *p; p++) if (*p == ' ') n++;
    // A BIP39 mnemonic is 24 words at most. Clamping says so out loud: it keeps
    // a malformed store from inventing pages, and it is what lets the counter
    // below fit a fixed buffer (gcc's format-truncation check assumes INT_MAX
    // otherwise, and -Werror stops the device build).
    if (n > WSEED_MAX_WORDS) n = WSEED_MAX_WORDS;

    const int pages = (n + WORDS_PER_PAGE - 1) / WORDS_PER_PAGE;
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    s_words_page = page;

    const int first = page * WORDS_PER_PAGE;
    int on = n - first;
    if (on > WORDS_PER_PAGE) on = WORDS_PER_PAGE;
    const int rows = (on + 1) / 2;          // fill column one, then column two

    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_I_WORDS_BTN), tr(STR_I_WORDS_S));

    // One card per column. This is the screen a holder photographs with their
    // eyes and copies onto paper one line at a time, and twelve numbered words
    // floating on the page gave them nothing to keep their place against. The
    // cards are sized from `rows`, not from a constant: the last page of a 24
    // word mnemonic has fewer rows than the first.
    const int WROW = 46;
    const int card_h = rows * WROW + 12;
    lv_obj_t *col[2] = { wt_card(s_scr, 48, 96, 344, card_h), NULL };
    if (on > rows) col[1] = wt_card(s_scr, 408, 96, 344, card_h);

    const char *p = words;
    for (int i = 0; i < n && *p; i++) {
        char w[12], buf[32];   // BIP39 words are <= 8 chars; sized like the wizard
        int wl = 0;
        while (p[wl] && p[wl] != ' ' && wl < 11) wl++;
        memcpy(w, p, (size_t)wl); w[wl] = 0;
        p += wl; while (*p == ' ') p++;
        if (i < first || i >= first + on) continue;
        snprintf(buf, sizeof buf, "%2d. %s", i + 1, w);
        const int k = i - first, c = k / rows, r = k % rows;
        if (!col[c]) continue;                 // cannot happen: c is 0 or 1
        wt_lbl(col[c], buf, 14, 12 + r * WROW, wt_font28(), WT_INK);
    }
    memset(words, 0, sizeof words);

    if (pages > 1) {
        char cnt[40];
        snprintf(cnt, sizeof cnt, "%d-%d / %d", first + 1, first + on, n);
        if (page > 0)
            wt_pill(s_scr, tr(STR_C_BACK), 48, WT_ACTION_Y, 140, words_page_cb,
                    (void *)(intptr_t)-1);
        // STR_R_NEXT ("NEXT") is the receive flow's page-forward label. Same
        // word, already translated in all 21 locales; borrowing it beats
        // adding a string that would have to reach every table to ship.
        if (page < pages - 1)
            wt_pill(s_scr, tr(STR_R_NEXT), 208, WT_ACTION_Y, 140, words_page_cb,
                    (void *)(intptr_t)1);
        wt_lbl(s_scr, cnt, 380, 416, wt_font23(), WT_MUT);
    }
    wt_pill(s_scr, tr(STR_C_DONE), 610, WT_ACTION_Y, 140, words_back_cb, NULL);
}

static void words_show_cb(lv_event_t *e)
{
    (void)e;
    words_render_page(0);
}

// VERIFY MY COPY: hand off to the setup module's paper-check flow, then return
// to the screen that launched RECOVERY WORDS (normally Settings).
static void winfo_after_verify(void) { words_finish(); }

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
    lv_obj_t *sp = wt_pill(s_scr, tr(STR_I_SHOW_WORDS), 48, WT_ACTION_Y, 240, words_show_cb, NULL);
    wt_pill_primary(sp);
    wt_pill(s_scr, tr(STR_I_VERIFY_COPY), 300, WT_ACTION_Y, 240, verify_copy_cb, NULL);
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, words_back_cb, NULL);
}

// ---- the section home: facts + actions ----
static void pair_open_cb(lv_event_t *e)  { (void)e; pair_screen(); }

static void info_screen(void)
{
    s_pair_qr = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_T), NULL);

    // This is a two-column screen. Its subtitle belongs to the facts column,
    // so it is measured and wrapped inside that column instead of being given
    // the generic 704px subtitle lane that crosses into the actions column.
    // 366 wide, not 340: this lane is bounded by PAIR COORDINATOR at x=430,
    // not by the left column's 340, and the extra 26px is the difference
    // between this setting on one line and breaking after "network,".
    // 40, not 58. The lane was budgeted for two lines at font23 and this English
    // setting takes one; the second line's worth is what pays for the facts card
    // below it having any padding at all. A translation that needs two lines
    // still gets them, at font14, and still clears the card's top edge.
    wt_note(s_scr, tr(STR_I_S), 48, 62, 366, 40);

    // facts, left column, in ONE card.
    //
    // Four captions with four values under them, floating on the page, is the
    // flat list the settings screen stopped being: small type on WT_BG has no
    // edge to be read against. Four separate cards do not fit -- these values
    // are a mono28 fingerprint and a two line address, and the padding of four
    // boxes costs more vertical room than the column has -- so the column IS the
    // card, with the four facts stacked inside it. Same idiom as the Sign
    // screen's footer strip: one panel, several facts, hairlines between them.
    //
    // Offsets below are relative to the card, which starts at FACTS_Y.
    // 278 is measured, not chosen: 10 of top padding, then the four caption and
    // value pairs at the same 9px rhythm the flat column already used, the last
    // of them a two line address, and 12 of bottom padding. 108 + 278 = 386,
    // which clears WT_CONTENT_BOTTOM by 12. There are no hairlines between the
    // facts because there is no room for them; the rhythm does that job.
    lv_obj_t *facts = wt_card(s_scr, 48, 108, 366, 278);
    const int FX = 14, FW = 366 - 28;   // inner left edge and usable width
    uint8_t fp[4];
    char buf[128], grouped[120];
    wallet_ui_last_fp(fp);

    // help chips sit AFTER the section text: titles vary wildly in width
    // across 19 languages, a fixed x overlaps the longer ones (pt, ru)
    // Caption small, VALUE big. These four values are the whole point of the
    // screen -- the fingerprint you check, the network you are on, the address
    // you read out loud -- so they get the size, and their labels stay eyebrows.
    //
    // The rhythm is 7px of air above a section eyebrow and 2px between a
    // caption and the value it names, all the way down, because the last item
    // is a grouped address that takes TWO lines at font23 and the column has to
    // end by WT_CONTENT_BOTTOM. It used to start that address at y=360, which
    // put its second line under the action row where the bar now covers it: the
    // owner was reading half an address and had no way to know it.
    lv_obj_t *sec = wt_section(facts, tr(STR_D_FINGERPRINT), FX, 10);
    lv_obj_update_layout(sec);
    wt_help_chip(facts, FX + lv_obj_get_width(sec) + 12, 4, WT_MUT, help_cb,
                 (void *)"fp");
    snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    // Fixed pitch. The letter_space 2 is gone with it: that was hand kerning
    // a proportional face into looking tabular, which the mono face is by
    // construction. Same value, same face, same width as the Sign header and
    // the unlock screen now print it.
    wt_lbl(facts, buf, FX, 29, wt_font_mono28(), WT_INK);

    wt_section(facts, tr(STR_I_SEC_NET), FX, 72);
    wt_lbl(facts, wallet_testnet() ? tr(STR_I_NET_TEST) : tr(STR_I_NET_MAIN),
           FX, 91, wt_font23(), wallet_testnet() ? WT_WARN : WT_INK);

    sec = wt_section(facts, tr(STR_I_SEC_TYPE), FX, 129);
    lv_obj_update_layout(sec);
    wt_help_chip(facts, FX + lv_obj_get_width(sec) + 12, 123, WT_MUT, help_cb,
                 (void *)"type");
    // Type and path on ONE line, which is what let the path reach font23.
    //
    // The path was the only value on this column at font14, and there was no
    // room to grow it: 126..396 against a 398 floor, and the 11px it needed
    // were 11px the two line address below did not have. The line above it was
    // "Native SegWit (bc1...)", and the address immediately below it starts
    // bc1. So the parenthesis was spending a whole 29px line restating the
    // value four rows down. Dropping it merges two lines into one and pays for
    // the bigger path with 18px to spare.
    //
    // h, not an apostrophe, and this is a correctness fix rather than a style
    // one. At font14 the apostrophes in m/84'/0'/0' render as tick marks a few
    // pixels tall, and on the device panel the line reads as m/84/0/0. Those
    // are DIFFERENT PATHS: a coordinator handed the unhardened one derives
    // different keys and finds none of this wallet's addresses. h is
    // unambiguous at any size, and it is what the receive and silent payment
    // screens already print.
    //
    // WT_INK, not WT_MUT, for the reason wt_section learned the hard way:
    // #7A869C on #070A10 survives a monitor and disappears on this panel. A
    // reference you read out character by character cannot be the faintest
    // thing on the screen.
    //
    // A path you read out to a coordinator is not a footnote. The receive
    // screen already prints it at 23 with its own caption; the two screens
    // print the SAME string, and one of them rendering it in the smallest type
    // the device owns invited exactly the doubt the h fix above was about.
    int sc = wallet_script();
    int purpose = sc == WSCRIPT_LEGACY ? 44 : sc == WSCRIPT_NESTED ? 49 : 84;
    snprintf(buf, sizeof buf, "%s   m/%dh/%dh/0h",
             sc == WSCRIPT_LEGACY ? "Legacy"
                 : sc == WSCRIPT_NESTED ? "Nested SegWit" : "Native SegWit",
             purpose, wallet_testnet() ? 1 : 0);
    wt_lbl(facts, buf, FX, 148, wt_font23(), WT_INK);

    sec = wt_section(facts, tr(STR_I_SEC_FIRST), FX, 186);
    lv_obj_update_layout(sec);
    wt_help_chip(facts, FX + lv_obj_get_width(sec) + 12, 180, WT_MUT, help_cb,
                 (void *)"addr");
    if (wallet_session_address(0, 0, buf, sizeof buf) != 0)
        snprintf(buf, sizeof buf, "%s", tr(STR_C_SESSION_LOCKED));
    wt_group4(buf, grouped, sizeof grouped);
    lv_obj_t *a = wt_lbl(facts, grouped, FX, 208, wt_font23(), WT_INK);
    lv_obj_set_width(a, FW);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);

    // One normal-size action. Pair used to call wt_pill_primary(), which
    // promoted its label to 28pt and made it shout over every fact on screen.
    // Selected styling keeps the visual priority while the text stays on the
    // same 23pt rung as BACK.
    lv_obj_t *pp = wt_pill_icon(s_scr, WT_ICON_KEY, tr(STR_I_PAIR_T), 430, 96,
                                340, 60, pair_open_cb, NULL);
    wt_pill_select(pp, true);
    wt_note(s_scr, tr(STR_I_PAIR_BTN_NOTE), 430, 162, 340, 62);

    // The silent-payment SCAN KEY is a top-level export, not a footnote of the
    // pairing flow it used to hide inside. It hands a coordinator the scan
    // PRIVATE key -- the one export on this device that lets someone else watch
    // every payment you receive -- so it deserves its own pill and keeps its own
    // consent warning (sp_key_warn_cb), which is still the only way to reach the
    // key itself.
    // 72 tall at y=228, not 60 at 236, because the badge under the label moved
    // up to a readable 23. "silent payment" is the one thing on this screen
    // that says WHICH kind of address this key belongs to, and it was rendering
    // in the smallest type the device owns. The taller row needs 12px and the
    // main label needs its own 29px line, so the pill grows and starts 8px
    // higher; the note below gives back the difference and still clears the
    // three lines at 23 it was widened for.
    lv_obj_t *skp = wt_pill_icon(s_scr, WT_ICON_SECRET, tr(STR_R_SP_SCAN_BTN),
                                 430, 228, 340, 72, sp_key_warn_cb, NULL);
    wt_pill_two_line_val(skp, tr(STR_S_SP_BADGE));
    // "Scan" elsewhere means the camera. Here it means searching the chain.
    // Explain that distinction without putting the private key one tap closer.
    mk_help_chip(728, 249, "scan");
    // 94, not 62: this sentence needs three lines at 23 and was silently
    // dropping to font14 beside a PAIR COORDINATOR note at 23 -- the smaller
    // type on the export that gives away the scan key. Nothing sits between
    // here and BACK at y=404, so the rows were free the whole time.
    wt_note(s_scr, tr(STR_R_SP_EXPORT_NOTE), 430, 306, 340, 90);

    // Both actions on this screen are the same size, chosen once for the pair
    // rather than per label: PAIR COORDINATOR is short and would otherwise sit
    // a rung above the export beside it.
    // Measured WITH the icons, because that is what actually gets drawn.
    {
        char pt[WT_ICON_TEXT_MAX], st[WT_ICON_TEXT_MAX];
        wt_icon_text(pt, sizeof pt, WT_ICON_KEY, tr(STR_I_PAIR_T));
        wt_icon_text(st, sizeof st, WT_ICON_SECRET, tr(STR_R_SP_SCAN_BTN));
        const char *lbls[2] = { pt, st };
        wt_pill_fit_t f = wt_pill_group_fit(lbls, 2, 340, 60, false);
        wt_pill_apply_fit(pp, f, 340);
    }
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb, NULL);
}

void wallet_info_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = NULL;
    s_pair_fmt = 0;
    info_screen();
}

void wallet_info_open_words(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = done_cb;
    words_warn_screen(NULL);
}
