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
static bool s_addr_full;                   // detail address is folded by default
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

// ---- receive ----
static void recv_refresh(void) {
  char addr[91];
  int rc = kiss_session_address(0, s_idx, addr, sizeof(addr));
  // Refused = no QR. This square is the one a SENDER is invited to scan, and
  // the failure string used to go straight through wt_qr_update -- a scannable
  // code whose content was the words "SESSION LOCKED", offered as a payment
  // address. The words render once, in the address card, as a state.
  wt_qr_refusal(s_qr, rc != 0);
  if (rc == 0 && s_lock_note) { lv_obj_delete(s_lock_note); s_lock_note = NULL; }
  char grouped[120];
  if (s_addr_sg) lv_obj_delete(s_addr_sg);   // spans have no set_text: rebuild
  if (rc != 0) {
    lv_obj_t *par0 = s_addr_card ? s_addr_card : s_scr;
    s_addr_sg = wt_lbl(par0, tr(STR_C_SESSION_LOCKED), 14, 10,
                       wt_font23(), WT_WARN);
    // The hidden QR's own footprint carries the reassurance -- the one region
    // guaranteed empty in this state, and the place the eye goes looking for
    // the missing square. Created per refresh and deleted on recovery like
    // s_addr_sg, through the same handle discipline.
    if (!s_lock_note) {
      s_lock_note = wt_note(s_scr, tr(STR_C_LOCKED_B), 44, 130, 252, 220);
    }
  } else
  // The caption goes with the address it captions: "compare the lit
  // characters" under a state word is an instruction with no object.
  if (s_cmp_lbl) lv_obj_remove_flag(s_cmp_lbl, LV_OBJ_FLAG_HIDDEN);
  if (rc != 0 && s_cmp_lbl) lv_obj_add_flag(s_cmp_lbl, LV_OBJ_FLAG_HIDDEN);
  if (rc == 0)
  if (s_qr)
    wt_qr_update(s_qr, addr, (uint32_t)strlen(addr));

  // FOLDED by default, and folded is the same one line the address list draws:
  // constant prefix and middle muted, the four characters after the prefix and
  // the final four lit. Those eight are the ones worth comparing and the eight
  // the caption under them names, so the screen now shows exactly what it asks
  // you to check instead of 42 characters with a note about which 8 matter.
  //
  // Tapping the card unfolds it to every character, grouped in fours. Nothing is
  // hidden, it is one tap away, and the QR beside it has carried the whole
  // address the entire time. Same fold that the silent payment screen has always
  // had, so it is one behaviour on both address screens rather than two.
  lv_obj_t *par = s_addr_card ? s_addr_card : s_scr;
  if (rc != 0) {
    // rendered above; nothing address-shaped to fold
  } else if (s_addr_full) {
    wt_group4(addr, grouped, sizeof(grouped));
    s_addr_sg = wt_addr_spans(par, grouped, RECV_CARD_W - 28, RECV_ADDR_FONT);
  } else {
    s_addr_sg = wt_addr_short(par, addr, RECV_ADDR_FONT);
  }
  // The fold's own label, naming the state a tap moves TO, right-aligned in the
  // card's top corner so it clears the address block centred below it.
  if (s_addr_more) {
    lv_label_set_text(s_addr_more,
                      tr_sym(s_addr_full ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN,
                             s_addr_full ? STR_R_SP_SHOW_SHORT : STR_R_SP_SHOW_FULL));
    lv_obj_update_layout(s_addr_more);
    lv_obj_set_pos(s_addr_more,
                   RECV_CARD_W - 14 - lv_obj_get_width(s_addr_more), 8);
  }

  // Centred as a block, because the card's height is fixed for the taller state:
  // top-aligning would leave the folded line floating in a box half empty.
  if (s_addr_sg && s_cmp_lbl) {
    lv_obj_update_layout(s_addr_sg);
    lv_obj_update_layout(s_cmp_lbl);
    int ah = lv_obj_get_height(s_addr_sg), ch = lv_obj_get_height(s_cmp_lbl);
    int top = (RECV_CARD_H - (ah + 6 + ch)) / 2;
    if (top < 10) top = 10;
    lv_obj_set_pos(s_addr_sg, 14, top);
    lv_obj_set_pos(s_cmp_lbl, 14, top + ah + 6);
  }
  lv_label_set_text_fmt(s_idx_lbl, tr(STR_R_ADDR_N_FMT), (unsigned)s_idx);
  // The derivation path label is only wired up on the sub-screens that ask for
  // it (currently just the SP detail; HANDOFF-03 pulled it off the base
  // detail). If neither label was created this open, the refresh path skips
  // silently rather than call lv_label_set_text_fmt on NULL.
  if (s_path_lbl) {
    int purpose = kiss_script() == WSCRIPT_LEGACY ? 44
                : kiss_script() == WSCRIPT_NESTED ? 49 : 84;
    lv_label_set_text_fmt(s_path_lbl, "m/%dh/%dh/0h/0/%u",
                          purpose, kiss_testnet() ? 1 : 0, (unsigned)s_idx);
    if (s_path_tn_lbl) {
      lv_label_set_text(s_path_tn_lbl,
                        on_net_line());
      lv_obj_update_layout(s_path_lbl);
      lv_obj_set_pos(s_path_tn_lbl,
                     400 + lv_obj_get_width(s_path_lbl) + 16, RECV_PATH_Y + 28);
    }
  }

  if ((int)s_idx > s_seen_high) s_seen_high = (int)s_idx;   // seeds next open's landing

  // The state chip HANDOFF-03 asks for, in the vocabulary every other bitcoin
  // wallet uses: an address is USED or UNUSED. The doc drew it as "NEVER HANDED
  // OUT" and "HANDED OUT ALREADY", which the owner cut, and rightly: handing an
  // address to somebody happens off this device, so a signer with no chain view
  // cannot know whether it happened. Claiming it did, on the screen whose
  // subtitle is "trust what you see here", spends the credit that sentence asks
  // for.
  //
  // Used and unused is a claim of the same shape as what kiss_usage_high
  // actually holds: an address this device signed a spend from is on chain, so
  // USED is certain. UNUSED means no record here, which is what the standard
  // term means in any watch-only wallet too, and the privacy note beside the
  // chip is what tells the owner to move on to a fresh one either way.
  if (s_state_chip) {
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    int high = kiss_usage_high(fp, kiss_testnet() ? 1 : 0, kiss_script());
    bool handed = high >= 0 && (int)s_idx <= high;
    wt_state_chip_set(s_state_chip,
                      tr(handed ? STR_R_HANDED_ALREADY : STR_R_NEVER_HANDED),
                      handed ? WT_WARN : WT_OK);
    // Right aligned to x=752, on the same row as ADDRESS #N. Recomputed
    // every refresh because the label length differs between the two states
    // AND per locale.
    lv_obj_set_pos(s_state_chip, 752 - lv_obj_get_width(s_state_chip), 96);
  }
}

// Tapping the address card folds and unfolds it. recv_refresh rebuilds the
// spans from scratch either way, so the toggle only has to flip the flag; the
// flag is a static because NEXT rebuilds the same spans and the fold has to
// survive it.
static void addr_toggle_cb(lv_event_t *e) {
  (void)e;
  s_addr_full = !s_addr_full;
  recv_refresh();
}

static void next_cb(lv_event_t *e) {
  (void)e;
  s_idx++;
  recv_refresh();
}

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

// ---- the address list: what RECEIVE opens on ----
// This is the first surface on the device a finger can drag. Every other
// container in the wallet turns scrolling off on purpose, so nothing here can
// lean on scrolling already working: the detail screen keeps its own PREV/NEXT
// chevrons, which means there is still a way through the addresses that needs
// no flick at all if the panel's touch turns out to be unkind to one.
static void recv_detail_open(void);
static void recv_list_open(void);

static void row_tap_cb(lv_event_t *e) {
  s_idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  recv_detail_open();
}

// One row: index, then the address on a single line.
//
// A CARD, the same box Settings gives every one of its rows. This was a hairline
// and no fill, on the argument that twenty stacked boxes read as twenty
// competing controls. That argument is about a page of DIFFERENT controls; here
// every row is the same control twenty times, so the box is not competing with
// anything, and the mono28 address gets the edge it needs to read as a value
// rather than as grey text on the page.
//
// Nothing here marks an address as used. The signer only knows what it has
// shown you and what it has signed a spend FROM; it has no chain view, so
// colouring rows on that basis states more than it knows and, unexplained,
// just raises a question the screen cannot answer. The advice that actually
// helps -- use a fresh one each time -- is on the detail screen, where you are
// about to hand the address to somebody.
static lv_obj_t *recv_list_row(lv_obj_t *list, uint32_t idx) {
  char addr[91];
  if (kiss_session_address(0, idx, addr, sizeof addr) != 0)
    snprintf(addr, sizeof addr, "%s", tr(STR_C_SESSION_LOCKED));

  // A wt_row_x shell rather than a bare wt_card, for the CHEVRON. These rows
  // already looked right -- a card, a fill, an edge -- and never said the one
  // thing that mattered, which is that tapping an address opens it. The rest of
  // the device says that with a chevron and this list was the only place that
  // did not.
  //
  // Empty label and no sub: the address is a wt_addr_short span group, which
  // lights its head and tail in different inks, so it cannot be a row's plain
  // label. The row is built empty and the span group is placed into it, which
  // is what the icon lane at x=52 is measured to leave room for.
  char num[8];
  snprintf(num, sizeof num, "#%u", (unsigned)idx);
  lv_obj_t *row = wt_row_x(list, LV_SYMBOL_DOWNLOAD, "", NULL, NULL, NULL,
                           NULL, WT_MUT, false, 0, 0, 690, ROW_H,
                           row_tap_cb, (void *)(uintptr_t)idx);

  // The index is a label FOR the address, not a rival to it, so it stays in the
  // small face while the address gets the readable one.
  lv_obj_t *n = lv_label_create(row);
  lv_label_set_text(n, num);
  lv_obj_set_style_text_font(n, wt_font14(), 0);
  lv_obj_set_style_text_color(n, WT_MUT, 0);
  lv_obj_align(n, LV_ALIGN_LEFT_MID, 52, 0);

  lv_obj_t *sg = wt_addr_short(row, addr, wt_font_mono28());
  lv_obj_align(sg, LV_ALIGN_LEFT_MID, 104, 0);
  return row;
}

// Spent, not missing. Half opacity on the pill and its glyph, and the tap
// feedback and the click flag both off, so it neither lights up nor answers.
static void page_arrow_dim(lv_obj_t *p) {
  if (!p) return;
  lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(p, LV_OPA_40, 0);
}

static void page_cb(lv_event_t *e) {
  int step = (int)(intptr_t)lv_event_get_user_data(e);
  int base = (int)s_list_base + step * RECV_LIST_N;
  if (base < 0 || base >= RECV_LIST_CAP) return;     // ends of the range: no wrap
  s_list_base = (uint32_t)base;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  recv_list_open();
}

// BACK on the list means back to the ADDRESS, not out of Receive.
//
// The list used close_cb, which tears the whole flow down and lands on the main
// menu. That was right when the list WAS the receive screen and the detail was
// one tap in from it; HANDOFF-03 reversed the two, so from then on the only way
// to see the list was ALL ADDRESSES from the detail, and its BACK skipped the
// level it had come from. Escaping a screen goes up ONE level, always.
static void back_to_detail_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  s_addr_card = s_cmp_lbl = s_addr_more = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  recv_detail_open();
}

static void recv_list_open(void) {
  s_qr = s_addr_sg = s_idx_lbl = s_path_lbl = s_lock_note = NULL;   // detail-only widgets are gone
  s_state_chip = NULL;
  s_path_tn_lbl = NULL;
  s_addr_card = s_cmp_lbl = s_addr_more = NULL;

  // No subtitle. "trust what you see here, not your computer screen" is
  // anti-phishing advice about ONE address you are about to hand over, so it
  // belongs on the screen that shows one -- here it only cost the list a row
  // and said nothing about the list.
  // The page counter owns this screen's top-right corner, which is the useful
  // thing to put there on a screen whose whole job is saying which slice of the
  // hundred you are looking at.
  s_scr = wt_screen(s_parent, tr(STR_R_T), NULL);

  lv_obj_t *list = lv_obj_create(s_scr);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, 48, 72);
  lv_obj_set_size(list, 704, RECV_LIST_H);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_layout(list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, ROW_GAP, 0);   // the gap between cards
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  // Rest on a row boundary, always. A flick that dies between two cards leaves
  // one of them sliced through its own characters, which on a screen full of
  // near-identical addresses is the worst possible place to lose pixels: the
  // difference between #16 and #18 is exactly the part that got cut.
  lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_START);
  // remove_style_all took the default scrollbar with it, and on this background
  // an unstyled one is invisible -- which on the device reads as "the list does
  // not scroll" rather than "you have not scrolled yet".
  wt_list_scrollbar(list);
  // ON, not AUTO, and this is the one thing about this screen that had to
  // change. There are TWO ways to move through a hundred addresses here, the
  // list scrolls and the arrows page by twenty, and the viewport is 324px
  // against 48px rows: six and three quarters. AUTO hides the bar until you
  // have already scrolled, so the only hint that row seven exists is a clipped
  // row at the bottom edge, and the obvious control on the screen is an arrow
  // that jumps straight past it. A reader could reasonably conclude the page
  // holds six and that > skips fourteen they never saw. A bar that is there
  // before the first touch says how much list there is, which is the question.
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ON);

  // The list is NEVER scrolled programmatically. lv_obj_scroll_to_view()
  // during construction leaves the rows DRAWN at their scrolled positions
  // while touch still finds them at the unscrolled ones -- tapping the top row
  // did nothing, and tapping empty space 200px lower opened it. Paging picks a
  // first index instead, which needs no scroll to land where it means to.
  // One probe speaks for the whole page: every row derives from the same
  // session, so a locked one gives twenty rows each wearing SESSION LOCKED in
  // its address slot -- a list of identical failures dressed as a list of
  // addresses, every one with a chevron inviting a tap. The state renders
  // once, as a state.
  {
    char probe[91];
    if (kiss_session_address(0, s_list_base, probe, sizeof probe) != 0) {
      lv_obj_t *chip = wt_state_chip(list, tr(STR_C_SESSION_LOCKED), WT_WARN);
      (void)chip;
      wt_note(s_scr, tr(STR_C_LOCKED_B), 48, 132, 620, 80);   // reassurance, not a fault
    } else {
      for (uint32_t i = 0; i < RECV_LIST_N; i++)
        recv_list_row(list, s_list_base + i);
    }
  }

  // Which slice of the range is on screen. Says OF 100 so the cap is a stated
  // fact rather than the list mysteriously refusing to go further.
  // "OF" was hardcoded English on a device that ships 21 languages, and the
  // range was joined with a hyphen, which is not punctuation this project
  // uses. STR_C_OSD_OF is the localized "of" the scan overlay already counts
  // parts with, so this needed no new string: ja renders it "/", which reads
  // correctly here too. The range now uses an ellipsis, which is a span in
  // every locale rather than a minus sign in some of them.
  lv_obj_t *pg = wt_lbl(s_scr, "", 0, 0, wt_font14(), lv_color_hex(0x4B5464));
  lv_label_set_text_fmt(pg, "%u…%u  %s  %d", (unsigned)s_list_base + 1,
                        (unsigned)s_list_base + RECV_LIST_N,
                        tr(STR_C_OSD_OF), RECV_LIST_CAP);
  lv_obj_update_layout(pg);
  lv_obj_set_pos(pg, 752 - lv_obj_get_width(pg), 34);

  // No VERIFY here. It lives on the address page, one tap in, and this list is
  // a hundred addresses: putting it on both meant the same button appeared on
  // 101 screens. The list's job is choosing WHICH address, and VERIFY is not a
  // choice of address, so it does not belong in the row where that happens.
  //
  // BACK sits in the SAME corner here as on the address page one tap away.
  // The two screens are one errand, and the way out once swapped corners
  // under the finger between them; it does not any more, and every change to
  // this bar has to keep that true.
  //
  // The pager takes WT_ACT_X, because paging IS this screen's action: its job
  // is choosing WHICH address and the arrows are how the choosing happens. <
  // before > in reading order, adjacent so the pair reads as one control, and
  // BACK keeps the standard corner every lone-BACK screen uses.
  //
  // No SILENT PAYMENT here. It sat in this bar for a while, but the address
  // page one tap back already carries the SP row among its destinations, and
  // the same button on two screens of one errand said the device had two
  // opinions about where that door is. The list's bar is down to the two
  // things that belong to the LIST: turning its pages and leaving it.
  //
  // row[] indices below are load bearing: page_arrow_dim addresses the two
  // arrows by index, so < stays [1] and > stays [2] no matter where they sit.
  lv_obj_t *row[3];
  row[0] = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                   back_to_detail_cb, NULL);
  row[1] = wt_pill(s_scr, LV_SYMBOL_LEFT, WT_ACT_X, WT_ACTION_Y, 56, page_cb, (void *)(intptr_t)-1);
  row[2] = wt_pill(s_scr, LV_SYMBOL_RIGHT, WT_ACT_X + 56 + 22, WT_ACTION_Y, 56, page_cb, (void *)(intptr_t)1);
  wt_pill_row(row, 3);

  // An arrow at the end of the range says so. page_cb has always refused to
  // step past 0 or the cap, correctly, but it refused SILENTLY: on the first
  // page < looked exactly like > and did nothing, which reads as a device that
  // missed the touch rather than a list that has no page before this one. The
  // control is left in place and dimmed rather than hidden, because a button
  // that vanishes takes its neighbour's position with it and the row would
  // reflow under the finger.
  if (s_list_base == 0) page_arrow_dim(row[1]);
  if (s_list_base + RECV_LIST_N >= RECV_LIST_CAP) page_arrow_dim(row[2]);
}

// The path's "?", and it answers about the PATH alone.
//
// The text was already written and already translated: it is the second
// paragraph of the WALLET page's ADDRESS TYPE card, lifted out per locale, so
// 21 languages gained this explainer without a word of new translation. That
// card keeps both paragraphs, because on WALLET the path sits INSIDE the
// address type section and its column has no room for a second chip. Two
// screens, two right answers, one body of text.
#ifdef SIMULATOR
// Kept only so sim/sim_main.c can link without a stale prototype: HANDOFF-03
// pulled the derivation path row (and its "?" chip) off the receive detail.
// The path explainer still lives on the WALLET card. Delete once sim_main.c
// stops calling this.
void kiss_recv_sim_open_path_help(void) {}
#endif

// ALL ADDRESSES: opens the paginated list the detail used to be reached from.
static void list_from_detail_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  recv_list_open();
}

static void recv_detail_open(void) {
  s_addr_sg = NULL;
  s_addr_full = false;   // every open starts folded, like the SP screen
  s_scr = wt_screen(s_parent, tr(STR_R_T), tr(STR_R_S));
  // Left column: QR at (48, 112) 238x238 per HANDOFF-03. The card widget owns
  // the "+" corner cue for the zoom affordance. The extra vertical room the
  // 238 square costs came from dropping the derivation path row: the path is
  // a step you take once when pairing, not information you read every time
  // you hand out an address, and the WALLET card still shows it.
  wt_qr_card(s_scr, &s_qr, 48, 112, 238, 202);

  // Right column, x=310, w=442.
  //   y=106 caption ADDRESS #N + state chip right-aligned to x=752
  //   y=144 card, 442x114: the address and the compare caption, centred as a
  //         block by recv_refresh, folded or full
  //   y=268 privacy note, 442 wide, one line
  //   y=312 NEXT ADDRESS pill primary 250 wide 46 tall
  //
  // Nothing below the card moves for the extra ten pixels: the card ends at 258
  // and the note was always at 268, and the QR beside it already runs to 314.
  s_idx_lbl = wt_section(s_scr, "", 310, 102);
  s_state_chip = wt_state_chip(s_scr, "", WT_MUT);

  s_addr_card = wt_card(s_scr, RECV_CARD_X, RECV_CARD_Y,
                        RECV_CARD_W, RECV_CARD_H);
  // The card IS the fold control. No separate hit box and no extra pill: the
  // thing you want bigger is the thing you tap, and a 442x124 target needs no
  // aiming.
  lv_obj_add_flag(s_addr_card, LV_OBJ_FLAG_CLICKABLE);
  wt_tap_feedback(s_addr_card);
  lv_obj_add_event_cb(s_addr_card, addr_toggle_cb, LV_EVENT_CLICKED, NULL);

  // ...and it SAYS so. A big target nobody knows to press is not an
  // affordance, and the silent-payment screen two taps away gives the very
  // same gesture a labelled pill -- so the device answered "how do I see the
  // whole address" twice, differently, on two screens showing an address.
  //
  // Not a pill here: the action row is full (BACK, NEXT, VERIFY) and the
  // gesture belongs to the card, not to the row. A mark and its word in the
  // card's own corner, at the caption rung, using the two strings the SP
  // screen already ships in 21 locales. Text set by recv_refresh, because it
  // names the state the tap will move TO.
  s_addr_more = wt_lbl(s_addr_card, "", 14, 8, wt_font14(), WT_MUT);

  // The compare caption from HANDOFF-01: same string in every locale, points
  // at the lit characters above. Font14 muted so it never competes with the
  // characters it labels. Placed by recv_refresh, under whatever height the
  // address came out at.
  s_cmp_lbl = wt_lbl(s_addr_card, tr(STR_S_CMP_8), 14, 116, wt_font14(), WT_MUT);
  lv_obj_set_width(s_cmp_lbl, RECV_CARD_W - 28);
  lv_label_set_long_mode(s_cmp_lbl, LV_LABEL_LONG_DOT);

  // The two DESTINATIONS, as rows in the right column. They were buttons three
  // and four of a bar carrying four, which is what made this the busiest bar on
  // the device -- and neither of them does anything: both open a screen. A row
  // says that with a chevron, in the same shape the address list below and the
  // whole of Settings already use.
  //
  // The arithmetic is exact and there is no room for a third. The address card
  // ends at 258, a row is WT_ROW_H, and 268 + 64 = 332, 334 + 64 = 398 -- so the
  // second row's last drawn line is 397, one clear of WT_CONTENT_BOTTOM. NEXT
  // stays a button for that reason and a better one: it is an ACTION, it changes
  // what this screen is showing, and it belongs in the bar with VERIFY.
  //
  // The privacy reminder rides on ALL ADDRESSES' sub-line, which is where it
  // belongs: the list is where you go to use a different address, and "use a new
  // address each time" is why you would. HANDOFF-03 asks for two lines and the
  // second clause of STR_R_ONE_EACH ("reuse links payments") only fits 442 at
  // font14 in about 18 of 21 locales, so this takes the first clause only; the
  // WALLET card still carries the full caution on its ADDRESS TYPE row.
  {
    char one[80];
    const char *full = tr(STR_R_ONE_EACH);
    const char *nl = strchr(full, '\n');
    if (nl && (size_t)(nl - full) < sizeof one) {
      lv_memcpy(one, full, (size_t)(nl - full));
      one[nl - full] = 0;
    } else {
      snprintf(one, sizeof one, "%s", full);
    }
    wt_row_x(s_scr, LV_SYMBOL_LIST, tr(STR_R_ALL_ADDR), one, NULL, NULL, NULL,
             WT_INK, false, RECV_CARD_X, 260, RECV_CARD_W, 0,
             list_from_detail_cb, NULL);
  }
  // The secret mark is the silent payment identity and wears the accent on
  // the KEYS card already; one mark, one colour.
  lv_obj_t *sp = wt_row_x(s_scr, WT_ICON_SECRET, tr(STR_R_SP_BTN), NULL, NULL,
                          NULL, NULL, WT_INK, false, RECV_CARD_X, 331,
                          RECV_CARD_W, 0, sp_open_cb, NULL);
  wt_row_icon_accent(sp);

  // Action bar: the three things that ACT. BACK is LEFTMOST and VERIFY is the
  // far-right primary, which is the reverse of what HANDOFF-03's table said and
  // of what every lone-BACK screen does. The drawing is right and it is a rule,
  // not a quirk: the way OUT sits where a thumb rests and can be hit without
  // looking, and the far right corner is reserved for the action that does the
  // screen's work. The same order appears on redraws 01 and 02, so Sign follows
  // it too.
  //
  // NEXT ADDRESS came down from the content, where it floated at (310, 312) with
  // nothing to stand on. Three controls in 704 instead of four gives each one
  // room and puts the two that change something next to each other.
  lv_obj_t *row[3];
  row[0] = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                   close_cb, NULL);
  row[1] = wt_pill(s_scr, tr_sym(LV_SYMBOL_REFRESH, STR_R_NEXT), 240,
                   WT_ACTION_Y, 250, next_cb, NULL);
  row[2] = wt_pill(s_scr, tr(STR_R_VERIFY), WT_ACT_X, WT_ACTION_Y, 140,
                   vfy_scan, NULL);
  wt_pill_row(row, 3);
  wt_pill_primary(row[2]);
  recv_refresh();
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
