// Step 3: the login. Spec: the passphrase keyboard is the most-used screen —
// big-key landscape QWERTY, char-flash-then-mask entry, show/hide toggle, and
// the fingerprint shown HUGE after entry (there is no "wrong passphrase" error
// by design; the fingerprint is how you recognize your wallet).
#include "wallet_ui.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_scan.h"    // wallet_scan_open_raw: passphrase-from-QR
#include "wallet_duress_ui.h"  // last setup step: which stroke opens which signer
#include "wallet_seed.h"
#include "wallet_setup.h"   // optional full post-creation recovery rehearsal
#include "wallet_theme.h"

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

#define BG_COL   WT_BG
#define INK_COL  WT_INK
#define MUT_COL  WT_MUT
#define KEY_COL  WT_KEY
#define KEYP_COL wt_accent_pressed()

#define PASS_MAX 128
#define FLASH_MS 900                       // last char visible this long, then masked

static lv_obj_t *s_login;                  // full-screen passphrase entry
static lv_obj_t *s_fpscr;                  // full-screen fingerprint reveal
static lv_obj_t *s_errscr;                 // setup-failed STOP (never enters home)
static lv_obj_t *s_entry;                  // the masked entry label
static lv_obj_t *s_count;                  // "N characters" (catches hidden typos)
static lv_obj_t *s_showbtn_lbl;
static lv_obj_t *s_kflash;                 // key-press flash, child of s_login
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
static bool s_caps_lock;                   // CAPS plane: stays until tapped off
static bool s_one_shot;                    // UPPER plane: one character, then back
static uint32_t s_shift_t0;                // last shift-key tap, for double tap
static bool s_hold_lock_ok;                // this shift press may lock on hold
#define SHIFT_DBL_MS 400                   // two shift taps within this = CAPS

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
    lv_label_set_text(s_meter, tr(STR_L_WEAK));
    lv_obj_set_style_text_color(s_meter, lv_color_hex(0xFF4D5E), 0);
  } else if (bits < 70) {
    lv_label_set_text(s_meter, tr(STR_L_FAIR));
    lv_obj_set_style_text_color(s_meter, lv_color_hex(0xF2B84B), 0);
  } else {
    lv_label_set_text(s_meter, tr(STR_L_STRONG));
    lv_obj_set_style_text_color(s_meter, WT_OK, 0);
  }
}

static lv_obj_t *s_pp_intro;   // setup passphrase-intro screen (owns touch too)

bool wallet_ui_active(void) {
  return s_login != NULL || s_fpscr != NULL || s_errscr != NULL || s_pp_intro != NULL;
}

// ---- keyboard maps (three planes) ----
static const char *MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "#1!", "CANCEL", " ", "OK", ""};
// Two upper planes, identical keys, different shift label. One-shot drops back
// to lowercase after a single character; CAPS stays until tapped again. They
// MUST look different: behind the dots a wrong-case passphrase is invisible,
// and at login there is no error, just a different wallet.
static const char *MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "#1!", "CANCEL", " ", "OK", ""};
static const char *MAP_CAPS[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "CAPS", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
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

// long-passphrase fitting: 28pt holds ~40 glyphs in the 704px slot; past that
// drop to 14pt, and past ~78 show "..." + the tail (the newest chars are what
// the user is checking). Never scroll-animate a masked secret.
static void entry_apply(const char *txt, int chars) {
  lv_obj_set_style_text_font(s_entry, chars > 40 ? wt_font14()
                                                 : wt_font28(), 0);
  // SHOW means the user has already decided nobody is looking, so showing only
  // the tail buys nothing and hides the half they are trying to check (and the
  // half they are about to backspace through). Wrap instead: 128 chars of
  // font14 fit two lines of the 704px slot.
  if (s_show) {
    lv_obj_set_width(s_entry, 704);
    lv_label_set_long_mode(s_entry, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_entry, txt);
    return;
  }
  lv_label_set_long_mode(s_entry, LV_LABEL_LONG_SCROLL);
  if (chars > 78) {
    const char *p = txt + strlen(txt);
    int keep = 76;
    while (keep && p > txt) {                    // step back whole UTF-8 chars
      p--;
      while (p > txt && ((*p & 0xC0) == 0x80)) p--;
      keep--;
    }
    static char tail[3 * 76 + 8];
    snprintf(tail, sizeof tail, "...%s", p);
    lv_label_set_text(s_entry, tail);
  } else {
    lv_label_set_text(s_entry, txt);
  }
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
      lv_label_set_text_fmt(s_count, tr(STR_L_COUNT_SP_FMT), s_plen, sp);
    else lv_label_set_text_fmt(s_count, tr(STR_L_COUNT_FMT), s_plen);
  }
  if (s_plen == 0) {
    lv_obj_set_style_text_font(s_entry, wt_font28(), 0);
    lv_label_set_text(s_entry, tr(STR_L_TYPE_PROMPT));
    lv_obj_set_style_text_color(s_entry, MUT_COL, 0);
    return;
  }
  lv_obj_set_style_text_color(s_entry, wt_accent(), 0);
  if (s_show) {
    entry_apply(s_pass, s_plen);
    return;
  }
  int n = 0;
  int shown = s_flash ? s_plen - 1 : s_plen;   // chars rendered as dots
  for (int i = 0; i < shown && n < (int)sizeof(buf) - 4; i++) {
    buf[n++] = '\xE2'; buf[n++] = '\x80'; buf[n++] = '\xA2';  // U+2022 bullet
  }
  if (s_flash && n < (int)sizeof(buf) - 2) buf[n++] = s_pass[s_plen - 1];
  buf[n] = 0;
  entry_apply(buf, s_plen);
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

// The caption above the entry carries two unrelated kinds of text. One is a
// quiet eyebrow naming the field ("PASSPHRASE"); the other is the state that
// decides what the owner types next -- "TYPE IT AGAIN TO CONFIRM", "THOSE
// DIDN'T MATCH", "THAT OPENS A DIFFERENT WALLET". Only the eyebrow belongs at
// 14. The rest auto-fit into the 486px left of the SCAN button, wrapping
// rather than running underneath it when a translation is long.
#define CAP_W 486
static void cap_set(const char *txt, lv_color_t col, bool alert) {
  if (!s_cap) return;
  lv_label_set_text(s_cap, txt);
  lv_obj_set_style_text_color(s_cap, col, 0);
  lv_obj_set_style_text_font(s_cap, alert ? wt_body_font(txt, CAP_W, 58)
                                          : wt_font14(), 0);
  lv_obj_set_pos(s_cap, 48, alert ? 22 : 28);
}

// after a weak-ack warning, any edit returns the caption to the stage prompt
static void setup_cap_reset(void) {
  if (!s_setup_mode || !s_weak_ack || !s_cap) return;
  s_weak_ack = false;
  if (s_first_done) cap_set(tr(STR_L_TYPE_AGAIN), lv_color_hex(0xF2B84B), true);
  else              cap_set(tr(STR_L_CREATE_YOUR_PASS), MUT_COL, false);
}

static lv_obj_t *s_cancel_ovl;             // "cancel setup?" confirm (setup mode only)
static lv_obj_t *s_weak_ovl;               // weak-passphrase deliberate-use card
static lv_obj_t *s_warnscr;                // post-setup passphrase warning (one screen)
static bool s_backup_verified;             // every word + exact passphrase rehearsed
static bool s_backup_verify_pass;          // keyboard is checking that passphrase now

static void wipe_and_close(void) {
  if (s_cancel_ovl) { lv_obj_delete_async(s_cancel_ovl); s_cancel_ovl = NULL; }
  if (s_weak_ovl) { lv_obj_delete_async(s_weak_ovl); s_weak_ovl = NULL; }
  if (s_warnscr) { lv_obj_delete_async(s_warnscr); s_warnscr = NULL; }
  // cancelling setup before the fingerprint confirm drops the staged seed, so
  // an abandoned setup never leaves a half-made wallet in flash (P0 safety net)
  if (s_setup_mode) wallet_seed_discard();
  memset(s_pass, 0, sizeof(s_pass));         // never keep the passphrase around
  memset(s_first, 0, sizeof(s_first));
  s_setup_mode = false;
  s_first_done = false;
  s_weak_ack = false;
  s_backup_verified = false;
  s_backup_verify_pass = false;
  s_plen = 0;
  s_show = false;
  s_flash = false;
  if (s_mask_tmr) { lv_timer_delete(s_mask_tmr); s_mask_tmr = NULL; }
  if (s_pop_tmr) { lv_timer_delete(s_pop_tmr); s_pop_tmr = NULL; }
  s_pop = NULL; s_pop_lbl = NULL; s_kflash = NULL;   // children of s_login: die with it
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
  // 540x268 only ever fit this body at 14pt: at 23 it needs four lines and the
  // card had 106px between the title and the buttons. Grown so the readable
  // size is the one that fits, not the one that survives.
  lv_obj_set_size(card, 560, 300);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, BG_COL, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = lv_label_create(card);
  lv_label_set_text(t, tr(STR_L_CANCEL_SETUP_T));
  lv_obj_set_style_text_color(t, INK_COL, 0);
  lv_obj_set_style_text_font(t, wt_font28(), 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);

  // y=82 down to the button row (300 - 22 - 58 = 220) leaves 138px, so 23 fits
  // on four lines. wt_note takes it and drops to 14 only if it cannot.
  lv_obj_t *s = wt_note(card, tr(STR_L_CANCEL_SETUP_B), 30, 82, 500, 124);
  lv_obj_set_style_text_color(s, MUT_COL, 0);
  lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *keep = lv_button_create(card);           // safe choice, green outline
  lv_obj_set_size(keep, 232, 58);
  lv_obj_align(keep, LV_ALIGN_BOTTOM_LEFT, 18, -22);
  lv_obj_set_style_bg_color(keep, KEY_COL, 0);
  lv_obj_set_style_border_color(keep, lv_color_hex(0x35D07F), 0);
  lv_obj_set_style_border_width(keep, 2, 0);
  lv_obj_set_style_shadow_width(keep, 0, 0);
  lv_obj_add_event_cb(keep, cancel_keep_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *kl = lv_label_create(keep);
  lv_label_set_text(kl, tr(STR_L_KEEP_GOING));
  lv_obj_set_style_text_color(kl, INK_COL, 0);
  lv_obj_set_style_text_font(kl,
      wt_body_font(tr(STR_L_KEEP_GOING), 204, 42), 0);
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
  lv_label_set_text(dl, tr(STR_L_DISCARD));
  lv_obj_set_style_text_color(dl, lv_color_hex(0xFF4D5E), 0);
  lv_obj_set_style_text_font(dl,
      wt_body_font(tr(STR_L_DISCARD), 204, 42), 0);
  lv_obj_set_style_text_letter_space(dl, 2, 0);
  lv_obj_center(dl);
}

// Capture the first setup entry and move to the exact-repeat stage. Both the
// normal-strength OK path and the weak-passphrase card's explicit USE ANYWAY
// action land here, so the safety-critical transition has one implementation.
static void setup_accept_first(void) {
  memcpy(s_first, s_pass, sizeof s_first);
  s_first_done = true;
  s_weak_ack = false;
  memset(s_pass, 0, sizeof s_pass);
  s_plen = 0;
  s_show = false;
  if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
  cap_set(tr(STR_L_TYPE_AGAIN), lv_color_hex(0xF2B84B), true);
  entry_refresh();
}

static void weak_back_cb(lv_event_t *e) {
  (void)e;
  if (s_weak_ovl) { lv_obj_delete_async(s_weak_ovl); s_weak_ovl = NULL; }
  // Keep the entry intact so the owner can lengthen it instead of retyping.
  setup_cap_reset();
}

static void weak_use_cb(lv_event_t *e) {
  (void)e;
  if (s_weak_ovl) { lv_obj_delete_async(s_weak_ovl); s_weak_ovl = NULL; }
  setup_accept_first();
}

// A weak passphrase is a consequential choice, not a status tag. The old
// warning replaced the keyboard caption with a long sentence, which rendered
// at the smallest face and asked for an unexplained second press of OK.
// Keep the keyboard and entered secret behind a modal card, make the warning
// readable at 23px, and name both outcomes.
static void show_weak_confirm(void) {
  if (s_weak_ovl) return;
  s_weak_ack = true;

  s_weak_ovl = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_weak_ovl);
  lv_obj_set_size(s_weak_ovl, 800, 480);
  lv_obj_set_pos(s_weak_ovl, 0, 0);
  lv_obj_set_style_bg_color(s_weak_ovl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_weak_ovl, 200, 0);
  lv_obj_add_flag(s_weak_ovl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_weak_ovl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(s_weak_ovl);

  lv_obj_t *card = lv_obj_create(s_weak_ovl);
  lv_obj_remove_style_all(card);
  // 360, not 440: the body is two lines in English and at most four in the
  // longest translation, and a card sized for text that is not there reads as
  // a missing paragraph. Height here is set by what the copy needs.
  lv_obj_set_size(card, 704, 360);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, BG_COL, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xFF4D5E), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *icon = wt_lbl(card, LV_SYMBOL_WARNING, 0, 0,
                          &lv_font_montserrat_48, lv_color_hex(0xFF4D5E));
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 30);

  lv_obj_t *t = wt_lbl(card, tr(STR_L_WEAK_T), 0, 0, wt_font28(), INK_COL);
  lv_obj_set_style_text_letter_space(t, 2, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 88);

  lv_obj_t *b = wt_note(card, tr(STR_L_WEAK_ACK), 32, 144, 640, 124);
  lv_obj_set_style_text_color(b, MUT_COL, 0);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);

  // 314 wide, not 280: "USE ANYWAY" is one word in English and three in most
  // other languages, and this is the button that decides whether a guessable
  // passphrase guards real funds. It gets the width it needs to stay readable.
  wt_pillh(card, tr(STR_C_BACK), 18, 284, 314, 56, weak_back_cb, NULL);
  lv_obj_t *use = wt_pillh(card, tr(STR_L_USE_ANYWAY), 372, 284, 314, 56,
                           weak_use_cb, NULL);
  wt_pill_select(use, true);
}

// ---- fingerprint reveal ----
static void setup_fail_screen(void);
static void setup_warn_screen(void);

// dismiss the STOP screen back to the game; nothing was saved
static void setup_fail_dismiss_cb(lv_event_t *e) {
  (void)e;
  s_caps_lock = false; s_one_shot = false; s_shift_t0 = 0; s_hold_lock_ok = false;
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
  s_pop = NULL; s_pop_lbl = NULL; s_kflash = NULL;
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
  lv_label_set_text(t, s_setup_mode ? tr(STR_L_FAIL_SETUP_T)
                                    : tr(STR_L_FAIL_OPEN_T));
  lv_obj_set_style_text_color(t, INK_COL, 0);
  lv_obj_set_style_text_font(t, wt_font28(), 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 190);

  lv_obj_t *s = wt_note(s_errscr, s_setup_mode ? tr(STR_L_FAIL_SETUP_B)
                                               : tr(STR_L_FAIL_OPEN_B),
                        40, 232, 720, 58);
  lv_obj_set_style_text_color(s, MUT_COL, 0);
  lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *btn = lv_button_create(s_errscr);
  lv_obj_set_size(btn, 200, 56);
  lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 300);
  lv_obj_set_style_bg_color(btn, KEY_COL, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, setup_fail_dismiss_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, tr(STR_C_BACK));
  lv_obj_set_style_text_color(bl, INK_COL, 0);
  lv_obj_set_style_text_font(bl, wt_body_font(tr(STR_C_BACK), 180, 44), 0);
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

// The decoy signer opens straight from the game with no login screen at all,
// so nothing here runs to record its fingerprint. main.c sets it directly
// rather than duplicating the home-chip logic on that path.
void wallet_ui_set_last_fp(const uint8_t fp[4]) { memcpy(s_last_fp, fp, 4); }

// Post-setup, pre-home: recovery words + passphrase rederive this wallet.
// Exposed words permit offline passphrase guessing, and nothing can recover a
// lost passphrase. Session is open + seed committed; OK finishes the unlock.
// The stroke chooser is the LAST thing in setup, and it has to be last: the
// decoy signer IS this seed with no passphrase, so the choice only means
// anything once a real passphrase exists to contrast it with. Whatever the
// owner does there -- set both strokes, or skip -- the wallet then opens.
static void (*s_after_duress)(void);

static void duress_done_cb(void) {
  void (*cb)(void) = s_after_duress;
  s_after_duress = NULL;
  if (cb) cb();
}

static void setup_warn_ok_cb(lv_event_t *e) {
  (void)e;
  void (*cb)(void) = s_unlocked_cb;
  const bool nopass_setup = wallet_session_decoy();   // see setup_warn_screen
  wipe_and_close();                        // also deletes s_warnscr
  // A wallet with no passphrase IS the decoy: wallet_session_open(NULL) is what
  // both ways in reach. Offering to configure a "real" stroke here would let
  // someone believe their funds sit behind it and hand over the spare having
  // hidden nothing -- the feature failing in exactly the situation it exists
  // for. So do not offer it; the warning screen above says why.
  if (nopass_setup) {
    if (cb) cb();
    return;
  }
  s_after_duress = cb;
  wallet_duress_ui_open(lv_screen_active(), duress_done_cb);
}

static void setup_warn_words_done(void)
{
  if (!wallet_setup_verify_succeeded()) {
    setup_warn_screen();                    // optional check cancelled or did not match
    return;
  }

  // The words matched. Now throw away the passphrase that created the session
  // and require it fresh: comparing the resulting fingerprint proves the exact
  // words + exact passphrase combination without ever storing that passphrase.
  s_backup_verify_pass = true;
  memset(s_pass, 0, sizeof s_pass);
  memset(s_first, 0, sizeof s_first);
  s_plen = 0;
  s_show = false;
  s_flash = false;
  if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
  if (s_fpscr) { lv_obj_delete_async(s_fpscr); s_fpscr = NULL; }
  if (s_login) {
    lv_obj_clear_flag(s_login, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_login);
  }
  cap_set(tr(STR_L_VERIFY_PASS), lv_color_hex(0xF2B84B), true);
  entry_refresh();
}

static void setup_warn_verify_cb(lv_event_t *e)
{
  (void)e;
  if (s_warnscr) { lv_obj_delete_async(s_warnscr); s_warnscr = NULL; }
  wallet_setup_open_verify(lv_screen_active(), setup_warn_words_done);
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

  // Same again: with no passphrase the old copy explained the wrong thing on
  // the screen whose whole job is teaching the owner what they just made.
  //
  // NOT s_plen here. The optional full-backup rehearsal deliberately clears the
  // passphrase and asks for it fresh (setup_warn_words_done), and the compare
  // path zeroes s_plen again the moment it matches -- so by the time this runs
  // after a rehearsal, s_plen is 0 on a wallet that definitely HAS a
  // passphrase. Reading it here told those owners they had none and skipped
  // their stroke setup. The open session knows the truth and cannot drift.
  const bool warn_nopass = wallet_session_decoy();
  const int warn_t = warn_nopass ? STR_L_WARN_T_NOPASS : STR_L_WARN_T;
  const int warn_b = warn_nopass ? STR_L_WARN_B_NOPASS : STR_L_WARN_B;

  lv_obj_t *t = lv_label_create(s_warnscr);
  lv_label_set_text(t, tr(warn_t));
  lv_obj_set_style_text_color(t, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_text_font(t, wt_font28(), 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

  // body runs from y=92 down to the fingerprint: auto-fit keeps the
  // short English copy big and a long translation off the fingerprint
  lv_obj_t *b = lv_label_create(s_warnscr);
  lv_label_set_text(b, tr(warn_b));
  lv_obj_set_style_text_color(b, INK_COL, 0);
  lv_obj_set_style_text_font(b, wt_body_font(tr(warn_b), 740, 204), 0);
  lv_obj_set_width(b, 740);
  lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 92);

  lv_obj_t *f = lv_label_create(s_warnscr);
  lv_label_set_text_fmt(f, "%02X%02X%02X%02X",
                        s_last_fp[0], s_last_fp[1], s_last_fp[2], s_last_fp[3]);
  lv_obj_set_style_text_color(f, INK_COL, 0);
  lv_obj_set_style_text_font(f, wt_font28(), 0);
  lv_obj_set_style_text_letter_space(f, 4, 0);
  lv_obj_align(f, LV_ALIGN_TOP_MID, 0, 300);   // body below now runs to 295

  lv_obj_t *state = lv_label_create(s_warnscr);
  lv_label_set_text(state, s_backup_verified ? tr_sym(LV_SYMBOL_OK, STR_L_BACKUP_VERIFIED)
                                              : tr(STR_L_BACKUP_UNVERIFIED));
  lv_obj_set_style_text_color(state, s_backup_verified ? WT_OK : WT_STOP, 0);
  // whether the backup is verified decides whether the red ring stays: that is
  // a status the owner reads, not a tag, so it belongs on the ladder.
  lv_obj_set_style_text_font(state, wt_body_font(s_backup_verified
                                                   ? tr(STR_L_BACKUP_VERIFIED)
                                                   : tr(STR_L_BACKUP_UNVERIFIED),
                                                 720, 29), 0);
  lv_obj_set_style_text_letter_space(state, 2, 0);
  lv_obj_align(state, LV_ALIGN_TOP_MID, 0, 348);

  lv_obj_t *verify = wt_pillh(s_warnscr, tr(STR_L_VERIFY_FULL_BACKUP),
                              48, 398, 300, 66, setup_warn_verify_cb, NULL);
  if (!s_backup_verified)
    lv_obj_set_style_border_color(verify, WT_WARN, 0);

  // Skipping is allowed, but it must look like a conscious decision. The red
  // ring disappears only after every word and the exact passphrase have both
  // recreated the fingerprint above.
  lv_obj_t *ok = wt_pillh(s_warnscr, tr(STR_C_I_UNDERSTAND),
                          430, 398, 320, 66, setup_warn_ok_cb, NULL);
  lv_obj_set_style_border_width(ok, 2, 0);
  lv_obj_set_style_border_color(ok, s_backup_verified ? WT_OK : WT_STOP, 0);
}

// reveal pop-in: the code card rises + fades in (one-shot, no per-frame cost after)
static void fp_pop_ty_cb(void *v, int32_t y) { lv_obj_set_style_translate_y((lv_obj_t *)v, y, 0); }
static void fp_pop_opa_cb(void *v, int32_t o) { lv_obj_set_style_opa((lv_obj_t *)v, o, 0); }

static void show_fingerprint(void) {
  uint8_t fp[4] = {0};
  if (wallet_fingerprint(s_plen ? s_pass : NULL, fp) != 0) {
    // derivation failed: STOP here. Never cache or reveal the zeroed fp —
    // it would flow into s_last_fp and render as the "SIGNING AS" identity.
    if (s_setup_mode) wallet_seed_discard();
    setup_fail_screen();
    return;
  }
  memcpy(s_last_fp, fp, 4);

  s_fpscr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_fpscr);
  lv_obj_set_size(s_fpscr, 800, 480);
  lv_obj_set_style_bg_color(s_fpscr, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_fpscr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_fpscr, LV_OBJ_FLAG_CLICKABLE);  // buttons only, no tap-anywhere

  lv_obj_t *cap = lv_label_create(s_fpscr);
  lv_label_set_text(cap, tr(STR_L_FP_CAP));
  lv_obj_set_style_text_color(cap, MUT_COL, 0);
  lv_obj_set_style_text_font(cap, wt_font14(), 0);
  lv_obj_set_style_text_letter_space(cap, 2, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 64);

  // the fingerprint sits in a chip-style box, like the home screen's corner chip
  lv_obj_t *box = lv_obj_create(s_fpscr);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 420, 118);
  lv_obj_set_style_radius(box, 16, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, wt_accent(), 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x0C1018), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 104);

  lv_obj_t *big = lv_label_create(box);
  lv_label_set_text_fmt(big, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
  lv_obj_set_style_text_color(big, wt_accent(), 0);
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

  // These two lines are how an owner learns what the fingerprint above is FOR,
  // on the first screen the device ever shows them. They were 14pt sitting 28px
  // apart, which is one 23pt line plus nothing -- so the room had to grow with
  // the type: the chip ends at 222 and the OPEN pill starts at 388, and two
  // 58px slots at 246 and 310 land inside that with clearance at both ends.
  //
  // An EMPTY passphrase is a legitimate choice with its own confirmation card,
  // and both of these lines used to describe a passphrase the owner does not
  // have -- on the first screen the device ever shows them.
  const bool nopass = (s_plen == 0);
  lv_obj_t *note = wt_note(s_fpscr,
                           tr(nopass ? STR_L_FP_NOTE_NOPASS : STR_L_FP_NOTE),
                           50, 246, 700, 58);
  lv_obj_set_style_text_color(note, lv_color_hex(0xB9C2D4), 0);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *note2 = wt_note(s_fpscr,
                            tr(nopass ? STR_L_FP_NOTE2_NOPASS : STR_L_FP_NOTE2),
                            50, 310, 700, 58);
  lv_obj_set_style_text_color(note2, MUT_COL, 0);
  lv_obj_set_style_text_align(note2, LV_TEXT_ALIGN_CENTER, 0);

  // bottom action pill
  lv_obj_t *go = lv_obj_create(s_fpscr);
  lv_obj_remove_style_all(go);
  lv_obj_set_size(go, 260, 52);
  lv_obj_set_style_radius(go, 26, 0);
  lv_obj_set_style_bg_color(go, wt_accent_bg(), 0);
  lv_obj_set_style_bg_color(go, wt_accent_pressed(), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(go, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(go, 2, 0);
  lv_obj_set_style_border_color(go, wt_accent(), 0);
  lv_obj_align(go, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_add_flag(go, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(go, fp_tap_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *gol = lv_label_create(go);
  lv_label_set_text(gol, tr(STR_L_TAP_TO_OPEN));
  lv_obj_set_style_text_color(gol, INK_COL, 0);
  lv_obj_set_style_text_font(gol, wt_body_font(tr(STR_L_TAP_TO_OPEN), 236, 40), 0);
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
  lv_label_set_text(backl, tr(STR_C_BACK));
  lv_obj_set_style_text_color(backl, MUT_COL, 0);
  lv_obj_set_style_text_font(backl, wt_body_font(tr(STR_C_BACK), 116, 40), 0);
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

static lv_anim_t s_pop_a;
static void pop_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }

static void pop_hide_cb(lv_timer_t *t) {
  (void)t;
  s_pop_tmr = NULL;
  if (s_pop) lv_obj_add_flag(s_pop, LV_OBJ_FLAG_HIDDEN);
}

// ---- key flash: the pressed key lights and fades ----
// The board has no vibration motor, so "the tap registered" has to be carried
// by the eye alone. The callout above the key answers WHAT was typed; this
// answers THAT something was, and it outlives the finger by a beat so a fast
// typist still sees each press land. Opacity only: transform_scale allocates
// an LVGL layer and hangs the device.
static lv_anim_t s_kflash_a;

static void kflash_opa(void *o, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
  lv_obj_set_style_border_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}

static void key_flash(uint32_t id) {
  if (!s_login) return;
  int kx, ky, kw, kh;
  key_rect(id, &kx, &ky, &kw, &kh);
  if (!s_kflash) {
    s_kflash = lv_obj_create(s_login);
    lv_obj_remove_style_all(s_kflash);
    lv_obj_set_style_radius(s_kflash, 10, 0);
    lv_obj_set_style_bg_color(s_kflash, wt_accent(), 0);
    lv_obj_set_style_border_width(s_kflash, 2, 0);
    lv_obj_set_style_border_color(s_kflash, wt_accent(), 0);
    lv_obj_remove_flag(s_kflash, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_anim_delete(s_kflash, kflash_opa);      // retrigger cleanly on a fast repeat
  lv_obj_set_pos(s_kflash, kx, ky);
  lv_obj_set_size(s_kflash, kw, kh);
  lv_obj_move_foreground(s_kflash);
  if (s_pop) lv_obj_move_foreground(s_pop);  // the callout stays on top of it
  lv_anim_init(&s_kflash_a);
  lv_anim_set_var(&s_kflash_a, s_kflash);
  lv_anim_set_exec_cb(&s_kflash_a, kflash_opa);
  lv_anim_set_path_cb(&s_kflash_a, lv_anim_path_ease_out);
  lv_anim_set_values(&s_kflash_a, 110, 0);
  lv_anim_set_duration(&s_kflash_a, 190);
  lv_anim_start(&s_kflash_a);
}

static void pop_show(const char *ch, uint32_t id) {
  if (!s_pop) {                             // iPhone-style key callout: larger than the
    s_pop = lv_obj_create(s_login);         // key, big glyph, bottom overlapping the
    lv_obj_remove_style_all(s_pop);         // pressed key so it visibly grows out of it
    lv_obj_set_size(s_pop, 96, 96);
    lv_obj_set_style_radius(s_pop, 20, 0);
    lv_obj_set_style_bg_color(s_pop, wt_accent_bg(), 0);
    lv_obj_set_style_bg_opa(s_pop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pop, 2, 0);
    lv_obj_set_style_border_color(s_pop, wt_accent(), 0);
    lv_obj_set_style_shadow_width(s_pop, 18, 0);
    lv_obj_set_style_shadow_color(s_pop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_pop, LV_OPA_60, 0);
    s_pop_lbl = lv_label_create(s_pop);
    lv_obj_set_style_text_color(s_pop_lbl, wt_accent(), 0);
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

  // Rise, do not fade. The lift is what the eye reads as "that registered".
  // NOT lv_obj_set_style_opa: the callout has a child label and a shadow, and
  // whole-object opacity makes LVGL render it through a layer buffer. That
  // allocation hangs the device (the same trap as transform_scale) and it hung
  // the simulator on the very first keypress. translate_y is layer free.
  lv_anim_delete(s_pop, pop_ty);
  lv_anim_init(&s_pop_a);
  lv_anim_set_var(&s_pop_a, s_pop);
  lv_anim_set_path_cb(&s_pop_a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&s_pop_a, pop_ty);
  lv_anim_set_values(&s_pop_a, 10, -6);      // starts low, lifts off the key
  lv_anim_set_duration(&s_pop_a, 190);
  lv_anim_start(&s_pop_a);

  key_flash(id);
  if (s_pop_tmr) lv_timer_delete(s_pop_tmr);
  s_pop_tmr = lv_timer_create(pop_hide_cb, 300, NULL);
  lv_timer_set_repeat_count(s_pop_tmr, 1);
}

// ---- keyboard events ----
static void kb_cb(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(kb);
  const char *txt = lv_buttonmatrix_get_button_text(kb, id);
  if (!txt) return;

  // shift: tap once for a single capital, HOLD it to lock (kb_long_cb), or tap
  // twice quickly for the same lock. Tapping the locked key unlocks. All three
  // are what a phone keyboard does.
  if (strcmp(txt, "ABC") == 0 || strcmp(txt, "abc") == 0) {
    uint32_t now = lv_tick_get();
    bool dbl = s_shift_t0 && lv_tick_elaps(s_shift_t0) < SHIFT_DBL_MS;
    s_shift_t0 = now;
    s_hold_lock_ok = true;
    if (dbl) { s_caps_lock = true;  s_one_shot = false; kb_plane(kb, MAP_CAPS); }
    else if (txt[0] == 'A') { s_one_shot = true;  kb_plane(kb, MAP_UPPER); }
    else                    { s_one_shot = false; kb_plane(kb, MAP_LOWER); }
  }
  else if (strcmp(txt, "CAPS") == 0) {
    s_caps_lock = false; s_one_shot = false; s_shift_t0 = 0;
    // a slow tap to UNLOCK must not be read as a hold and re-lock instantly
    s_hold_lock_ok = false;
    kb_plane(kb, MAP_LOWER);
  }
  else if (strcmp(txt, "#1!") == 0) kb_plane(kb, MAP_SYM);
  else if (strcmp(txt, "#2~") == 0) kb_plane(kb, MAP_SYM2);
  else if (strcmp(txt, tr(STR_C_CANCEL)) == 0) {
    if (s_backup_verify_pass) {
      // This rehearsal is optional. Cancel returns to the warning with the
      // unverified red state; it does not abandon the wallet just created.
      s_backup_verify_pass = false;
      memset(s_pass, 0, sizeof s_pass);
      s_plen = 0;
      entry_refresh();
      setup_warn_screen();
    }
    else if (s_setup_mode) show_cancel_confirm();   // don't throw away a fresh seed on one tap
    else wipe_and_close();                     // normal login: nothing to lose
  }
  else if (strcmp(txt, "OK") == 0) {
    if (s_backup_verify_pass) {
      uint8_t fp[4] = {0};
      bool match = wallet_fingerprint(s_plen ? s_pass : NULL, fp) == 0
                && memcmp(fp, s_last_fp, sizeof fp) == 0;
      memset(fp, 0, sizeof fp);
      memset(s_pass, 0, sizeof s_pass);
      s_plen = 0;
      s_show = false;
      s_flash = false;
      if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
      if (!match) {
        cap_set(tr(STR_L_BACKUP_PASS_BAD), WT_STOP, true);
        entry_refresh();
      } else {
        s_backup_verify_pass = false;
        s_backup_verified = true;
        entry_refresh();
        setup_warn_screen();
      }
    }
    else if (s_setup_mode && !s_first_done && pass_bits() < 40) {
      show_weak_confirm();
    } else if (s_setup_mode && !s_first_done) {
      // setup: capture the first entry, demand it again — a typo here is an
      // unreproducible passphrase (= lost coins) later
      setup_accept_first();
    } else if (s_setup_mode && strcmp(s_first, s_pass) != 0) {
      memset(s_first, 0, sizeof s_first);
      s_first_done = false;
      memset(s_pass, 0, sizeof s_pass);
      s_plen = 0;
      cap_set(tr(STR_L_NO_MATCH), lv_color_hex(0xFF4D5E), true);
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
    if (s_one_shot) {                        // one capital, then back to lowercase
      s_one_shot = false;
      kb_plane(kb, MAP_LOWER);
    }
  }
}

// Two holds, both the phone gesture:
//   hold the SHIFT key   -> caps lock on
//   hold a LETTER        -> that one letter capitalised, plane unchanged
//
// A buttonmatrix fires VALUE_CHANGED on PRESS, so by the time the hold is
// recognised the press has already been handled: the plane has flipped, and a
// lowercase letter has ALREADY been typed. Upcase it in place rather than
// appending, or a hold silently enters two characters -- invisible behind the
// dots, and a passphrase you can never reproduce.
static void kb_long_cb(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(kb);
  const char *txt = lv_buttonmatrix_get_button_text(kb, id);
  if (!txt) return;

  // Read whichever shift label is showing NOW, not the one that was tapped:
  // the press already swapped the plane under the finger.
  if (strcmp(txt, "ABC") == 0 || strcmp(txt, "abc") == 0 ||
      strcmp(txt, "CAPS") == 0) {
    if (!s_hold_lock_ok) return;            // this press was the unlock tap
    s_hold_lock_ok = false;
    s_caps_lock = true; s_one_shot = false; s_shift_t0 = 0;
    kb_plane(kb, MAP_CAPS);
    return;
  }

  if (strlen(txt) != 1) return;
  char c = txt[0];
  if (c < 'a' || c > 'z') return;            // only letters have another case
  if (s_plen == 0 || s_pass[s_plen - 1] != c) return;   // not the char just typed
  s_pass[s_plen - 1] = (char)(c - 'a' + 'A');
  setup_cap_reset();
  flash_last();
  char up[2] = { s_pass[s_plen - 1], 0 };
  pop_show(up, id);
}

// ---- passphrase from a QR ----
// The scanned text becomes the passphrase exactly as decoded, including case
// and spaces: a passphrase that cannot be reproduced byte for byte is a wallet
// nobody can reopen. Refuse anything the keyboard could not have typed, for
// the same reason (see the printable-ASCII rule above).
// The scan screen closes itself before calling back, so the keyboard underneath
// is already visible again: fill it in place rather than rebuilding it (which
// wallet_login_open would refuse to do anyway while a login is open).
static void pp_scan_text_cb(const char *txt, size_t len) {
  if (!s_login || len == 0 || len > PASS_MAX)
    return;                                 // unusable: leave what was typed
  for (size_t i = 0; i < len; i++)
    if (txt[i] < 0x20 || txt[i] > 0x7E)
      return;
  memcpy(s_pass, txt, len);
  s_pass[len] = 0;
  s_plen = (int)len;
  entry_refresh();
}

static void pp_scan_cancel_cb(void) { }     // the keyboard was never torn down

static void pp_scan_go_cb(lv_event_t *e) {
  lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
  wallet_scan_open_raw(lv_screen_active(), pp_scan_text_cb, pp_scan_cancel_cb);
}

static void pp_scan_back_cb(lv_event_t *e) {
  lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void pp_scan_warn_cb(lv_event_t *e) {
  (void)e;
  lv_obj_t *scr = wt_screen(lv_screen_active(), tr(STR_L_SCAN_WARN_T),
                            tr(STR_L_SCAN_WARN_S));
  lv_obj_move_foreground(scr);
  lv_obj_t *b = wt_lbl(scr, tr(STR_L_SCAN_WARN_B), 48, 122,
                       wt_body_font(tr(STR_L_SCAN_WARN_B), 704, 274), WT_MUT);
  lv_obj_set_width(b, 704);
  lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
  lv_obj_t *go = wt_pill(scr, tr(STR_L_SCAN_GO), 48, 404, 300, pp_scan_go_cb, scr);
  wt_pill_primary(go);
  wt_pill(scr, tr(STR_C_BACK), 610, 404, 140, pp_scan_back_cb, scr);
}

static void show_cb(lv_event_t *e) {
  (void)e;
  s_show = !s_show;
  lv_label_set_text(s_showbtn_lbl, s_show ? tr(STR_L_HIDE) : tr(STR_L_SHOW));
  entry_refresh();
}

void wallet_ui_ensure_indev(void) { ensure_indev(); }

// setup-only interstitial: the passphrase deserves one calm screen of WHY
// before the keyboard appears (meter + type-twice enforce the HOW).
static void (*s_setup_next_cb)(void);
static void pp_intro_go_cb(lv_event_t *e) {
  (void)e;
  lv_obj_delete_async(s_pp_intro);
  s_pp_intro = NULL;
  wallet_login_open(s_setup_next_cb);
}

void wallet_login_open_setup(void (*unlocked_cb)(void)) {
  s_setup_mode = true;
  s_first_done = false;
  s_backup_verified = false;
  s_backup_verify_pass = false;
  s_first[0] = 0;
  ensure_indev();
  s_setup_next_cb = unlocked_cb;
  lv_obj_t *scr = wt_screen(lv_screen_active(), tr(STR_L_PPINTRO_T),
                            tr(STR_L_PPINTRO_S));
  s_pp_intro = scr;
  lv_obj_t *ib = wt_lbl(scr, tr(STR_L_PPINTRO_B),
      48, 116, wt_body_font(tr(STR_L_PPINTRO_B), 704, 280), WT_MUT);
  lv_obj_set_width(ib, 704);
  lv_label_set_long_mode(ib, LV_LABEL_LONG_WRAP);
  lv_obj_t *go = wt_pill(scr, tr(STR_L_CREATE_PASS_BTN), 48, 404, 280, pp_intro_go_cb, NULL);
  wt_pill_primary(go);
}

// A label whose text swaps at runtime (SHOW <-> HIDE) has to be sized for BOTH
// strings: fitting only the one present at build time clips the other in every
// language where they differ in length, and an unwidthed LVGL label clips
// silently. Pick the smaller of the two rungs and set it once.
static const lv_font_t *font_for_both(const char *a, const char *b,
                                      int w, int max_h) {
  const lv_font_t *fa = wt_body_font(a, w, max_h);
  const lv_font_t *fb = wt_body_font(b, w, max_h);
  if (fa == wt_font14() || fb == wt_font14()) return wt_font14();
  if (fa == wt_font23() || fb == wt_font23()) return wt_font23();
  return fa;
}

void wallet_login_open(void (*unlocked_cb)(void)) {
  if (wallet_ui_active()) return;
  ensure_indev();
  s_unlocked_cb = unlocked_cb;
  // localized CANCEL on every plane (array slot 32 = button id 29); kb_cb
  // compares against the same tr() pointer, so the match is exact
  MAP_LOWER[32] = MAP_UPPER[32] = MAP_SYM[32] = MAP_SYM2[32] = tr(STR_C_CANCEL);

  s_login = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_login);
  lv_obj_set_size(s_login, 800, 480);
  lv_obj_set_style_bg_color(s_login, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_login, LV_OPA_COVER, 0);

  s_cap = lv_label_create(s_login);
  lv_obj_set_width(s_cap, CAP_W);          // stop long alerts under SCAN/SHOW
  lv_label_set_long_mode(s_cap, LV_LABEL_LONG_WRAP);
  cap_set(s_setup_mode ? tr(STR_L_CREATE_YOUR_PASS) : tr(STR_L_PASSPHRASE_CAP),
          MUT_COL, false);

  // show/hide toggle (top-right, inset from the panel's right overscan)
  lv_obj_t *showbtn = lv_button_create(s_login);
  lv_obj_set_style_bg_color(showbtn, KEY_COL, 0);
  lv_obj_set_style_shadow_width(showbtn, 0, 0);
  lv_obj_set_size(showbtn, 92, 40);
  lv_obj_set_pos(showbtn, 650, 18);
  lv_obj_add_event_cb(showbtn, show_cb, LV_EVENT_CLICKED, NULL);
  s_showbtn_lbl = lv_label_create(showbtn);
  lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
  lv_obj_set_style_text_color(s_showbtn_lbl, MUT_COL, 0);
  lv_obj_set_style_text_font(s_showbtn_lbl,
      font_for_both(tr(STR_L_SHOW), tr(STR_L_HIDE), 84, 32), 0);
  lv_obj_center(s_showbtn_lbl);

  // SCAN: a passphrase kept as a QR (some owners do). Gated behind one warning
  // screen, the same pattern as the scan-key export, because a passphrase in a
  // QR is only as private as wherever that QR lives.
  {
    lv_obj_t *sb = lv_button_create(s_login);
    lv_obj_set_style_bg_color(sb, KEY_COL, 0);
    lv_obj_set_style_shadow_width(sb, 0, 0);
    lv_obj_set_size(sb, 92, 40);
    lv_obj_set_pos(sb, 550, 18);
    lv_obj_add_event_cb(sb, pp_scan_warn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sb);
    lv_label_set_text(sl, tr(STR_L_SCAN_BTN));
    lv_obj_set_style_text_color(sl, MUT_COL, 0);
    lv_obj_set_style_text_font(sl, wt_body_font(tr(STR_L_SCAN_BTN), 84, 32), 0);
    lv_obj_center(sl);
  }

  s_entry = lv_label_create(s_login);
  lv_obj_set_style_text_font(s_entry, wt_font28(), 0);
  lv_obj_set_width(s_entry, 704);
  lv_label_set_long_mode(s_entry, LV_LABEL_LONG_SCROLL);   // long passphrases scroll
  lv_obj_set_style_text_align(s_entry, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_entry, 48, 92);

  s_count = lv_label_create(s_login);      // live length readout under the entry
  lv_label_set_text(s_count, "");
  lv_obj_set_style_text_color(s_count, MUT_COL, 0);
  lv_obj_set_style_text_font(s_count, wt_font14(), 0);
  lv_obj_set_pos(s_count, 48, 132);

  s_meter = lv_label_create(s_login);      // WEAK/FAIR/STRONG (setup mode only)
  lv_label_set_text(s_meter, "");
  lv_obj_set_style_text_font(s_meter, wt_font14(), 0);
  lv_obj_set_style_text_letter_space(s_meter, 2, 0);
  lv_obj_set_pos(s_meter, 660, 132);
  entry_refresh();

  s_kb = lv_buttonmatrix_create(s_login);
  // a fresh keyboard always starts lowercase and unlocked: inheriting a CAPS
  // lock from a previous screen would silently change what gets typed
  s_caps_lock = false; s_one_shot = false; s_shift_t0 = 0; s_hold_lock_ok = false;
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
  lv_obj_set_style_text_font(s_kb, wt_font28(), LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(s_kb, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
  // bottom row: plane switch, CANCEL, wide space, OK
  lv_obj_set_style_bg_color(s_kb, wt_accent_bg(),
                            LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(s_kb, kb_long_cb, LV_EVENT_LONG_PRESSED, NULL);
}

// ---- build identity (shared: Settings footer + wallet home corner) ----
// Honest about what this firmware is: version + commit, then the flash-
// encryption state read from the CHIP eFuse at runtime and the C6 radio
// reset pad read back from the GPIO, never assumed from the build. Bad
// states are amber WARNINGS; good states go calm. Dev builds carry a "dev"
// marker in amber (dev seed, no release hardening).
void wallet_build_id_restyle(lv_obj_t *version_label)
{
  if (!version_label) return;
#ifdef KISS_RELEASE
  bool enc = false;
#ifndef SIMULATOR
  enc = esp_efuse_is_flash_encryption_enabled();
#endif
  lv_obj_set_style_text_color(version_label, enc ? MUT_COL : wt_accent(), 0);
#else
  lv_obj_set_style_text_color(version_label, WT_WARN, 0);
#endif
}

lv_obj_t *wallet_build_id_make(lv_obj_t *parent, int x, int y)
{
  bool enc = false, radio_held = true;   // sim: no radio hardware exists
#ifndef SIMULATOR
  enc = esp_efuse_is_flash_encryption_enabled();
  radio_held = radio_is_held();
#endif
  lv_obj_t *v = lv_label_create(parent);
  lv_obj_set_style_text_font(v, wt_font14(), 0);
  lv_obj_set_pos(v, x, y);
#ifdef KISS_RELEASE
  lv_label_set_text_fmt(v, "KISS %s (%s)", KISS_VERSION_STR, KISS_COMMIT_STR);
  wallet_build_id_restyle(v);
#else
  lv_label_set_text_fmt(v, "KISS %s dev (%s)", KISS_VERSION_STR, KISS_COMMIT_STR);
  wallet_build_id_restyle(v);
#endif
  lv_obj_t *w = lv_label_create(parent);
  lv_obj_set_style_text_font(w, wt_font14(), 0);
  lv_label_set_text_fmt(w, "-  flash encryption: %s", enc ? "ENABLED" : "OFF");
  lv_obj_set_style_text_color(w, enc ? MUT_COL : lv_color_hex(0xF2B84B), 0);
  lv_obj_update_layout(v);
  lv_obj_set_pos(w, x + lv_obj_get_width(v) + 10, y);

  lv_obj_t *r = lv_label_create(parent);
  lv_obj_set_style_text_font(r, wt_font14(), 0);
  lv_label_set_text_fmt(r, "-  radio: %s", radio_held ? "held in reset" : "NOT HELD");
  lv_obj_set_style_text_color(r, radio_held ? MUT_COL : lv_color_hex(0xF2B84B), 0);
  lv_obj_update_layout(w);
  lv_obj_set_pos(r, x + lv_obj_get_width(v) + 10 + lv_obj_get_width(w) + 10, y);

  return v;
}
