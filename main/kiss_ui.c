// Step 3: the login. Spec: the passphrase keyboard is the most-used screen —
// big-key landscape QWERTY, char-flash-then-mask entry, show/hide toggle, and
// the fingerprint shown HUGE after entry (there is no "wrong passphrase" error
// by design; the fingerprint is how you recognize your wallet).
#include "kiss_ui.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "kiss_backup.h"  // the paper check, remembered past this session
#include "kiss_crypto.h"
#include "kiss_rehearse.h"
#include "kiss_scan.h"    // kiss_scan_open_raw: passphrase-from-QR
#include "kiss_duress_ui.h"  // last setup step: which stroke opens which signer
#include "kiss_seed.h"
#include "kiss_setup.h"   // optional full post-creation recovery rehearsal
#include "pass_edit.h"      // insert/delete at the caret, tested in sim/test_passedit.c
#include "kiss_info.h"    // kiss_info_fp_card_open: the "?" on the reveal screen
#include "kiss_theme.h"
#include "kiss_wipe.h"  // secret wipes survive dead-store elimination

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
static int s_caret;                        // insertion point, 0..s_plen (s_plen = at the end)
static void pop_wipe_text(void);           // defined with the key callout, below
static void login_teardown(void);          // defined with wipe_and_close, below
static lv_obj_t *s_caret_obj;              // the visible bar, child of s_entry
static bool s_show;                        // show-all toggle
static bool s_flash;                       // last char currently unmasked
static void (*s_unlocked_cb)(void);
static lv_obj_t *s_cap;                    // caption (setup mode repurposes it)
static bool s_setup_mode;                  // first login after the wizard: type twice
static bool s_pass_later;                  // "add a passphrase" from Settings: the
                                           // wizard's staged seed is not involved
static bool s_first_done;                  // first of the two entries captured
// Restoring words the owner already owns. Everything about staging and
// committing the seed is unchanged -- this ONLY turns off the two checks that
// belong to a passphrase being invented.
//
// Type-twice is a safety net against a typo in a secret nobody has ever seen.
// Re-entering an existing passphrase is the opposite problem: the same typo
// made twice passes, and opens a different, valid, EMPTY wallet under a
// fingerprint the owner has never seen either. The check that actually works
// here already runs on the next screen -- the fingerprint, against the one
// their coordinator shows.
//
// The weak-passphrase gate goes for the same reason. It asks the owner to pick
// a stronger one, and a stronger one is a DIFFERENT WALLET. Nagging about a
// passphrase that cannot be changed without abandoning the coins behind it
// teaches the owner to dismiss the warning that matters.
static bool s_restore_mode;
static bool s_weak_ack;                    // weak passphrase needs a second OK
static lv_obj_t *s_meter;                  // WEAK/FAIR/STRONG (setup only)
static lv_obj_t *s_pp_hint;                // length-coaching hint (setup only)
static char s_first[PASS_MAX + 1];
// The same keyboard, borrowed to collect a KEF backup password. Never a
// passphrase: it opens no wallet and derives no session. Create mode gets the
// meter, the weak ack and type-twice (a typo here is a backup nobody can ever
// open); open mode is a single entry checked by the caller, which answers
// with ONE vague failure — a wrong password and a corrupt envelope must read
// the same. These arms run FIRST in kb_cb, the s_backup_verify_pass shape.
static bool s_kef_mode;
static bool s_kef_create;
static bool s_kef_first_done;
static char s_kef_first[PASS_MAX + 1];
static int (*s_kef_check_cb)(const char *pass, size_t len);
static void (*s_kef_done_cb)(void);
static void (*s_kef_cancel_cb)(void);
static uint8_t s_last_fp[4];               // fingerprint of the wallet just unlocked
static uint8_t s_shown_fp[4];              // candidate shown, unpublished until OPEN
static bool s_shown_fp_valid;
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
  if ((!s_setup_mode && !s_kef_create) || s_plen == 0) {
    lv_label_set_text(s_meter, "");
    if (s_pp_hint) lv_label_set_text(s_pp_hint, "");
    return;
  }
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
  // Coach toward length while weak or fair; length is the best lever, so the
  // hint does not branch. Cleared once strong.
  if (s_pp_hint) lv_label_set_text(s_pp_hint, bits < 70 ? tr(STR_L_PP_HINT) : "");
}

static lv_obj_t *s_pp_intro;   // setup passphrase-intro screen (owns touch too)

bool kiss_ui_active(void) {
  return s_login != NULL || s_fpscr != NULL || s_errscr != NULL || s_pp_intro != NULL;
}

// The login row's secret deadline, suppressed while the RECOVER screen holds
// the staged secret. The hidden login still owns the touch, but its
// 120-second wipe would erase the passphrase the retry keeps in s_pass -- and
// TRY AGAIN would then commit an EMPTY passphrase under the stale fingerprint,
// opening a different wallet than the one the owner watched the fingerprint
// of. The recovery row carries no deadline of its own (the staged seed may be
// the last copy anywhere), so suppressing this row leaves nothing due.
bool kiss_ui_login_deadline_active(void) {
  return kiss_ui_active() && !kiss_ui_recover_active();
}

// ---- keyboard maps (three planes) ----
//
// The shift and symbol keys read the way a phone's do: one chevron for shift,
// "123" into the numbers and symbols, "#+=" for the second symbol plane, "abc"
// back to letters.
//
// NOT the literal "⇧" the design asks for. U+21E7 is outside every range in
// gen_fonts.sh's LAT list, and a codepoint a label references but the font
// lacks does not draw a box, it hard-hangs the LVGL renderer on the device.
// LV_SYMBOL_UP is FontAwesome F077, which is already subset in, so this is the
// same idea drawn with a glyph that exists.
//
// The one place this deliberately does NOT copy a phone: caps LOCK gets the
// padlock rather than a fourth shading of the same chevron. Behind the dots a
// wrong-case passphrase is invisible, and at login there is no error, just a
// different wallet, so the state that STICKS is the one that has to be
// unmistakable. One-shot shift is transient and self correcting; caps lock is
// not.
#define KEY_SHIFT LV_SYMBOL_UP
#define KEY_CAPS  WT_ICON_LOCK
#define KEY_SYM   "123"
#define KEY_SYM2  "#+="
#define KEY_ABC   "abc"

static const char *MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    KEY_SHIFT, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    KEY_SYM, "CANCEL", " ", "OK", ""};
// Two upper planes, identical keys. One-shot drops back to lowercase after a
// single character; CAPS stays until tapped again. The lower and upper planes
// now share a shift GLYPH, so the difference between them is carried by the
// highlight kb_plane asserts on that key, and by the letters themselves.
static const char *MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    KEY_SHIFT, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    KEY_SYM, "CANCEL", " ", "OK", ""};
static const char *MAP_CAPS[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    KEY_CAPS, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    KEY_SYM, "CANCEL", " ", "OK", ""};
// two symbol planes so ALL 32 ASCII punctuation chars are reachable (spec:
// passphrase = printable ASCII; an untypeable char = an unrecoverable wallet).
// This is why the letter planes say "123" and not "123" alone would do: the
// second plane needs its own name, and "#+=" is the name phones give it.
static const char *MAP_SYM[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "&", "(", ")", "?", "\n",
    KEY_SYM2, "-", "_", "=", "+", ".", ",", "/", LV_SYMBOL_BACKSPACE, "\n",
    KEY_ABC, "CANCEL", " ", "OK", ""};
static const char *MAP_SYM2[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "\"", "'", ":", ";", "[", "]", "{", "}", "*", "\n",
    KEY_SYM, "<", ">", "\\", "|", "`", "~", "^", LV_SYMBOL_BACKSPACE, "\n",
    KEY_ABC, "CANCEL", " ", "OK", ""};

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
  // Shift is id 19 (10 + 9 keys before it). Lower and upper share a glyph, so
  // the lit key is the whole difference between "the next letter is a capital"
  // and "it is not". Assert it on every plane swap rather than trusting LVGL to
  // carry per-button ctrl across a set_map.
  if (map == MAP_UPPER || map == MAP_CAPS)
    lv_buttonmatrix_set_button_ctrl(kb, 19, LV_BUTTONMATRIX_CTRL_CHECKED);
  else
    lv_buttonmatrix_clear_button_ctrl(kb, 19, LV_BUTTONMATRIX_CTRL_CHECKED);
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

// File scope, not function scope, so the simulator can drop it again -- see
// kiss_ui_drop_indev_for_test below.
static lv_indev_t *s_indev;

static void ensure_indev(void) {
  if (s_indev) return;
  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, indev_read);
}

#ifdef SIMULATOR
// Test seam, simulator only. This indev is created lazily, and for a long time
// the only creators were the login screen and the setup wizard -- every way
// into the wallet went through one of them, so nobody noticed the dependency.
// Then the decoy started opening the session directly and arrived with no
// indev: the wallet home drew, the game still worked (it reads the touch
// controller itself), and every LVGL sub-screen was deaf. Settings looked
// frozen. It shipped, because the scripted walk always ran a full setup first
// and so was never cold when it opened the decoy.
//
// The walk cannot get cold on its own -- creating a seed requires the wizard,
// which creates the indev -- so it needs a way to put the process back into the
// state a real board is in at power-on. That is all this does.
void kiss_ui_drop_indev_for_test(void) {
  if (!s_indev) return;
  lv_indev_delete(s_indev);
  s_indev = NULL;
}
#endif

// long-passphrase fitting: past ~78 chars show "..." + the tail (the newest
// chars are what the user is checking). Never scroll-animate a masked secret.
//
// The SIZE is the ladder's, not a ternary's. This read `chars > 40 ? font14 :
// font28` -- the 28 to 14 fall with 23 skipped, which is the exact shape
// wt_body_font was written to stop, on the one screen where an owner checks a
// value character by character. The slot is 704x40 and one font23 line is 29,
// so every secret between 41 and about 62 glyphs was dropping two rungs for
// nothing.
static void entry_apply(const char *txt, int chars) {
  (void)chars;
  // _typed: this is the OWNER'S secret, not the product's copy, so the FIT
  // gate is not told when a 90 character passphrase lands on font14. There is
  // nothing here anybody can shorten.
  lv_obj_set_style_text_font(s_entry,
                             wt_body_font_typed(txt ? txt : "", 704, 40), 0);
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
    kiss_wipe(tail, sizeof tail);   // LVGL copied it; the static buffer must not keep it
  } else {
    lv_label_set_text(s_entry, txt);
  }
}

// ---- the caret: tap a character while SHOW is on, edit there ----
// Shown only with SHOW on. Behind the dots there is nothing to aim at, and a
// passphrase edited in the wrong place is one nobody can reproduce.
static void caret_refresh(void) {
  if (!s_caret_obj || !s_entry) return;
  if (!s_show || s_plen == 0) {
    lv_obj_add_flag(s_caret_obj, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_update_layout(s_entry);      // letter_pos reads the settled line breaks
  const lv_font_t *f = lv_obj_get_style_text_font(s_entry, LV_PART_MAIN);
  lv_point_t p;
  lv_label_get_letter_pos(s_entry, (uint32_t)s_caret, &p);
  lv_obj_set_size(s_caret_obj, 2, lv_font_get_line_height(f));
  lv_obj_set_style_bg_color(s_caret_obj, wt_accent(), 0);
  lv_obj_set_pos(s_caret_obj, p.x, p.y);
  lv_obj_clear_flag(s_caret_obj, LV_OBJ_FLAG_HIDDEN);
}

// ---- entry display: dots, optional flash of the newest char, show-all ----
// The label's text lives in LVGL's heap, copied there by lv_label_set_text,
// and while SHOW is on that copy IS the passphrase. Wipe the previous render
// before replacing it, on every re-render; the guard skips the label's
// initial static-empty text, which is not ours to write.
static void entry_wipe_text(void) {
  if (!s_entry) return;
  const char *t = lv_label_get_text(s_entry);
  if (t && *t) kiss_wipe((void *)t, strlen(t) + 1);
}

static void entry_refresh_text(void) {
  static char buf[PASS_MAX * 3 + 8];
  entry_wipe_text();
  meter_refresh();
  if (s_count) {
    int sp = 0;
    for (int i = 0; i < s_plen; i++) if (s_pass[i] == ' ') sp++;
    if (s_plen == 0) lv_label_set_text(s_count, "");
    else if (sp)     // spaces are the classic invisible typo, so call them out
      lv_label_set_text_fmt(s_count, tr(STR_L_COUNT_SP_FMT), s_plen, sp);
    else lv_label_set_text_fmt(s_count, tr(STR_L_COUNT_FMT), s_plen);
  }
  if (s_plen == 0) {
    lv_obj_set_style_text_font(s_entry, wt_font28(), 0);
    // The ghost prompt names what is being typed, and in KEF mode that is a
    // backup password, never a passphrase (vocabulary is load bearing here:
    // a passphrase opens a wallet, this opens an envelope).
    lv_label_set_text(s_entry, tr(s_kef_mode ? STR_L_KEF_TYPE_PROMPT
                                             : STR_L_TYPE_PROMPT));
    lv_obj_set_style_text_color(s_entry, MUT_COL, 0);
    kiss_wipe(buf, sizeof buf);
    return;
  }
  lv_obj_set_style_text_color(s_entry, wt_accent(), 0);
  if (s_show) {
    entry_apply(s_pass, s_plen);
    kiss_wipe(buf, sizeof buf);
    return;
  }
  // Dots, with the character just entered left bare for FLASH_MS. That
  // character is the one BEFORE the caret, which is the end of the passphrase
  // in the ordinary case and the middle of it after a tap.
  int n = 0;
  int flash_at = s_flash ? s_caret - 1 : -1;
  for (int i = 0; i < s_plen && n < (int)sizeof(buf) - 4; i++) {
    if (i == flash_at) { buf[n++] = s_pass[i]; continue; }
    buf[n++] = '\xE2'; buf[n++] = '\x80'; buf[n++] = '\xA2';  // U+2022 bullet
  }
  buf[n] = 0;
  entry_apply(buf, s_plen);
  kiss_wipe(buf, sizeof buf);
}

// Text first, then the caret. The caret is placed by asking the label where a
// character sits, so it can only be positioned once the text it indexes into
// is the text actually on screen.
static void entry_refresh(void) {
  if (s_caret > s_plen || s_caret < 0) s_caret = s_plen;   // never index off the end
  entry_refresh_text();
  caret_refresh();
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
  // The ladder on EVERY path. This branched on `alert`, which is a colour
  // decision, not a question about whether the string is an eyebrow -- and two
  // of the strings coming through the quiet branch are instructions: "CREATE
  // YOUR PASSPHRASE" and "CREATE A BACKUP PASSWORD", both landing at font14 in
  // a 64px band.
  lv_obj_set_style_text_font(s_cap, wt_body_font(txt, CAP_W, 58), 0);
  lv_obj_set_pos(s_cap, 48, alert ? 22 : 28);
}

// after a weak-ack warning, any edit returns the caption to the stage prompt
static void setup_cap_reset(void) {
  if ((!s_setup_mode && !s_kef_create) || !s_weak_ack || !s_cap) return;
  s_weak_ack = false;
  if (s_kef_mode) {
    if (s_kef_first_done)
      cap_set(tr(STR_L_TYPE_AGAIN), lv_color_hex(0xF2B84B), true);
    else
      cap_set(tr(STR_L_KEF_PASS_NEW), MUT_COL, false);
    return;
  }
  if (s_first_done) cap_set(tr(STR_L_TYPE_AGAIN), lv_color_hex(0xF2B84B), true);
  else              cap_set(tr(STR_L_CREATE_YOUR_PASS), MUT_COL, false);
}

static lv_obj_t *s_cancel_ovl;             // "cancel setup?" confirm (setup mode only)
static lv_obj_t *s_weak_ovl;               // weak-passphrase deliberate-use card
static lv_obj_t *s_warnscr;                // post-setup passphrase warning (one screen)
static bool s_backup_verified;             // every word + exact passphrase rehearsed
static bool s_backup_verify_pass;          // keyboard is checking that passphrase now

// The typed passphrase, cleared through kiss_wipe: an ordinary memset can be
// optimized away the moment the compiler sees the buffer is dead, which is
// exactly the shape every wipe of a secret is in.
static void wipe_login_secrets(void) {
  kiss_wipe(s_pass, sizeof s_pass);
  kiss_wipe(s_first, sizeof s_first);
  s_plen = 0;
  s_caret = 0;
}

static void wipe_and_close(void) {
  if (s_cancel_ovl) { lv_obj_delete_async(s_cancel_ovl); s_cancel_ovl = NULL; }
  if (s_weak_ovl) { lv_obj_delete_async(s_weak_ovl); s_weak_ovl = NULL; }
  if (s_warnscr) { lv_obj_delete_async(s_warnscr); s_warnscr = NULL; }
  // cancelling setup before the fingerprint confirm drops the staged seed, so
  // an abandoned setup never leaves a half-made wallet in flash (P0 safety net)
  if (s_setup_mode) kiss_seed_discard();
  wipe_login_secrets();
  s_setup_mode = false;
  s_restore_mode = false;
  s_pass_later = false;
  s_first_done = false;
  s_weak_ack = false;
  s_backup_verified = false;
  s_backup_verify_pass = false;
  s_kef_mode = false;
  s_kef_create = false;
  s_kef_first_done = false;
  kiss_wipe(s_kef_first, sizeof s_kef_first);
  s_kef_check_cb = NULL;
  s_kef_done_cb = NULL;
  s_kef_cancel_cb = NULL;
  s_plen = 0;
  s_caret = 0;
  s_show = false;
  s_flash = false;
  login_teardown();
}

// A rendered secret is a copy of the passphrase in LVGL's own heap: the entry
// label's current text (the full passphrase while SHOW is on) and the key
// callout's single character. The buffers outlive the wipe of s_pass — they
// are freed only with the screen — so scrub them wherever the login dies
// (login_teardown) or is left standing while the flow moves on (the success
// path of fp_tap_cb).
static void login_scrub_rendered(void) {
  entry_wipe_text();
  pop_wipe_text();
}

// The login screen dies. Scrub every rendered copy of the secret first, kill
// the timers that would call back into freed widgets, then delete the screen
// and null EVERY pointer into it. A half-nulled set leaves the rest dangling
// for the next wipe or callback: recover_screen used to null only s_login,
// and the next teardown then dereferenced the freed entry label.
static void login_teardown(void) {
  login_scrub_rendered();
  if (s_mask_tmr) { lv_timer_delete(s_mask_tmr); s_mask_tmr = NULL; }
  if (s_pop_tmr) { lv_timer_delete(s_pop_tmr); s_pop_tmr = NULL; }
  s_pop = NULL; s_pop_lbl = NULL; s_kflash = NULL; s_kb = NULL;
  s_entry = NULL; s_count = NULL; s_showbtn_lbl = NULL; s_cap = NULL;
  s_caret_obj = NULL; s_meter = NULL; s_pp_hint = NULL;
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
  kiss_wipe(s_pass, sizeof s_pass);
  s_plen = 0;
  s_caret = 0;
  s_show = false;
  if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
  cap_set(tr(STR_L_TYPE_AGAIN), lv_color_hex(0xF2B84B), true);
  entry_refresh();
}

// The KEF twin of setup_accept_first: same transition, its OWN capture
// buffer, so a backup password can never bleed into the setup flow's s_first.
static void kef_accept_first(void) {
  memcpy(s_kef_first, s_pass, sizeof s_kef_first);
  s_kef_first_done = true;
  s_weak_ack = false;
  kiss_wipe(s_pass, sizeof s_pass);
  s_plen = 0;
  s_caret = 0;
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

// A weak passphrase is not a choice the device offers. This card used to say
// so and then put USE ANYWAY under it, selected, at the one moment in the
// product where the owner is most impatient to get past a screen -- so the
// sentence above the pill was advice and the pill was the answer.
//
// It refuses now. Same argument as the dice and the blind draw: this is a
// secret being MADE, and making a longer one costs seconds. The only thing on
// the far side of the warning was a guessable passphrase guarding real funds.
//
// BACK is the whole card, and it keeps the entry, so the way out is to add
// characters to what is already typed rather than start again. What this can
// never become is a judgement at LOGIN: every passphrase there is valid and
// opens some wallet, so a device that refused one would be refusing a wallet
// (see pass_bits). Restoring is exempt for the same reason -- those words and
// that passphrase already exist.
//
// It is not the warning for an EMPTY passphrase either. That is a legitimate
// choice with its own screens, and it has its own action (pp_intro_nopass_cb).
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

  // A KEF password and a passphrase are different words for a reason: the
  // card must name the thing being weak, and for a backup the threat is
  // unlimited offline guessing rather than "another wallet".
  lv_obj_t *t = wt_lbl(card, tr(s_kef_mode ? STR_L_KEF_WEAK_T : STR_L_WEAK_T),
                       0, 0, wt_font28(), INK_COL);
  lv_obj_set_style_text_letter_space(t, 2, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 88);

  lv_obj_t *b = wt_note(card, tr(s_kef_mode ? STR_L_KEF_WEAK_ACK
                                            : STR_L_WEAK_ACK),
                        32, 144, 640, 124);
  lv_obj_set_style_text_color(b, MUT_COL, 0);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);

  // One exit, centred on the card's own 704 rather than left where it sat
  // beside the one that is gone. The arrow action sizes to its word, so the
  // centring is an align rather than a measured x.
  lv_obj_t *back = wt_arrow_action(card, tr(STR_C_BACK), true, false,
                                   195, 288, 314, false, weak_back_cb, NULL);
  lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 288);
}

// ---- fingerprint reveal ----
static void setup_fail_screen(void);
static void recover_screen(void);
static void fp_tap_cb(lv_event_t *e);

// The commit that could not finish, and the only screen on this device whose
// job is to keep an owner from throwing something away.
//
// It is reached when a KEEP wallet was replacing another and the replacement
// may have taken the old one with it -- the residue scrub erased and could not
// write back, or the new blob committed over the old one and then failed its
// readback. Either way flash holds nothing usable and the staged copy in RAM is
// the last one in existence.
//
// The generic setup STOP used to be shown here, and its body reads "nothing was
// saved. start again from the menu." Every word of that is wrong: something was
// most certainly lost, and starting again is the one action that destroys what
// is left. This screen says the opposite, and gives the two things that are
// actually worth doing -- read the words onto paper, and try the write again.
static lv_obj_t *s_recovscr;
static bool s_recover_retry_queued;

static void recover_close(void)
{
    if (s_recovscr) { lv_obj_delete_async(s_recovscr); s_recovscr = NULL; }
}

// The words screen opened from here reads the same staged copy, so the lock
// exemption has to travel with it: s_recovscr is already gone while it is up,
// and without the flag the idle lock's registered kiss_info close would
// delete the one screen naming words that exist nowhere else.
static bool s_words_from_recov;

// The registry row in main.c's SCREENS[] for this state. It owns the touch --
// the game must not read a corner tap or a tile band through the screen --
// and it holds the idle clock off, for the reason the wizard rows give:
// reading twelve words onto paper takes minutes of a screen nobody touches.
// Its close stays NULL there, deliberately. The staged copy in RAM may be the
// last one anywhere, and s_setup_mode surviving is what keeps the type-twice
// login able to commit it; a teardown routed through wipe_and_close would
// call kiss_seed_discard and destroy the wallet it exists to save.
bool kiss_ui_recover_active(void)
{
    return s_recovscr != NULL || s_words_from_recov;
}

// Straight back to the same commit and the same continuation. The passphrase is
// still in s_pass -- fp_tap_cb returns on RECOVER before the wipe below it, so a
// retry derives the identical session and finishes setup exactly as a first
// attempt would have. A scrub or readback that failed once on a transient flash
// error is worth one more press before an owner is sent to their paper.
// Deferred, for the reason the duress and word wizards defer their stages:
// never build a screen inside the event callback that asked for it. Re-entering
// fp_tap_cb synchronously from the pill's own CLICKED handler wedged the walk
// outright -- the retry runs the whole commit and builds the next screen while
// LVGL is still dispatching the press that started it.
static void recover_retry_async(void *ud)
{
    (void)ud;
    s_recover_retry_queued = false;

    // The retry must open the wallet whose fingerprint the owner already saw.
    // Recovery retains s_pass for exactly that, but the screen has its own
    // deadline now (main.c's registry) and an untouched five minutes wipes
    // the passphrase while leaving the words: continuing then would open the
    // empty-passphrase wallet under a stale fingerprint. Never that.
    uint8_t fp[4] = {0};
    bool same = s_shown_fp_valid &&
                kiss_fingerprint(s_plen ? s_pass : NULL, fp) == 0 &&
                memcmp(fp, s_shown_fp, sizeof fp) == 0;
    kiss_wipe(fp, sizeof fp);
    if (!same) {
        // Ask for it again, on the keyboard RECOVER hid rather than deleted.
        // The wipe already put the flow back to its first stage, so the
        // wizard's own type-twice runs from scratch and lands on the same
        // fingerprint screen the commit failed from. Refusing silently was
        // safe and unusable: a button that does nothing, on the screen
        // holding the only copy of a wallet.
        if (!s_login) return;      // nothing to ask with: RECOVER keeps SHOW WORDS
        recover_close();
        lv_obj_remove_flag(s_login, LV_OBJ_FLAG_HIDDEN);
        if (s_entry) { entry_refresh_text(); caret_refresh(); }
        return;
    }

    recover_close();
    fp_tap_cb(NULL);
}

static void recover_retry_cb(lv_event_t *e)
{
    (void)e;
    if (s_recover_retry_queued) return;
    // lv_async_call allocates both a record and a timer. If either allocation
    // fails, leave RECOVER visible: deleting the last-copy screen before the
    // retry is actually queued strands the only safe actions it offers.
    if (lv_async_call(recover_retry_async, NULL) == LV_RESULT_OK)
      s_recover_retry_queued = true;
}

static void recover_words_done(void)
{
    s_words_from_recov = false;
    recover_screen();
}

static void recover_words_cb(lv_event_t *e)
{
    (void)e;
    recover_close();
    s_words_from_recov = true;
    // kiss_seed_load answers from the staging while it is held, so this is the
    // ordinary words screen reading the copy that has not reached flash.
    kiss_info_open_words(lv_screen_active(), recover_words_done);
}

#ifndef ESP_PLATFORM
// Opened directly by the walk, like the duress and firmware leaves. Driving it
// through a real failed commit mid-setup wedged the walk: setup does not
// complete, so every tap after it lands on the wrong screen.
void kiss_ui_test_recover_screen(void);
#endif

static void recover_screen(void)
{
#ifdef SIMULATOR
#endif
    // The login is HIDDEN, not deleted. The retry re-derives the same
    // session, and the setup flow after it (the optional rehearsal) reopens
    // the keyboard from the warn screen: setup_warn_words_done un-hides
    // s_login. A deleted login left that step with nothing to type into, and
    // its stale pointers were what the retry's later teardown dereferenced.
    // The rendered copies of the passphrase are scrubbed here (the label
    // heap holds the SHOW text); s_pass itself stays for the retry. The
    // mask timer dies with the scrub: a live flash timer would re-render
    // the retained passphrase into the hidden label on its next tick,
    // repainting the exact text the scrub just erased. s_flash resets so
    // the entry comes back fully masked.
    if (s_login) {
      login_scrub_rendered();
      if (s_mask_tmr) { lv_timer_delete(s_mask_tmr); s_mask_tmr = NULL; }
      s_flash = false;
      lv_obj_add_flag(s_login, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_fpscr) { lv_obj_delete_async(s_fpscr); s_fpscr = NULL; }
    recover_close();

    s_recovscr = wt_screen(lv_screen_active(), tr(STR_L_RECOVER_T), NULL);

    // The subject, framed, above the actions: what the device is holding and
    // where it is. WT_ICON_SECRET is the words' own mark everywhere else.
    {
        lv_obj_t *col = lv_obj_create(s_recovscr);
        lv_obj_remove_style_all(col);
        lv_obj_set_pos(col, 48, 112);
        lv_obj_set_width(col, 704);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *row = wt_diagram_row(col);
        char b[WT_ICON_TEXT_MAX];
        wt_icon_text(b, sizeof b, WT_ICON_SECRET, tr(STR_D_WORDS));
        wt_chip(row, b, true);
        wt_diagram_op(row, LV_SYMBOL_RIGHT);
        wt_chip(row, tr(STR_GD_OFF), false);   // NOT SET: nowhere on this device
    }
    wt_why_body(s_recovscr, tr(STR_L_RECOVER_B), 190, WT_WARN, true);

    // SHOW WORDS opens the reveal; TRY AGAIN returns to the login above, so
    // it is the escape and takes the corner, wearing the accent as the way
    // the product steers.
    wt_arrow_action(s_recovscr, tr(STR_I_SHOW_WORDS), false, false, WT_ACT_X,
                    WT_ACTION_Y, 300, false, recover_words_cb, NULL);
    wt_arrow_action(s_recovscr, tr(STR_C_TRY_AGAIN), true, true, WT_EXIT_X,
                    WT_ACTION_Y, 140, true, recover_retry_cb, NULL);
}
static void setup_warn_screen(void);

// dismiss the STOP screen back to the game; nothing was saved
static void setup_fail_dismiss_cb(lv_event_t *e) {
  (void)e;
  s_caps_lock = false; s_one_shot = false; s_shift_t0 = 0; s_hold_lock_ok = false;
  s_setup_mode = false; s_restore_mode = false; s_first_done = false; s_weak_ack = false;
  s_pass_later = false;      // a failed add-later run is over; the next
                             // Settings entry re-arms it
  wipe_login_secrets();
  s_show = false; s_flash = false;
  if (s_errscr) { lv_obj_delete_async(s_errscr); s_errscr = NULL; }
}

// Setup couldn't be saved (staged seed failed to commit, or the session didn't
// derive): show a clear STOP instead of silently entering a broken home.
static void setup_fail_screen(void) {
  login_teardown();

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
  // Fingerprint derivation already proved the staged words + passphrase. Keep
  // the old unlocked session completely untouched until the replacement is
  // durable; otherwise BACK or a failed commit can leave the old home showing
  // while Receive/Sign secretly use the candidate wallet.
  // Three outcomes, not two. "Nonzero is failure, discard the staging" was one
  // rule covering results that mean opposite things.
  //
  //   CLEANUP  the destination is written, read back and byte-compared: this
  //            wallet IS durable and only an old artifact survived. Reporting
  //            it as a failed setup told the owner their wallet did not exist
  //            while it sat on the flash they were about to walk away from.
  //   RECOVER  the partition was erased and the write-back failed. Flash may
  //            hold nothing, so the staged RAM copy is the last one there is
  //            and discarding it IS the data loss, not the report of it.
  //            setup_fail_dismiss_cb does not discard, so it survives the STOP.
  //   others   the previous state is intact and staging is safe to drop.
  // A setup-mode commit publishes the STAGED seed. An add-later run has no
  // staged seed -- the signer's wallet is what it already was; the passphrase
  // derives the session, exactly like an everyday login -- and committing
  // nothing returns WSEED_ERR_NO_SEED, which is not a failure here.
  if (s_setup_mode && !s_pass_later) {
    int crc = kiss_seed_commit();
    if (crc == WSEED_ERR_RECOVER) {
      // Its own screen, not the generic STOP. The staging is kept, and the
      // owner is told to read it onto paper rather than to start again.
      recover_screen();
      return;
    }
    if (crc != WSEED_OK && crc != WSEED_ERR_CLEANUP) {
      kiss_seed_discard();
      wipe_login_secrets();
      setup_fail_screen();
      return;
    }
  }
  if (kiss_session_open(s_plen ? s_pass : NULL) != 0) {
    // A successful setup commit was verified before this second derivation.
    // If the swap still fails, never publish the candidate fingerprint or
    // pretend that an unlocked session exists.
    if (s_setup_mode) kiss_session_close();
    // The typed passphrase dies HERE, not on the failure screen's dismissal:
    // that screen only inherits the login's two-minute secret-idle wipe, so a
    // device set down on it would still sit on plaintext for up to 120 s.
    wipe_login_secrets();
    setup_fail_screen();
    return;
  }
  if (s_shown_fp_valid) {
    memcpy(s_last_fp, s_shown_fp, sizeof s_last_fp);
  }
  // The passphrase dies HERE, which is what this comment always claimed and
  // what only one of the two branches below actually did. Setup returns to a
  // warning screen first, and every wipe was on the far side of it: OK reached
  // wipe_and_close, VERIFY reached setup_warn_words_done, and an owner who set
  // the device down on that screen reached neither. Setup is also exempt from
  // the idle auto-lock -- deliberately, because writing words down takes
  // minutes -- so the plaintext sat in RAM with nothing coming to clear it.
  //
  // Nothing past this point reads it. The derived session is what Receive and
  // Sign use, and both exits below already wiped exactly this set, so doing it
  // here changes when rather than what. Returning to the keyboard re-types it,
  // which is what setup_warn_words_done was already relying on.
  wipe_login_secrets();
  // The SHOW text is a second copy of the passphrase in the entry label's
  // heap, and the warn screen is on top of a login that stays alive until
  // wipe_and_close: scrub it here, where the buffer dies, not when the screen
  // finally does.
  login_scrub_rendered();
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

// The idle deadline expired with a secret on the glass. Expire the SECRET and
// nothing else: the flow stays where it was, at its entry stage, so the owner
// who walked away comes back to a keyboard asking again -- not to a decision
// half made. Deliberately NOT wipe_and_close: that path discards the staged
// seed under s_setup_mode, and on the commit-failed branch the staging is the
// last copy of the wallet anywhere. This wipes buffers a re-type can replace.
void kiss_ui_idle_wipe(void) {
  if (!kiss_ui_active()) return;
  // The fingerprint screen is derived from the passphrase being wiped: TAP TO
  // OPEN there commits with `s_plen ? s_pass : NULL`, so leaving it standing
  // would commit the staged seed under an EMPTY passphrase beneath a stale
  // fingerprint. BACK's own semantics, minus the kept passphrase.
  if (s_fpscr) {
    lv_obj_delete_async(s_fpscr); s_fpscr = NULL;
    if (s_login) lv_obj_clear_flag(s_login, LV_OBJ_FLAG_HIDDEN);
  }
  wipe_login_secrets();
  s_first_done = false;      // stage 2 described an entry that no longer exists
  s_show = false;
  s_flash = false;
  // The toggle keeps its own label, and the wipe turns SHOW off underneath
  // it: the button read HIDE over a field that was already masked and empty,
  // so the first press unmasked instead of masking.
  if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
  // A weak-passphrase card is a question about the entry just wiped; the
  // cancel confirm is a question about the SETUP and survives on purpose.
  if (s_weak_ovl) { lv_obj_delete_async(s_weak_ovl); s_weak_ovl = NULL; }
  s_weak_ack = false;
  // These flags are the FLOW and stay. The caption walks back to the stage the
  // wipe returned the owner to: a backup rehearsal still asks for the exact
  // backup passphrase, a restore still asks for the existing passphrase, and
  // only a newly made wallet asks the owner to create one.
  if (s_login && s_cap && s_setup_mode) {
    const int cap = s_backup_verify_pass ? STR_L_VERIFY_PASS
                  : s_restore_mode       ? STR_L_PASSPHRASE_CAP
                                         : STR_L_CREATE_YOUR_PASS;
    cap_set(tr(cap), s_backup_verify_pass ? lv_color_hex(0xF2B84B) : MUT_COL,
            s_backup_verify_pass);
  }
  if (s_login && s_entry) { entry_refresh_text(); caret_refresh(); }
  pop_wipe_text();   // the key callout may hold the last typed char, hidden or not
}

void kiss_ui_last_fp(uint8_t out[4]) { memcpy(out, s_last_fp, 4); }

// "rehearsed during THIS setup", which is the question the setup warning screen
// asks: it is deciding whether to let the owner walk away, so a check done on a
// previous boot is not an answer.
bool kiss_ui_backup_verified(void) { return s_backup_verified; }

// "has this wallet's paper ever been proven against this device", which is the
// question SETTINGS asks. Same fact, longer memory. Kept apart from the flag
// above on purpose -- merging them would let a stored answer excuse the owner
// from the rehearsal they are standing in front of.
bool kiss_ui_backup_checked(void)
{
  return s_backup_verified || kiss_backup_checked(s_last_fp);
}

// The decoy signer opens straight from the game with no login screen at all,
// so nothing here runs to record its fingerprint. main.c sets it directly
// rather than duplicating the home-chip logic on that path.
void kiss_ui_set_last_fp(const uint8_t fp[4])
{
  memcpy(s_last_fp, fp, 4);
}

// Locking must forget WHICH KEYS were open, not only the key material itself.
// s_last_fp is the fingerprint of the keys last unlocked, and it survived
// kiss_session_close: nothing ever cleared it. Two ways that showed:
//
//   * The decoy opens with no login screen, so main.c sets the fingerprint
//     directly from the session it just opened. If that derivation failed it
//     set nothing, and the decoy then rendered the fingerprint of the REAL
//     keys from the previous session -- on the one screen whose whole purpose
//     is that it must not admit those keys exist.
//   * A second derivation can fail after a session opened cleanly, and the
//     fallback was whatever was left here.
//
// Zero is the "no keys open" value; the chip and the backup lookup both key
// off a real fingerprint, so a zeroed one shows nothing rather than the wrong
// thing.
void kiss_ui_forget_fp(void)
{
  kiss_wipe(s_last_fp, sizeof s_last_fp);
}

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
  const bool nopass_setup = kiss_session_decoy();   // see setup_warn_screen
  const bool restored_setup = s_restore_mode;       // wipe_and_close clears it
  wipe_and_close();                        // also deletes s_warnscr
  // A wallet with no passphrase IS the decoy: kiss_session_open(NULL) is what
  // both ways in reach. Offering to configure a "real" stroke here would let
  // someone believe their funds sit behind it and hand over the spare having
  // hidden nothing -- the feature failing in exactly the situation it exists
  // for. So do not offer it; the warning screen above says why.
  if (nopass_setup) {
    if (cb) cb();
    return;
  }
  // RESTORING ends here too, and this is the "incomplete flow" the owner hit:
  // loading words they already had walked them straight on into decoy setup,
  // so getting back into an existing wallet meant sitting through a wizard
  // about a second one. Someone restoring has a wallet already and a reason to
  // be in a hurry; the spare is a decision for later, and Settings > Duress is
  // where it is made -- that page now offers the drawing as well as the swipe.
  //
  // A NEW seed keeps the wizard. That is the one moment the two-ways-in idea
  // has to be taught, because nothing else in the product will bring it up.
  if (restored_setup) {
    if (cb) cb();
    return;
  }
  s_after_duress = cb;
  kiss_duress_ui_open(lv_screen_active(), duress_done_cb);
}

static void setup_warn_words_done(void)
{
  if (!kiss_setup_verify_succeeded()) {
    setup_warn_screen();                    // optional check cancelled or did not match
    return;
  }

  // A wallet with NO passphrase has nothing to type, and this asked for it
  // anyway: "TYPE THE EXACT BACKUP PASSPHRASE" on a seed that has none, where
  // the only accepted answer is an empty field and the caption never says so.
  // Every real attempt was rejected, and cancelling left the session's
  // fingerprint zeroed, which is where an owner's 00000000 came from. For
  // these keys the words ARE the whole backup, so matching them finishes the
  // rehearsal. Same flag setup_warn_screen reads below, for the same reason.
  if (kiss_rehearse_after_words(kiss_session_decoy()) == KISS_REHEARSE_VERIFIED) {
    s_backup_verified = true;
    kiss_backup_mark(s_last_fp);
    wipe_login_secrets();
    setup_warn_screen();
    return;
  }

  // The words matched. Now throw away the passphrase that created the session
  // and require it fresh: comparing the resulting fingerprint proves the exact
  // words + exact passphrase combination without ever storing that passphrase.
  s_backup_verify_pass = true;
  wipe_login_secrets();
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
  kiss_setup_open_verify(lv_screen_active(), setup_warn_words_done);
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
  const bool warn_nopass = kiss_session_decoy();
  const int warn_t = warn_nopass ? STR_L_WARN_T_NOPASS : STR_L_WARN_T;
  const int warn_b = warn_nopass ? STR_L_WARN_B_NOPASS : STR_L_WARN_B;

  lv_obj_t *t = lv_label_create(s_warnscr);
  lv_label_set_text(t, tr(warn_t));
  lv_obj_set_style_text_color(t, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_text_font(t, wt_font28(), 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

  // Never print a zeroed fingerprint. Zero is kiss_ui_forget_fp's "no keys
  // open" value and it also survives a derivation that failed, so an owner was
  // shown 00000000 in a value card captioned FINGERPRINT -- a code that looks
  // real, is not, and would be copied onto paper. kiss_setup.c's BACKUP
  // VERIFIED screen already guards this; this screen did not.
  const bool fp_known = kiss_fp_known(s_last_fp);

  // The claims as ruled blocks, not a wall. The no-passphrase branch was the
  // last BARE screen the overlap gate knew: a 740px centred paragraph with
  // nothing framed on the page (its only company is a state chip, which is
  // not a frame). Both bodies were already written in paragraphs, so the
  // blocks split on the blank lines the copy has -- zero new strings in any
  // locale. Lone newlines inside a paragraph were line breaks for the old
  // 740px label; they become spaces so each block wraps to its own width.
  //
  // The trailing "know ... by ... fingerprint:" paragraph is the value card's
  // own intro: it renders as one line above the card and drops with it when
  // no fingerprint derived -- the dangling-colon fault, handled structurally
  // this time instead of by cutting at the last blank line.
  char wb[512];
  snprintf(wb, sizeof wb, "%s", tr(warn_b));
  size_t wlen = strlen(wb);            // before the splits punch NULs into it
  char *para[4] = { wb, NULL, NULL, NULL };
  int np = 1;
  for (char *q = wb; (q = strstr(q, "\n\n")) != NULL && np < 4; ) {
    *q = 0;
    q += 2;
    para[np++] = q;
  }
  for (size_t i = 0; i < wlen; i++)
    if (wb[i] == '\n') wb[i] = ' ';

  const char *fp_intro = np >= 2 ? para[np - 1] : NULL;
  int nclaims = np >= 2 ? np - 1 : np;

  if (nclaims >= 2) {
    // The proven pair: what these keys are on the accent rule, where they go
    // wrong on the amber one. 160px keeps both clear of the card row at 300
    // with the intro line between.
    const lv_font_t *f = wt_body_font2(para[0], para[1], 330, 160);
    wt_why_block(s_warnscr, NULL, para[0], 48, 92, 344, 160, f, wt_accent());
    wt_why_block(s_warnscr, NULL, para[1], 408, 92, 344, 160, f, WT_WARN);
  } else {
    wt_why_block(s_warnscr, NULL, para[0], 48, 92, 704, 160,
                 wt_body_font(para[0], 690, 160), WT_WARN);
  }

  if (fp_known && fp_intro) {
    lv_obj_t *n = lv_label_create(s_warnscr);
    lv_label_set_text(n, fp_intro);
    lv_obj_set_style_text_color(n, MUT_COL, 0);
    lv_obj_set_style_text_font(n, wt_font23(), 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 264);
  }

  // The fingerprint in a value card, and the backup state as a real chip beside
  // it. Both were bare centred labels: on the screen that teaches an owner what
  // they just made, the one figure they have to copy onto paper read as a line
  // of prose rather than as the value the screen is about. The card is what the
  // design review draws around every figure meant to be read off the glass, and
  // it is the same object the Receive and WALLET pages now use, so the three
  // screens present a fingerprint identically.
  //
  // Centred as a pair: the card sizes itself to its caption in whatever locale
  // is rendering, so its x follows the measured width rather than a constant.
  char fpbuf[16];
  snprintf(fpbuf, sizeof fpbuf, "%02X%02X%02X%02X",
           s_last_fp[0], s_last_fp[1], s_last_fp[2], s_last_fp[3]);
  // Card and chip share a ROW rather than stacking. Stacked, the pair ran from
  // 288 to about 414, and the tall action row starts at 398: the state a holder
  // most needs to see would have been the half under the bar. Side by side the
  // pair is one card tall, which the band from the body's floor to 398 can hold
  // in every locale.
  lv_obj_t *card = fp_known
      ? wt_value_card(s_warnscr, tr(STR_D_FINGERPRINT), fpbuf, 110, 300, 300, true)
      : NULL;
  lv_obj_t *state = wt_state_chip(s_warnscr,
                                  s_backup_verified ? tr(STR_L_BACKUP_VERIFIED)
                                                    : tr(STR_L_BACKUP_UNVERIFIED),
                                  s_backup_verified ? WT_OK : WT_STOP);
  lv_obj_update_layout(state);
  if (card) {
    lv_obj_update_layout(card);
    lv_obj_set_pos(state, 440,
                   300 + (lv_obj_get_height(card) - lv_obj_get_height(state)) / 2);
  } else {
    // Alone, the chip takes the card's lane instead of sitting where a card
    // used to be beside it.
    lv_obj_set_pos(state, 110, 300);
  }

  // The status colour that used to be a ring around each pill goes on the
  // words themselves now: VERIFY wears the caution until the check has been
  // run, and I UNDERSTAND's tick answers in green or red for whether skipping
  // is walking past a verified backup or an unchecked one.
  lv_obj_t *verify = wt_word_action(s_warnscr, WT_ICON_ARR_R,
                                    tr(STR_L_VERIFY_FULL_BACKUP), false,
                                    s_backup_verified ? WT_INK : WT_WARN,
                                    false, setup_warn_verify_cb, NULL);
  lv_obj_set_pos(verify, 48, WT_ACTION_Y_TALL + 13);

  // Skipping is allowed, but it must look like a conscious decision.
  lv_obj_t *ok = wt_word_action(s_warnscr, LV_SYMBOL_OK,
                                tr(STR_C_I_UNDERSTAND), true,
                                s_backup_verified ? WT_OK : WT_STOP,
                                false, setup_warn_ok_cb, NULL);
  // The tick alone carries the verdict; the word is a word.
  lv_obj_set_style_text_color(lv_obj_get_child(ok, 1), WT_INK, 0);
  lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -48, WT_ACTION_Y_TALL + 13);
}

#ifdef SIMULATOR
// STATE, not screen. check_screen_coverage proves every screen gets opened; it
// cannot prove every screen gets opened in every STATE, and that gap is exactly
// where the 00000000 fingerprint and the impossible passphrase prompt lived --
// on a screen the walk photographed happily, in the one combination it never
// reached. Both faults were on the no-passphrase branch of a screen whose
// with-passphrase branch had a stop.
//
// So the walk can now ask for the combination directly. Reaching it by walking
// would mean committing a second wallet mid-run and rewriting everything after,
// which is how it stayed unphotographed in the first place.
void kiss_ui_sim_warn_screen(bool verified, bool fp_zero)
{
  if (s_warnscr) { lv_obj_delete_async(s_warnscr); s_warnscr = NULL; }
  s_backup_verified = verified;
  // "fingerprint back" has to actually put one back: the fp_zero call wipes
  // s_last_fp, and nothing on the forced path rederives it, so the verified
  // frame silently rendered the chip-alone layout while its walk comment
  // promised a card. The dev seed's code makes the frame honest.
  if (fp_zero) kiss_wipe(s_last_fp, sizeof s_last_fp);
  else if (!kiss_fp_known(s_last_fp))
    kiss_ui_set_last_fp((const uint8_t[4]){ 0x73, 0xC5, 0xDA, 0x0A });
  setup_warn_screen();
}
#endif

// reveal pop-in: the code card rises + fades in (one-shot, no per-frame cost after)
static void fp_pop_ty_cb(void *v, int32_t y) { lv_obj_set_style_translate_y((lv_obj_t *)v, y, 0); }
static void fp_pop_opa_cb(void *v, int32_t o) { lv_obj_set_style_opa((lv_obj_t *)v, o, 0); }

// The reveal screen's "?": the same explainer the home chip opens, for the
// wallet being revealed. s_shown_fp and not kiss_ui_last_fp, because the
// candidate is deliberately unpublished until the owner taps OPEN, and on the
// very first setup there is no last fingerprint at all.
static void fp_help_cb(lv_event_t *e) {
  (void)e;
  if (!s_fpscr) return;
  char fpbuf[16];
  snprintf(fpbuf, sizeof fpbuf, "%02X%02X%02X%02X",
           s_shown_fp[0], s_shown_fp[1], s_shown_fp[2], s_shown_fp[3]);
  kiss_info_fp_card_open(s_fpscr, s_shown_fp_valid ? fpbuf : NULL, false);
}

static void show_fingerprint(void) {
  uint8_t fp[4] = {0};
  if (kiss_fingerprint(s_plen ? s_pass : NULL, fp) != 0) {
    // derivation failed: STOP here. Never cache or reveal the zeroed fp —
    // it would flow into s_last_fp and render as the "SIGNING AS" identity.
    // The typed passphrase dies with it: the fail screen only inherits the
    // login's two-minute secret-idle wipe, and staging is already gone.
    if (s_setup_mode) kiss_seed_discard();
    wipe_login_secrets();
    setup_fail_screen();
    return;
  }
  memcpy(s_shown_fp, fp, sizeof s_shown_fp);
  s_shown_fp_valid = true;

  // An EMPTY passphrase is a legitimate choice with its own confirmation card,
  // and the two notes below used to describe a passphrase the owner does not
  // have -- on the first screen the device ever shows them.
  const bool nopass = (s_plen == 0);

  // A real page, at last. This was the one screen on the device built as a bare
  // lv_obj on the active screen: no wt_screen, no card frame, no title, no
  // action bar, two hand rolled pills. Everything it showed was centred in a
  // column down the middle with dead space either side, which is what "the rest
  // looks plain" was pointing at. The big code was never the problem.
  s_fpscr = wt_screen(lv_screen_active(), tr(STR_D_FINGERPRINT), NULL);
  lv_obj_remove_flag(s_fpscr, LV_OBJ_FLAG_CLICKABLE);  // buttons only, no tap-anywhere

  // The "?", top right, same 30px circle and 54px target as every other
  // anonymous help affordance on the device.
  //
  // The notes below say what the code MEANS on this screen; the card behind the
  // "?" is where the equation that produced it lives, and it is the same card
  // the home chip opens. This is the screen where the owner meets the number
  // for the first time, so it is where the question gets asked.
  //
  // The title has to be told to keep clear of the chip. wt_screen fits it to the
  // full 704px lane, and a title has no width of its own, so a long translation
  // of FINGERPRINT would run straight underneath the circle.
  wt_title_fit(s_fpscr, 640);
  wt_help_chip(s_fpscr, 715, 35, MUT_COL, fp_help_cb, NULL);

  // Band one: the code, in a framed card CENTRED at the size it has always had.
  // 420x118 with the number at num48 is what the owner asked to keep, and the
  // brief detour through a full width card is why they had to: something was
  // meant to sit beside it, so the card grew to fill the page when that fell
  // through. Nothing sits beside it. wt_diagram_fp was the candidate and it is
  // a flex ROW of three chips, about 600px in English and wider in half the
  // locales, so a 404 column clipped "RECOVERY WORDS" off one end and
  // "FINGERPRINT" off the other. It stays where it already lives, one tap away
  // behind the FINGERPRINT explainer, and this card goes back to being the one
  // object on its line.
  lv_obj_t *box = lv_obj_create(s_fpscr);
  lv_obj_remove_style_all(box);
  lv_obj_set_pos(box, 190, 96);
  lv_obj_set_size(box, 420, 118);
  lv_obj_set_style_radius(box, 16, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, wt_accent(), 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x0C1018), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  // The caption moves INSIDE the card. Outside it was an eyebrow floating over
  // a box; inside it is the box's own label, which is how every other framed
  // value on this device is built (wt_value_card, the receive address card).
  lv_obj_t *cap = lv_label_create(box);
  lv_label_set_text(cap, tr(STR_L_FP_CAP));
  lv_obj_set_style_text_color(cap, MUT_COL, 0);
  lv_obj_set_style_text_font(cap, wt_font14(), 0);
  lv_obj_set_style_text_letter_space(cap, 2, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 16);
  lv_obj_set_width(cap, 384);
  lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);

  lv_obj_t *big = lv_label_create(box);
  lv_label_set_text_fmt(big, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
  lv_obj_set_style_text_color(big, wt_accent(), 0);
  // font_kiss_num48, not lv_font_montserrat_48. This is the number the holder
  // copies onto paper next to their recovery words and compares against the
  // home screen forever after, so it is the one value on the device that most
  // needs to render identically every single time. The built-in Montserrat
  // face is proportional, so 8s and 1s changed the string's width with its
  // content. lv_font_montserrat_48 stays in the image regardless: six other
  // sites use it for LV_SYMBOL glyphs and overlay text, so this buys a stable
  // width here and nothing in flash.
  //
  // letter_space 4 goes with it. It was opening up a proportional face to make
  // a hex string scannable; a fixed advance already does that, and the extra
  // tracking on top pushed the 8 characters wider than the box.
  lv_obj_set_style_text_font(big, wt_font_num48(), 0);
  lv_obj_align(big, LV_ALIGN_BOTTOM_MID, 0, -18);

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

  // Band two: the two things an owner has to know about the code above, as the
  // review's pair of ruled blocks. They were centred grey paragraphs stacked
  // down the middle of the page, which is the arrangement people skip; two
  // claims side by side with a coloured rule are two claims somebody reads.
  //
  // Accent on the first because it states how the wallet works, WT_WARN on the
  // second because it is the branch where something has gone wrong: a code that
  // does not match the paper means the passphrase was mistyped, and every
  // passphrase is valid, so nothing else on the device will ever say so.
  //
  // One font for the pair, measured against the longer of the two. Sized apart
  // they land on different rungs and the block that matters more is whichever
  // happened to be shorter.
  {
    const char *b1 = tr(nopass ? STR_L_FP_NOTE_NOPASS  : STR_L_FP_NOTE);
    const char *b2 = tr(nopass ? STR_L_FP_NOTE2_NOPASS : STR_L_FP_NOTE2);
    // 232, not the 204 its siblings moved to: the FP reveal's code box is 190,96 118 tall, so it ends at 214,
    // so there is nothing to reclaim above this pair.
    const int BW = 344, BY = 232, BH = WT_CONTENT_BOTTOM - BY;
    // Measured against BH - 8, not BH. wt_body_font answers for the text alone
    // and wt_why_block wraps it in a box whose own metrics cost a couple of
    // pixels, so a translation that fits "exactly" overhangs: Czech ran 4px
    // past WT_CONTENT_BOTTOM at the size this said was fine.
    const lv_font_t *f = wt_body_font(strlen(b1) >= strlen(b2) ? b1 : b2,
                                      BW - 14, BH - 8);
    wt_why_block(s_fpscr, NULL, b1,  48, BY, BW, BH, f, wt_accent());
    wt_why_block(s_fpscr, NULL, b2, 408, BY, BW, BH, f, WT_WARN);
  }

  // The action bar every other screen has. This one holds the screen's real
  // action, so it is the second case in WT_BACK_X's rule: the way out goes
  // corner, and TAP TO OPEN takes the left. TAP TO OPEN commits the staged
  // seed on a PLAIN TAP, with no hold and no confirm in front of it, so the
  // corner is exactly where it must not be.
  wt_arrow_action(s_fpscr, tr(STR_C_BACK), true, false, WT_BACK_X,
                  WT_ACTION_Y, 140, true, fp_back_cb, NULL);
  wt_arrow_action(s_fpscr, tr(STR_L_TAP_TO_OPEN), false, true, WT_ACT_X,
                  WT_ACTION_Y, 260, false, fp_tap_cb, NULL);

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

// The callout shows the character just typed, a single plaintext char of the
// passphrase, in LVGL's heap: wipe it before it is replaced and when the
// callout hides, or the last char typed would sit in the label buffer until
// the login screen is torn down.
static void pop_wipe_text(void) {
  if (!s_pop_lbl) return;
  const char *t = lv_label_get_text(s_pop_lbl);
  if (t && *t) kiss_wipe((void *)t, strlen(t) + 1);
}

static void pop_hide_cb(lv_timer_t *t) {
  (void)t;
  s_pop_tmr = NULL;
  if (s_pop) lv_obj_add_flag(s_pop, LV_OBJ_FLAG_HIDDEN);
  pop_wipe_text();
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
    // mono34, the largest word-capable mono face: the bubble shows whatever
    // key was pressed, so the digits-only 48 face cannot serve it.
    lv_obj_set_style_text_font(s_pop_lbl, wt_font_mono34(), 0);
    lv_obj_center(s_pop_lbl);
  }
  int kx, ky, kw, kh;
  key_rect(id, &kx, &ky, &kw, &kh);
  pop_wipe_text();
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
  // Where shift goes is decided by the state we are IN, not by the label. The
  // old code read txt[0] == 'A' to tell "ABC" (go upper) from "abc" (go lower),
  // and those two labels are now one glyph, so the label can no longer answer
  // it. The state always could.
  if (strcmp(txt, KEY_SHIFT) == 0 || strcmp(txt, KEY_CAPS) == 0) {
    if (s_caps_lock) {                 // locked: this tap unlocks, back to lower
      s_caps_lock = false; s_one_shot = false; s_shift_t0 = 0;
      // a slow tap to UNLOCK must not be read as a hold and re-lock instantly
      s_hold_lock_ok = false;
      kb_plane(kb, MAP_LOWER);
    } else {
      bool dbl = s_shift_t0 && lv_tick_elaps(s_shift_t0) < SHIFT_DBL_MS;
      s_shift_t0 = lv_tick_get();
      s_hold_lock_ok = true;
      if (dbl)             { s_caps_lock = true; s_one_shot = false; kb_plane(kb, MAP_CAPS); }
      else if (s_one_shot) { s_one_shot = false; kb_plane(kb, MAP_LOWER); }
      else                 { s_one_shot = true;  kb_plane(kb, MAP_UPPER); }
    }
  }
  // "abc" only ever appears on the two symbol planes now, so it needs its own
  // arm. It used to fall through the shift branch above and land in the "go to
  // lowercase" leg by accident, which also silently dropped you out of caps
  // lock: s_caps_lock stayed true while the lowercase plane was showing. Coming
  // back to the plane you left is both more obvious and more honest.
  else if (strcmp(txt, KEY_ABC) == 0) {
    s_one_shot = false;
    kb_plane(kb, s_caps_lock ? MAP_CAPS : MAP_LOWER);
  }
  else if (strcmp(txt, KEY_SYM) == 0)  kb_plane(kb, MAP_SYM);
  else if (strcmp(txt, KEY_SYM2) == 0) kb_plane(kb, MAP_SYM2);
  else if (strcmp(txt, tr(STR_C_CANCEL)) == 0) {
    if (s_kef_mode) {
      // Nothing staged, nothing derived: fold the keyboard and hand the
      // screen back to whichever flow borrowed it.
      void (*cb)(void) = s_kef_cancel_cb;
      wipe_and_close();
      if (cb) cb();
    }
    else if (s_backup_verify_pass) {
      // This rehearsal is optional. Cancel returns to the warning with the
      // unverified red state; it does not abandon the wallet just created.
      s_backup_verify_pass = false;
      kiss_wipe(s_pass, sizeof s_pass);
      s_plen = 0;
      s_caret = 0;
      entry_refresh();
      setup_warn_screen();
    }
    else if (s_setup_mode && !s_pass_later) show_cancel_confirm(); // don't throw away a fresh seed on one tap
    else {
      // Normal login (nothing to lose) and add-later (the window's staged
      // seed does not exist). The add-later case owes a return: it opened
      // from Settings, so CANCEL goes back there rather than to the game.
      bool later = s_pass_later;
      void (*cb)(void) = s_unlocked_cb;
      wipe_and_close();
      if (later && cb) cb();
    }
  }
  else if (strcmp(txt, tr(STR_C_OK)) == 0) {
    if (s_kef_mode) {
      if (s_plen == 0) return;               // an empty password locks nothing
      if (s_kef_create && !s_kef_first_done && pass_bits() < 40) {
        show_weak_confirm();                 // offline guessing is the threat
                                             // and it never gets tired
      } else if (s_kef_create && !s_kef_first_done) {
        kef_accept_first();
      } else if (s_kef_create && strcmp(s_kef_first, s_pass) != 0) {
        kiss_wipe(s_kef_first, sizeof s_kef_first);
        s_kef_first_done = false;
        kiss_wipe(s_pass, sizeof s_pass);
        s_plen = 0;
        s_caret = 0;
        cap_set(tr(STR_L_NO_MATCH), lv_color_hex(0xFF4D5E), true);
        entry_refresh();
      } else {
        // The derive takes a visible moment (100k PBKDF2 on the device);
        // say so before blocking, or OK reads as a dead key.
        cap_set(tr(s_kef_create ? STR_L_KEF_LOCKING : STR_L_KEF_UNLOCKING),
                MUT_COL, true);
        lv_refr_now(NULL);
        int rc = s_kef_check_cb ? s_kef_check_cb(s_pass, (size_t)s_plen) : -1;
        if (rc != 0) {
          kiss_wipe(s_pass, sizeof s_pass);
          s_plen = 0;
          s_caret = 0;
          s_show = false;
          if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
          cap_set(tr(s_kef_create ? STR_L_KEF_FAIL : STR_L_KEF_BAD),
                  lv_color_hex(0xFF4D5E), true);
          entry_refresh();
        } else {
          void (*cb)(void) = s_kef_done_cb;
          wipe_and_close();
          if (cb) cb();
        }
      }
    }
    else if (s_backup_verify_pass) {
      uint8_t fp[4] = {0};
      bool match = kiss_fingerprint(s_plen ? s_pass : NULL, fp) == 0
                && kiss_rehearse_pass_ok(fp, s_last_fp);
      kiss_wipe(fp, sizeof fp);
      kiss_wipe(s_pass, sizeof s_pass);
      s_plen = 0;
      s_caret = 0;
      s_show = false;
      s_flash = false;
      if (s_showbtn_lbl) lv_label_set_text(s_showbtn_lbl, tr(STR_L_SHOW));
      if (!match) {
        cap_set(tr(STR_L_BACKUP_PASS_BAD), WT_STOP, true);
        entry_refresh();
      } else {
        s_backup_verify_pass = false;
        s_backup_verified = true;
        kiss_backup_mark(s_last_fp);   // and it outlives this session now
        entry_refresh();
        setup_warn_screen();
      }
    }
    // Restoring: one entry, and the fingerprint on the next screen is the
    // check. See s_restore_mode.
    else if (s_setup_mode && s_restore_mode) {
      kiss_wipe(s_first, sizeof s_first);
      show_fingerprint();
    }
    // Empty, in setup: the same place the NO PASSPHRASE action goes, and for its
    // reason. There is no secret here to call weak, and the card now refuses
    // rather than asking -- so leaving empty on this arm would have made OK a
    // dead key on a screen where a dead key reads as a missed touch. BACK from
    // the fingerprint uncovers this keyboard, so an accidental empty costs one
    // tap to undo.
    else if (s_setup_mode && !s_first_done && s_plen == 0) {
      kiss_wipe(s_pass, sizeof s_pass);
      s_plen = 0;
      s_caret = 0;
      kiss_wipe(s_first, sizeof s_first);
      show_fingerprint();
    }
    else if (s_setup_mode && !s_first_done && pass_bits() < 40) {
      show_weak_confirm();                   // a dead end: BACK, and lengthen it
    } else if (s_setup_mode && !s_first_done) {
      // setup: capture the first entry, demand it again — a typo here is an
      // unreproducible passphrase (= lost coins) later
      setup_accept_first();
    } else if (s_setup_mode && strcmp(s_first, s_pass) != 0) {
      kiss_wipe(s_first, sizeof s_first);
      s_first_done = false;
      kiss_wipe(s_pass, sizeof s_pass);
      s_plen = 0;
      s_caret = 0;
      cap_set(tr(STR_L_NO_MATCH), lv_color_hex(0xFF4D5E), true);
      entry_refresh();
    } else {
      kiss_wipe(s_first, sizeof s_first);
      show_fingerprint();
    }
  }
  else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
    pass_edit_delete(s_pass, &s_plen, &s_caret);
    s_flash = false;
    setup_cap_reset();                       // editing cancels a weak-ack prompt
    entry_refresh();
  } else if (strlen(txt) == 1 && txt[0] >= 0x20 && txt[0] < 0x7F) {
    // printable ASCII only (spec, v1)
    if (pass_edit_insert(s_pass, PASS_MAX, &s_plen, &s_caret, txt[0])) {
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
  // the press already swapped the plane under the finger. KEY_ABC is no longer
  // in this set: it belongs to the symbol planes, and holding it should not
  // lock capitals.
  if (strcmp(txt, KEY_SHIFT) == 0 || strcmp(txt, KEY_CAPS) == 0) {
    if (!s_hold_lock_ok) return;            // this press was the unlock tap
    s_hold_lock_ok = false;
    s_caps_lock = true; s_one_shot = false; s_shift_t0 = 0;
    kb_plane(kb, MAP_CAPS);
    return;
  }

  if (strlen(txt) != 1) return;
  char c = txt[0];
  if (c < 'a' || c > 'z') return;            // only letters have another case
  // The character the press just entered sits before the caret, wherever the
  // caret is. Testing the end of the buffer instead would silently do nothing
  // after a tap, leaving the plane flipped and the letter lowercase.
  if (s_caret == 0 || s_pass[s_caret - 1] != c) return;   // not the char just typed
  s_pass[s_caret - 1] = (char)(c - 'a' + 'A');
  setup_cap_reset();
  flash_last();
  char up[2] = { s_pass[s_caret - 1], 0 };
  pop_show(up, id);
}

// ---- passphrase from a QR ----
// The scanned text becomes the passphrase exactly as decoded, including case
// and spaces: a passphrase that cannot be reproduced byte for byte is a wallet
// nobody can reopen. Refuse anything the keyboard could not have typed, for
// the same reason (see the printable-ASCII rule above).
// The scan screen closes itself before calling back, so the keyboard underneath
// is already visible again: fill it in place rather than rebuilding it (which
// kiss_login_open would refuse to do anyway while a login is open).
static void pp_scan_text_cb(const char *txt, size_t len) {
  if (!s_login || len == 0 || len > PASS_MAX)
    return;                                 // unusable: leave what was typed
  for (size_t i = 0; i < len; i++)
    if (txt[i] < 0x20 || txt[i] > 0x7E)
      return;
  memcpy(s_pass, txt, len);
  s_pass[len] = 0;
  s_plen = (int)len;
  s_caret = s_plen;             // a scan fills the field; typing continues at the end
  entry_refresh();
}

static void pp_scan_cancel_cb(void) { }     // the keyboard was never torn down

static void pp_scan_go_cb(lv_event_t *e) {
  lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
  kiss_scan_open_raw(lv_screen_active(), pp_scan_text_cb, pp_scan_cancel_cb);
}

static void pp_scan_back_cb(lv_event_t *e) {
  lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void pp_scan_warn_cb(lv_event_t *e) {
  (void)e;
  lv_obj_t *scr = wt_screen(lv_screen_active(), tr(STR_L_SCAN_WARN_T),
                            tr(STR_L_SCAN_WARN_S));
  lv_obj_move_foreground(scr);
  wt_why_body(scr, tr(STR_L_SCAN_WARN_B), 122, WT_WARN, true);
  wt_arrow_action(scr, tr(STR_L_SCAN_GO), false, true, WT_ACT_X, WT_ACTION_Y,
                  300, false, pp_scan_go_cb, scr);
  wt_arrow_action(scr, tr(STR_C_BACK), true, false, WT_BACK_X, WT_ACTION_Y,
                  140, true, pp_scan_back_cb, scr);
}

static void show_cb(lv_event_t *e) {
  (void)e;
  s_show = !s_show;
  // Hiding returns the caret to the end. Editing mid-string behind the dots is
  // blind, and the masked view truncates past 78 characters anyway, so the
  // caret could sit somewhere the owner cannot see.
  if (!s_show) s_caret = s_plen;
  lv_label_set_text(s_showbtn_lbl, s_show ? tr(STR_L_HIDE) : tr(STR_L_SHOW));
  entry_refresh();
}

// Tap a character to put the caret there. SHOW only, for the reason above.
//
// lv_label_get_letter_on wants a point relative to the object's own top-left
// and subtracts the padding itself, so hand it the raw offset from coords.x1.
// It answers with the index of the glyph whose cell the tap landed in, which
// on its own always puts the caret BEFORE that glyph: tapping a character and
// pressing backspace would then delete its neighbour. Comparing against the
// glyph's midpoint is what makes "tap the character you want gone" work.
static void entry_tap_cb(lv_event_t *e) {
  if (!s_show || s_plen == 0 || !s_entry) return;
  lv_indev_t *indev = lv_event_get_indev(e);
  if (!indev) return;

  lv_point_t pt;
  lv_indev_get_point(indev, &pt);
  lv_area_t a;
  lv_obj_get_coords(s_entry, &a);
  lv_point_t rel = { pt.x - a.x1, pt.y - a.y1 };

  lv_obj_update_layout(s_entry);
  int c = (int)lv_label_get_letter_on(s_entry, &rel, false);
  if (c < 0) c = 0;
  if (c > s_plen) c = s_plen;

  if (c < s_plen) {
    int32_t pad_l = lv_obj_get_style_pad_left(s_entry, LV_PART_MAIN);
    lv_point_t p0, p1;
    lv_label_get_letter_pos(s_entry, (uint32_t)c, &p0);
    lv_label_get_letter_pos(s_entry, (uint32_t)(c + 1), &p1);
    // Only when both sit on the same line. A glyph at a wrap point has its
    // successor on the next line, where a midpoint in x means nothing.
    if (p1.y == p0.y && rel.x - pad_l > (p0.x + p1.x) / 2) c++;
  }

  s_caret = c;
  s_flash = false;                  // the flashed character is no longer the newest
  caret_refresh();
}

void kiss_ui_ensure_indev(void) { ensure_indev(); }

// setup-only interstitial: the passphrase deserves one calm screen of WHY
// before the keyboard appears (meter + type-twice enforce the HOW).
static void (*s_setup_next_cb)(void);
static void pp_intro_go_cb(lv_event_t *e) {
  (void)e;
  lv_obj_delete_async(s_pp_intro);
  s_pp_intro = NULL;
  kiss_login_open(s_setup_next_cb);
}

// The other half of the choice, and it was missing. A wallet with no passphrase
// is not an edge case here: it has its own warning screen (L_WARN_T_NOPASS),
// its own fingerprint notes, and the duress chooser deliberately refuses to
// appear for it (setup_warn_ok_cb) -- all of it written and translated. The
// only thing the device never had was a way to CHOOSE it. Reaching it meant
// pressing OK on an empty keyboard and then overriding a card that says a
// short passphrase is easier to guess, which is the wrong warning for a
// passphrase that does not exist and frames a legitimate choice as a mistake.
//
// CREATE PASSPHRASE keeps the primary slot, so the default still steers the way
// it always did. This action only stops the device lying about the alternative.
static void pp_intro_nopass_cb(lv_event_t *e) {
  (void)e;
  lv_obj_delete_async(s_pp_intro);
  s_pp_intro = NULL;
  kiss_login_open(s_setup_next_cb);
  // Empty is the whole point, so state the emptiness rather than inheriting it:
  // every other entry to this screen has been through wipe_and_close, but a
  // buffer this one never wrote is not a promise, and show_fingerprint derives
  // from s_plen.
  kiss_wipe(s_pass, sizeof s_pass);
  s_plen = 0;
  s_caret = 0;
  // No weak card on the way past: that card exists to question a guessable
  // secret and there is no secret here to question. The accurate warning is
  // setup_warn_screen's, and it still runs before the wallet opens.
  //
  // The keyboard is built and then hidden by show_fingerprint, which is what
  // makes BACK work: it uncovers a keyboard that is already there, so "actually,
  // set one" costs a single tap rather than restarting setup.
  show_fingerprint();
}

void kiss_login_open_restore(void (*unlocked_cb)(void)) {
  s_restore_mode = true;
  kiss_login_open_setup(unlocked_cb);
}

void kiss_login_open_setup(void (*unlocked_cb)(void)) {
  // Not cleared here: kiss_login_open_restore sets it and calls straight in.
  // Every other path through the wizard reaches this function without it set,
  // and login_teardown puts it back.
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

  // This screen was a title, one 704px grey paragraph and a button. Three
  // paragraphs stacked down the page is the arrangement people skip, and it is
  // the exact shape wt_why_block was written to replace -- on the one screen
  // that has to land, because everything it says is irreversible.
  //
  // Band one: the equation, framed. words + passphrase -> fingerprint is not
  // decoration here, it IS the subject: the sentence "your passphrase chooses
  // which wallet you get" drawn instead of written. Each chip carries its own
  // mark, so the claim arrives before the labels are read.
  //
  // 128..212, the same rhythm the fingerprint reveal uses (card ends 214, blocks
  // start 232), so the two setup screens share a skeleton.
  lv_obj_t *card = wt_card(scr, 48, 128, 704, 64);
  lv_obj_t *col = lv_obj_create(card);
  lv_obj_remove_style_all(col);
  lv_obj_set_pos(col, 0, 0);
  lv_obj_set_size(col, 704, 84);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  wt_diagram_fp(col);

  // Band two: the two claims, side by side, on the reveal screen's geometry.
  // Accent on how it works, WT_WARN on the branch where it goes wrong -- the
  // same colour argument the reveal screen makes, so a reader who has seen one
  // already knows which side is the warning.
  {
    const char *b1 = tr(STR_L_PPINTRO_W1_B), *b2 = tr(STR_L_PPINTRO_W2_B);
    const int BW = 344, BY = 204, BH = WT_CONTENT_BOTTOM - BY;
    // The shared font is measured against the room LEFT BY THE HEADING, which
    // wt_why_block adds above the body at font14. Budgeting for two heading
    // lines costs a rung in the locales whose heading fits on one, and that is
    // the safe direction: the alternative is a heading that wraps in Norwegian
    // and pushes the body through WT_CONTENT_BOTTOM into the action row.
    const lv_font_t *f = wt_body_font2_head(tr(STR_L_PPINTRO_W1_H), b1,
                                           tr(STR_L_PPINTRO_W2_H), b2,
                                           BW - 14, BH);
    wt_why_block(scr, tr(STR_L_PPINTRO_W1_H), b1,  48, BY, BW, BH, f, wt_accent());
    wt_why_block(scr, tr(STR_L_PPINTRO_W2_H), b2, 408, BY, BW, BH, f, WT_WARN);
  }

  // Both ways forward, in the row's usual arrangement: the plainer choice
  // leftmost, the one the product steers toward primary in the corner. The
  // arrow actions size to their words, so KEINE PASSPHRASE and БЕЗ КОДОВОЙ
  // ФРАЗЫ cost nothing but width they actually use.
  wt_arrow_action(scr, tr(STR_L_NO_PASSPHRASE), false, false, 48, WT_ACTION_Y,
                  330, false, pp_intro_nopass_cb, NULL);
  // CREATE PASSPHRASE is an instruction to invent one, which is wrong for words
  // being restored: theirs already exists and inventing a second opens a
  // different wallet. PASSPHRASE / NO PASSPHRASE is the parallel pair, and both
  // halves already ship.
  wt_arrow_action(scr, tr(s_restore_mode ? STR_L_PASSPHRASE_CAP
                                         : STR_L_CREATE_PASS_BTN),
                  false, true, 422, WT_ACTION_Y, 330, true,
                  pp_intro_go_cb, NULL);
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

// "ADD A PASSPHRASE" from Settings: the same type-twice, weak-ack, warning
// and fingerprint reveal the wizard's passphrase step runs -- minus the staged
// seed. There is none: the signer's wallet is already committed, and the
// passphrase only derives the session. s_pass_later diverts the two staging
// touchpoints (fp_tap_cb's commit and the cancel overlay's one-tap shield).
void kiss_login_open_add_later(void (*unlocked_cb)(void))
{
  kiss_login_open_setup(unlocked_cb);
  s_pass_later = true;
}

void kiss_login_open(void (*unlocked_cb)(void)) {
  if (kiss_ui_active()) return;
  ensure_indev();
  s_unlocked_cb = unlocked_cb;
  // localized CANCEL and OK on every plane, CAPS included (slots 32 and 34 =
  // button ids 29 and 31; slot 33 is the space key, width 3, and must keep
  // its single space). kb_cb compares against the same tr() pointers, so the
  // matches are exact. CAPS used to keep the static literals, so caps lock
  // showed "CANCEL"/"OK" in every locale (hr-HR: OTKAŽI / U REDU).
  MAP_LOWER[32] = MAP_UPPER[32] = MAP_CAPS[32] = MAP_SYM[32] = MAP_SYM2[32] =
      tr(STR_C_CANCEL);
  MAP_LOWER[34] = MAP_UPPER[34] = MAP_CAPS[34] = MAP_SYM[34] = MAP_SYM2[34] =
      tr(STR_C_OK);

  s_login = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_login);
  lv_obj_set_size(s_login, 800, 480);
  lv_obj_set_style_bg_color(s_login, BG_COL, 0);
  lv_obj_set_style_bg_opa(s_login, LV_OPA_COVER, 0);

  s_cap = lv_label_create(s_login);
  lv_obj_set_width(s_cap, CAP_W);          // stop long alerts under SCAN/SHOW
  lv_label_set_long_mode(s_cap, LV_LABEL_LONG_WRAP);
  // "CREATE YOUR PASSPHRASE" over a keyboard where the owner is RE-ENTERING one
  // they already have is an instruction to invent a second one, which opens a
  // different wallet. Restoring gets the plain caption the ordinary unlock uses.
  cap_set(s_kef_mode
              ? tr(s_kef_create ? STR_L_KEF_PASS_NEW : STR_L_KEF_PASS_OPEN)
          : s_setup_mode && !s_restore_mode ? tr(STR_L_CREATE_YOUR_PASS)
                                            : tr(STR_L_PASSPHRASE_CAP),
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
  // QR is only as private as wherever that QR lives. Not offered for a KEF
  // password: that flow is already inside a QR ritual, and a second camera
  // hop from a password screen is a surface nobody asked for.
  if (!s_kef_mode) {
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
  // Tap-to-edit. One line of font28 is a 34px target, which is mean for a
  // fingertip, so the touch area is widened without moving the layout.
  lv_obj_add_flag(s_entry, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_entry, 20);
  lv_obj_add_event_cb(s_entry, entry_tap_cb, LV_EVENT_CLICKED, NULL);

  s_caret_obj = lv_obj_create(s_entry);
  lv_obj_remove_style_all(s_caret_obj);
  lv_obj_set_style_bg_opa(s_caret_obj, LV_OPA_COVER, 0);
  lv_obj_set_size(s_caret_obj, 2, 2);
  // The caret must never swallow a tap aimed at the character behind it
  lv_obj_clear_flag(s_caret_obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_caret_obj, LV_OBJ_FLAG_HIDDEN);

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

  s_pp_hint = lv_label_create(s_login);    // length-coaching hint (setup mode only)
  lv_label_set_text(s_pp_hint, "");
  lv_obj_set_style_text_font(s_pp_hint, wt_font14(), 0);
  lv_obj_set_style_text_color(s_pp_hint, MUT_COL, 0);
  lv_obj_align(s_pp_hint, LV_ALIGN_TOP_MID, 0, 132);   // centered between count and meter
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

// The login keyboard, borrowed for a KEF backup password. create = invent one
// (meter, weak ack, typed twice); otherwise one entry. on_check runs on OK
// with the accepted password and answers 0 or -1 — on -1 the keyboard stays,
// shows one vague failure and lets the owner retype; on 0 the keyboard folds
// itself and THEN calls on_done, so the next screen never finds the login
// still standing. CANCEL folds and calls on_cancel. No wallet, no session,
// no fingerprint anywhere in this mode.
void kiss_ui_kef_pass_open(bool create,
                           int (*on_check)(const char *pass, size_t len),
                           void (*on_done)(void), void (*on_cancel)(void))
{
  if (kiss_ui_active()) return;
  s_kef_mode = true;
  s_kef_create = create;
  s_kef_first_done = false;
  s_kef_first[0] = 0;
  s_kef_check_cb = on_check;
  s_kef_done_cb = on_done;
  s_kef_cancel_cb = on_cancel;
  kiss_login_open(NULL);
}

// ---- build identity (shared: Settings footer + wallet home corner) ----
// Honest about what this firmware is: version + commit, then the flash-
// encryption state read from the CHIP eFuse at runtime and the C6 radio
// reset pad read back from the GPIO, never assumed from the build. Bad
// states are amber WARNINGS; good states go calm. Dev builds carry a "dev"
// marker in amber (dev seed, no release hardening).
void kiss_build_id_restyle(lv_obj_t *version_label)
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

// Row pitch for the STACKED build identity: font14's line box is 19px, and 3
// of air is what keeps two rows reading as a block rather than as one
// squashed paragraph. Both rows together are 41px, which fits inside the
// action bar (398..480) with room above and below.
static int s_build_id_right;   // measured right edge, see kiss_build_id_right

#define BUILD_ID_ROW 22

// Gap between version and encryption when they share ONE row. Wider than a
// word space on purpose: these are two unrelated facts, not a sentence, and
// nothing may be drawn between them (a separator here would be a dash used as
// punctuation, which this codebase does not do).
#define BUILD_ID_GAP 28

// Two callers, two shapes, and the shape belongs to the CALLER, not to this
// function's other argument. Settings stacks, because it has three facts to
// place beside a row of buttons. The home corner does not: it has two, an
// empty bottom edge to put them on, and stacking them there turned a quiet
// one line signature into a two line block wedged into the corner, which is
// what the device showed and what got this parameter written.
bool kiss_fp_card(lv_obj_t *parent, int y)
{
  uint8_t fp[4];
  kiss_ui_last_fp(fp);
  // kiss_fp_known and not a fresh test of the same bytes: this is the third
  // screen to need the question and the answer already had a name.
  if (!kiss_fp_known(fp)) return false;
  char id[16];
  snprintf(id, sizeof id, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
  // Boxless. The value card drew a panel and an edge around a caption in
  // font14 -- a box around fine print, the two shapes this look removes.
  // The same two facts now sit open on the glass: caption at the chrome
  // rung, code at the def rows' own mono28.
  const char *cap = tr(STR_L_FP_CAP);
  lv_obj_t *c = wt_lbl(parent, cap, 231, y, wt_chrome21(cap), wt_accent());
  lv_obj_add_flag(c, WT_FLAG_ACCENT);
  lv_obj_set_style_text_letter_space(c, 2, 0);
  lv_obj_set_width(c, 340);
  lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *v = wt_lbl(parent, id, 231,
                       y + lv_font_get_line_height(wt_chrome21(cap)) + 6,
                       wt_font_mono28(), WT_INK);
  lv_obj_set_style_text_letter_space(v, 2, 0);
  lv_obj_set_width(v, 340);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
  return true;
}

lv_obj_t *kiss_build_id_make(lv_obj_t *parent, int x, int y, bool with_radio,
                               bool stacked)
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
  kiss_build_id_restyle(v);
#else
  lv_label_set_text_fmt(v, "KISS %s dev (%s)", KISS_VERSION_STR, KISS_COMMIT_STR);
  kiss_build_id_restyle(v);
#endif
  lv_obj_t *w = lv_label_create(parent);
  lv_obj_set_style_text_font(w, wt_font14(), 0);
  // The word in full. It was abbreviated to "enc" back when the commit came
  // from `git describe` and this line ran the width of the panel -- but that
  // was the commit's fault, and shortening the one word a reader actually
  // needs was the wrong half to cut. A bare short hash left the room.
  //
  // Stays ASCII on purpose: a glyph missing from the generated font draws an
  // empty placeholder box, and this line is the wrong place to spend a "." or
  // an em dash on the chance one is missing.
  // Stacked, this used to run version, encryption and radio end to end, which
  // made it as wide as the panel and pushed it into the strip UNDER the action
  // row, the only place a 450px line still fitted. In two rows it is ~215px and
  // sits in the action bar's own empty left half instead, beside the buttons
  // rather than beneath them.
  //
  // The version gets the top row to ITSELF, because it is the field that grows:
  // a longer version string or a dirty commit suffix extends row one and leaves
  // the status facts below exactly where they were. On one row that argument
  // does not apply, since there is nothing under them to push.
  lv_label_set_text_fmt(w, "encryption: %s", enc ? "ON" : "OFF");
  lv_obj_set_style_text_color(w, enc ? MUT_COL : lv_color_hex(0xF2B84B), 0);
  if (stacked) {
    // Row two, and the version keeps row one to ITSELF. Encryption was tried up
    // there beside it and the gate caught what the arithmetic missed: a DEV
    // version prints "KISS 0.1.0-beta7 dev (local)", which pushes encryption
    // right far enough to land on the theme name at y=412. A release version is
    // shorter and would have fitted, which is the worst kind of pass.
    //
    // The version is the field that grows, so nothing shares its row.
    lv_obj_set_pos(w, x, y + BUILD_ID_ROW);
  } else {
    lv_obj_update_layout(v);
    lv_obj_set_pos(w, x + lv_obj_get_width(v) + BUILD_ID_GAP, y);
  }

  // The C6 radio readback is a diagnostic for people who already know what a
  // C6 is. It earns its place in Settings, not in the corner of the home
  // screen where it was permanent chrome nobody could act on.
  // Right edge of row two, measured, so the block below can take the max of the
  // two rows rather than assuming which one won.
  int facts_right = 0;
  if (with_radio) {
    lv_obj_t *r = lv_label_create(parent);
    lv_obj_set_style_text_font(r, wt_font14(), 0);
    lv_label_set_text_fmt(r, "radio: %s", radio_held ? "HELD" : "NOT HELD");
    lv_obj_set_style_text_color(r, radio_held ? MUT_COL : lv_color_hex(0xF2B84B), 0);
    // Shares its row with encryption, and follows its MEASURED width: the word
    // is ON or OFF and the translation of neither is fixed, so the gap is added
    // to what encryption actually rendered rather than to a guess about it.
    lv_obj_update_layout(w);
    lv_obj_set_pos(r, lv_obj_get_x(w) + lv_obj_get_width(w) + 24,
                   lv_obj_get_y(w));

    // Third fact on the same row: whether the chip's RNG has a physical noise
    // source behind it (kiss_crypto.h). It is stated rather than tested
    // because no test can tell a seeded PRNG from a TRNG -- a self test that
    // generates seeds and counts collisions passes cleanly on a board whose
    // source was switched off, which is precisely the state this firmware
    // shipped in until it was turned on at boot. Provenance is the only
    // answerable question, so provenance is what the line reports.
    //
    // OFF is amber for the same reason encryption OFF is: it does not mean the
    // seed is weak (that is a fold of camera, chip and taps, so a dead source
    // costs a source), it means the SD backup key has one source and that one
    // is not what it claims. Follows radio's measured width, same as above.
    lv_obj_t *n = lv_label_create(parent);
    lv_obj_set_style_text_font(n, wt_font14(), 0);
    // "randomness", not "RNG" or "TRNG": the two facts beside it are named in
    // words a reader can look up, and the acronym buys nothing an owner can
    // act on. NOISE says where the numbers come from, which is the whole
    // claim. The bad state is NO SOURCE rather than OFF or NONE, because
    // neither of those is true -- numbers still come out, they just have
    // nothing physical behind them, and that is the sentence to render.
    // Two legs, named, because the seed folds two independent physical
    // sources and a footer that says only NOISE describes the one that can
    // fail. TIMING is the board's own delays (kiss_jitter): a different
    // circuit from the chip's noise, which is the whole reason it is folded.
    // It needs no switch and cannot be off, so it has no bad state to report.
    bool noise = kiss_trng_live();
    lv_label_set_text_fmt(n, "randomness: %s",
                          noise ? "NOISE + TIMING" : "NO SOURCE");
    lv_obj_set_style_text_color(n, noise ? MUT_COL : lv_color_hex(0xF2B84B), 0);
    // Beside radio, on row two, stacked or not. It used to take a third row of
    // its own on the grounds that three facts end to end reach x=443 and read
    // as a caption under the colour picker rather than a line of this block.
    // They never overlapped anything -- that was an aesthetic call, and the
    // owner has made the opposite one: two rows of small print in the corner of
    // Settings beat three.
    lv_obj_update_layout(r);
    lv_obj_set_pos(n, lv_obj_get_x(r) + lv_obj_get_width(r) + 24,
                   lv_obj_get_y(r));
    lv_obj_update_layout(n);
    facts_right = lv_obj_get_x(n) + lv_obj_get_width(n);
  } else {
    (void)radio_held;
  }

  // Where this block actually ENDS, measured, for whatever sits beside it.
  //
  // The home's SD badge was pinned at a constant x=410 with a comment saying
  // the build line "ends around x=380". It does, in a dev build: "KISS 0.1.0
  // -beta7 dev (local)" plus "encryption: OFF" lands at 382. A RELEASE build
  // prints the commit instead -- "KISS 0.1.0-beta7 (58ae53d-dirty)" -- which
  // is four characters wider, and the badge came down on the F of OFF. The
  // version is the field that grows; anything to its right has to ask.
  lv_obj_update_layout(w);
  s_build_id_right = lv_obj_get_x(w) + lv_obj_get_width(w);
  if (stacked) {
    // Stacked, row one is version + encryption and row two is radio +
    // randomness, so the block's right edge is whichever row won. w is the end
    // of row one; facts_right is the end of row two, or 0 when there is none.
    if (facts_right > s_build_id_right) s_build_id_right = facts_right;
  }

  return v;
}

// The right edge of the row kiss_build_id_make last drew. Read it straight
// after building, before anything else lays out beside it.
int kiss_build_id_right(void) { return s_build_id_right; }

#ifndef ESP_PLATFORM
void kiss_ui_test_recover_screen(void) { recover_screen(); }
void kiss_ui_test_recover_close(void)
{
    s_words_from_recov = false;
    recover_close();
}
bool kiss_ui_test_rendered_secret_empty(void)
{
    const char *entry = s_entry ? lv_label_get_text(s_entry) : NULL;
    const char *pop = s_pop_lbl ? lv_label_get_text(s_pop_lbl) : NULL;
    return (!entry || !*entry) && (!pop || !*pop);
}
#endif
