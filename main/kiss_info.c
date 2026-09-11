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
#include "kiss_terms.h"   // the ten cards, and this page's two of them
#include "kiss_theme.h"
#include "kiss_usage.h"   // has a coordinator ever spoken: the 5c empty state
#include "kiss_wipe.h"
#include "kiss_recv.h"   // kiss_recv_open_first: FIRST ADDRESS rows land there
#include "kiss_rehearse.h"
#include "kiss_ui.h"   // kiss_ui_last_fp; the borrowed KEF password keyboard
#include "platform_sd.h"

static lv_obj_t *s_scr;                 // whichever wallet-section screen is up
static lv_obj_t *s_parent;
static void (*s_words_done)(void);
// Where the scan key reveal goes home to. NULL is KEYS, its own page; RECEIVE's
// SILENT tab sets it, the same shape s_words_done gives the backup grid. ONE
// flow either way -- the second door opens the same gate, never a copy of it.
static void (*s_scan_key_done)(void);
static int s_pair_fmt;                  // 0 = descriptor (Sparrow), 1 = BlueWallet
static lv_obj_t *s_pair_qr, *s_pair_tabs;
// The import path, as steps the selected tab fills in: up to three lines cut
// out of the note's first line, and the note's second line under them. The
// locked one replaces the lot when the session cannot export.
#define PAIR_RUNGS 3
static lv_obj_t *s_pair_rung[PAIR_RUNGS], *s_pair_tail, *s_pair_locked;

static void info_screen(void);
static void kef_warn_screen(lv_event_t *e);
static void kef_wipe(void);
static void sp_key_wipe(void);   // the exported scan key, out of the label's heap

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
    sp_key_wipe();    // nor a scan key in the label's heap block
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

// FIRST ADDRESS goes to the address itself: RECEIVE's THIS ADDRESS at index
// 0, the one place a full address renders as text. Both tabs' address rows
// share this door; neither explains in place any more.
static void first_addr_go_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *par = s_parent;
    close_cb(NULL);
    kiss_recv_open_first(par);
}

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
    // centred boxes. The group centres; the box centres; they agree.
    //
    // The text is content sized with a 482 ceiling rather than a fixed 482 box.
    // Content sized is what lets a short row centre tightly around its own
    // words; the ceiling is what keeps a long translation wrapping inside the
    // box instead of running out of it. Every locale fits one line today, so
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
// IN A CARD, since the explainer body stopped being a ruled block. That rule
// -- 3px wide and as tall as the paragraph -- was what oc_is_frame counted on
// these screens, and the diagram beside it never was: a chip is about 40x28
// and the frame test wants 100x30. So the picture was invisible to BARE the
// moment the rule went, and the gate reported a screen with a diagram on it as
// a wall of text. Same answer kiss_word_ui.c reached for the same reason, and
// it reads better: the diagram is one object on the glass instead of two chips
// floating in the band above the prose.
static int aside_col(lv_obj_t *par, int x, int y, int w, void (*fill)(lv_obj_t *))
{
    const int pad = 12;
    lv_obj_t *card = wt_card(par, x, y, w, 2 * pad);
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 0, pad);
    lv_obj_set_width(col, w);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    fill(col);
    lv_obj_update_layout(col);
    const int h = lv_obj_get_height(col) + 2 * pad;
    lv_obj_set_height(card, h);
    return h;
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

    // Three claims, three lines, each led by its mark: the card once ran six
    // rendered lines and the bench asked for three. The icons are composed
    // here, not in the translated strings -- translators never handle glyph
    // bytes -- and the body font (wt_font28/23) resolves them through the nat
    // chain at full size. No blank lines: one costs a whole line of type.
    const char *b  = tr(STR_I_H_FP_B);
    const char *nl = strchr(b, '\n');
    char body[512];
    if (nl && exit_hint)
        snprintf(body, sizeof body, "%s %.*s\n%s %s\n%s %s",
                 WT_ICON_LINK, (int)(nl - b), b, LV_SYMBOL_OK, nl + 1,
                 LV_SYMBOL_PLAY, tr(STR_H_EXIT_HINT));
    else if (nl)
        snprintf(body, sizeof body, "%s %.*s\n%s %s",
                 WT_ICON_LINK, (int)(nl - b), b, LV_SYMBOL_OK, nl + 1);
    else
        snprintf(body, sizeof body, "%s", b);

    return help_open_on(parent, title, body, DIAG_FP, false, NULL, NULL, 0);
}

lv_obj_t *kiss_info_help_card_open(lv_obj_t *parent, const char *title,
                                     const char *body, const char *icon)
{
    return help_open_on(parent, title, body, DIAG_NONE, false, icon, NULL, 0);
}

// The section "?" chips on this screen are all gone now. "fp" and "type" went
// when both facts started opening their definitions IN PLACE; "scan" and
// "addr" went with the cards that carried them; and "pair" was the last, a
// PAGE level explanation hanging off a section mark, which the page's own
// [ ? n ] carries properly. The fingerprint card itself lives on -- the
// reveal screen and the home chip still open it through
// kiss_info_fp_card_open.

// ---- PAIR COORDINATOR ----
// The pairing screen's right column, as numbers both halves read.
//
// The import note is built EMPTY and filled by pair_refresh, so its box and
// its fit budget live apart -- and they disagreed: the box was 108 tall and
// the refit was told it had 190, which is 76px past the standing line below
// it. English fits in 108 and never showed it. Ten locales overflowed into
// that line, and it took the twenty one locale sweep's first complete run to
// say so.
// THE TWO BAYS. The code on the left at the size it has always been, the
// import path on the right, and a rule between them.
//
// THE QR DOES NOT SHRINK. The frame put it in the right half of a 704x258
// card at 200px, on the reasoning that 200 still gives seven pixels a dot at
// 25 to 29 dots. A pairing descriptor is not 25 dots. It is 138 bytes, which
// qrcodegen fits at version 8 -- 49 dots -- so 200px is 4.1 pixels a dot
// against the 5.4 this screen shipped with, a quarter gone off the one code
// on the device with no fallback behind it. The signed transaction has EASY
// SCAN when a phone struggles; this page has nothing, because a static
// descriptor has no loop to slow down and no frames to fatten. So the code
// keeps its 264, which means it keeps the left margin and its full height,
// and the card that would have framed both bays goes instead. The white QR
// card is the frame on this page, as it was before.
#define PAIR_STRIP_Y     62   // the contract's 70, less the 8 the QR needs
#define PAIR_QR_X        48
#define PAIR_QR_Y        96
#define PAIR_QR_CARD    300
#define PAIR_QR_PX      264
#define PAIR_RULE_X     372
#define PAIR_BAY_X      392
#define PAIR_BAY_W      (752 - PAIR_BAY_X)   // 360; the widest rung is 350
#define PAIR_RUNG_Y     112
#define PAIR_RUNG_PITCH  52
#define PAIR_TAIL_Y     280

// The note's FIRST line, cut on " > " into ladder rungs. The separator is
// literal and never per-locale: the paths themselves are the coordinator's own
// menu items and ship the same in every language, arrows included, so one
// split covers all twenty one. A line with no separator in it is one rung
// carrying the whole thing, which is what a coordinator with a one-step import
// would leave here.
static int pair_ladder(const char *note, char out[PAIR_RUNGS][96],
                       const char **tail)
{
    const char *nl = strchr(note, '\n');
    *tail = nl ? nl + 1 : "";
    const char *p = note;
    const char *end = nl ? nl : note + strlen(note);
    int n = 0;
    while (p < end && n < PAIR_RUNGS) {
        const char *sep = strstr(p, " > ");
        if (!sep || sep > end) sep = end;
        size_t len = (size_t)(sep - p);
        if (len >= sizeof out[0]) len = sizeof out[0] - 1;
        memcpy(out[n], p, len);
        out[n][len] = 0;
        n++;
        p = sep < end ? sep + 3 : end;
    }
    return n;
}

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
    // The LOCKED note takes the whole ladder's lane, not the tail's. It is
    // three sentences about why there is no code, and the two lines the second
    // half of a working import gets would have dot-truncated the middle one.
    if (s_pair_locked) {
        if (rc != 0) lv_obj_remove_flag(s_pair_locked, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s_pair_locked, LV_OBJ_FLAG_HIDDEN);
    }
    if (rc == 0 && s_pair_qr)
        wt_qr_update(s_pair_qr, txt, (uint32_t)strlen(txt));

    const char *note = s_pair_fmt ? tr(STR_I_NOTE_BW) : tr(STR_I_NOTE_SPARROW);

    char rung[PAIR_RUNGS][96];
    const char *tail = "";
    const int n = rc == 0 ? pair_ladder(note, rung, &tail) : 0;
    for (int i = 0; i < PAIR_RUNGS; i++) {
        if (!s_pair_rung[i]) continue;
        if (i >= n) { lv_obj_add_flag(s_pair_rung[i], LV_OBJ_FLAG_HIDDEN); continue; }
        // THE SEPARATOR STAYS ON THE STEP IT FOLLOWS. The string writes the
        // path as "File > New Wallet > ...", which is how the coordinator's
        // own menu reads it and how anyone would say it out loud, so the mark
        // between two steps is the one already in the copy. The first cut drew
        // an L shaped rule in the gutter instead -- the handoff asked for a
        // down-and-right arrow, U+21B3, which is in none of these fonts -- and
        // a bracket nobody wrote is a symbol the reader has to learn.
        char line[100];
        if (i + 1 < n) snprintf(line, sizeof line, "%s >", rung[i]);
        else           snprintf(line, sizeof line, "%s", rung[i]);
        lv_label_set_text(s_pair_rung[i], line);
        lv_obj_remove_flag(s_pair_rung[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pair_tail) {
        // The face is chosen per STRING, not once at build: chrome18 hands
        // back the mono rung for a line it can draw and the locale's own 14
        // for one it cannot, and the two apps' second lines disagree about
        // that inside a single locale (es-MX draws one and not the other).
        const lv_font_t *tf = wt_chrome18(tail);
        lv_obj_set_style_text_font(s_pair_tail, tf, 0);
        lv_obj_set_height(s_pair_tail, 2 * lv_font_get_line_height(tf));
        lv_label_set_text(s_pair_tail, rc == 0 ? tail : "");
    }
}

// The pick, from the tab strip that IS the pick now. The strip carries the
// state the two 175x60 panes used to carry in an accent on an app name, so
// nothing here recolours anything: it moves the brackets and re-fills the bay.
static void pair_fmt_cb(lv_event_t *e)
{
    const int to = (int)(intptr_t)lv_event_get_user_data(e);
    if (to == s_pair_fmt) return;
    if (s_pair_tabs) wt_tabs_flex_select(s_pair_tabs, s_pair_fmt, to, false);
    s_pair_fmt = to;
    pair_refresh();
}

// The same pick, as a STROKE. Two apps side by side is a deck of two, and the
// bench's rule is that anything a finger can step should also take the swipe:
// a tap is a target you have to find, a swipe is not.
//
// The SETTINGS cycles do not get this and cannot -- a swipe there already
// steps the tab deck, and a screen where one gesture means two things is
// worse than one that means nothing.
static void pair_gesture_cb(lv_event_t *e)
{
    const int step = wt_swipe_step(e);
    if (!step) return;
    const int to = s_pair_fmt + step;
    if (to < 0 || to > 1) return;      // a deck of two, and it does not wrap
    if (s_pair_tabs) wt_tabs_flex_select(s_pair_tabs, s_pair_fmt, to, false);
    s_pair_fmt = to;
    pair_refresh();
}

// True when a coordinator has spoken for these keys. It is the device's only
// honest signal for paired-ness, and it is the same store RECEIVE's lamp
// reads: this signer has no chain view of its own.
static bool coord_known(void)
{
    uint8_t cfp[4];
    kiss_ui_last_fp(cfp);
    int chigh;
    uint32_t cheight;
    return kiss_usage_chain_known(cfp, kiss_testnet() ? 1 : 0, kiss_script(),
                                  &chigh, &cheight);
}

// Was PAIR COORDINATOR the way IN, rather than a step off the KEYS page?
// kiss_info_open() sets it when no coordinator has spoken.
static bool s_pair_entry;

static void pair_back_cb(lv_event_t *e)
{
    (void)e;
    // HOME, not KEYS, when pairing was the entry -- and this is not a
    // preference. KEYS forwards to this screen whenever no coordinator has
    // spoken, so a BACK that rebuilt KEYS would be forwarded straight back
    // here, forever, with the owner watching the same screen redraw.
    if (s_pair_entry) { close_cb(NULL); return; }
    swap_screen();
    info_screen();
}

static void pair_screen(void);

static void pair_qr_back_cb(lv_event_t *e)
{
    (void)e;
    pair_screen();
}

// The address proof, from the page that asks for it.
//
// The card below already says to scan the coordinator's first address and
// check it is yours; what it has never had is a way to do that. An owner who
// read it had to leave for KEYS, open FIRST ADDRESS, and find VERIFY on the
// receive screen -- two screens and three taps between an instruction and the
// act, on the one page whose subject is that pairing is not finished until it
// has been proved.
//
// The same shape FIRST ADDRESS uses one page back: take the parent, close this
// section, hand the glass over. close_cb rather than swap_screen, because the
// next screen is not one of this file's -- swap_screen leaves the section
// context half-alive for a page that will never be built.
static void pair_verify_go_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *par = s_parent;
    close_cb(NULL);
    kiss_recv_open_verify(par);
}

static void pair_instructions_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_pair_qr = NULL;
    // The trail, not the app name: the card's own mark and steps already say
    // which app these instructions are for.
    s_scr = wt_screen(s_parent, tr(STR_I_PAIR_T), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_I_T),
                 tr(STR_D_ONLINE_APP));
        wt_trail(s_scr, WT_ICON_QR, trail, false);
    }

    // Page two is intentionally static: first the exact import steps, then
    // the independent address proof. The raw descriptor is already encoded in
    // the QR and no longer crowds out the instructions people must read.
    // A card per step, eyebrow inside it. Two paragraphs of instructions with
    // two small captions above them, all floating on the page, left the reader to
    // work out which caption owned which paragraph. Both bodies keep a three line
    // budget at font23, which is what they had.
    // ONE MARK, and it is the thing being shown. This branched on the app: a
    // file for Sparrow, a telephone for BlueWallet. U+F095 is a HANDSET -- the
    // curved receiver off a desk phone -- so the glyph standing for "a wallet
    // in your pocket" was reading as "place a call", on a device that has no
    // radio and makes no calls. The same trap as a lightning bolt on a fee.
    //
    // Nothing is lost by dropping the distinction. The eyebrow under the tabs
    // already says DESKTOP or MOBILE in words, and the head says SHOW IT TO --
    // so the mark's job is to name what gets shown, which is the QR.
    // The body clears the HEAD, measured rather than guessed: wt_section moved
    // up a rung and both bodies were sitting inside the head's own line box,
    // which the overlap gate read as text on text on both cards.
    lv_obj_t *c1 = wt_card(s_scr, 36, 96, 716, 162);
    lv_obj_t *h1 = wt_section(c1, tr_sym(WT_ICON_QR, STR_I_SHOW_TO), 16, 10);
    lv_obj_update_layout(h1);
    const int b1 = 10 + lv_obj_get_height(h1) + 4;
    lv_obj_t *steps = wt_note(c1,
        s_pair_fmt ? tr(STR_I_NOTE_BW) : tr(STR_I_NOTE_SPARROW),
        16, b1, 688, 162 - b1 - 12);
    lv_obj_set_style_text_color(steps, WT_INK, 0);

    lv_obj_t *c2 = wt_card(s_scr, 36, 264, 716, 132);
    lv_obj_t *h2 = wt_section(c2, tr_sym(LV_SYMBOL_OK, STR_R_VERIFY), 16, 8);
    lv_obj_update_layout(h2);
    const int b2 = 8 + lv_obj_get_height(h2) + 4;
    lv_obj_t *prove = wt_note(c2, tr(STR_I_PROVE), 16, b2, 688, 132 - b2 - 10);
    lv_obj_set_style_text_color(prove, WT_INK, 0);

    // RECEIVE's band, in RECEIVE's own three positions: the screen's action on
    // the left at WT_ACT_X, the secondary at 300, the exit in the corner. BACK
    // moves out of the left slot because it is page navigation and VERIFY is
    // what this page is FOR -- the rule at WT_ACT_X in kiss_theme.h, applied
    // the moment the page acquired an action of its own.
    //
    // Primary and forward-arrowed, exactly as the same word is on RECEIVE, so
    // the control an owner meets here and the one they meet there are visibly
    // the same control. It opens the camera directly; see kiss_recv_open_verify.
    wt_arrow_action(s_scr, tr(STR_R_VERIFY), false, true, WT_ACT_X, WT_ACTION_Y,
                    0, false, pair_verify_go_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 300, WT_ACTION_Y, 0, false, pair_qr_back_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, pair_back_cb, NULL);
}

static void sp_key_warn_cb(lv_event_t *e);   // scan-key export, warning first

// PAIRING's own two words. The section chip beside SHOW TO used to open a
// card titled THE COORDINATOR, which was a PAGE level explanation hanging off
// a SECTION level mark -- and it explained the descriptor in its own words
// rather than naming it, so a reader left knowing what this device calls the
// thing and not what Sparrow calls it.
//
// What it said is what these two cards say: DESCRIPTOR is what your app sees
// with and cannot spend from, FINGERPRINT is what your keys are called. Its
// third line -- that the bitcoin is on the network and in neither one -- goes
// with it; that claim belongs to a page about coins, not to a QR.
static void pair_screen(void);

// PAGE TWO for DESCRIPTOR: the artefact itself. A descriptor is 150 odd
// characters and a definition row has one line for a value, so the row says
// what it IS and this page is where the thing lives.
//
// Through wt_addr_spans, the same idiom every other long value on this device
// gets, so it reads in blocks rather than as a wall -- and never truncated: a
// descriptor cut short that looks complete is the worst outcome available.
static bool pair_term_has_more(int id)
{
    char txt[512];
    return id == KISS_TERM_DESC && kiss_session_descriptor(txt, sizeof txt) == 0;
}

static void pair_terms_cb(lv_event_t *e);
static void desc2_back_cb(lv_event_t *e) { (void)e; pair_terms_cb(NULL); }

static void pair_term_more(int id)
{
    char txt[512];
    if (id != KISS_TERM_DESC || kiss_session_descriptor(txt, sizeof txt) != 0)
        return;
    kiss_terms_leaving();
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_T_WATCH_CAP), NULL);
    wt_chrome_head(s_scr);
    wt_trail(s_scr, WT_ICON_WHAT, tr(STR_S_GLOSSARY_T), false);

    wt_lbl(s_scr, tr(STR_T_WATCH_P2_HEAD), WT_LANE_X, 118, wt_font_mono28(),
           WT_INK);
    wt_lbl(s_scr, tr(STR_T_WATCH_P2_B), WT_LANE_X, 158,
           wt_font_mono23(), WT_MUT);
    // FRAMED, because a descriptor is a VALUE and the chrome contract asks
    // every screen for something framed above the action row. It was four
    // lines of raw text on the page ground with a hundred pixels of nothing
    // under them -- reported BARE the moment overlapcheck could read a
    // spangroup, which is what this block is.
    //
    // The card is measured to the text rather than fixed: a descriptor's
    // length moves with the script type and the fingerprint, and a box that
    // fits zpub would clip a longer one.
    // A NARROWER COLUMN, which is the silent payment view's idiom and is here
    // for its reason rather than for the gate's: a 704px run of base58 is four
    // lines an eye cannot keep its place in, and sp_addr_render sets its own
    // address in a 386px column for exactly that. More lines, each trackable.
    //
    // It also stops being a WALL by measure, which is the honest order of
    // events: the column is narrower because it reads better, and a 704px
    // paragraph of data in a box was what the gate objected to.
    const int dw = 470;
    lv_obj_t *d = wt_addr_spans(s_scr, txt, dw - 48, wt_font_mono23());
    lv_obj_update_layout(d);
    lv_obj_t *card = wt_card(s_scr, WT_LANE_X, 196, dw,
                             lv_obj_get_height(d) + 44);
    lv_obj_set_parent(d, card);
    lv_obj_set_pos(d, 24, 22);

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, WT_BACK_X,
                    WT_ACTION_Y, 140, true, desc2_back_cb, NULL);
}

static void pair_terms_back_cb(lv_event_t *e)
{
    (void)e;
    kiss_terms_leaving();
    pair_screen();
}

static void pair_terms_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_screen(s_parent, tr(STR_S_GLOSSARY_T), NULL);
    wt_chrome_head(s_scr);
    wt_trail(s_scr, WT_ICON_WHAT, tr(STR_I_PAIR_T), false);
    kiss_terms_more_hook(pair_term_has_more, pair_term_more);
    kiss_terms_list(s_scr, KISS_TERMS_PAIR, 3);
    kiss_terms_hint(s_scr, KISS_TERMS_PAIR, 3);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, WT_BACK_X,
                    WT_ACTION_Y, 140, true, pair_terms_back_cb, NULL);
}

// WHAT THIS QR HANDS OVER, at the moment it is handed over -- on the BAND now
// rather than in the lane, because the lane belongs to the one card about the
// one app and this sentence is true of both of them.
//
// K_EXPL_COORD carries both halves in one string and ships in 21 locales, so
// this costs no key: "it sees every transaction. it can never spend one." The
// two halves are two labels rather than one paragraph, because the accent has
// to fall on the SECOND sentence and not merely on the stop between them --
// the reassurance and the limit are the point, and the limit is the half
// worth finding at a glance. Centred between NEXT on the left and the exit in
// the corner.
//
// ja and zh end a sentence with U+3002 and never with ". ", which is the same
// split spans_fill has to make; a locale with neither takes the whole string
// on one line rather than being cut at a boundary it does not have.
//
// The lane is MEASURED off the control beside it, not pinned at 190. Turkish
// says NEXT in a word wide enough to reach past that, and the walk caught both
// halves of this sentence sitting on its arrow -- a fixed left edge here is a
// promise about how long a translation is.
// WHAT THIS QR HANDS OVER, at the moment it is handed over -- on the BAND now
// rather than in the lane, because the lane belongs to the one card about the
// one app and this sentence is true of both of them.
//
// K_EXPL_COORD carries both halves in one string and ships in 21 locales, so
// this costs no key: "it sees every transaction. it can never spend one." The
// two halves are two labels rather than one paragraph, because the accent has
// to fall on the SECOND sentence and not merely on the stop between them --
// the reassurance and the limit are the point, and the limit is the half
// worth finding at a glance. Centred between NEXT on the left and the exit in
// the corner.
//
// ja and zh end a sentence with U+3002 and never with ". ", which is the same
// split spans_fill has to make; a locale with neither takes the whole string
// on one line rather than being cut at a boundary it does not have.
//
// The lane is MEASURED off the control beside it, not pinned at 190. Turkish
// says NEXT in a word wide enough to reach past that, and the walk caught both
// halves of this sentence sitting on its arrow -- a fixed left edge here is a
// promise about how long a translation is.
static void pair_band_claim(lv_obj_t *next_act)
{
    const char *txt = tr(STR_K_EXPL_COORD);
    const char *sp = strstr(txt, ". ");
    const char *cjk = strstr(txt, "\xE3\x80\x82");
    char head[192];
    const char *rest = NULL;
    if (sp) {
        size_t n = (size_t)(sp - txt) + 1;              // the stop stays with it
        if (n >= sizeof head) n = sizeof head - 1;
        memcpy(head, txt, n); head[n] = 0;
        rest = sp + 2;
    } else if (cjk) {
        size_t n = (size_t)(cjk - txt) + 3;
        if (n >= sizeof head) n = sizeof head - 1;
        memcpy(head, txt, n); head[n] = 0;
        rest = cjk + 3;
    }
    const char *one = rest && *rest ? head : txt;
    const lv_font_t *f = wt_chrome18(one);
    const int lh = lv_font_get_line_height(f);
    const int n = rest && *rest ? 2 : 1;
    // Centred in the band the way wt_standing centres its own line, so the
    // claim sits on the same rung as every other sentence down here.
    int y = WT_ACTION_Y + (WT_ACTION_H - n * lh) / 2;
    int x0 = 190;
    if (next_act) {
        lv_obj_update_layout(next_act);   // or the width answers zero
        const int after = WT_ACT_X + lv_obj_get_width(next_act) + 16;
        if (after > x0) x0 = after;
    }
    const int w = 592 - 12 - x0;
    if (w < 120) return;                  // no lane left: the controls win
    const char *line[2] = { one, rest };
    for (int i = 0; i < n; i++) {
        lv_obj_t *l = wt_lbl(s_scr, line[i], x0, y + i * lh, f,
                             i ? wt_accent() : WT_MUT);
        if (i) lv_obj_add_flag(l, WT_FLAG_ACCENT);
        lv_obj_set_width(l, w);
        lv_obj_set_height(l, lh);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
}

static void pair_screen(void)
{
    swap_screen();
    s_pair_qr = s_pair_tabs = NULL;
    s_pair_tail = s_pair_locked = NULL;
    for (int i = 0; i < PAIR_RUNGS; i++) s_pair_rung[i] = NULL;
    // No subtitle: a full screen QR under PAIR COORDINATOR is its own
    // instruction, and the card below says the rest.
    s_scr = wt_screen(s_parent, tr(STR_I_PAIR_T), NULL);
    wt_chrome_head(s_scr);
    // THE STRIP TAKES THE ROW, and the KEYS trail that used to share it is
    // gone. A page may carry tabs or a breadcrumb on the 30px strip and never
    // both (kiss_theme.h), and the tabs are the more useful of the two here:
    // the title already says PAIR COORDINATOR and BACK already goes to KEYS,
    // so the trail was spending half a row to repeat two things on screen.
    //
    // Desktop against mobile was a pair of 175x60 panes in the content lane,
    // which is the one thing this page could not afford -- it is a decision
    // ABOUT the lane, not a thing in it. On the strip it costs the lane
    // nothing and the accent brackets say which app is open, which is what
    // recolouring an app name was trying to say from inside the content.
    //
    // A screen and a phone: WHERE the app on the other end of this QR runs,
    // which is the one thing the two tabs differ by that a glyph can carry.
    // Both are new codepoints in the generated fonts (tools/fonts/gen_fonts.sh)
    // -- the set had a telephone HANDSET and nothing else device shaped, and a
    // handset is the glyph for placing a call.
    const wt_tab_t ptabs[2] = {
        { WT_ICON_DESKTOP, tr(STR_I_APP_DESKTOP), false, false },
        { WT_ICON_MOBILE,  tr(STR_I_APP_MOBILE),  false, false },
    };
    s_pair_tabs = wt_tabs_flex(s_scr, ptabs, 2, s_pair_fmt, pair_fmt_cb);
    // PUSHED RIGHT, to the end of the lane the [ ? ] divider leaves -- which is
    // where the last tab lands on every strip with enough of them to fill the
    // row. The kit centres a short deck, and centred is right on a page whose
    // content runs the full width. This page is split: the code on the left,
    // the selected app's steps on the right. Centred put "Sparrow" directly
    // over the QR, which belongs to neither tab in particular, and left the
    // column the tab actually governs with nothing above it.
    lv_obj_set_flex_align(s_pair_tabs, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ht = wt_help_tab_n(s_scr, NULL, kiss_terms_unread(KISS_TERMS_PAIR, 3),
                                 pair_terms_cb, NULL);
    // THE WHOLE ROW LIFTS, both halves together. The contract puts the strip at
    // 70, which is 30 tall and ends at 100 -- and this page's QR card starts at
    // 96, because a 300px code at the left margin has to start there to clear
    // the action band. So the strip's last four pixels sat on the white card.
    // Nothing overlapped in the gate's terms, which reads boxes of TEXT, and it
    // looked like the tab was resting on the code.
    //
    // Raised HERE and not in the contract: every other page on the strip has a
    // lane starting at 114 and nothing to clear. The [ ? ] moves with the tabs
    // or the row stops being a row.
    for (lv_obj_t *o = s_pair_tabs; o; o = (o == s_pair_tabs ? ht : NULL))
        lv_obj_set_y(o, PAIR_STRIP_Y);
    // NO NETWORK BADGE. It sat at 672,30 and it is the third place the same
    // word appears on the way here: the home screen wears it, and the KEYS
    // page one step back carries it as the standing line on its action band,
    // pulsing. A fact restated on every screen stops being read on any of
    // them. Worth saying once, because this page is where keys go OUT: the
    // route straight from home when nothing has paired yet skips KEYS, so on
    // that one path the home badge is the only prior sighting.
    // The code, at the size and the place it has always had -- and with the
    // kit's own "+" cue, which is what RECEIVE and both of this file's other
    // QR pages wear. It was swapped for the words TAP TO ENLARGE in the
    // column, and a word is not better than a mark here: it is more to read,
    // and it sits across the rule from the thing it opens while the mark sits
    // beside it. A cue against the bezel is the price of a code at the page
    // margin, and every other QR on the device already pays it.
    wt_qr_card(s_scr, &s_pair_qr, PAIR_QR_X, PAIR_QR_Y, PAIR_QR_CARD,
               PAIR_QR_PX);
    {
        // The rule is VERTICAL, which wt_line_rule is not: that one is a row's
        // 1px underline and it draws itself in left to right. Same ink, same
        // 1px, turned ninety degrees, saying the code and the steps are two
        // halves of one instruction rather than two unrelated blocks.
        lv_obj_t *v = lv_obj_create(s_scr);
        lv_obj_remove_style_all(v);
        lv_obj_set_pos(v, PAIR_RULE_X, PAIR_QR_Y + 8);
        lv_obj_set_size(v, 1, PAIR_QR_CARD - 16);
        lv_obj_set_style_bg_color(v, WT_DIV, 0);
        lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    }

    // ---- steps bay: the path, and the line under it -----------------------
    //
    // NO DESKTOP / MOBILE EYEBROW. It opened this column while the tab above
    // it wore a monitor or a phone and named the app -- the same fact three
    // times before the first instruction. The glyph carries it now, so the
    // column opens on the step the owner actually has to take, and the
    // three steps get the room the caption was holding.

    for (int i = 0; i < PAIR_RUNGS; i++) {
        const int ry = PAIR_RUNG_Y + i * PAIR_RUNG_PITCH;
        lv_obj_t *r = wt_lbl(s_scr, "", PAIR_BAY_X, ry, wt_font23(), WT_INK);
        lv_obj_set_width(r, PAIR_BAY_W);
        lv_obj_set_height(r, lv_font_get_line_height(wt_font23()));
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        s_pair_rung[i] = r;
    }

    // The note's second line: what else works, or what the import is called on
    // the other end. Two lines at the 17px rung, never a fit helper -- a fixed
    // rung says the same thing in every locale and cannot quietly hand this
    // page the smallest type on the device.
    s_pair_tail = wt_lbl(s_scr, "", PAIR_BAY_X, PAIR_TAIL_Y,
                         wt_font_mono18(), WT_MUT);
    lv_obj_set_width(s_pair_tail, PAIR_BAY_W);
    lv_label_set_long_mode(s_pair_tail, LV_LABEL_LONG_DOT);

    // The refusal, over the steps' whole lane. Hidden unless the session
    // cannot export; pair_refresh decides.
    s_pair_locked = wt_note(s_scr, tr(STR_C_LOCKED_B), PAIR_BAY_X,
                            PAIR_RUNG_Y, PAIR_BAY_W,
                            PAIR_QR_Y + PAIR_QR_CARD - PAIR_RUNG_Y);
    lv_obj_add_flag(s_pair_locked, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_t *nx = wt_arrow_action(s_scr, tr(STR_R_NEXT), false, false, WT_ACT_X, WT_ACTION_Y, 0, false, pair_instructions_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, pair_back_cb, NULL);
    pair_band_claim(nx);
    wt_swipe_watch(s_scr, pair_gesture_cb);
    // The silent-payment SCAN KEY used to live HERE, buried one tap inside PAIR
    // COORDINATOR. It is its own export with its own consent warning, and
    // hiding it behind the descriptor flow implied the two were one action.
    // It is a top-level action on the KEYS screen now; see info_screen().
    pair_refresh();
}

// ---- silent-payment SCAN KEY export (BIP-392 sp(spscan...)), warning first ----
// Hands a coordinator the scan PRIVATE key so it can DETECT payments to this
// wallet's silent-payment address. It can never spend. Deliberate two-step
// behind an honest warning, not bundled silently into a wallet import.
// Back to the KEYS screen, not the pair screen. SCAN KEY is launched from
// info_screen(); while it lived inside PAIR COORDINATOR this returned to
// pair_screen(), and leaving it that way would drop the user somewhere they
// never came from -- the kind of navigation bug that reads as the device
// having done something unexpected with a key export.
//
// One launcher means one destination, so this is a plain call again. RECEIVE's
// silent payment tab used to offer the same export and set its own return, and
// the indirection existed only for that.
// The exported key, as it exists on the SCREEN. kiss_session_sp_scan_export
// wipes its own working buffers (wally_bzero, kiss_crypto.c), and this file
// wipes the stack copy the moment the QR and the label have taken theirs --
// but the LABEL then holds the only remaining copy, in a heap block LVGL frees
// without scrubbing when the screen goes. That is the shape of the mnemonic
// left in freed heap by the QR decoder, and this one is a PRIVATE key.
//
// So the label is remembered and its own buffer is zeroed in place before the
// delete, on every way out: DONE, and the idle auto-lock. KEF does exactly
// this for its envelope one page down; the SCAN KEY screen never did.
static lv_obj_t *s_sp_key_lbl;
static lv_obj_t *s_sp_key_qr;

static void sp_key_wipe(void)
{
    if (s_sp_key_lbl) {
        char *t = lv_label_get_text(s_sp_key_lbl);
        if (t) kiss_wipe(t, strlen(t));
        s_sp_key_lbl = NULL;
    }
    // The QR is the same key in the form a camera can read. wt_qr_scrub zeroes
    // the cached payload and the drawn modules both.
    if (s_sp_key_qr) { wt_qr_scrub(s_sp_key_qr); s_sp_key_qr = NULL; }
}

static void sp_key_back_cb(lv_event_t *e)
{
    (void)e;
    sp_key_wipe();
    swap_screen();
    void (*done)(void) = s_scan_key_done;
    s_scan_key_done = NULL;
    if (done) done();
    else info_screen();
}

static void sp_key_show(void *ud)
{
    (void)ud;
    // Both handles belong to the screen that is about to be replaced. Cleared
    // on entry as well as on exit, so no later wipe can reach a deleted object
    // if a future way off this screen forgets to call sp_key_wipe.
    s_sp_key_lbl = NULL;
    s_sp_key_qr = NULL;
    swap_screen();
    // The same trail as the gate in front of this screen: one path, told
    // once. "watch only" is the gate's whole lesson and stays there.
    s_scr = wt_screen(s_parent, tr(STR_R_SP_SCAN_BTN), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_I_T),
                 tr(STR_D_ONLINE_APP));
        wt_trail(s_scr, WT_ICON_SECRET, trail, false);
    }

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
    // 256 bytes of PRIVATE key on the stack. Wiped on both exits below, not
    // left for whatever reuses this frame.
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
        // A refusal can still have written part of a key before it gave up.
        kiss_wipe(key, sizeof key);
        return;
    }

    lv_obj_t *qr = NULL;
    wt_qr_card(s_scr, &qr, 48, 96, 300, 264);
    if (qr)
        wt_qr_update(qr, key, (uint32_t)strlen(key));
    s_sp_key_qr = qr;

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
    s_sp_key_lbl = k;

    // Placed off the key's MEASURED height rather than a y decided in advance:
    // the wrap depends on where LVGL takes its breaks, and a hard 250 is how
    // the old layout ended up with a band of dead glass above it.
    lv_obj_update_layout(k);
    int note_y = 96 + lv_obj_get_height(k) + 16;
    wt_note(s_scr, tr(STR_R_SP_EXPORT_NOTE), 400, note_y, 360,
            WT_CONTENT_BOTTOM - note_y);

    // 592, not WT_BACK_X: 160 wide, so 752-160 is flush.
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, sp_key_back_cb, NULL);

    // The QR and the label hold their own copies now, so the stack one has no
    // reader left. Here rather than at a single exit, because there is none.
    kiss_wipe(key, sizeof key);
}

static void sp_key_warn_cb(lv_event_t *e)
{
    (void)e;
    swap_screen();
    s_scr = wt_chrome(s_parent, tr(STR_R_SP_SCAN_BTN));
    char trail[96];
    snprintf(trail, sizeof trail, "%s / %s", tr(STR_I_T),
             tr(STR_D_ONLINE_APP));
    wt_trail(s_scr, WT_ICON_SECRET, trail, false);

    // The gate shape, amber: sharing the key is a caution the owner can walk
    // back from right up to the hold. The three-row permission model folds
    // into the shape's own two answers -- CANNOT SPEND is what survives,
    // SEES THEM FOREVER is what does not -- and FINDS PAYMENTS is what the
    // paragraph already says the key is for. The passphrase note rides the
    // warn slot: it is the one subtlety worth reading before sharing.
    char para[224];
    snprintf(para, sizeof para, "%s", tr(STR_R_SP_WARN_B));
    char *cut = strstr(para, "\n\n");
    const char *note = NULL;
    if (cut) { *cut = '\0'; note = cut + 2; }
    wt_gate_t g = {
        .mark     = WT_ICON_SECRET,
        .sentence = tr(STR_K_SPGATE_SENT),
        .para     = para,
        .warn     = note,
        .surv = tr(STR_K_SPGATE_SURV),
        .goes = tr(STR_K_SPGATE_GOES),
        .stop     = false,
    };
    wt_gate(s_scr, &g);

    // Revealing a reusable private scan key should not be one stray tap
    // away: the slide's full-width travel is the gate no accidental brush
    // can cross.
    // The ACCENT, not amber. Showing the key is what the screen is FOR, and
    // the gate's amber belongs to the caution line, not to the way forward --
    // the third of the three yellow runs the bench counted on this screen.
    wt_slide_rule(s_scr, tr(STR_W_HOLD_SHOW), tr(STR_G_FW_KEEP_HOLDING),
                  WT_ACT_X, WT_ACTION_Y_SLIDE, 330, sp_key_show, NULL);
    wt_arrow_action(s_scr, tr(STR_C_CANCEL), true, false, 592, WT_ACTION_Y,
                    160, true, sp_key_back_cb, NULL);
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
    // Split once: the grid takes pointers and the labels copy, so the split
    // lives exactly as long as this frame and the wipe below reaches all of
    // it. A BIP39 mnemonic is 24 words at most; clamping says so out loud
    // and keeps a malformed store from inventing pages.
    char wbuf[WSEED_MAX_WORDS][12];
    const char *wp[WSEED_MAX_WORDS];
    int n = 0;
    for (const char *p = words; *p && n < WSEED_MAX_WORDS;) {
        int wl = 0;
        while (p[wl] && p[wl] != ' ' && wl < 11) wl++;
        memcpy(wbuf[n], p, (size_t)wl);
        wbuf[n][wl] = 0;
        wp[n] = wbuf[n];
        n++;
        p += wl;
        while (*p == ' ') p++;
    }

    const int pages = (n + WORDS_PER_PAGE - 1) / WORDS_PER_PAGE;
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    s_words_page = page;
    const int first = page * WORDS_PER_PAGE;
    int on = n - first;
    if (on > WORDS_PER_PAGE) on = WORDS_PER_PAGE;

    swap_screen();
    s_scr = wt_chrome(s_parent, tr(STR_I_WORDS_BTN));

    // The trail answers the question the old fingerprint line answered --
    // WHICH keys these words open -- as the place the owner is standing:
    // a holder with two signers, or a passphrase and a decoy, tells one
    // word list from another by this code. Guarded as every fingerprint is:
    // zero is "no keys open", never a code to copy down.
    {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        char trail[64];
        if (kiss_fp_known(fp))
            snprintf(trail, sizeof trail, "%s / %02X%02X%02X%02X",
                     tr(STR_I_T), fp[0], fp[1], fp[2], fp[3]);
        else
            snprintf(trail, sizeof trail, "%s", tr(STR_I_T));
        lv_obj_t *tl = wt_trail(s_scr, WT_ICON_KEY, trail, false);
        // The trail gives up the lane the sheet dots and the verdict chip
        // stand in -- the gate reads boxes, not ink.
        lv_obj_set_width(tl, 524 - 12 - lv_obj_get_x(tl));
    }
    if (pages > 1) wt_sheet_dots(s_scr, pages, page);

    // The same verdict WRITE THESE DOWN carries. These words came off a
    // stored seed, so their checksum holds by construction -- saying so is
    // what stops a holder wondering whether a word they cannot read is a
    // word gone wrong.
    lv_obj_t *okc = wt_state_chip(s_scr, tr(STR_W_WRITE_OK), WT_OK);
    lv_obj_update_layout(okc);
    lv_obj_set_pos(okc, 752 - lv_obj_get_width(okc),
                   70 + (30 - lv_obj_get_height(okc)) / 2);

    wt_word_grid(s_scr, wp, on, first);
    kiss_wipe(words, sizeof words);
    kiss_wipe(wbuf, sizeof wbuf);

    // The band says the one thing that outranks navigation on this screen,
    // and the pulsing amber dot is what earns the interruption.
    wt_standing(s_scr, tr(STR_W_GRID_WARN), WT_WARN, true);

    // Forward only, and DONE only on the last sheet: leaving is a decision,
    // and it happens once the owner has seen every word. STR_R_NEXT is the
    // receive flow's page-forward label, already in 21 locales.
    if (page < pages - 1)
        wt_arrow_action(s_scr, tr(STR_R_NEXT), false, true, 592, WT_ACTION_Y,
                        160, true, words_page_cb, (void *)(intptr_t)1);
    else
        wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y,
                        160, true, words_back_cb, NULL);
}

static void words_page(void);

// The WT_WARN gate in front of the reveal (shape 4): words on screen is a
// caution an owner can still walk back from, so it is amber, and the hold is
// what makes a pocket press incapable of putting a seed on the glass.
static void words_reveal(void *ud)
{
    (void)ud;
    words_render_page(0);
}

static void words_gate_cancel_cb(lv_event_t *e)
{
    (void)e;
    words_page();
}

static void words_gate_screen(void)
{
    swap_screen();
    s_scr = wt_chrome(s_parent, tr(STR_I_WORDS_BTN));
    char trail[96];
    snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
             tr(STR_I_WTAB_PAPER));
    wt_trail(s_scr, WT_ICON_KEY, trail, false);
    wt_gate_t g = {
        .mark     = LV_SYMBOL_EYE_OPEN,
        .sentence = tr(STR_W_SHOW_SENT),
        // NO PARAGRAPH. "Your seed words appear on this screen." says what is
        // about to happen and the two facts below say what it costs and what
        // it does not; there was nothing left for a third line to add, so it
        // was carrying the COPYING instruction from the setup grid -- an
        // answer to a question this screen does not ask.
        .para     = NULL,
        .surv = tr(STR_W_SHOW_SURV),
        .goes = tr(STR_W_SHOW_GOES),
        .stop     = false,
    };
    wt_gate(s_scr, &g);
    wt_slide_rule_c(s_scr, tr(STR_W_HOLD_SHOW), tr(STR_G_FW_KEEP_HOLDING),
                    // The ACCENT. Amber is a mark colour, and a slide label
                    // is a word the owner reads. WT_STOP still carries the one
                    // gate that cannot be undone.
                    WT_ACT_X, WT_ACTION_Y_SLIDE, 330, wt_accent(), wt_accent(),
                    words_reveal, NULL);
    wt_arrow_action(s_scr, tr(STR_C_CANCEL), true, false, 592, WT_ACTION_Y,
                    160, true, words_gate_cancel_cb, NULL);
}

static void words_show_cb(lv_event_t *e)
{
    (void)e;
    words_gate_screen();
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
    //
    // The leg EXISTS here now: kiss_setup_open_verify is opened with
    // with_pass, so a wallet with a passphrase is asked for it and has to
    // rederive the fingerprint before this route claims anything. So the
    // condition is "the words were the whole backup, OR the passphrase proved
    // it too" -- and the half check still cannot turn the chip green.
    if (kiss_setup_verify_succeeded() &&
        (kiss_setup_verify_full() ||
         kiss_rehearse_after_words(kiss_session_decoy()) ==
             KISS_REHEARSE_VERIFIED)) {
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
    kiss_setup_open_verify(s_parent, winfo_after_verify, true);
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
    wt_row_wide(w_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label   = tr(STR_I_WROW_CHECK),
        .sub     = wsub,
        .sub_col = ok ? WT_OK : WT_WARN,
        .kind    = WT_WIDE_OPEN,
        .val     = wval,
        .vcol    = ok ? WT_OK : WT_WARN,
        .sev     = ok ? WT_SEV_OK : WT_SEV_WARN,
        .cb      = verify_copy_cb,
    });

    // CHECK MY COPY FIRST. It was second, under SHOW THEM, and the order is
    // the page's advice: the row above is the only one here that changes
    // anything, it is what turns the amber state green, and it is what an
    // owner holding a page of words should do before asking for them again.
    // Putting the reveal first offers the glass as the answer to "have I got
    // these right", which it is not -- reading them off the screen proves
    // nothing about the paper.
    //
    // NO SUB on the reveal. It said "paper only. no photo, no file." and the
    // gate one tap later says the same instruction in full, which is the
    // restatement this page's body was already cut for once.
    wt_row_wide(w_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label = tr(STR_I_WROW_SHOW),
        .kind  = WT_WIDE_OPEN,
        .cb    = words_show_cb,
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

    // No group note. It read "Only a check here proves your paper is right",
    // and it is not true in the way it sounds: the check compares WORDS, and
    // the fingerprint the page was framing underneath proves nothing about a
    // page of them. The bench said so directly -- "no one is checking
    // fingerprint to prove paper is right, they are checking seed words".
    (void)rows;

    // WHICH keys, in the band the two rows leave under them. Every row in this
    // group is about a set of keys and none of them says whose; the
    // fingerprint is the only thing an owner can hold against the paper
    // already in their hand, and it is what the words on the next screen are
    // CALLED. On the value-card idiom, so the same eight characters sit where
    // they sit on the fingerprint reveal and the pairing screen, and on
    // STR_L_FP_CAP, which those screens already ship in 21 locales.
    //
    // NO FINGERPRINT CARD. It framed eight characters under a page whose
    // whole subject is a list of words, and it answered a question nobody
    // asks here: the check on this page compares WORDS against paper, and the
    // fingerprint proves nothing about a page of them. It stays where it
    // does work -- the reveal, the pairing screen and the restore verdict.
    (void)rows;
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
    // ONE sub, and it no longer names the file. It read "one QR, or
    // 9A2C33E3.kef" once the fingerprint was known -- a filename extension a
    // first-week owner has never met, wrapped around eight hex characters that
    // read as noise beside it, and the bench said so. What the row gives you is
    // a QR you can print or a file, which are both things this device has
    // already shown them; the fingerprint stays on the screens where it is
    // the subject, and WHERE the file goes is said on the screen that writes
    // it. Naming the card here as well put the line past both the SLACK lane
    // and the 3.5in board's whole content width.
    //
    // The branch went with it: there is nothing left for the fingerprint to
    // change, so there is nothing to branch on.
    const char *ksub = tr(STR_I_ROW_KEF_SUB);

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
    // THE ANSWER, not the question. This read I_KEF_PP_H -- "what is in
    // it" -- which is a HEADING: it and I_KEF_W2_H ("if you lose it") are
    // the two heads of a block whose bodies were deleted, and both were left
    // wired up. So the row an owner opens to find out what is in the envelope
    // asked them the question back, and did it only on a signer WITH a
    // passphrase, which is the case where the answer matters.
    //
    // I_KEF_WARN_S_PP is that answer, already written and already in 21
    // locales -- it was sitting on the orphan backlog. The fact it carries is
    // the one that bites: this QR holds the words and NOT the passphrase, so
    // on its own it rebuilds different keys.
    wt_row_wide(w_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label   = tr(STR_I_WROW_HOLDS),
        .sub     = tr(pp ? STR_I_KEF_WARN_S_PP : STR_I_KEF_WARN_S),
        .sub_col = pp ? WT_WARN : WT_MUT,
        .kind    = WT_WIDE_OPEN,
    });

    // ...and the other head goes the same way. "if you lose it" ended the tab
    // on a dangling fragment; what it was reaching for is what opens the
    // thing, which is the one fact the two rows above do not carry.
    wt_group_note(w_pane, 2, tr_sym(WT_ICON_LOCK, STR_I_KEF_SHOW_S));

    // No fingerprint card here either. The row above already names the file
    // by its fingerprint ("one QR, or 9A2C33E3.kef"), so the card was the
    // same eight characters twice on one screen -- and the bench asked
    // outright whether it was needed. It is not.
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

// The stroke, on the RECOVERY WORDS page: two tabs, one deck, no pages and
// no overlay to guard.
static void words_gesture_cb(lv_event_t *e)
{
    const int step = wt_swipe_step(e);
    if (!step) return;
    const int to = w_tab + step;
    if (to < 0 || to >= WTAB_N) return;   // the deck ends where the strip does
    wt_pane_go(&s_wctx, to, false, wtab_build);
}

static void words_page(void)
{
    swap_screen();
    s_wctx.pane = s_wctx.pane_out = s_wctx.tabs = NULL;
    s_wctx.entering = false;
    if (w_tab < 0 || w_tab >= WTAB_N) w_tab = WTAB_PAPER;

    // The title keeps STR_I_WORDS_BTN: check_screen_coverage.py tracks a page
    // by its title's string id, and a literal one silently drops out of the
    // count.
    s_scr = s_wctx.scr = wt_chrome(s_parent, tr(STR_I_WORDS_BTN));

    const wt_tab_t tabs[WTAB_N] = {
        { WT_ICON_SECRET, tr(STR_I_WTAB_PAPER),
          !kiss_ui_backup_checked(), false },
        { WT_ICON_LOCK,   tr(STR_I_WTAB_ENC),   false, false },
    };
    s_wctx.select = wt_tabs_flex_select;
    s_wctx.tabs = wt_tabs_flex(s_scr, tabs, WTAB_N, w_tab, wtab_cb);
    wt_pane_tabs_watch(&s_wctx);
    wt_swipe_watch(s_scr, words_gesture_cb);

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
    // Declared font14: a state CHIP is a mark by kit definition, and this one
    // reports a result the screen has already acted on. The one chip on the
    // device that is lifted is the touch-dead banner, because there it is the
    // only thing on screen and it is an instruction.
    wt_state_chip_set(s_kef_sd_chip,
                      tr_sym(ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                             ok ? STR_S_SAVED_NOTE : STR_S_FAIL_SD_WRITE),
                      ok ? WT_OK : WT_STOP);
}

static void kef_show_screen(void)
{
    swap_screen();
    // No subtitle: the note beside the QR already says only the password
    // opens it.
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_KEF), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_I_WTAB_ENC));
        wt_trail(s_scr, WT_ICON_LOCK, trail, false);
    }
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

    lv_obj_t *sd = wt_word_action(s_scr, WT_ICON_SD, tr(STR_I_KEF_SD_BTN),
                                  true, WT_INK, false, kef_sd_cb, NULL);
    lv_obj_set_pos(sd, WT_ACT_X, WT_ACTION_Y + 6);
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
    // No subtitle: the words-vs-keys distinction the pp variant carried is
    // exactly what the diagram's left chip draws two lines below.
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_KEF), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_I_WTAB_ENC));
        wt_trail(s_scr, WT_ICON_LOCK, trail, false);
    }

    // THE EXPLAINER SHAPE, the same one every other teaching screen on this
    // device wears since the pass that rebuilt them: a headline that makes
    // the claim, a paragraph under it, and labelled facts on a caption lane.
    // This screen was the last one still built out of a diagram card and a
    // two column claim pair, and beside SEED WORDS or PASSPHRASE it read as
    // a different product.
    //
    // The diagram went with it. It drew "words plus a password becomes a
    // locked QR", which is what the headline now says in words -- and a
    // drawing of a sentence already on the screen is decoration.
    //
    // TWO facts, not three. The band is 344 here because of the slide, so the
    // lane ends at 336: a third row lands at 338.
    const wt_fact_t facts[2] = {
        { .cap = tr(pp ? STR_I_KEF_F1_C : STR_I_KEF_F1_C_NP),
          .val = tr(pp ? STR_I_KEF_F1_V : STR_I_KEF_F1_V_NP),
          .icon = pp ? WT_ICON_SECRET : WT_ICON_LOCK },
        { .cap = tr(STR_G_TECHNICAL), .val = tr(STR_I_KEF_TERM),
          .icon = LV_SYMBOL_LIST },
    };
    // With a passphrase the paragraph has one more thing to say and it is the
    // one that matters: the envelope holds the WORDS, and the words alone are
    // not these keys. The passphrase is wiped at login by design
    // (kiss_crypto.h), so it is not in there and no future version can
    // quietly put it there.
    // WT_ACTION_Y_SLIDE, not the default bottom: the band is 344 here
    // because of the slide, so an explainer hanging from WT_CONTENT_BOTTOM
    // would put its second fact behind it.
    wt_explain_to(s_scr, tr(STR_I_KEF_HEAD),
                  tr(pp ? STR_I_KEF_EXP_B : STR_I_KEF_EXP_B_NP), facts, 2,
                  WT_ACTION_Y_SLIDE);

    // Making the envelope puts the keys on the glass as a QR one screen
    // later, so the entry is a deliberate slide, the scan-key precedent.
    wt_slide_rule(s_scr, tr(STR_I_KEF_MAKE_BTN), tr(STR_G_FW_KEEP_HOLDING),
                  WT_ACT_X, WT_ACTION_Y_SLIDE, 330, kef_make, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, kef_finish_cb, NULL);
}

// ---- the section home: facts + actions ----
static void pair_open_cb(lv_event_t *e)  { (void)e; pair_screen(); }

// The four fact rows and the two coordinator rows all open something, and a row
// carries its key the way a help chip used to.

// RECOVERY WORDS is a different page with a different tab count, and a context
// remembers WHICH tab is open -- one shared with s_wctx would land an owner
// back from the words page on whichever KEYS tab matched the index.
static void info_tab_build(void);

// The [ ? ] tab, open: the content lane replaced by the page's explainer.
// Cleared by any real tab tap, so the strip is also the way back.
static bool s_help_open;

static void info_help_cb(lv_event_t *e)
{
    (void)e;
    s_help_open = !s_help_open;
    // wt_pane_go refuses a same-tab call, so this is its swap by hand: stop
    // whatever is mid-flight, send the old group out, build the new one in.
    // [ ? ] never highlights, but the strip releases the tab behind it.
    const bool was_moving = s_ictx.entering;
    wt_pane_stop(&s_ictx);
    if (was_moving && s_ictx.pane) {
        lv_obj_delete(s_ictx.pane);
        s_ictx.pane = NULL;
    }
    s_ictx.pane_out = s_ictx.pane;
    s_ictx.pane = wt_pane_new(&s_ictx);
    info_tab_build();
    wt_accent_restyle(s_ictx.pane);
    const int dir = s_help_open ? 1 : -1;
    wt_pane_enter(&s_ictx, dir, false);
    wt_pane_exit(&s_ictx, dir);
}

// The stroke, on the KEYS page: one page and its [ ? ]. The explainer stays a
// toggle rather than a position on a deck, and the stroke reaches it -- left
// opens it, right comes back. SETTINGS shipped this first, from the bench's
// "i cant swipe to the question mark".
static void info_gesture_cb(lv_event_t *e)
{
    const int step = wt_swipe_step(e);
    if (!step) return;
    if (s_help_open) {
        if (step < 0) info_help_cb(NULL);
        return;
    }
    // One page, so the deck is the page and the [ ? ]: a left stroke opens the
    // explainer, and there is nowhere to the right of it to go.
    if (step > 0) info_help_cb(NULL);
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
// and where it takes you". They are one shape now, on a 704 wide lane, so an
// address fits at mono23 and a path fits beside its own type.
static void info_tab_build(void)
{
    lv_obj_t *p = s_ictx.pane;
    const int X = 48, W = 704;

    if (s_help_open) {
        // The [ ? ] content: the lane replaced, not a card and not an
        // overlay. Nothing on it is interactive; the strip is the way back.
        // The three ROWS of this page, in the order the page shows them.
        // They used to be three unrelated nouns under a headline about a
        // fourth thing -- and the middle one, ACCOUNT / "network and address
        // style", is not what an account is. The bench read the page and could
        // not say what it was explaining, which is the whole report.
        wt_fact_t facts[3] = {
            { .cap = tr(STR_K_HELP_F1C), .val = tr(STR_K_HELP_F1V),
              .icon = WT_ICON_KEY },
            { .cap = tr(STR_K_HELP_F3C), .val = tr(STR_K_HELP_F3V),
              .icon = WT_ICON_QR },
            { .cap = tr(STR_I_SEC_FIRST), .val = tr(STR_S_CMP_8),
              .icon = LV_SYMBOL_EYE_OPEN },
        };
        wt_explain(p, tr(STR_K_HELP_HEAD), tr(STR_K_HELP_BODY), facts, 3);
        return;
    }

    const int H = 76;

    // NO EMPTY STATE. An unpaired signer never reaches this page: it opened
    // on a headline naming the absence, a sentence saying what pairing would
    // give, and one row that went to the pairing screen -- and the sentence
    // was the same claim as this page's own [ ? ], one tap away. Three taps
    // to reach the QR, two of them spent reading a screen that only said
    // "the thing you came for is through here".
    //
    // kiss_info_open() forwards straight to pair_screen() instead; see the
    // note there for why BACK from it has to leave for HOME.

    wt_line_row_stage(wt_line_row(p, X, 120, W, H, tr(STR_K_CAP_PAIRING),
                                  tr(STR_I_PAIR_T), wt_font28(), WT_INK,
                                  tr(STR_K_PAIR_SUB), NULL,
                                  pair_open_cb, NULL), 0);
    wt_line_rule_draw(wt_line_rule(p, X, 120 + H, W), 110, 320);
    // "Scan" elsewhere on this device means the camera. Here it means searching
    // the chain, and the caption above the value is what says which.
    //
    // The export's strings are R_ (RECEIVE) keys -- STR_R_SP_SCAN_BTN and the
    // warn/reveal set with it -- because the row it replaced lived on RECEIVE's
    // silent payment tab. This is the only launcher now and the screens are
    // owned here. The prefix is historical; there is no RECEIVE screen to go
    // looking for, and renaming a key is 21 locale files for an internal name.
    wt_line_row_stage(wt_line_row(p, X, 196, W, H, tr(STR_R_SP_BTN),
                                  tr(STR_R_SP_SCAN_BTN), wt_font28(), WT_INK,
                                  tr(STR_K_SP_SUB), NULL,
                                  sp_key_warn_cb, NULL), 1);
    wt_line_rule_draw(wt_line_rule(p, X, 196 + H, W), 152, 320);

    lv_obj_t *ar = wt_line_row(p, X, 272, W, H, tr(STR_I_SEC_FIRST), NULL,
                   NULL, WT_INK, tr(STR_S_CMP_8), NULL,
                   first_addr_go_cb, NULL);
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
    // Not a remembered state: leaving the page with [ ? ] open must not
    // re-land the next visit on the explainer under a strip whose brackets
    // claim a section is selected.
    s_help_open = false;
    s_scr = wt_chrome(s_parent, tr(STR_I_T));

    // NO TAB STRIP. THIS SIGNER held three rows and every one of them was a
    // fact with a home somewhere else: NETWORK and ADDRESS TYPE are changed on
    // SETTINGS > SIGNER, and the network is already this page's own standing
    // line; FIRST ADDRESS is what RECEIVE opens on. A tab whose whole content
    // is a read-only copy of another page is a tab an owner has to check twice.
    //
    // Desktop against mobile does not want tabs either, and could not use
    // them: it is a switch INSIDE pair_screen because it changes the QR
    // PAYLOAD -- a descriptor for Sparrow, a different export for BlueWallet
    // -- and a strip up here would only take the reader further from it.
    //
    // What is left is one page about one thing: how a coordinator comes to
    // watch these keys, and how you prove it worked.
    s_ictx.scr    = s_scr;
    s_ictx.tab    = 0;
    s_ictx.tabs   = NULL;
    wt_swipe_watch(s_scr, info_gesture_cb);

    // The band's left lane holds ONE line, by rank: the test network caution
    // beats everything, the first-run hint speaks until [ ? ] has been opened
    // once, and the standing statement -- what is permanently true of this
    // page -- is what the lane says for the rest of the device's life.
    const char *hint = NULL;
    if (kiss_testnet()) {
        // Through the kit like its two siblings, not the hand-built font14
        // pair this once was -- the smallest type on the device, on the one
        // line whose whole job is to be seen. The dot breathes: the caution
        // earns it.
        //
        // The CHAIN'S OWN NAME, not a description of it. This said "TEST
        // NETWORK" while the home badge said TESTNET, the sign chip said
        // "TESTNET, practice coins" and the Settings row said TESTNET over
        // "not real bitcoin" -- four phrasings of one fact, and this one was
        // wrong on signet as well, since the same string covered both chains.
        // kiss_net_name() is what the home badge and the Settings row already
        // print, so there is one name and it is the chain's.
        wt_standing(s_scr, kiss_net_name(), WT_WARN, true);
    } else if (!wt_help_seen()) {
        hint = tr(STR_C_HELP_HINT);
    } else {
        wt_standing(s_scr, tr(STR_K_STANDING), WT_DIM, false);
    }
    wt_help_tab(s_scr, hint, info_help_cb, NULL);
    // AFTER the tab, and that is not a style choice: the trail's box runs to
    // 752 unless something is already sitting there, so built first it prints
    // straight through the [ ? ].
    //
    // The strip row keeps a word. The tab this page lost was called
    // COORDINATOR and that was the one thing on it worth saying: the page is
    // about the conversation with a coordinator, not about the keys as an
    // object. A trail is where a page with no tabs says so.
    wt_trail(s_scr, WT_ICON_LINK, tr(STR_D_ONLINE_APP), false);

    s_ictx.pane = wt_pane_new(&s_ictx);
    info_tab_build();

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
}

void kiss_info_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = NULL;
    s_scan_key_done = NULL;
    s_pair_fmt = 0;
    // A fresh entry lands on tab 1. The context keeps its tab across a screen
    // rebuild on purpose -- that is what returns an owner to the tab they left
    // when a row's screen goes BACK -- so entering the page has to say so.
    s_ictx.tab = 0;
    // STRAIGHT TO THE QR while nothing has paired yet. The KEYS page in that
    // state was one row and two lines saying the row was there, and the lines
    // repeated the page's own [ ? ]. What an owner opening KEYS on a signer
    // with no coordinator wants is the code and the choice of app, which is
    // the screen behind that row.
    //
    // Nothing is stranded by skipping it: SCAN KEY has its second door on
    // RECEIVE's SILENT tab (kiss_info_open_scan_key), FIRST ADDRESS is what
    // RECEIVE opens on, and the pair screen carries its own [ ? ].
    s_pair_entry = !coord_known();
    if (s_pair_entry) { pair_screen(); return; }
    info_screen();
}

void kiss_info_open_words(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    s_parent = parent;
    s_words_done = done_cb;
    words_warn_screen(NULL);
}

void kiss_info_open_scan_key(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    s_parent = parent;
    s_scan_key_done = done_cb;
    sp_key_warn_cb(NULL);
}
