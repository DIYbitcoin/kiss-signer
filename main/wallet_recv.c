// Step 4: Receive. The address is derived ON THIS DEVICE from the session key
// (never trusted from a computer — that's the anti-phishing point), shown as
// text and a static QR. VERIFY scans an address QR from the coordinator's
// screen and answers the only question that matters: is this one of MINE —
// the defense against malware swapping the receive address on the computer.
// Compiled in BOTH device and sim builds; the sim stubs wallet_session_* in sim_main.c.
#include "wallet_recv.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_scan.h"
#include "wallet_info.h"   // the one "?" card implementation lives there
#include "wallet_theme.h"
#include "wallet_ui.h"      // wallet_ui_last_fp: keys the reuse guard per wallet
#include "wallet_usage.h"   // highest receive index this wallet has used

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
#define ROW_H 48

static lv_obj_t *s_scr;                    // whichever receive-flow screen is up
static lv_obj_t *s_parent;
// Where the derivation path block starts on the detail screen: caption at this
// y, its "?" chip centred on it, the path 22 below. The address above now ends
// at 198 (two lines of font23 from 140) and the privacy reminder below starts
// at 308, so the block owns 230..287 with air on both sides.
#define RECV_PATH_Y 236

static lv_obj_t *s_qr, *s_addr_sg, *s_addr_tail, *s_idx_lbl, *s_path_lbl, *s_path_tn_lbl;
static lv_obj_t *s_state_chip;
static lv_obj_t *s_sp_path_lbl, *s_sp_path_sec, *s_sp_back_pill, *s_sp_toggle_pill;
static lv_obj_t *s_sp_addr_hit;
static uint32_t s_idx;
static uint32_t s_list_base;               // first index the list shows
// The highest address used or shown seeds the next fresh landing.
// s_seen_key detects a wallet/network/type switch.
static int s_seen_high = -1;
static char s_seen_key[16];
static bool s_sp_full;                     // silent-payment text is folded by default
static char s_sp_addr[128];

bool wallet_recv_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = s_addr_tail = NULL;
  s_sp_path_lbl = s_sp_path_sec = NULL;
  s_sp_back_pill = s_sp_toggle_pill = s_sp_addr_hit = NULL;
  s_state_chip = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void wallet_recv_close(void) { close_cb(NULL); }   // idle auto-lock path

// ---- receive ----
static void recv_refresh(void) {
  char addr[91];
  int rc = wallet_session_address(0, s_idx, addr, sizeof(addr));
  if (rc != 0)
    snprintf(addr, sizeof(addr), "%s", tr(STR_C_SESSION_LOCKED));
  if (s_qr)
    wt_qr_update(s_qr, addr, (uint32_t)strlen(addr));
  char grouped[120];
  (void)grouped;
  if (s_addr_sg) lv_obj_delete(s_addr_sg);   // spans have no set_text: rebuild
  if (s_addr_tail) { lv_obj_delete(s_addr_tail); s_addr_tail = NULL; }
  // Right column, x=310 w=442. Everything but the final 8 characters, grouped
  // in fours at mono23 and muted, then those 8 on their own line at mono28 in
  // ink with an underline. Nothing is elided: on a screen whose subtitle is
  // "trust what you see here", every character stays on the glass.
  wt_addr_head_tail(s_scr, addr, 442, wt_font_mono23(), wt_font_mono28(),
                    &s_addr_sg, &s_addr_tail);
  if (s_addr_sg) lv_obj_set_pos(s_addr_sg, 310, 146);
  if (s_addr_tail) lv_obj_set_pos(s_addr_tail, 310, 222);
  lv_label_set_text_fmt(s_idx_lbl, tr(STR_R_ADDR_N_FMT), (unsigned)s_idx);
  // The derivation path label is only wired up on the sub-screens that ask for
  // it (currently just the SP detail; HANDOFF-03 pulled it off the base
  // detail). If neither label was created this open, the refresh path skips
  // silently rather than call lv_label_set_text_fmt on NULL.
  if (s_path_lbl) {
    int purpose = wallet_script() == WSCRIPT_LEGACY ? 44
                : wallet_script() == WSCRIPT_NESTED ? 49 : 84;
    lv_label_set_text_fmt(s_path_lbl, "m/%dh/%dh/0h/0/%u",
                          purpose, wallet_testnet() ? 1 : 0, (unsigned)s_idx);
    if (s_path_tn_lbl) {
      lv_label_set_text(s_path_tn_lbl,
                        wallet_testnet() ? tr(STR_R_ON_TESTNET) : "");
      lv_obj_update_layout(s_path_lbl);
      lv_obj_set_pos(s_path_tn_lbl,
                     400 + lv_obj_get_width(s_path_lbl) + 16, RECV_PATH_Y + 28);
    }
  }

  if ((int)s_idx > s_seen_high) s_seen_high = (int)s_idx;   // seeds next open's landing

  // The state chip named in HANDOFF-03: has this address ever been given away?
  // The best proxy the offline signer has is wallet_usage_high: an address that
  // has been spent from was definitely handed out to whoever sent the coins in
  // the first place. Received-only addresses are not tracked, so "NEVER HANDED
  // OUT" here means "we have no record of it being spent from", not a promise
  // the address is virgin. That is honest and matches how the fresh landing
  // logic already reads usage_high, so the chip and the landing agree on which
  // address is which.
  if (s_state_chip) {
    uint8_t fp[4];
    wallet_ui_last_fp(fp);
    int high = wallet_usage_high(fp, wallet_testnet() ? 1 : 0, wallet_script());
    bool handed = high >= 0 && (int)s_idx <= high;
    wt_state_chip_set(s_state_chip,
                      tr(handed ? STR_R_HANDED_ALREADY : STR_R_NEVER_HANDED),
                      handed ? WT_WARN : WT_OK);
    // Right aligned to x=752, on the same row as ADDRESS #N. Recomputed
    // every refresh because the label length differs between the two states
    // AND per locale.
    lv_obj_set_pos(s_state_chip, 752 - lv_obj_get_width(s_state_chip), 106);
  }
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
      if (wallet_session_address(c, i, mine, sizeof mine) != 0)
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
  wallet_recv_open(s_parent);
}

// Is this our own silent-payment address? It is not on any bc1/tb1 chain, so
// vfy_find can never match it: compare against the one we derive ourselves.
static int vfy_is_sp_mine(const char *addr) {
  char mine[128];
  if (wallet_session_sp_address(mine, sizeof mine) != 0)
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
                                   : wallet_address_validate(addr);

  s_scr = wt_screen(s_parent, tr(STR_R_VT), tr(STR_R_VS));
  wt_lock_mark(s_scr);
  // Only format and tail-highlight something that really is an address.
  // Arbitrary QR text is not grouped address data; feeding a short malformed
  // string through the span formatter also left LVGL with a broken short-span
  // layout on the error screen.
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
    lv_obj_t *n = wt_wrap(s_scr, 48, note_y, 700);
    lv_label_set_text(n, tr(STR_R_NOT_FOUND_B));
    lv_obj_set_style_text_color(n, WT_WARN, 0);
  } else if (validity == WADDR_WRONG_NETWORK) {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_CLOSE, STR_R_WRONG_NET),
           48, 130, wt_font28(), WT_STOP);
    lv_obj_t *n = wt_wrap(s_scr, 48, note_y, 700);
    // Name both networks. The %s placeholders in STR_R_WRONG_NET_B are
    // (address_network, wallet_network) so the reader learns what was scanned
    // and what the device is set to in one sentence. "mainnet" and "testnet"
    // are Bitcoin proper nouns and stay untranslated; every locale already
    // uses those two words as English in this file.
    char buf[256];
    const char *addr_net = wallet_testnet() ? "mainnet" : "testnet";
    const char *wall_net = wallet_testnet() ? "testnet" : "mainnet";
    snprintf(buf, sizeof buf, tr(STR_R_WRONG_NET_B), addr_net, wall_net);
    lv_label_set_text(n, buf);
  } else {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_CLOSE, STR_R_INVALID),
           48, 130, wt_font28(), WT_STOP);
    lv_obj_t *n = wt_wrap(s_scr, 48, note_y, 700);
    lv_label_set_text(n, tr(STR_R_INVALID_B));
  }

  lv_obj_t *again = wt_pill(s_scr, tr(STR_R_SCAN_ANOTHER), 48, WT_ACTION_Y, 220, vfy_scan, NULL);
  wt_pill_primary(again);
  wt_pill(s_scr, tr(STR_C_DONE), 610, WT_ACTION_Y, 140, vfy_done_cb, NULL);
}

static void vfy_cancel(void) {
  wallet_recv_open(s_parent);            // backed out of the camera: back to Receive
}

static void vfy_scan(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  wallet_scan_open_raw(s_parent, vfy_result, vfy_cancel);
}

// ---- silent payment (BIP352) static receive address ----
// One reusable sp1/tsp1, derived on-device (m/352'). No index, no reuse guard:
// a silent-payment address is meant to be shared and reused; that's the point.

static void sp_back_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  s_sp_path_lbl = s_sp_path_sec = NULL;
  s_sp_back_pill = s_sp_toggle_pill = s_sp_addr_hit = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  wallet_recv_open(s_parent);
}

static void sp_help_close_cb(lv_event_t *e) {
  lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

// A reusable Silent Payment address is intentionally NOT the address that
// appears in the transaction. This surprises people who compare Sparrow's
// output list after a first test payment, so explain the two prefixes beside
// the address instead of making them discover it in documentation.
static void sp_help_cb(lv_event_t *e) {
  (void)e;
  lv_obj_t *ovl = lv_obj_create(s_scr);
  lv_obj_remove_style_all(ovl);
  lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(ovl, WT_BG, 0);
  lv_obj_set_style_bg_opa(ovl, 245, 0);
  lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ovl, sp_help_close_cb, LV_EVENT_CLICKED, ovl);

  // The two prefixes are the whole subject, so they are arguments rather than
  // baked text: %s appears five times and a translation may place them in any
  // order it needs. body is sized for the longest locale plus five 4-char
  // prefixes, not for English.
  const bool tn = wallet_testnet();
  const char *share = tn ? "tsp1" : "sp1";     // what you hand out
  const char *seen  = tn ? "tb1p" : "bc1p";    // what lands in the transaction
  char title[96], body[640];
  snprintf(title, sizeof title, tr(STR_R_SP_WHY_T), tn ? "TB1P" : "BC1P");
  snprintf(body, sizeof body, tr(STR_R_SP_WHY_B),
           share, seen, share, seen, seen);

  lv_obj_t *t = wt_lbl(ovl, title, 0, 0, wt_font28(), wt_accent());
  lv_obj_set_style_text_letter_space(t, 2, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 82);

  lv_obj_t *b = wt_lbl(ovl, body, 0, 0,
                       wt_body_font(body, 720, 238), WT_MUT);
  lv_obj_set_width(b, 720);
  lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 142);

  wt_pill(ovl, tr(STR_C_OK), 300, 392, 200, sp_help_close_cb, ovl);
  wt_card_intro(ovl);
}

static void sp_addr_render(void) {
  if (s_addr_sg) lv_obj_delete(s_addr_sg);
  if (s_sp_full) {
    // The full string remains one tap away. It is the source of truth for
    // reading or comparing the address; the folded default is only a view.
    char grouped[200];
    wt_group4(s_sp_addr, grouped, sizeof(grouped));
    s_addr_sg = wt_addr_spans(s_scr, grouped, 386, wt_font_mono28());
    lv_obj_set_pos(s_addr_sg, 366, 100);
  } else {
    // Match the readable list form: constant prefix muted, four meaningful
    // characters near each end lit. The QR still receives all of `s_sp_addr`.
    s_addr_sg = wt_addr_short(s_scr, s_sp_addr, wt_font_mono28());
    lv_obj_set_pos(s_addr_sg, 366, 150);
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
  wt_lock_mark(s_scr);
  // The longer tsp1 full view can make LVGL auto-scroll a default container
  // to its newest child, shifting the fixed 800x480 composition off-screen.
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  // Same 30px visual / 54px touch target as every anonymous help affordance.
  wt_help_chip(s_scr, 715, 35, WT_MUT, sp_help_cb, NULL);
  // This is the one receive code meant to be scanned by somebody else's
  // phone. Folding the default text view buys enough room to raise the QR one
  // module scale while preserving a real white quiet zone around it.
  wt_qr_card(s_scr, &s_qr, 44, 96, 304, 280);

  if (wallet_session_sp_address(s_sp_addr, sizeof(s_sp_addr)) != 0)
    snprintf(s_sp_addr, sizeof(s_sp_addr), "%s", tr(STR_C_SESSION_LOCKED));
  if (s_qr)
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
  s_sp_path_sec = wt_section(s_scr, tr(STR_I_SEC_PATH), 366, 200);
  s_sp_path_lbl = wt_lbl(s_scr, "", 366, 222, wt_font_mono23(), WT_INK);
  lv_label_set_text_fmt(s_sp_path_lbl, "m/352h/%dh/0h   %s",
                        wallet_testnet() ? 1 : 0,
                        wallet_testnet() ? tr(STR_R_ON_TESTNET) : "");

  s_sp_back_pill = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                           sp_back_cb, NULL);
  s_sp_toggle_pill = wt_pill(s_scr, tr(STR_R_SP_SHOW_FULL),
                             48, WT_ACTION_Y, 280, sp_toggle_cb, NULL);
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
// No pill. A row is not a button you press for an action, it is a line in a
// list you pick from, and twenty stacked lozenges read as twenty competing
// controls. A hairline under each row and a fill only while pressed says the
// same thing quietly.
//
// Nothing here marks an address as used. The signer only knows what it has
// shown you and what it has signed a spend FROM; it has no chain view, so
// colouring rows on that basis states more than it knows and, unexplained,
// just raises a question the screen cannot answer. The advice that actually
// helps -- use a fresh one each time -- is on the detail screen, where you are
// about to hand the address to somebody.
static lv_obj_t *recv_list_row(lv_obj_t *list, uint32_t idx) {
  char addr[91];
  if (wallet_session_address(0, idx, addr, sizeof addr) != 0)
    snprintf(addr, sizeof addr, "%s", tr(STR_C_SESSION_LOCKED));

  lv_obj_t *row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 690, ROW_H);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_bg_color(row, wt_accent_pressed(), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
  // Hairline separator, not a border box: the row is a line in a list.
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(0x1B212C), 0);
  lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);   // the LIST scrolls, not the row
  wt_tap_feedback(row);
  lv_obj_add_event_cb(row, row_tap_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

  // The index is a label FOR the address, not a rival to it, so it stays in the
  // small face while the address gets the readable one.
  lv_obj_t *n = lv_label_create(row);
  lv_label_set_text_fmt(n, "#%u", (unsigned)idx);
  lv_obj_set_style_text_font(n, wt_font14(), 0);
  lv_obj_set_style_text_color(n, WT_MUT, 0);
  lv_obj_align(n, LV_ALIGN_LEFT_MID, 10, 0);

  lv_obj_t *sg = wt_addr_short(row, addr, wt_font_mono28());
  lv_obj_align(sg, LV_ALIGN_LEFT_MID, 62, 0);
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

static void recv_list_open(void) {
  s_qr = s_addr_sg = s_idx_lbl = s_path_lbl = NULL;   // detail-only widgets are gone
  s_state_chip = NULL;
  s_path_tn_lbl = NULL;

  // No subtitle. "trust what you see here, not your computer screen" is
  // anti-phishing advice about ONE address you are about to hand over, so it
  // belongs on the screen that shows one -- here it only cost the list a row
  // and said nothing about the list.
  s_scr = wt_screen(s_parent, tr(STR_R_T), NULL);
  wt_lock_mark(s_scr);

  lv_obj_t *list = lv_obj_create(s_scr);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, 48, 72);
  lv_obj_set_size(list, 704, 324);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_layout(list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  // remove_style_all took the default scrollbar with it, and on this background
  // an unstyled one is invisible -- which on the device reads as "the list does
  // not scroll" rather than "you have not scrolled yet".
  lv_obj_set_style_bg_color(list, WT_MUT, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 6, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
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
  for (uint32_t i = 0; i < RECV_LIST_N; i++)
    recv_list_row(list, s_list_base + i);

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
  // Dropping it is also what un-crams this row. Four controls in 704px instead
  // of five gives every gap 22px and hands BACK the standard 140 it has
  // everywhere else, so the one screen that had to squeeze to 110 no longer
  // does.
  lv_obj_t *row[4];
  // STR_R_SP_BTN, not STR_S_SP_BADGE. The badge is a descriptor: it names a
  // kind of address, and it is right in lower case as a screen title and as an
  // inline label in the list above. In this row it is a button standing next to
  // two arrows and BACK, and the only lower case button on the device reads as
  // a bug rather than as a distinction. One string cannot be both, so there are
  // two, and each locale's button is its own badge cased for a button.
  row[0] = wt_pill(s_scr, tr(STR_R_SP_BTN), 48, WT_ACTION_Y, 220, sp_open_cb, NULL);
  row[1] = wt_pill(s_scr, LV_SYMBOL_LEFT, 290, WT_ACTION_Y, 56, page_cb, (void *)(intptr_t)-1);
  row[2] = wt_pill(s_scr, LV_SYMBOL_RIGHT, 368, WT_ACTION_Y, 56, page_cb, (void *)(intptr_t)1);
  row[3] = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb, NULL);
  wt_pill_row(row, 4);

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
void wallet_recv_sim_open_path_help(void) {}
#endif

// ALL ADDRESSES: opens the paginated list the detail used to be reached from.
static void list_from_detail_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = s_addr_tail = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  recv_list_open();
}

static void recv_detail_open(void) {
  s_addr_sg = s_addr_tail = NULL;
  s_scr = wt_screen(s_parent, tr(STR_R_T), tr(STR_R_S));
  wt_lock_mark(s_scr);
  // Left column: QR at (48, 112) 238x238 per HANDOFF-03. The card widget owns
  // the "+" corner cue for the zoom affordance. The extra vertical room the
  // 238 square costs came from dropping the derivation path row: the path is
  // a step you take once when pairing, not information you read every time
  // you hand out an address, and the WALLET card still shows it.
  wt_qr_card(s_scr, &s_qr, 48, 112, 238, 202);

  // Right column, x=310, w=442. HANDOFF-03 geometry.
  //   y=112 caption ADDRESS #N + state chip right-aligned to x=752
  //   y=146 address body wt_addr_spans mono23 muted, built by recv_refresh
  //   y=236 lit tail own object mono28 ink, built by recv_refresh
  //   y=268 caption "compare these 8"
  //   y=300 privacy note, 442 wide, up to 2 lines
  //   y=330 NEXT ADDRESS pill primary 250 wide 52 tall
  s_idx_lbl = wt_section(s_scr, "", 310, 112);
  s_state_chip = wt_state_chip(s_scr, "", WT_MUT);

  // The compare caption from HANDOFF-01: same string in every locale, points
  // at the underlined mono28 line above. Font14 muted so it never competes
  // with the characters it labels.
  wt_lbl(s_scr, tr(STR_S_CMP_8), 310, 258, wt_font14(), WT_MUT);

  // Privacy reminder as a single short line: HANDOFF-03 asks for 2 lines here,
  // but the second half of STR_R_ONE_EACH ("reuse links payments") only fits
  // in 442 wide at font14 in ~18 of 21 locales — fr/it/nl need three. Rather
  // than pay a font drop for standing advice, this shows only the first
  // clause; the WALLET card carries the full "reuse links payments" caution
  // on its ADDRESS TYPE row, so the concept is not lost from the device.
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
    lv_obj_t *note = wt_lbl(s_scr, one, 310, 296, wt_font14(), WT_MUT);
    lv_obj_set_width(note, 442);
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
  }

  // NEXT ADDRESS: primary, in the right column, next_cb bumps s_idx and
  // recv_refresh redraws every widget that depends on the derived address.
  // 344 + 46 = 390 clears the action-bar hairline; the pill is one rung
  // shorter than the 52 HANDOFF-03 named so the right column stays inside
  // WT_CONTENT_BOTTOM without pushing the note back into the tail above.
  wt_pill_primary(wt_pillh(s_scr, tr_sym(LV_SYMBOL_REFRESH, STR_R_NEXT),
                           310, 344, 250, 46, next_cb, NULL));

  // Action bar per HANDOFF-03: ALL ADDRESSES / SILENT PAYMENT / VERIFY / BACK.
  // Widths 190, 200, 140, 140. Positions 48, 250, 462, 610 (WT_BACK_X). Two
  // 12px gutters between the first three (48+190+12=250, 250+200+12=462),
  // and 462+140+8=610 lets BACK sit on the shared WT_BACK_X anchor.
  lv_obj_t *row[4];
  row[0] = wt_pill(s_scr, tr(STR_R_ALL_ADDR), 48,  WT_ACTION_Y, 190,
                   list_from_detail_cb, NULL);
  row[1] = wt_pill(s_scr, tr(STR_R_SP_BTN),   250, WT_ACTION_Y, 200,
                   sp_open_cb, NULL);
  row[2] = wt_pill(s_scr, tr(STR_R_VERIFY),   462, WT_ACTION_Y, 140,
                   vfy_scan, NULL);
  row[3] = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                   close_cb, NULL);
  wt_pill_row(row, 4);
  recv_refresh();
}

void wallet_recv_open(lv_obj_t *parent) {
  if (s_scr) return;
  s_parent = parent;
  s_addr_sg = NULL;

  // Figure out the freshest address to land on. Key by wallet + network + type;
  // a switch resets the session view-history to the persisted used-high,
  // otherwise keep growing it (a sign this session may have bumped it).
  uint8_t fp[4];
  wallet_ui_last_fp(fp);
  char key[16];
  snprintf(key, sizeof key, "%02x%02x%02x%02x%d%d", fp[0], fp[1], fp[2], fp[3],
           wallet_testnet() ? 1 : 0, wallet_script());
  int used = wallet_usage_high(fp, wallet_testnet() ? 1 : 0, wallet_script());
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
