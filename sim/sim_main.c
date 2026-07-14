// Headless LVGL simulator for the FRUIT ISLAND game.
// Compiles the REAL game code (main/main.c with -DSIMULATOR) against desktop
// LVGL, renders into an in-memory RGB565 framebuffer, scripts touch input, and
// dumps frames to .ppm so the actual rendering can be inspected without hardware.
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

// Whole game is LANDSCAPE: the sim renders the 800x480 logical canvas directly
// (the device reaches it via a one-time panel rotation at boot).
#define HRES 800
#define VRES 480

void build_game(void);  // from main/main.c

static uint16_t g_fb[HRES * VRES];
static int g_tx, g_ty;
static bool g_pressed;

// Render-cost instrumentation. The sim can't measure wall-clock lag (desktop is
// orders of magnitude faster than the P4), but it sees every pixel LVGL flushes,
// and per-frame redraw area is exactly what the P4 software renderer is slow at.
// So this counts flushed pixels -> an objective proxy for device render cost.
static long g_flush_px;
static long g_flush_n;      // number of flush calls
static long g_flush_max;    // largest single flush area

// platform seam: the game reads "touch" from here
bool platform_read_touch(int *x, int *y) {
  if (g_pressed) { *x = g_tx; *y = g_ty; return true; }
  return false;
}

// crypto seam: the sim has no libwally; fake a passphrase-dependent fingerprint
// (empty passphrase yields the real dev-seed value so screens match the device)
int wallet_fingerprint(const char *passphrase, unsigned char out[4]) {
  unsigned char h = 0;
  for (const char *p = passphrase ? passphrase : ""; *p; p++) h = (h * 31) ^ *p;
  out[0] = 0x73 ^ h; out[1] = 0xC5 ^ h; out[2] = 0xDA ^ h; out[3] = 0x0A ^ h;
  return 0;
}

// step-7 seams: the seed store is a RAM flag. A fresh sim run starts SEEDED so
// the legacy script flows unchanged; the wizard test at the end wipes first.
#include <string.h>
#include "wallet_seed.h"
static char s_sim_seed[256] =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";
static int s_sim_has_seed = 1;
static char s_sim_pending[256];
static int s_sim_has_pending;
int wallet_seed_exists(void) { return s_sim_has_seed || s_sim_has_pending; }
int wallet_seed_store(const char *m) {
  snprintf(s_sim_seed, sizeof s_sim_seed, "%s", m);
  s_sim_has_seed = 1;
  return 0;
}
int wallet_seed_load(char *out, size_t n) {
  if (s_sim_has_pending) { snprintf(out, n, "%s", s_sim_pending); return 0; }
  if (!s_sim_has_seed) return -1;
  snprintf(out, n, "%s", s_sim_seed);
  return 0;
}
int wallet_seed_wipe(void) { s_sim_has_seed = 0; return 0; }
int wallet_seed_validate(const char *m) { (void)m; return 0; }
int wallet_seed_stage(const char *m) {
  snprintf(s_sim_pending, sizeof s_sim_pending, "%s", m);
  s_sim_has_pending = 1;
  return 0;
}
int wallet_seed_commit(void) {
  if (!s_sim_has_pending) return -1;
  snprintf(s_sim_seed, sizeof s_sim_seed, "%s", s_sim_pending);
  s_sim_has_seed = 1;
  s_sim_has_pending = 0;
  return 0;
}
void wallet_seed_discard(void) { s_sim_has_pending = 0; }
static const char *SIM_WORDS[] = {
  "gravity", "machine", "north", "sort", "system", "female", "filter",
  "attitude", "volume", "fold", "club", "stay", "feature", "office",
  "ecology", "stable", "narrow", "fence", "abandon", "ability", "able",
  "about", "zone", "zoo"};
int wallet_seed_from_entropy(const uint8_t *e, size_t len, char *out, size_t n) {
  (void)e;
  int count = len == 32 ? 24 : 12;
  size_t o = 0;
  for (int i = 0; i < count && o + 12 < n; i++)
    o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", SIM_WORDS[i]);
  return 0;
}
int wallet_seed_word(int i, const char **out) {
  *out = SIM_WORDS[i % 24];
  return 0;
}
int wallet_seed_suggest(const char *prefix, const char *out[], int n) {
  int found = 0;
  for (int i = 0; i < 24 && found < n; i++)
    if (strncmp(SIM_WORDS[i], prefix, strlen(prefix)) == 0)
      out[found++] = SIM_WORDS[i];
  return found;
}

// network seam: wallet_settings + the verify screen read it (no wallet_crypto.c
// in the sim, so the real setter lives here as a plain flag)
static int s_sim_testnet;
void wallet_set_network(int testnet) { s_sim_testnet = testnet; }
int wallet_testnet(void) { return s_sim_testnet; }
static int s_sim_script;
void wallet_set_script(int s) { s_sim_script = s; }
int wallet_script(void) { return s_sim_script; }

// step-4 session seams: plausible-looking fakes so the Receive/Export screens render
int wallet_session_open(const char *passphrase) { (void)passphrase; return 0; }
void wallet_session_close(void) {}
int wallet_session_address(int change, unsigned int index, char *out, unsigned long len) {
  if (s_sim_script == 2)                 // legacy 1.../m...
    snprintf(out, len, "%c%s%02u", s_sim_testnet ? 'm' : '1',
             "K3n7xPq2wDeRfGh9jLmNoPqRsTuV", (change * 50 + index) % 100u);
  else if (s_sim_script == 1)            // nested 3.../2...
    snprintf(out, len, "%c%s%02u", s_sim_testnet ? '2' : '3',
             "J8k4tYp6wA1zX3cV5bN7mQ9rS2dF", (change * 50 + index) % 100u);
  else
    snprintf(out, len, "%s1qcr8te4kr609gcawutmrza0j4xv80jy8z3%c%02u",
             s_sim_testnet ? "tb" : "bc", change ? 'c' : 'q', index % 100u);
  return 0;
}
int wallet_session_bw_export(char *out, unsigned long len) {
  snprintf(out, len, "[73c5da0a/84'/%d'/0']zpub6rFR7y4Q2AijBEqTUquhVz398htDFrt"
                     "ymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31"
                     "mGDtKsAYz2oz2AGutZYs", s_sim_testnet ? 1 : 0);
  return 0;
}
int wallet_session_descriptor(char *out, unsigned long len) {
  snprintf(out, len, "wpkh([73c5da0a/84h/0h/0h]xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuy"
                     "vz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhL"
                     "v1VMrjPC7PW6V/<0;1>/*)");
  return 0;
}

// step-5 seams: no libwally in the sim, so fake the PSBT layer with the same
// numbers the desktop test fixture uses. A file whose content contains "STOP"
// renders the blocked verify screen (so the sim can show both states).
// (step 6's qr_transport + cUR are REAL in the sim — only the camera is faked,
// by injecting decoded strings via wallet_scan_inject.)
#include "wallet_psbt.h"
#include "wallet_scan.h"
#include "qr_transport.h"
#include <string.h>
int wallet_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s) {
  memset(s, 0, sizeof *s);
  s->testnet = s_sim_testnet != 0;
  s->purpose = s_sim_script == 2 ? 44 : s_sim_script == 1 ? 49 : 84;
  s->n_in = 1; s->n_out = 2;
  s->in_sats = 100000; s->send_sats = 60000; s->change_sats = 39000; s->fee_sats = 1000;
  s->est_vsize = 141; s->fee_rate_x10 = 70; s->rbf = true;
  snprintf(s->outs[0].addr, sizeof s->outs[0].addr,
           "bc1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz");
  s->outs[0].sats = 60000;
  snprintf(s->outs[1].addr, sizeof s->outs[1].addr,
           "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
  s->outs[1].sats = 39000; s->outs[1].is_change = true;
  if (len >= 4 && memmem(bytes, len, "STOP", 4)) {
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "input amount unverifiable");
  } else if (len >= 3 && memmem(bytes, len, "FEE", 3)) {
    // mirrors wallet_psbt.c's high-fee caution so the sim can show it
    s->send_sats = 4000; s->fee_sats = 57000; s->outs[0].sats = 4000;
    s->fee_rate_x10 = 4042;
    s->status = WPSBT_CAUTION;
    s->caution_flags = WPSBT_C_HIGHFEE;
    snprintf(s->reason, sizeof s->reason,
             "unusually high fee - check it before signing");
  } else if (len >= 5 && memmem(bytes, len, "COMBO", 5)) {
    // several cautions at once: proves the summary + WHY card stack up
    s->send_sats = 3000; s->fee_sats = 800; s->change_sats = 200;
    s->outs[0].sats = 3000; s->outs[1].sats = 200; s->in_sats = 4000;
    s->fee_rate_x10 = 570;
    s->status = WPSBT_CAUTION;
    s->caution_flags = WPSBT_C_HIGHFEE | WPSBT_C_DUST_INPUT | WPSBT_C_DUST_CHANGE;
    snprintf(s->reason, sizeof s->reason, "unusually high fee, tiny coins");
  }
  return 0;
}
int wallet_psbt_details(wpsbt_details_t *d) {
  memset(d, 0, sizeof *d);
  d->version = 2; d->locktime = 0; d->txid_final = true; d->n_in = 1; d->n_total = 1;
  snprintf(d->txid, sizeof d->txid, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
  snprintf(d->ins[0].txid, sizeof d->ins[0].txid, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  d->ins[0].vout = 0; d->ins[0].sats = 100000;
  d->ins[0].purpose = 84; d->ins[0].change = 0; d->ins[0].index = 0;
  return 0;
}
int wallet_psbt_sign(uint8_t *out, size_t out_len, size_t *written) {
  size_t n = out_len < 220 ? out_len : 220;
  memset(out, 0xAB, n); *written = n;
  return 0;
}
void wallet_psbt_free(void) {}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  long a = (long)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
  g_flush_px += a; g_flush_n++; if (a > g_flush_max) g_flush_max = a;
  uint16_t *p = (uint16_t *)px;
  for (int y = area->y1; y <= area->y2; y++)
    for (int x = area->x1; x <= area->x2; x++, p++)
      if (x >= 0 && x < HRES && y >= 0 && y < VRES) g_fb[y * HRES + x] = *p;
  lv_display_flush_ready(disp);
}

static void meas_reset(void) { g_flush_px = 0; g_flush_n = 0; g_flush_max = 0; }
static void meas_report(const char *tag, int frames) {
  long per = g_flush_px / (frames > 0 ? frames : 1);
  printf("[render] %-6s %ld px/frame (%.0f%% screen) | %ld flushes (%.1f/frame) | max flush %ld px (%.0f%% screen)\n",
         tag, per, 100.0 * per / (HRES * VRES),
         g_flush_n, (double)g_flush_n / (frames > 0 ? frames : 1),
         g_flush_max, 100.0 * g_flush_max / (HRES * VRES));
}

static void pump(int frames) {
  for (int i = 0; i < frames; i++) { lv_tick_inc(16); lv_timer_handler(); }
}

static void save(const char *path) {
  lv_refr_now(NULL);   // saved frames always reflect every pending invalidation
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", HRES, VRES);
  for (int i = 0; i < HRES * VRES; i++) {
    uint16_t c = g_fb[i];
    unsigned char r = ((c >> 11) & 0x1F) * 255 / 31;
    unsigned char g = ((c >> 5) & 0x3F) * 255 / 63;
    unsigned char b = (c & 0x1F) * 255 / 31;
    fputc(r, f); fputc(g, f); fputc(b, f);
  }
  fclose(f);
  printf("wrote %s\n", path);
}

void sim_home_status(const char *msg);   // main.c (SIMULATOR): bottom-center status slot

static void touch(int x, int y) { g_tx = x; g_ty = y; g_pressed = true; }
static void release(void) { g_pressed = false; }

int main(void) {
  // seed the fake SD card for the step-5 Sign flow
  mkdir("/tmp/simsd", 0777);
  unlink("/tmp/simsd/payment-01-signed.psbt");
  FILE *sd = fopen("/tmp/simsd/payment-01.psbt", "wb");
  if (sd) { fputs("fake-psbt-binary", sd); fclose(sd); }
  sd = fopen("/tmp/simsd/risky-STOP.psbt", "wb");
  if (sd) { fputs("STOP", sd); fclose(sd); }
  // sorts AFTER risky-STOP (the list is qsorted) so the older walks' row taps
  // keep hitting the files they were written for
  unlink("/tmp/simsd/silly-FEE-signed.psbt");
  sd = fopen("/tmp/simsd/silly-FEE.psbt", "wb");
  if (sd) { fputs("FEE", sd); fclose(sd); }
  unlink("/tmp/simsd/warn-COMBO-signed.psbt");
  sd = fopen("/tmp/simsd/warn-COMBO.psbt", "wb");
  if (sd) { fputs("COMBO", sd); fclose(sd); }

  lv_init();
  lv_display_t *d = lv_display_create(HRES, VRES);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  static uint8_t buf[HRES * 60 * 2];
  lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(d, flush_cb);

  build_game();
  pump(20);                                      // ~320ms: logo letters mid-drop
  save("/tmp/sim_menu_intro.ppm");
  pump(110);                                     // letters land, fruit hop in (~2.1s): settled
  save("/tmp/sim_menu.ppm");

  // idle -> attract-mode screensaver; let the fruit drift into view, then capture it
  pump(3760);                                    // ~60s with no touch triggers saver_show()
  pump(240);                                     // ~3.8s for the floating fruit to rise on screen
  save("/tmp/sim_saver.ppm");
  touch(400, 240); pump(2); release(); pump(6);  // a touch wakes the screensaver

  touch(400, 240); pump(2); release(); pump(4);  // tap to start (landscape 800x480)
  pump(14);                                      // a couple fruit airborne

  // swipe across the wide field, slicing to stay alive; measure redraw cost (the lag proxy)
  meas_reset();
  int sf = 0;
  for (int s = 0; s < 6; s++) {
    for (int i = 0; i < 12; i++) { touch(80 + i * 56, 110 + i * 28); pump(1); sf++; }
    release(); pump(1); sf++;
    if (s == 0) save("/tmp/sim_play.ppm");       // gameplay frame
  }
  meas_report("swipe", sf);
  save("/tmp/sim_swipe.ppm");

  // stop slicing -> fruit fall, lives lost -> game over; capture the landscape card
  for (int i = 0; i < 260 && /* until over */ 1; i++) pump(1);
  save("/tmp/sim_over.ppm");

  // baked game-over buttons: PLAY AGAIN restarts, MENU pill -> main menu
  touch(400, 410); pump(3); release(); pump(4);     // PLAY AGAIN -> fresh run
  save("/tmp/sim_playagain.ppm");                   // HUD back: score 0, hearts
  for (int i = 0; i < 700; i++) pump(1);            // fruit fall unsliced -> game over again
  touch(682, 57); pump(3); release(); pump(120);    // MENU pill -> menu (full re-intro settles)
  save("/tmp/sim_menu_back.ppm");

  // draw the word "KISS" -> the hidden wallet appears (K spine+arms, I, S, S)
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);      // K spine
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);  // K upper arm
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);  // K lower arm
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);       // I
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);  // S
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(3);  // S
  save("/tmp/sim_login.ppm");                       // KISS now lands on the passphrase login

  // type "abc" on the QWERTY (kb y0=158, 4 rows ~76px: centers 202/278/354/430)
  touch(46, 278); pump(3); release(); pump(3);      // 'a' (row 2, col 0)
  save("/tmp/sim_login_a.ppm");                     // last char should be flashing
  touch(481, 354); pump(3); release(); pump(3);     // 'b' (row 3)
  touch(306, 354); pump(3); release(); pump(3);     // 'c' (row 3)
  pump(70);                                         // > FLASH_MS: all masked now
  save("/tmp/sim_login_typed.ppm");
  touch(696, 38); pump(3); release(); pump(3);      // SHOW toggle
  save("/tmp/sim_login_shown.ppm");

  // symbol planes: #1! -> type '.' -> #2~ -> type '~' -> abc (entry: "abc.~")
  touch(70, 430); pump(3); release(); pump(3);      // #1! (bottom-left key)
  save("/tmp/sim_login_sym1.ppm");
  touch(488, 353); pump(3); release(); pump(3);     // '.' (plane 1, row 3)
  touch(47, 353); pump(3); release(); pump(3);      // #2~ (plane 1, row 3 col 0)
  save("/tmp/sim_login_sym2.ppm");
  touch(576, 353); pump(3); release(); pump(3);     // '~' (plane 2, row 3)
  touch(70, 430); pump(3); release(); pump(3);      // abc -> letters again
  save("/tmp/sim_login_back.ppm");                  // space/OK ctrls must survive

  // long-press: char keys must NOT repeat, backspace MUST
  touch(46, 278); pump(50); release(); pump(3);     // hold 'a' ~800ms
  save("/tmp/sim_login_hold_a.ppm");                // counter must read 6 characters
  touch(753, 353); pump(80); release(); pump(3);    // hold backspace ~1.3s -> wipes all
  save("/tmp/sim_login_hold_bs.ppm");               // counter must read 0
  for (int i = 0; i < 44; i++) { touch(46, 278); pump(3); release(); pump(3); } // 44 chars
  pump(70);                                         // all masked
  save("/tmp/sim_login_long.ppm");                  // 14pt now, tail visible, no clip
  touch(753, 353); pump(640); release(); pump(3);   // hold backspace: wipe all 44
  touch(46, 278); pump(3); release(); pump(3);      // retype 'a' so OK unlocks non-empty
  touch(467, 430); pump(3); release(); pump(3);     // space -> counter flags it
  save("/tmp/sim_login_space.ppm");                 // "2 characters (1 space)"

  touch(725, 430); pump(3); release(); pump(25);    // OK; let the code card pop-in settle
  save("/tmp/sim_fp.ppm");                          // fingerprint reveal
  touch(400, 414); pump(3); release(); pump(12);    // TAP TO OPEN -> hex noise decrypting
  save("/tmp/sim_fp_scramble.ppm");                 // mid-descramble, center of home screen
  pump(50);                                         // code locks; glide to the chip begins
  save("/tmp/sim_fp_fly.ppm");                      // mid-glide
  pump(70);                                         // landed; chip + caption faded in
  save("/tmp/sim_wallet.ppm");
  pump(90);                                         // ~1.4s idle: motes drift up
  save("/tmp/sim_home_idle.ppm");                   // motes at new positions here
  pump(120);                                        // more drift
  save("/tmp/sim_home_idle2.ppm");                  // motes should have moved further up

  // preview the SD-insert indicator (device polls this; sim just sets the text)
  sim_home_status("SD card ready");
  lv_obj_invalidate(lv_screen_active()); lv_refr_now(NULL); pump(4);
  save("/tmp/sim_sd_ready.ppm");
  sim_home_status(""); lv_refr_now(NULL); pump(2);   // clear so later frames are unaffected

  // step 4: Receive (address #0 QR, next -> #1) and watch-only Export
  touch(310, 240); pump(3);
  save("/tmp/sim_tile_press.ppm");                  // glow under the held tile
  release(); pump(6);                               // Receive tile
  save("/tmp/sim_recv.ppm");
  touch(553, 422); pump(3); release(); pump(4);     // NEXT -> address #1
  save("/tmp/sim_recv1.ppm");
  {  // VERIFY: uppercase bitcoin: URI of stub receive addr #7 -> YOURS; junk -> NOT
    touch(697, 422); pump(3); release(); pump(6);   // VERIFY -> raw scan screen
    const char *good = "BITCOIN:BC1QCR8TE4KR609GCAWUTMRZA0J4XV80JY8Z3Q07?amount=0.001";
    wallet_scan_inject(good, strlen(good)); pump(6);
    save("/tmp/sim_vfy_yes.ppm");
    touch(158, 430); pump(3); release(); pump(6);   // SCAN ANOTHER
    const char *bad = "bc1qnotmineatallnotmineatallnotmine00";
    wallet_scan_inject(bad, strlen(bad)); pump(6);
    save("/tmp/sim_vfy_no.ppm");
    touch(680, 430); pump(3); release(); pump(6);   // DONE -> Receive
  }
  touch(118, 430); pump(3); release(); pump(4);     // BACK -> home
  touch(680, 60); pump(3); release(); pump(14);     // fingerprint chip -> education card
  save("/tmp/sim_home_fp.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  touch(490, 240); pump(3); release(); pump(6);     // Wallet tile -> section home
  save("/tmp/sim_winfo.ppm");
  touch(211, 109); pump(3); release(); pump(14);    // "?" chip (fingerprint) -> card
  save("/tmp/sim_winfo_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  touch(590, 130); pump(3); release(); pump(6);     // PAIR COORDINATOR
  save("/tmp/sim_pair.ppm");                        // descriptor (Sparrow) active
  touch(672, 150); pump(3); release(); pump(4);     // MOBILE / BlueWallet segment
  save("/tmp/sim_pair_bw.ppm");
  touch(541, 105); pump(3); release(); pump(14);    // "?" chip -> coordinator card
  save("/tmp/sim_pair_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> section home
  touch(590, 278); pump(3); release(); pump(6);     // BACKUP WORDS -> warning
  save("/tmp/sim_words_warn.ppm");
  touch(188, 430); pump(3); release(); pump(6);     // SHOW THE WORDS
  save("/tmp/sim_words.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // DONE -> section home
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  save("/tmp/sim_home_end.ppm");

  // step 5: Sign via SD — chooser, file list, verify, hold-to-sign, signed, STOP
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> QR/SD chooser
  save("/tmp/sim_sign_choose.ppm");
  touch(718, 170); pump(3); release(); pump(6);     // "?" chip -> coordinator card
  save("/tmp/sim_sign_help.ppm");
  touch(400, 366); pump(3); release(); pump(6);     // OK closes the card
  touch(218, 256); pump(3); release(); pump(6);     // FROM SD CARD -> file list
  save("/tmp/sim_sign_files.ppm");
  touch(328, 136); pump(3); release(); pump(8);     // first file -> verify (READY)
  save("/tmp/sim_sign_verify.ppm");
  touch(293, 430); pump(3); release(); pump(6);     // DETAILS -> raw facts page
  save("/tmp/sim_sign_details.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> verify again
  touch(626, 430); pump(40);                        // hold the sign pill: ring ~half full
  save("/tmp/sim_sign_hold.ppm");
  pump(45);                                         // past 1.2s: signs + writes SD
  release(); pump(8);
  save("/tmp/sim_sign_done.ppm");
  touch(400, 430); pump(3); release(); pump(6);     // DONE -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 256); pump(3); release(); pump(6);     // FROM SD CARD
  touch(328, 202); pump(3); release(); pump(8);     // the STOP file -> blocked verify
  save("/tmp/sim_sign_stop.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 256); pump(3); release(); pump(6);     // FROM SD CARD
  touch(328, 268); pump(3); release(); pump(8);     // the FEE file -> amber caution
  save("/tmp/sim_sign_fee.ppm");                    // summary + "I UNDERSTAND" gate
  touch(626, 430); pump(3); release(); pump(6);     // I UNDERSTAND -> hold pill revealed
  save("/tmp/sim_sign_fee_ack.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 256); pump(3); release(); pump(6);     // FROM SD CARD
  touch(328, 334); pump(3); release(); pump(8);     // COMBO file -> stacked cautions
  save("/tmp/sim_sign_combo.ppm");
  touch(735, 361); pump(3); release(); pump(6);     // "?" -> WHY FLAGGED card
  save("/tmp/sim_sign_why.ppm");
  touch(400, 438); pump(3); release(); pump(6);     // OK closes the card
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> home

  // step 6: Sign via QR — scan (real UR fountain parts injected as if the
  // camera decoded them), verify, sign, animated UR out
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> chooser
  touch(218, 176); pump(3); release(); pump(6);     // SCAN QR -> scan screen
  save("/tmp/sim_qr_scan.ppm");
  {
    uint8_t fake[300];
    memset(fake, 0x5A, sizeof fake);
    memcpy(fake, "psbt\xff", 5);
    qrt_encoder_t *enc = qrt_encoder_new(QRT_FMT_UR, fake, sizeof fake);
    char part[600];
    if (enc && qrt_encoder_next(enc, part, sizeof part) == 0) {
      wallet_scan_inject(part, strlen(part));       // one part in: progress shows
      pump(3);
    }
    save("/tmp/sim_qr_scan_part.ppm");
    for (int i = 0; i < 32 && wallet_scan_active(); i++) {
      if (enc && qrt_encoder_next(enc, part, sizeof part) == 0)
        wallet_scan_inject(part, strlen(part));
      pump(2);
    }
    qrt_encoder_free(enc);
  }
  pump(8);
  save("/tmp/sim_qr_verify.ppm");                   // verify screen, source = scan
  touch(626, 430); pump(40);                        // hold to sign
  pump(45); release(); pump(8);
  save("/tmp/sim_qr_out1.ppm");                     // animated UR out, first part
  pump(20);                                         // ~320ms: 250ms timer advanced
  save("/tmp/sim_qr_out2.ppm");                     // ...a different part
  touch(530, 270); pump(3); release(); pump(6);     // EASY SCAN: sparser, slower QR
  save("/tmp/sim_qr_out_ez.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // DONE -> home
  save("/tmp/sim_qr_end.ppm");

  // reuse guard: those signs spent from receive #0, so Receive now lands past
  // it; paging back to a used index warns and offers FRESH.
  touch(310, 240); pump(3); release(); pump(6);     // Receive tile
  save("/tmp/sim_recv_fresh.ppm");                  // advanced past used, no warning
  touch(436, 422); pump(3); release(); pump(4);     // PREV
  touch(436, 422); pump(3); release(); pump(4);     // PREV
  touch(436, 422); pump(3); release(); pump(4);     // PREV -> down onto a used index
  save("/tmp/sim_recv_reuse.ppm");                  // amber warning + FRESH pill
  touch(698, 266); pump(3); release(); pump(4);     // FRESH -> jump back to a new one
  save("/tmp/sim_recv_fresh2.ppm");                 // warning gone again
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> home

  // settings: address-type chooser (all 3 visible, active highlighted) + the
  // TESTNET home badge; verify Receive/verify reflect testnet, then restore.
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile
  save("/tmp/sim_settings.ppm");                    // mainnet, NATIVE highlighted
  touch(333, 310); pump(3); release(); pump(4);     // pick LEGACY -> highlight moves
  save("/tmp/sim_settings_legacy.ppm");
  touch(103, 310); pump(3); release(); pump(4);     // back to NATIVE
  touch(674, 48); pump(3); release(); pump(4);      // theme dot: CYPHERPINK
  save("/tmp/sim_settings_pink.ppm");               // accent recolors selections+title
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home still pink
  save("/tmp/sim_wallet_pink.ppm");
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(578, 48); pump(3); release(); pump(4);      // theme dot: back to MONO
  touch(218, 176); pump(3); release(); pump(4);     // TESTNET pill (center y=176)
  save("/tmp/sim_settings_tn.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  save("/tmp/sim_wallet_testnet.ppm");              // home now shows TESTNET badge
  touch(310, 240); pump(3); release(); pump(6);     // Receive: tb1 address now
  save("/tmp/sim_recv_tn.ppm");
  touch(118, 430); pump(3); release(); pump(4);     // BACK
  touch(130, 240); pump(3); release(); pump(6);     // Sign -> chooser
  touch(218, 256); pump(3); release(); pump(6);     // FROM SD
  touch(328, 136); pump(3); release(); pump(8);     // file -> verify: TESTNET row
  save("/tmp/sim_verify_tn.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(218, 126); pump(3); release(); pump(4);     // MAINNET restore (center y=126)
  touch(680, 430); pump(3); release(); pump(4);     // BACK -> home

  // step 7: seed wizard — lock, wipe the seed, KISS again -> first-boot flow
  touch(100, 60); pump(3); release(); pump(20);     // KISS logo -> lock -> menu
  wallet_seed_wipe();                               // pretend a factory-fresh device
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(4);
  save("/tmp/sim_setup_choose.ppm");                // NEW / RESTORE chooser

  // peek at RESTORE: word entry + autocomplete, then back out
  touch(218, 256); pump(3); release(); pump(4);     // RESTORE FROM WORDS
  touch(218, 176); pump(3); release(); pump(4);     // 12 WORDS
  save("/tmp/sim_setup_restore.ppm");
  touch(44, 314); pump(3); release(); pump(3);      // 'a'
  touch(450, 374); pump(3); release(); pump(3);     // 'b'
  save("/tmp/sim_setup_sug.ppm");                   // suggestions visible
  touch(163, 182); pump(3); release(); pump(3);     // accept "abandon" -> word 2
  touch(160, 434); pump(3); release(); pump(4);     // CANCEL -> chooser

  // the real path: CREATE NEW, 12 words, simulated entropy, quiz, login twice
  touch(218, 176); pump(3); release(); pump(4);     // CREATE NEW
  touch(218, 176); pump(3); release(); pump(4);     // 12 WORDS
  save("/tmp/sim_setup_entropy.ppm");
  touch(168, 430); pump(3); release(); pump(4);     // CAPTURE (simulated)
  save("/tmp/sim_setup_words.ppm");                 // the write-these-down grid
  touch(590, 430); pump(3); release(); pump(4);     // I WROTE THEM DOWN
  save("/tmp/sim_setup_quiz.ppm");
  touch(218, 226); pump(3); release(); pump(4);     // round 1: pill 0 correct
  touch(598, 226); pump(3); release(); pump(4);     // round 2: pill 1
  touch(218, 306); pump(3); release(); pump(6);     // round 3: pill 2 -> stored
  save("/tmp/sim_setup_ppintro.ppm");               // ONE MORE LAYER (what a passphrase is)
  touch(188, 430); pump(3); release(); pump(6);     // CREATE PASSPHRASE -> keyboard
  save("/tmp/sim_setup_pass.ppm");                  // CREATE YOUR PASSPHRASE
  // CANCEL during setup must confirm (don't throw away a fresh seed on one tap)
  touch(200, 430); pump(3); release(); pump(6);     // CANCEL -> confirm modal
  save("/tmp/sim_setup_cancel.ppm");
  touch(264, 323); pump(3); release(); pump(4);     // KEEP GOING -> back to keyboard
  touch(46, 278); pump(3); release(); pump(3);      // 'a' (deliberately weak)
  touch(725, 430); pump(3); release(); pump(4);     // OK -> weak warning
  save("/tmp/sim_setup_weak.ppm");                  // WEAK PASSWORD - OK AGAIN
  touch(725, 430); pump(3); release(); pump(4);     // OK again -> confirm stage
  save("/tmp/sim_setup_pass2.ppm");                 // TYPE IT AGAIN
  touch(46, 278); pump(3); release(); pump(3);      // 'a' again
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  touch(400, 414); pump(3); release(); pump(8);     // TAP TO OPEN -> passphrase warning
  lv_refr_now(NULL); pump(2);
  save("/tmp/sim_setup_warn.ppm");                  // "YOUR PASSWORD IS PART OF THE WALLET"
  touch(400, 416); pump(3); release(); pump(140);   // I UNDERSTAND -> home settles
  save("/tmp/sim_setup_home.ppm");

  // step 8: idle auto-lock — 2min untouched on the home must close the session
  // and land back on the game menu (7700 frames x 16ms > 120s + intro settle)
  pump(7700);
  save("/tmp/sim_autolock.ppm");                    // must be the game MENU again

  // unlock again (wizard wallet, password 'a') for the wipe preview
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(3);
  touch(46, 278); pump(3); release(); pump(3);      // 'a'
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  touch(400, 414); pump(3); release(); pump(140);   // TAP TO OPEN -> home

  // step 9: WIPE WALLET — arm (red), confirm, ERASED screen, OK -> game menu
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile
  touch(590, 346); pump(4); release(); pump(8);     // WIPE WALLET -> armed
  lv_refr_now(NULL); pump(2);                       // (sim: force the restyle flush)
  save("/tmp/sim_wipe_arm.ppm");                    // "TAP AGAIN TO WIPE" in red
  touch(590, 346); pump(3); release(); pump(6);     // second tap -> seed erased
  save("/tmp/sim_wiped.ppm");                       // WALLET ERASED confirmation
  touch(400, 366); pump(3); release(); pump(130);   // OK -> locked to game menu
  save("/tmp/sim_wiped_menu.ppm");                  // must be the game MENU

  printf("sim done\n");
  return 0;
}
