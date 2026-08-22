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

// How many addresses the list offers at once. Twenty is two chains' worth of
// ordinary use and costs 40 child derivations at open — well inside what this
// screen already does elsewhere, since VERIFY's ownership search runs up to
// VFY_SCAN_DEPTH on BOTH chains (200) in one go and has always been fine.
// It is only affordable at all because the account key is cached now; without
// that, each row would pay for three hardened derivations of its own.
#define RECV_LIST_N 20
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
#define RECV_LIST_H (6 * ROW_PITCH)

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
static lv_obj_t *s_state_chip;
// The card the address lives in on the detail screen, and the caption inside it.
// recv_refresh rebuilds the spans on every NEXT, so both have to outlive one
// refresh: the spans are children of the card and are placed against the
// caption, which is a child of the card too.
static lv_obj_t *s_addr_card, *s_cmp_lbl;
static lv_obj_t *s_addr_more;   // "FULL ADDRESS" / "SHORT", the fold's own label
static lv_obj_t *s_sp_path_lbl, *s_sp_path_sec, *s_sp_back_pill, *s_sp_toggle_pill;
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
  s_sp_back_pill = s_sp_toggle_pill = s_sp_addr_hit = NULL;
  s_sp_card = NULL;
  s_state_chip = NULL;
  s_addr_card = s_cmp_lbl = s_addr_more = NULL;
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
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
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
  (void)len;
  // must hold a silent-payment address (~117 chars) whole: a truncated address
  // silently becomes a DIFFERENT address, which is the one thing this screen
  // exists to rule out. grouped adds a space every 4 chars.
  char addr[128], grouped[200], buf[200];  // translated line, 3 bytes/char worst
  vfy_norm(txt, addr, sizeof addr);
  int change = 0;
  uint32_t idx = 0;
  int mine = vfy_find(addr, &change, &idx);
  int sp_mine = !mine && vfy_is_sp_mine(addr);
  int validity = (mine || sp_mine) ? WADDR_CURRENT_NETWORK
                                   : kiss_address_validate(addr);

  s_scr = wt_screen(s_parent, tr(STR_R_VT), tr(STR_R_VS));
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
                                wt_body_font(buf, 700, 44), WT_WARN);
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

  lv_obj_t *again = wt_pill(s_scr, tr(STR_R_SCAN_ANOTHER), 48, WT_ACTION_Y, 220, vfy_scan, NULL);
  wt_pill_primary(again);
  wt_pill(s_scr, tr(STR_C_DONE), WT_BACK_X, WT_ACTION_Y, 140, vfy_done_cb, NULL);
}

static void vfy_cancel(void) {
  kiss_recv_open(s_parent);            // backed out of the camera: back to Receive
}

static void vfy_scan(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  kiss_scan_open_raw(s_parent, vfy_result, vfy_cancel);
}

// ---- silent payment (BIP352) static receive address ----
// One reusable sp1/tsp1, derived on-device (m/352'). No index, no reuse guard:
// a silent-payment address is meant to be shared and reused; that's the point.

static void sp_back_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  s_sp_path_lbl = s_sp_path_sec = NULL;
  s_sp_back_pill = s_sp_toggle_pill = s_sp_addr_hit = NULL;
  s_sp_card = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  kiss_recv_open(s_parent);
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

  lv_obj_t *toggle_lbl = lv_obj_get_child(s_sp_toggle_pill, 0);
  const char *toggle_txt = tr(s_sp_full ? STR_R_SP_SHOW_SHORT
                                        : STR_R_SP_SHOW_FULL);
  lv_label_set_text(toggle_lbl, toggle_txt);
  wt_pill_apply_fit(s_sp_toggle_pill,
                    wt_pill_fit(toggle_txt, 280, 52, false), 280);
  lv_obj_t *row[2] = {s_sp_back_pill, s_sp_toggle_pill};
  wt_pill_row(row, 2);
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

  s_sp_back_pill = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                           sp_back_cb, NULL);
  s_sp_toggle_pill = wt_pill(s_scr, tr(STR_R_SP_SHOW_FULL),
                             WT_ACT_X, WT_ACTION_Y, 280, sp_toggle_cb, NULL);
  s_sp_addr_hit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_sp_addr_hit);
  lv_obj_set_style_radius(s_sp_addr_hit, 8, 0);
  lv_obj_add_flag(s_sp_addr_hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_sp_addr_hit, LV_OBJ_FLAG_SCROLLABLE);
  wt_tap_feedback(s_sp_addr_hit);
  lv_obj_add_event_cb(s_sp_addr_hit, sp_toggle_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *row[2] = {s_sp_back_pill, s_sp_toggle_pill};
  wt_pill_row(row, 2);
  sp_addr_render();
}

static void sp_open_cb(lv_event_t *e) {
  (void)e;
  s_sp_full = false;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  sp_addr_open(s_parent);
}

// ---- RECEIVE: three tabs on one lane ----
// THIS ADDRESS, ALL ADDRESSES and SILENT PAYMENT were a screen, a screen one
// tap in from it, and a row in a column. They are three tabs now, on the same
// bracket strip KEYS wears, and the two destination rows that used to eat the
// bottom of the detail screen are the strip.
static wt_pane_t s_rctx;
// The kit keeps its own exec callbacks private, and these two are a line each.
static void wt_anim_ty(void *v, int32_t y)  { lv_obj_set_style_translate_y(v, y, 0); }
static void wt_anim_opa(void *v, int32_t o) { lv_obj_set_style_opa(v, (lv_opa_t)o, 0); }
static lv_obj_t *s_lamp_dot, *s_lamp_lbl;
static lv_obj_t *s_expl;              // the line under the path row, tab 1
static lv_obj_t *s_pop;               // the index popover, or NULL
static void recv_tab_build(void);
static void recv_detail_open(void);

#define RECV_COL_X 296
#define RECV_COL_W 456
#define RECV_PAGE_N 4                 // lines on ALL ADDRESSES, one page

static void pop_close(void) {
  if (s_pop) { lv_obj_delete(s_pop); s_pop = NULL; }
}

static void recv_tab_cb(lv_event_t *e) {
  pop_close();
  wt_pane_go(&s_rctx, (int)(intptr_t)lv_event_get_user_data(e), false,
             recv_tab_build);
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
static void lamp_set(bool used) {
  if (!s_lamp_dot || !s_lamp_lbl) return;
  lv_color_t col = used ? WT_WARN : WT_OK;
  lv_obj_set_style_bg_color(s_lamp_dot, col, 0);
  lv_obj_set_style_shadow_color(s_lamp_dot, col, 0);
  lv_label_set_text(s_lamp_lbl, tr(used ? STR_R_HANDED_ALREADY
                                        : STR_R_NEVER_HANDED));
  lv_obj_set_style_text_color(s_lamp_lbl, col, 0);
  lv_obj_update_layout(s_lamp_lbl);
  // Right aligned to 752 and recomputed every refresh: the two words are
  // different lengths, and more so per locale.
  lv_obj_set_pos(s_lamp_lbl, 752 - lv_obj_get_width(s_lamp_lbl), 120);
  lv_obj_set_pos(s_lamp_dot, 752 - lv_obj_get_width(s_lamp_lbl) - 8 - 14, 126);
}

static bool recv_used(uint32_t idx) {
  uint8_t fp[4];
  kiss_ui_last_fp(fp);
  int high = kiss_usage_high(fp, kiss_testnet() ? 1 : 0, kiss_script());
  return high >= 0 && (int)idx <= high;
}

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
                       wt_font23(), WT_WARN);
    if (!s_lock_note)
      s_lock_note = wt_note(par, tr(STR_C_LOCKED_B), 48, 130, 216, 200);
  } else {
    if (s_qr) wt_qr_update(s_qr, addr, (uint32_t)strlen(addr));
    // ONE object, one line, never wrapped -- the same rule every address on
    // this device follows. 456 at mono23 holds the fold with room; if a future
    // prefix pushes it, the air around the ellipsis goes, never a block and
    // never the font size.
    s_addr_sg = wt_addr_short(par, addr, wt_font_mono23());
    lv_obj_set_pos(s_addr_sg, RECV_COL_X, 160);
  }
  // The caption goes with the address it captions: "compare the lit
  // characters" under a state word is an instruction with no object.
  if (s_cmp_lbl) {
    if (rc != 0) lv_obj_add_flag(s_cmp_lbl, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_remove_flag(s_cmp_lbl, LV_OBJ_FLAG_HIDDEN);
  }

  if (s_idx_lbl) lv_label_set_text_fmt(s_idx_lbl, tr(STR_R_ADDR_N_FMT),
                                       (unsigned)s_idx);
  if (s_path_lbl) {
    int purpose = kiss_script() == WSCRIPT_LEGACY ? 44
                : kiss_script() == WSCRIPT_NESTED ? 49 : 84;
    lv_label_set_text_fmt(s_path_lbl, "m/%dh/%dh/0h/0/%u",
                          purpose, kiss_testnet() ? 1 : 0, (unsigned)s_idx);
    lv_obj_update_layout(s_path_lbl);
    lv_obj_set_pos(s_path_lbl, 752 - 6 - lv_obj_get_width(s_path_lbl), 259);
  }

  if ((int)s_idx > s_seen_high) s_seen_high = (int)s_idx;   // seeds next open's landing

  // Used and unused is a claim of the same shape as what kiss_usage_high
  // actually holds: an address this device signed a spend from is on chain, so
  // USED is certain. UNUSED means no record here, which is what the standard
  // term means in any watch-only wallet too.
  const bool used = recv_used(s_idx);
  lamp_set(used);
  // The line under the path row answers the lamp. On an unused address it is
  // the privacy rule; on a used one it is what to do about it, in amber,
  // because at that point the rule has already been broken once.
  if (s_expl) {
    wt_note_fit(s_expl, tr(used ? STR_R_USED_NOTE : STR_R_ONE_EACH_SHORT),
                RECV_COL_W, WT_CONTENT_BOTTOM - 300);
    lv_obj_set_style_text_color(s_expl, used ? WT_WARN : WT_MUT, 0);
  }
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

static void next_cb(lv_event_t *e) {
  (void)e;
  pop_close();
  // The next UNUSED index, not merely the next one: the button's whole promise
  // is a fresh address, and stepping onto one this signer has already shown
  // would break it silently.
  do { s_idx++; } while (recv_used(s_idx) && s_idx < RECV_LIST_CAP);
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

static void pop_open(void) {
  if (s_pop) { pop_close(); return; }        // a second tap closes it
  lv_obj_t *par = s_rctx.pane ? s_rctx.pane : s_scr;
  // The current index plus or minus two, clamped to the real range. NOT a
  // hundred: a long list is what ALL ADDRESSES is for, and a popover that
  // scrolls is a list wearing a shadow.
  int lo = (int)s_idx - 2, hi = (int)s_idx + 2;
  if (lo < 0) { hi -= lo; lo = 0; }
  if (hi >= RECV_LIST_CAP) hi = RECV_LIST_CAP - 1;
  const int n = hi - lo + 1;
  const int item_h = 44;

  s_pop = lv_obj_create(par);
  lv_obj_remove_style_all(s_pop);
  lv_obj_set_pos(s_pop, RECV_COL_X, 148);
  lv_obj_set_size(s_pop, 300, n * item_h);
  lv_obj_set_style_radius(s_pop, 8, 0);
  lv_obj_set_style_bg_color(s_pop, WT_BAR, 0);
  lv_obj_set_style_bg_opa(s_pop, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_pop, 1, 0);
  lv_obj_set_style_border_color(s_pop, wt_accent(), 0);
  lv_obj_set_style_border_opa(s_pop, 115, 0);
  lv_obj_add_flag(s_pop, WT_FLAG_ACCENT_BORDER);
  lv_obj_set_style_shadow_width(s_pop, 40, 0);
  lv_obj_set_style_shadow_offset_y(s_pop, 18, 0);
  lv_obj_set_style_shadow_color(s_pop, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(s_pop, LV_OPA_60, 0);
  lv_obj_remove_flag(s_pop, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < n; i++) {
    const uint32_t idx = (uint32_t)(lo + i);
    const bool sel = idx == s_idx;
    lv_obj_t *it = lv_obj_create(s_pop);
    lv_obj_remove_style_all(it);
    lv_obj_set_pos(it, 0, i * item_h);
    lv_obj_set_size(it, 300, item_h);
    lv_obj_remove_flag(it, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(it, wt_accent_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(it, pop_pick_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)idx);
    // The tick is built on EVERY item and hidden with opacity, the same
    // discipline as the tab brackets: an item that gains a glyph on selection
    // reflows the row under the finger that just picked it.
    lv_obj_t *ok = wt_lbl(it, LV_SYMBOL_OK, 12, 0, wt_font14(),
                          wt_accent());
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
                          0, 0, wt_font_mono14(), u ? WT_DIM : WT_OK);
    lv_obj_set_style_text_letter_space(st, 2, 0);
    lv_obj_align(st, LV_ALIGN_RIGHT_MID, -14, 0);
    if (i < n - 1) wt_line_rule(it, 0, item_h - 1, 300);
  }
  // A tap anywhere else closes it. The catcher is built LAST so it sits under
  // nothing and over everything else on the pane, and it is a sibling of the
  // popover rather than its parent so a pick lands on the item.
  lv_obj_t *away = lv_obj_create(par);
  lv_obj_remove_style_all(away);
  lv_obj_set_pos(away, 0, 0);
  lv_obj_set_size(away, 800, 480);
  lv_obj_add_flag(away, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(away, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(away, pop_dismiss_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_move_to_index(away, lv_obj_get_index(s_pop));

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

static void pop_toggle_cb(lv_event_t *e) { (void)e; pop_open(); }

static void path_help_cb(lv_event_t *e) {
  (void)e;
  // The explainer already exists and already says "m/...: the branch your
  // coordinator wallet follows to find your keys." KEYS' ADDRESS TYPE line
  // opens the same one. A second explainer for the same value would be the
  // device teaching one lesson twice.
  wt_explain_t x = {
      .title  = tr(STR_I_SEC_TYPE),
      .icon   = LV_SYMBOL_DIRECTORY,
      .body   = tr(STR_I_H_TYPE_B),
      .ok_txt = tr(STR_C_OK),
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

static void page_cb(lv_event_t *e) {
  int step = (int)(intptr_t)lv_event_get_user_data(e);
  int base = (int)s_list_base + step * RECV_PAGE_N;
  if (base < 0 || base >= RECV_LIST_CAP) return;     // ends of the range: no wrap
  s_list_base = (uint32_t)base;
  wt_pane_go(&s_rctx, 2, false, recv_tab_build);     // same tab: rebuild in place
}

// ---- tab 3: SILENT PAYMENT ----
// Coming back from either destination lands on the tab it left from, which is
// what the context remembers the tab for.
static void sp_return(void) {
  s_rctx.tab = 2;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  recv_detail_open();
}

static void sp_key_export_cb(lv_event_t *e) {
  (void)e;
  // One implementation of the export, in kiss_info.c, told where BACK goes.
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  kiss_info_open_scan_key(s_parent, sp_return);
}

// ---- the three groups ----
static void recv_tab_build(void) {
  lv_obj_t *p = s_rctx.pane;
  const int X = 48, W = 704;
  s_qr = s_addr_sg = s_idx_lbl = s_path_lbl = s_lock_note = NULL;
  s_cmp_lbl = s_lamp_dot = s_lamp_lbl = s_expl = NULL;
  s_state_chip = NULL;

  if (s_rctx.tab == 0) {
    // Left column: the QR and the one line that says it opens.
    wt_qr_card(p, &s_qr, X, 120, 216, 180);
    lv_obj_t *hit = lv_obj_create(p);
    lv_obj_remove_style_all(hit);
    lv_obj_set_pos(hit, X, 340);
    lv_obj_set_size(hit, 216, 26);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, enlarge_cb, LV_EVENT_CLICKED, NULL);
    // Shifts 4px right while held, the arrow action's answer at a smaller
    // scale: this line is a direction too.
    lv_obj_set_style_translate_x(hit, 0, 0);
    lv_obj_set_style_translate_x(hit, 4, LV_STATE_PRESSED);
    lv_obj_t *ic = wt_lbl(hit, LV_SYMBOL_EYE_OPEN, 0, 3, wt_font14(),
                          wt_accent());
    lv_obj_add_flag(ic, WT_FLAG_ACCENT);
    lv_obj_update_layout(ic);
    lv_obj_t *el = wt_lbl(hit, tr(STR_R_ENLARGE),
                          lv_obj_get_width(ic) + 10, 3, wt_font_mono14(),
                          WT_MUT);
    lv_obj_set_style_text_letter_space(el, 3, 0);

    // Right column. The caption is the popover's control, so it carries the
    // chevron that says so and the whole pair is one target.
    lv_obj_t *ih = lv_obj_create(p);
    lv_obj_remove_style_all(ih);
    lv_obj_set_pos(ih, RECV_COL_X, 114);
    lv_obj_set_size(ih, 200, 28);
    lv_obj_remove_flag(ih, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ih, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ih, pop_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_idx_lbl = wt_lbl(ih, "", 0, 6, wt_font_mono14(), wt_accent());
    lv_obj_set_style_text_letter_space(s_idx_lbl, 3, 0);
    lv_obj_add_flag(s_idx_lbl, WT_FLAG_ACCENT);
    lv_obj_update_layout(s_idx_lbl);
    lv_obj_t *cv = wt_lbl(ih, LV_SYMBOL_DOWN, 0, 7, wt_font14(), wt_accent());
    lv_obj_add_flag(cv, WT_FLAG_ACCENT);
    lv_obj_set_pos(cv, 112, 7);

    s_lamp_dot = lv_obj_create(p);
    lv_obj_remove_style_all(s_lamp_dot);
    lv_obj_set_size(s_lamp_dot, 8, 8);
    lv_obj_set_style_radius(s_lamp_dot, 4, 0);
    lv_obj_set_style_bg_opa(s_lamp_dot, LV_OPA_COVER, 0);
    // The glow. If it is ever expensive on glass, this goes before the dot
    // does: the dot is the readout, the glow is what makes it look lit.
    lv_obj_set_style_shadow_width(s_lamp_dot, 10, 0);
    lv_obj_set_style_shadow_opa(s_lamp_dot, 60, 0);
    lv_obj_remove_flag(s_lamp_dot, LV_OBJ_FLAG_CLICKABLE);
    // A readout, not a control. Tapping it does nothing on purpose.
    s_lamp_lbl = wt_lbl(p, "", 0, 120, wt_font_mono14(), WT_OK);
    lv_obj_set_style_text_letter_space(s_lamp_lbl, 3, 0);

    s_cmp_lbl = wt_lbl(p, tr(STR_S_CMP_8), RECV_COL_X, 200, wt_font14(),
                       WT_DIM);
    lv_obj_set_width(s_cmp_lbl, RECV_COL_W);
    lv_label_set_long_mode(s_cmp_lbl, LV_LABEL_LONG_DOT);
    wt_line_rule(p, RECV_COL_X, 242, RECV_COL_W);

    // The path row runs 12 LEFT of the column, so the pressed rail sits
    // outside the text lane rather than under the first letter of its label.
    lv_obj_t *pr = lv_obj_create(p);
    lv_obj_remove_style_all(pr);
    lv_obj_set_pos(pr, RECV_COL_X - 12, 254);
    lv_obj_set_size(pr, RECV_COL_W + 12, 36);
    lv_obj_remove_flag(pr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pr, path_help_cb, LV_EVENT_CLICKED, NULL);
    wt_line_press(pr);
    lv_obj_t *pc = wt_lbl(pr, tr(STR_R_PATH_CAP), 12, 0, wt_font_mono14(),
                          WT_DIM);
    lv_obj_set_style_text_letter_space(pc, 3, 0);
    lv_obj_align(pc, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_update_layout(pc);
    // The whole row is the target; the "?" is the sign that says so.
    wt_help_mark(pr, 12 + lv_obj_get_width(pc) + 10, 8);
    s_path_lbl = wt_lbl(p, "", 0, 259, wt_font_mono23(), wt_accent());
    lv_obj_add_flag(s_path_lbl, WT_FLAG_ACCENT);

    s_expl = wt_lbl(p, "", RECV_COL_X, 300, wt_font23(), WT_MUT);
    lv_obj_set_width(s_expl, RECV_COL_W);
    lv_label_set_long_mode(s_expl, LV_LABEL_LONG_WRAP);
    recv_refresh();
    return;
  }

  if (s_rctx.tab == 1) {
    // Four lines at 56, not the 60 the handoff draws. 60 puts the last rule on
    // 360 and leaves the count line 26px, which is one font14 line -- and the
    // second half of that string is the sentence explaining what the list IS.
    // Four pixels a row buys it 42 and the readable rung. The rule about not
    // shrinking type to fit a layout wins over four pixels of gap.
    const int H = 56;
    char addr[91];
    uint32_t shown = 0;
    for (int i = 0; i < RECV_PAGE_N; i++) {
      const uint32_t idx = s_list_base + (uint32_t)i;
      if (idx >= RECV_LIST_CAP) break;
      const int y = 120 + i * H;
      char cap[24];
      snprintf(cap, sizeof cap, tr(STR_R_ADDR_N_FMT), (unsigned)idx);
      const bool u = recv_used(idx);
      lv_obj_t *row = wt_line_row(p, X, y, W, H, cap, NULL, NULL, WT_INK,
                                  tr(u ? STR_R_HANDED_ALREADY
                                       : STR_R_NEVER_HANDED),
                                  wt_font_mono14(), row_tap_cb,
                                  (void *)(uintptr_t)idx);
      // The sub is the state, so it wears the state's colour rather than the
      // sub's grey. UNUSED is the only green on this page and it means the
      // same thing it means in the lamp.
      lv_obj_t *sub = lv_obj_get_child(row, -1);
      lv_obj_set_style_text_color(sub, u ? WT_MUT : WT_OK, 0);
      if (kiss_session_address(0, idx, addr, sizeof addr) != 0)
        snprintf(addr, sizeof addr, "%s", tr(STR_C_SESSION_LOCKED));
      lv_obj_t *sg = wt_addr_short(row, addr, wt_font_mono23());
      lv_obj_set_pos(sg, WT_LINE_PAD, wt_line_val_y());
      wt_line_rule(p, X, y + H, W);
      shown++;
    }
    lv_obj_t *note = wt_lbl(p, "", X, 356, wt_font23(), WT_MUT);
    lv_obj_set_width(note, W - 110);
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(note, tr(STR_R_LIST_COUNT),
                          (unsigned)(s_list_base + 1),
                          (unsigned)(s_list_base + shown),
                          (unsigned)RECV_LIST_CAP);
    // Paging lives IN the content, at the count line's right edge. The action
    // bar is full, and these two move the list rather than leaving the screen,
    // which is not what a bar is for.
    for (int i = 0; i < 2; i++) {
      const bool fwd = i == 1;
      const int step = fwd ? 1 : -1;
      const int base = (int)s_list_base + step * RECV_PAGE_N;
      lv_obj_t *pa = lv_obj_create(p);
      lv_obj_remove_style_all(pa);
      lv_obj_set_size(pa, 36, 36);
      lv_obj_set_pos(pa, fwd ? 752 - 36 : 752 - 36 - 44, 352);
      lv_obj_remove_flag(pa, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_t *g = wt_lbl(pa, fwd ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, 0, 0,
                           wt_font23(), wt_accent());
      lv_obj_add_flag(g, WT_FLAG_ACCENT);
      lv_obj_center(g);
      if (base < 0 || base >= RECV_LIST_CAP) {
        // Spent, not missing: half opacity, no click flag, no feedback, so it
        // neither lights up nor answers.
        lv_obj_set_style_opa(pa, LV_OPA_40, 0);
      } else {
        lv_obj_add_flag(pa, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_translate_x(pa, 0, 0);
        lv_obj_set_style_translate_x(pa, fwd ? 5 : -5, LV_STATE_PRESSED);
        lv_obj_add_event_cb(pa, page_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)step);
      }
    }
    return;
  }

  // Tab 3. Two lines and the sentence the current screen never says on the
  // screen itself: what a silent payment IS, in two clauses.
  const int H = 66;
  char sp[128];
  if (kiss_session_sp_address(sp, sizeof sp) != 0)
    snprintf(sp, sizeof sp, "%s", tr(STR_C_SESSION_LOCKED));
  lv_obj_t *r1 = wt_line_row(p, X, 120, W, H, tr(STR_R_SP_ADDR_CAP), NULL,
                             NULL, WT_INK, tr(STR_R_SP_QR_SUB), NULL,
                             sp_open_cb, NULL);
  lv_obj_t *sg = wt_addr_short(r1, sp, wt_font_mono23());
  lv_obj_set_pos(sg, WT_LINE_PAD, wt_line_val_y());
  wt_line_rule(p, X, 120 + H, W);
  wt_line_row(p, X, 186, W, H, tr(STR_R_SP_SCAN_BTN), tr(STR_R_SP_EXPORT),
              wt_font23(), WT_INK, tr(STR_K_SP_SUB), NULL,
              sp_key_export_cb, NULL);
  wt_line_rule(p, X, 186 + H, W);
  wt_note(p, tr(STR_R_EXPL_SP), X + 14, 274, W - 28,
          WT_CONTENT_BOTTOM - 274);
}

static void recv_detail_open(void) {
  s_addr_sg = NULL;
  s_pop = NULL;
  s_scr = wt_screen(s_parent, tr(STR_R_T), NULL);
  wt_title_fit(s_scr, 704);
  wt_title_cursor(s_scr);
  // No subtitle. "trust what you see here, not your computer screen" is
  // anti-phishing advice about ONE address, and the line under the path row
  // now says the thing this screen actually needs said, where it is needed.

  wt_tab_t t[3] = {
      { .icon = WT_ICON_QR,      .label = tr(STR_R_TAB_THIS) },
      { .icon = LV_SYMBOL_LIST,  .label = tr(STR_R_ALL_ADDR) },
      { .icon = WT_ICON_SECRET,  .label = tr(STR_R_SP_BTN) },
  };
  s_rctx.scr    = s_scr;
  s_rctx.select = wt_brackets_select;
  s_rctx.tabs   = wt_brackets(s_scr, t, 3, s_rctx.tab, 48, 70, 704,
                              recv_tab_cb);
  wt_pane_tabs_watch(&s_rctx);
  s_rctx.pane = wt_pane_new(&s_rctx);
  recv_tab_build();

  // Three arrows, the pill bar's three positions. VERIFY is the primary and
  // takes the accent on its LABEL as well: there is no filled primary left to
  // give it, and none is wanted -- a fill is a box.
  wt_arrow_action(s_scr, tr(STR_R_VERIFY), false, true, WT_ACT_X, WT_ACTION_Y,
                  0, false, vfy_scan, NULL);
  wt_arrow_action(s_scr, tr(STR_R_NEXT_ADDR), false, false, 300, WT_ACTION_Y,
                  0, false, next_cb, NULL);
  wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                  true, close_cb, NULL);
}

void kiss_recv_open(lv_obj_t *parent) {
  if (s_scr) return;
  s_parent = parent;
  s_addr_sg = NULL;

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
  // for it from the detail screen ("21 - 40 OF 100" reads round).
  uint32_t fresh = s_idx < RECV_LIST_CAP ? s_idx : RECV_LIST_CAP - 1;
  s_list_base = (fresh / RECV_LIST_N) * RECV_LIST_N;

  // Per HANDOFF-03: RECEIVE lands on one address, not on a hundred. The list
  // is one tap away behind ALL ADDRESSES; the default is the freshest.
  recv_detail_open();
}
