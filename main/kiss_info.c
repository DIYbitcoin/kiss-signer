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
// Where the SCAN KEY export returns to. NULL means KEYS, which is where it
// has always gone; RECEIVE's silent payment tab sets its own.
static void (*s_scan_done)(void);
static int s_pair_fmt;                  // 0 = descriptor (Sparrow), 1 = BlueWallet
static lv_obj_t *s_pair_pill[2], *s_pair_app[2], *s_pair_note, *s_pair_qr;

static void info_screen(void);
static void kef_warn_screen(lv_event_t *e);
static void kef_wipe(void);

// RECOVERY WORDS is a tabbed page; this is its two-lane group state. Declared
// up here because swap_screen() and the idle close both have to stop its
// motion before the screen goes, and both run above the page that owns it.
static wt_pane_t s_wctx;
// KEYS' own lane, declared beside it for the same reason: close_cb and
// swap_screen both run above the page that owns it and both must stop it.
static wt_pane_t s_ictx;

bool kiss_info_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e)
{
    (void)e;
    kef_wipe();       // the idle close must never leave an envelope behind
    wt_pane_stop(&s_wctx);
    wt_pane_stop(&s_ictx);
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void kiss_info_close(void) { close_cb(NULL); }

static void swap_screen(void)           // replace the current section screen
{
    // Everything moving, stopped, before the screen under it is scheduled to
    // die. The delete is async, so the animation timer would otherwise keep
    // running against a screen already on its way out -- and the idle
    // auto-lock lands in exactly that window.
    wt_pane_stop(&s_wctx);
    wt_pane_stop(&s_ictx);
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

// Three entries, and the mark for each. A body written `TERM: definition` per
// line is a LIST, and passing icons here is what says so: wt_explain_open then
// draws badges and headings instead of a grey paragraph the reader has to
// finish before finding the half that applies to them.
static const char *const PAIR_ICONS[] = {
    LV_SYMBOL_EYE_OPEN,
    WT_ICON_LOCK,
    LV_SYMBOL_GPS,       // the bitcoin is on the network, not in either device
};
static const char *const TYPE_ICONS[] = { LV_SYMBOL_OK, LV_SYMBOL_DIRECTORY };

_Static_assert(sizeof PAIR_ICONS / sizeof PAIR_ICONS[0] == 3,
               "the pairing explainer supplies three semantic badges");

static lv_obj_t *help_open_on(lv_obj_t *parent, const char *title,
                              const char *body, int diagram, bool fp_exit_hint,
                              const char *icon, const char *const *icons,
                              size_t icons_count)
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
        .icons_count = icons_count,
        .aside  = diagram == DIAG_FP     ? aside_fp
                : diagram == DIAG_PAIR   ? aside_pair
                : diagram == DIAG_SCAN   ? aside_scan : NULL,
    };
    return wt_explain_open(parent, &e);
}

static lv_obj_t *help_open_d(const char *title, const char *body, int diagram,
                             const char *const *icons, size_t icons_count)
{
    return help_open_on(s_scr, title, body, diagram, false, NULL, icons,
                        icons_count);
}

static void help_open(const char *title, const char *body, const char *icon,
                      const char *const *icons, size_t icons_count)
{
    help_open_on(s_scr, title, body, DIAG_NONE, false, icon, icons,
                 icons_count);
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
                        exit_hint, NULL, NULL, 0);
}

lv_obj_t *kiss_info_help_card_open(lv_obj_t *parent, const char *title,
                                     const char *body, const char *icon)
{
    return help_open_on(parent, title, body, DIAG_NONE, false, icon, NULL, 0);
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
                  TYPE_ICONS, sizeof TYPE_ICONS / sizeof TYPE_ICONS[0]);
    else if (!strcmp(key, "pair"))
        help_open_d(tr(STR_I_H_PAIR_T), tr(STR_I_H_PAIR_B), DIAG_PAIR,
                    PAIR_ICONS, sizeof PAIR_ICONS / sizeof PAIR_ICONS[0]);
    // "scan" is gone with the card that carried its "?". It opened
    // STR_R_SP_WARN_B, which is the SAME string the warn screen one tap away
    // prints in full above the permission rows -- so the lesson was reachable
    // as a sub-line, as a card and as a warn screen, and only the first of the
    // three cost a tap. The warn screen is the one that also asks for consent,
    // so it is the one that stays.
    else
        help_open(tr(STR_I_SEC_FIRST), tr(STR_I_H_ADDR_B), LV_SYMBOL_DOWNLOAD,
                  NULL, 0);
}

#ifdef SIMULATOR
void kiss_info_sim_open_type_help(void)
{
    if (s_scr) help_open(tr(STR_I_SEC_TYPE), tr(STR_I_H_TYPE_B),
                         LV_SYMBOL_DIRECTORY, TYPE_ICONS,
                         sizeof TYPE_ICONS / sizeof TYPE_ICONS[0]);
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
        // The flag rides with the paint, or the selected app name keeps the
        // OLD accent after a theme change -- this runs on every pick, so the
        // stale colour survives until the pill is tapped again.
        lv_obj_set_style_text_color(s_pair_app[i],   // app name above the category
                                    on ? wt_accent() : lv_color_hex(0x525C6E), 0);
        if (on) lv_obj_add_flag(s_pair_app[i], WT_FLAG_ACCENT);
        else    lv_obj_remove_flag(s_pair_app[i], WT_FLAG_ACCENT);
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

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 48, WT_ACTION_Y, 0, false, pair_qr_back_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, pair_back_cb, NULL);
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
    wt_arrow_action(s_scr, tr(STR_R_NEXT), false, false, WT_ACT_X, WT_ACTION_Y, 0, false, pair_instructions_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, pair_back_cb, NULL);
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
    // Where it came from, which is not always here any more: RECEIVE's silent
    // payment tab offers the same export, and landing that owner on KEYS would
    // be the device having moved them somewhere they never asked to go, in the
    // middle of exporting a key.
    if (s_scan_done) { void (*d)(void) = s_scan_done; s_scan_done = NULL; d(); }
    else info_screen();
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
        wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, sp_key_back_cb, NULL);
        return;
    }

    lv_obj_t *qr = NULL;
    wt_qr_card(s_scr, &qr, 48, 96, 300, 264);
    if (qr)
        wt_qr_update(qr, key, (uint32_t)strlen(key));

    // Machine-import string: still wrapped whole, not grouped like an address,
    // because a descriptor is one token and blocking it invites someone to type
    // the spaces back in. What was wrong was the FACE and the SIZE. This is the
    // only string on the screen an owner may have to read back character by
    // character against a coordinator, and it was set in font14 -- the size
    // this file reserves for metadata -- in the PROPORTIONAL face, where the
    // bech32 charset's l and 1 are one shape and its narrow letters carry no
    // column to count along. Every other machine string on the device is
    // mono23: the fingerprint row and the folded address two screens back.
    //
    // The room was already here. 100 down to the note at 250 is 150px and
    // font14 was using 78 of it, so the fix costs nothing but the empty band.
    // 360 wide at mono23 is 26 cells of 13.81px, and 144 characters wrap into
    // six lines of 25 -- 96 through 246, with the note moved down to meet it.
    lv_obj_t *k = wt_lbl(s_scr, key, 400, 96, wt_font_mono23(), WT_INK);
    lv_obj_set_width(k, 360);
    lv_label_set_long_mode(k, LV_LABEL_LONG_WRAP);

    // Placed off the key's MEASURED height rather than a y decided in advance:
    // the wrap depends on where LVGL takes its breaks, and a hard 250 is how
    // the old layout ended up with a band of dead glass above it.
    lv_obj_update_layout(k);
    int note_y = 96 + lv_obj_get_height(k) + 16;
    wt_note(s_scr, tr(STR_R_SP_EXPORT_NOTE), 400, note_y, 360,
            WT_CONTENT_BOTTOM - note_y);

    // 592, not WT_BACK_X: 160 wide, so 752-160 is flush.
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, sp_key_back_cb, NULL);
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
            wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 48, WT_ACTION_Y, 0, false, words_page_cb, (void *)(intptr_t)-1);
        // STR_R_NEXT ("NEXT") is the receive flow's page-forward label. Same
        // word, already translated in all 21 locales; borrowing it beats
        // adding a string that would have to reach every table to ship.
        if (page < pages - 1)
            wt_arrow_action(s_scr, tr(STR_R_NEXT), false, false, 208, WT_ACTION_Y, 0, false, words_page_cb, (void *)(intptr_t)1);
        wt_lbl(s_scr, cnt, 380, 416, wt_font23(), WT_MUT);
    }
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, words_back_cb, NULL);
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

// ---- RECOVERY WORDS: two groups, PAPER and ENCRYPTED --------------------
// The page used to put five idioms on one screen -- two state chips, a
// destination row wedged into whatever width the first chip left over, a
// conditional third chip, and a three paragraph body taking 226 of the 302
// usable pixels under the header. Three quarters of the page was one amber
// paragraph, and the encrypted backup was an afterthought BY CONSTRUCTION:
// its x was computed from the measured width of the chip beside it.
//
// Underneath that it was asking three unlabelled questions at once -- is my
// paper proven, do I want the words on the glass now, and do I want a second
// encrypted backup that does not contain my passphrase. Two of those are the
// paper copy and one is not, so there are two groups, and the strip names
// them.
//
// The body is gone rather than moved. The words screen itself already says
// "copy them onto paper, in order. never a photo, never a file." at the moment
// the words are actually on the glass; the landing page was restating it one
// screen early, which the copy rule says to cut. What survives is one muted
// line per group.
enum { WTAB_PAPER = 0, WTAB_ENC, WTAB_N };

#define w_pane  s_wctx.pane
#define w_tab   s_wctx.tab

static void wtab_paper(void)
{
    const bool ok = kiss_ui_backup_checked();

    wt_row_wide(w_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label = tr(STR_I_WROW_SHOW),
        .sub   = tr(STR_I_WROW_SHOW_SUB),
        .kind  = WT_WIDE_OPEN,
        .cb    = words_show_cb,
    });

    // The same two facts the SETTINGS backup row states, in the same shape: a
    // glyph for the value and the state in the sub, so a long locale grows the
    // lane it has rather than the chip it does not.
    char wsub[96], wval[8];
    if (ok) {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        char idstr[16];
        snprintf(idstr, sizeof idstr, "%02X%02X%02X%02X",
                 fp[0], fp[1], fp[2], fp[3]);
        snprintf(wsub, sizeof wsub, tr(STR_I_WORDS_VERIFIED_FMT), idstr);
        snprintf(wval, sizeof wval, "%s", LV_SYMBOL_OK);
    } else {
        snprintf(wsub, sizeof wsub, "%s", tr(STR_I_WORDS_UNVERIFIED));
        snprintf(wval, sizeof wval, "%s", LV_SYMBOL_WARNING);
    }
    wt_row_wide(w_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label   = tr(STR_I_WROW_CHECK),
        .sub     = wsub,
        .sub_col = ok ? WT_OK : WT_WARN,
        .kind    = WT_WIDE_OPEN,
        .val     = wval,
        .vcol    = ok ? WT_OK : WT_WARN,
        .sev     = ok ? WT_SEV_OK : WT_SEV_WARN,
        .cb      = verify_copy_cb,
    });

    // The entropy judge's verdict, carried forward from the seed that was
    // made. It belongs on THIS page and not on the one that showed it first:
    // the warning during setup arrives at the most excited moment of the
    // ritual, and this is the screen someone opens to copy words onto paper,
    // which is the last moment redoing the seed is still cheap.
    //
    // A row rather than a chip now, so the page has one idiom instead of two.
    // Reuses the string the warning screen wears, so it costs no new key in 21
    // locales.
    //
    // NOT WT_WIDE_INERT, which was a straight contradiction: wt_row_wide
    // computes vcol as `inert ? WT_DIM : col_or(...)`, so the WT_WARN below was
    // thrown away and the row drew a GREY warning sign inside an amber card.
    // Inert means present and dead. This is a live caution about the seed the
    // signer is holding; it simply has nothing to tap, which is what OPEN with
    // no callback says.
    int rows = 2;
    const int note = kiss_seed_entropy_note();
    if (note != 0) {
        wt_row_wide(w_pane, WT_WIDE_Y(2), &(wt_wide_t){
            .label = tr(WSEED_ENTQ_IS_CARDS(note) ? STR_W_CARDS_WARN_T
                                                  : STR_W_DICE_WARN_T),
            .kind  = WT_WIDE_OPEN,
            .val   = LV_SYMBOL_WARNING,
            .vcol  = WT_WARN,
            .sev   = WT_SEV_WARN,
        });
        rows = 3;
    }

    wt_group_note(w_pane, rows, tr(STR_I_EXPL_BACKUP));

    // WHICH keys, in the band the two rows leave under them. Every row in this
    // group is about a set of keys and none of them says whose; the
    // fingerprint is the only thing an owner can hold against the paper
    // already in their hand, and it is what the words on the next screen are
    // CALLED. On the value-card idiom, so the same eight characters sit where
    // they sit on the fingerprint reveal and the pairing screen, and on
    // STR_L_FP_CAP, which those screens already ship in 21 locales.
    //
    // Only when the group left room. A third row pushes the note to 336 and
    // there is no band to earn.
    if (rows == 2) kiss_fp_card(w_pane, 302);
}

static void wtab_enc(void)
{
    // kiss_session_decoy() is 1 for the EMPTY-passphrase session, so pp true
    // means the owner HAS a passphrase -- and the envelope then holds half of
    // what restores them.
    const bool pp = !kiss_session_decoy();

    // The sub NAMES the two artifacts, because "a locked QR of your keys" left
    // an owner with nothing to look for on the card. One QR to photograph, or
    // one file, and the file's name is the fingerprint -- which the card below
    // this group is already showing, so the two read together.
    char ksub[64];
    uint8_t kfp[4];
    kiss_ui_last_fp(kfp);
    if (kiss_fp_known(kfp))
        snprintf(ksub, sizeof ksub, tr(STR_I_ROW_KEF_SUB_FMT),
                 kfp[0], kfp[1], kfp[2], kfp[3]);
    else
        snprintf(ksub, sizeof ksub, "%s", tr(STR_I_ROW_KEF_SUB));

    wt_row_wide(w_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label = tr(STR_I_WROW_KEF),
        .sub   = ksub,
        .kind  = WT_WIDE_OPEN,
        .cb    = kef_warn_screen,
    });

    // What is in it, before the owner taps the row above. The consent screen
    // states this too, but as one of two blocks weighted the same as the
    // reassuring one -- and the thing that bites is that this QR rebuilds
    // DIFFERENT keys on its own. It leads here.
    //
    // NO VALUE GLYPH, no vcol, no severity. This row DESCRIBES a format; it
    // does not report a state, and this device's green tick means one thing --
    // kiss_theme.h calls it "a state already satisfied", and every other tick
    // in the product obeys that: paper verified, the sealed file present, the
    // storage mode you are on. It came back from the bench read exactly the
    // way the grammar says to read it: "misleading, making it seem like one was
    // already set."
    //
    // It could not have been true either way. There is no record anywhere that
    // an encrypted backup was ever made -- no NVS key, no counter, and
    // kef_wipe() clears the session copy on every exit -- so even "you made one
    // two minutes ago" is unknowable here. Counting .kef files means mounting
    // the card inside a tab animation, and answers about whatever card is in
    // the slot rather than about this signer.
    //
    // The firmware screen had this same bug and its fix is the rule: the label
    // IS the claim, so there is no value beside it and no severity colour. A
    // tick is a result. WT_WIDE_OPEN with no callback keeps it in full ink with
    // no chevron and no tap; the amber is on the sub alone, where it is a
    // caution about CONTENT rather than a claim about state.
    wt_row_wide(w_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label   = tr(STR_I_WROW_HOLDS),
        .sub     = tr(pp ? STR_I_KEF_PP_H : STR_I_KEF_WARN_S),
        .sub_col = pp ? WT_WARN : WT_MUT,
        .kind    = WT_WIDE_OPEN,
    });

    wt_group_note(w_pane, 2, tr(STR_I_KEF_W2_H));

    // The same fingerprint the PAPER group frames, for the same reason and in
    // the same place: this is a backup OF a set of keys, the owner is entitled
    // to know which, and the sealed file is named by it. Two rows leave the
    // band; the subject earns it.
    kiss_fp_card(w_pane, 302);
}

static void wtab_build(void)
{
    if (w_tab == WTAB_ENC) wtab_enc();
    else                   wtab_paper();
}

static void wtab_cb(lv_event_t *e)
{
    // No stop group here: neither of these two is destructive, so nothing
    // rises and nothing reddens.
    wt_pane_go(&s_wctx, (int)(intptr_t)lv_event_get_user_data(e), false,
               wtab_build);
}

static void words_page(void)
{
    swap_screen();
    s_wctx.pane = s_wctx.pane_out = s_wctx.tabs = NULL;
    s_wctx.entering = false;
    if (w_tab < 0 || w_tab >= WTAB_N) w_tab = WTAB_PAPER;

    // NO SUBTITLE, and that is what frees y=68 for the strip -- the same trade
    // SETTINGS makes. What the subtitle said ("the seed words that rebuild
    // your keys") is what the groups now say by being named.
    //
    // The title keeps STR_I_WORDS_BTN: check_screen_coverage.py tracks a page
    // by its title's string id, and a literal one silently drops out of the
    // count.
    s_scr = s_wctx.scr = wt_screen(s_parent, tr(STR_I_WORDS_BTN), NULL);

    const wt_tab_t tabs[WTAB_N] = {
        { WT_ICON_SECRET, tr(STR_I_WTAB_PAPER),
          !kiss_ui_backup_checked(), false },
        { WT_ICON_LOCK,   tr(STR_I_WTAB_ENC),   false, false },
    };
    // Brackets, like the page one tap above it. This file built a slab strip
    // here and a bracket strip in info_screen -- two tab idioms, one file, two
    // taps apart, which is the worst place on the device to have had them.
    s_wctx.select = wt_brackets_select;
    s_wctx.tabs = wt_brackets(s_scr, tabs, WTAB_N, w_tab, WT_WIDE_X, 68,
                              WT_WIDE_W, wtab_cb);
    wt_pane_tabs_watch(&s_wctx);

    s_wctx.pane = wt_pane_new(&s_wctx);
    wtab_build();

    // BACK and nothing else. SHOW THE WORDS and VERIFY WORDS are rows now, so
    // the action bar stops competing with the page for the same subject. No
    // attention chip either: wt_alert_chip plants itself at WT_ACT_X, which is
    // exactly where those two pills used to sit.
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, words_back_cb, NULL);
}

static void words_warn_screen(lv_event_t *e)
{
    (void)e;
    words_page();
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
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, true, 592, WT_ACTION_Y, 160, true, kef_finish_cb, NULL);
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

    // Whether a passphrase stands between the words in this envelope and the
    // keys it opens. kiss_session_decoy() is 1 for the session an EMPTY
    // passphrase derives, so 0 here means the owner typed one -- and the
    // envelope holds the words alone, which is not the same thing.
    const bool pp = !kiss_session_decoy();
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_KEF),
                      tr(pp ? STR_I_KEF_WARN_S_PP : STR_I_KEF_WARN_S));

    // The mechanism, drawn before it is explained: what goes in, plus one
    // password, becomes a QR that only the password opens. With a passphrase
    // the left chip is the WORDS, because the keys are the words plus the
    // passphrase and only one of those two is going in the envelope.
    lv_obj_t *card = wt_card(s_scr, 48, 96, 704, 64);
    lv_obj_t *row = wt_diagram_row(card);
    wt_chip(row, tr_sym(WT_ICON_KEY, pp ? STR_D_WORDS : STR_D_KEYS), true);
    wt_diagram_op(row, "+");
    wt_chip(row, tr_sym(WT_ICON_LOCK, STR_L_KEF_PASS_OPEN), true);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, tr_sym(WT_ICON_QR, STR_I_KEF_CHIP_QR), false);
    lv_obj_center(row);

    // Two claims, split. Without a passphrase this is the kit's usual pairing:
    // the accent rule on how it works, WT_WARN on where it goes wrong.
    //
    // WITH a passphrase there is no "how it works" claim left to make, because
    // the shipped one is FALSE for that owner: "type the password, and your
    // keys are back" is true only when the words alone are the keys. The
    // passphrase is wiped at login by design (kiss_crypto.h), so it is not in
    // the envelope and no future version can quietly put it there.
    //
    // That sentence used to lead in the ACCENT colour -- the colour this kit
    // uses for how a thing works -- beside a WT_WARN block about a lesser
    // risk, so the page said "here is a feature, and by the way" about the one
    // fact standing between a passphrase owner and a backup that restores an
    // empty wallet. For that owner BOTH claims are where it goes wrong, and
    // both wear WT_WARN. The diagram above already carries the mechanism.
    {
        const char *h1 = tr(pp ? STR_I_KEF_PP_H : STR_I_KEF_W1_H);
        const char *b1 = tr(pp ? STR_I_KEF_PP_B : STR_I_KEF_W1_B);
        const char *h2 = tr(STR_I_KEF_W2_H), *b2 = tr(STR_I_KEF_W2_B);
        const int BW = 344, BY = 176, BH = WT_CONTENT_BOTTOM - BY;
        const lv_font_t *f = wt_body_font2_head(h1, b1, h2, b2, BW - 14, BH);
        wt_why_block(s_scr, h1, b1,  48, BY, BW, BH, f,
                     pp ? WT_WARN : wt_accent());
        wt_why_block(s_scr, h2, b2, 408, BY, BW, BH, f, WT_WARN);
    }

    // Making the envelope puts the keys on the glass as a QR one screen
    // later, so the entry is a deliberate hold, the scan-key precedent.
    wt_hold_pill(s_scr, tr(STR_I_KEF_MAKE_BTN), WT_ACT_X, WT_ACTION_Y, 330,
                 WT_ACTION_H, 900, kef_make, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, kef_finish_cb, NULL);
}

// ---- the section home: facts + actions ----
static void pair_open_cb(lv_event_t *e)  { (void)e; pair_screen(); }

// The four fact rows and the two coordinator rows all open something, and a row
// carries its key the way a help chip used to.
static void row_help_cb(lv_event_t *e) { help_cb(e); }

// RECOVERY WORDS is a different page with a different tab count, and a context
// remembers WHICH tab is open -- one shared with s_wctx would land an owner
// back from the words page on whichever KEYS tab matched the index.
static void info_tab_build(void);

static void info_tab_cb(lv_event_t *e)
{
    wt_pane_go(&s_ictx, (int)(intptr_t)lv_event_get_user_data(e), false,
               info_tab_build);
}

// The address fold this lane can hold. wt_addr_short's own fold is 28 mono
// cells and 28 at mono23 is 387px against the ~330 a line row's value lane
// leaves, so the air around the ellipsis goes and nothing else does: same
// blocks, same last eight lit. Cutting a BLOCK would change which characters
// the row teaches an owner to check, and shrinking the font is the bug this
// whole redesign was drawn to fix.
static void info_addr_value(lv_obj_t *row)
{
    char buf[128];
    size_t n = kiss_session_address(0, 0, buf, sizeof buf) == 0
                   ? strlen(buf) : 0;
    if (n < 20) {
        // The state, as words, never through the fold: an ellipsis and a lit
        // tail would turn LOCKED into an address-shaped fragment, and a state
        // has to read as a state.
        lv_obj_t *st = wt_lbl(row, tr(STR_C_SESSION_LOCKED), WT_LINE_PAD,
                              wt_line_val_y(), wt_font23(), WT_MUT);
        lv_obj_set_width(st, 300);
        lv_obj_set_height(st, lv_font_get_line_height(wt_font23()));
        lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
        return;
    }
    int pre = !strncmp(buf, "tsp1", 4) ? 5
            : (!strncmp(buf, "bc1", 3) || !strncmp(buf, "tb1", 3) ||
               !strncmp(buf, "sp1", 3)) ? 4 : 0;
    // Prefix, ellipsis, the last EIGHT in two blocks -- shorter than the
    // receive screen's fold because this lane is ~330px, and the blocks that
    // go are the ones no rule tells an owner to check. The eight that stay are
    // the eight the sub-line names.
    const char *t = buf + n - 8;
    // The prefix through a bounded copy, not "%.*s": the device compiler's
    // truncation gate cannot see that pre is at most 5, and a copy into a
    // char[8] is a bound it can prove.
    char pfx[8] = {0};
    if (pre) { memcpy(pfx, buf, (size_t)pre); pfx[pre] = ' '; }
    char head[24], tail[16];
    snprintf(head, sizeof head, "%s\xE2\x80\xA6 ", pfx);
    snprintf(tail, sizeof tail, "%.4s %.4s", t, t + 4);

    lv_obj_t *sg = lv_spangroup_create(row);
    // A spangroup is clickable out of the box and silently eats every press
    // that lands on it -- inside a tappable row that kills the row exactly
    // where the address is printed.
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);
    lv_obj_set_style_text_font(sg, wt_font_mono28(), 0);
    lv_span_t *s1 = lv_spangroup_new_span(sg);
    lv_span_set_text(s1, head);
    lv_style_set_text_color(lv_span_get_style(s1), WT_MUT);
    lv_span_t *s2 = lv_spangroup_new_span(sg);
    lv_span_set_text(s2, tail);
    // Brightness alone marks the compared run, the same as every other address
    // on the device: no underline, no second hue.
    lv_style_set_text_color(lv_span_get_style(s2), wt_accent());
    // accent_walk repaints the LAST span of a flagged group, which is the
    // lit tail by construction here as everywhere else.
    lv_obj_add_flag(sg, WT_FLAG_ACCENT);
    lv_spangroup_refresh(sg);
    lv_obj_set_pos(sg, WT_LINE_PAD, wt_line_val_y());
}

// Four lines and two lines, both on the one full-width lane. The old screen
// put four facts in a left column and two destinations in a right one, which
// is four different shapes for six things that are all "a label, what it says,
// and where it takes you". They are one shape now, and the tab strip is what
// buys the room: the lane is 704 wide instead of 365, so an address fits at
// mono23 and a path fits beside its own type.
static void info_tab_build(void)
{
    lv_obj_t *p = s_ictx.pane;
    char buf[128];
    const int X = 48, W = 704;

    if (s_ictx.tab == 0) {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        const int H = 84;

        // Mono. This is a code you hold beside a coordinator's screen and
        // compare digit by digit, and the proportional face is the one that
        // makes 0 and O and 8 and B argue.
        snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
        wt_line_row_stage(wt_line_row(p, X, 120, W, H, tr(STR_D_FINGERPRINT),
                                      buf, wt_font_mono28(), WT_INK,
                                      tr(STR_K_FP_SUB), NULL,
                                      row_help_cb, (void *)"fp"), 0);
        wt_line_rule_draw(wt_line_rule(p, X, 120 + H, W), 110, 320);

        // The one line with nothing to open, so the one line with no arrow at
        // all. Not a dimmed arrow: a mark at low opacity still says there is
        // something under it.
        wt_line_row_stage(wt_line_row(p, X, 204, W, H, tr(STR_I_SEC_NET), kiss_net_name(),
                    wt_font28(), kiss_testnet() ? WT_WARN : WT_INK,
                    tr(kiss_testnet() ? STR_G_TESTNET_NOTE
                                      : STR_G_MAINNET_NOTE), NULL, NULL, NULL), 1);
        wt_line_rule_draw(wt_line_rule(p, X, 204 + H, W), 152, 320);

        // h, not an apostrophe, and this is correctness rather than style: at
        // small sizes the apostrophes in m/84'/0'/0' render as tick marks and
        // the line reads as m/84/0/0. Those are DIFFERENT PATHS, and a
        // coordinator handed the unhardened one finds none of these keys.
        int sc = kiss_script();
        int purpose = sc == WSCRIPT_LEGACY ? 44 : sc == WSCRIPT_NESTED ? 49 : 84;
        snprintf(buf, sizeof buf, "m/%dh/%dh/0h", purpose,
                 kiss_testnet() ? 1 : 0);
        wt_line_row_stage(wt_line_row(p, X, 288, W, H, tr(STR_I_SEC_TYPE),
                    tr(sc == WSCRIPT_LEGACY ? STR_S_TY_LEGACY
                       : sc == WSCRIPT_NESTED ? STR_S_TY_NESTED
                                              : STR_S_TY_NATIVE),
                    wt_font28(), WT_INK, buf, wt_font_mono23(),
                    row_help_cb, (void *)"type"), 2);
        wt_line_rule_draw(wt_line_rule(p, X, 288 + H, W), 194, 320);

        return;
    }

    const int H = 76;
    wt_line_row_stage(wt_line_row(p, X, 120, W, H, tr(STR_K_CAP_PAIRING),
                                  tr(STR_I_PAIR_T), wt_font28(), WT_INK,
                                  tr(STR_K_PAIR_SUB), NULL,
                                  pair_open_cb, NULL), 0);
    wt_line_rule_draw(wt_line_rule(p, X, 120 + H, W), 110, 320);
    // "Scan" elsewhere on this device means the camera. Here it means searching
    // the chain, and the caption above the value is what says which.
    wt_line_row_stage(wt_line_row(p, X, 196, W, H, tr(STR_R_SP_BTN),
                                  tr(STR_R_SP_SCAN_BTN), wt_font28(), WT_INK,
                                  tr(STR_K_SP_SUB), NULL,
                                  sp_key_warn_cb, NULL), 1);
    wt_line_rule_draw(wt_line_rule(p, X, 196 + H, W), 152, 320);

    lv_obj_t *ar = wt_line_row(p, X, 272, W, H, tr(STR_I_SEC_FIRST), NULL,
                   NULL, WT_INK, tr(STR_S_CMP_8), NULL,
                   row_help_cb, (void *)"addr");
    info_addr_value(ar);
    wt_line_rule_draw(wt_line_rule(p, X, 272 + H, W), 194, 320);
    // 368, thirty clear of the floor. No explainer on this tab: four lines
    // IS the explanation, and a sentence under them would be the page
    // telling an owner what they have just read.

    // The card that held SCAN KEY is gone, and its note with it: the note is
    // already repeated on the warn screen this line opens, which is where a
    // caution about handing out a key belongs. What replaces it is the one
    // sentence neither screen ever said -- what the coordinator can and cannot
    // do -- and it is the tab's whole point in two clauses.
    // Pinned to ONE line at font23, not through wt_note: wt_note's ladder
    // measures a WRAPPED block against the box, and a 42px band cannot hold
    // two wrapped lines at 23, so it dropped the whole sentence to 14 -- the
    // tiny-type bug, arrived at by a helper doing exactly what it says. One
    // line at 23 fits the band with room; a locale too long for the lane loses
    // its tail and CUT is what reports that.
    lv_obj_t *ex = wt_lbl(p, tr(STR_K_EXPL_COORD), X, 358, wt_font23(),
                          WT_MUT);
    lv_obj_set_width(ex, W);
    lv_obj_set_height(ex, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(ex, LV_LABEL_LONG_DOT);
}

static void info_screen(void)
{
    s_pair_qr = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_T), NULL);
    wt_title_fit(s_scr, 704);
    wt_title_cursor(s_scr);

    static const wt_tab_t tabs[2] = {
        { .icon = WT_ICON_KEY,        .label = "THIS SIGNER" },
        { .icon = WT_ICON_LINK,       .label = "COORDINATOR" },
    };
    // The label strings are per-locale, so the array's two are placeholders
    // that never reach the glass: wt_brackets is handed the translated pair.
    wt_tab_t t[2] = { tabs[0], tabs[1] };
    t[0].label = tr(STR_I_SEC_THIS_WALLET);
    t[1].label = tr(STR_D_ONLINE_APP);

    s_ictx.scr    = s_scr;
    s_ictx.select = wt_brackets_select;
    s_ictx.tabs   = wt_brackets(s_scr, t, 2, s_ictx.tab, 48, 70, 704,
                                info_tab_cb);
    wt_pane_tabs_watch(&s_ictx);
    s_ictx.pane = wt_pane_new(&s_ictx);
    info_tab_build();

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
    // The network chip: a mark and its word, no box. It is the one thing on
    // this page that is amber, and it is amber because it is a caution rather
    // than a colour -- an owner on a test network is looking at money that is
    // not money. Absent on mainnet; a chip reading MAINNET would be the device
    // congratulating itself on the normal case.
    if (kiss_testnet()) {
        lv_obj_t *w = wt_lbl(s_scr, LV_SYMBOL_WARNING, WT_ACT_X,
                             WT_ACTION_Y + 18, wt_font14(), WT_WARN);
        lv_obj_update_layout(w);
        lv_obj_t *l = wt_lbl(s_scr, tr(STR_G_TEST_CHIP),
                             WT_ACT_X + lv_obj_get_width(w) + 10,
                             WT_ACTION_Y + 16, wt_font14(), WT_WARN);
        lv_obj_set_style_text_letter_space(l, 2, 0);
    }
}

void kiss_info_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = NULL;
    s_pair_fmt = 0;
    // A fresh entry lands on tab 1. The context keeps its tab across a screen
    // rebuild on purpose -- that is what returns an owner to the tab they left
    // when a row's screen goes BACK -- so entering the page has to say so.
    s_ictx.tab = 0;
    s_scan_done = NULL;
    info_screen();
}

void kiss_info_open_scan_key(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent;
    s_scan_done = done_cb;
    sp_key_warn_cb(NULL);
}

void kiss_info_open_words(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = done_cb;
    words_warn_screen(NULL);
}
