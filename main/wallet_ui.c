// Step 3: the login. Spec: the passphrase keyboard is the most-used screen —
// big-key landscape QWERTY, char-flash-then-mask entry, show/hide toggle, and
// the fingerprint shown HUGE after entry (there is no "wrong passphrase" error
// by design; the fingerprint is how you recognize your wallet).
#include "wallet_ui.h"

#include <stdio.h>
#include <string.h>

#include "wallet_crypto.h"
#include "wallet_seed.h"

#ifndef SIMULATOR
#include "esp_efuse.h"
bool radio_is_held(void);   // main.c: reads back the C6 reset pad (GPIO54)
#endif
#ifndef KISS_VERSION_STR
#define KISS_VERSION_STR "dev"
#endif
#ifndef KISS_COMMIT_STR
#define KISS_COMMIT_STR "local"
#endif

#define BG_COL   lv_color_hex(0x070A10)
#define INK_COL  lv_color_hex(0xE8EEF7)   // Mono theme accent
#define MUT_COL  lv_color_hex(0x7A869C)
#define KEY_COL  lv_color_hex(0x10141D)
#define KEYP_COL lv_color_hex(0x2A3242)

#define PASS_MAX 128
#define FLASH_MS 900                       // last char visible this long, then masked

static lv_obj_t *s_login;                  // full-screen passphrase entry
static lv_obj_t *s_fpscr;                  // full-screen fingerprint reveal
static lv_obj_t *s_errscr;                 // setup-failed STOP (never enters home)
static lv_obj_t *s_entry;                  // the masked entry label
static lv_obj_t *s_count;                  // "N characters" (catches hidden typos)
static lv_obj_t *s_showbtn_lbl;
static lv_obj_t *s_kb;
static lv_timer_t *s_mask_tmr;
static lv_obj_t *s_pop;                    // key-press bubble ("visual haptics")
static lv_obj_t *s_pop_lbl;
static lv_timer_t *s_pop_tmr;
static char s_pass[PASS_MAX + 1];
static int s_plen;
static bool s_show;                        // show-all toggle
static bool s_flash;                       // last char currently unmasked
static void (*s_unlocked_cb)(void);
static lv_obj_t *s_cap;                    // caption (setup mode repurposes it)
static bool s_setup_mode;                  // first login after the wizard: type twice
static bool s_first_done;                  // first of the two entries captured
static bool s_weak_ack;                    // weak passphrase needs a second OK
static lv_obj_t *s_meter;                  // WEAK/FAIR/STRONG (setup only)
static char s_first[PASS_MAX + 1];

// Rough passphrase strength in bits: length x log2(character pool). Only a
// guardrail, only shown at CREATION — the normal login never judges (any
// passphrase might be a legitimate other wallet).
static int pass_bits(void) {
  bool lo = false, up = false, di = false, sy = false;
  for (int i = 0; i < s_plen; i++) {
    char c = s_pass[i];
    if (c >= 'a' && c <= 'z') lo = true;
    else if (c >= 'A' && c <= 'Z') up = true;
    else if (c >= '0' && c <= '9') di = true;
    else sy = true;
  }
  int pool = (lo ? 26 : 0) + (up ? 26 : 0) + (di ? 10 : 0) + (sy ? 33 : 0);
  if (!pool) return 0;
  int lb10 = pool >= 89 ? 65 : pool >= 62 ? 60 : pool >= 36 ? 52
           : pool >= 26 ? 47 : 33;             // log2(pool) x10, coarse
  return s_plen * lb10 / 10;
}

static void meter_refresh(void) {
  if (!s_meter) return;
  if (!s_setup_mode || s_plen == 0) { lv_label_set_text(s_meter, ""); return; }
  int bits = pass_bits();
  if (bits < 40) {
    lv_label_set_text(s_meter, "WEAK");
    lv_obj_set_style_text_color(s_meter, lv_color_hex(0xFF4D5E), 0);
  } else if (bits < 70) {
    lv_label_set_text(s_meter, "FAIR");
    lv_obj_set_style_text_color(s_meter, lv_color_hex(0xF2B84B), 0);
  } else {
    lv_label_set_text(s_meter, "STRONG");
    lv_obj_set_style_text_color(s_meter, lv_color_hex(0x35D07F), 0);
  }
}

bool wallet_ui_active(void) {
  return s_login != NULL || s_fpscr != NULL || s_errscr != NULL;
}

// ---- keyboard maps (three planes) ----
static const char *MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "#1!", "CANCEL", " ", "OK", ""};
static const char *MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "#1!", "CANCEL", " ", "OK", ""};
// two symbol planes so ALL 32 ASCII punctuation chars are reachable (spec:
// passphrase = printable ASCII; an untypeable char = an unrecoverable wallet)
static const char *MAP_SYM[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "&", "(", ")", "?", "\n",
    "#2~", "-", "_", "=", "+", ".", ",", "/", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "CANCEL", " ", "OK", ""};
static const char *MAP_SYM2[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "\"", "'", ":", ";", "[", "]", "{", "}", "*", "\n",
    "#1!", "<", ">", "\\", "|", "`", "~", "^", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "CANCEL", " ", "OK", ""};

// every plane keeps the same 10/9/9/4 button layout, so key_rect() and the
// id-based ctrls (space #30 width, OK #31 accent) hold on all of them; ctrls
// are re-asserted anyway since LVGL may reset them on map change
static void kb_plane(lv_obj_t *kb, const char **map) {
  lv_buttonmatrix_set_map(kb, map);
  // hold-to-repeat ONLY on backspace (id 27 on every plane): a slow press on a
  // character key must never repeat into the masked field (silent-input footgun)
  lv_buttonmatrix_set_button_ctrl_all(kb, LV_BUTTONMATRIX_CTRL_NO_REPEAT);
  lv_buttonmatrix_clear_button_ctrl(kb, 27, LV_BUTTONMATRIX_CTRL_NO_REPEAT);
  lv_buttonmatrix_set_button_width(kb, 30, 3);
  lv_buttonmatrix_set_button_ctrl(kb, 31, LV_BUTTONMATRIX_CTRL_CHECKED);
}

// ---- LVGL pointer indev over the same touch seam the game uses ----
extern bool platform_read_touch(int *x, int *y);

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  int x = 0, y = 0;
  if (platform_read_touch(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void ensure_indev(void) {
  static lv_indev_t *indev;
  if (indev) return;
  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, indev_read);
}

// ---- entry display: dots, optional flash of the newest char, show-all ----
static void entry_refresh(void) {
  static char buf[PASS_MAX * 3 + 8];
  meter_refresh();
  if (s_count) {
    int sp = 0;
    for (int i = 0; i < s_plen; i++) if (s_pass[i] == ' ') sp++;
    if (s_plen == 0) lv_label_set_text(s_count, "");
    else if (sp)     // spaces are the classic invisible typo — call them out
      lv_label_set_text_fmt(s_count, "%d character%s (%d space%s)",
                            s_plen, s_plen == 1 ? "" : "s", sp, sp == 1 ? "" : "s");
    else lv_label_set_text_fmt(s_count, "%d character%s", s_plen, s_plen == 1 ? "" : "s");
  }
  if (s_plen == 0) {
    lv_label_set_text(s_entry, "type your passphrase");
    lv_obj_set_style_text_color(s_entry, MUT_COL, 0);
    return;
  }
  lv_obj_set_style_text_color(s_entry, INK_COL, 0);
  if (s_show) {
    lv_label_set_text(s_entry, s_pass);
    return;
  }
  int n = 0;
  int shown = s_flash ? s_plen - 1 : s_plen;   // chars rendered as dots
  for (int i = 0; i < shown && n < (int)sizeof(buf) - 4; i++) {
    buf[n++] = '\xE2'; buf[n++] = '\x80'; buf[n++] = '\xA2';  // U+2022 bullet
  }
  if (s_flash && n < (int)sizeof(buf) - 2) buf[n++] = s_pass[s_plen - 1];
  buf[n] = 0;
  lv_label_set_text(s_entry, buf);
}

static void mask_cb(lv_timer_t *t) {
  (void)t;
  s_flash = false;
  s_mask_tmr = NULL;
  entry_refresh();
}

static void flash_last(void) {
  s_flash = true;
  if (s_mask_tmr) lv_timer_delete(s_mask_tmr);
  s_mask_tmr = lv_timer_create(mask_cb, FLASH_MS, NULL);
  lv_timer_set_repeat_count(s_mask_tmr, 1);
  entry_refresh();
}

// after a weak-ack warning, any edit returns the caption to the stage prompt
static void setup_cap_reset(void) {
  if (!s_setup_mode || !s_weak_ack || !s_cap) return;
  s_weak_ack = false;
  lv_label_set_text(s_cap, s_first_done ? "TYPE IT AGAIN TO CONFIRM"
                                        : "CREATE YOUR PASSPHRASE");
  lv_obj_set_style_text_color(s_cap, s_first_done ? lv_color_hex(0xF2B84B) : MUT_COL, 0);
}

static lv_obj_t *s_cancel_ovl;             // "cancel setup?" confirm (setup mode only)
static lv_obj_t *s_warnscr;                // post-setup passphrase warning (one screen)

static void wipe_and_close(void) {
  if (s_cancel_ovl) { lv_obj_delete_async(s_cancel_ovl); s_cancel_ovl = NULL; }
  if (s_warnscr) { lv_obj_delete_async(s_warnscr); s_warnscr = NULL; }
  // cancelling setup before the fingerprint confirm drops the staged seed, so
  // an abandoned setup never leaves a half-made wallet in flash (P0 safety net)
  if (s_setup_mode) wallet_seed_discard();
  memset(s_pass, 0, sizeof(s_pass));         // never keep the passphrase around
  memset(s_first, 0, sizeof(s_first));
  s_setup_mode = false;
  s_first_done = false;
  s_weak_ack = false;
  s_plen = 0;
  s_show = false;
  s_flash = false;
  if (s_mask_tmr) { lv_timer_delete(s_mask_tmr); s_mask_tmr = NULL; }
  if (s_pop_tmr) { lv_timer_delete(s_pop_tmr); s_pop_tmr = NULL; }
  s_pop = NULL; s_pop_lbl = NULL;            // children of s_login: die with it
  // async: this runs from event callbacks of children of these screens — deleting
  // an ancestor of the event target mid-event corrupts the rest of the event pass
  if (s_login) { lv_obj_delete_async(s_login); s_login = NULL; }
  if (s_fpscr) { lv_obj_delete_async(s_fpscr); s_fpscr = NULL; }
}

// CANCEL during setup would throw away the seed you just wrote down, so make it
// a deliberate choice instead of a one-tap exit. (Normal login has nothing to
// lose, so it still cancels straight to the game.)
static void cancel_keep_cb(lv_event_t *e) {          // "keep going": dismiss the prompt
  (void)e;
  if (s_cancel_ovl) { lv_obj_delete_async(s_cancel_ovl); s_cancel_ovl = NULL; }
}
static void cancel_discard_cb(lv_event_t *e) {       // "discard": drop the staged seed
  (void)e;
  wipe_and_close();                                  // clears s_cancel_ovl too
}
static void show_cancel_confirm(void) {
  if (s_cancel_ovl) return;
  s_cancel_ovl = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_cancel_ovl);
  lv_obj_set_size(s_cancel_ovl, 800, 480);
  lv_obj_set_pos(s_cancel_ovl, 0, 0);
  lv_obj_set_style_bg_color(s_cancel_ovl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_cancel_ovl, 190, 0);     // dim the keyboard behind
  lv_obj_add_flag(s_cancel_ovl, LV_OBJ_FLAG_CLICKABLE);   // swallow stray taps
  lv_obj_clear_flag(s_cancel_ovl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(s_cancel_ovl);

  lv_obj_t *card = lv_obj_create(s_cancel_ovl);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 540, 268);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, BG_COL, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = lv_label_create(card);
  lv_label_set_text(t, "CANCEL SETUP?");
  lv_obj_set_style_text_color(t, INK_COL, 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);

  lv_obj_t *s = lv_label_create(card);
  lv_label_set_text(s, "the new wallet you just made will be lost.\n"
                       "you would have to start over from the menu.");
  lv_obj_set_style_text_color(s, MUT_COL, 0);
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 82);

  lv_obj_t *keep = lv_button_create(card);           // safe choice, green outline
  lv_obj_set_size(keep, 232, 58);
  lv_obj_align(keep, LV_ALIGN_BOTTOM_LEFT, 18, -22);
  lv_obj_set_style_bg_color(keep, KEY_COL, 0);
  lv_obj_set_style_border_color(keep, lv_color_hex(0x35D07F), 0);
  lv_obj_set_style_border_width(keep, 2, 0);
  lv_obj_set_style_shadow_width(keep, 0, 0);
  lv_obj_add_event_cb(keep, cancel_keep_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *kl = lv_label_create(keep);
  lv_label_set_text(kl, "KEEP GOING");
  lv_obj_set_style_text_color(kl, INK_COL, 0);
  lv_obj_set_style_text_letter_space(kl, 2, 0);
  lv_obj_center(kl);

  lv_obj_t *disc = lv_button_create(card);           // destructive choice, red
  lv_obj_set_size(disc, 232, 58);
  lv_obj_align(disc, LV_ALIGN_BOTTOM_RIGHT, -18, -22);
  lv_obj_set_style_bg_color(disc, KEY_COL, 0);
  lv_obj_set_style_border_color(disc, lv_color_hex(0xFF4D5E), 0);
  lv_obj_set_style_border_width(disc, 2, 0);
  lv_obj_set_style_shadow_width(disc, 0, 0);
  lv_obj_add_event_cb(disc, cancel_discard_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *dl = lv_label_create(disc);
  lv_label_set_text(dl, "DISCARD");
  lv_obj_set_style_text_color(dl, lv_color_hex(0xFF4D5E), 0);
  lv_obj_set_style_text_letter_space(dl, 2, 0);
  lv_obj_center(dl);
}

// ---- fingerprint reveal ----
static void setup_fail_screen(void);
static void setup_warn_screen(void);

// dismiss the STOP screen back to the game; nothing was saved
static void setup_fail_dismiss_cb(lv_event_t *e) {
  (void)e;
  s_setup_mode = false; s_first_done = false; s_weak_ack = false;
  s_plen = 0; s_show = false; s_flash = false;
  memset(s_pass, 0, sizeof s_pass);
  memset(s_first, 0, sizeof s_first);
  if (s_errscr) { lv_obj_delete_async(s_errscr); s_errscr = NULL; }
}

// Setup couldn't be saved (staged seed failed to commit, or the session didn't
// derive): show a clear STOP instead of silently entering a broken home.
static void setup_fail_screen(void) {
  if (s_mask_tmr) { lv_timer_delete(s_mask_tmr); s_mask_tmr = NULL; }
  if (s_pop_tmr) { lv_timer_delete(s_pop_tmr); s_pop_tmr = NULL; }
  s_pop = NULL; s_pop_lbl = NULL;
  if (s_login) { lv_obj_delete_async(s_login); s_login = NULL; }
  if (s_fpscr) { lv_obj_delete_async(s_fpscr); s_fpscr = NULL; }

  s_errscr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_errscr);
  lv_obj_set_size(s_errscr, 800, 480);
  lv_obj_set_style_bg_color(s_errscr, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_errscr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_errscr, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *icon = lv_label_create(s_errscr);
  lv_label_set_text(icon, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(icon, lv_color_hex(0xFF4D5E), 0);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 120);

  lv_obj_t *t = lv_label_create(s_errscr);
  lv_label_set_text(t, s_setup_mode ? "COULDN'T SET UP THE WALLET"
                                    : "COULDN'T OPEN THE WALLET");
  lv_obj_set_style_text_color(t, INK_COL, 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 190);

  lv_obj_t *s = lv_label_create(s_errscr);
  lv_label_set_text(s, s_setup_mode ? "nothing was saved. start again from the menu."
                                    : "something went wrong. try again from the menu.");
  lv_obj_set_style_text_color(s, MUT_COL, 0);
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
  lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 236);

  lv_obj_t *btn = lv_button_create(s_errscr);
  lv_obj_set_size(btn, 200, 56);
  lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 300);
  lv_obj_set_style_bg_color(btn, KEY_COL, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, setup_fail_dismiss_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "BACK");
  lv_obj_set_style_text_color(bl, INK_COL, 0);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
  lv_obj_center(bl);
}

static void fp_tap_cb(lv_event_t *e) {
  (void)e;
  void (*cb)(void) = s_unlocked_cb;
  // Open the session on the (staged, during setup) seed FIRST — if it doesn't
  // derive, never persist it and never enter the home with a broken session.
  if (wallet_session_open(s_plen ? s_pass : NULL) != 0) {
    if (s_setup_mode) wallet_seed_discard();
    setup_fail_screen();
    return;
  }
  // Setup only reaches here after passphrase-twice + this fingerprint confirm
  // AND a session that derives: NOW the seed is safe to persist (P0 safety net).
  if (s_setup_mode && wallet_seed_commit() != 0) {
    wallet_session_close();            // couldn't save it: don't pretend we have a wallet
    wallet_seed_discard();
    setup_fail_screen();
    return;
  }
  // the passphrase dies here (deniability); the derived session key lives in RAM
  // until wallet lock so Receive/Sign can derive without re-typing
  if (s_setup_mode) {          // one last screen: what the passphrase really is
    setup_warn_screen();       // (its OK button finishes the unlock)
    return;
  }
  wipe_and_close();
  if (cb) cb();
}

static void fp_back_cb(lv_event_t *e) {
  (void)e;                                   // back to the keyboard, passphrase kept
  if (s_fpscr) { lv_obj_delete_async(s_fpscr); s_fpscr = NULL; }
  if (s_login) lv_obj_clear_flag(s_login, LV_OBJ_FLAG_HIDDEN);
}

static uint8_t s_last_fp[4];               // fingerprint of the wallet just unlocked

void wallet_ui_last_fp(uint8_t out[4]) { memcpy(out, s_last_fp, 4); }

// Post-setup, pre-home: the one thing a new owner must actually understand —
// the passphrase is PART of the wallet (it locks the words, like encryption),
// and nothing can recover it. Session is open + seed committed by now; the OK
// button finishes the unlock. Spec: one screen max, plain words.
static void setup_warn_ok_cb(lv_event_t *e) {
  (void)e;
  void (*cb)(void) = s_unlocked_cb;
  wipe_and_close();                        // also deletes s_warnscr
  if (cb) cb();
}

static void setup_warn_screen(void) {
  if (s_warnscr) return;
  s_warnscr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_warnscr);
  lv_obj_set_size(s_warnscr, 800, 480);
  lv_obj_set_style_bg_color(s_warnscr, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_warnscr, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_warnscr, LV_OBJ_FLAG_CLICKABLE);   // swallow stray taps
  lv_obj_clear_flag(s_warnscr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(s_warnscr);

  lv_obj_t *t = lv_label_create(s_warnscr);
  lv_label_set_text(t, "YOUR PASSPHRASE IS PART OF THE WALLET");
  lv_obj_set_style_text_color(t, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 56);

  lv_obj_t *b = lv_label_create(s_warnscr);
  lv_label_set_text(b,
      "your words + your passphrase TOGETHER make this wallet. the\n"
      "passphrase adds a layer of security on the words, like encryption.\n\n"
      "write your words down and keep them offline, but the passphrase\n"
      "matters most: words alone open a different wallet, never this\n"
      "one. words AND passphrase together open everything.\n\n"
      "NOTHING can bring back a lost passphrase. not this device, not\n"
      "anyone. a typo just opens a different wallet, with no error.\n"
      "know yours by its fingerprint:");
  lv_obj_set_style_text_color(b, INK_COL, 0);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 112);

  lv_obj_t *f = lv_label_create(s_warnscr);
  lv_label_set_text_fmt(f, "%02X%02X%02X%02X",
                        s_last_fp[0], s_last_fp[1], s_last_fp[2], s_last_fp[3]);
  lv_obj_set_style_text_color(f, INK_COL, 0);
  lv_obj_set_style_text_font(f, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_letter_space(f, 4, 0);
  lv_obj_align(f, LV_ALIGN_TOP_MID, 0, 322);

  lv_obj_t *ok = lv_button_create(s_warnscr);
  lv_obj_set_size(ok, 260, 56);
  lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 388);
  lv_obj_set_style_bg_color(ok, KEY_COL, 0);
  lv_obj_set_style_shadow_width(ok, 0, 0);
  lv_obj_set_style_border_width(ok, 1, 0);
  lv_obj_set_style_border_color(ok, MUT_COL, 0);
  lv_obj_add_event_cb(ok, setup_warn_ok_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *ol = lv_label_create(ok);
  lv_label_set_text(ol, "I UNDERSTAND");
  lv_obj_set_style_text_color(ol, INK_COL, 0);
  lv_obj_set_style_text_font(ol, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(ol, 2, 0);
  lv_obj_center(ol);
}

// reveal pop-in: the code card rises + fades in (one-shot, no per-frame cost after)
static void fp_pop_ty_cb(void *v, int32_t y) { lv_obj_set_style_translate_y((lv_obj_t *)v, y, 0); }
static void fp_pop_opa_cb(void *v, int32_t o) { lv_obj_set_style_opa((lv_obj_t *)v, o, 0); }

static void show_fingerprint(void) {
  uint8_t fp[4] = {0};
  wallet_fingerprint(s_plen ? s_pass : NULL, fp);
  memcpy(s_last_fp, fp, 4);

  s_fpscr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_fpscr);
  lv_obj_set_size(s_fpscr, 800, 480);
  lv_obj_set_style_bg_color(s_fpscr, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_fpscr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_fpscr, LV_OBJ_FLAG_CLICKABLE);  // buttons only, no tap-anywhere

  lv_obj_t *cap = lv_label_create(s_fpscr);
  lv_label_set_text(cap, "YOUR WALLET'S FINGERPRINT");
  lv_obj_set_style_text_color(cap, MUT_COL, 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(cap, 2, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 64);

  // the fingerprint sits in a chip-style box, like the home screen's corner chip
  lv_obj_t *box = lv_obj_create(s_fpscr);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 420, 118);
  lv_obj_set_style_radius(box, 16, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, INK_COL, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x0C1018), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 104);

  lv_obj_t *big = lv_label_create(box);
  lv_label_set_text_fmt(big, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
  lv_obj_set_style_text_color(big, INK_COL, 0);
  lv_obj_set_style_text_font(big, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_letter_space(big, 4, 0);
  lv_obj_center(big);

  // the code card rises + fades in when the fingerprint is computed
  lv_anim_t pa;
  lv_anim_init(&pa);
  lv_anim_set_var(&pa, box);
  lv_anim_set_duration(&pa, 260);
  lv_anim_set_delay(&pa, 40);
  lv_anim_set_path_cb(&pa, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&pa, fp_pop_ty_cb);
  lv_anim_set_values(&pa, 22, 0);
  lv_anim_start(&pa);
  lv_anim_set_exec_cb(&pa, fp_pop_opa_cb);
  lv_anim_set_values(&pa, 40, 255);
  lv_anim_start(&pa);

  lv_obj_t *note = lv_label_create(s_fpscr);
  lv_label_set_text(note, "your passphrase always opens the wallet with this code");
  lv_obj_set_style_text_color(note, lv_color_hex(0xB9C2D4), 0);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 258);

  lv_obj_t *note2 = lv_label_create(s_fpscr);
  lv_label_set_text(note2, "not the code you wrote down?  go back and retype your passphrase");
  lv_obj_set_style_text_color(note2, MUT_COL, 0);
  lv_obj_set_style_text_font(note2, &lv_font_montserrat_14, 0);
  lv_obj_align(note2, LV_ALIGN_TOP_MID, 0, 286);

  // bottom action pill
  lv_obj_t *go = lv_obj_create(s_fpscr);
  lv_obj_remove_style_all(go);
  lv_obj_set_size(go, 260, 52);
  lv_obj_set_style_radius(go, 26, 0);
  lv_obj_set_style_bg_color(go, lv_color_hex(0x24406B), 0);
  lv_obj_set_style_bg_opa(go, LV_OPA_COVER, 0);
  lv_obj_align(go, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_add_flag(go, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(go, fp_tap_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *gol = lv_label_create(go);
  lv_label_set_text(gol, "TAP TO OPEN");
  lv_obj_set_style_text_color(gol, INK_COL, 0);
  lv_obj_set_style_text_font(gol, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(gol, 2, 0);
  lv_obj_center(gol);

  lv_obj_t *back = lv_obj_create(s_fpscr);   // bottom-left: back to the keyboard
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 140, 52);
  lv_obj_set_style_radius(back, 26, 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_border_color(back, MUT_COL, 0);
  lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 48, -40);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, fp_back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *backl = lv_label_create(back);
  lv_label_set_text(backl, "BACK");
  lv_obj_set_style_text_color(backl, MUT_COL, 0);
  lv_obj_set_style_text_font(backl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(backl, 2, 0);
  lv_obj_center(backl);

  lv_obj_add_flag(s_login, LV_OBJ_FLAG_HIDDEN);
}

// ---- key-press bubble: the tapped character pops up and rises above the key ----
// (the board has no vibration motor, so feedback is visual)
#define KB_X 0
#define KB_Y 158
#define KB_W 800
#define KB_H 316
#define KB_PAD 6

// geometry of button `id` in the shared 10/9/9/4 layout (space = 3 units wide)
static void key_rect(uint32_t id, int *x, int *y, int *w, int *h) {
  static const uint8_t row_first[] = {0, 10, 19, 28, 32};
  static const uint8_t row_units[] = {10, 9, 9, 6};
  int r = 3;
  for (int i = 0; i < 4; i++)
    if (id < row_first[i + 1]) { r = i; break; }
  int rh = (KB_H - 2 * KB_PAD - 3 * KB_PAD) / 4;
  *y = KB_Y + KB_PAD + r * (rh + KB_PAD);
  *h = rh;
  int units_before = 0, my_units = 1;
  for (uint32_t i = row_first[r]; i < id; i++)
    units_before += (i == 30) ? 3 : 1;
  if (id == 30) my_units = 3;
  int n_keys = row_first[r + 1] - row_first[r];
  float unit = (float)(KB_W - 2 * KB_PAD - (n_keys - 1) * KB_PAD) / row_units[r];
  int idx = id - row_first[r];
  *x = KB_X + KB_PAD + (int)(units_before * unit) + idx * KB_PAD;
  *w = (int)(my_units * unit);
}

static void pop_hide_cb(lv_timer_t *t) {
  (void)t;
  s_pop_tmr = NULL;
  if (s_pop) lv_obj_add_flag(s_pop, LV_OBJ_FLAG_HIDDEN);
}

static void pop_show(const char *ch, uint32_t id) {
  if (!s_pop) {                             // iPhone-style key callout: larger than the
    s_pop = lv_obj_create(s_login);         // key, big glyph, bottom overlapping the
    lv_obj_remove_style_all(s_pop);         // pressed key so it visibly grows out of it
    lv_obj_set_size(s_pop, 96, 96);
    lv_obj_set_style_radius(s_pop, 20, 0);
    lv_obj_set_style_bg_color(s_pop, lv_color_hex(0x33415C), 0);
    lv_obj_set_style_bg_opa(s_pop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pop, 2, 0);
    lv_obj_set_style_border_color(s_pop, INK_COL, 0);
    lv_obj_set_style_shadow_width(s_pop, 18, 0);
    lv_obj_set_style_shadow_color(s_pop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_pop, LV_OPA_60, 0);
    s_pop_lbl = lv_label_create(s_pop);
    lv_obj_set_style_text_color(s_pop_lbl, INK_COL, 0);
    lv_obj_set_style_text_font(s_pop_lbl, &lv_font_montserrat_48, 0);
    lv_obj_center(s_pop_lbl);
  }
  int kx, ky, kw, kh;
  key_rect(id, &kx, &ky, &kw, &kh);
  lv_label_set_text(s_pop_lbl, ch);
  int px = kx + kw / 2 - 48;
  if (px < 4) px = 4;
  if (px > 800 - 100) px = 800 - 100;
  lv_obj_set_pos(s_pop, px, ky - 68);       // bottom ~28px into the key = attached to it
  lv_obj_clear_flag(s_pop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_pop);
  if (s_pop_tmr) lv_timer_delete(s_pop_tmr);
  s_pop_tmr = lv_timer_create(pop_hide_cb, 280, NULL);
  lv_timer_set_repeat_count(s_pop_tmr, 1);
}

// ---- keyboard events ----
static void kb_cb(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(kb);
  const char *txt = lv_buttonmatrix_get_button_text(kb, id);
  if (!txt) return;

  if (strcmp(txt, "ABC") == 0)      kb_plane(kb, MAP_UPPER);
  else if (strcmp(txt, "abc") == 0) kb_plane(kb, MAP_LOWER);
  else if (strcmp(txt, "#1!") == 0) kb_plane(kb, MAP_SYM);
  else if (strcmp(txt, "#2~") == 0) kb_plane(kb, MAP_SYM2);
  else if (strcmp(txt, "CANCEL") == 0) {
    if (s_setup_mode) show_cancel_confirm();   // don't throw away a fresh seed on one tap
    else wipe_and_close();                     // normal login: nothing to lose
  }
  else if (strcmp(txt, "OK") == 0) {
    if (s_setup_mode && !s_first_done && pass_bits() < 40 && !s_weak_ack) {
      // weak passphrase: make "yes, really" a separate deliberate press
      s_weak_ack = true;
      lv_label_set_text(s_cap, "WEAK PASSPHRASE. TAP OK AGAIN TO USE IT ANYWAY");
      lv_obj_set_style_text_color(s_cap, lv_color_hex(0xFF4D5E), 0);
    } else if (s_setup_mode && !s_first_done) {
      // setup: capture the first entry, demand it again — a typo here is an
      // unreproducible passphrase (= lost coins) later
      memcpy(s_first, s_pass, sizeof s_first);
      s_first_done = true;
      s_weak_ack = false;
      memset(s_pass, 0, sizeof s_pass);
      s_plen = 0;
      s_show = false;
      if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, "SHOW");
      lv_label_set_text(s_cap, "TYPE IT AGAIN TO CONFIRM");
      lv_obj_set_style_text_color(s_cap, lv_color_hex(0xF2B84B), 0);
      entry_refresh();
    } else if (s_setup_mode && strcmp(s_first, s_pass) != 0) {
      memset(s_first, 0, sizeof s_first);
      s_first_done = false;
      memset(s_pass, 0, sizeof s_pass);
      s_plen = 0;
      lv_label_set_text(s_cap, "THOSE DIDN'T MATCH. START OVER");
      lv_obj_set_style_text_color(s_cap, lv_color_hex(0xFF4D5E), 0);
      entry_refresh();
    } else {
      memset(s_first, 0, sizeof s_first);
      show_fingerprint();
    }
  }
  else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
    if (s_plen > 0) s_pass[--s_plen] = 0;
    s_flash = false;
    setup_cap_reset();                       // editing cancels a weak-ack prompt
    entry_refresh();
  } else if (strlen(txt) == 1 && txt[0] >= 0x20 && txt[0] < 0x7F) {
    if (s_plen < PASS_MAX) {                 // printable ASCII only (spec, v1)
      s_pass[s_plen++] = txt[0];
      s_pass[s_plen] = 0;
      setup_cap_reset();
      flash_last();
      pop_show(txt[0] == ' ' ? "_" : txt, id);
    }
  }
}

static void show_cb(lv_event_t *e) {
  (void)e;
  s_show = !s_show;
  lv_label_set_text(s_showbtn_lbl, s_show ? "HIDE" : "SHOW");
  entry_refresh();
}

void wallet_ui_ensure_indev(void) { ensure_indev(); }

void wallet_login_open_setup(void (*unlocked_cb)(void)) {
  s_setup_mode = true;
  s_first_done = false;
  s_first[0] = 0;
  wallet_login_open(unlocked_cb);
}

void wallet_login_open(void (*unlocked_cb)(void)) {
  if (wallet_ui_active()) return;
  ensure_indev();
  s_unlocked_cb = unlocked_cb;

  s_login = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_login);
  lv_obj_set_size(s_login, 800, 480);
  lv_obj_set_style_bg_color(s_login, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_login, LV_OPA_COVER, 0);

  s_cap = lv_label_create(s_login);
  lv_label_set_text(s_cap, s_setup_mode ? "CREATE YOUR PASSPHRASE" : "PASSPHRASE");
  lv_obj_set_style_text_color(s_cap, MUT_COL, 0);
  lv_obj_set_style_text_font(s_cap, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(s_cap, 48, 26);

  // show/hide toggle (top-right, inset from the panel's right overscan)
  lv_obj_t *showbtn = lv_button_create(s_login);
  lv_obj_set_style_bg_color(showbtn, KEY_COL, 0);
  lv_obj_set_style_shadow_width(showbtn, 0, 0);
  lv_obj_set_size(showbtn, 92, 40);
  lv_obj_set_pos(showbtn, 650, 18);
  lv_obj_add_event_cb(showbtn, show_cb, LV_EVENT_CLICKED, NULL);
  s_showbtn_lbl = lv_label_create(showbtn);
  lv_label_set_text(s_showbtn_lbl, "SHOW");
  lv_obj_set_style_text_color(s_showbtn_lbl, MUT_COL, 0);
  lv_obj_set_style_text_font(s_showbtn_lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(s_showbtn_lbl);

  s_entry = lv_label_create(s_login);
  lv_obj_set_style_text_font(s_entry, &lv_font_montserrat_28, 0);
  lv_obj_set_width(s_entry, 704);
  lv_label_set_long_mode(s_entry, LV_LABEL_LONG_SCROLL);   // long passphrases scroll
  lv_obj_set_style_text_align(s_entry, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_entry, 48, 92);

  s_count = lv_label_create(s_login);      // live length readout under the entry
  lv_label_set_text(s_count, "");
  lv_obj_set_style_text_color(s_count, MUT_COL, 0);
  lv_obj_set_style_text_font(s_count, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(s_count, 48, 132);

  s_meter = lv_label_create(s_login);      // WEAK/FAIR/STRONG (setup mode only)
  lv_label_set_text(s_meter, "");
  lv_obj_set_style_text_font(s_meter, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(s_meter, 2, 0);
  lv_obj_set_pos(s_meter, 660, 132);
  entry_refresh();

  s_kb = lv_buttonmatrix_create(s_login);
  kb_plane(s_kb, MAP_LOWER);
  lv_obj_set_size(s_kb, 800, 316);
  lv_obj_set_pos(s_kb, 0, 158);
  lv_obj_set_style_bg_color(s_kb, BG_COL, 0);
  lv_obj_set_style_border_width(s_kb, 0, 0);
  lv_obj_set_style_pad_all(s_kb, 6, 0);
  lv_obj_set_style_pad_gap(s_kb, 6, 0);
  // keys: big, dark, readable
  lv_obj_set_style_bg_color(s_kb, KEY_COL, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s_kb, KEYP_COL, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(s_kb, INK_COL, LV_PART_ITEMS);
  lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_28, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(s_kb, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
  // bottom row: plane switch, CANCEL, wide space, OK
  lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x24406B),
                            LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// ---- build identity (shared: Settings footer + wallet home corner) ----
// Honest about what this firmware is: version + commit, then the flash-
// encryption state read from the CHIP eFuse at runtime and the C6 radio
// reset pad read back from the GPIO, never assumed from the build. Bad
// states are amber WARNINGS; good states go calm. Dev builds carry a "dev"
// marker in amber (dev seed, no release hardening).
void wallet_build_id_make(lv_obj_t *parent, int x, int y)
{
  bool enc = false, radio_held = true;   // sim: no radio hardware exists
#ifndef SIMULATOR
  enc = esp_efuse_is_flash_encryption_enabled();
  radio_held = radio_is_held();
#endif
  lv_obj_t *v = lv_label_create(parent);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(v, x, y);
#ifdef KISS_RELEASE
  lv_label_set_text_fmt(v, "KISS %s (%s)", KISS_VERSION_STR, KISS_COMMIT_STR);
  lv_obj_set_style_text_color(v, enc ? MUT_COL : INK_COL, 0);
#else
  lv_label_set_text_fmt(v, "KISS %s dev (%s)", KISS_VERSION_STR, KISS_COMMIT_STR);
  lv_obj_set_style_text_color(v, lv_color_hex(0xF2B84B), 0);
#endif
  lv_obj_t *w = lv_label_create(parent);
  lv_obj_set_style_text_font(w, &lv_font_montserrat_14, 0);
  lv_label_set_text_fmt(w, "-  flash encryption: %s", enc ? "ENABLED" : "OFF");
  lv_obj_set_style_text_color(w, enc ? MUT_COL : lv_color_hex(0xF2B84B), 0);
  lv_obj_update_layout(v);
  lv_obj_set_pos(w, x + lv_obj_get_width(v) + 10, y);

  lv_obj_t *r = lv_label_create(parent);
  lv_obj_set_style_text_font(r, &lv_font_montserrat_14, 0);
  lv_label_set_text_fmt(r, "-  radio: %s", radio_held ? "held in reset" : "NOT HELD");
  lv_obj_set_style_text_color(r, radio_held ? MUT_COL : lv_color_hex(0xF2B84B), 0);
  lv_obj_update_layout(w);
  lv_obj_set_pos(r, x + lv_obj_get_width(v) + 10 + lv_obj_get_width(w) + 10, y);
}
