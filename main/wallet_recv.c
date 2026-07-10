// Step 4: Receive + watch-only export. The address is derived ON THIS DEVICE from
// the session key (never trusted from a computer — that's the anti-phishing point),
// shown as text and a static QR. The export screen carries the wpkh descriptor so a
// coordinator (Sparrow) can watch the wallet without ever seeing a private key.
// Compiled in BOTH device and sim builds; the sim stubs wallet_session_* in sim_main.c.
#include "wallet_recv.h"

#include <stdio.h>
#include <string.h>

#include "wallet_crypto.h"

#define BG_COL   lv_color_hex(0x070A10)
#define INK_COL  lv_color_hex(0xE8EEF7)   // Mono theme accent
#define MUT_COL  lv_color_hex(0x7A869C)
#define KEY_COL  lv_color_hex(0x10141D)
#define CARD_COL lv_color_hex(0xF2F5FA)   // QR card: scanners want dark-on-light

static lv_obj_t *s_scr;                    // whichever of the two screens is up
static lv_obj_t *s_qr, *s_addr_lbl, *s_idx_lbl, *s_path_lbl;
static uint32_t s_idx;

bool wallet_recv_active(void) { return s_scr != NULL; }

static void close_cb(lv_event_t *e) {
  (void)e;
  if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void wallet_recv_close(void) { close_cb(NULL); }   // idle auto-lock path

// hardware-wallet convention: groups of 4 make visual compare against the
// coordinator's screen much less error-prone than one 42-char run
static void group4(const char *in, char *out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
    if (i && i % 4 == 0) out[o++] = ' ';
    out[o++] = in[i];
  }
  out[o] = 0;
}

static lv_obj_t *mk_screen(lv_obj_t *parent, const char *title, const char *sub) {
  s_scr = lv_obj_create(parent);
  lv_obj_remove_style_all(s_scr);
  lv_obj_set_size(s_scr, 800, 480);
  lv_obj_set_style_bg_color(s_scr, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_foreground(s_scr);

  lv_obj_t *cap = lv_label_create(s_scr);
  lv_label_set_text(cap, title);
  lv_obj_set_style_text_color(cap, INK_COL, 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_letter_space(cap, 3, 0);
  lv_obj_set_pos(cap, 48, 30);

  lv_obj_t *s = lv_label_create(s_scr);
  lv_label_set_text(s, sub);
  lv_obj_set_style_text_color(s, MUT_COL, 0);
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(s, 48, 68);
  return s_scr;
}

static lv_obj_t *mk_qr_card(void) {        // white card + QR at the fixed left slot
  lv_obj_t *card = lv_obj_create(s_scr);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 300, 300);
  lv_obj_set_pos(card, 48, 96);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_bg_color(card, CARD_COL, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  s_qr = lv_qrcode_create(card);
  if (s_qr) {
    lv_qrcode_set_size(s_qr, 264);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x0B0E14));
    lv_qrcode_set_light_color(s_qr, CARD_COL);
    lv_obj_center(s_qr);
  }
  return card;
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb) {
  lv_obj_t *p = lv_obj_create(s_scr);
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, w, 52);
  lv_obj_set_pos(p, x, y);
  lv_obj_set_style_radius(p, 26, 0);
  lv_obj_set_style_bg_color(p, KEY_COL, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_color(p, MUT_COL, 0);
  lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, INK_COL, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_center(l);
  return p;
}

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
  group4(addr, grouped, sizeof(grouped));
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
  mk_screen(parent, "RECEIVE", "this address was made on the device. trust what you see here, not your computer screen.");
  mk_qr_card();

  s_idx_lbl = lv_label_create(s_scr);      // "ADDRESS  #N" caption (index lives here)
  lv_obj_set_style_text_color(s_idx_lbl, MUT_COL, 0);
  lv_obj_set_style_text_font(s_idx_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(s_idx_lbl, 2, 0);
  lv_obj_set_pos(s_idx_lbl, 400, 102);

  s_addr_lbl = lv_label_create(s_scr);
  lv_obj_set_style_text_color(s_addr_lbl, INK_COL, 0);
  lv_obj_set_style_text_font(s_addr_lbl, &lv_font_montserrat_28, 0);
  lv_obj_set_width(s_addr_lbl, 360);
  lv_label_set_long_mode(s_addr_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(s_addr_lbl, 400, 140);

  s_path_lbl = lv_label_create(s_scr);
  lv_obj_set_style_text_color(s_path_lbl, MUT_COL, 0);
  lv_obj_set_style_text_font(s_path_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(s_path_lbl, 400, 300);

  mk_pill(LV_SYMBOL_LEFT, 400, 396, 72, prev_cb);
  mk_pill(LV_SYMBOL_RIGHT " NEXT", 488, 396, 130, next_cb);
  mk_pill("BACK", 48, 404, 140, close_cb);
  recv_refresh();
}

// ---- watch-only export ----
void wallet_export_open(lv_obj_t *parent) {
  if (s_scr) return;
  mk_screen(parent, "EXPORT", "scan this into Sparrow or another wallet app to pair it with this device");
  mk_qr_card();

  char desc[256];
  if (wallet_session_descriptor(desc, sizeof(desc)) != 0)
    snprintf(desc, sizeof(desc), "SESSION LOCKED");
  lv_qrcode_update(s_qr, desc, (uint32_t)strlen(desc));

  lv_obj_t *cap = lv_label_create(s_scr);
  lv_label_set_text(cap, "DESCRIPTOR");
  lv_obj_set_style_text_color(cap, MUT_COL, 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(cap, 2, 0);
  lv_obj_set_pos(cap, 400, 102);

  lv_obj_t *d = lv_label_create(s_scr);
  lv_label_set_text(d, desc);
  lv_obj_set_style_text_color(d, INK_COL, 0);
  lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
  lv_obj_set_width(d, 360);
  lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(d, 400, 130);

  lv_obj_t *note = lv_label_create(s_scr);
  lv_label_set_text(note, "the app sees your balance, receives, and builds\n"
                          "transactions for THIS device to sign. it can't\n"
                          "spend on its own - the keys never leave here.");
  lv_obj_set_style_text_color(note, MUT_COL, 0);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(note, 400, 330);

  mk_pill("BACK", 48, 404, 140, close_cb);
}
