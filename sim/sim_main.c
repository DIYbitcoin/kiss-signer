// Headless LVGL simulator for the FRUIT ISLAND game.
// Compiles the REAL game code (main/main.c with -DSIMULATOR) against desktop
// LVGL, renders into an in-memory RGB565 framebuffer, scripts touch input, and
// dumps frames to .ppm so the actual rendering can be inspected without hardware.
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_info.h"
#include "wallet_settings.h"

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
static int s_sim_mode;
static int s_sim_pending_mode = -1;   // staged wizard answer, -1 = none
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
  if (s_sim_pending_mode >= 0) s_sim_mode = s_sim_pending_mode;
  s_sim_pending_mode = -1;
  if (s_sim_mode == WSEED_MODE_AMNESIC) {
    s_sim_has_seed = 0;          // the old stored wallet goes WITH the commit
    return 0;                    // the new words stay in RAM until the lock
  }
  snprintf(s_sim_seed, sizeof s_sim_seed, "%s", s_sim_pending);
  s_sim_has_seed = 1;
  s_sim_has_pending = 0;
  return 0;
}
void wallet_seed_discard(void) { s_sim_has_pending = 0; s_sim_pending_mode = -1; }
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
// storage mode: the real logic + its edge cases live in wallet_seed.c and are
// covered by kisstest. Here it only has to steer the screens -- but it has to
// steer them the same way, so the staged-vs-applied split is mirrored: the
// wizard stages, commit applies, and only an explicit set_mode erases now.
int wallet_seed_mode(void) {
  return s_sim_pending_mode >= 0 ? s_sim_pending_mode : s_sim_mode;
}
void wallet_seed_stage_mode(int m) {
  s_sim_pending_mode = m == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC
                                               : WSEED_MODE_KEEP;
}
int wallet_seed_set_mode(int m) {
  s_sim_pending_mode = -1;
  s_sim_mode = m == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC : WSEED_MODE_KEEP;
  if (s_sim_mode == WSEED_MODE_AMNESIC) s_sim_has_seed = 0;
  return 0;
}
void wallet_seed_forget(void) {
  if (s_sim_mode == WSEED_MODE_AMNESIC) s_sim_has_pending = 0;
}
// Enough of the real parser to drive the walk: the numeric SeedQR shape and a
// plain mnemonic are accepted, anything else is the "NOT A SEED" path.
int wallet_seed_from_qr(const char *data, size_t len, char *out, size_t n) {
  if (out && n) out[0] = 0;
  if (!data || !out || len == 0) return -1;
  if (len == 48 || len == 96) {
    for (size_t i = 0; i < len; i++)
      if (data[i] < '0' || data[i] > '9') goto text;
    size_t o = 0;
    for (size_t w = 0; w < len / 4 && o + 12 < n; w++)
      o += (size_t)snprintf(out + o, n - o, "%s%s", w ? " " : "", SIM_WORDS[w % 24]);
    return 0;
  }
text:
  {   // a mnemonic is 12 or 24 words; anything else is the NOT A SEED path
    int words = 1;
    for (size_t i = 0; i < len; i++) if (data[i] == ' ') words++;
    if ((words == 12 || words == 24) && len + 1 <= n) {
      snprintf(out, n, "%.*s", (int)len, data);
      return 0;
    }
  }
  return -1;
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
// mirrors main/wallet_seed.c (the shipped, unit-tested one)
int wallet_seed_diff_word(const char *typed, const char *stored) {
  const char *a = typed, *b = stored;
  while (*a == ' ') a++; while (*b == ' ') b++;
  for (int idx = 0;; idx++) {
    const char *ae = a; while (*ae && *ae != ' ') ae++;
    const char *be = b; while (*be && *be != ' ') be++;
    size_t al = (size_t)(ae - a), bl = (size_t)(be - b);
    if (al == 0 && bl == 0) return -1;
    if (al != bl || strncmp(a, b, al) != 0) return idx;
    a = ae; b = be; while (*a == ' ') a++; while (*b == ' ') b++;
  }
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
static int s_sim_decoy;
int wallet_session_open(const char *passphrase) {
  s_sim_decoy = !(passphrase && passphrase[0]);   // empty passphrase = the decoy signer
  return 0;
}
void wallet_session_close(void) { s_sim_decoy = 0; }
int wallet_session_decoy(void) { return s_sim_decoy; }
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
int wallet_address_validate(const char *addr) {
  if (!addr || !*addr) return WADDR_INVALID;
  size_t alen = strlen(addr);
  int base58_len = alen >= 26 && alen <= 35;
  int test_addr = strncmp(addr, "tb1", 3) == 0 || strncmp(addr, "tsp1", 4) == 0
               || (base58_len && (addr[0] == 'm' || addr[0] == 'n' || addr[0] == '2'));
  int main_addr = strncmp(addr, "bc1", 3) == 0 || strncmp(addr, "sp1", 3) == 0
               || (base58_len && (addr[0] == '1' || addr[0] == '3'));
  if (!test_addr && !main_addr) return WADDR_INVALID;
  return (test_addr == !!s_sim_testnet) ? WADDR_CURRENT_NETWORK
                                        : WADDR_WRONG_NETWORK;
}
int wallet_session_sp_address(char *out, unsigned long len) {
  snprintf(out, len, "%s", s_sim_testnet
    ? "tsp1qqdpels3srq45dlezqvk20t3dlueftry6p5thc7msjm0s6jm3g84jzq5rxzzunfck6d45va2jcqxk429agt3e4klf3vzmcgp3zqthryhhqgnz4k3n"
    : "sp1qqfqnnv8czppwysafq3uwgwvsc638hc8rx3hscuddh0xa2yd746s7xqh6yy9ncjnqhqxazct0fzh98w7lpkm5fvlepqec2yy0sxlq4j6ccc3h6t0g");
  return 0;
}
// Stage B scan-key export: the real strings kisstest pins against embit
// (sp_test_scan_export), so the sim lays out exactly what the device shows.
int wallet_session_sp_scan_export(char *out, unsigned long len) {
  snprintf(out, len, "%s", s_sim_testnet
    ? "sp([73c5da0a/352h/1h/0h]tspscan1q8pjcdy7qzlzxl44chw2tsanvzg7dtwhkqf3nsvzmdavls2ekl8qq9qesshy6w9knddr825kqp442302zuwddh6vtqk7zqvgszace9aczgnuqn3)"
    : "sp([73c5da0a/352h/0h/0h]spscan1q0rnl6lft0gkpg4nsn528qgdpytfdej40atdqgrxpqqsg8c5r8vys973ppv7y5c9cphgkzm6g4efmhhcdkazt87ggxwz3pruphc9vkkxxhtvyag)");
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
  } else if (len >= 4 && memmem(bytes, len, "SPAY", 4)) {
    // BIP375 silent payment send: out0 is the tsp1 destination the signer
    // derived + verified on-device (renders badge + note + ~117-char address)
    s->n_sp = 1;
    s->outs[0].is_sp = true;
    s->outs[0].sats = 60000;
    snprintf(s->outs[0].addr, sizeof s->outs[0].addr,
             "tsp1qqfaysl7pn7mknpmmsapdd6sczx8ncnnjk84gcm0xq2n66jjpm0sxsqmpuxc7nhj7gt9jqplhef2tncx40mgnjw8664kn7x09w5f63l8q8ymd0lna");
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
  // n_total > n_in exercises the many-inputs header (S_D_MANYIN_FMT) with a
  // 2-digit count: the longest formatted line in the whole sign flow (ja is
  // ~140 bytes) and the exact case that used to truncate in buf[128]
  d->version = 2; d->locktime = 0; d->txid_final = true; d->n_in = 1; d->n_total = 17;
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

// SIM_LANG=<code> (en, de, es-MX, ...) renders the whole walk in that language
// and prefixes every frame: /tmp/sim_de_login.ppm etc. NVS is stubbed in the
// sim, so the env var is the only language input.
static const char *g_lang_code;

static void save(const char *path) {
  char lp[160];
  if (g_lang_code && strncmp(path, "/tmp/sim_", 9) == 0) {
    snprintf(lp, sizeof lp, "/tmp/sim_%s_%s", g_lang_code, path + 9);
    path = lp;
  }
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

// Type a short prefix on wallet_setup.c's recovery-word keyboard, then choose
// its first suggestion.  Keeping this as a real touch walk means the optional
// recovery rehearsal is tested through the exact UI a person uses.
static void restore_word(const char *prefix)
{
  static const char *rows[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
  static const int x0[] = { 44, 47, 53 };
  static const int dx[] = { 79, 88, 99 };
  static const int yy[] = { 254, 314, 374 };
  for (const char *p = prefix; *p; p++) {
    for (int r = 0; r < 3; r++) {
      const char *at = strchr(rows[r], *p);
      if (!at) continue;
      touch(x0[r] + (int)(at - rows[r]) * dx[r], yy[r]);
      pump(3); release(); pump(3);
      break;
    }
  }
  touch(163, 182); pump(3); release(); pump(3);    // first suggestion
}

int main(void) {
  const char *sl = getenv("SIM_LANG");
  if (sl && *sl && strcmp(sl, "en") != 0) {
    for (int i = 0; i < I18N_LANG_N; i++)
      if (strcmp(i18n_lang_info(i)->code, sl) == 0) {
        i18n_set_lang(i);
        g_lang_code = sl;
      }
    if (!g_lang_code) { fprintf(stderr, "unknown SIM_LANG %s\n", sl); return 1; }
  }

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
  // sorts LAST (after warn-COMBO) so existing sign-walk row taps stay put
  unlink("/tmp/simsd/zsp-SPAY-signed.psbt");
  sd = fopen("/tmp/simsd/zsp-SPAY.psbt", "wb");
  if (sd) { fputs("SPAY", sd); fclose(sd); }

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
  touch(295, 426); pump(3); release(); pump(6);     // Silent payment -> SP address view
  save("/tmp/sim_recv_sp.ppm");
  touch(730, 50); pump(3); release(); pump(30);     // ? -> sp1/bc1p explanation
  save("/tmp/sim_recv_sp_help.ppm");
  touch(400, 418); pump(3); release(); pump(6);     // OK closes the explanation
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> Receive
  touch(529, 430); pump(3); release(); pump(4);     // NEXT -> address #1
  save("/tmp/sim_recv1.ppm");
  {  // VERIFY: own, valid-but-not-found, wrong-network, invalid, then own SP.
    touch(678, 430); pump(3); release(); pump(6);   // VERIFY -> raw scan screen
    const char *good = "BITCOIN:BC1QCR8TE4KR609GCAWUTMRZA0J4XV80JY8Z3Q07?amount=0.001";
    wallet_scan_inject(good, strlen(good)); pump(6);
    save("/tmp/sim_vfy_yes.ppm");
    touch(158, 430); pump(3); release(); pump(6);   // SCAN ANOTHER
    const char *bad = "bc1qnotmineatallnotmineatallnotmine00";
    wallet_scan_inject(bad, strlen(bad)); pump(6);
    save("/tmp/sim_vfy_no.ppm");
    touch(158, 430); pump(3); release(); pump(6);   // SCAN ANOTHER
    const char *wrong_net = "tb1qwrongnetworkwrongnetworkwrongnetwork00";
    wallet_scan_inject(wrong_net, strlen(wrong_net)); pump(6);
    save("/tmp/sim_vfy_wrong_net.ppm");
    touch(158, 430); pump(3); release(); pump(6);   // SCAN ANOTHER
    const char *invalid = "not-an-address";
    wallet_scan_inject(invalid, strlen(invalid)); pump(6);
    save("/tmp/sim_vfy_invalid.ppm");
    touch(158, 430); pump(3); release(); pump(6);   // SCAN ANOTHER
    // our OWN silent-payment address: 117 chars, longer than any bc1/tb1
    const char *sp = "sp1qqfqnnv8czppwysafq3uwgwvsc638hc8rx3hscuddh0xa2yd746s7xq"
                     "h6yy9ncjnqhqxazct0fzh98w7lpkm5fvlepqec2yy0sxlq4j6ccc3h6t0g";
    wallet_scan_inject(sp, strlen(sp)); pump(6);
    save("/tmp/sim_vfy_sp.ppm");
    touch(680, 430); pump(3); release(); pump(6);   // DONE -> Receive
  }
  touch(118, 430); pump(3); release(); pump(4);     // BACK -> home
  touch(680, 60); pump(3); release(); pump(30);     // fingerprint chip -> education card
  save("/tmp/sim_home_fp.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  touch(490, 240); pump(3); release(); pump(6);     // Wallet tile -> section home
  save("/tmp/sim_winfo.ppm");
  wallet_info_sim_open_fp_help(); pump(30);         // deterministic: see the type chip below
  save("/tmp/sim_winfo_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  wallet_info_sim_open_type_help(); pump(30);       // deterministic: chip x varies by locale
  save("/tmp/sim_winfo_type_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the type card
  touch(590, 130); pump(3); release(); pump(6);     // PAIR COORDINATOR
  save("/tmp/sim_pair.ppm");                        // descriptor (Sparrow) active
  touch(672, 150); pump(3); release(); pump(4);     // MOBILE / BlueWallet segment
  save("/tmp/sim_pair_bw.ppm");
  touch(541, 105); pump(3); release(); pump(30);    // "?" chip -> coordinator card
  save("/tmp/sim_pair_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  // Page two: the import steps plus the address proof. It is the page the
  // owner actually follows, so it gets walked and rendered like any other.
  touch(680, 430); pump(3); release(); pump(6);     // NEXT -> HOW TO PAIR
  save("/tmp/sim_pair_steps.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the QR page
  // SCAN KEY is no longer buried in the pair screen: it is a top-level pill on
  // the WALLET screen at (430,236,340,60), so back out of pairing first. It was
  // moved because hiding a separate PRIVATE-key export one tap inside the
  // descriptor flow implied the two were the same action.
  touch(118, 430); pump(3); release(); pump(6);     // BACK (x=48 pill) -> WALLET
  touch(600, 266); pump(3); release(); pump(6);     // SCAN KEY -> consent warning
  save("/tmp/sim_sp_warn.ppm");
  touch(198, 430); pump(3); release(); pump(6);     // SHOW THE SCAN KEY -> export
  save("/tmp/sim_sp_key.ppm");
  touch(128, 430); pump(3); release(); pump(6);     // DONE -> WALLET screen
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> section home
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> section home
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  save("/tmp/sim_home_end.ppm");

  // step 5: Sign via SD — chooser, file list, verify, hold-to-sign, signed, STOP
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> QR/SD chooser
  save("/tmp/sim_sign_choose.ppm");
  touch(702, 82); pump(3); release(); pump(30);     // "PSBT ?" -> signing explainer
  save("/tmp/sim_sign_help.ppm");
  touch(400, 430); pump(3); release(); pump(6);     // OK closes the card
  touch(218, 298); pump(3); release(); pump(6);     // FROM SD CARD (y=268) -> file list
  save("/tmp/sim_sign_files.ppm");
  touch(328, 150); pump(3); release(); pump(8);     // first file -> verify (READY)
  save("/tmp/sim_sign_verify.ppm");
  // the RBF "?" is wallet_sign.c's 26px chip, now pinned at (740,358) for BOTH
  // the replaceable and final wordings. It used to sit right after the text at
  // 592 or 620 depending on which string rendered, so its x moved with the
  // translation; it is fixed now, and far enough from the caution chip at
  // (430,330) that neither lands in the other's hit box. This is its centre.
  touch(753, 371); pump(3); release(); pump(8);     // RBF "?" -> explainer (mid-intro)
  save("/tmp/sim_sign_rbf_mid.ppm");
  pump(30);                                          // let the stagger settle
  save("/tmp/sim_sign_rbf.ppm");
  touch(400, 426); pump(3); release(); pump(6);     // OK closes the card
  touch(293, 430); pump(3); release(); pump(6);     // DETAILS -> raw facts page
  save("/tmp/sim_sign_details.ppm");
  touch(656, 50); pump(3); release(); pump(30);     // SIMPLE EXPLAINERS
  save("/tmp/sim_sign_glossary.ppm");
  touch(400, 430); pump(3); release(); pump(6);     // OK closes glossary
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> verify again
  touch(626, 430); pump(40);                        // hold the sign pill: ring ~half full
  save("/tmp/sim_sign_hold.ppm");
  pump(45);                                         // past 1.2s: signs + writes SD
  release(); pump(8);
  save("/tmp/sim_sign_done.ppm");
  touch(400, 430); pump(3); release(); pump(6);     // DONE -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 298); pump(3); release(); pump(6);     // FROM SD CARD
  touch(328, 216); pump(3); release(); pump(8);     // the STOP file -> blocked verify
  save("/tmp/sim_sign_stop.ppm");
  // BACK is ONE STEP now: from a transaction it returns to the list that
  // transaction came from, not to the home screen. The three files below are
  // opened one after another without ever leaving SIGN, which is the whole
  // point -- picking the wrong file used to cost the entire trip back in.
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the file list
  save("/tmp/sim_sign_back_files.ppm");
  touch(328, 282); pump(3); release(); pump(8);     // the FEE file -> amber caution
  save("/tmp/sim_sign_fee.ppm");                    // summary + "I UNDERSTAND" gate
  touch(524, 430); pump(3); release(); pump(6);     // I UNDERSTAND (398..650) -> hold pill
  save("/tmp/sim_sign_fee_ack.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the file list
  touch(328, 348); pump(3); release(); pump(8);     // COMBO file -> stacked cautions
  save("/tmp/sim_sign_combo.ppm");
  touch(735, 361); pump(3); release(); pump(6);     // "?" -> WHY FLAGGED card
  save("/tmp/sim_sign_why.ppm");
  touch(400, 438); pump(3); release(); pump(6);     // OK closes the card
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the SCAN/SD chooser
  save("/tmp/sim_sign_back_choose.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  // silent payment send: out0 renders as a tsp1 address with the SP badge+note.
  // the list shows only the first 4 files, so clear the others (all frames above
  // are already saved) to leave zsp-SPAY alone in row 0.
  unlink("/tmp/simsd/payment-01.psbt");   unlink("/tmp/simsd/payment-01-signed.psbt");
  unlink("/tmp/simsd/risky-STOP.psbt");
  unlink("/tmp/simsd/silly-FEE.psbt");    unlink("/tmp/simsd/silly-FEE-signed.psbt");
  unlink("/tmp/simsd/warn-COMBO.psbt");
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 298); pump(3); release(); pump(6);     // FROM SD CARD -> list (only SPAY)
  touch(328, 150); pump(3); release(); pump(8);     // zsp-SPAY (row 0) -> SP verify
  save("/tmp/sim_sign_sp.ppm");                      // SP output row: badge + address + note
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the chooser
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home

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
  touch(426, 430); pump(3); release(); pump(4);     // PREV
  touch(426, 430); pump(3); release(); pump(4);     // PREV
  touch(426, 430); pump(3); release(); pump(4);     // PREV (row y=404) -> a used index
  save("/tmp/sim_recv_reuse.ppm");                  // amber warning + FRESH pill
  touch(698, 266); pump(3); release(); pump(4);     // FRESH -> jump back to a new one
  save("/tmp/sim_recv_fresh2.ppm");                 // warning gone again
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> home

  // settings: address-type chooser (all 3 visible, active highlighted) + the
  // TESTNET home badge; verify Receive/verify reflect testnet, then restore.
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile
  save("/tmp/sim_settings.ppm");                    // mainnet, NATIVE highlighted

  // RECOVERY WORDS now belongs to Settings. Verify the paper copy, return to
  // Settings, then separately exercise the sensitive word reveal.
  touch(600, 216); pump(3); release(); pump(6);     // RECOVERY WORDS -> warning
  save("/tmp/sim_words_warn.ppm");                  // SHOW / VERIFY MY COPY / BACK
  // VERIFY MY COPY: type the stored dev mnemonic (11x abandon + about).
  // 'abandon' = 'a','b' -> suggestion[0]; 'about' = 'a','b','o' -> suggestion[0].
  touch(420, 430); pump(3); release(); pump(6);     // VERIFY MY COPY -> intro
  save("/tmp/sim_verify_intro.ppm");
  touch(198, 430); pump(3); release(); pump(6);     // TYPE MY WORDS -> keypad
  save("/tmp/sim_verify_entry.ppm");
  for (int i = 0; i < 12; i++) {                    // all 'abandon' -> word 12 wrong
    touch(44, 314); pump(3); release(); pump(3);    // a
    touch(450, 374); pump(3); release(); pump(3);   // b -> "ab"
    touch(163, 182); pump(3); release(); pump(3);   // accept "abandon"
  }
  pump(4);
  save("/tmp/sim_verify_mismatch.ppm");             // "word #12 does not match"
  touch(198, 430); pump(3); release(); pump(6);     // TYPE AGAIN -> keypad
  for (int i = 0; i < 11; i++) {                    // 11x abandon
    touch(44, 314); pump(3); release(); pump(3);
    touch(450, 374); pump(3); release(); pump(3);
    touch(163, 182); pump(3); release(); pump(3);
  }
  touch(44, 314); pump(3); release(); pump(3);      // a
  touch(450, 374); pump(3); release(); pump(3);     // b
  touch(664, 254); pump(3); release(); pump(3);     // o -> "abo"
  touch(163, 182); pump(3); release(); pump(4);     // accept "about" -> VERIFIED
  save("/tmp/sim_verify_ok.ppm");
  touch(198, 430); pump(3); release(); pump(6);     // DONE -> Settings

  touch(600, 216); pump(3); release(); pump(6);     // RECOVERY WORDS -> warning again
  touch(168, 430); pump(3); release(); pump(6);     // SHOW THE WORDS
  save("/tmp/sim_words.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // DONE -> Settings

  // A 24-word seed is the only case that paginates, and 12-word wallets are
  // what the rest of this walk uses -- so swap the stored mnemonic directly
  // (same file, same statics) rather than typing 24 words through the keypad.
  {
    char save_seed[sizeof s_sim_seed];
    snprintf(save_seed, sizeof save_seed, "%s", s_sim_seed);
    size_t o = 0;
    for (int i = 0; i < 24; i++)
      o += (size_t)snprintf(s_sim_seed + o, sizeof s_sim_seed - o,
                            "%s%s", i ? " " : "", SIM_WORDS[i]);
    touch(600, 216); pump(3); release(); pump(6);   // RECOVERY WORDS -> warning
    touch(168, 430); pump(3); release(); pump(6);   // SHOW THE WORDS
    save("/tmp/sim_words24_p1.ppm");                // 1-12 / 24, NEXT but no BACK
    touch(278, 430); pump(3); release(); pump(6);   // NEXT
    save("/tmp/sim_words24_p2.ppm");                // 13-24 / 24, BACK but no NEXT
    touch(118, 430); pump(3); release(); pump(6);   // BACK -> page 1 again
    save("/tmp/sim_words24_back.ppm");
    touch(680, 430); pump(3); release(); pump(6);   // DONE -> Settings
    snprintf(s_sim_seed, sizeof s_sim_seed, "%s", save_seed);
  }

  touch(510, 424); pump(3); release(); pump(6);     // language pill (y=398) -> picker
  save("/tmp/sim_lang_picker.ppm");                 // 21 locale choices, current selected
  {                                                 // re-pick the ACTIVE language so a
    int li = wallet_lang_pick_slot(i18n_get_lang()); // SIM_LANG walk stays in its locale
    touch(16 + (li % 3) * 260 + 124, 76 + (li / 3) * 52 + 22);
    pump(3); release(); pump(10);                   // settings rebuilt, same language
  }
  // ADDRESS TYPE is a full-width subpage now: the settings row opens it, and
  // the pick happens there. Walking both halves keeps a broken chooser from
  // hiding behind a frame that only ever showed the list.
  touch(218, 306); pump(3); release(); pump(6);     // ADDRESS TYPE row -> chooser
  save("/tmp/sim_addr_type.ppm");                   // 3 names + notes, NATIVE selected
  touch(400, 122); pump(3); release(); pump(6);     // pick LEGACY -> back to settings
  save("/tmp/sim_settings_legacy.ppm");             // the row now reads Legacy / 1...
  touch(218, 306); pump(3); release(); pump(6);     // reopen the chooser
  touch(400, 308); pump(3); release(); pump(6);     // back to NATIVE
  touch(674, 48); pump(3); release(); pump(4);      // theme dot: CYPHERPINK
  save("/tmp/sim_settings_pink.ppm");               // accent recolors selections+title
  touch(680, 424); pump(3); release(); pump(6);     // BACK (y=398) -> home still pink
  save("/tmp/sim_wallet_pink.ppm");
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(578, 48); pump(3); release(); pump(4);      // theme dot: back to MONO
  touch(218, 164); pump(3); release(); pump(4);     // TESTNET pill (y=142, h44)
  save("/tmp/sim_settings_tn.ppm");
  touch(680, 424); pump(3); release(); pump(6);     // BACK -> home
  save("/tmp/sim_wallet_testnet.ppm");              // home now shows TESTNET badge
  touch(310, 240); pump(3); release(); pump(6);     // Receive: tb1 address now
  save("/tmp/sim_recv_tn.ppm");
  // The testnet silent-payment address is one character longer than mainnet
  // (tsp1 vs sp1) and was the only receive QR the walk never rendered, which
  // is where a truncation report landed. tools/check_qr_payloads.sh decodes
  // this frame and diffs it against the text beside it.
  touch(295, 426); pump(3); release(); pump(6);     // Silent payment (testnet)
  save("/tmp/sim_recv_sp_tn.ppm");
  // The explainer names the prefixes, so testnet renders a different title
  // ("WHY YOU SEE TB1P") and two more characters of body than mainnet does.
  // Capture the longer one: it is the variant that would overflow first.
  touch(730, 50); pump(3); release(); pump(30);     // ? -> tsp1/tb1p explanation
  save("/tmp/sim_recv_sp_help_tn.ppm");
  touch(400, 418); pump(3); release(); pump(6);     // OK closes the explanation
  touch(118, 430); pump(3); release(); pump(6);     // BACK out of the SP view
  touch(118, 430); pump(3); release(); pump(4);     // BACK
  touch(130, 240); pump(3); release(); pump(6);     // Sign -> chooser
  touch(218, 298); pump(3); release(); pump(6);     // FROM SD
  touch(328, 150); pump(3); release(); pump(8);     // file -> verify: TESTNET row
  save("/tmp/sim_verify_tn.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the chooser
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(218, 116); pump(3); release(); pump(4);     // MAINNET restore (y=94, h44)
  touch(680, 424); pump(3); release(); pump(4);     // BACK -> home

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
  touch(218, 290); pump(3); release(); pump(4);     // RESTORE FROM WORDS (pill at 264)
  save("/tmp/sim_setup_storage.ppm");               // KEEP ON THIS DEVICE / NOTHING SAVED
  touch(218, 176); pump(3); release(); pump(4);     // KEEP ON THIS DEVICE
  // restoring shows a third option here: a SeedQR carries its own length, so
  // it sits beside 12/24 rather than after them
  save("/tmp/sim_setup_count_restore.ppm");         // 12 / 24 / SCAN SEED QR
  touch(218, 176); pump(3); release(); pump(4);     // 12 WORDS
  save("/tmp/sim_setup_restore.ppm");
  touch(44, 314); pump(3); release(); pump(3);      // 'a'
  touch(450, 374); pump(3); release(); pump(3);     // 'b'
  save("/tmp/sim_setup_sug.ppm");                   // suggestions visible
  touch(163, 182); pump(3); release(); pump(3);     // accept "abandon" -> word 2
  touch(160, 434); pump(3); release(); pump(4);     // CANCEL -> chooser

  // the real path: CREATE SEED, simulated entropy, quiz, login twice.
  // Creating no longer asks how many words -- it is always 12 -- so KEEP ON
  // THIS DEVICE lands straight on the entropy screen, and the reveal is a
  // single page with no pager.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(218, 176); pump(3); release(); pump(4);     // KEEP ON THIS DEVICE
  save("/tmp/sim_setup_entropy.ppm");
  touch(168, 430); pump(3); release(); pump(4);     // CAPTURE (simulated)
  save("/tmp/sim_setup_words.ppm");                 // 12 words, one page, CANCEL + I WROTE THEM DOWN
  touch(590, 430); pump(3); release(); pump(4);     // I WROTE THEM DOWN
  save("/tmp/sim_setup_quiz.ppm");
  touch(218, 226); pump(3); release(); pump(4);     // round 1: pill 0 correct
  touch(598, 226); pump(3); release(); pump(4);     // round 2: pill 1
  touch(218, 306); pump(3); release(); pump(6);     // round 3: pill 2 -> stored
  save("/tmp/sim_setup_ppintro.ppm");               // ONE MORE LAYER (what a passphrase is)
  touch(188, 430); pump(3); release(); pump(6);     // CREATE PASSPHRASE -> keyboard
  save("/tmp/sim_setup_pass.ppm");                  // CREATE YOUR PASSPHRASE

  // shift semantics. Row 3 is [ABC z x c v b n m BKSP] at y=355; ABC x=46,
  // z x=135, backspace x=752.
  touch(135, 277); pump(2); release(); pump(2);      // 's': catch the feedback live
  lv_refr_now(NULL);
  save("/tmp/sim_kb_feedback.ppm");                 // key flash + risen callout
  pump(30); touch(752, 355); pump(3); release(); pump(3);
  touch(46, 355); pump(3); release(); pump(4);      // ABC once -> one-shot upper
  save("/tmp/sim_kb_shift_on.ppm");                 // upper plane, key reads "abc"
  touch(135, 355); pump(3); release(); pump(4);     // Z
  save("/tmp/sim_kb_shift_off.ppm");                // dropped BACK to lowercase
  touch(46, 355); pump(2); release(); pump(2);      // two taps inside 400ms
  touch(46, 355); pump(2); release(); pump(4);
  save("/tmp/sim_kb_caps.ppm");                     // locked: key reads "CAPS"
  touch(135, 355); pump(3); release(); pump(4);     // Z, and the plane MUST stay
  save("/tmp/sim_kb_caps_stays.ppm");
  touch(46, 355); pump(3); release(); pump(4);      // CAPS off
  // HOLD the shift key = caps lock. This is the gesture people actually reach
  // for; the double tap above is the alternative, not the only way in.
  touch(46, 355); pump(40); release(); pump(4);
  save("/tmp/sim_kb_hold_caps.ppm");                // key must read "CAPS"
  touch(135, 355); pump(3); release(); pump(4);     // Z, and the plane MUST stay
  save("/tmp/sim_kb_hold_caps_stays.ppm");
  // a SLOW tap on CAPS unlocks; it must NOT also register as a hold and relock
  touch(46, 355); pump(40); release(); pump(4);
  save("/tmp/sim_kb_caps_slow_off.ppm");            // key must read "ABC"
  touch(135, 355); pump(40); release(); pump(4);    // HOLD z -> Z, still lowercase
  save("/tmp/sim_kb_hold.ppm");
  for (int i = 0; i < 8; i++) { touch(752, 355); pump(3); release(); pump(3); }
  save("/tmp/sim_kb_cleared.ppm");                  // back to an empty field
  // CANCEL during setup must confirm (don't throw away a fresh seed on one tap)
  touch(200, 430); pump(3); release(); pump(6);     // CANCEL -> confirm modal
  save("/tmp/sim_setup_cancel.ppm");
  touch(264, 323); pump(3); release(); pump(4);     // KEEP GOING -> back to keyboard
  touch(46, 278); pump(3); release(); pump(3);      // 'a' (deliberately weak)
  touch(725, 430); pump(3); release(); pump(4);     // OK -> weak warning
  save("/tmp/sim_setup_weak.ppm");                  // modal: BACK / USE ANYWAY
  // BACK first: a weak passphrase must be escapable, and the entry has to
  // survive it so the owner can lengthen what they typed instead of retyping.
  touch(223, 372); pump(3); release(); pump(4);     // GO BACK -> keyboard, 'a' intact
  save("/tmp/sim_setup_weak_back.ppm");
  touch(725, 430); pump(3); release(); pump(4);     // OK -> the card again
  touch(577, 372); pump(3); release(); pump(4);     // USE ANYWAY -> confirm stage
  save("/tmp/sim_setup_pass2.ppm");                 // TYPE IT AGAIN
  touch(46, 278); pump(3); release(); pump(3);      // 'a' again
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  touch(400, 414); pump(3); release(); pump(8);     // TAP TO OPEN -> passphrase warning
  lv_refr_now(NULL); pump(2);
  save("/tmp/sim_setup_warn.ppm");                  // unverified: I UNDERSTAND has red ring

  // Optional full recovery rehearsal: all generated words, then the exact
  // passphrase. Prefixes below uniquely put each expected word in suggestion 0.
  touch(198, 430); pump(3); release(); pump(6);     // VERIFY MY COPY -> intro
  save("/tmp/sim_setup_rehearse_intro.ppm");
  touch(198, 430); pump(3); release(); pump(6);     // TYPE MY WORDS -> keypad
  static const char *verify_prefixes[] = {
    "g", "m", "no", "so", "sy", "fem",
    "fi", "at", "v", "fo", "c", "stay"
  };
  for (size_t i = 0; i < sizeof verify_prefixes / sizeof verify_prefixes[0]; i++)
    restore_word(verify_prefixes[i]);
  pump(4);                                          // all words -> VERIFIED
  touch(198, 430); pump(3); release(); pump(8);     // DONE -> fresh passphrase entry
  save("/tmp/sim_setup_rehearse_pass.ppm");
  touch(46, 278); pump(3); release(); pump(3);      // exact passphrase: 'a'
  // The rehearsal above completes again now that creating makes 12 words: the
  // prefixes were always written for a 12-word seed, and the wizard's "TEMP
  // probe" switch to 24 had quietly left them one word list out of step. Every
  // frame still CHANGED while it was broken, which is why check_sim_taps.py
  // never flagged it -- it catches dead taps, not taps landing on a wrong but
  // still-live screen.
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  touch(400, 414); pump(3); release(); pump(10);    // TAP TO OPEN -> verified warning
  save("/tmp/sim_setup_verified.ppm");              // green full-backup state, at last
  touch(590, 430); pump(3); release(); pump(30);    // I UNDERSTAND -> the stroke chooser

  // The LAST step of setup: the ONE stroke that reaches the real signer
  // (wallet_duress_ui.c). Plain KISS opens the spare and always will, so there
  // is nothing to configure for it. The stroke is drawn against the printed
  // reference word at (250,170)-(550,268), which is the same box that
  // wallet_duress_classify measures in the game.
  save("/tmp/sim_duress_intro.ppm");                // two ways in
  touch(148, 430); pump(3); release(); pump(40);    // OK -> fund the spare
  save("/tmp/sim_duress_fund.ppm");                 // why the decoy needs coins in it
  touch(148, 430); pump(3); release(); pump(40);    // OK -> pick your stroke
  save("/tmp/sim_duress_pick_real.ppm");            // six strokes, two rows of three
  touch(158, 176); pump(3); release(); pump(40);    // UNDERLINE (first pill)
  save("/tmp/sim_duress_draw_real.ppm");            // draw it, over the reference word
  for (int i = 0; i <= 22; i++) { touch(262 + i * 12, 300); pump(1); }
  release(); pump(40);                              // an underline: wide, flat, low
  save("/tmp/sim_duress_draw_real2.ppm");           // ...and once more to confirm
  for (int i = 0; i <= 22; i++) { touch(262 + i * 12, 302); pump(1); }
  release(); pump(40);
  save("/tmp/sim_duress_done.ppm");                 // the one way in is set
  touch(148, 430); pump(3); release(); pump(140);   // DONE -> saves, home settles
  save("/tmp/sim_setup_home.ppm");

  // step 8: idle auto-lock — WALLET_AUTOLOCK_MS untouched on the home must
  // close the session and land back on the game menu.
  //
  // Keep this ahead of WALLET_AUTOLOCK_MS in main.c (300000ms today). When
  // that went from 2min to 5min in efb60bc this pump stayed at 7700 frames
  // (123s), so the lock never fired, the KISS gesture below was drawn onto
  // the still-open home screen, and every frame from here to the end of the
  // walk silently became a copy of whatever tile that opened. The wipe and
  // amnesic-mode steps were dead for four commits and still "passed".
  // tools/check_sim_taps.py is what catches that now; this margin is what
  // stops it happening in the first place.
  pump(20000);                                      // 320s > 300s + intro settle
  save("/tmp/sim_autolock.ppm");                    // must be the game MENU again

  // THE point of the whole feature: with strokes configured, drawing KISS on
  // its own opens the DECOY straight from the game. No keyboard, no passphrase
  // field, nothing on screen that says a second signer exists. This frame must
  // be a wallet home, not the login.
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(140);
  save("/tmp/sim_decoy_home.ppm");                  // decoy home, reached with no login

  // Settings in a DECOY session must not carry the WAYS IN row: a row that
  // only appears for one of the two signers is exactly the tell this feature
  // exists to avoid. The frame is what proves it.
  touch(670, 240); pump(3); release(); pump(20);    // SETTINGS tile
  save("/tmp/sim_decoy_settings.ppm");              // no WAYS IN row here
  touch(680, 426); pump(3); release(); pump(20);    // BACK
  touch(100, 60); pump(3); release(); pump(20);     // KISS logo -> lock, back to the game

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
  // the WIPE pill is wallet_settings.c's mk_pillh(430, 310, 340, 52)
  touch(590, 336); pump(4); release(); pump(8);     // WIPE WALLET -> confirm screen
  lv_refr_now(NULL); pump(2);
  save("/tmp/sim_wipe_confirm.ppm");                // ERASE THIS WALLET? + HOLD pill
  // a tap is NOT enough: press, release early, nothing must happen
  touch(208, 398); pump(2); release(); pump(4);
  save("/tmp/sim_wipe_tap_noop.ppm");               // still the confirm screen
  // hold it: 2000ms at 16ms/frame is 125 frames, give it margin
  touch(208, 398); pump(60);                        // ~half way: the fill sweeps
  lv_refr_now(NULL);
  save("/tmp/sim_wipe_holding.ppm");                // partial red fill, not fired
  pump(100); release(); pump(6);                    // hold through -> erased
  save("/tmp/sim_wiped.ppm");                       // WALLET ERASED confirmation
  touch(400, 412); pump(3); release(); pump(130);   // OK (200x52 at y=386) -> menu
  save("/tmp/sim_wiped_menu.ppm");                  // must be the game MENU

  // step 10: AMNESIC mode — nothing is stored, so the KISS gesture lands on
  // LOAD YOUR WALLET instead of the wizard, and a seed QR is a valid way in.
  // the wipe above already left us locked on the game cover with no seed
  wallet_seed_set_mode(WSEED_MODE_AMNESIC);
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(4);
  save("/tmp/sim_amnesic_load.ppm");                // LOAD YOUR WALLET

  touch(218, 290); pump(3); release(); pump(6);     // SCAN A SEED QR (pill at 264)
  wallet_scan_inject("not a seed qr at all", 20); pump(6);
  save("/tmp/sim_amnesic_qrbad.ppm");               // NOT A SEED, nothing loaded
  touch(198, 430); pump(3); release(); pump(6);     // TRY AGAIN -> load screen
  touch(218, 290); pump(3); release(); pump(6);     // SCAN A SEED QR again
  {   // a numeric SeedQR: 12 indices, four digits each
    const char *sq = "000000000000000000000000000000000000000000000003";
    wallet_scan_inject(sq, 48);
  }
  pump(8);
  save("/tmp/sim_amnesic_pass.ppm");                // straight to the passphrase

  // a passphrase can come from a QR too, behind one warning screen
  touch(596, 38); pump(3); release(); pump(6);      // SCAN
  save("/tmp/sim_amnesic_ppwarn.ppm");              // PASSPHRASE FROM A QR
  touch(198, 430); pump(3); release(); pump(6);     // SCAN IT -> camera
  wallet_scan_inject("correct horse battery staple correct horse battery "
                     "staple correct horse battery staple xyz", 90);
  pump(6);
  touch(696, 38); pump(3); release(); pump(4);      // SHOW
  save("/tmp/sim_kb_show_long.ppm");                // 90 chars, wrapped not "..."
  for (int i = 0; i < 90; i++) { touch(752, 355); pump(1); release(); pump(1); }
  touch(46, 278);  pump(3); release(); pump(3);     // 'a'
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  touch(400, 414); pump(3); release(); pump(140);   // TAP TO OPEN -> home
  save("/tmp/sim_amnesic_home.ppm");                // an amnesic wallet, unlocked

  // LVGL heap watermark: the pool is only 128K (matches the device), and a
  // failed lv_malloc during rendering = LVGL assert = infinite loop. Keep an
  // eye on max_used whenever screens/labels are added (the i18n picker was
  // the first thing to blow the old 64K pool).
  {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[lvheap] total %u used %u max_used %u frag %u%%\n",
           (unsigned)mon.total_size, (unsigned)(mon.total_size - mon.free_size),
           (unsigned)mon.max_used, (unsigned)mon.frag_pct);
  }
  printf("sim done\n");
  return 0;
}
