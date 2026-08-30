// Step 4: Receive. The address is derived ON THIS DEVICE from the session key
// (never trusted from a computer — that's the anti-phishing point), shown as
// text and a static QR. VERIFY scans an address QR from the coordinator's
// screen and answers the only question that matters: is this one of MINE —
// the defense against malware swapping the receive address on the computer.
// Compiled in BOTH device and sim builds; the sim stubs kiss_session_* in sim_main.c.
#include "kiss_recv.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "kiss_crypto.h"
#include "kiss_scan.h"
#include "kiss_info.h"   // the one "?" card implementation lives there
#include "kiss_theme.h"
#include "kiss_ui.h"      // kiss_ui_last_fp: keys the reuse guard per wallet
#include "kiss_usage.h"   // highest receive index this wallet has used

#define VFY_SCAN_DEPTH 100   // bounded, honest ownership search per chain

// How many addresses a page shows. Three whole line rows fill the lane, and a
// swipe turns the page -- the deck idiom the SIGN file list shipped. A page
// costs three child derivations at open, well inside what this screen already
// does elsewhere: VERIFY's ownership search runs up to VFY_SCAN_DEPTH on BOTH
// chains (200) in one go and has always been fine.
#define RECV_PAGE 3
// Hard ceiling on how far the list will go. A signer with no chain view cannot
// know which addresses were ever used, so an endless list is an endless
// invitation to derive addresses nothing will ever pay to. A hundred is more
// than a personal wallet gets through, and the counter states it out loud
// rather than letting the list just stop.
#define RECV_LIST_CAP 100
// List rows are CARDS now, like every row on Settings. The height stays 48: a
// row holds ONE mono28 line, where a settings row holds a label and a sub, so 48
// is the same internal padding at half the content. The 6px gap is what a card
// needs and a hairline list did not.
//
// 54 divides 324, the viewport height, exactly six times, and that is not a
// coincidence: with LV_SCROLL_SNAP_START the list can only come to rest on a
// multiple of the pitch, so six whole rows are visible and the seventh begins
// exactly ON the bottom edge. Nothing is ever half a row. The old hairline list
// had no such property and stopped wherever the throw died, which is why the
// overlap gate found the bottom row clipped through its own text.
#define ROW_H 48
#define ROW_GAP 6
#define ROW_PITCH (ROW_H + ROW_GAP)

// The detail screen's right column: the address in a card, because that is what
// the rest of the device does with a value worth reading off the glass.
#define RECV_CARD_X 310
// Measured off a rendered frame rather than reasoned about. The state chip beside
// the eyebrow is a font14 label with pad_ver 5 and a 1px border, so it is 30
// tall; at y=96 it owns rows 96..125 inclusive, and this card at 136 leaves ten
// pixels of page between them. It was 134 against a chip at 104 once -- top
// border on bottom border, no background between -- and two rounded boxes
// touching read on glass as one clipping the other, which is what came back off
// the device.
//
// The whole band moved up 8 to make that clearance a THIRD time, at the other
// end: two 64px destination rows now sit under this card, and at the old y the
// second one's bottom border landed on the action bar's hairline. Same defect,
// same fix, and the chip has never needed the space it had above it.
#define RECV_CARD_Y 136
#define RECV_CARD_W 442
// Fixed at the height the EXPANDED address needs, with the block centred inside
// it, so the card is the thing that does not move: NEXT ADDRESS stays under the
// same thumb whether the address is folded or not. Folding was the whole point
// of the change and a fold that shoves the button up gives it back.
//
// 114 = two lines of mono23 (58), 6, the compare caption (18), and 16 of padding
// top and bottom. The grouped form of a 42 character bech32 is 52 characters and
// takes exactly two lines in RECV_CARD_W - 28.
#define RECV_CARD_H 114
// mono23, where the list rows use mono28, and the difference is arithmetic
// rather than taste: the folded form is 28 characters whatever the address, which
// is about 500px at mono28, and this card has 414 to give beside a 238px QR. At
// mono28 the final lit block fell off the card's right edge, which on the screen
// whose whole job is showing you an address is the one thing that must not
// happen. A list row is 690 wide and keeps the bigger face.
#define RECV_ADDR_FONT wt_font_mono23()

// The silent-payment screen's right column, beside its 304px QR card. 356..752
// for the card, 366 for the content, and SP_COL_W is what is left after a 14px
// right margin: the folded address is about 28 characters at mono28 (~406px on
// testnet, whose prefix is a character longer), so it takes two lines here and
// one in a 690px list row. It used to take one line and run off the panel.
#define SP_CARD_X 356
#define SP_CARD_W 396
#define SP_COL_X  366
#define SP_COL_W  372

// The suffix under a derivation path: "on TESTNET" / "on SIGNET", and nothing
// at all on mainnet, where the path already says 0h and the absence is the
// statement. The two test networks derive identically -- this names which one
// the owner picked, so a tb1 address is not read as the other chain's.
static const char *on_net_line(void)
{
    if (!kiss_testnet()) return "";
    return kiss_network() == KISS_NET_SIGNET ? tr(STR_R_ON_SIGNET)
                                             : tr(STR_R_ON_TESTNET);
}

static lv_obj_t *s_scr;                    // whichever receive-flow screen is up
static lv_obj_t *s_parent;
// Where the derivation path block starts on the detail screen: caption at this
// y, its "?" chip centred on it, the path 22 below. The address above now ends
// at 198 (two lines of font23 from 140) and the privacy reminder below starts
// at 308, so the block owns 230..287 with air on both sides.
#define RECV_PATH_Y 236

static lv_obj_t *s_qr, *s_addr_sg, *s_idx_lbl, *s_path_lbl, *s_path_tn_lbl;
static lv_obj_t *s_state_chip, *s_chain_lbl;
// The card the address lives in on the detail screen, and the caption inside it.
// recv_refresh rebuilds the spans on every NEXT, so both have to outlive one
// refresh: the spans are children of the card and are placed against the
// caption, which is a child of the card too.
static lv_obj_t *s_addr_card, *s_cmp_lbl;
static lv_obj_t *s_addr_more;   // "FULL ADDRESS" / "SHORT", the fold's own label
static lv_obj_t *s_sp_path_lbl, *s_sp_path_sec, *s_sp_toggle_act;
// The card behind the silent-payment address and its path. Sized by
// sp_addr_render, because the folded and full views are wildly different
// heights and the box has to be the shape of whichever one is up.
static lv_obj_t *s_sp_card;
static lv_obj_t *s_sp_addr_hit;
static uint32_t s_idx;
static uint32_t s_list_base;               // first index the list shows
// The highest address used or shown seeds the next fresh landing.
// s_seen_key detects a wallet/network/type switch.
static int s_seen_high = -1;
static char s_seen_key[16];
static bool s_sp_full;                     // silent-payment text is folded by default
static char s_sp_addr[128];
static lv_obj_t *s_lock_note;              // the locked reassurance in the QR's space
// Declared up here because close_cb nulls every one of them: the lamp pair,
// the selector's chevron, the popover and its scrim, the NEXT action, and
// the address fold's tap target all die with the screen.
static lv_obj_t *s_lamp_dot, *s_lamp_lbl, *s_idx_chev;
static lv_obj_t *s_next_act;          // NEXT ADDRESS: only tab 0 may show it
static lv_obj_t *s_pop;               // the index popover, or NULL
// Its tap-away catcher, which is a SIBLING and therefore does not die with it.
// Held here because a catcher that outlives the popover covers the whole pane
// and silently eats every tap on the screen behind it.
static lv_obj_t *s_pop_away;
// Whether tab 0 shows the address grouped and whole instead of folded. A view,
// not a remembered state: reset on every open. This is the ONLY place on the
// device a full receive address renders as text -- everything else folds and
// points here.
static bool s_addr_full;
static lv_obj_t *s_addr_hit;          // the tap target over the address

bool kiss_recv_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e) {
  (void)e;
  // The detail-only five, which recv_list_open() already nulls and this did
  // not. Two of them are read UNGUARDED -- wt_qr_refusal(s_qr, ...) and
  // lv_label_set_text_fmt(s_idx_lbl, ...) in recv_refresh() -- so a stale
  // pointer here is a dereference and not a skipped branch. Only that one
  // static function reads them and only from callbacks on the screen being
  // deleted, which is exactly what was true of Settings' pane until something
  // reachable from another screen read it.
  s_qr = s_idx_lbl = s_path_lbl = s_path_tn_lbl = s_lock_note = NULL;
  s_addr_sg = NULL;
  s_sp_path_lbl = s_sp_path_sec = NULL;
  s_sp_toggle_act = s_sp_addr_hit = NULL;
  s_sp_card = NULL;
  s_state_chip = s_chain_lbl = NULL;
  s_addr_card = s_cmp_lbl = s_addr_more = NULL;
  s_next_act = s_addr_hit = NULL;
  // The popover and its scrim die with the screen; the idle auto-lock used to
  // leave both statics pointing at the freed pair until the next open.
  s_pop = s_pop_away = NULL;
  s_idx_chev = s_lamp_dot = s_lamp_lbl = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void kiss_recv_close(void) { close_cb(NULL); }   // idle auto-lock path

// ---- VERIFY: scan an address QR, answer "is this mine?" ----
// bitcoin: URI wrapper off, params off, uppercase bech32 (QR alphanumeric
// mode) folded to lowercase. Base58 stays untouched (it is case-sensitive).
static void vfy_norm(const char *in, char *out, size_t cap) {
  if ((in[0] == 'b' || in[0] == 'B') && strlen(in) > 8) {
    const char *scheme = "bitcoin:";
    int m = 1;
    for (int i = 0; i < 8; i++) {
      char c = in[i] >= 'A' && in[i] <= 'Z' ? in[i] + 32 : in[i];
      if (c != scheme[i]) { m = 0; break; }
    }
    if (m) in += 8;
  }
  size_t o = 0;
  for (; *in && *in != '?' && o + 1 < cap; in++)
    out[o++] = *in;
  out[o] = 0;
  // bech32 families we can be shown: segwit (bc1/tb1) and silent payments
  // (sp1/tsp1, ~117 chars). QR alphanumeric mode is uppercase, so fold those.
  static const char *const HRP[] = { "bc1", "tb1", "sp1", "tsp1" };
  int b32 = 0;
  for (size_t k = 0; !b32 && k < sizeof HRP / sizeof HRP[0]; k++) {
    size_t n = strlen(HRP[k]), i = 0;
    if (o <= n) continue;
    for (; i < n; i++) {
      char c = out[i] >= 'A' && out[i] <= 'Z' ? out[i] + 32 : out[i];
      if (c != HRP[k][i]) break;
    }
    b32 = (i == n);
  }
  if (b32)
    for (size_t i = 0; i < o; i++)
      if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
}

static int vfy_find(const char *addr, int *change, uint32_t *idx) {
  char mine[91];
  for (int c = 0; c < 2; c++)
    for (uint32_t i = 0; i < VFY_SCAN_DEPTH; i++) {
      if (kiss_session_address(c, i, mine, sizeof mine) != 0)
        return 0;
      if (strcmp(mine, addr) == 0) { *change = c; *idx = i; return 1; }
    }
  return 0;
}

static void vfy_scan(lv_event_t *e);

static void vfy_done_cb(lv_event_t *e) {
  (void)e;
  close_cb(NULL);
  kiss_recv_open(s_parent);
}

// Is this our own silent-payment address? It is not on any bc1/tb1 chain, so
// vfy_find can never match it: compare against the one we derive ourselves.
static int vfy_is_sp_mine(const char *addr) {
  char mine[128];
  if (kiss_session_sp_address(mine, sizeof mine) != 0)
    return 0;
  return strcmp(mine, addr) == 0;
}

static void vfy_result(const char *txt, size_t len) {
  // must hold a silent-payment address (~117 chars) whole: a truncated address
  // silently becomes a DIFFERENT address, which is the one thing this screen
  // exists to rule out. grouped adds a space every 4 chars.
  char addr[128], grouped[200], buf[200];  // translated line, 3 bytes/char worst

  // A coordinator's usage payload rides in the same QR as the address it is
  // about, so this runs on the RAW scanned text, ahead of vfy_norm. Anything
  // without the magic falls straight through and the address path below is
  // exactly what it was.
  //
  // The index in the payload says NOTHING about this address. It is a fact
  // about the wallet, and vfy_find goes on re-deriving and searching for the
  // address itself: the ownership answer is what this screen exists for, and
  // nothing that arrived through a camera gets to shortcut it.
  kiss_usage_msg_t um;
  int have_um = kiss_usage_parse(txt, len, &um) == 0;
  int um_recorded = 0, um_view = 0;
  if (have_um) {
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    int um_mine = memcmp(fp, um.fp, 4) == 0;
    // Stored in the bucket the payload NAMES, which need not be the one on
    // screen. A different purpose is a different account key, so a coordinator
    // on another address type really is watching other keys -- and reporting an
    // update over a chip that did not move is the more confusing answer.
    um_view = um_mine && um.testnet == (kiss_testnet() ? 1 : 0) &&
              um.script == kiss_script();
    if (um_mine)
      um_recorded = kiss_usage_chain_set(um.fp, um.testnet, um.script,
                                         um.high, um.height);
    txt = um.addr;                       // the address half, for the check below
  }

  vfy_norm(txt, addr, sizeof addr);
  int change = 0;
  uint32_t idx = 0;
  int mine = vfy_find(addr, &change, &idx);
  int sp_mine = !mine && vfy_is_sp_mine(addr);
  int validity = (mine || sp_mine) ? WADDR_CURRENT_NETWORK
                                   : kiss_address_validate(addr);

  s_scr = wt_screen(s_parent, tr(STR_R_VT), tr(STR_R_VS));
  wt_chrome_head(s_scr);
  // Only format and tail-highlight something that really is an address.
  // Arbitrary QR text is not grouped address data; feeding a short malformed
  // string through the span formatter also left LVGL with a broken short-span
  // layout on the error screen.
  // The address gets a CARD. It is the figure the whole screen is about, and
  // it was a bare label under a bare paragraph -- which the BARE gate started
  // reporting the moment the explanation below stopped being font14 and became
  // a paragraph the check can see. Rule 1: something framed, above the action
  // row, and a headline does not count.
  //
  // Sized to the address after it is laid out, because a grouped silent
  // payment address is three lines where a bech32 one is two.
  lv_obj_t *acard = wt_card(s_scr, 24, 170, 752, 0);

  lv_obj_t *shown;
  if (validity == WADDR_INVALID) {
    shown = wt_lbl(s_scr, addr, 48, 186, wt_font_mono28(), WT_MUT);
    lv_obj_set_width(shown, 700);
    lv_label_set_long_mode(shown, LV_LABEL_LONG_WRAP);
  } else {
    wt_group4(addr, grouped, sizeof grouped);
    // Same reasoning as the silent-payment receive screen: long meant font14
    // here too. This column is 700 wide, so 23 wraps a grouped SP address in
    // three lines and there is no reason to go smaller.
    bool longaddr = strlen(addr) > 64;
    shown = wt_addr_spans(s_scr, grouped, 700,
                          longaddr ? wt_font_mono23() : wt_font_mono28());
    lv_obj_set_pos(shown, 48, 186);
  }
  lv_obj_update_layout(shown);
  lv_obj_set_size(acard, 752, lv_obj_get_height(shown) + 32);
  lv_obj_move_to_index(acard, 0);        // behind the address, not over it
  int note_y = 186 + lv_obj_get_height(shown) + 16;
  if (note_y < 280) note_y = 280;

  if (mine || sp_mine) {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_OK, STR_R_YOURS), 48, 130, wt_font28(), WT_OK);
    if (sp_mine)
      snprintf(buf, sizeof buf, "%s", tr(STR_S_SP_BADGE));
    else if (change)
      snprintf(buf, sizeof buf, tr(STR_R_CHANGE_FMT), (unsigned)idx);
    else
      snprintf(buf, sizeof buf, tr(STR_R_RECV_FMT), (unsigned)idx);
    // which address this is (receive #N / change #N / silent payment): the
    // fact the owner checks against their coordinator, not a unit tag
    wt_lbl(s_scr, buf, 48, note_y, wt_body_font(buf, 700, 29), WT_MUT);
  } else if (validity == WADDR_CURRENT_NETWORK) {
    snprintf(buf, sizeof buf, tr(STR_R_NOT_FOUND_FMT), VFY_SCAN_DEPTH);
    lv_obj_t *headline = wt_lbl(s_scr, "", 48, 130,
                                wt_body_font(buf, 700, 44),
                                wt_ink_for(WT_WARN));
    lv_label_set_text_fmt(headline, LV_SYMBOL_WARNING " %s", buf);
    // The HEADLINE carries the amber and the explanation does not, which is
    // what the other two verdicts on this screen already do: wrong network and
    // invalid both put the colour on the verdict and leave the sentence under
    // it muted. This branch painted both, and when the note was font14 that
    // read as a tint; at the size it should always have been it is half the
    // page in warning colour, saying "caution" twice about one fact.
    //
    // The sentence is not a second warning either. It says the address is
    // valid and the search was bounded -- the reassuring half of the verdict.
    wt_wrap(s_scr, tr(STR_R_NOT_FOUND_B), 48, note_y, 700,
            WT_CONTENT_BOTTOM - note_y);
  } else if (validity == WADDR_WRONG_NETWORK) {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_CLOSE, STR_R_WRONG_NET),
           48, 130, wt_font28(), WT_STOP);
    // Name both networks. The %s placeholders in STR_R_WRONG_NET_B are
    // (address_network, kiss_network) so the reader learns what was scanned
    // and what the device is set to in one sentence. "mainnet" and "testnet"
    // are Bitcoin proper nouns and stay untranslated; every locale already
    // uses those two words as English in this file.
    char buf[256];
    // The address's side cannot be narrowed past "testnet": a tb1 address is
    // the same string on all three test chains. Ours can, and it is the half a
    // reader acts on.
    const char *addr_net = kiss_testnet() ? "mainnet" : "testnet";
    const char *wall_net = kiss_network() == KISS_NET_SIGNET ? "signet"
                         : kiss_testnet()                    ? "testnet"
                                                             : "mainnet";
    snprintf(buf, sizeof buf, tr(STR_R_WRONG_NET_B), addr_net, wall_net);
    wt_wrap(s_scr, buf, 48, note_y, 700, WT_CONTENT_BOTTOM - note_y);
  } else {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_CLOSE, STR_R_INVALID),
           48, 130, wt_font28(), WT_STOP);
    wt_wrap(s_scr, tr(STR_R_INVALID_B), 48, note_y, 700,
            WT_CONTENT_BOTTOM - note_y);
  }

  // What the usage half said, under the answer the owner actually came for.
  // A mismatched fingerprint does NOT refuse the scan: ownership is answerable
  // without trusting a byte of the payload, and refusing would withhold the one
  // answer this screen owes in order to punish a half that is only display data.
  if (have_um) {
    char note[160];
    if (!um_view)
      snprintf(note, sizeof note, "%s", tr(STR_R_UM_OTHER_KEYS));
    else if (!um_recorded)
      snprintf(note, sizeof note, "%s", tr(STR_R_UM_NO_CHANGE));
    else if (um.high >= 0)
      snprintf(note, sizeof note, tr(STR_R_CHAIN_UPTO_FMT), (unsigned)um.high);
    else
      snprintf(note, sizeof note, "%s", tr(STR_R_CHAIN_CLEAN));
    // font23 on one line, not wt_note_fit into 34px -- that box only ever had
    // room for font14 and this is a sentence the owner reads.
    lv_obj_t *ul = wt_lbl(s_scr, note, 48, WT_ACTION_Y - 44, wt_font23(),
                          WT_MUT);
    lv_obj_set_width(ul, 700);
    lv_obj_set_height(ul, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(ul, LV_LABEL_LONG_DOT);
  }

  wt_arrow_action(s_scr, tr(STR_R_SCAN_ANOTHER), true, false, 48, WT_ACTION_Y, 0, false, vfy_scan, NULL);
  wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, 592, WT_ACTION_Y, 160, true, vfy_done_cb, NULL);
}

static void vfy_cancel(void) {
  kiss_recv_open(s_parent);            // backed out of the camera: back to Receive
}

static void vfy_scan(lv_event_t *e) {
  (void)e;
  // The FULL close, not a bare screen delete: every static this file holds
  // must be nulled before the glass changes hands, or the reopen touches a
  // freed control -- s_next_act was exactly that crash.
  close_cb(NULL);
  kiss_scan_open_raw(s_parent, vfy_result, vfy_cancel);
}

// ---- silent payment (BIP352) static receive address ----
// One reusable sp1/tsp1, derived on-device (m/352'). No index, no reuse guard:
// a silent-payment address is meant to be shared and reused; that's the point.

static void sp_back_cb(lv_event_t *e) {
  (void)e;
  close_cb(NULL);
  // Back to the tab this screen opened FROM. A fresh open lands on THIS
  // ADDRESS now, so going home the ordinary way would silently change tabs
  // under the BACK tap.
  kiss_recv_open_sp(s_parent);
}

// A reusable Silent Payment address is intentionally NOT the address that
// appears in the transaction. This surprises people who compare Sparrow's
// output list after a first test payment, so explain the two prefixes beside
// the address instead of making them discover it in documentation.
// Which prefix goes where, as two chips. The card is about two strings that
// look different, and the answer to "why are they different" is a picture of
// the pair: what you hand out, and what turns up in the transaction. The marks
// carry the roles, so the diagram costs no string in any of the 21 locales --
// upload is the one you give away, and the eye is the card's own badge.
//
// The aside takes no user data, so the two prefixes live here for the length of
// the card. Same lifetime as the title and body beside them.
static const char *s_sp_share, *s_sp_seen;

static int aside_sp_prefixes(lv_obj_t *par, int x, int y, int w) {
  (void)w;
  char buf[WT_ICON_TEXT_MAX];
  lv_obj_t *col = lv_obj_create(par);
  lv_obj_remove_style_all(col);
  lv_obj_set_pos(col, x, y);
  lv_obj_set_width(col, 704);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

  // wt_icon_text writes the icon into out before it reads txt, so out and txt
  // must not be the same buffer -- aliasing them drops the label and leaves two
  // chips wearing nothing but their mark.
  lv_obj_t *row = wt_diagram_row(col);
  wt_icon_text(buf, sizeof buf, LV_SYMBOL_UPLOAD, s_sp_share);
  wt_chip(row, buf, false);
  wt_diagram_op(row, LV_SYMBOL_RIGHT);
  wt_icon_text(buf, sizeof buf, LV_SYMBOL_EYE_OPEN, s_sp_seen);
  wt_chip(row, buf, true);

  lv_obj_update_layout(col);
  return lv_obj_get_height(col);
}

static void sp_help_cb(lv_event_t *e) {
  (void)e;
  // The prefixes are the whole subject, so they are arguments rather than baked
  // text. Two of the three paragraphs said which one you share and which one
  // appears -- that IS the diagram now, and what survives is the part no
  // picture makes: why the sender's wallet swaps one for the other.
  const bool tn = kiss_testnet();
  static char title[96], body[640];            // outlive this call: the card reads them
  s_sp_share = tn ? "tsp1" : "sp1";            // what you hand out
  s_sp_seen  = tn ? "tb1p" : "bc1p";           // what lands in the transaction
  snprintf(title, sizeof title, tr(STR_R_SP_WHY_T), tn ? "TB1P" : "BC1P");
  snprintf(body, sizeof body, tr(STR_R_SP_WHY_B), s_sp_seen);

  // An eye, because the whole card is about which address other people SEE.
  wt_explain_t x = {
      .title  = title,
      .icon   = LV_SYMBOL_EYE_OPEN,
      .body   = body,
      .ok_txt = tr(STR_C_OK),
      .aside  = aside_sp_prefixes,
  };
  wt_explain_open(s_scr, &x);
}

static void sp_addr_render(void) {
  if (s_addr_sg) lv_obj_delete(s_addr_sg);
  if (s_sp_full) {
    // The full string remains one tap away. It is the source of truth for
    // reading or comparing the address; the folded default is only a view.
    char grouped[200];
    wt_group4(s_sp_addr, grouped, sizeof(grouped));
    s_addr_sg = wt_addr_spans(s_scr, grouped, SP_COL_W, wt_font_mono28());
    lv_obj_set_pos(s_addr_sg, SP_COL_X, 100);
  } else {
    // Match the readable list form: constant prefix muted, four meaningful
    // characters near each end lit. The QR still receives all of `s_sp_addr`.
    //
    // TWO lines if it needs them, and it does. This was a real defect rather
    // than a preference: the folded form is about 28 characters whatever the
    // address, which is ~406px at mono28, and this column is 386. The last lit
    // block ran off the right edge of the panel as a partial group, and on
    // testnet, where the prefix is a character longer, it ran into the frame. An
    // address on a receive screen may not be clipped, ever.
    //
    // The fix is the wrap, not a smaller face. wt_addr_short builds a one line
    // group because a list row is 690 wide and wants one; here the group is told
    // its width and allowed to break, so the size stays readable and the height
    // grows. sp_addr_render measures the address to place the path below it, so
    // the second line costs nothing but the space it takes.
    s_addr_sg = wt_addr_short(s_scr, s_sp_addr, wt_font_mono28());
    lv_spangroup_set_mode(s_addr_sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_width(s_addr_sg, SP_COL_W);
    lv_obj_set_height(s_addr_sg, LV_SIZE_CONTENT);
    lv_spangroup_refresh(s_addr_sg);
    lv_obj_set_pos(s_addr_sg, SP_COL_X, 140);
  }

  // Full mainnet and testnet addresses wrap to different heights; keep the
  // path below either form and above the bottom controls.
  //
  // The ceiling is measured, not written down. It used to be a flat 370, which
  // is where a font23 line STARTS if it is to end on 398, except that the line
  // box is 28 and 370 + 28 is 398 exactly, so the last row of pixels landed ON
  // WT_CONTENT_BOTTOM rather than above it. One pixel, in all 21 locales, and
  // only in the testnet full view: tsp1 is a character longer than sp1, which
  // is the one case that pushes the address tall enough for the clamp to bite.
  //
  // Nobody saw it because nobody could get here. The walk's TESTNET tap had
  // been missing its pill since the action bar landed, so every frame named
  // _tn was a picture of mainnet.
  //
  // The block is the caption AND the path now, so the ceiling has to be
  // measured against both. 22 is the caption-to-value step the receive screen
  // uses, kept identical so the two blocks read as the same component.
  lv_obj_update_layout(s_addr_sg);
  lv_obj_update_layout(s_sp_path_lbl);
  const int cap_step = 22;
  int path_y = lv_obj_get_y(s_addr_sg) + lv_obj_get_height(s_addr_sg) + 14;
  int path_max = WT_CONTENT_BOTTOM - cap_step - lv_obj_get_height(s_sp_path_lbl);
  if (path_y > path_max) path_y = path_max;
  if (s_sp_path_sec) lv_obj_set_y(s_sp_path_sec, path_y);
  lv_obj_set_y(s_sp_path_lbl, path_y + cap_step);

  // The card is fitted to whichever view is up, once both its contents have
  // been measured and placed. A fixed box cannot serve both: the folded address
  // is two lines and the full one is nine.
  //
  // Both edges are clamped to the page, and in the full view both clamps bite.
  // 96 is the content line, so the card can never reach up into the subtitle;
  // WT_CONTENT_BOTTOM is the floor, so it can never run under the action bar.
  // The full view fills the column exactly, which is why its padding comes out
  // thinner than the folded view's: there is no room to spend and an address may
  // not be shortened to make a box look comfortable.
  if (s_sp_card) {
    int top = lv_obj_get_y(s_addr_sg) - 14;
    int bot = path_y + cap_step + lv_obj_get_height(s_sp_path_lbl) + 12;
    if (top < 96) top = 96;
    if (bot > WT_CONTENT_BOTTOM) bot = WT_CONTENT_BOTTOM;
    lv_obj_set_pos(s_sp_card, SP_CARD_X, top);
    lv_obj_set_size(s_sp_card, SP_CARD_W, bot - top);
  }

  // The folded text is useful enough to be a direct affordance, but address
  // span groups deliberately do not accept taps globally: doing that would
  // swallow taps on every address-list row. A persistent transparent hit box
  // gives only this standalone address the shortcut and survives its own
  // callback while the spans beneath it are rebuilt.
  if (s_sp_addr_hit) {
    lv_obj_set_pos(s_sp_addr_hit, lv_obj_get_x(s_addr_sg) - 8,
                   lv_obj_get_y(s_addr_sg) - 8);
    lv_obj_set_size(s_sp_addr_hit, lv_obj_get_width(s_addr_sg) + 16,
                    lv_obj_get_height(s_addr_sg) + 16);
    lv_obj_move_foreground(s_sp_addr_hit);
  }

  // Through the kit's own setter. Reaching for child 0 relabelled the ARROW
  // on a forward action and drew the word twice, one on top of the other.
  wt_arrow_action_set_text(s_sp_toggle_act,
                           tr(s_sp_full ? STR_R_SP_SHOW_SHORT
                                        : STR_R_SP_SHOW_FULL));
}

static void sp_toggle_cb(lv_event_t *e) {
  (void)e;
  s_sp_full = !s_sp_full;
  sp_addr_render();
}

static void sp_addr_open(lv_obj_t *parent) {
  s_parent = parent;
  s_addr_sg = NULL;
  s_scr = wt_screen(parent, tr(STR_S_SP_BADGE), tr(STR_R_S));
  wt_chrome_head(s_scr);
  // Keep the title out of the help target at the right edge. This mirrors the
  // fingerprint header: both put the same 54px target at x=715.
  wt_title_fit(s_scr, 640);
  // The longer tsp1 full view can make LVGL auto-scroll a default container
  // to its newest child, shifting the fixed 800x480 composition off-screen.
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  // Same 30px visual / 54px touch target as every anonymous help affordance.
  wt_help_chip(s_scr, 715, 35, WT_MUT, sp_help_cb, NULL);
  // This is the one receive code meant to be scanned by somebody else's
  // phone. Folding the default text view buys enough room to raise the QR one
  // module scale while preserving a real white quiet zone around it.
  wt_qr_card(s_scr, &s_qr, 44, 96, 304, 280);

  // Same refusal contract as recv_refresh: this QR exists to be scanned by
  // somebody else's phone, so a failed derivation hides it rather than encoding
  // the failure. The state still lands in s_sp_addr for the text lane, where
  // words reading as words is the point.
  if (kiss_session_sp_address(s_sp_addr, sizeof(s_sp_addr)) != 0) {
    snprintf(s_sp_addr, sizeof(s_sp_addr), "%s", tr(STR_C_SESSION_LOCKED));
    wt_qr_refusal(s_qr, true);
  } else if (s_qr)
    wt_qr_update(s_qr, s_sp_addr, (uint32_t)strlen(s_sp_addr));

  // WT_INK for the same reason as the other two paths: on this panel WT_MUT is
  // not a quieter grey, it is nearly none. This one is already font23 and keeps
  // its bare form, without the caption and "?" the detail screen's path got,
  // because its y is computed from the address above it and clamped at 370
  // (sp_addr_render) so the folded and full views can share the screen. A
  // three object block cannot ride that clamp without landing under the action
  // bar in the full view. It belongs with the phase 3 receive restructure.
  // Captioned, like the path on the receive screen beside it. It was a bare
  // m/352h/0h/0h under a silent payment address: correct, at a readable size,
  // and with nothing on screen saying what it was. The two screens print the
  // same KIND of value and a reader who learned what it meant on one of them
  // had to learn it again here. STR_I_SEC_PATH is the caption the other screen
  // already uses, so this needed no new string in any of the 21 locales.
  //
  // Both are placed by sp_addr_render, because the address above them wraps to
  // different heights in the folded and full views.
  // Created BEFORE the address and the path so it sits behind both, and resized
  // by sp_addr_render once their real heights are known.
  s_sp_card = wt_card(s_scr, SP_CARD_X, 126, SP_CARD_W, 160);
  s_sp_path_sec = wt_section(s_scr, tr(STR_I_SEC_PATH), SP_COL_X, 200);
  s_sp_path_lbl = wt_lbl(s_scr, "", SP_COL_X, 222, wt_font_mono23(), WT_INK);
  lv_label_set_text_fmt(s_sp_path_lbl, "m/352h/%dh/0h   %s",
                        kiss_testnet() ? 1 : 0,
                        on_net_line());

  wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, sp_back_cb, NULL);
  s_sp_toggle_act = wt_arrow_action(s_scr, tr(STR_R_SP_SHOW_FULL), false, false, WT_ACT_X, WT_ACTION_Y, 0, false, sp_toggle_cb, NULL);
  s_sp_addr_hit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_sp_addr_hit);
  lv_obj_set_style_radius(s_sp_addr_hit, 8, 0);
  lv_obj_add_flag(s_sp_addr_hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_sp_addr_hit, LV_OBJ_FLAG_SCROLLABLE);
  wt_tap_feedback(s_sp_addr_hit);
  lv_obj_add_event_cb(s_sp_addr_hit, sp_toggle_cb, LV_EVENT_CLICKED, NULL);
  sp_addr_render();
}

static void sp_open_cb(lv_event_t *e) {
  (void)e;
  s_sp_full = false;
  close_cb(NULL);
  sp_addr_open(s_parent);
}

// The SCAN KEY row's door: the ONE consent gate and reveal, which live in
// kiss_info beside the KEYS launcher, with this tab as the way home. The
// close is the full close_cb, not a bare screen delete: everything RECEIVE
// holds must be nulled before another module owns the glass.
static void sp_scan_key_return(void) { kiss_recv_open_sp(s_parent); }
static void sp_scan_key_cb(lv_event_t *e) {
  (void)e;
  lv_obj_t *par = s_parent;
  close_cb(NULL);
  kiss_info_open_scan_key(par, sp_scan_key_return);
}

// ---- RECEIVE: three tabs on one lane ----
// THIS ADDRESS, ALL ADDRESSES and SILENT PAYMENT were a screen, a screen one
// tap in from it, and a row in a column. They are three tabs now, on the same
// bracket strip KEYS wears, and the two destination rows that used to eat the
// bottom of the detail screen are the strip.
static wt_pane_t s_rctx;
// The kit keeps its own exec callbacks private, and these two are a line each.
static void wt_anim_ty(void *v, int32_t y)  { lv_obj_set_style_translate_y(v, y, 0); }
// The dot swells by changing SIZE, not by transform_scale, and the difference
// is not taste. A transform puts LVGL on the layer path: it allocates a buffer
// the size of the transformed object every frame, and when that allocation
// fails LV_ASSERT_MALLOC does not return -- it spins. The walk hung on the one
// frame that flips this lamp, for ever, having drawn 167 of 467 frames. Size
// is the same picture and allocates nothing.
//
// Recentred as it grows, so it swells about its own middle the way a scale
// would rather than growing down and to the right.
static void wt_anim_dot(void *v, int32_t d) {
  lv_obj_t *o = v;
  const int cx = lv_obj_get_x(o) + lv_obj_get_width(o) / 2;
  const int cy = lv_obj_get_y(o) + lv_obj_get_height(o) / 2;
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, d / 2 + 1, 0);
  lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
}
static void wt_anim_opa(void *v, int32_t o) { lv_obj_set_style_opa(v, (lv_opa_t)o, 0); }
static void recv_tab_build(void);
static void recv_detail_open(void);
static void pop_chevron(bool open);

#define RECV_COL_X 296
#define RECV_COL_W 456

static void pop_close(void) {
  // The scrim OWNS the box, so one delete takes both. Deleting the box alone
  // is what left a full screen catcher on the page eating every later tap.
  // Async, because this runs from click handlers on the popover's own
  // descendants -- the same hazard close_cb already schedules around.
  if (s_pop_away) lv_obj_delete_async(s_pop_away);
  s_pop = s_pop_away = NULL;
  pop_chevron(false);
}

// Whether the content lane is showing the page's [ ? ] explainer instead of
// the selected tab. Reset on every open of the page: it is a view, not a
// remembered state.
static bool s_help_open;

static void recv_help_cb(lv_event_t *e) {
  (void)e;
  pop_close();
  s_help_open = !s_help_open;
  // wt_pane_go refuses a same-tab call, so this is its swap by hand -- the
  // same hand swap the KEYS page does. [ ? ] still never highlights, but the
  // strip DOES release its tab: a bracketed THIS ADDRESS under the receive
  // explainer says the owner is still on it.
  wt_tabs_flex_help(s_rctx.tabs, s_rctx.tab, s_help_open);
  const bool was_moving = s_rctx.entering;
  wt_pane_stop(&s_rctx);
  if (was_moving && s_rctx.pane) {
    lv_obj_delete(s_rctx.pane);
    s_rctx.pane = NULL;
  }
  s_rctx.pane_out = s_rctx.pane;
  s_rctx.pane = wt_pane_new(&s_rctx);
  recv_tab_build();
  wt_accent_restyle(s_rctx.pane);
  const int dir = s_help_open ? 1 : -1;
  wt_pane_enter(&s_rctx, dir, false);
  wt_pane_exit(&s_rctx, dir);
}

static void recv_tab_go(int tab) {
  if (tab < 0 || tab > 2) return;      // the deck ends where the strip does
  pop_close();
  // A real tab is also the way back from [ ? ]: tapping the one already
  // selected re-lands on its rows, which wt_pane_go's same-tab refusal
  // would otherwise swallow.
  if (s_help_open && tab == s_rctx.tab) { recv_help_cb(NULL); return; }
  s_help_open = false;
  wt_pane_go(&s_rctx, tab, false, recv_tab_build);
}

static void recv_tab_cb(lv_event_t *e) {
  recv_tab_go((int)(intptr_t)lv_event_get_user_data(e));
}

// ---- tab 1: the lamp ----
// It replaces wt_state_chip on this screen: no border, no fill, no radius. A
// chip is a box, and the box is what this redesign removes -- what is left is
// a lit dot and the word beside it, which is what a readout looks like
// everywhere outside software.
//
// USED and UNUSED, four letters apart, and both words a new owner already
// knows. NEVER HANDED OUT / HANDED OUT ALREADY was tried and cut: too long for
// the lane and too wordy for the reader.
// Motion 11. The dot swells and settles when the state CHANGES -- not on every
// refresh, or the lamp would throb each time the screen redrew for a reason
// that has nothing to do with it.
// UNUSED breathes for as long as it is true: the green lamp is the page's one
// live claim ("safe to hand out"), and a static dot was filed from the bench
// as not pulsing. USED sits still -- a warning at rest, not an invitation.
static int s_lamp_was = -1;
static void lamp_breathe_maybe(lv_anim_t *a) {
  if (s_lamp_was == 0) wt_dot_breathe(a->var, 10, 4, false);
}
static void lamp_pulse_done(lv_anim_t *a) {
  lv_anim_t b;
  lv_anim_init(&b);
  lv_anim_set_var(&b, a->var);
  lv_anim_set_exec_cb(&b, (lv_anim_exec_xcb_t)wt_anim_dot);
  lv_anim_set_values(&b, 21, 10);
  lv_anim_set_duration(&b, 200);
  lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
  lv_anim_set_completed_cb(&b, lamp_breathe_maybe);
  lv_anim_start(&b);
}

static void lamp_set(bool used) {
  if (!s_lamp_dot || !s_lamp_lbl) return;
  const bool changed = s_lamp_was >= 0 && s_lamp_was != (int)used;
  s_lamp_was = (int)used;
  // Whatever was breathing or swelling stops here, and the styles it drives
  // come back to rest before the state below decides what moves next.
  lv_anim_del(s_lamp_dot, NULL);
  lv_obj_set_style_opa(s_lamp_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_translate_x(s_lamp_dot, 0, 0);
  lv_obj_set_style_translate_y(s_lamp_dot, 0, 0);
  lv_obj_set_size(s_lamp_dot, 10, 10);
  lv_obj_set_style_radius(s_lamp_dot, 5, 0);
  // GREEN's accent is byte identical to WT_OK and ORANGE is a near match for
  // WT_WARN, so on those two themes the lamp's colour says nothing the rest of
  // the page is not already saying, and a readout that cannot be told from
  // chrome has stopped being a readout.
  //
  // The kit's rule for this is that the ACCENT stands aside, never the status
  // -- a state must keep its colour or it stops meaning anything. So what
  // stands aside here is the accent-painted caption sitting beside the lamp:
  // ADDRESS #N drops to WT_MUT whenever the accent would collide with the
  // state it sits next to, and the lamp keeps WT_OK and WT_WARN in all four.
  // The LAMP keeps the state colour; the word beside it takes the accent when
  // that colour is the caution. Amber is a mark colour here.
  lv_color_t col = used ? WT_WARN : WT_OK;
  if (s_idx_lbl) {
    const bool clash = lv_color_eq(wt_accent(), col);
    lv_obj_set_style_text_color(s_idx_lbl, clash ? WT_MUT : wt_accent(), 0);
    if (clash) lv_obj_remove_flag(s_idx_lbl, WT_FLAG_ACCENT);
    else       lv_obj_add_flag(s_idx_lbl, WT_FLAG_ACCENT);
  }
  lv_obj_set_style_bg_color(s_lamp_dot, col, 0);
  lv_obj_set_style_shadow_color(s_lamp_dot, col, 0);
  lv_label_set_text(s_lamp_lbl, tr(used ? STR_R_HANDED_ALREADY
                                        : STR_R_NEVER_HANDED));
  // The dot above keeps the state colour; the two words beside it take the
  // accent when that colour is the caution.
  lv_obj_set_style_text_color(s_lamp_lbl, wt_ink_for(col), 0);
  lv_obj_update_layout(s_lamp_lbl);
  // Right aligned to 752 and recomputed every refresh: the two words are
  // different lengths, and more so per locale.
  lv_obj_set_pos(s_lamp_lbl, 752 - lv_obj_get_width(s_lamp_lbl), 120);
  lv_obj_set_pos(s_lamp_dot, 752 - lv_obj_get_width(s_lamp_lbl) - 8 - 14, 131);
  if (changed) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lamp_dot);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)wt_anim_dot);
    lv_anim_set_values(&a, 10, 21);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, lamp_pulse_done);
    lv_anim_start(&a);
  } else if (!used) {
    wt_dot_breathe(s_lamp_dot, 10, 4, false);
  }
}

static bool recv_used(uint32_t idx) {
  uint8_t fp[4];
  kiss_ui_last_fp(fp);
  const int net = kiss_testnet() ? 1 : 0, sc = kiss_script();
  int high = kiss_usage_high(fp, net, sc);
  // What a COORDINATOR said, when it has said anything. This signer has no
  // chain view: its own high-water mark is only "what I have seen", and a
  // coordinator that has scanned the chain knows better. Its number wins when
  // it is larger, which is the only direction that can be true.
  int chigh = -1; uint32_t cheight = 0;
  if (kiss_usage_chain_known(fp, net, sc, &chigh, &cheight) && chigh > high)
    high = chigh;
  return high >= 0 && (int)idx <= high;
}

// Has a coordinator ever told this signer anything about these keys?
// ---- receive ----
static void recv_refresh(void) {
  if (s_rctx.tab != 0) return;        // the other two tabs own none of this
  char addr[91];
  int rc = kiss_session_address(0, s_idx, addr, sizeof(addr));
  // Refused = no QR. This square is the one a SENDER is invited to scan, and
  // the failure string used to go straight through wt_qr_update -- a scannable
  // code whose content was the words "SESSION LOCKED", offered as a payment
  // address. The words render once, in the address lane, as a state.
  wt_qr_refusal(s_qr, rc != 0);
  if (rc == 0 && s_lock_note) { lv_obj_delete(s_lock_note); s_lock_note = NULL; }
  if (s_addr_sg) { lv_obj_delete(s_addr_sg); s_addr_sg = NULL; }
  lv_obj_t *par = s_rctx.pane ? s_rctx.pane : s_scr;

  if (rc != 0) {
    // A state must read as a state. Never through the fold: an ellipsis and a
    // lit tail would turn LOCKED into an address-shaped fragment.
    s_addr_sg = wt_lbl(par, tr(STR_C_SESSION_LOCKED), RECV_COL_X, 160,
                       wt_font23(), wt_ink_for(WT_WARN));
    if (!s_lock_note)
      s_lock_note = wt_note(par, tr(STR_C_LOCKED_B), 48, 130, 216, 200);
  } else {
    if (s_qr) wt_qr_update(s_qr, addr, (uint32_t)strlen(addr));
    if (s_addr_full) {
      // The whole address, grouped in fours with the lit tail -- the SILENT
      // screen's own render, on the one tab allowed to show it. It wraps;
      // the caption below moves with it.
      char grouped[128];
      wt_group4(addr, grouped, sizeof grouped);
      s_addr_sg = wt_addr_spans(par, grouped, RECV_COL_W, wt_font_mono23());
      lv_obj_set_pos(s_addr_sg, RECV_COL_X, 160);
    } else {
      // ONE object, one line, never wrapped -- the same rule every address on
      // this device follows. 456 at mono23 holds the fold with room; if a
      // future prefix pushes it, the air around the ellipsis goes, never a
      // block and never the font size.
      s_addr_sg = wt_addr_short(par, addr, wt_font_mono23());
      lv_obj_set_pos(s_addr_sg, RECV_COL_X, 160);
    }
  }
  // The caption goes with the address it captions: "compare the lit
  // characters" under a state word is an instruction with no object -- and
  // under the FULL form it sits wherever the wrap ends.
  if (s_cmp_lbl) {
    if (rc != 0) lv_obj_add_flag(s_cmp_lbl, LV_OBJ_FLAG_HIDDEN);
    else {
      lv_obj_remove_flag(s_cmp_lbl, LV_OBJ_FLAG_HIDDEN);
      if (s_addr_sg) {
        lv_obj_update_layout(s_addr_sg);
        lv_obj_set_y(s_cmp_lbl, 160 + lv_obj_get_height(s_addr_sg) + 8);
      }
    }
  }

  if (s_idx_lbl) {
    lv_label_set_text_fmt(s_idx_lbl, tr(STR_R_ADDR_N_FMT), (unsigned)s_idx);
    lv_obj_update_layout(s_idx_lbl);
    if (s_idx_chev)
      lv_obj_set_pos(s_idx_chev, lv_obj_get_width(s_idx_lbl) + 10, 5);
  }
  if (s_path_lbl) {
    int purpose = kiss_script() == WSCRIPT_LEGACY ? 44
                : kiss_script() == WSCRIPT_NESTED ? 49 : 84;
    lv_label_set_text_fmt(s_path_lbl, "m/%dh/%dh/0h/0/%u",
                          purpose, kiss_testnet() ? 1 : 0, (unsigned)s_idx);
    lv_obj_update_layout(s_path_lbl);
    // Right-aligned against the "?" mark inside its own row.
    lv_obj_set_pos(s_path_lbl,
                   RECV_COL_W - 19 - 2 - 10 - lv_obj_get_width(s_path_lbl),
                   (34 - lv_font_get_line_height(wt_font_mono18())) / 2);
  }

  if ((int)s_idx > s_seen_high) s_seen_high = (int)s_idx;   // seeds next open's landing

  // Used and unused is a claim of the same shape as what kiss_usage_high
  // actually holds: an address this device signed a spend from is on chain, so
  // USED is certain. UNUSED means no record here, which is what the standard
  // term means in any watch-only wallet too.
  const bool used = recv_used(s_idx);
  lamp_set(used);
  // No paragraph under the path any more. The lamp's answer was a three-way
  // lecture on the tab's first look; the lamp already says the state, the
  // [ ? ] teaches the rule, and NEXT ADDRESS is the remedy sitting right
  // there in the band.
}

static void addr_swap_anim(void) {
  // The address changed under a page that did not: it rises 5 and fades in, so
  // the eye is told something moved without the whole column redrawing.
  if (!s_addr_sg) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_addr_sg);
  lv_anim_set_duration(&a, 240);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_values(&a, 5, 0);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)wt_anim_ty);
  lv_anim_start(&a);
  lv_obj_set_style_opa(s_addr_sg, LV_OPA_TRANSP, 0);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)wt_anim_opa);
  lv_anim_start(&a);
}

// The address block's tap: folded to whole and back. The locked render never
// builds the spans, so toggling while locked redraws the refusal unchanged.
static void addr_toggle_cb(lv_event_t *e) {
  (void)e;
  s_addr_full = !s_addr_full;
  recv_refresh();
  addr_swap_anim();
}

static void next_cb(lv_event_t *e) {
  (void)e;
  // Tab 0 only. The action is hidden on the other tabs now, but a guard here
  // is what stops a stray event from silently advancing the hidden index --
  // which is exactly what tapping the visible-but-inert pill used to do.
  if (s_rctx.tab != 0) return;
  pop_close();
  // The next UNUSED index, not merely the next one: the button's whole promise
  // is a fresh address, and stepping onto one this signer has already shown
  // would break it silently. CAPPED: the old loop's cap only bounded the
  // skip-used walk, so repeated taps marched s_idx past #99 and the popover
  // could no longer contain its own selection.
  if (s_idx >= RECV_LIST_CAP - 1) return;
  do { s_idx++; } while (s_idx < RECV_LIST_CAP - 1 && recv_used(s_idx));
  recv_refresh();
  addr_swap_anim();
}

// ---- tab 1: the index popover ----
static void pop_pick_cb(lv_event_t *e) {
  s_idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  pop_close();
  recv_refresh();
  addr_swap_anim();
}

static void pop_dismiss_cb(lv_event_t *e) { (void)e; pop_close(); }

// PAGE-ALIGNED, never re-centred. The old window was the index plus or minus
// two, rebuilt around every pick -- so the same physical row changed meaning
// between taps, and going BACK an index meant six open/pick cycles. Pages of
// five hold still under a finger (#0..#4, #5..#9, ...), and the two pager
// rows walk them; a long hunt is still what ALL ADDRESSES is for.
#define POP_ROWS   5
#define POP_ITEM_H 44
#define POP_PAGE_H 28
static uint32_t s_pop_base;

static void pop_show_at(uint32_t base);

static void pop_page_cb(lv_event_t *e) {
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  int base = (int)s_pop_base + dir * POP_ROWS;
  if (base < 0) base = 0;
  if (base > RECV_LIST_CAP - POP_ROWS) base = RECV_LIST_CAP - POP_ROWS;
  if ((uint32_t)base == s_pop_base) return;
  pop_close();
  pop_show_at((uint32_t)base);
}

static void pop_show_at(uint32_t base) {
  lv_obj_t *par = s_rctx.pane ? s_rctx.pane : s_scr;
  s_pop_base = base;
  const int h = POP_PAGE_H * 2 + POP_ROWS * POP_ITEM_H;

  // wt_overlay_box, not a bare box: the scrim is what closes the popover on a
  // tap outside AND what says the column underneath is behind something. Built
  // by hand the first time, the catcher was a transparent sibling -- so the
  // page still read as one plane, overlapcheck reported the popover colliding
  // with every line it was sitting on top of, and the catcher outlived the
  // popover and silently ate every later tap on the screen.
  s_pop = wt_overlay_box(par, &s_pop_away, RECV_COL_X, 120, 300, h,
                         8, pop_dismiss_cb);
  pop_chevron(true);
  // The accent rim the KEYS/RECEIVE weight asks for, in place of WT_EDGE.
  lv_obj_set_style_border_color(s_pop, wt_accent(), 0);
  lv_obj_set_style_border_opa(s_pop, 115, 0);
  lv_obj_add_flag(s_pop, WT_FLAG_ACCENT_BORDER);

  // The two pager rows, both always BUILT: a row that appears on page two
  // reflows the list under the finger that went looking for it. A dead edge
  // (first or last page) keeps its glyph at ghost opacity and takes no tap.
  for (int pg = 0; pg < 2; pg++) {
    const bool down = pg == 1;
    const bool dead = down ? base + POP_ROWS >= RECV_LIST_CAP : base == 0;
    lv_obj_t *pr = lv_obj_create(s_pop);
    lv_obj_remove_style_all(pr);
    lv_obj_set_pos(pr, 0, down ? POP_PAGE_H + POP_ROWS * POP_ITEM_H : 0);
    lv_obj_set_size(pr, 300, POP_PAGE_H);
    lv_obj_remove_flag(pr, LV_OBJ_FLAG_SCROLLABLE);
    if (!dead) {
      lv_obj_add_flag(pr, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(pr, wt_accent_pressed(), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(pr, LV_OPA_COVER, LV_STATE_PRESSED);
      lv_obj_add_event_cb(pr, pop_page_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)(down ? 1 : -1));
    }
    lv_obj_t *g = wt_lbl(pr, down ? LV_SYMBOL_DOWN : LV_SYMBOL_UP, 0, 0,
                         wt_font14(), dead ? WT_DIM : wt_accent());
    if (!dead) lv_obj_add_flag(g, WT_FLAG_ACCENT);
    lv_obj_set_style_text_opa(g, dead ? 90 : LV_OPA_COVER, 0);
    lv_obj_center(g);
  }

  for (int i = 0; i < POP_ROWS; i++) {
    const uint32_t idx = base + (uint32_t)i;
    if (idx >= RECV_LIST_CAP) break;
    const bool sel = idx == s_idx;
    lv_obj_t *it = lv_obj_create(s_pop);
    lv_obj_remove_style_all(it);
    lv_obj_set_pos(it, 0, POP_PAGE_H + i * POP_ITEM_H);
    lv_obj_set_size(it, 300, POP_ITEM_H);
    lv_obj_remove_flag(it, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(it, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(it, pop_pick_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)idx);
    // The tick is built on EVERY item and hidden with opacity, the same
    // discipline as the tab brackets: an item that gains a glyph on selection
    // reflows the row under the finger that just picked it.
    lv_obj_t *ok = wt_lbl(it, LV_SYMBOL_OK, 12, 0, wt_font14(), wt_accent());
    lv_obj_set_style_text_opa(ok, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (sel) lv_obj_add_flag(ok, WT_FLAG_ACCENT);
    lv_obj_align(ok, LV_ALIGN_LEFT_MID, 12, 0);
    char num[8];
    snprintf(num, sizeof num, "#%u", (unsigned)idx);
    lv_obj_t *nl = wt_lbl(it, num, 0, 0, wt_font_mono23(),
                          sel ? WT_INK : WT_MUT);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 40, 0);
    const bool u = recv_used(idx);
    lv_obj_t *st = wt_lbl(it, tr(u ? STR_R_HANDED_ALREADY : STR_R_NEVER_HANDED),
                          0, 0, wt_font23(), u ? WT_DIM : WT_OK);
    lv_obj_align(st, LV_ALIGN_RIGHT_MID, -14, 0);
    if (i < POP_ROWS - 1) wt_line_rule(it, 0, POP_ITEM_H - 1, 300);
  }

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_pop);
  lv_anim_set_duration(&a, 160);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_values(&a, -6, 0);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)wt_anim_ty);
  lv_anim_start(&a);
  lv_obj_set_style_opa(s_pop, LV_OPA_TRANSP, 0);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)wt_anim_opa);
  lv_anim_start(&a);
}

static void pop_open(void) {
  if (s_pop) { pop_close(); return; }        // a second tap closes it
  pop_show_at((s_idx / POP_ROWS) * POP_ROWS);
}

// Motion 17: the chevron turns over as the popover opens, and back as it
// closes, so the mark states which way the thing under it is going.
static void pop_chevron(bool open) {
  if (!s_idx_chev) return;
  // The glyph SWAPS rather than rotating, for the same reason the dot swells
  // by size: transform_rotation is the layer path and the layer path is what
  // hangs this renderer. DOWN and UP are both in the baked symbol set, so the
  // mark still states which way the thing under it is going.
  lv_label_set_text(s_idx_chev, open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
}

static void pop_toggle_cb(lv_event_t *e) { (void)e; pop_open(); }

static void path_help_cb(lv_event_t *e) {
  (void)e;
  // The PATH's own explainer. This opened the ADDRESS TYPE card, on the
  // reasoning that the card's second line mentions "m/...". It does, in
  // passing, under a heading about something else -- so a reader who tapped
  // the "?" beside a derivation path got a page titled ADDRESS TYPE, and the
  // bench asked exactly why: "there is no bitcoin simple explainer, it
  // explains fucking ADDRESS TYPE". Two values, two questions, two cards.
  //
  // The term line names what a coordinator calls this. Plain sentence first,
  // real term underneath: a reader who meets ACCOUNT in Sparrow has to
  // recognise it as the thing this page just explained, and a card that only
  // ever says "the numbered branch" teaches a phrase that exists nowhere
  // else in bitcoin.
  wt_explain_t x = {
      .title      = tr(STR_R_PATH_H),
      .icon       = LV_SYMBOL_DIRECTORY,
      .body       = tr(STR_R_PATH_B),
      .term       = tr(STR_T_PATH_TERM),
      .term_label = tr(STR_G_TECHNICAL),
      .ok_txt     = tr(STR_C_OK),
  };
  wt_explain_open(s_scr, &x);
}

void kiss_recv_sim_open_path_help(void) { path_help_cb(NULL); }

static void enlarge_cb(lv_event_t *e) { (void)e; wt_qr_zoom(s_qr); }

// ---- tab 2: ALL ADDRESSES ----
static void row_tap_cb(lv_event_t *e) {
  s_idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  wt_pane_go(&s_rctx, 0, false, recv_tab_build);
}

// The stroke, on the RECEIVE page: one horizontal deck across the strip --
// [THIS ADDRESS] [ALL ADDRESSES p1..p34] [SILENT] -- so inside tab 1 a swipe
// turns the address pages and crosses to the neighbouring tab at either end
// of them. Entering the list by swipe lands on its remembered page, the same
// place a tap on the tab lands.
static void recv_gesture_cb(lv_event_t *e) {
  const int step = wt_swipe_step(e);
  if (!step) return;
  // [ ? ] stays a toggle, not a position on the deck, but the stroke reaches
  // it: past SILENT opens it, a right swipe on it comes back. SETTINGS and
  // KEYS make the same move, from the bench's "i cant swipe to the question
  // mark".
  if (s_help_open) {
    if (step < 0) recv_help_cb(NULL);
    return;
  }
  if (s_rctx.tab == 1) {
    const int base = (int)s_list_base + step * RECV_PAGE;
    if (base >= 0 && base < RECV_LIST_CAP) {
      s_list_base = (uint32_t)base;
      wt_page_flip(&s_rctx, recv_tab_build, step);
      return;
    }
  }
  if (s_rctx.tab + step > 2) { recv_help_cb(NULL); return; }
  recv_tab_go(s_rctx.tab + step);
}

// ---- tab 3: SILENT PAYMENT ----
// The SCAN KEY export used to be launched from here as well as from KEYS, and
// the return path that served it lived in kiss_info.c. Both are gone; the tab
// names the export and does not open it. See recv_tab_build().

// ---- the three groups ----
// NEXT ADDRESS shows only where it acts: tab 0 with the rows up, never under
// the [ ? ] and never on the list or SILENT.
static void recv_next_vis(void) {
  if (!s_next_act) return;
  if (s_rctx.tab == 0 && !s_help_open)
    lv_obj_remove_flag(s_next_act, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(s_next_act, LV_OBJ_FLAG_HIDDEN);
}

static void recv_tab_build(void) {
  lv_obj_t *p = s_rctx.pane;
  // The old pane took the popover and its catcher with it.
  s_pop = s_pop_away = NULL;
  const int X = 48, W = 704;
  s_qr = s_addr_sg = s_idx_lbl = s_path_lbl = s_lock_note = NULL;
  s_cmp_lbl = s_lamp_dot = s_lamp_lbl = s_idx_chev = NULL;
  s_addr_hit = NULL;
  s_state_chip = NULL;
  recv_next_vis();

  if (s_help_open) {
    // The [ ? ] content: the lane replaced, not a card and not an overlay.
    // Nothing on it is interactive; the strip is the way back.
    wt_fact_t facts[3] = {
        { tr(STR_R_HELP_F1C), tr(STR_R_HELP_F1V), LV_SYMBOL_PLUS },
        { tr(STR_R_HELP_F2C), tr(STR_R_HELP_F2V), LV_SYMBOL_EYE_OPEN },
        { tr(STR_R_HELP_F3C), tr(STR_R_HELP_F3V), WT_ICON_SECRET },
    };
    wt_explain(p, tr(STR_R_HELP_HEAD), tr(STR_R_HELP_BODY), facts, 3);
    return;
  }

  if (s_rctx.tab == 0) {
    // Left column: the QR and the one line that says it opens.
    wt_qr_card(p, &s_qr, X, 120, 216, 180);
    lv_obj_t *hit = lv_obj_create(p);
    lv_obj_remove_style_all(hit);
    lv_obj_set_pos(hit, X, 342);
    lv_obj_set_size(hit, 236, 32);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, enlarge_cb, LV_EVENT_CLICKED, NULL);
    // Shifts 4px right while held, the arrow action's answer at a smaller
    // scale: this line is a direction too.
    lv_obj_set_style_translate_x(hit, 0, 0);
    lv_obj_set_style_translate_x(hit, 4, LV_STATE_PRESSED);
    lv_obj_t *ic = wt_lbl(hit, WT_ICON_EXPAND, 0, 6, wt_font14(),
                          wt_accent());
    lv_obj_add_flag(ic, WT_FLAG_ACCENT);
    lv_obj_update_layout(ic);
    wt_lbl(hit, tr(STR_R_ENLARGE), lv_obj_get_width(ic) + 10, 0, wt_font23(),
           WT_MUT);

    // Right column. The caption is the popover's control, so it carries the
    // chevron that says so and the whole pair is one target. 260x40 and
    // chrome23, not the 200x28 font14 sliver the bench could not find: this
    // is the row's one control and it has to read as one.
    lv_obj_t *ih = lv_obj_create(p);
    lv_obj_remove_style_all(ih);
    lv_obj_set_pos(ih, RECV_COL_X, 108);
    lv_obj_set_size(ih, 260, 40);
    lv_obj_remove_flag(ih, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ih, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ih, pop_toggle_cb, LV_EVENT_CLICKED, NULL);
    // "ADDRESS #12" is a word carrying a number, not a code to compare, and it
    // sits beside the lamp's word. It goes with the captions.
    s_idx_lbl = wt_lbl(ih, "", 0, 5, wt_chrome23(tr(STR_R_ADDR_N_FMT)),
                       wt_accent());
    lv_obj_set_style_text_letter_space(s_idx_lbl, 2, 0);
    lv_obj_add_flag(s_idx_lbl, WT_FLAG_ACCENT);
    lv_obj_update_layout(s_idx_lbl);
    // Placed after the caption is MEASURED, not at a guessed x: "ADDRESS #0"
    // and "ADDRESS #12" are different widths, and so is every locale's word
    // for address. A fixed x drew the chevron through the index.
    lv_obj_t *cv = wt_lbl(ih, LV_SYMBOL_DOWN, 0, 5, wt_font23(), wt_accent());
    lv_obj_add_flag(cv, WT_FLAG_ACCENT);
    s_idx_chev = cv;
    // The chevron BOUNCES, forever: the bench asked for a control an owner
    // notices without being told. Translate only -- layout-free, and the
    // glyph swap in pop_chevron rides underneath it untouched.
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, cv);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)wt_anim_ty);
        lv_anim_set_values(&a, 0, 4);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_playback_duration(&a, 600);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    s_lamp_dot = lv_obj_create(p);
    lv_obj_remove_style_all(s_lamp_dot);
    lv_obj_set_size(s_lamp_dot, 10, 10);
    lv_obj_set_style_radius(s_lamp_dot, 5, 0);
    lv_obj_set_style_bg_opa(s_lamp_dot, LV_OPA_COVER, 0);
    // The glow. If it is ever expensive on glass, this goes before the dot
    // does: the dot is the readout, the glow is what makes it look lit.
    lv_obj_set_style_shadow_width(s_lamp_dot, 14, 0);
    lv_obj_set_style_shadow_opa(s_lamp_dot, 90, 0);
    lv_obj_remove_flag(s_lamp_dot, LV_OBJ_FLAG_CLICKABLE);
    // A readout, not a control. Tapping it does nothing on purpose.
    s_lamp_lbl = wt_lbl(p, "", 0, 120, wt_font_mono23(), WT_OK);

    s_cmp_lbl = wt_lbl(p, tr(STR_S_CMP_8), RECV_COL_X, 198, wt_font23(),
                       WT_DIM);
    lv_obj_set_width(s_cmp_lbl, RECV_COL_W);
    lv_label_set_long_mode(s_cmp_lbl, LV_LABEL_LONG_DOT);

    // The whole address block is ONE tap target: folded to whole and back.
    // This tab is the only place on the device a full receive address renders
    // as text; everything else folds and points here. Built before refresh so
    // the first render can size the caption against whichever form is up.
    s_addr_hit = lv_obj_create(p);
    lv_obj_remove_style_all(s_addr_hit);
    lv_obj_set_pos(s_addr_hit, RECV_COL_X - 8, 150);
    lv_obj_set_size(s_addr_hit, RECV_COL_W + 16, 96);
    lv_obj_remove_flag(s_addr_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_addr_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_translate_x(s_addr_hit, 0, 0);
    lv_obj_set_style_translate_x(s_addr_hit, 4, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_addr_hit, addr_toggle_cb, LV_EVENT_CLICKED, NULL);

    // The derivation path, reduced to what the bench asked for: the digits,
    // quietly, with the "?" beside them -- no caption row, no second typeface.
    // mono18 is DATA at metadata weight; the whole line is the tap target and
    // the mark is the sign that says so.
    lv_obj_t *ph = lv_obj_create(p);
    lv_obj_remove_style_all(ph);
    lv_obj_set_pos(ph, RECV_COL_X, 356);
    lv_obj_set_size(ph, RECV_COL_W, 34);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ph, path_help_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_translate_x(ph, 0, 0);
    lv_obj_set_style_translate_x(ph, 4, LV_STATE_PRESSED);
    // A 30px CHIP, not the 19px sign. wt_help_mark is a mark on a target you
    // cannot miss; this one sits at the bottom of a column beside an address,
    // and it came back from the bench as "tiny ass question mark (make it
    // bigger)". The whole line still takes the tap -- the chip is the sign
    // that says so, at the size the rest of the device signs a question.
    wt_help_chip(ph, RECV_COL_W - 30 - 2, 2, wt_accent(), path_help_cb, NULL);
    s_path_lbl = wt_lbl(ph, "", 0, 0, wt_font_mono18(), WT_DIM);

    recv_refresh();
    return;
  }

  if (s_rctx.tab == 1) {
    // Three whole lines on the lane, a page at a time -- the SIGN list's
    // shape, here because the owner asked for its swipe on every list. The
    // scroll window, its settle correction and the two arrow chips all went
    // with it: a stroke turns the page, and the page IS the window, so
    // nothing can come to rest cut through its own caption any more.
    // 76, because the caption is a WORD at font23 and a row holds it over a
    // mono23 value.
    const int H = 76;
    char addr[91];
    uint32_t shown = 0;

    for (int i = 0; i < RECV_PAGE; i++) {
      const uint32_t idx = s_list_base + (uint32_t)i;
      if (idx >= RECV_LIST_CAP) break;
      char cap[24];
      snprintf(cap, sizeof cap, tr(STR_R_ADDR_N_FMT), (unsigned)idx);
      const bool u = recv_used(idx);
      lv_obj_t *row = wt_line_row(p, X, 120 + i * H, W, H, cap, NULL, NULL,
                                  WT_INK,
                                  tr(u ? STR_R_HANDED_ALREADY
                                       : STR_R_NEVER_HANDED),
                                  wt_font23(), row_tap_cb,
                                  (void *)(uintptr_t)idx);
      // The sub is the state, so it wears the state's colour rather than the
      // sub's grey. UNUSED means here what it means in the lamp.
      lv_obj_t *sub = lv_obj_get_child(row, -1);
      lv_obj_set_style_text_color(sub, u ? WT_MUT : WT_OK, 0);
      if (kiss_session_address(0, idx, addr, sizeof addr) != 0)
        snprintf(addr, sizeof addr, "%s", tr(STR_C_SESSION_LOCKED));
      lv_obj_t *sg = wt_addr_short(row, addr, wt_font_mono23());
      lv_obj_set_pos(sg, WT_LINE_PAD, wt_line_val_y());
      wt_line_rule_draw(wt_line_rule(row, 0, H - 1, W), 42 * i + 110, 320);
      shown++;
    }

    // 34 pages is a ruler, not an indicator, so wt_pager_line draws no dots
    // here and the count line keeps the whole lane. The line is the position.
    char count[160];
    snprintf(count, sizeof count, tr(STR_R_LIST_COUNT),
             (unsigned)(s_list_base + 1), (unsigned)(s_list_base + shown),
             (unsigned)RECV_LIST_CAP);
    wt_pager_line(p, count, false, (int)(s_list_base / RECV_PAGE),
                  (RECV_LIST_CAP + RECV_PAGE - 1) / RECV_PAGE);
    return;
  }

  // Tab 3. Two lines and the sentence the current screen never says on the
  // screen itself: what a silent payment IS, in two clauses.
  const int H = 76;
  char sp[128];
  if (kiss_session_sp_address(sp, sizeof sp) != 0)
    snprintf(sp, sizeof sp, "%s", tr(STR_C_SESSION_LOCKED));
  lv_obj_t *r1 = wt_line_row(p, X, 120, W, H, tr(STR_R_SP_ADDR_CAP), NULL,
                             NULL, WT_INK, tr(STR_R_SP_QR_SUB), NULL,
                             sp_open_cb, NULL);
  lv_obj_t *sg = wt_addr_short(r1, sp, wt_font_mono23());
  lv_obj_set_pos(sg, WT_LINE_PAD, wt_line_val_y());
  wt_line_rule_draw(wt_line_rule(p, X, 120 + H, W), 110, 320);
  // A door again, and the SAME door. The export hands a coordinator a PRIVATE
  // key and has one consent flow, in kiss_info -- this row opens that exact
  // gate (kiss_info_open_scan_key) and comes back here when the owner leaves.
  // The dead-label version of this row was filed from the bench as "doesn't
  // actually allow to show the SCAN KEY": a fact with no door read as a
  // broken control, not as a signpost.
  wt_line_row(p, X, 196, W, H, tr(STR_R_SP_SCAN_BTN), NULL, NULL, WT_INK,
              tr(STR_K_SP_SUB), NULL, sp_scan_key_cb, NULL);
  wt_line_rule_draw(wt_line_rule(p, X, 196 + H, W), 152, 320);

  // What a silent payment buys, one mark and one line each -- no paragraph,
  // no box. The three claims the owner kept asking for: one address, no
  // reuse on chain, nothing for a watcher to connect.
  {
    static const char *const SP_ICONS[3] = {
        LV_SYMBOL_LOOP, LV_SYMBOL_SHUFFLE, WT_ICON_HIDDEN };
    const char *const lines[3] = {
        tr(STR_R_EXPL_SP), tr(STR_R_SP_FRESH), tr(STR_R_SP_PRIV) };
    for (int i = 0; i < 3; i++) {
      const int y = 288 + i * 36;
      lv_obj_t *ic = wt_lbl(p, SP_ICONS[i], X, y, wt_font23(), wt_accent());
      lv_obj_add_flag(ic, WT_FLAG_ACCENT);
      lv_obj_t *l = wt_lbl(p, lines[i], X + 40, y, wt_font23(), WT_MUT);
      lv_obj_set_width(l, W - 40);
      lv_obj_set_height(l, lv_font_get_line_height(wt_font23()));
      lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
  }
}

static void recv_detail_open(void) {
  s_addr_sg = NULL;
  s_pop = s_pop_away = NULL;
  s_help_open = false;
  s_scr = wt_chrome(s_parent, tr(STR_R_T));
  // No subtitle. "trust what you see here, not your computer screen" is
  // anti-phishing advice about ONE address, and the line under the path row
  // now says the thing this screen actually needs said, where it is needed.

  wt_tab_t t[3] = {
      { .icon = WT_ICON_QR,      .label = tr(STR_R_TAB_THIS) },
      { .icon = WT_ICON_LIST,    .label = tr(STR_R_ALL_ADDR) },
      // SILENT, not SILENT PAYMENT: the flex strip sizes its brackets to the
      // words, but three labels still share the 620 the [ ? ] divider
      // leaves, and the full term is the one that starves the other two.
      { .icon = WT_ICON_SECRET,  .label = tr(STR_R_TAB_SP) },
  };
  s_rctx.scr    = s_scr;
  s_rctx.select = wt_tabs_flex_select;
  s_rctx.tabs   = wt_tabs_flex(s_scr, t, 3, s_rctx.tab, recv_tab_cb);
  wt_pane_tabs_watch(&s_rctx);
  wt_swipe_watch(s_scr, recv_gesture_cb);
  // No band hint: this band's left lane belongs to VERIFY, so the mark's
  // breathing is the whole first-run invitation here.
  wt_help_tab(s_scr, NULL, recv_help_cb, NULL);
  s_rctx.pane = wt_pane_new(&s_rctx);
  recv_tab_build();

  // Three arrows, the action row's three positions. VERIFY is the primary and
  // takes the accent on its LABEL as well: there is no filled primary left to
  // give it, and none is wanted -- a fill is a box.
  wt_arrow_action(s_scr, tr(STR_R_VERIFY), false, true, WT_ACT_X, WT_ACTION_Y,
                  0, false, vfy_scan, NULL);
  // Held by the tab logic: NEXT ADDRESS belongs to THIS ADDRESS alone. On the
  // other tabs it used to advance the hidden index with nothing on screen
  // moving -- invisible state mutation wearing a working control's clothes.
  s_next_act = wt_arrow_action(s_scr, tr(STR_R_NEXT_ADDR), false, false, 300,
                               WT_ACTION_Y, 0, false, next_cb, NULL);
  recv_next_vis();
  wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                  true, close_cb, NULL);
}

static void recv_open_at(lv_obj_t *parent, int tab) {
  if (s_scr) return;
  s_parent = parent;
  s_addr_sg = NULL;
  // Fresh entries land where the caller says, and the fold is a view, not a
  // memory. The tab used to survive a full close: the mainnet walk re-entered
  // "THIS ADDRESS" and photographed the SILENT tab instead.
  s_rctx.tab = tab;
  s_addr_full = false;

  // Figure out the freshest address to land on. Key by wallet + network + type;
  // a switch resets the session view-history to the persisted used-high,
  // otherwise keep growing it (a sign this session may have bumped it).
  uint8_t fp[4];
  kiss_ui_last_fp(fp);
  char key[16];
  snprintf(key, sizeof key, "%02x%02x%02x%02x%d%d", fp[0], fp[1], fp[2], fp[3],
           kiss_testnet() ? 1 : 0, kiss_script());
  int used = kiss_usage_high(fp, kiss_testnet() ? 1 : 0, kiss_script());
  if (strcmp(key, s_seen_key) != 0) {          // different wallet/net/type
    snprintf(s_seen_key, sizeof s_seen_key, "%s", key);
    s_seen_high = used;
  } else if (used > s_seen_high) {
    s_seen_high = used;
  }
  s_idx = s_seen_high < 0 ? 0 : (uint32_t)(s_seen_high + 1);

  // Open on the page that holds the fresh address, aligned to a page boundary
  // so the ALL ADDRESSES list still lands on the right page if the user asks
  // for it from the detail screen ("4 to 6 of 100" reads round).
  uint32_t fresh = s_idx < RECV_LIST_CAP ? s_idx : RECV_LIST_CAP - 1;
  s_list_base = (fresh / RECV_PAGE) * RECV_PAGE;

  // Per HANDOFF-03: RECEIVE lands on one address, not on a hundred. The list
  // is one tap away behind ALL ADDRESSES; the default is the freshest.
  recv_detail_open();
}

void kiss_recv_open(lv_obj_t *parent) { recv_open_at(parent, 0); }

// The scan key gate's way home: the SILENT tab, not the landing tab.
void kiss_recv_open_sp(lv_obj_t *parent) { recv_open_at(parent, 2); }

// KEYS' FIRST ADDRESS rows land here: THIS ADDRESS at index 0, the address
// those rows fold -- the one place on the device a full address shows.
void kiss_recv_open_first(lv_obj_t *parent) {
  recv_open_at(parent, 0);
  if (!s_scr) return;
  s_idx = 0;
  recv_refresh();
}
