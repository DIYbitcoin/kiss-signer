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
#include "wallet_theme.h"
#include "wallet_ui.h"      // wallet_ui_last_fp: keys the reuse guard per wallet
#include "wallet_usage.h"   // highest receive index this wallet has used

#define VFY_SCAN_DEPTH 200   // how far down each chain VERIFY searches

static lv_obj_t *s_scr;                    // whichever receive-flow screen is up
static lv_obj_t *s_parent;
static lv_obj_t *s_qr, *s_addr_sg, *s_idx_lbl, *s_path_lbl;
static lv_obj_t *s_reuse_lbl, *s_fresh_pill;   // reuse-guard banner + jump button
static uint32_t s_idx;
// reuse guard: warn when viewing an index at/below what this wallet already
// used or showed. s_floor is frozen at open (prior-session usage) so browsing
// fresh addresses never warns; s_seen_high grows as you view, seeding the next
// open's fresh landing. s_seen_key detects a wallet/network/type switch.
static int  s_floor, s_seen_high = -1;
static char s_seen_key[16];

bool wallet_recv_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  s_reuse_lbl = NULL;
  s_fresh_pill = NULL;
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
    lv_qrcode_update(s_qr, addr, (uint32_t)strlen(addr));
  char grouped[120];
  wt_group4(addr, grouped, sizeof(grouped));
  if (s_addr_sg) lv_obj_delete(s_addr_sg);   // spans have no set_text: rebuild
  s_addr_sg = wt_addr_spans(s_scr, grouped, 360, wt_font28());
  lv_obj_set_pos(s_addr_sg, 400, 140);
  lv_label_set_text_fmt(s_idx_lbl, tr(STR_R_ADDR_N_FMT), (unsigned)s_idx);
  int purpose = wallet_script() == WSCRIPT_LEGACY ? 44
              : wallet_script() == WSCRIPT_NESTED ? 49 : 84;
  lv_label_set_text_fmt(s_path_lbl, "m/%dh/%dh/0h/0/%u   %s",
                        purpose, wallet_testnet() ? 1 : 0, (unsigned)s_idx,
                        wallet_testnet() ? tr(STR_R_ON_TESTNET) : "");

  // reuse guard: warn on an already-used/shown index; a fresh one stays quiet
  bool reused = (int)s_idx <= s_floor;
  if (s_reuse_lbl) {
    if (reused) lv_obj_clear_flag(s_reuse_lbl, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s_reuse_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_fresh_pill) {
    if (reused) lv_obj_clear_flag(s_fresh_pill, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s_fresh_pill, LV_OBJ_FLAG_HIDDEN);
  }
  if ((int)s_idx > s_seen_high) s_seen_high = (int)s_idx;   // seeds next open's landing
}

static void fresh_cb(lv_event_t *e) {
  (void)e;
  s_idx = s_seen_high < 0 ? 0 : (uint32_t)(s_seen_high + 1);
  recv_refresh();
}

static void prev_cb(lv_event_t *e) {
  (void)e;
  if (s_idx > 0) { s_idx--; recv_refresh(); }
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

  s_scr = wt_screen(s_parent, tr(STR_R_VT), tr(STR_R_VS));
  wt_group4(addr, grouped, sizeof grouped);

  // a silent-payment address needs ~3 lines even at font14; the note below has
  // to start under whatever the address actually occupies, not a fixed y
  bool longaddr = strlen(addr) > 64;
  lv_obj_t *sg = wt_addr_spans(s_scr, grouped, 700,
                               longaddr ? wt_font14() : wt_font28());
  lv_obj_set_pos(sg, 48, 186);
  lv_obj_update_layout(sg);
  int note_y = 186 + lv_obj_get_height(sg) + 16;
  if (note_y < 280) note_y = 280;

  if (mine || sp_mine) {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_OK, STR_R_YOURS), 48, 130, wt_font28(), WT_OK);
    if (sp_mine)
      snprintf(buf, sizeof buf, "%s", tr(STR_S_SP_BADGE));
    else if (change)
      snprintf(buf, sizeof buf, tr(STR_R_CHANGE_FMT), (unsigned)idx);
    else
      snprintf(buf, sizeof buf, tr(STR_R_RECV_FMT), (unsigned)idx);
    wt_lbl(s_scr, buf, 48, note_y, wt_font14(), WT_MUT);
  } else {
    wt_lbl(s_scr, tr_sym(LV_SYMBOL_CLOSE, STR_R_NOT_YOURS),
           48, 130, wt_font28(), WT_STOP);
    lv_obj_t *n = wt_wrap(s_scr, 48, note_y, 700);
    lv_label_set_text(n, tr(STR_R_NOT_B));
  }

  lv_obj_t *again = wt_pill(s_scr, tr(STR_R_SCAN_ANOTHER), 48, 404, 220, vfy_scan, NULL);
  wt_pill_primary(again);
  wt_pill(s_scr, tr(STR_C_DONE), 610, 404, 140, vfy_done_cb, NULL);
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
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  wallet_recv_open(s_parent);
}

static void sp_addr_open(lv_obj_t *parent) {
  s_parent = parent;
  s_addr_sg = NULL;
  s_scr = wt_screen(parent, tr(STR_S_SP_BADGE), tr(STR_R_S));
  wt_qr_card(s_scr, &s_qr, 48, 96, 300, 264);

  char addr[128];
  if (wallet_session_sp_address(addr, sizeof(addr)) != 0)
    snprintf(addr, sizeof(addr), "%s", tr(STR_C_SESSION_LOCKED));
  if (s_qr)
    lv_qrcode_update(s_qr, addr, (uint32_t)strlen(addr));

  char grouped[200];
  wt_group4(addr, grouped, sizeof(grouped));
  s_addr_sg = wt_addr_spans(s_scr, grouped, 360, wt_font14());
  lv_obj_set_pos(s_addr_sg, 400, 110);

  lv_obj_t *path = wt_lbl(s_scr, "", 400, 320, wt_font23(), WT_MUT);
  lv_label_set_text_fmt(path, "m/352h/%dh/0h   %s",
                        wallet_testnet() ? 1 : 0,
                        wallet_testnet() ? tr(STR_R_ON_TESTNET) : "");

  wt_pill(s_scr, tr(STR_C_BACK), 48, 404, 140, sp_back_cb, NULL);
}

static void sp_open_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
  sp_addr_open(s_parent);
}

void wallet_recv_open(lv_obj_t *parent) {
  if (s_scr) return;
  s_parent = parent;
  s_addr_sg = NULL;

  // reuse guard: figure out the freshest address to land on. Key by wallet +
  // network + type; a switch resets the session view-history to the persisted
  // used-high, otherwise keep growing it (a sign this session may have bumped it).
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
  s_floor = s_seen_high;                        // frozen: warnings compare to prior use
  s_idx = s_seen_high < 0 ? 0 : (uint32_t)(s_seen_high + 1);

  s_scr = wt_screen(parent, tr(STR_R_T), tr(STR_R_S));
  wt_qr_card(s_scr, &s_qr, 48, 96, 300, 264);

  s_idx_lbl = wt_section(s_scr, "", 400, 102);   // "ADDRESS  #N" caption (index lives here)

  // reuse banner + FRESH jump (hidden unless the shown index was used/shown before)
  s_reuse_lbl = wt_lbl(s_scr, tr_sym(LV_SYMBOL_WARNING, STR_R_REUSED),
                       400, 246, wt_font14(), WT_WARN);
  lv_obj_add_flag(s_reuse_lbl, LV_OBJ_FLAG_HIDDEN);
  s_fresh_pill = wt_pillh(s_scr, tr(STR_R_FRESH), 636, 244, 124, 44, fresh_cb, NULL);
  wt_pill_primary(s_fresh_pill);
  lv_obj_add_flag(s_fresh_pill, LV_OBJ_FLAG_HIDDEN);

  // derivation path stays small (reference), the VERIFY instruction does not.
  // Both sit under the FRESH pill at 288, above the pill row at 404.
  s_path_lbl = wt_lbl(s_scr, "", 400, 292, wt_font14(), WT_MUT);
  wt_note(s_scr, tr(STR_R_VERIFY_NOTE), 400, 314, 360, 90);

  wt_pill(s_scr, LV_SYMBOL_LEFT, 400, 404, 72, prev_cb, NULL);
  wt_pill(s_scr, tr_sym(LV_SYMBOL_RIGHT, STR_R_NEXT), 488, 404, 130, next_cb, NULL);
  wt_pill(s_scr, tr(STR_R_VERIFY), 634, 404, 126, vfy_scan, NULL);
  wt_pill(s_scr, tr(STR_C_BACK), 48, 404, 140, close_cb, NULL);
  wt_pill(s_scr, tr(STR_S_SP_BADGE), 200, 404, 190, sp_open_cb, NULL);
  recv_refresh();
}
