// Step 4: Receive + watch-only export. The address is derived ON THIS DEVICE from
// the session key (never trusted from a computer — that's the anti-phishing point),
// shown as text and a static QR. The export screen carries the wpkh descriptor so a
// coordinator (Sparrow) can watch the wallet without ever seeing a private key.
// Compiled in BOTH device and sim builds; the sim stubs wallet_session_* in sim_main.c.
#include "wallet_recv.h"

#include <stdio.h>
#include <string.h>

#include "wallet_crypto.h"
#include "wallet_theme.h"

static lv_obj_t *s_scr;                    // whichever of the two screens is up
static lv_obj_t *s_qr, *s_addr_lbl, *s_idx_lbl, *s_path_lbl;
static uint32_t s_idx;

bool wallet_recv_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e) {
  (void)e;
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
  lv_label_set_text(s_addr_lbl, grouped);
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

void wallet_recv_open(lv_obj_t *parent) {
  if (s_scr) return;
  s_idx = 0;
  s_scr = wt_screen(parent, "RECEIVE", "this address was made on the device. trust what you see here, not your computer screen.");
  wt_qr_card(s_scr, &s_qr, 48, 96, 300, 264);

  s_idx_lbl = wt_section(s_scr, "", 400, 102);   // "ADDRESS  #N" caption (index lives here)

  s_addr_lbl = wt_lbl(s_scr, "", 400, 140, &lv_font_montserrat_28, WT_INK);
  lv_obj_set_width(s_addr_lbl, 360);
  lv_label_set_long_mode(s_addr_lbl, LV_LABEL_LONG_WRAP);

  s_path_lbl = wt_lbl(s_scr, "", 400, 300, &lv_font_montserrat_14, WT_MUT);

  wt_pill(s_scr, LV_SYMBOL_LEFT, 400, 396, 72, prev_cb, NULL);
  wt_pill(s_scr, LV_SYMBOL_RIGHT " NEXT", 488, 396, 130, next_cb, NULL);
  wt_pill(s_scr, "BACK", 48, 404, 140, close_cb, NULL);
  recv_refresh();
}

// ---- watch-only export ----
void wallet_export_open(lv_obj_t *parent) {
  if (s_scr) return;
  s_scr = wt_screen(parent, "EXPORT", "scan this into Sparrow Wallet or another app to pair it with this device");
  wt_qr_card(s_scr, &s_qr, 48, 96, 300, 264);

  char desc[256];
  if (wallet_session_descriptor(desc, sizeof(desc)) != 0)
    snprintf(desc, sizeof(desc), "SESSION LOCKED");
  lv_qrcode_update(s_qr, desc, (uint32_t)strlen(desc));

  wt_section(s_scr, "DESCRIPTOR", 400, 102);

  lv_obj_t *d = wt_lbl(s_scr, desc, 400, 130, &lv_font_montserrat_14, WT_INK);
  lv_obj_set_width(d, 360);
  lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);

  wt_lbl(s_scr, "the app sees your balance, receives, and builds\n"
                "transactions for THIS device to sign. it can't\n"
                "spend on its own - the keys never leave here.",
         400, 330, &lv_font_montserrat_14, WT_MUT);

  wt_pill(s_scr, "BACK", 48, 404, 140, close_cb, NULL);
}
