// Step 4: Receive. The address is derived ON THIS DEVICE from the session key
// (never trusted from a computer — that's the anti-phishing point), shown as
// text and a static QR. VERIFY scans an address QR from the coordinator's
// screen and answers the only question that matters: is this one of MINE —
// the defense against malware swapping the receive address on the computer.
// Compiled in BOTH device and sim builds; the sim stubs wallet_session_* in sim_main.c.
#include "wallet_recv.h"

#include <stdio.h>
#include <string.h>

#include "wallet_crypto.h"
#include "wallet_scan.h"
#include "wallet_theme.h"

#define VFY_SCAN_DEPTH 200   // how far down each chain VERIFY searches

static lv_obj_t *s_scr;                    // whichever receive-flow screen is up
static lv_obj_t *s_parent;
static lv_obj_t *s_qr, *s_addr_sg, *s_idx_lbl, *s_path_lbl;
static uint32_t s_idx;

bool wallet_recv_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e) {
  (void)e;
  s_addr_sg = NULL;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void wallet_recv_close(void) { close_cb(NULL); }   // idle auto-lock path

// ---- receive ----
static void recv_refresh(void) {
  char addr[91];
  int rc = wallet_session_address(0, s_idx, addr, sizeof(addr));
  if (rc != 0)
    snprintf(addr, sizeof(addr), "SESSION LOCKED");
  if (s_qr) {
    lv_result_t qres = lv_qrcode_update(s_qr, addr, (uint32_t)strlen(addr));
  }
  char grouped[120];
  wt_group4(addr, grouped, sizeof(grouped));
  if (s_addr_sg) lv_obj_delete(s_addr_sg);   // spans have no set_text: rebuild
  s_addr_sg = wt_addr_spans(s_scr, grouped, 360, &lv_font_montserrat_28);
  lv_obj_set_pos(s_addr_sg, 400, 140);
  lv_label_set_text_fmt(s_idx_lbl, "ADDRESS  #%u", (unsigned)s_idx);
  int purpose = wallet_script() == WSCRIPT_LEGACY ? 44
              : wallet_script() == WSCRIPT_NESTED ? 49 : 84;
  lv_label_set_text_fmt(s_path_lbl, "m/%dh/%dh/0h/0/%u   %s",
                        purpose, wallet_testnet() ? 1 : 0, (unsigned)s_idx,
                        wallet_testnet() ? "on TESTNET" : "");
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
  char p0 = out[0] >= 'A' && out[0] <= 'Z' ? out[0] + 32 : out[0];
  char p1 = out[1] >= 'A' && out[1] <= 'Z' ? out[1] + 32 : out[1];
  int b32 = (o > 3) && out[2] == '1' &&
            ((p0 == 'b' && p1 == 'c') || (p0 == 't' && p1 == 'b'));
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

static void vfy_result(const char *txt, size_t len) {
  (void)len;
  char addr[92], grouped[120], buf[96];
  vfy_norm(txt, addr, sizeof addr);
  int change = 0;
  uint32_t idx = 0;
  int mine = vfy_find(addr, &change, &idx);

  s_scr = wt_screen(s_parent, "VERIFY ADDRESS",
                    "checked on this device, against this wallet's own keys");
  wt_group4(addr, grouped, sizeof grouped);

  if (mine) {
    lv_obj_t *t = wt_lbl(s_scr, LV_SYMBOL_OK "  THIS ADDRESS IS YOURS",
                         48, 130, &lv_font_montserrat_28, WT_OK);
    (void)t;
    lv_obj_t *sg = wt_addr_spans(s_scr, grouped, 700, &lv_font_montserrat_28);
    lv_obj_set_pos(sg, 48, 186);
    if (change)
      snprintf(buf, sizeof buf, "change address #%u - your coordinator uses"
                                " these internally", (unsigned)idx);
    else
      snprintf(buf, sizeof buf, "receive address #%u - safe to give out or"
                                " send to", (unsigned)idx);
    wt_lbl(s_scr, buf, 48, 280, &lv_font_montserrat_14, WT_MUT);
  } else {
    wt_lbl(s_scr, LV_SYMBOL_CLOSE "  NOT THIS WALLET'S",
           48, 130, &lv_font_montserrat_28, WT_STOP);
    lv_obj_t *sg = wt_addr_spans(s_scr, grouped, 700, &lv_font_montserrat_28);
    lv_obj_set_pos(sg, 48, 186);
    lv_obj_t *n = wt_wrap(s_scr, 48, 280, 700);
    lv_label_set_text(n,
        "not among this wallet's first 200 receive or change addresses.\n"
        "it could be another address type or network setting, a different\n"
        "wallet, or malware swapping addresses on your computer.\n"
        "do not send to it until you know whose it is.");
  }

  lv_obj_t *again = wt_pill(s_scr, "SCAN ANOTHER", 48, 404, 220, vfy_scan, NULL);
  wt_pill_primary(again);
  wt_pill(s_scr, "DONE", 610, 404, 140, vfy_done_cb, NULL);
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

void wallet_recv_open(lv_obj_t *parent) {
  if (s_scr) return;
  s_parent = parent;
  s_idx = 0;
  s_addr_sg = NULL;
  s_scr = wt_screen(parent, "RECEIVE", "this address was made on the device. trust what you see here, not your computer screen.");
  wt_qr_card(s_scr, &s_qr, 48, 96, 300, 264);

  s_idx_lbl = wt_section(s_scr, "", 400, 102);   // "ADDRESS  #N" caption (index lives here)

  s_path_lbl = wt_lbl(s_scr, "", 400, 300, &lv_font_montserrat_14, WT_MUT);
  wt_lbl(s_scr, "VERIFY: scan an address your computer shows\n"
                "and this device says if it is really yours",
         400, 330, &lv_font_montserrat_14, WT_MUT);

  wt_pill(s_scr, LV_SYMBOL_LEFT, 400, 396, 72, prev_cb, NULL);
  wt_pill(s_scr, LV_SYMBOL_RIGHT " NEXT", 488, 396, 130, next_cb, NULL);
  wt_pill(s_scr, "VERIFY", 634, 396, 126, vfy_scan, NULL);
  wt_pill(s_scr, "BACK", 48, 404, 140, close_cb, NULL);
  recv_refresh();
}
