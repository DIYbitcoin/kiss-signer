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
#include "wallet_recv.h"    // sim-only hook for the derivation path "?"
#include "wallet_settings.h"
#include "wallet_theme.h"   // SIM_ACCENT picks the theme the walk renders in
#include "wallet_ui.h"      // wallet_ui_drop_indev_for_test: cold-boot the decoy

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
static int s_sim_sd_present = 1;      // hot-plug state for the unlock gate
int wallet_seed_exists(void) { return s_sim_has_seed || s_sim_has_pending; }
int wallet_seed_store(const char *m) {
  snprintf(s_sim_seed, sizeof s_sim_seed, "%s", m);
  s_sim_has_seed = 1;
  return 0;
}
int wallet_seed_load(char *out, size_t n) {
  if (s_sim_has_pending) { snprintf(out, n, "%s", s_sim_pending); return 0; }
  if (s_sim_mode == WSEED_MODE_SD && !s_sim_sd_present)
    return WSEED_ERR_SD_MISSING;
  if (!s_sim_has_seed) return -1;
  snprintf(out, n, "%s", s_sim_seed);
  return 0;
}
int wallet_seed_wipe(void) {
  s_sim_has_seed = 0;
  s_sim_has_pending = 0;
  return 0;
}
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
  if (s_sim_mode == WSEED_MODE_SD && !s_sim_sd_present)
    return WSEED_ERR_SD_MISSING;
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
  s_sim_pending_mode = (m == WSEED_MODE_AMNESIC || m == WSEED_MODE_SD)
                     ? m : WSEED_MODE_KEEP;
}
int wallet_seed_set_mode(int m) {
  s_sim_pending_mode = -1;
  s_sim_mode = (m == WSEED_MODE_AMNESIC || m == WSEED_MODE_SD)
             ? m : WSEED_MODE_KEEP;
  if (s_sim_mode == WSEED_MODE_AMNESIC) s_sim_has_seed = 0;
  return 0;
}
// The desktop store is a plain file, so at-rest encryption is off: the FLASH
// note reads as the unencrypted (steering) copy in the sim, matching a normal
// beta device.
int wallet_seed_flash_encrypted(void) { return 0; }
int wallet_seed_move_to(int m) {
  if (m != WSEED_MODE_KEEP && m != WSEED_MODE_SD &&
      m != WSEED_MODE_AMNESIC)
    return WSEED_ERR_INVALID;
  if (m == s_sim_mode) return WSEED_OK;
  if ((m == WSEED_MODE_SD || s_sim_mode == WSEED_MODE_SD) &&
      !s_sim_sd_present)
    return WSEED_ERR_SD_MISSING;
  if (!s_sim_has_seed && !s_sim_has_pending) return WSEED_ERR_NO_SEED;

  if (m == WSEED_MODE_AMNESIC) {
    if (!s_sim_has_pending) {
      snprintf(s_sim_pending, sizeof s_sim_pending, "%s", s_sim_seed);
      s_sim_has_pending = 1;
    }
    s_sim_has_seed = 0;
  } else {
    if (s_sim_has_pending)
      snprintf(s_sim_seed, sizeof s_sim_seed, "%s", s_sim_pending);
    s_sim_has_seed = 1;
    s_sim_has_pending = 0;
  }
  s_sim_pending_mode = -1;
  s_sim_mode = m;
  return WSEED_OK;
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
static int s_sim_prepared_decoy;
int wallet_session_prepare(const char *passphrase) {
  s_sim_prepared_decoy = !(passphrase && passphrase[0]);
  return 0;
}
int wallet_session_activate_prepared(void) {
  s_sim_decoy = s_sim_prepared_decoy;
  return 0;
}
void wallet_session_discard_prepared(void) { s_sim_prepared_decoy = 0; }
int wallet_session_open(const char *passphrase) {
  int rc = wallet_session_prepare(passphrase);
  return rc == 0 ? wallet_session_activate_prepared() : rc;
}
void wallet_session_close(void) {
  s_sim_decoy = 0;
  wallet_seed_forget();          // real wallet_crypto.c does the same on lock
}
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

// Frame sequence capture, for the one thing in this repo that a screenshot
// cannot show: the game turning into a signer.
//
// README.md has always described the gesture in prose beside two static frames,
// the menu and the wallet home, which is a picture of the start and a picture
// of the end with the entire pitch missing from between them. So the walk that
// already draws KISS on the menu records what it draws, every LVGL tick,
// straight from the same framebuffer the screenshots come from.
//
// Deliberately not save(): these are not walk checkpoints. Routing them through
// save() would add ~90 stops to sim/overlapcheck.c and ~90 pairs to
// tools/check_sim_taps.py, which compares consecutive frames and would object
// that a stroke in progress looks like the stroke before it. Both are right to
// object; a mid animation frame is not a settled screen and has no business
// being measured as one.
static int g_seq_on, g_seq_n;
static void save_seq(void);

static void pump(int frames) {
  for (int i = 0; i < frames; i++) {
    lv_tick_inc(16);
    lv_timer_handler();
    if (g_seq_on) save_seq();
  }
}

// SIM_LANG=<code> (en, de, es-MX, ...) renders the whole walk in that language
// and prefixes every frame: /tmp/sim_de_login.ppm etc. NVS is stubbed in the
// sim, so the env var is the only language input.
static const char *g_lang_code;

// SIM_ACCENT=<name> (MONO, GREEN, CYPHERPINK, ORANGE) starts the walk in that
// theme. The walk itself still visits the theme dots near the end and leaves on
// MONO, which is deliberate: this only decides what the other ~160 stops are
// wearing. Added for the ROLE check in sim/overlapcheck.c, which asks whether
// an element carries an accent and a status colour at once and therefore has
// nothing to look at until an accent is actually selected.
static void sim_pick_accent(void) {
  const char *a = getenv("SIM_ACCENT");
  if (!a || !*a) return;
  for (int i = 0; i < WT_ACC_N; i++) {
    wt_accent_set(i);
    if (strcmp(wt_accent_name(), a) == 0) return;
  }
  wt_accent_set(WT_ACC_MONO);
  fprintf(stderr, "unknown SIM_ACCENT %s\n", a);
  exit(1);
}

#ifdef OVERLAPCHECK
void oc_check(const char *tag);   // sim/overlapcheck.c
int  oc_report(void);
int  oc_selftest(void);
#endif

// RGB565 framebuffer to a binary P6 PPM. Shared by the walk's checkpoints and
// by the animation capture, so both are literally the same pixels.
static bool write_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", HRES, VRES);
  for (int i = 0; i < HRES * VRES; i++) {
    uint16_t c = g_fb[i];
    unsigned char r = ((c >> 11) & 0x1F) * 255 / 31;
    unsigned char g = ((c >> 5) & 0x3F) * 255 / 63;
    unsigned char b = (c & 0x1F) * 255 / 31;
    fputc(r, f); fputc(g, f); fputc(b, f);
  }
  fclose(f);
  return true;
}

static void save(const char *path) {
  char lp[160];
  lv_refr_now(NULL);   // saved frames always reflect every pending invalidation
#ifdef OVERLAPCHECK
  // The overlap gate reuses this walk rather than keeping a second copy that
  // would drift from it. Every stop the walk saves is a settled screen, which
  // is where a layout question belongs, so any frame added here is checked
  // without anyone having to remember. No image is written in gate builds.
  //
  // Called before the language prefix goes on, so the gate sees the same frame
  // name in all 21 locales and its per-frame rules stay comparable.
  oc_check(path);
  return;
#endif
  if (g_lang_code && strncmp(path, "/tmp/sim_", 9) == 0) {
    snprintf(lp, sizeof lp, "/tmp/sim_%s_%s", g_lang_code, path + 9);
    path = lp;
  }
  if (write_ppm(path)) printf("wrote %s\n", path);
}

// One numbered frame of an animation, straight out of the same framebuffer,
// plus where the finger was when it was taken.
//
// The finger is the whole reason the path file exists. Recording the gesture
// proved that the device draws NOTHING while it is being made: the menu idles,
// its stars twinkle, and 56 frames later the signer is simply there. That is
// the security property working as designed, and it also means a recording of
// the screen alone shows a jump cut and teaches nobody where to draw. So the
// touch coordinates are written beside the frames and tools/gen_docs_shots.py
// traces them onto the picture as an annotation, from this data rather than
// from a second copy of the stroke table that would drift the first time a
// coordinate moved.
//
// English only: the frames become a single GIF in the README, and 21 sets of
// them would be 21 copies of a wordless gesture. Silent, because ~90 "wrote"
// lines would bury the walk's own output.
static void save_seq(void) {
  char path[64];
#ifdef OVERLAPCHECK
  // The gate builds run this same walk, one of them per accent. Without this
  // they would overwrite the captured frames with whatever theme they were
  // sweeping, and the next gen_docs_shots.py would quietly build the README's
  // GIF in ORANGE.
  return;
#endif
  if (g_lang_code) return;
  lv_refr_now(NULL);
  snprintf(path, sizeof path, "/tmp/sim_reveal_%03d.ppm", g_seq_n);
  if (!write_ppm(path)) return;

  FILE *p = fopen("/tmp/sim_reveal_path.txt", g_seq_n ? "a" : "w");
  if (p) {
    fprintf(p, "%d %d %d %d\n", g_seq_n, g_tx, g_ty, (int)g_pressed);
    fclose(p);
  }
  g_seq_n++;
}

void sim_home_status(const char *msg);   // main.c (SIMULATOR): bottom-center status slot

static void touch(int x, int y) { g_tx = x; g_ty = y; g_pressed = true; }
static void release(void) { g_pressed = false; }

// The unlock word as used throughout the scripted walk. Kept as a helper for
// storage hot-plug coverage added at the end, so that test does not invent a
// second approximation of the gesture recognizer's real input.
static void draw_kiss(void)
{
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); }
  release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); }
  release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); }
  release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); }
  release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1);
  touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1);
  touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1);
  touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1);
  touch(462, 272); pump(1); release();
  // 40 frames, not 4. Whichever door this call ends up taking, main.c may hold
  // it for KISS_OPEN_DELAY_MS so the real wallet and the decoy cannot be told
  // apart by how fast the screen arrives. At 16ms a frame this is 640ms of
  // slack over a 500ms wait. Callers that land on the setup wizard or the
  // amnesic loader open immediately and do not need it; the slack costs them
  // nothing and stops this helper breaking if a caller changes door.
  pump(40);
}

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

#ifdef OVERLAPCHECK
  // Before the walk, because it needs a display and nothing else.
  if (getenv("OVERLAPCHECK_SELFTEST")) return oc_selftest();
#endif

  sim_pick_accent();

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
  //
  // Recorded as well as captured: see save_seq(). A beat on the untouched menu
  // first, so the GIF opens on the thing everyone else sees.
  g_seq_on = 1; pump(8);
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);      // K spine
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);  // K upper arm
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);  // K lower arm
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);       // I
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);  // S
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(3);  // S
  // 45, not 16. The door is held for KISS_OPEN_DELAY_MS now, so 19 frames of
  // total slack (304ms) stopped short of the reveal this is here to record.
  pump(45); g_seq_on = 0;                           // hold on the reveal, then stop recording
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

  // tap-to-edit, SHOW only. "abc" is centred in the 704px slot at font28, so
  // the three glyphs sit around x 374..425 with the entry row at y~109. Tap
  // the left half of 'b', insert, take it back out, then send the caret to the
  // end so the symbol-plane walk below still starts from exactly "abc".
  touch(395, 109); pump(3); release(); pump(3);     // caret lands before 'b'
  save("/tmp/sim_login_caret.ppm");                 // bar between 'a' and 'b'
  touch(127, 353); pump(3); release(); pump(3);     // 'z' inserted mid-string
  save("/tmp/sim_login_caret_ins.ppm");             // reads "azbc"
  touch(753, 353); pump(3); release(); pump(3);     // backspace takes the 'z' back
  save("/tmp/sim_login_caret_del.ppm");             // reads "abc" again
  touch(700, 109); pump(3); release(); pump(3);     // tap past the text: caret to the end

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
  touch(622, 430); pump(3); release(); pump(12);    // TAP TO OPEN -> hex noise decrypting
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
  // No sim_tile_press.ppm any more: the home tiles draw nothing on press, so a
  // frame taken between touch and release is a picture of the home screen and
  // check_sim_taps would rightly call it a dead interaction.
  touch(310, 240); pump(3); release(); pump(6);     // Receive tile
  save("/tmp/sim_recv.ppm");                        // HANDOFF-03 landing: single-address detail
  touch(455, 430); pump(3); release(); pump(6);     // SILENT PAYMENT pill -> SP address view
  save("/tmp/sim_recv_sp.ppm");                     // folded text + largest receive QR
  touch(196, 248); pump(3); release(); pump(6);     // QR -> full-screen scan view
  save("/tmp/sim_recv_sp_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom, exact state preserved
  touch(520, 166); pump(3); release(); pump(6);     // folded address itself -> full
  save("/tmp/sim_recv_sp_full.ppm");
  touch(188, 430); pump(3); release(); pump(6);     // SHOW SHORT -> folded default
  touch(730, 50); pump(3); release(); pump(30);     // ? -> sp1/bc1p explanation
  save("/tmp/sim_recv_sp_help.ppm");
  touch(400, 418); pump(3); release(); pump(6);     // OK closes the explanation
  touch(680, 430); pump(3); release(); pump(6);     // BACK from SP -> detail again
  touch(255, 430); pump(3); release(); pump(6);     // ALL ADDRESSES pill -> the list
  save("/tmp/sim_recv_list.ppm");                   // paginated list, one tap away now
  // Actually DRAG it. This is the first scrolling surface in the whole wallet
  // -- every other container turns scrolling off -- so the walk flicks it for
  // real rather than trusting that a scrollable flag implies a list that moves.
  touch(400, 300); pump(2);
  touch(400, 200); pump(2);
  touch(400, 120); pump(2);                         // finger travels up: later indices
  release(); pump(20);                              // let the throw and snap settle
  save("/tmp/sim_recv_scrolled.ppm");
  touch(400, 120); pump(3); release(); pump(6);     // tap a row -> that one address
  save("/tmp/sim_recv_detail.ppm");                 // QR + body + lit tail + compare-8
  touch(167, 231); pump(3); release(); pump(6);     // QR card -> zoom
  save("/tmp/sim_recv_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
  touch(435, 356); pump(3); release(); pump(4);     // NEXT ADDRESS pill -> next index
  save("/tmp/sim_recv1.ppm");
  {  // VERIFY: own, valid-but-not-found, wrong-network, invalid, then own SP.
    touch(680, 430); pump(3); release(); pump(6);   // VERIFY pill -> raw scan screen
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
  touch(100, 430); pump(3); release(); pump(4);     // BACK (leftmost now) -> home
  touch(680, 60); pump(3); release(); pump(40);     // fingerprint chip -> education card
  save("/tmp/sim_home_fp.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  touch(490, 240); pump(3); release(); pump(6);     // Wallet tile -> section home
  save("/tmp/sim_winfo.ppm");
  wallet_info_sim_open_fp_help(); pump(40);         // full staggered card intro settles
  save("/tmp/sim_winfo_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  wallet_info_sim_open_type_help(); pump(30);       // deterministic: chip x varies by locale
  save("/tmp/sim_winfo_type_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the type card
  touch(590, 130); pump(3); release(); pump(6);     // PAIR COORDINATOR
  save("/tmp/sim_pair.ppm");                        // descriptor (Sparrow) active
  touch(198, 228); pump(3); release(); pump(6);     // descriptor QR -> zoom
  save("/tmp/sim_pair_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
  touch(672, 150); pump(3); release(); pump(4);     // MOBILE / BlueWallet segment
  save("/tmp/sim_pair_bw.ppm");
  touch(541, 105); pump(3); release(); pump(30);    // "?" chip -> coordinator card
  save("/tmp/sim_pair_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  // Page two: the import steps plus the address proof. It is the page the
  // owner actually follows, so it gets walked and rendered like any other.
  touch(118, 430); pump(3); release(); pump(6);     // NEXT -> HOW TO PAIR
  save("/tmp/sim_pair_steps.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the QR page
  // SCAN KEY is no longer buried in the pair screen: it is a top-level ROW in
  // the WALLET screen's COORDINATOR column, so back out of pairing first. It was
  // moved because hiding a separate PRIVATE-key export one tap inside the
  // descriptor flow implied the two were the same action.
  touch(680, 430); pump(3); release(); pump(6);     // BACK (WT_BACK_X pill) -> WALLET
  touch(748, 262); pump(3); release(); pump(40);    // "?" -> what SCAN KEY means
  save("/tmp/sim_sp_help.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  touch(594, 198); pump(3); release(); pump(6);     // SCAN KEY row -> consent warning
  save("/tmp/sim_sp_warn.ppm");
  touch(198, 430); pump(25); release(); pump(6);    // early release: key stays hidden
  save("/tmp/sim_sp_warn_early.ppm");
  touch(198, 430); pump(65); release(); pump(8);    // full hold -> export
  save("/tmp/sim_sp_key.ppm");
  touch(198, 228); pump(3); release(); pump(6);     // private scan-key QR -> zoom
  save("/tmp/sim_sp_key_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
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
  // Pill tap feedback (pill_tap_feedback in wallet_theme.c). The device has
  // no haptics, so a press is answered optically or not at all, and "not at
  // all" is the kind of thing a refactor takes away in silence. This is the
  // walk's ordinary FROM SD CARD tap, just photographed twice on the way
  // through, so it costs the walk nothing and still pins both halves: the
  // ring exists ONLY in the first frame (it is outside the pill edge there and
  // gone by the second), so if the animation ever stops rendering the two
  // frames become identical and check_sim_taps.py fails.
  // 19 pumps held is ~304ms, deliberately under LVGL's 400ms long-press.
  touch(218, 340); pump(5);                        // FROM SD CARD (row 1)
  save("/tmp/sim_pill_ring.ppm");                  // ring still outside the edge
  pump(14);
  save("/tmp/sim_pill_held.ppm");                  // settled: accent fill, 2px down
  release(); pump(6);                              // -> file list
  save("/tmp/sim_sign_files.ppm");
  touch(328, 150); pump(3); release(); pump(8);     // first file -> verify (READY)
  save("/tmp/sim_sign_verify.ppm");
  // the RBF "?" is wallet_sign.c's 30px chip, pinned at (738, SG_FOOT_Y-6) for
  // BOTH the replaceable and final wordings. It used to sit right after the
  // text, so its x moved with the translation; it is fixed now. The redraw
  // moved the footer rule to 288 and the cells to 300, taking the chip with
  // it. This is its centre.
  touch(753, 309); pump(3); release(); pump(8);     // RBF "?" -> explainer (mid-intro)
  save("/tmp/sim_sign_rbf_mid.ppm");
  pump(30);                                          // let the stagger settle
  save("/tmp/sim_sign_rbf.ppm");
  touch(400, 426); pump(3); release(); pump(6);     // OK closes the card
  touch(235, 430); pump(3); release(); pump(6);     // DETAILS -> raw facts page
  save("/tmp/sim_sign_details.ppm");
  touch(656, 50); pump(3); release(); pump(30);     // SIMPLE EXPLAINERS
  save("/tmp/sim_sign_glossary.ppm");
  touch(400, 430); pump(3); release(); pump(6);     // OK closes glossary
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> verify again
  touch(620, 430); pump(40);                        // hold the sign pill: ring ~half full
  save("/tmp/sim_sign_hold.ppm");
  pump(45);                                         // past 1.2s: signs + writes SD
  release(); pump(8);
  save("/tmp/sim_sign_done.ppm");
  touch(400, 430); pump(3); release(); pump(6);     // DONE -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 340); pump(3); release(); pump(6);     // FROM SD CARD (row 1)
  touch(328, 216); pump(3); release(); pump(8);     // the STOP file -> blocked verify
  save("/tmp/sim_sign_stop.ppm");
  // BACK is ONE STEP now: from a transaction it returns to the list that
  // transaction came from, not to the home screen. The three files below are
  // opened one after another without ever leaving SIGN, which is the whole
  // point -- picking the wrong file used to cost the entire trip back in.
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  save("/tmp/sim_sign_back_files.ppm");
  touch(328, 282); pump(3); release(); pump(8);     // the FEE file -> amber caution
  save("/tmp/sim_sign_fee.ppm");                    // summary + "I UNDERSTAND" gate
  // I UNDERSTAND moved out of the action row and into the caution row itself,
  // which is the point of the redraw: the answer sits beside the thing being
  // read, and HOLD TO SIGN keeps its coordinates in both states. The pill is
  // local (543,8) 170x40 inside a row pinned at (24, SG_PANEL_Y), so row 0's is
  // 567..737 x 158..198. This is its centre. It was still tapping the old
  // action-row position at (364,430), which the redraw deleted.
  touch(652, 178); pump(3); release(); pump(6);     // I UNDERSTAND -> row goes green
  save("/tmp/sim_sign_fee_ack.ppm");
  // BACK out of a screen an acknowledgement repainted. The tap above is what
  // makes the orphaned-screen check at the end of this walk mean anything: the
  // repaint is the only thing in the app that ever replaced a live screen
  // without deleting it, so if the ack pill is not actually hit, nothing counts
  // an orphan and the check passes on a build that leaks.
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  touch(328, 348); pump(3); release(); pump(8);     // COMBO file -> stacked cautions
  save("/tmp/sim_sign_combo.ppm");
  // The caution "?" used to be anchored to the top of the caution stack, so
  // its y moved with the number of cautions that fired. Now that every
  // caution owns its own row and answers for itself, one chip at (738,108)
  // covers the whole stack and never moves. This is its centre.
  touch(753, 123); pump(3); release(); pump(6);     // "?" -> WHY FLAGGED card
  save("/tmp/sim_sign_why.ppm");
  touch(400, 438); pump(3); release(); pump(6);     // OK closes the card
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
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
  touch(218, 340); pump(3); release(); pump(6);     // FROM SD CARD -> list (only SPAY)
  touch(328, 150); pump(3); release(); pump(8);     // zsp-SPAY (row 0) -> SP verify
  save("/tmp/sim_sign_sp.ppm");                      // SP output row: badge + address + note
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the chooser
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home

  // step 6: Sign via QR — scan (real UR fountain parts injected as if the
  // camera decoded them), verify, sign, animated UR out
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> chooser
  touch(218, 240); pump(3); release(); pump(6);     // SCAN QR -> scan screen
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
  touch(620, 430); pump(40);                        // hold to sign
  pump(45); release(); pump(8);
  save("/tmp/sim_qr_out1.ppm");                     // animated UR out, first part
  pump(20);                                         // ~320ms: 250ms timer advanced
  save("/tmp/sim_qr_out2.ppm");                     // ...a different part
  touch(206, 258); pump(3); release(); pump(20);    // animated signed QR -> zoom
  save("/tmp/sim_qr_out_zoom.ppm");                 // animation keeps moving enlarged
  touch(763, 35); pump(3); release(); pump(6);      // close on latest frame
  touch(530, 270); pump(3); release(); pump(6);     // EASY SCAN: sparser, slower QR
  save("/tmp/sim_qr_out_ez.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // DONE -> home
  save("/tmp/sim_qr_end.ppm");

  // Receive lands past the highest address used or shown. Every detail keeps
  // the same privacy reminder visible; it does not claim an offline signer
  // knows whether this particular address received a payment.
  touch(310, 240); pump(3); release(); pump(6);     // Receive tile -> detail landing
  save("/tmp/sim_recv_fresh.ppm");                  // freshest address, one screen
  // The address card folds. Tap it once for every character grouped in fours,
  // tap it again to go back to the eight that matter. Both states get a frame:
  // the fold is the only way to read the whole address off this screen.
  touch(530, 190); pump(3); release(); pump(6);     // address card -> full address
  save("/tmp/sim_recv_full.ppm");
  touch(530, 190); pump(3); release(); pump(6);     // and back to folded
  touch(435, 335); pump(3); release(); pump(6);     // NEXT ADDRESS -> next index
  save("/tmp/sim_recv_reminder.ppm");               // same layout, different address text
  touch(435, 335); pump(3); release(); pump(4);     // NEXT ADDRESS again
  save("/tmp/sim_recv_next.ppm");
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> home

  // settings: address-type chooser (all 3 visible, active highlighted) + the
  // TESTNET home badge; verify Receive/verify reflect testnet, then restore.
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile
  save("/tmp/sim_settings.ppm");                    // mainnet, NATIVE highlighted

  // STORAGE is a first-class Settings row, not a setup-only choice. SD is
  // offered on every build now, so exercise a real FLASH -> SD migration,
  // including the fact that a short press cannot fire it.
  touch(200, 250); pump(3); release(); pump(6);     // storage row (left col) -> chooser
  save("/tmp/sim_storage_choose.ppm");               // all three selectable
  touch(174, 244); pump(3); release(); pump(6);     // SD CARD -> confirmation
  save("/tmp/sim_storage_confirm_sd.ppm");
  touch(213, 425); pump(30); release(); pump(6);    // <1500ms: no migration
  save("/tmp/sim_storage_hold_noop.ppm");
  touch(213, 425); pump(105); release(); pump(8);   // deliberate hold -> success
  save("/tmp/sim_storage_sd_ok.ppm");
  touch(400, 430); pump(3); release(); pump(8);     // OK -> Settings
  save("/tmp/sim_settings_sd.ppm");                 // current mode reads SD CARD
  // The home now carries the SD-storage badge (accent, breathing while the
  // card is in). Pop out to capture it, then return to Settings to migrate
  // back to FLASH.
  touch(680, 430); pump(3); release(); pump(8);      // Settings BACK, right corner -> home
  save("/tmp/sim_home_sd.ppm");                      // SD storage badge on home
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile -> Settings
  touch(200, 250); pump(3); release(); pump(6);     // storage row -> chooser
  touch(174, 144); pump(3); release(); pump(6);     // FLASH
  touch(213, 425); pump(105); release(); pump(8);
  touch(400, 430); pump(3); release(); pump(8);     // back on FLASH

  // RECOVERY WORDS now belongs to Settings. Verify the paper copy, return to
  // Settings, then separately exercise the sensitive word reveal.
  touch(580, 122); pump(3); release(); pump(6);     // Recovery words row -> warning
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

  touch(580, 122); pump(3); release(); pump(6);     // Recovery words row -> warning again
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
    touch(580, 122); pump(3); release(); pump(6);   // Recovery words row -> warning
    touch(168, 430); pump(3); release(); pump(6);   // SHOW THE WORDS
    save("/tmp/sim_words24_p1.ppm");                // 1-12 / 24, NEXT but no BACK
    touch(278, 430); pump(3); release(); pump(6);   // NEXT
    save("/tmp/sim_words24_p2.ppm");                // 13-24 / 24, BACK but no NEXT
    touch(118, 430); pump(3); release(); pump(6);   // BACK -> page 1 again
    save("/tmp/sim_words24_back.ppm");
    touch(680, 430); pump(3); release(); pump(6);   // DONE -> Settings
    snprintf(s_sim_seed, sizeof s_sim_seed, "%s", save_seed);
  }

  touch(667, 40); pump(3); release(); pump(6);      // language pill, TOP right now -> picker
  save("/tmp/sim_lang_picker.ppm");                 // 21 locale choices, current selected
  {                                                 // re-pick the ACTIVE language so a
    int li = wallet_lang_pick_slot(i18n_get_lang()); // SIM_LANG walk stays in its locale
    touch(16 + (li % 3) * 260 + 124, 76 + (li / 3) * 52 + 22);
    pump(3); release(); pump(10);                   // settings rebuilt, same language
  }
  // ADDRESS TYPE is a full-width subpage now: the settings row opens it, and
  // the pick happens there. Walking both halves keeps a broken chooser from
  // hiding behind a frame that only ever showed the list.
  touch(200, 188); pump(3); release(); pump(6);     // Address type row -> chooser
  save("/tmp/sim_addr_type.ppm");                   // 3 names + notes, NATIVE selected
  touch(400, 122); pump(3); release(); pump(6);     // pick LEGACY -> back to settings
  save("/tmp/sim_settings_legacy.ppm");             // the row now reads Legacy / 1...
  // 198, not 254. The Address card spans y=166..230; 254 was inside the
  // STORAGE card below it, so this reopened the storage chooser and the walk
  // spent the next six taps on the wrong subpage. It passed the tap gate
  // anyway, because every frame it landed on still differed from the one
  // before -- moving Settings BACK across the bar is what finally made the
  // misroute visible.
  touch(218, 198); pump(3); release(); pump(6);     // Address card -> reopen the chooser
  touch(400, 308); pump(3); release(); pump(6);     // back to NATIVE
  touch(533, 426); pump(3); release(); pump(4);     // theme dot in the action bar: CYPHERPINK
  save("/tmp/sim_settings_pink.ppm");               // accent recolors selections+title
  touch(680, 430); pump(3); release(); pump(6);      // BACK, right corner -> home still pink
  save("/tmp/sim_wallet_pink.ppm");
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(479, 426); pump(3); release(); pump(4);     // theme dot in the action bar: back to MONO
  // The Network row is a SEGMENTED control now, so a tap on the row itself
  // does nothing -- you pick a side. TESTNET is the right lozenge: the card
  // starts at y=95, the track is centred in its 64 height and the lozenges sit
  // 3px inside that, so 291..375 x 113..141. This is its centre.
  touch(333, 127); pump(3); release(); pump(4);     // TESTNET segment
  save("/tmp/sim_settings_tn.ppm");
  touch(680, 430); pump(3); release(); pump(6);      // BACK, right corner -> home
  save("/tmp/sim_wallet_testnet.ppm");              // home now shows TESTNET badge
  touch(310, 240); pump(3); release(); pump(6);     // Receive: tb1 detail landing
  save("/tmp/sim_recv_tn.ppm");                     // detail, on testnet
  // The list is one pill away now. Capture it on testnet so the tb1 rows and
  // page counter render at least once outside the fresh-landing default.
  touch(255, 430); pump(3); release(); pump(6);     // ALL ADDRESSES -> list
  save("/tmp/sim_recv_detail_tn.ppm");              // reused filename: now the list
  touch(680, 430); pump(3); release(); pump(6);     // BACK from list -> home
  touch(310, 240); pump(3); release(); pump(6);     // Receive again -> detail
  // The testnet silent-payment address is one character longer than mainnet
  // (tsp1 vs sp1) and was the only receive QR the walk never rendered, which
  // is where a truncation report landed. Capture both sizes so their decoded
  // payloads can be compared byte-for-byte.
  touch(455, 430); pump(3); release(); pump(6);     // SILENT PAYMENT (testnet)
  save("/tmp/sim_recv_sp_tn.ppm");                  // folded tsp1, prefix skipped correctly
  touch(196, 248); pump(3); release(); pump(6);     // longest receive payload -> zoom
  save("/tmp/sim_recv_sp_zoom_tn.ppm");
  touch(763, 35); pump(3); release(); pump(6);
  touch(188, 430); pump(3); release(); pump(6);     // SHOW FULL: longest full form
  save("/tmp/sim_recv_sp_full_tn.ppm");
  touch(188, 430); pump(3); release(); pump(6);     // SHOW SHORT before opening help
  // The explainer names the prefixes, so testnet renders a different title
  // ("WHY YOU SEE TB1P") and two more characters of body than mainnet does.
  // Capture the longer one: it is the variant that would overflow first.
  touch(730, 50); pump(3); release(); pump(30);     // ? -> tsp1/tb1p explanation
  save("/tmp/sim_recv_sp_help_tn.ppm");
  touch(400, 418); pump(3); release(); pump(6);     // OK closes the explanation
  touch(680, 430); pump(3); release(); pump(6);     // BACK from SP -> detail
  touch(100, 430); pump(3); release(); pump(4);     // BACK from detail (leftmost) -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign -> chooser
  touch(218, 340); pump(3); release(); pump(6);     // FROM SD
  touch(328, 150); pump(3); release(); pump(8);     // file -> verify: TESTNET row
  save("/tmp/sim_verify_tn.ppm");
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the chooser
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(247, 127); pump(3); release(); pump(4);     // MAINNET segment: flip back
  touch(680, 430); pump(3); release(); pump(4);      // BACK, right corner -> home

  // step 7: seed wizard — lock, wipe the seed, KISS again -> first-boot flow
  touch(44, 44); pump(3); release(); pump(20);     // KISS logo -> lock -> menu
  wallet_seed_wipe();                               // pretend a factory-fresh device
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(4);
  // KNOWN DEFECT, worked around here rather than papered over: the last letter
  // of KISS is what makes the wallet appear, so the setup chooser is built with
  // a finger still down, and LVGL resolves that press onto whatever now sits
  // under it. This stroke ends at (462, 272), inside CREATE A NEW WALLET, so
  // the gesture picks an option by itself and the walk lands on STORAGE.
  //
  // The 340px pills these rows replaced escaped it by luck -- they stopped at
  // x=388. A 716 wide row leaves nowhere harmless for a finger to end up, which
  // is what turned an accident into a certainty. lv_indev_reset,
  // lv_indev_wait_release and a z-ordered shield in wt_screen were all tried;
  // none holds, because LVGL re-resolves the still-pressed point on the
  // following cycle. The fix belongs in the gesture recogniser, which already
  // carries an s_wallet_swallow flag for the decoy's version of this and does
  // not set it on this path.
  //
  // So BACK out of the screen the gesture chose and photograph the chooser it
  // should have landed on. Deterministic: the stroke always ends in the same
  // place, so it always picks the same row.
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the chooser
  save("/tmp/sim_setup_choose.ppm");                // NEW / RESTORE chooser

  // peek at RESTORE: word entry + autocomplete, then back out
  touch(218, 240); pump(3); release(); pump(4);     // RESTORE FROM WORDS (row 1)
  save("/tmp/sim_setup_storage.ppm");               // FLASH / SD CARD / AMNESIC
  touch(174, 144); pump(3); release(); pump(4);     // FLASH
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
  if (s_sim_pending_mode != -1) {
    fprintf(stderr, "setup cancel left storage mode staged\n");
    return 1;
  }

  // the real path: CREATE SEED, simulated entropy, quiz, login twice.
  // Creating no longer asks how many words -- it is always 12 -- so FLASH
  // lands straight on the entropy screen, and the reveal is one page.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(4);     // FLASH
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

  // shift semantics. Row 3 is [shift z x c v b n m BKSP] at y=355; shift
  // x=46, z x=135, backspace x=752. The shift key is a chevron in the lower
  // and upper planes and a padlock once caps is locked, so these frames are
  // checked by looking at them, not by reading a label out of the source.
  touch(135, 277); pump(3); release(); pump(3);      // 's': catch the feedback live
  lv_refr_now(NULL);
  save("/tmp/sim_kb_feedback.ppm");                 // key flash + risen callout
  pump(30); touch(752, 355); pump(3); release(); pump(3);
  touch(46, 355); pump(3); release(); pump(4);      // shift once -> one-shot upper
  save("/tmp/sim_kb_shift_on.ppm");                 // upper plane, shift key LIT
  touch(135, 355); pump(3); release(); pump(4);     // Z
  save("/tmp/sim_kb_shift_off.ppm");                // dropped BACK to lowercase
  touch(46, 355); pump(2); release(); pump(2);      // two taps inside 400ms
  touch(46, 355); pump(2); release(); pump(4);
  save("/tmp/sim_kb_caps.ppm");                     // locked: key is the padlock
  touch(135, 355); pump(3); release(); pump(4);     // Z, and the plane MUST stay
  save("/tmp/sim_kb_caps_stays.ppm");
  touch(46, 355); pump(3); release(); pump(4);      // caps off
  // HOLD the shift key = caps lock. This is the gesture people actually reach
  // for; the double tap above is the alternative, not the only way in.
  touch(46, 355); pump(40); release(); pump(4);
  save("/tmp/sim_kb_hold_caps.ppm");                // key must be the padlock
  touch(135, 355); pump(3); release(); pump(4);     // Z, and the plane MUST stay
  save("/tmp/sim_kb_hold_caps_stays.ppm");
  // a SLOW tap on the padlock unlocks; it must NOT also register as a hold and
  // relock
  touch(46, 355); pump(40); release(); pump(4);
  save("/tmp/sim_kb_caps_slow_off.ppm");            // back to the unlit chevron
  touch(135, 355); pump(40); release(); pump(4);    // HOLD z -> Z, still lowercase
  save("/tmp/sim_kb_hold.ppm");

  // The two symbol planes: "123" in from the letters, "#+=" across to the
  // second plane, "123" back to the first, "abc" home. Four transitions, and
  // nothing exercised any of them until now, so the plane keys could have been
  // renamed into nothing and every other frame here would still have passed.
  // A passphrase can contain any printable ASCII, and a character the keyboard
  // cannot reach is a wallet that cannot be reopened.
  touch(70, 430); pump(3); release(); pump(4);      // 123 -> symbols
  save("/tmp/sim_kb_sym.ppm");
  touch(46, 277); pump(3); release(); pump(3);      // '!'
  touch(46, 355); pump(3); release(); pump(4);      // #+= -> second plane
  save("/tmp/sim_kb_sym2.ppm");
  touch(580, 277); pump(3); release(); pump(3);     // '{'
  touch(46, 355); pump(3); release(); pump(4);      // 123 -> back to the first
  save("/tmp/sim_kb_sym_back.ppm");
  touch(70, 430); pump(3); release(); pump(4);      // abc -> letters
  save("/tmp/sim_kb_sym_abc.ppm");
  touch(752, 355); pump(3); release(); pump(3);     // drop the two symbols again so
  touch(752, 355); pump(3); release(); pump(3);     // the clear below still empties
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
  touch(622, 430); pump(3); release(); pump(8);     // TAP TO OPEN -> passphrase warning
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
  // No TAP TO OPEN tap here. The OK above already lands on the verified warning
  // -- fp_tap_cb goes straight to setup_warn_screen in setup mode -- so the tap
  // that used to sit here was aimed at a screen that had already been left. It
  // survived because (400, 414) fell in the dead gap between VERIFY FULL BACKUP
  // and I UNDERSTAND and did nothing at all. TAP TO OPEN is at the standard
  // right corner now, which is where I UNDERSTAND is, so the same dead tap
  // became a live one and skipped the screen this frame exists to photograph.
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

  // Drop the LVGL indev first, so what follows is a COLD open of the decoy:
  // the state a real device is in at power-on, and the one that shipped broken.
  // The indev is created lazily and the decoy is the only way in that does not
  // pass through the login screen or the wizard, so it arrived without one: the
  // home drew, the game still worked (it reads the touch controller directly),
  // and every LVGL sub-screen below was deaf. Settings looked frozen.
  //
  // This walk missed it because it always ran a full setup first and so was
  // never cold by the time it got here. The taps after this line are the test:
  // check_sim_taps.py fails on an interaction that does not change the screen,
  // which is precisely what a wallet with no indev produces.
  wallet_ui_drop_indev_for_test();

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
  touch(44, 44); pump(3); release(); pump(20);     // KISS logo -> lock, back to the game

  // KISS **plus the configured stroke** must reach the PASSPHRASE login, not the
  // spare. This is the case that shipped broken and that nothing here covered:
  // detect_KISS fired on the LIFT OF THE LAST S, so the decoy opened before the
  // modifier could be drawn and the owner's stroke was unreachable. The chooser
  // screens never caught it because they capture strokes themselves rather than
  // going through the game's recognizer.
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(3);
  // the UNDERLINE the wizard configured, under the word, well inside the
  // KISS_OPEN_DELAY_MS the word waits out before settling for the spare
  for (int i = 0; i <= 30; i++) { touch(152 + i * 13, 336); pump(1); }
  // 45 frames, not 20. The owner's door no longer opens on the lift of the
  // modifier stroke: main.c holds it for KISS_OPEN_DELAY_MS so it cannot be
  // told apart from the decoy by how fast the screen arrives. 20 frames is
  // 320ms, which is inside that wait, so this shot used to catch the menu and
  // every step after it shifted by one. That surfaces as dozens of overlap
  // findings on later screens rather than as a failure here, so keep the slack.
  release(); pump(45);
  save("/tmp/sim_real_login.ppm");                  // passphrase keyboard, NOT a wallet home
  touch(46, 278); pump(3); release(); pump(3);      // 'a'
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  touch(622, 430); pump(3); release(); pump(140);   // TAP TO OPEN -> home

  // step 9: WIPE WALLET — arm (red), confirm, ERASED screen, OK -> game menu
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile
  // the WIPE pill is wallet_settings.c's mk_pillh(430, 310, 340, 52)
  touch(580, 356); pump(4); release(); pump(8);     // Erase this wallet -> confirm screen
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
  touch(622, 430); pump(3); release(); pump(140);   // TAP TO OPEN -> home
  save("/tmp/sim_amnesic_home.ppm");                // an amnesic wallet, unlocked

  // Move the live RAM wallet to SD, lock, then remove the card. KISS must land
  // on INSERT WALLET SD CARD -- never on first-boot setup. A failed retry stays
  // there; reinserting the card advances to the ordinary passphrase screen.
  wallet_seed_move_to(WSEED_MODE_SD);
  s_sim_sd_present = 0;
  touch(44, 44); pump(3); release(); pump(20);      // explicit lock -> game
  draw_kiss();
  save("/tmp/sim_sd_missing.ppm");
  touch(168, 430); pump(3); release(); pump(8);      // TRY AGAIN, still absent
  save("/tmp/sim_sd_missing_retry.ppm");            // still the missing-card gate
  s_sim_sd_present = 1;
  touch(168, 430); pump(3); release(); pump(12);     // hot-plug retry -> login
  save("/tmp/sim_sd_reinserted_login.ppm");

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
  // A sign screen that was replaced without being deleted stays parented under
  // its replacement, invisible, until a BACK peels the top one off and drops
  // the owner back on a transaction they already left. No saved frame shows it
  // -- the walk's own diff between a good build and a leaking one was byte
  // identical across all 177 frames -- so wallet_sign.c counts it instead and
  // this is where the count is answered.
  {
    extern int g_sign_orphaned_screens;
    if (g_sign_orphaned_screens) {
      printf("FAIL: %d orphaned sign screen(s) left parented during the walk\n",
             g_sign_orphaned_screens);
      return 1;
    }
    printf("ok: no orphaned sign screens\n");
  }
  printf("sim done\n");
#ifdef OVERLAPCHECK
  return oc_report();
#else
  return 0;
#endif
}
