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
#include "wallet_backup.h"  // wallet_backup_mark: the paper check, made durable
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
// The code the fingerprint card is currently explaining. A static, because the
// aside callback wt_explain_open takes has no user data and this is the only
// diagram on the device that needs to draw a live value.
static char s_fp_code[16];
static void diagram_fpid(lv_obj_t *col) { wt_diagram_fpid(col, s_fp_code); }

static int aside_fp(lv_obj_t *p, int x, int y, int w)
{ return aside_col(p, x, y, w, diagram_fpid); }
static int aside_pair(lv_obj_t *p, int x, int y, int w)
{ return aside_col(p, x, y, w, wt_diagram_pair); }
static int aside_scan(lv_obj_t *p, int x, int y, int w)
{ return aside_col(p, x, y, w, sp_permission_model); }

// Two entries, and the mark for each. A body written `TERM: definition` per
// line is a LIST, and passing icons here is what says so: wt_explain_open then
// draws badges and headings instead of a grey paragraph the reader has to
// finish before finding the half that applies to them.
static const char *const PAIR_ICONS[] = { LV_SYMBOL_EYE_OPEN, WT_ICON_LOCK };
static const char *const TYPE_ICONS[] = { LV_SYMBOL_OK, LV_SYMBOL_DIRECTORY };

static lv_obj_t *help_open_on(lv_obj_t *parent, const char *title,
                              const char *body, int diagram, bool fp_exit_hint,
                              const char *icon, const char *const *icons)
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
    // The diagram picks the badge when there is one, because the reader met that
    // mark on the control that sent them here. Without a diagram the caller says.
    wt_explain_t e = {
        .title  = title,
        .icon   = diagram == DIAG_FP     ? WT_ICON_KEY
                : diagram == DIAG_PAIR   ? WT_ICON_QR
                : diagram == DIAG_SCAN   ? WT_ICON_SECRET : icon,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .mode   = icons ? WT_GRID_ICONS : WT_BODY_PROSE,
        .icons  = icons,
        .aside  = diagram == DIAG_FP     ? aside_fp
                : diagram == DIAG_PAIR   ? aside_pair
                : diagram == DIAG_SCAN   ? aside_scan : NULL,
    };
    return wt_explain_open(parent, &e);
}

static lv_obj_t *help_open_d(const char *title, const char *body, int diagram,
                             const char *const *icons)
{
    return help_open_on(s_scr, title, body, diagram, false, NULL, icons);
}

static void help_open(const char *title, const char *body, const char *icon,
                      const char *const *icons)
{
    help_open_on(s_scr, title, body, DIAG_NONE, false, icon, icons);
}

lv_obj_t *wallet_info_fp_card_open(lv_obj_t *parent, const char *fingerprint,
                                   bool exit_hint)
{
    char title[64];
    const bool has_code = fingerprint && fingerprint[0];
    snprintf(s_fp_code, sizeof s_fp_code, "%s", has_code ? fingerprint : "");
    if (has_code)
        snprintf(title, sizeof title, tr(STR_H_FP_CARD_FMT), fingerprint);
    else
        snprintf(title, sizeof title, "%s", tr(STR_D_FINGERPRINT));

    return help_open_on(parent, title, tr(STR_I_H_FP_B), DIAG_FP,
                        exit_hint, NULL, NULL);
}

lv_obj_t *wallet_info_help_card_open(lv_obj_t *parent, const char *title,
                                     const char *body, const char *icon)
{
    return help_open_on(parent, title, body, DIAG_NONE, false, icon, NULL);
}

// The fingerprint row's explainer, opened WITH the code so the card draws the
// wallet's picture. This screen is the one place where the rule about never
// showing a picture without its number is met by the PAGE rather than by the
// card: the fingerprint row behind this overlay is already showing the eight
// characters in mono, and that row is what the reader tapped to get here.
static void fp_help_open(void)
{
    uint8_t fp[4];
    char fpbuf[16];
    wallet_ui_last_fp(fp);
    snprintf(fpbuf, sizeof fpbuf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    wallet_info_fp_card_open(s_scr, fpbuf, false);
}

static void help_cb(lv_event_t *e)
{
    const char *key = (const char *)lv_event_get_user_data(e);
    if (!strcmp(key, "fp"))
        fp_help_open();
    // DIRECTORY for the address type, because what it is really about is the
    // derivation branch under the name; DOWNLOAD for the first address, because
    // an address is where money arrives. Both are in the baked symbol set.
    else if (!strcmp(key, "type"))
        help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B), LV_SYMBOL_DIRECTORY,
                  TYPE_ICONS);
    else if (!strcmp(key, "pair"))
        help_open_d(tr(STR_I_H_PAIR_T), tr(STR_I_H_PAIR_B), DIAG_PAIR, PAIR_ICONS);
    else if (!strcmp(key, "scan"))
        help_open_d(tr(STR_R_SP_SCAN_BTN), tr(STR_R_SP_WARN_B), DIAG_SCAN, NULL);
    else
        help_open(tr(STR_I_SEC_FIRST), tr(STR_I_H_ADDR_B), LV_SYMBOL_DOWNLOAD,
                  NULL);
}

#ifdef SIMULATOR
void wallet_info_sim_open_type_help(void)
{
    if (s_scr) help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B),
                         LV_SYMBOL_DIRECTORY, TYPE_ICONS);
}

void wallet_info_sim_open_fp_help(void)
{
    if (s_scr) fp_help_open();   // the row's own path, so the shot matches the device
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
    // Marks before words, and the mark says which device the steps are for: a
    // phone for BlueWallet, a file for Sparrow on a computer. Both are in SYMS.
    lv_obj_t *c1 = wt_card(s_scr, 36, 96, 716, 162);
    wt_section(c1, tr_sym(s_pair_fmt ? WT_ICON_PHONE : LV_SYMBOL_FILE,
                          STR_I_SHOW_TO), 16, 10);
    lv_obj_t *steps = wt_note(c1,
        s_pair_fmt ? tr(STR_I_NOTE_BW) : tr(STR_I_NOTE_SPARROW),
        16, 34, 688, 116);
    lv_obj_set_style_text_color(steps, WT_INK, 0);

    lv_obj_t *c2 = wt_card(s_scr, 36, 264, 716, 132);
    wt_section(c2, tr_sym(LV_SYMBOL_OK, STR_R_VERIFY), 16, 8);
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

    // This BACK used to take the corner on the theory that an escape from the
    // whole flow earns it while a step back to one page does not. That rule was
    // real and written down, but it was one of two rules the product held at
    // once: seven screens put the way out in the corner and seven put the
    // action there, so the corner meant "leave" on one screen and "do it" on
    // the next. A distinction nobody can see is not a distinction. The corner
    // now always belongs to the action, and BACK is always leftmost, whether it
    // steps back one page or drops the flow.
    wt_pill(s_scr, tr(STR_R_NEXT), 612, WT_ACTION_Y, 140,
            pair_instructions_cb, NULL);
    wt_pill(s_scr, tr(STR_C_BACK), 48, WT_ACTION_Y, 140, pair_back_cb, NULL);
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
//
// The result used to be dropped on the floor here, so the only route an owner
// can reach after setup could not turn the backup row green -- it stayed amber
// forever no matter how many times they read their paper into the keypad.
//
// No second passphrase prompt on this route, unlike the setup rehearsal. There
// the session was made by a passphrase invented moments earlier, so it proves
// nothing and has to be retyped. Here the owner is already unlocked: the live
// fingerprint IS the words + passphrase pair, and the check just compared every
// word against the stored mnemonic. Asking again would only teach them that the
// device nags.
static void winfo_after_verify(void)
{
    if (wallet_setup_verify_succeeded()) {
        uint8_t fp[4];
        wallet_ui_last_fp(fp);
        wallet_backup_mark(fp);
    }
    words_finish();
}

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

    // SETTINGS folded its two backup cards into one, so this page inherits the
    // subject: it is where the paper check gets stated and where it gets done.
    // The chip sits on the content line rather than beside the title, so the
    // page title keeps its full 34 -- a 36 character Dutch chip up there would
    // shrink it two rungs.
    //
    // Glyph AND colour, per ADDENDUM-02: GREEN theme's accent is byte identical
    // to WT_OK, so a green chip alone says nothing in that theme.
    bool ok = wallet_ui_backup_checked();
    lv_obj_t *chip = wt_state_chip(s_scr,
                                   tr_sym(ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                                          ok ? STR_L_BACKUP_VERIFIED
                                             : STR_L_BACKUP_UNVERIFIED),
                                   ok ? WT_OK : WT_WARN);
    lv_obj_set_pos(chip, 48, 96);
    // Measure it rather than budget for it: the chip is self sizing and its
    // height follows the locale's font, so the body starts under the real box.
    lv_obj_update_layout(chip);
    int below = 96 + lv_obj_get_height(chip);

    // The dice judge's verdict, carried forward from the seed that was made.
    // It belongs on THIS page and not on the one that showed it first: the
    // warning during setup arrives at the most excited moment of the ritual,
    // and this is the screen someone opens to copy words onto paper, which is
    // the last moment redoing the seed is still cheap.
    //
    // Reuses the string the warning screen already wears, so this costs no
    // new key in 21 locales, and the same mark: glyph AND colour, per
    // ADDENDUM-02, because an amber chip alone says nothing in some themes.
    //
    // Which warning screen depends on which path made the seed. Dice can no
    // longer produce a note at all -- it refuses instead -- so a dice title
    // here only ever comes from a seed made before that changed.
    int note = wallet_seed_entropy_note();
    if (note != 0) {
        lv_obj_t *ent = wt_state_chip(s_scr,
                                      tr_sym(LV_SYMBOL_WARNING,
                                             WSEED_ENTQ_IS_CARDS(note)
                                                 ? STR_W_CARDS_WARN_T
                                                 : STR_W_DICE_WARN_T),
                                      WT_WARN);
        lv_obj_update_layout(ent);
        // Side by side when the locale leaves room, stacked when it does not.
        // A 36 character Dutch backup chip beside this one would run off the
        // page, and measuring is cheaper than guessing which locales do that.
        int x = 48 + lv_obj_get_width(chip) + 12;
        if (x + lv_obj_get_width(ent) <= 752) {
            lv_obj_set_pos(ent, x, 96);
        } else {
            lv_obj_set_pos(ent, 48, below + 8);
            below += 8 + lv_obj_get_height(ent);
        }
    }

    wt_why_body(s_scr, tr(STR_I_WARN_B), below + 12, WT_WARN, true);

    // BACK leftmost, the two actions right aligned to the lane's edge, primary
    // in the corner. The order they are READ in is unchanged; only where the
    // row sits is.
    lv_obj_t *sp = wt_pill(s_scr, tr(STR_I_SHOW_WORDS), 512, WT_ACTION_Y, 240, words_show_cb, NULL);
    wt_pill_primary(sp);
    // The unchecked chip names the gap; this is the button that closes it, so
    // it wears the same amber until it has been used (as the setup warning
    // screen's VERIFY FULL BACKUP does).
    lv_obj_t *vp = wt_pill(s_scr, tr(STR_I_VERIFY_COPY), 250, WT_ACTION_Y, 240,
                           verify_copy_cb, NULL);
    if (!ok) lv_obj_set_style_border_color(vp, WT_WARN, 0);
    wt_pill(s_scr, tr(STR_C_BACK), 48, WT_ACTION_Y, 140, words_back_cb, NULL);
}

// ---- the section home: facts + actions ----
static void pair_open_cb(lv_event_t *e)  { (void)e; pair_screen(); }

// The four fact rows and the two coordinator rows all open something, and a row
// carries its key the way a help chip used to.
static void row_help_cb(lv_event_t *e) { help_cb(e); }

static void info_screen(void)
{
    s_pair_qr = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_T), NULL);

    // ---- the same list Settings is drawn on ----
    // This screen was one 366x278 fact card with four eyebrow-and-value pairs
    // stacked inside it, beside two pills each trailing a loose paragraph. Four
    // different shapes for six things that are all "a label, what it says, and
    // where it takes you". Settings had already solved that, and the owner reads
    // Settings without effort, so this is the same wt_row list on the same
    // geometry rather than a second idiom for the same job. WT_LIST_* lives in
    // the theme now precisely so the two cannot drift apart.
    //
    // No subtitle. "fingerprint, network, addresses" named the three rows
    // directly underneath it, and dropping it is what puts the first eyebrow on
    // the same y=72 line Settings starts on.
    //
    // The help chips are gone with it. A row that opens an explainer opens it
    // when you tap the ROW, which is the gesture this device already teaches on
    // every Settings line, and a 365x64 target needs no aiming at a 20px circle.
    // The labels stay in the eyebrow's upper case: they are the strings the 21
    // locales already carry for these four facts, and inventing sentence case
    // for them would mean English saying something no other language says.
    char buf[128], grouped[120];
    uint8_t fp[4];
    wallet_ui_last_fp(fp);

    wt_row_head(s_scr, tr(STR_I_SEC_THIS_WALLET), WT_LIST_L_X, WT_LIST_TOP,
                WT_LIST_W);

    // Mono, through wt_row_f. This is a code you hold beside a coordinator's
    // screen and compare digit by digit, and the proportional face is the one
    // that makes 0 and O and 8 and B argue.
    snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    wt_row_f(s_scr, tr(STR_D_FINGERPRINT), NULL, NULL, buf, wt_font_mono23(),
             WT_INK, WT_LIST_L_X, WT_LIST_Y(0), WT_LIST_W,
             row_help_cb, (void *)"fp");

    // The one row with nothing to open, so the one row with no chevron. Same
    // pair of strings Settings puts on its own network row.
    wt_row(s_scr, tr(STR_I_SEC_NET),
           wallet_testnet() ? tr(STR_G_TESTNET_NOTE) : tr(STR_G_MAINNET_NOTE),
           wallet_testnet() ? "TESTNET" : "MAINNET",
           wallet_testnet() ? WT_WARN : WT_INK,
           WT_LIST_L_X, WT_LIST_Y(1), WT_LIST_W, NULL, NULL);

    // Type and path BOTH on the sub-line, and no value at all. The path was the
    // value at mono23 first, and "ADDRESS TYPE" beside it ellipsised to
    // "ADDRESS T..." -- a row's label and its value share one line, and these
    // two are each about 170px in a 365 card. The rows in this column split
    // cleanly in two anyway: a short fact goes in the value slot, a long
    // reference goes on the sub-line, and this row and the address under it are
    // both references.
    //
    // h, not an apostrophe, and this is correctness rather than style: at small
    // sizes the apostrophes in m/84'/0'/0' render as tick marks and the line
    // reads as m/84/0/0. Those are DIFFERENT PATHS, and a coordinator handed the
    // unhardened one finds none of this wallet's addresses.
    int sc = wallet_script();
    int purpose = sc == WSCRIPT_LEGACY ? 44 : sc == WSCRIPT_NESTED ? 49 : 84;
    snprintf(buf, sizeof buf, "%s   m/%dh/%dh/0h",
             sc == WSCRIPT_LEGACY ? "Legacy"
                 : sc == WSCRIPT_NESTED ? "Nested SegWit" : "Native SegWit",
             purpose, wallet_testnet() ? 1 : 0);
    wt_row_f(s_scr, tr(STR_I_SEC_TYPE), buf, wt_font_mono14(), NULL, NULL,
             WT_INK, WT_LIST_L_X, WT_LIST_Y(2), WT_LIST_W,
             row_help_cb, (void *)"type");

    // The address goes on the SUB line, in mono, because it is 42 characters and
    // a value slot holds a word. Folded to the head and tail the rest of the
    // device shows: the full form belongs on RECEIVE, which is the screen built
    // for reading one out, and this row's job is to say which wallet you are in.
    if (wallet_session_address(0, 0, buf, sizeof buf) != 0)
        snprintf(buf, sizeof buf, "%s", tr(STR_C_SESSION_LOCKED));
    wt_addr_fold(buf, grouped, sizeof grouped);
    wt_row_f(s_scr, tr(STR_I_SEC_FIRST), grouped, wt_font_mono14(), NULL, NULL,
             WT_INK, WT_LIST_L_X, WT_LIST_Y(3), WT_LIST_W,
             row_help_cb, (void *)"addr");

    // ---- right column ----
    // Two exports, as rows. They were pills, which said "button" about two things
    // that are really destinations: both open a screen and neither does anything
    // by itself. Their notes were loose paragraphs floating beside them; a note
    // that belongs to a control belongs INSIDE it, which is the whole point of a
    // row's sub-line.
    wt_row_head(s_scr, tr(STR_D_ONLINE_APP), WT_LIST_R_X, WT_LIST_TOP,
                WT_LIST_W);
    // STR_I_PAIR_S, not STR_I_PAIR_BTN_NOTE. A row's sub-line is pinned to one
    // line and ellipsised, and the note is "the coordinator wallet that watches.
    // it cannot sign." -- the half that gets cut is the half that matters. This
    // string says what the row DOES, which is what a row's sub-line is for, and
    // "it cannot sign" is still on the pairing screen and in its explainer.
    wt_row(s_scr, tr(STR_I_PAIR_T), tr(STR_I_PAIR_S), NULL, WT_INK,
           WT_LIST_R_X, WT_LIST_Y(0), WT_LIST_W, pair_open_cb, NULL);
    // "Scan" elsewhere on this device means the camera. Here it means searching
    // the chain, and the badge under the label is what says which.
    wt_row(s_scr, tr(STR_R_SP_SCAN_BTN), tr(STR_S_SP_BADGE), NULL, WT_INK,
           WT_LIST_R_X, WT_LIST_Y(1), WT_LIST_W, sp_key_warn_cb, NULL);
    // The export's own warning is three lines about handing someone the key that
    // watches every payment you ever receive, which is more than a sub-line
    // holds. It gets a CARD with the "?" in its top right corner, which is the
    // idiom the scan screen already uses for a paragraph that owns a help
    // affordance. Loose on the page with the chip floating out to the right, it
    // read as a stray control belonging to nothing.
    //
    // 160 tall, not 96. Nothing else lives in this column, so the card ran out
    // at 333 with 65px of empty page under it and its note squeezed into 72 --
    // which is font14, on a paragraph nobody is required to read twice. At 160
    // it bottoms out at 396, one pixel clear of WT_CONTENT_BOTTOM, so the right
    // column reaches the floor the way the four rows on the left do, and the
    // note gets the height to be read at font23.
    {
        lv_obj_t *why = wt_card(s_scr, WT_LIST_R_X, WT_LIST_Y(2),
                                WT_LIST_W, 160);
        // The same badge the explainer this "?" opens wears in ITS top right
        // corner: help_cb's "scan" branch goes through DIAG_SCAN, and DIAG_SCAN
        // picks WT_ICON_SECRET. One mark on the card and on the page behind it
        // is the entire reason wt_explain_open takes an icon at all -- a reader
        // should recognise where they landed before reading a word of it.
        lv_obj_t *badge = lv_obj_create(why);
        lv_obj_remove_style_all(badge);
        lv_obj_set_pos(badge, 14, 12);
        lv_obj_set_size(badge, 34, 34);
        lv_obj_set_style_radius(badge, 17, 0);
        lv_obj_set_style_bg_color(badge, WT_KEY, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_set_style_border_color(badge, WT_EDGE, 0);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(wt_lbl(badge, WT_ICON_SECRET, 0, 0, wt_font23(),
                             wt_accent()));
        wt_help_chip(why, WT_LIST_W - 42, 12, WT_MUT, help_cb, (void *)"scan");
        // UNDER both marks, not beside them: the badge owns 14..48 and the chip
        // 323..353, and a note threaded between them would be 275 wide and back
        // at font14. Full width below the row is what buys the size.
        wt_note(why, tr(STR_R_SP_EXPORT_NOTE), 14, 58, WT_LIST_W - 28, 90);
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
