// The WALLET tile. Two jobs, one section:
//   facts   — fingerprint / network / address type / first address, each with a
//             small "?" chip that opens a plain-words explainer (new users learn,
//             experienced users ignore).
//   pair    — the coordinator export: descriptor for Sparrow-family apps, key
//             origin + SLIP-132 zpub for BlueWallet (it doesn't read descriptors).
// Settings owns the RECOVERY WORDS entry; its warning/reveal implementation
// remains in this module. Words stay paper-only as PLAINTEXT: the one
// sanctioned export is the KEF encrypted backup below, which leaves the box
// only under a password.
// Compiled in BOTH device and sim builds; sim stubs the crypto seams.
#include "kiss_info.h"

#include <stdint.h>   // intptr_t: page step smuggled through the callback's user data
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "kiss_backup.h"  // kiss_backup_mark: the paper check, made durable
#include "kiss_crypto.h"
#include "kiss_kef.h"     // the encrypted backup envelope
#include "kiss_seed.h"
#include "kiss_setup.h"   // kiss_setup_open_verify: check the paper backup
#include "kiss_theme.h"
#include "kiss_wipe.h"
#include "kiss_rehearse.h"
#include "kiss_ui.h"   // kiss_ui_last_fp; the borrowed KEF password keyboard
#include "platform_sd.h"

static lv_obj_t *s_scr;                 // whichever wallet-section screen is up
static lv_obj_t *s_parent;
static void (*s_words_done)(void);
static int s_pair_fmt;                  // 0 = descriptor (Sparrow), 1 = BlueWallet
static lv_obj_t *s_pair_pill[2], *s_pair_app[2], *s_pair_note, *s_pair_qr;

static void info_screen(void);
static void kef_warn_screen(lv_event_t *e);
static void kef_wipe(void);

bool kiss_info_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e)
{
    (void)e;
    kef_wipe();       // the idle close must never leave an envelope behind
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void kiss_info_close(void) { close_cb(NULL); }

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

lv_obj_t *kiss_info_fp_card_open(lv_obj_t *parent, const char *fingerprint,
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

lv_obj_t *kiss_info_help_card_open(lv_obj_t *parent, const char *title,
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
    kiss_ui_last_fp(fp);
    snprintf(fpbuf, sizeof fpbuf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    kiss_info_fp_card_open(s_scr, fpbuf, false);
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
void kiss_info_sim_open_type_help(void)
{
    if (s_scr) help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B),
                         LV_SYMBOL_DIRECTORY, TYPE_ICONS);
}

void kiss_info_sim_open_fp_help(void)
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
    int rc = s_pair_fmt ? kiss_session_bw_export(txt, sizeof txt)
                        : kiss_session_descriptor(txt, sizeof txt);
    // Refused = no QR at all. The failure string used to go through
    // wt_qr_update, so a coordinator was offered a scannable code whose
    // content was the words "SESSION LOCKED" -- an import that fails somewhere
    // over there instead of being refused here. See wt_qr_refusal.
    wt_qr_refusal(s_pair_qr, rc != 0);
    if (rc != 0) {
        wt_note_fit(s_pair_note, tr(STR_C_LOCKED_B), 360, 190);   // see the scan key refusal
        return;
    }
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
    wt_pill(s_scr, tr(STR_C_DONE), WT_BACK_X, WT_ACTION_Y, 140, pair_back_cb, NULL);
}

static void sp_key_warn_cb(lv_event_t *e);   // scan-key export, warning first

static void pair_screen(void)
{
    swap_screen();
    s_pair_qr = s_pair_note = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_PAIR_T),
                      tr(STR_I_PAIR_S));
    if (kiss_testnet()) {
        lv_obj_t *net = wt_lbl(s_scr, kiss_net_name(), 672, 30, wt_font14(), WT_WARN);
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
    // now always belongs to the way out, and the step-forward takes the left.
    // Neither pairing bar holds a pager PAIR -- page 1 has only NEXT and page 2
    // only its page-back -- so the adjacency exemption has nothing to protect
    // here, and both pages agree on where the exit is.
    wt_pill(s_scr, tr(STR_R_NEXT), WT_ACT_X, WT_ACTION_Y, 140,
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

    // The refusal is its OWN render, decided before anything is drawn. This
    // used to fall through the success path with the failure string in the
    // key's place -- so the screen showed a scannable QR that ENCODED the
    // words "SESSION LOCKED", those words in the machine-import slot, and the
    // usual coordinator note under them. A coordinator scanning what looks
    // like a finished export would import garbage. A failed export has no
    // key, so it draws no QR and nothing that resembles one.
    //
    // First rendered by the walk's failure stop; every frame before that was
    // the success path, which is how the fall-through survived.
    char key[256];
    if (kiss_session_sp_scan_export(key, sizeof key) != 0) {
        lv_obj_t *card = wt_card(s_scr, 48, 128, 704, 140);
        lv_obj_t *ic = wt_lbl(card, WT_ICON_LOCK, 0, 0, wt_font34(), WT_WARN);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 28, 0);
        lv_obj_t *chip = wt_state_chip(card, tr(STR_C_SESSION_LOCKED), WT_WARN);
        lv_obj_align(chip, LV_ALIGN_LEFT_MID, 92, 0);
        // Not L_FAIL_OPEN_B: nothing went WRONG. The lock is the device
        // doing its job, and a refusal note that reads like a fault teaches an
        // owner to fear a feature. Reassurance first, then the mechanism, then
        // the way back.
        wt_note(s_scr, tr(STR_C_LOCKED_B), 48, 296, 704, 90);
        wt_pill(s_scr, tr(STR_C_DONE), 592, WT_ACTION_Y, 160, sp_key_back_cb, NULL);
        return;
    }

    lv_obj_t *qr = NULL;
    wt_qr_card(s_scr, &qr, 48, 96, 300, 264);
    if (qr)
        wt_qr_update(qr, key, (uint32_t)strlen(key));

    // machine-import string: wrapped whole, not grouped like an address
    lv_obj_t *k = wt_lbl(s_scr, key, 400, 100, wt_font14(), WT_INK);
    lv_obj_set_width(k, 360);
    lv_label_set_long_mode(k, LV_LABEL_LONG_WRAP);

    // 250 down to the DONE pill at 404 is 154px, so this reads at 23.
    wt_note(s_scr, tr(STR_R_SP_EXPORT_NOTE), 400, 250, 360, 140);

    // 592, not WT_BACK_X: 160 wide, so 752-160 is flush.
    wt_pill(s_scr, tr(STR_C_DONE), 592, WT_ACTION_Y, 160, sp_key_back_cb, NULL);
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
    wt_pillh(s_scr, tr(STR_C_BACK), WT_EXIT_X, WT_ACTION_Y_TALL, 140, WT_ACTION_H_TALL,
             sp_key_back_cb, NULL);
    wt_hold_pill(s_scr, tr(STR_R_SP_SHOW), WT_ACT_X, WT_ACTION_Y_TALL, 330, WT_ACTION_H_TALL,
                 900, sp_key_show, NULL);
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
    if (kiss_seed_load(words, sizeof words) != 0)
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
    // 42, not 46. The four pixels a row gives back are what open the band under
    // the cards, and the screen needed it: twelve words in two boxes and
    // nothing else never said WHICH keys they are, on the one screen where
    // that is the whole question. A holder with two signers, or a passphrase
    // and a decoy, had no way to tell one word list from another.
    const int WROW = 42;
    const int card_h = rows * WROW + 12;
    lv_obj_t *col[2] = { wt_card(s_scr, 48, 96, 344, card_h), NULL };
    if (on > rows) col[1] = wt_card(s_scr, 408, 96, 344, card_h);

    // The same verdict WRITE THESE DOWN carries. These words came off a stored
    // seed, so their checksum holds by construction -- saying so is what stops
    // a holder wondering whether a word they cannot read is a word gone wrong.
    lv_obj_t *okc = wt_state_chip(s_scr, tr(STR_W_WRITE_OK), WT_OK);
    lv_obj_update_layout(okc);
    lv_obj_set_pos(okc, 752 - lv_obj_get_width(okc), 30);

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
    kiss_wipe(words, sizeof words);

    // Which keys these words open, under the list, in the band the tighter rows
    // paid for. Guarded the same way every other fingerprint on the device is:
    // zero is "no keys open", never a code to copy down.
    {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        if (kiss_fp_known(fp)) {
            char b[48];
            snprintf(b, sizeof b, "%s  %02X%02X%02X%02X", tr(STR_L_FP_CAP),
                     fp[0], fp[1], fp[2], fp[3]);
            lv_obj_t *l = wt_lbl(s_scr, b, 48, 96 + card_h + 10, wt_font23(), WT_MUT);
            lv_obj_set_width(l, 704);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        }
    }

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
    wt_pill(s_scr, tr(STR_C_DONE), WT_BACK_X, WT_ACTION_Y, 140, words_back_cb, NULL);
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
    // Only when the words ARE the whole backup. This marked the session
    // fingerprint the moment the typed words matched the stored seed, and on
    // keys opened with a passphrase those are two different claims: the words
    // are half of what restores them, and the fingerprint being marked is the
    // device saying the whole paper backup has been proven, which nothing on
    // this route ever checked.
    //
    // The setup rehearsal gets this right and has since kiss_rehearse existed:
    // after the words match it throws the session passphrase away and requires
    // it fresh, then compares fingerprints (kiss_ui.c, setup_warn_words_done).
    // That leg does not exist here, so the same seam decides the same way --
    // no passphrase, nothing left to prove, mark it; a passphrase, and the
    // chip stays as it was rather than being turned green by half a check.
    //
    // Nothing is taken away by this: kiss_backup_mark only ever sets, and keys
    // opened with a passphrase earned their mark on the rehearsal at setup,
    // where the passphrase actually was checked.
    if (kiss_setup_verify_succeeded() &&
        kiss_rehearse_after_words(kiss_session_decoy()) == KISS_REHEARSE_VERIFIED) {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        kiss_backup_mark(fp);
    }
    words_finish();
}

static void verify_copy_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();                       // drop this screen (async: safe mid-event)
    kiss_setup_open_verify(s_parent, winfo_after_verify);
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
    bool ok = kiss_ui_backup_checked();
    lv_obj_t *chip = wt_state_chip(s_scr,
                                   tr_sym(ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                                          ok ? STR_L_BACKUP_VERIFIED
                                             : STR_L_BACKUP_UNVERIFIED),
                                   ok ? WT_OK : WT_WARN);
    // Measure it rather than budget for it: the chip is self sizing and its
    // height follows the locale's font, so everything laid against it uses
    // the real box.
    lv_obj_update_layout(chip);
    int chip_w = lv_obj_get_width(chip);
    // The chip SHARES the 96 line with the encrypted-backup row: stacking
    // them cost the body below a font rung (a 64px row plus its gap is
    // exactly the difference between a 226px and a 174px body budget), and
    // the two are one subject read left to right — the paper's state, then
    // the other backup. The chip centres on the row's 64px band.
    lv_obj_set_pos(chip, 48,
                   96 + (WT_ROW_H - lv_obj_get_height(chip)) / 2);
    int rx = 48 + chip_w + 12;
    // The OTHER backup: the same keys, leaving locked. It lives on this page
    // because this IS the backup page — and because Settings' right column is
    // full (words row, NO UNDO, the theme card; measured, not assumed). A
    // long locale's chip narrows the row; the row ellipsises by design.
    wt_row(s_scr, tr(STR_I_ROW_KEF),
           tr_sym(WT_ICON_LOCK, STR_I_ROW_KEF_SUB), NULL, WT_INK,
           rx, 96, 752 - rx, kef_warn_screen, NULL);
    int below = 96 + WT_ROW_H;

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
    int note = kiss_seed_entropy_note();
    if (note != 0) {
        lv_obj_t *ent = wt_state_chip(s_scr,
                                      tr_sym(LV_SYMBOL_WARNING,
                                             WSEED_ENTQ_IS_CARDS(note)
                                                 ? STR_W_CARDS_WARN_T
                                                 : STR_W_DICE_WARN_T),
                                      WT_WARN);
        lv_obj_update_layout(ent);
        // The 96 line belongs to the chip + row pair now, so this verdict
        // always stacks under them rather than measuring for a free slot.
        lv_obj_set_pos(ent, 48, below + 8);
        below += 8 + lv_obj_get_height(ent);
    }

    wt_why_body(s_scr, tr(STR_I_WARN_B), below + 12, WT_WARN, true);

    // The exit takes the corner; the two actions run left to right from 48.
    // SHOW WORDS puts the live mnemonic on the glass with a plain tap, which is
    // the reason it does not get the corner. The order they are READ in is
    // unchanged; only where the row sits is.
    lv_obj_t *sp = wt_pill(s_scr, tr(STR_I_SHOW_WORDS), 310, WT_ACTION_Y, 240, words_show_cb, NULL);
    wt_pill_primary(sp);
    // The unchecked chip names the gap; this is the button that closes it, so
    // it wears the same amber until it has been used (as the setup warning
    // screen's VERIFY FULL BACKUP does).
    lv_obj_t *vp = wt_pill(s_scr, tr(STR_I_VERIFY_COPY), WT_ACT_X, WT_ACTION_Y, 240,
                           verify_copy_cb, NULL);
    if (!ok) lv_obj_set_style_border_color(vp, WT_WARN, 0);
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, words_back_cb, NULL);
}

// ---- ENCRYPTED BACKUP (KEF): consent -> password -> locked QR / SD ----
// The one sanctioned way the keys leave this box, and they leave locked: a
// KEF envelope under a password the owner chooses on the next screen. The
// envelope is built fresh from the stored words on demand, lives in this one
// buffer, and is wiped on every exit and on the idle close.
static uint8_t   s_kef_env[KEF_MAX_ENV];
static size_t    s_kef_env_len;
static char      s_kef_id[9];
static lv_obj_t *s_kef_sd_chip;

static void kef_wipe(void)
{
    kiss_wipe(s_kef_env, sizeof s_kef_env);
    s_kef_env_len = 0;
    memset(s_kef_id, 0, sizeof s_kef_id);
    s_kef_sd_chip = NULL;
}

static void kef_finish_cb(lv_event_t *e)
{
    (void)e;
    kef_wipe();
    words_warn_screen(NULL);            // back to the backup page it lives on
}

// Runs on the keyboard's OK with the confirmed password. -1 keeps the
// keyboard up with its one vague failure; nothing here says why.
static int kef_check_cb(const char *pass, size_t len)
{
    char words[WSEED_MAX_MNEMONIC];
    if (kiss_seed_load(words, sizeof words) != 0) return -1;
    int rc = kiss_kef_seal_seed(words, pass, len, s_kef_env, sizeof s_kef_env,
                                &s_kef_env_len, s_kef_id);
    kiss_wipe(words, sizeof words);
    if (rc != 0) kef_wipe();
    return rc;
}

static void kef_sd_cb(lv_event_t *e)
{
    (void)e;
    if (!s_kef_env_len) return;
    char name[32];
    snprintf(name, sizeof name, "%s.kef", s_kef_id);
    int rc = platform_sd_mount() == 0
                 ? platform_sd_write_atomic(name, s_kef_env, s_kef_env_len)
                 : -1;
    platform_sd_unmount();
    bool ok = rc == 0 || rc == PLATFORM_SD_ATOMIC_CLEANUP;
    // The verdict appears where the eye already is, under the fingerprint:
    // the card committed (atomic write, so committed means verified), or the
    // card refused and nothing was kept.
    if (!s_kef_sd_chip) {
        s_kef_sd_chip = wt_state_chip(s_scr, "", WT_OK);
        lv_obj_set_pos(s_kef_sd_chip, 400, 344);
    }
    wt_state_chip_set(s_kef_sd_chip,
                      tr_sym(ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                             ok ? STR_S_SAVED_NOTE : STR_S_FAIL_SD_WRITE),
                      ok ? WT_OK : WT_STOP);
}

static void kef_show_screen(void)
{
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_KEF), tr(STR_I_KEF_SHOW_S));
    s_kef_sd_chip = NULL;

    lv_obj_t *qr = NULL;
    wt_qr_card(s_scr, &qr, 48, 96, 300, 264);
    if (qr) wt_qr_update(qr, s_kef_env, (uint32_t)s_kef_env_len);

    // The fingerprint is the envelope's visible name: it says WHICH keys are
    // inside without opening it, and it is what the .kef file is called.
    lv_obj_t *card = wt_value_card(s_scr, tr(STR_D_FINGERPRINT), s_kef_id,
                                   400, 96, 352, true);
    lv_obj_update_layout(card);
    int below = 96 + lv_obj_get_height(card) + 12;
    wt_note(s_scr, tr(STR_I_KEF_SHOW_NOTE), 400, below, 352, 332 - below);

    wt_pill_icon(s_scr, WT_ICON_SD, tr(STR_I_KEF_SD_BTN), WT_ACT_X,
                 WT_ACTION_Y, 330, WT_ACTION_H, kef_sd_cb, NULL);
    lv_obj_t *dp = wt_pill(s_scr, tr(STR_C_DONE), WT_EXIT_X, WT_ACTION_Y, 140,
                           kef_finish_cb, NULL);
    wt_pill_primary(dp);
}

static void kef_warn_reopen(void) { kef_warn_screen(NULL); }

static void kef_make(void *ud)
{
    (void)ud;
    kiss_ui_kef_pass_open(true, kef_check_cb, kef_show_screen,
                          kef_warn_reopen);
}

static void kef_warn_screen(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_KEF), tr(STR_I_KEF_WARN_S));

    // The mechanism, drawn before it is explained: your keys plus one
    // password become a QR that only the password opens.
    lv_obj_t *card = wt_card(s_scr, 48, 96, 704, 64);
    lv_obj_t *row = wt_diagram_row(card);
    wt_chip(row, tr_sym(WT_ICON_KEY, STR_D_KEYS), true);
    wt_diagram_op(row, "+");
    wt_chip(row, tr_sym(WT_ICON_LOCK, STR_L_KEF_PASS_OPEN), true);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, tr_sym(WT_ICON_QR, STR_I_KEF_CHIP_QR), false);
    lv_obj_center(row);

    // Two claims, split: the accent rule on what the format buys, WT_WARN on
    // the one way it goes wrong. The password never has a reset.
    {
        const char *h1 = tr(STR_I_KEF_W1_H), *b1 = tr(STR_I_KEF_W1_B);
        const char *h2 = tr(STR_I_KEF_W2_H), *b2 = tr(STR_I_KEF_W2_B);
        const int BW = 344, BY = 176, BH = WT_CONTENT_BOTTOM - BY;
        const lv_font_t *f = wt_body_font2_head(h1, b1, h2, b2, BW - 14, BH);
        wt_why_block(s_scr, h1, b1,  48, BY, BW, BH, f, wt_accent());
        wt_why_block(s_scr, h2, b2, 408, BY, BW, BH, f, WT_WARN);
    }

    // Making the envelope puts the keys on the glass as a QR one screen
    // later, so the entry is a deliberate hold, the scan-key precedent.
    wt_hold_pill(s_scr, tr(STR_I_KEF_MAKE_BTN), WT_ACT_X, WT_ACTION_Y, 330,
                 WT_ACTION_H, 900, kef_make, NULL);
    wt_pill(s_scr, tr(STR_C_BACK), WT_EXIT_X, WT_ACTION_Y, 140,
            kef_finish_cb, NULL);
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
    char buf[128];
    uint8_t fp[4];
    kiss_ui_last_fp(fp);

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
           kiss_testnet() ? tr(STR_G_TESTNET_NOTE) : tr(STR_G_MAINNET_NOTE),
           kiss_net_name(),
           kiss_testnet() ? WT_WARN : WT_INK,
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
    int sc = kiss_script();
    int purpose = sc == WSCRIPT_LEGACY ? 44 : sc == WSCRIPT_NESTED ? 49 : 84;
    snprintf(buf, sizeof buf, "%s   m/%dh/%dh/0h",
             tr(sc == WSCRIPT_LEGACY ? STR_S_TY_LEGACY
                : sc == WSCRIPT_NESTED ? STR_S_TY_NESTED : STR_S_TY_NATIVE),
             purpose, kiss_testnet() ? 1 : 0);
    wt_row_f(s_scr, tr(STR_I_SEC_TYPE), buf, wt_font_mono14(), NULL, NULL,
             WT_INK, WT_LIST_L_X, WT_LIST_Y(2), WT_LIST_W,
             row_help_cb, (void *)"type");

    // The address on its own line at mono23, with the last EIGHT lit -- the
    // same eight every receive and verify screen marks. It was a mono14
    // sub-line with nothing lit, and both halves of that were wrong: an
    // address is compare material, read character by character against a
    // coordinator's screen, and font14 on it was a layout budget thrown away.
    // This is the last row of its column, so it grows to 76 (bottom 384,
    // above the 396 floor) instead of shrinking the one string on this page
    // an owner actually has to READ.
    {
        lv_obj_t *arow = wt_row_x(s_scr, NULL, tr(STR_I_SEC_FIRST), NULL, NULL,
                                  NULL, NULL, WT_INK, false, WT_LIST_L_X,
                                  WT_LIST_Y(3), WT_LIST_W, 76,
                                  row_help_cb, (void *)"addr");
        // With no sub the row centres its label; this row builds its second
        // line below, so the label takes the top lane every two-line row uses.
        // The label is the row's last child: no icon, no value and no sub
        // means only the chevron is built before it.
        lv_obj_set_y(lv_obj_get_child(arow, -1), 7);

        size_t n = kiss_session_address(0, 0, buf, sizeof buf) == 0
                       ? strlen(buf) : 0;
        if (n < 20) {
            // The state, as words. Never through the address fold: an ellipsis
            // and a lit tail would turn LOCKED into an address-shaped fragment,
            // and a state must read as a state. font23, because it is the one
            // thing on the row an owner is being told -- not metadata.
            lv_obj_t *st = wt_lbl(arow, tr(STR_C_SESSION_LOCKED), 14, 43,
                                  wt_font23(), WT_MUT);
            lv_obj_set_width(st, 300);   // stops short of the chevron's lane
            lv_obj_set_height(st, lv_font_get_line_height(wt_font23()));
            lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
        } else {
            // wt_addr_short's fold, drawn locally: its double-spaced ellipsis
            // is 28 mono cells, and 28 at mono23 (13.8px a cell) is 387px
            // against the ~330 this card has. Same blocks, same last eight
            // lit; only the air around the ellipsis goes. Cutting a BLOCK
            // instead would change which characters the row teaches an owner
            // to check, and shrinking the font is the bug being fixed.
            int pre = !strncmp(buf, "tsp1", 4) ? 5
                    : (!strncmp(buf, "bc1", 3) || !strncmp(buf, "tb1", 3) ||
                       !strncmp(buf, "sp1", 3)) ? 4 : 0;
            const char *t = buf + n - 12;
            // The prefix through a bounded copy, not "%.*s": the device
            // compiler's truncation gate cannot see that pre is at most 5,
            // and a copy into a char[8] is a bound it can prove.
            char pfx[8] = {0};
            if (pre) { memcpy(pfx, buf, (size_t)pre); pfx[pre] = ' '; }
            char head[24], tail[16];
            snprintf(head, sizeof head, "%s%.4s\xE2\x80\xA6%.4s ",
                     pfx, buf + pre, t);
            snprintf(tail, sizeof tail, "%.4s %.4s", t + 4, t + 8);

            lv_obj_t *sg = lv_spangroup_create(arow);
            // A spangroup is clickable out of the box and silently eats every
            // press that lands on it -- inside a tappable row that kills the
            // row exactly where the address is printed.
            lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
            lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);
            lv_obj_set_style_text_font(sg, wt_font_mono23(), 0);
            lv_span_t *s1 = lv_spangroup_new_span(sg);
            lv_span_set_text(s1, head);
            lv_style_set_text_color(lv_span_get_style(s1), WT_MUT);
            lv_span_t *s2 = lv_spangroup_new_span(sg);
            lv_span_set_text(s2, tail);
            // Brightness alone marks the compared run, the same as every
            // other address on the device: no underline, no second hue.
            lv_style_set_text_color(lv_span_get_style(s2), wt_accent());
            lv_spangroup_refresh(sg);
            // Below the chevron's band, so the tail can run past the
            // chevron's x lane without the two boxes sharing a pixel.
            lv_obj_set_pos(sg, 14, 47);
        }
    }

    // ---- right column ----
    // Two exports. They were pills, which said "button" about two things
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
    //
    // ONE card, where a row and an explainer card used to stack. The row's
    // sub-line said "silent payment", the note in the box under it explained
    // the export, and the warn screen the row opened explained it again -- a
    // reader met the same lesson as a sub-line, as a card, and as a warn
    // screen, and only the first of the three took a tap. Worse, the box and
    // the row wore the same fill and border, so nothing on the page said which
    // of two identical panels was the control. Merged, the card IS the
    // destination: the row's label and sub-line sit beside the badge, the note
    // keeps the full width below them, and the whole panel opens the export
    // the way the row did. It runs from the row's old slot down to the 396
    // floor the explainer already stood on, one pixel clear of
    // WT_CONTENT_BOTTOM, so the right column still reaches the bottom the way
    // the four rows on the left do.
    {
        const int card_h = 396 - WT_LIST_Y(1);
        lv_obj_t *why = wt_card(s_scr, WT_LIST_R_X, WT_LIST_Y(1),
                                WT_LIST_W, card_h);
        // The whole card is the control, like the receive screen's address
        // card: the thing being explained is the thing you tap, and a 365x230
        // target needs no aiming. The "?" chip stays its own clickable on top
        // of it and wins the taps that land there.
        lv_obj_add_flag(why, LV_OBJ_FLAG_CLICKABLE);
        wt_tap_feedback(why);
        lv_obj_add_event_cb(why, sp_key_warn_cb, LV_EVENT_CLICKED, NULL);
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
        // A big target nobody knows to press is not an affordance, so the card
        // says it opens the way a row does: a chevron on the title line. Built
        // and measured FIRST, like wt_row builds its own, so the label's box
        // can exclude it -- the overlap gate compares boxes, and a label
        // allowed to span the card would contain the chevron whatever the
        // translation does. It stops 10 short of the chip's box at 323, and
        // sits on the title line rather than at the card's right mid, because
        // the note below owns the full width and a mark dropped into its box
        // would collide with it in every locale at once.
        const int lh23 = lv_font_get_line_height(wt_font23());
        const int lh14 = lv_font_get_line_height(wt_font14());
        lv_obj_t *chev = wt_lbl(why, LV_SYMBOL_RIGHT, 0, 0, wt_font23(),
                                WT_MUT);
        lv_obj_update_layout(chev);
        const int chx = WT_LIST_W - 42 - 10 - lv_obj_get_width(chev);
        lv_obj_set_pos(chev, chx, 12 + (lh14 + 3) / 2);
        // The row's label and sub-line, in the row's own type, in the lane the
        // badge leaves: 14..48 plus the row's gutter. Both pinned to ONE line
        // and stopped short of the chevron for the same reason a row's are --
        // a translation too long to fit ellipsises rather than rearranging
        // the card.
        const int tx = 58;
        lv_obj_t *tl = wt_lbl(why, tr(STR_R_SP_SCAN_BTN), tx, 12,
                              wt_font23(), WT_INK);
        lv_obj_set_width(tl, chx - 10 - tx);
        lv_obj_set_height(tl, lh23);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
        lv_obj_t *sub = wt_lbl(why, tr(STR_S_SP_BADGE), tx, 12 + lh23 + 3,
                               wt_font14(), WT_MUT);
        lv_obj_set_width(sub, chx - 10 - tx);
        lv_obj_set_height(sub, lh14);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        // UNDER the marks and the title band, not beside them: a note threaded
        // between the badge and the chip would be 275 wide and back at font14.
        // Full width and everything left of the card's height is what buys
        // font23, measured from where the sub-line actually ends rather than
        // from a constant, because the line heights differ per font class.
        const int ny = 12 + lh23 + 3 + lh14 + 8;
        wt_note(why, tr(STR_R_SP_EXPORT_NOTE), 14, ny,
                WT_LIST_W - 28, card_h - ny - 12);
    }

    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb, NULL);
}

void kiss_info_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = NULL;
    s_pair_fmt = 0;
    info_screen();
}

void kiss_info_open_words(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = done_cb;
    words_warn_screen(NULL);
}

