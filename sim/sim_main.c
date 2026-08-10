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
#include <dirent.h>
#include <unistd.h>
#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_proof.h"   // WPROOF_NAME + the stubbed proof pipeline below
#include "platform_sd.h"    // the proof stub writes a real (small) file
#include "verify_page.h"    // ...and the real checker page beside it
#include "sha256/sha256.h"  // cUR's, real hash for the stub's junk
#include "wallet_duress_ui.h"   // the no-passphrase stop, unreachable by tapping
#include "wallet_fw.h"          // the SD firmware seams: no flash here, no key
#include "wallet_fw_ui.h"       // its screens, opened directly like the above
#include "wallet_duress.h"
#include "wallet_gword.h"      // WDG_* , to reach ST_INTRO's configured state
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

// Entropy source seam. On the device wallet_trng_start switches the SAR ADC
// noise source on and the flag records that it happened; on the host the
// callers reach /dev/urandom, which needs no switch, so the flag is the whole
// implementation. It is still a flag rather than a constant true, because the
// Settings footer reports it and the walk should render the state a booted
// device is in, not the state of a process that skipped boot.
static bool s_sim_trng;
void wallet_trng_start(void) { s_sim_trng = true; }
bool wallet_trng_live(void) { return s_sim_trng; }

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
// PROVE IT (main/wallet_proof.c wants wally SHA256; the sim links no wally).
// The stub writes a SMALL real file AND the real checker page through the real
// platform_sd so the walk's SD gate and the host directory stay honest, and
// hashes the junk with cUR's already-linked SHA256 -- still deterministic, but
// now dropping /tmp/simsd/kiss-proof.bin on the page (or scanning the sim's
// QR) shows MATCH instead of a confusing MISMATCH. Words stay the fixed
// SIM_WORDS; kisstest runs the real pipeline against a pinned vector.
int wallet_proof_run(const uint8_t *frame, size_t len, uint8_t hash_out[32],
                     char *words_out, size_t words_len) {
  (void)frame; (void)len;
  uint8_t junk[64];
  for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 3 + 1);
  if (platform_sd_mount() != 0 ||
      platform_sd_write_atomic(WPROOF_NAME, junk, sizeof junk) < 0)
    return WPROOF_ERR_SD;
  CRYAL_SHA256_CTX cx;
  ur_bundled_sha256_init(&cx);
  ur_bundled_sha256_update(&cx, junk, sizeof junk);
  ur_bundled_sha256_final(&cx, hash_out);
  // Same shape the device writes: the page with this run's hash over the claim
  // slot, so the sim's card really does self verify when opened in a browser.
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash_out[i]);
  uint8_t *page = malloc(verify_page_html_len);
  if (!page) return WPROOF_ERR_SD;
  memcpy(page, verify_page_html, verify_page_html_len);
  uint8_t *slot = memmem(page, verify_page_html_len, WPROOF_CLAIM_SLOT, 64);
  if (slot) memcpy(slot, hex, 64);
  int prc = platform_sd_write_atomic(WPROOF_PAGE_NAME, page,
                                     verify_page_html_len);
  free(page);
  if (prc < 0) return WPROOF_ERR_SD;
  return wallet_seed_from_entropy(hash_out, 32, words_out, words_len);
}
// Source 3 (taps) + the three-way mix. The real fold lives in wallet_tapent.c
// and wallet_crypto.c and needs wally's SHA256; the sim links no crypto, same
// reason as the seed stub above. Here they only have to let the tap screen
// advance and complete. kisstest exercises the real versions. 64 == WTAP_TARGET.
static unsigned s_sim_taps;
void wallet_tapent_reset(void) { s_sim_taps = 0; }
int wallet_tapent_tap(uint64_t us, uint32_t cyc, int16_t x, int16_t y) {
  (void)us; (void)cyc; (void)x; (void)y;
  if (s_sim_taps >= 64) return 0;
  s_sim_taps++;
  return 1;
}
unsigned wallet_tapent_count(void) { return s_sim_taps; }
int wallet_tapent_take(uint8_t out[32]) {
  if (s_sim_taps < 64) return -1;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(i * 7);
  return 0;
}
// Dice source (verifiable path). Real logic + SHA256 live in wallet_dice.c and
// are exercised by kisstest; the sim links no crypto, so this stub only has to
// let the dice screen advance and complete. The QUALITY judge is not stubbed:
// wallet_dice_q.c needs no crypto and links for real, so the walk renders true
// verdicts against this buffer — a fake WD_Q_OK would leave the warning screen
// unrendered by every gate, which is exactly the drift this walk exists to
// catch. Constants come from the header so a raised DICE_MAX cannot silently
// cap the sim under the screen it is walking.
#include "wallet_dice.h"
static char     s_sim_dice[DICE_MAX + 1];
static unsigned s_sim_dn;
void wallet_dice_reset(void) { s_sim_dn = 0; s_sim_dice[0] = 0; }
int wallet_dice_roll(int face) {
  if (face < 1 || face > 6) return 0;
  if (s_sim_dn >= DICE_MAX) return 0;
  s_sim_dice[s_sim_dn++] = (char)('0' + face);
  s_sim_dice[s_sim_dn] = 0;
  return 1;
}
int wallet_dice_undo(void) {
  if (s_sim_dn == 0) return 0;
  s_sim_dice[--s_sim_dn] = 0;
  return 1;
}
unsigned wallet_dice_count(void) { return s_sim_dn; }
const char *wallet_dice_digits(void) { return s_sim_dice; }
int wallet_dice_take(uint8_t *out, unsigned len) {
  if (!out || (len != 16 && len != 32)) return -1;
  unsigned floor = (len == 32) ? DICE_FLOOR_256 : DICE_FLOOR_128;
  if (s_sim_dn < floor) return -1;
  for (unsigned i = 0; i < len; i++) out[i] = (uint8_t)(i * 3 + 1);
  return 0;
}
int wallet_dice_peek(uint8_t out[32]) {
  if (!out) return -1;
  // no real SHA in the sim: a value that visibly moves as rolls are added, so
  // the fingerprint label can be walked and shot for the docs.
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(i + s_sim_dn);
  return 0;
}
// Last word source (cards path). The real enumeration needs wally's BIP39
// validator and is exercised by kisstest; the walk only needs the right SHAPE:
// 8 candidates after a 23 word prefix, 128 after 11, real-looking labels.
#include "wallet_lastword.h"
int wallet_lastword_candidates(const char *partial, uint16_t out[WLAST_MAX]) {
  if (!partial) return -1;
  int words = 1;
  for (const char *p = partial; *p; p++)
    if (*p == ' ') words++;
  if (words != 11 && words != 23) return -1;
  int n = words == 23 ? 8 : 128;
  for (int i = 0; i < n; i++) out[i] = (uint16_t)i;
  return n;
}
const char *wallet_lastword_word(uint16_t i) {
  return i < 2048 ? SIM_WORDS[i % 24] : NULL;
}
// The walk's 24 fake words stand in for the 2048 word list, so their INDICES
// have to spread the way real ones do: 83 apart is far wider than WC_NEAR, so
// an ordinary typed set judges clean and the flagged shapes stay exactly where
// the walk puts them on purpose. index() and word() are deliberately NOT
// inverses here -- nothing in the flow round trips them, the judge and the
// checksum card only read index(), the picker only reads word() -- and
// kisstest owns the real pair. i*83 runs to four digits too, so the widest
// number chip the card can draw is the one the walk already draws.
int wallet_lastword_index(const char *w) {
  for (int i = 0; i < 24; i++)
    if (strcmp(SIM_WORDS[i], w) == 0) return i * 83;
  return -1;
}
int wallet_entropy_mix3(const uint8_t a[32], const uint8_t b[32],
                        const uint8_t c[32], uint8_t out[32]) {
  if (!a || !b || !c || !out) return -1;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(a[i] ^ b[i] ^ c[i]);
  return 0;
}
// Stands in for the camera on the dead-lens path, so the walk has to link it.
// The real one measures two clocks against each other and kisstest exercises
// that; a scripted walk has no clocks worth measuring, so this only has to be
// non-constant and succeed. Nothing here is entropy and nothing here claims to
// be -- the walk never keeps a seed.
int wallet_jitter(uint8_t out[32]) {
  if (!out) return -1;
  static uint32_t s = 0x2545F491u;
  for (int i = 0; i < 32; i++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    out[i] = (uint8_t)(s & 0xFF);
  }
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
//
// The screen-walk sim always runs the beta lane. The settable version of this
// lives in main/wallet_seed.c, which is what the unit test binary links.
int wallet_seed_flash_encrypted(void) { return 0; }

// The screen walk creates seeds through the same funnel the device uses, so it
// reaches the entropy note. RAM here: the walk is one process and there is no
// boot for it to survive.
// SIM_ENT_NOTE=1 starts the walk with a flagged seed, which is the only way the
// overlap gate ever renders the second chip on the recovery words page. Without
// it the walk creates clean seeds and the widest version of that chip line --
// two self sizing chips beside each other, in 21 locales -- is never measured.
static int s_sim_ent_note = -1;
void wallet_seed_set_entropy_note(int v) { s_sim_ent_note = v; }
int wallet_seed_entropy_note(void) {
  if (s_sim_ent_note < 0) {
    const char *e = getenv("SIM_ENT_NOTE");
    s_sim_ent_note = (e && *e && *e != '0') ? 2 /* WD_Q_UNEVEN */ : 0;
  }
  return s_sim_ent_note;
}
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
  } else if (len >= 5 && memmem(bytes, len, "UNPRV", 5)) {
    // The new caution on its own: a two-input spend whose amounts were declared
    // and not proved. One row, footer kept -- the ordinary shape of it.
    s->n_in = 2;
    s->n_unproven_in = 2;
    s->status = WPSBT_CAUTION;
    s->caution_flags = WPSBT_C_UNPROVEN_IN;
    snprintf(s->reason, sizeof s->reason,
             "input amounts not proven - fee may be higher");
  } else if (len >= 5 && memmem(bytes, len, "COMBO", 5)) {
    // Every caution at once: proves the summary + WHY card stack up. FIVE rows
    // is the most the verify screen can ever draw, and it is the only fixture
    // that reaches the tight row metric (SG_ROW_H5) and the dropped footer, so
    // this is where that layout gets looked at.
    s->n_in = WPSBT_MERGE_INS;
    s->n_unproven_in = WPSBT_MERGE_INS;
    s->send_sats = 3000; s->fee_sats = 800; s->change_sats = 200;
    s->outs[0].sats = 3000; s->outs[1].sats = 200; s->in_sats = 4000;
    s->fee_rate_x10 = 570;
    s->status = WPSBT_CAUTION;
    s->caution_flags = WPSBT_C_HIGHFEE | WPSBT_C_DUST_INPUT |
                       WPSBT_C_DUST_CHANGE | WPSBT_C_MERGE_INS |
                       WPSBT_C_UNPROVEN_IN;
    snprintf(s->reason, sizeof s->reason, "unusually high fee, tiny coins");
  }
  return 0;
}
int wallet_psbt_details(wpsbt_details_t *d) {
  memset(d, 0, sizeof *d);
  // n_total > n_in exercises the many-inputs header (S_D_MANYIN_FMT) with a
  // 2-digit count: the longest formatted line in the whole sign flow (ja is
  // ~140 bytes) and the exact case that used to truncate in buf[128]
  // Five inputs, proven states mixed, so the page draws BOTH per-input marks
  // (a tick beside a coin a previous transaction vouched for, an eye-slash in
  // WARN beside one only claimed) AND overflows its viewport, which is what
  // keeps the always-on scrollbar honest: past two inputs the list must not
  // look like it ends at the fold.
  d->version = 2; d->locktime = 0; d->txid_final = true; d->n_in = 5; d->n_total = 17;
  snprintf(d->txid, sizeof d->txid, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
  for (uint32_t i = 0; i < 5; i++) {
    memset(d->ins[i].txid, "abcde"[i], 64);
    d->ins[i].txid[64] = 0;
    d->ins[i].vout = i; d->ins[i].sats = 100000 - i * 9750;
    d->ins[i].purpose = 84; d->ins[i].change = 0; d->ins[i].index = i;
    d->ins[i].proven = (i == 0);   // the fold hides unproven coins: scroll
  }
  return 0;
}
int wallet_psbt_sign(uint8_t *out, size_t out_len, size_t *written) {
  size_t n = out_len < 220 ? out_len : 220;
  memset(out, 0xAB, n); *written = n;
  return 0;
}
// The real fingerprint (sha256 over the signature bytes) needs wally; the sim
// links none, same as the sign stub above. A tiny deterministic hash gives the
// walk a stable, plausible code to render. kisstest covers the real function.
int wallet_psbt_sig_fingerprint(const uint8_t *b, size_t len, char out[9]) {
  if (!b || !out) return -1;
  unsigned long h = 2166136261UL;
  for (size_t i = 0; i < len; i++) { h ^= b[i]; h *= 16777619UL; }
  static const char HEX[] = "0123456789abcdef";
  for (int k = 0; k < 8; k++) out[k] = HEX[(h >> (28 - 4 * k)) & 0xf];
  out[8] = 0;
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
  // Every stop is a screen the gates get to question, so this is the one place
  // that can honestly say a screen was covered. See wt_sim_uncaptured.
  wt_sim_capture();
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

// Does any label on the live screen contain this text? The verify screen is the
// one place in the app where a missing string is a security defect rather than
// a cosmetic one, and a saved frame cannot say "the address is absent" -- it
// looks like a perfectly tidy screen that simply does not mention where the
// coins are going. So the walk asks the object tree directly.
// Spangroups too, and the address is one: wt_addr_spans splits it into a muted
// head and an accented tail, so no single label ever holds the whole string.
// Concatenating the spans is the only way to ask "is the address on screen".
static int find_label_text(lv_obj_t *o, const char *needle) {
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    if (t && strstr(t, needle)) return 1;
  }
  if (lv_obj_check_type(o, &lv_spangroup_class)) {
    char joined[512];
    size_t n = 0;
    uint32_t ns = lv_spangroup_get_span_count(o);
    for (uint32_t i = 0; i < ns && n + 1 < sizeof joined; i++) {
      const char *t = lv_span_get_text(lv_spangroup_get_child(o, (int32_t)i));
      if (!t) continue;
      n += (size_t)snprintf(joined + n, sizeof joined - n, "%s", t);
      if (n >= sizeof joined) n = sizeof joined - 1;
    }
    joined[n] = 0;
    if (strstr(joined, needle)) return 1;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    if (find_label_text(lv_obj_get_child(o, i), needle)) return 1;
  return 0;
}
// What IS on the screen, when what should be is not. A bare "no label contains
// the address" says nothing about whether the walk landed on the wrong screen,
// the right screen with the wrong numbers, or a screen that never finished
// building -- and those want three different fixes.
//
// This exists because the walk has been seen failing these checks
// nondeterministically: on 2026-08-07, four runs from a byte identical starting
// state gave 0, 3, 7 and 7 failures, and it has never been reproduced since
// (61 consecutive clean runs across idle, ASan, three allocator modes and CPU
// contention). Whatever it is, it is rare and it is not going to be caught by
// staring at the code, so the next occurrence has to explain itself.
static void dump_labels(lv_obj_t *o, int depth, int *budget) {
  if (*budget <= 0) return;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    if (t && *t) { printf("      %*s\"%s\"\n", depth, "", t); (*budget)--; }
  } else if (lv_obj_check_type(o, &lv_spangroup_class)) {
    uint32_t ns = lv_spangroup_get_span_count(o);
    printf("      %*s<spangroup %u>", depth, "", (unsigned)ns);
    for (uint32_t i = 0; i < ns; i++) {
      const char *t = lv_span_get_text(lv_spangroup_get_child(o, (int32_t)i));
      if (t) printf(" \"%s\"", t);
    }
    printf("\n");
    (*budget)--;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    dump_labels(lv_obj_get_child(o, i), depth + 1, budget);
}

static int g_walk_fails;
static void must_show(const char *what, const char *needle) {
  if (find_label_text(lv_screen_active(), needle)) return;
  printf("FAIL: %s: no label on screen contains \"%s\"\n", what, needle);
  // Only for the first failure of a run: six of these would bury the log, and
  // the first one is the one that says where the walk actually was.
  if (g_walk_fails == 0) {
    lv_obj_t *scr = lv_screen_active();
    int budget = 40;
    printf("      --- what the active screen actually holds (%u children) ---\n",
           (unsigned)lv_obj_get_child_count(scr));
    dump_labels(scr, 0, &budget);
    if (budget <= 0) printf("      ... (truncated at 40)\n");
    // And the picture, because the tree says what exists and the frame says
    // what the owner would have seen. NOT in gate builds: save() there is a
    // checkpoint call into the overlap gate, and a frame that only appears on
    // failing runs would be a stop the gate cannot compare against anything.
#ifndef OVERLAPCHECK
    save("/tmp/sim_walk_FAIL.ppm");
    printf("      --- frame written to /tmp/sim_walk_FAIL.ppm ---\n");
#endif
  }
  g_walk_fails++;
}

// The inverse, and the routing test needs it: the property being pinned is that
// a passphrase keyboard is ABSENT, which is the whole deniability claim.
static void must_not_show(const char *what, const char *needle) {
  if (!find_label_text(lv_screen_active(), needle)) return;
  printf("FAIL: %s: screen shows \"%s\" and must not\n", what, needle);
  g_walk_fails++;
}
static void release(void) { g_pressed = false; }

// The unlock word as used throughout the scripted walk. Kept as a helper for
// storage hot-plug coverage added at the end, so that test does not invent a
// second approximation of the gesture recognizer's real input.
// The word ALONE, no settling wait. Split out of draw_kiss so a modifier stroke
// can still arrive: main.c holds a bare word for KISS_OPEN_DELAY_MS precisely
// so the stroke after it is classified against the same draw, and draw_kiss's
// trailing pump(40) is 140ms past that window.
// ---- free marks, drawn as a hand would ------------------------------------
// Sampled coarser than 10px so the touch layer's decimation keeps them, and
// sized past WDF_MIN_SPAN. Positions are arbitrary on purpose: a free mark has
// no word to sit on, so anywhere on the panel has to work.
static void mark_line(void)  { for (int i = 0; i <= 10; i++) { touch(200 + i * 20, 400); pump(1); } release(); pump(2); }
static void mark_slash(void) { for (int i = 0; i <= 10; i++) { touch(180 + i * 18, 120 + i * 18); pump(1); } release(); pump(2); }
static void mark_check(void) {
  for (int i = 0; i <= 4; i++) { touch(300 + i * 10, 200 + i * 20); pump(1); }
  for (int i = 1; i <= 6; i++) { touch(340 + i * 15, 280 - i * 25); pump(1); }
  release(); pump(2);
}
static void mark_circle(void) {
  static const int cx[] = {300,420,420,300,180,180,298};
  static const int cy[] = {140,200,300,360,300,200,143};
  for (unsigned i = 0; i < sizeof cx / sizeof cx[0]; i++) {
    // walk the edge so the ink is a loop, not seven far-apart samples
    int fx = i ? cx[i-1] : cx[0], fy = i ? cy[i-1] : cy[0];
    for (int t = 1; t <= 6; t++) { touch(fx + (cx[i]-fx)*t/6, fy + (cy[i]-fy)*t/6); pump(1); }
  }
  release(); pump(2);
}

static void draw_kiss_word(void)
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
}

static void draw_kiss(void)
{
  draw_kiss_word();
  // 40 frames, not 4. Whichever door this call ends up taking, main.c may hold
  // it for KISS_OPEN_DELAY_MS so the real wallet and the decoy cannot be told
  // apart by how fast the screen arrives. At 16ms a frame this is 640ms of
  // slack over a 500ms wait. Callers that land on the setup wizard or the
  // amnesic loader open immediately and do not need it; the slack costs them
  // nothing and stops this helper breaking if a caller changes door.
  pump(40);
}

// The word, then one wide flat stroke under it: the shape sim/test_duress.c
// pins as WDG_UNDERLINE, drawn through the real touch layer so the whole path
// runs -- detect_KISS, wallet_duress_classify, unlock_kind -- and not only the
// classifier the unit test reaches on its own.
static void draw_kiss_underlined(void)
{
  draw_kiss_word();
  pump(2);
  for (int i = 0; i <= 20; i++) { touch(150 + i * 20, 315); pump(1); }
  release();
  pump(40);
}

// wallet_lock() sets s_gest_swallow so the rest of the closing tap cannot
// become the first stroke of a word, and it clears on the next lift. Without a
// throwaway lift the K's spine is eaten and the word never completes. The
// corner is chosen because the menu's "tap to play" would start the game, and
// the gesture collector is skipped entirely while ST_PLAY.
static void lock_to_menu(void)
{
  extern void wallet_wiped_lock(void);   // = wallet_lock(); it wipes nothing
  wallet_wiped_lock();
  pump(30);
  touch(6, 470); pump(2); release(); pump(6);
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

// ---- the walk owns its fixtures ----
// /tmp/simsd is platform_sd.c's SD_BASE on the host, and THREE binaries write
// into it: this walk, /tmp/kissoverlap (the same walk instrumented) and
// /tmp/kisstest (test_sdseed.c, test_fw.c and test_proof.c all put files
// there). The sign screens list that directory and the walk taps rows by
// position, so one file left behind by any of them silently shifts which PSBT
// a tap opens -- and the failure surfaces as six missing labels on a verify
// screen, which reads exactly like a layout regression in whatever change is
// being tested. That cost a long false bisect on 2026-08-07.
//
// So the walk takes ownership instead of unlinking a list of names it happens
// to remember: everything goes, then the six fixtures are written back. Same
// for the host's wallet state files, which kisstest also writes -- a leftover
// provisioned seed starts the walk on a login screen instead of first boot.
//
// Nothing outside a run depends on what a previous run left. kiss-proof.bin
// and kiss-verify.html are written DURING the walk by the proof flow, and
// sim/test_proof.c makes its own copies through wallet_proof_run before it
// reads them back, so it never needs the walk's.
//
// NOT a fix for run-to-run flakiness. The walk has been seen to give different
// results from a byte identical starting state under machine load; that is a
// separate defect and this function does not address it. What this buys is
// that the starting state is at least the same every time, so the next person
// bisecting has one fewer variable.
#define SIMSD "/tmp/simsd"

static void sim_fixture_reset(void) {
  mkdir(SIMSD, 0777);

  // Two passes: collect, close, then remove. Deleting inside the readdir loop
  // is the shape platform_sd.c avoids for the same reason -- see its comment
  // about f_readdir after f_unlink -- and there is no reason to write the
  // fragile version here just because the host happens to tolerate it.
  char doomed[64][256];
  int n = 0;
  DIR *d = opendir(SIMSD);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 64) {
      if (e->d_name[0] == '.') continue;
      snprintf(doomed[n++], sizeof doomed[0], SIMSD "/%s", e->d_name);
    }
    closedir(d);
  }
  for (int i = 0; i < n; i++) remove(doomed[i]);

  // The six the sign walk taps, and the content each one's verify screen is
  // built from (sim_main.c's wallet_psbt_load stub branches on these words).
  // The names are chosen so a plain sort puts them in the order the walk taps:
  // payment-01, risky-STOP, silly-FEE, warn-COMBO, zsp-SPAY, zzz-UNPRV.
  static const struct { const char *name, *body; } FIXTURES[] = {
    { "payment-01.psbt", "fake-psbt-binary" },
    { "risky-STOP.psbt", "STOP"  },
    { "silly-FEE.psbt",  "FEE"   },
    { "warn-COMBO.psbt", "COMBO" },
    { "zsp-SPAY.psbt",   "SPAY"  },
    { "zzz-UNPRV.psbt",  "UNPRV" },
  };
  for (unsigned i = 0; i < sizeof FIXTURES / sizeof FIXTURES[0]; i++) {
    char p[256];
    snprintf(p, sizeof p, SIMSD "/%s", FIXTURES[i].name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(FIXTURES[i].body, f); fclose(f); }
  }

  // wallet_seed.c and wallet_seed_sd.c persist to these on the host build.
  static const char *const STATE[] = {
    "/tmp/kiss_seed.txt",      "/tmp/kiss_seed.txt.tmp",
    "/tmp/kiss_seed_mode.txt", "/tmp/kiss_seed_mode.txt.tmp",
    "/tmp/kiss_seed_entq.txt", "/tmp/kiss_device_key.bin",
  };
  for (unsigned i = 0; i < sizeof STATE / sizeof STATE[0]; i++) remove(STATE[i]);
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
  sim_fixture_reset();

  // app_main does this right after the display comes up, and the Settings
  // footer reports it ("noise source"). Without it the walk would render an
  // amber OFF on a state that only means "the sim skipped boot".
  wallet_trng_start();

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

  // draw the word "KISS" -> the SPARE signer appears (K spine+arms, I, S, S)
  //
  // The bare word opens the spare and nothing else, on every device, configured
  // or not: that is wallet_duress_route, and the routing block a hundred lines
  // below asserts it directly. This frame used to be saved as sim_login.ppm
  // with a comment claiming a passphrase keyboard. It has been a wallet home
  // ever since the routing fork was closed, so the fifteen taps that followed
  // were landing on home tiles and SIGN screens while still being saved under
  // sim_login_* names -- 46,278 is the left edge of the Sign card, which is why
  // "hold 'a'" photographed the SIGN screen. check_sim_taps only caught the two
  // pairs where the stray taps happened to change nothing.
  //
  // Recorded as well as captured: see save_seq(). A beat on the untouched menu
  // first, so the GIF opens on the thing everyone else sees. The GIF stays on
  // the bare word on purpose -- it is the demo, and the modifier stroke is not
  // a thing to teach in a loop anyone can watch.
  g_seq_on = 1; pump(8);
  draw_kiss();                                      // = the word; helper waits out KISS_OPEN_DELAY_MS
  pump(5); g_seq_on = 0;                            // hold on the reveal, then stop recording
  save("/tmp/sim_spare_home.ppm");                  // the spare, opened by the word alone

  // The passphrase keyboard takes the word AND the modifier stroke. Nothing
  // else reaches it, so everything below has to come in that way.
  lock_to_menu();
  draw_kiss_underlined();
  pump(10);
  save("/tmp/sim_login.ppm");                       // the passphrase keyboard

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
  // 6, not 3. Repeated taps on the SAME key need a press long enough for the
  // indev to sample it as its own click: at pump(3) this loop entered 22 of its
  // 44 characters and at pump(4) it entered 30, so the frame below was labelled
  // "44 chars, 14pt" while photographing 22 at font23. Measured, not guessed --
  // 5 is where all 44 arrive and 6 keeps a frame of slack. Nothing here says
  // anything about the device: it is how fast this harness can inject a press,
  // and a real controller reports continuously while a finger is down.
  for (int i = 0; i < 44; i++) { touch(46, 278); pump(6); release(); pump(6); } // 44 chars
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


  // ---- unlock routing: one answer, whatever is configured ----
  //
  // The property both duress findings came down to. The device used to fork on
  // wallet_duress_real(): a configured device opened the decoy on a bare word,
  // an unconfigured one showed a passphrase keyboard. Drawing the word ONCE
  // told an attacker holding the device which kind it was. The byte in flash
  // was never the leak; the behaviour was.
  //
  // wallet_duress_route is unit tested in sim/test_duress.c, but that proves
  // the rule in isolation. This drives the whole path through the real touch
  // layer, because unlock_kind lives in main.c and no test binary links it --
  // which is exactly how the fork survived long enough to become a finding.
  //
  // HERE, and not at the end of the walk, because wallet_lock() returns early
  // unless s_wallet_on. By the tail a login screen sits on top with the wallet
  // already closed, so the menu never comes forward, the touches land on the
  // keyboard and not one point of ink reaches the collector. This is the last
  // point where the home is genuinely open. The block restores the session it
  // borrows, so every frame after it is unaffected.
  {
    extern int g_last_unlock_kind;
    const int cfgs[] = { WDG_UNDERLINE, WDG_NONE };
    for (unsigned c = 0; c < 2; c++) {
      wallet_duress_set(cfgs[c]);
      const char *tag = cfgs[c] == WDG_NONE ? "routing/no-stroke"
                                            : "routing/stroke-set";

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_kiss();
      if (g_last_unlock_kind != WDR_DECOY) {
        printf("FAIL: %s: word alone routed %d, expected WDR_DECOY (%d)\n",
               tag, g_last_unlock_kind, WDR_DECOY);
        g_walk_fails++;
      }
      must_not_show(tag, tr(STR_L_TYPE_PROMPT));

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_kiss_underlined();
      if (g_last_unlock_kind != WDR_REAL) {
        printf("FAIL: %s: word + stroke routed %d, expected WDR_REAL (%d)\n",
               tag, g_last_unlock_kind, WDR_REAL);
        g_walk_fails++;
      }

      // The REAL route left the passphrase keyboard up, and lock_to_menu is a
      // no-op while it is (wallet_lock returns early unless s_wallet_on). So
      // finish the same login the walk used to get here -- 'a', OK, TAP TO OPEN
      // -- and every pass, including the last, ends on the open home.
      touch(46, 278);  pump(3); release(); pump(3);
      touch(725, 430); pump(3); release(); pump(25);
      touch(622, 430); pump(3); release(); pump(12);
      pump(120);
    }
    wallet_duress_set(WDG_NONE);

    // ---- a written word replaces KISS outright ----
    //
    // The whole point of the feature, driven through the real touch layer:
    // main.c slices the draw by stroke, rebuilds a template from the first
    // strokes and compares it to the stored one. None of this is reachable
    // from a unit test, because unlock_kind lives in main.c and no test binary
    // links it -- the same gap that let the routing fork survive.
    {
      // Teach it the word by the same route the enrolment screen uses: build a
      // template from a real draw rather than hand-filling the struct, so the
      // walk exercises gw_make on the panel's own decimated ink.
      extern int sim_capture_word(gw_template_t *out);
      lock_to_menu();
      draw_kiss_word();                 // the letters KISS, as any word would be
      pump(4);
      gw_template_t t;
      if (sim_capture_word(&t) != 0 || gw_stored_set(&t) != 0) {
        printf("FAIL: written word: could not teach the device a word\n");
        g_walk_fails++;
      }
      pump(200);                        // let the collector's idle clear run

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_kiss();                      // the same word, no mark after it
      if (g_last_unlock_kind != WDR_DECOY) {
        printf("FAIL: written word: the word alone routed %d, expected WDR_DECOY (%d)\n",
               g_last_unlock_kind, WDR_DECOY);
        g_walk_fails++;
      }
      must_not_show("written/word-alone", tr(STR_L_TYPE_PROMPT));

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_kiss_underlined();           // the word plus one mark
      if (g_last_unlock_kind != WDR_REAL) {
        printf("FAIL: written word: word plus a mark routed %d, expected WDR_REAL (%d)\n",
               g_last_unlock_kind, WDR_REAL);
        g_walk_fails++;
      }
      touch(46, 278);  pump(3); release(); pump(3);
      touch(725, 430); pump(3); release(); pump(25);
      touch(622, 430); pump(3); release(); pump(12);
      pump(120);

      // And the sentence the feature exists to make true: with a word stored,
      // a DIFFERENT draw opens nothing at all. Two plain strokes here, nothing
      // like the stored shape and nowhere near its stroke count.
      lock_to_menu();
      g_last_unlock_kind = -2;
      mark_line(); mark_slash();
      pump(40);
      if (g_last_unlock_kind != -2) {
        printf("FAIL: written word: a wrong draw routed %d\n", g_last_unlock_kind);
        g_walk_fails++;
      }
      must_not_show("written/wrong-word", tr(STR_L_TYPE_PROMPT));

      // Put it back, and hand the walk the session it expects.
      gw_stored_set(NULL);
      pump(200);
      draw_kiss_underlined();
      touch(46, 278);  pump(3); release(); pump(3);
      touch(725, 430); pump(3); release(); pump(25);
      touch(622, 430); pump(3); release(); pump(12);
      pump(120);
      printf("ok: a written word replaces KISS, and a wrong draw opens nothing\n");
    }

    // Identical expectations for both configurations is the whole test: if
    // either arm ever needs a different one, the fork is back.
    printf("ok: unlock routing identical with and without a stroke configured\n");

    // No restore needed after the loop: each pass ends on the open home the
    // walk expects, because the login completion sits inside it.
  }

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
  touch(530, 366); pump(3); release(); pump(6);     // SILENT PAYMENT row -> SP address view
  save("/tmp/sim_recv_sp.ppm");                     // folded text + largest receive QR
  touch(196, 248); pump(3); release(); pump(6);     // QR -> full-screen scan view
  save("/tmp/sim_recv_sp_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom, exact state preserved
  touch(520, 166); pump(3); release(); pump(6);     // folded address itself -> full
  save("/tmp/sim_recv_sp_full.ppm");
  touch(612, 430); pump(3); release(); pump(6);     // SHOW SHORT -> folded default
  touch(730, 50); pump(3); release(); pump(30);     // ? -> sp1/bc1p explanation
  save("/tmp/sim_recv_sp_help.ppm");
  touch(400, 418); pump(3); release(); pump(6);     // OK closes the explanation
  touch(118, 430); pump(3); release(); pump(6);     // BACK from SP -> detail again
  touch(530, 300); pump(3); release(); pump(6);     // ALL ADDRESSES row -> the list
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
  touch(365, 430); pump(3); release(); pump(4);     // NEXT ADDRESS pill -> next index
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
  touch(682, 430); pump(3); release(); pump(6);     // NEXT -> HOW TO PAIR
  save("/tmp/sim_pair_steps.ppm");
  touch(118, 430); pump(3); release(); pump(6);     // BACK -> the QR page
  // SCAN KEY is no longer buried in the pair screen: it is a top-level ROW in
  // the WALLET screen's COORDINATOR column, so back out of pairing first. It was
  // moved because hiding a separate PRIVATE-key export one tap inside the
  // descriptor flow implied the two were the same action.
  touch(118, 430); pump(3); release(); pump(6);     // BACK (leftmost pill) -> WALLET
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
  touch(218, 296); pump(5);                        // FROM SD CARD (row 1)
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
  // The chip trails the measured mono23 code now (wallet_sign.c draw_sig_fp),
  // so its centre moved right when the code grew from mono14. The old tap at
  // (508,262) landed in the gap between code and chip, silently captured the
  // signed screen under this stop's name, and nothing failed: a wrong tap that
  // opens nothing is invisible to every check but a person looking at the frame.
  touch(552, 276); pump(3); release(); pump(6);     // ? beside SIGNATURE -> explainer
  save("/tmp/sim_sign_sigcheck.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> signed screen again
  touch(400, 430); pump(3); release(); pump(6);     // DONE -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 296); pump(3); release(); pump(6);     // FROM SD CARD (row 1)
  touch(328, 216); pump(3); release(); pump(8);     // the STOP file -> blocked verify
  save("/tmp/sim_sign_stop.ppm");
  // BACK is ONE STEP now: from a transaction it returns to the list that
  // transaction came from, not to the home screen. The three files below are
  // opened one after another without ever leaving SIGN, which is the whole
  // point -- picking the wrong file used to cost the entire trip back in.
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  save("/tmp/sim_sign_back_files.ppm");
  // payment-01.psbt was signed a few taps ago, so its row must now read SIGNED
  // ALREADY in amber and REMOVE SIGNED must have appeared in the action row.
  // That is the whole bug: the badge used to be computed from the row's OWN
  // name, so the source stayed grey UNSIGNED with its signature sitting right
  // beneath it, and there was no way to see what had already been done.
  // tr(), not the English: this walk runs in all 21 locales.
  must_show("file list after signing", tr(STR_S_SIGNED_ALREADY));
  must_show("file list after signing", tr(STR_S_RM_SIGNED));
  // REMOVE SIGNED is at 48..388 x 404..456; this is its centre.
  touch(218, 430); pump(3); release(); pump(8);
  save("/tmp/sim_sign_rm_list.ppm");                // one row per signed file
  // Row 0's own hold pill: rows start at y=132, 64 tall, pill at local (543,12)
  // 170x40, so 567..737 x 144..184. A tap is NOT enough.
  touch(652, 164); pump(2); release(); pump(4);
  save("/tmp/sim_sign_rm_noop.ppm");                // still the list, nothing gone
  touch(652, 164); pump(40);                        // hold: partial red sweep
  lv_refr_now(NULL);
  save("/tmp/sim_sign_rm_holding.ppm");
  release(); pump(6);                               // let go early -> nothing happened
  save("/tmp/sim_sign_rm_letgo.ppm");               // still the list, unchanged
  touch(680, 430); pump(3); release(); pump(8);     // BACK, keep the fixtures
  save("/tmp/sim_sign_rm_back.ppm");                // back to the file list
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
  // THE regression. Five cautions used to replace the output panels outright,
  // so the destination vanished from the transactions the device trusted least
  // -- and a coordinator could induce exactly that by padding the input count
  // or leaving the previous transactions off. The frame above proves it looks
  // right; these three prove the facts are actually on it.
  must_show("verify (5 cautions)", "bc1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz");
  must_show("verify (5 cautions)", "200 sats");       // the change amount
  must_show("verify (5 cautions)", "800 sats");       // the fee, in sats
  // Five cautions and the recipient address on the SAME screen. This frame is
  // the regression: the address panel used to be replaced by the row stack, so
  // the transaction the device trusted least was the one whose destination it
  // never showed. The bar's pill is at the row pill's old x, so the FEE ack tap
  // above and this REVIEW tap land in the same place.
  touch(652, 172); pump(3); release(); pump(8);     // REVIEW -> the rows, own page
  save("/tmp/sim_sign_cautions.ppm");
  // Row 0's I UNDERSTAND: rows start at y=88 with the pill at local (543,8),
  // so it is 567..737 x 96..136. This is its centre.
  touch(652, 116); pump(3); release(); pump(8);     // -> row goes green, page repaints
  save("/tmp/sim_sign_cautions_ack.ppm");
  touch(118, 430); pump(3); release(); pump(8);     // BACK -> verify, address still there
  save("/tmp/sim_sign_combo_back.ppm");
  // The caution "?" used to be anchored to the top of the caution stack, so
  // its y moved with the number of cautions that fired. The bar is one row at
  // one y whatever the count, so the chip at (738,108) never moves. This is its
  // centre.
  // pump(6) caught this card mid fade, so the frame showed a dimmed screen and
  // an icon on its way in. The card is the only place the caution reasons are
  // spelled out, and with four of them stacked it is exactly the frame worth
  // looking at, so wait for the fade to finish before saving.
  touch(753, 123); pump(3); release(); pump(30);    // "?" -> WHY FLAGGED card
  save("/tmp/sim_sign_why.ppm");
  touch(400, 438); pump(3); release(); pump(6);     // OK closes the card
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the SCAN/SD chooser
  save("/tmp/sim_sign_back_choose.ppm");
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  // silent payment send: out0 renders as a tsp1 address with the SP badge+note.
  // the list shows only the first 4 files, so clear the others (all frames above
  // are already saved) to leave zsp-SPAY in row 0 and zzz-UNPRV in row 1.
  unlink("/tmp/simsd/payment-01.psbt");   unlink("/tmp/simsd/payment-01-signed.psbt");
  unlink("/tmp/simsd/risky-STOP.psbt");
  unlink("/tmp/simsd/silly-FEE.psbt");    unlink("/tmp/simsd/silly-FEE-signed.psbt");
  unlink("/tmp/simsd/warn-COMBO.psbt");
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> chooser
  touch(218, 296); pump(3); release(); pump(6);     // FROM SD CARD -> list (only SPAY)
  touch(328, 150); pump(3); release(); pump(8);     // zsp-SPAY (row 0) -> SP verify
  save("/tmp/sim_sign_sp.ppm");                      // SP output row: badge + address + note
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  // The unproven-amount caution on its own: one row, footer kept. It is the
  // shape an ordinary two-input spend from a coordinator that ships bare
  // witness_utxos now has, so it is worth a stop of its own rather than only
  // being seen inside the five-row COMBO pile.
  touch(328, 216); pump(3); release(); pump(8);     // zzz-UNPRV (row 1) -> verify
  save("/tmp/sim_sign_unproven.ppm");
  // The single-caution shape: the one an ordinary two-input spend from a
  // coordinator that ships bare witness_utxos has, and the one most owners
  // will actually meet. Same guarantee.
  must_show("verify (1 caution)", "bc1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz");
  must_show("verify (1 caution)", "39 000 sats");
  must_show("verify (1 caution)", "1 000 sats");
  touch(753, 123); pump(3); release(); pump(30);    // "?" -> WHY FLAGGED, one entry
  save("/tmp/sim_sign_unproven_why.ppm");
  touch(400, 438); pump(3); release(); pump(6);     // OK closes the card
  touch(100, 430); pump(3); release(); pump(6);     // BACK (leftmost) -> the file list
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> the chooser
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home

  // step 6: Sign via QR — scan (real UR fountain parts injected as if the
  // camera decoded them), verify, sign, animated UR out
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> chooser
  touch(218, 190); pump(3); release(); pump(6);     // SCAN QR -> scan screen
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
  touch(365, 430); pump(3); release(); pump(6);     // NEXT ADDRESS -> next index
  save("/tmp/sim_recv_reminder.ppm");               // same layout, different address text
  touch(365, 430); pump(3); release(); pump(4);     // NEXT ADDRESS again
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

  // NO UNDO in its OTHER state. The paper has not been verified yet at this
  // point in the walk, so the chooser carries the amber qualifier over two
  // blocks that both promise the paper still opens this wallet. By step 9 the
  // walk has verified and that branch is unreachable, which is exactly how the
  // backup row's own unchecked frame went uncaptured for so long.
  touch(580, 220); pump(3); release(); pump(8);     // Replace or erase -> chooser
  save("/tmp/sim_endwords_unchecked.ppm");          // amber "paper never checked"
  touch(118, 430); pump(3); release(); pump(8);     // BACK (leftmost now) -> Settings

  // RECOVERY WORDS now belongs to Settings. Verify the paper copy, return to
  // Settings, then separately exercise the sensitive word reveal.
  touch(580, 122); pump(3); release(); pump(6);     // Recovery words row -> warning
  save("/tmp/sim_words_warn.ppm");                  // SHOW / VERIFY MY COPY / BACK
  // VERIFY MY COPY: type the stored dev mnemonic (11x abandon + about).
  // 'abandon' = 'a','b' -> suggestion[0]; 'about' = 'a','b','o' -> suggestion[0].
  touch(370, 430); pump(3); release(); pump(6);     // VERIFY MY COPY -> intro
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
  // The backup row in its OTHER state. The walk has always come back through
  // here and never looked: while "Paper checked" was a second card the amber
  // one was captured and the green one never was, and now that the two cards
  // are one row that changes colour, glyph and sub-line, the unchecked frame
  // covers half of what this row can draw.
  save("/tmp/sim_settings_checked.ppm");            // green, with the wallet ID

  touch(580, 122); pump(3); release(); pump(6);     // Recovery words row -> warning again
  touch(632, 430); pump(3); release(); pump(6);     // SHOW THE WORDS (rightmost now)
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
    touch(632, 430); pump(3); release(); pump(6);   // SHOW THE WORDS
    save("/tmp/sim_words24_p1.ppm");                // 1-12 / 24, NEXT but no BACK
    touch(278, 430); pump(3); release(); pump(6);   // NEXT
    save("/tmp/sim_words24_p2.ppm");                // 13-24 / 24, BACK but no NEXT
    touch(118, 430); pump(3); release(); pump(6);   // BACK -> page 1 again
    save("/tmp/sim_words24_back.ppm");
    touch(680, 430); pump(3); release(); pump(6);   // DONE -> Settings
    snprintf(s_sim_seed, sizeof s_sim_seed, "%s", save_seed);
  }

  // FIRMWARE, the other header pill: 232x44 at x=338, so its middle is (454,40).
  // The fw screens themselves are walked further down by calling
  // wallet_fw_ui_open directly with the seams set; what this proves is the
  // ROUTE, which nothing exercised until settings grew a way in. Settings tears
  // itself down before handing over, so a leak here shows up as the fw screen
  // drawn on top of a live settings page.
  touch(454, 40); pump(3); release(); pump(8);      // FIRMWARE -> the update screen
  save("/tmp/sim_settings_fw.ppm");                 // reached from settings, not directly
  touch(WT_BACK_X + 70, WT_ACTION_Y + 26); pump(3); release(); pump(8);  // BACK -> settings
  save("/tmp/sim_settings_fw_back.ppm");            // one settings page, rebuilt

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
  // Theme moved out of the action bar into the right column under NO UNDO.
  // Card at (412,260), dots card relative at 250 + i*27 on an 18px circle, so
  // absolute centres are 671, 698, 725, 752 at y=292. CYPHERPINK is i=2.
  touch(725, 292); pump(3); release(); pump(4);     // theme dot: CYPHERPINK
  save("/tmp/sim_settings_pink.ppm");               // accent recolors selections+title
  touch(680, 430); pump(3); release(); pump(6);      // BACK, right corner -> home still pink
  save("/tmp/sim_wallet_pink.ppm");
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  touch(671, 292); pump(3); release(); pump(4);     // theme dot: back to MONO (i=0)
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
  touch(530, 300); pump(3); release(); pump(6);     // ALL ADDRESSES row -> list
  save("/tmp/sim_recv_detail_tn.ppm");              // reused filename: now the list
  touch(680, 430); pump(3); release(); pump(6);     // BACK from list -> home
  touch(310, 240); pump(3); release(); pump(6);     // Receive again -> detail
  // The testnet silent-payment address is one character longer than mainnet
  // (tsp1 vs sp1) and was the only receive QR the walk never rendered, which
  // is where a truncation report landed. Capture both sizes so their decoded
  // payloads can be compared byte-for-byte.
  touch(530, 366); pump(3); release(); pump(6);     // SILENT PAYMENT row (testnet)
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
  touch(118, 430); pump(3); release(); pump(6);     // BACK from SP (leftmost) -> detail
  touch(100, 430); pump(3); release(); pump(4);     // BACK from detail (leftmost) -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign -> chooser
  touch(218, 296); pump(3); release(); pump(6);     // FROM SD
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
  // The whole word, via the same helper every other unlock in this walk uses.
  //
  // This used to be five strokes inline -- K, I and ONE S -- under a comment
  // saying the recogniser reveals as soon as "K I S" satisfies it. It did, and
  // that was the bug: three letters opened the device. The walk had been
  // written around the defect, which is why no gate ever saw it. Drawing the
  // real word here is what makes this stop mean anything.
  draw_kiss_word(); release(); pump(20);
  save("/tmp/sim_setup_choose.ppm");                // NEW / RESTORE chooser
  pump(15);                                          // let the K-draw reveal transition settle
                                                     // before the first tap, or it lands dead

  // The seed explainer, through its FIRST door -- the help card on this screen.
  // It had exactly one stop and that stop was on the count screen, which no
  // creation path reaches any more, so removing it left W_WHATSEED_T NEVER
  // OPENED. That is the state this very screen was in for its entire life, and
  // the reason it shipped as a wall of text: no walk stop, so no gate had an
  // opinion. check_screen_coverage.py caught it within a minute here, which is
  // the whole argument for that gate existing.
  //
  // Card at (42, 306) 716x76, so its centre is (400, 344). BACK returns to the
  // chooser because s_whatseed_ret was set by this door.
  touch(400, 344); pump(3); release(); pump(6);     // new here? what a seed phrase is
  save("/tmp/sim_setup_whatseed.ppm");               // YOUR SEED PHRASE
  touch(680, 425); pump(3); release(); pump(6);     // BACK -> the chooser

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

  // the real path: CREATE SEED, entropy, quiz, login twice. Creating no longer
  // asks how many words -- it is always 12 -- so FLASH lands on the method
  // choice (camera+taps vs dice). Both branches are visited: the camera one for
  // its layout only (it cannot complete without a sensor), the dice one all the
  // way to a wallet. kisstest covers the real SHA256.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(4);     // FLASH -> method choice
  save("/tmp/sim_setup_method.ppm");                // camera+taps vs dice

  // A detour through the CAMERA branch before taking dice. Nothing here can
  // gather entropy without a camera, but every save() in this walk is a stop
  // for sim/overlapcheck.c in all 21 locales, and these two screens had NO
  // coverage at all: the walk went straight to dice, so ADD RANDOMNESS and its
  // explainer were the only setup screens no gate had ever rendered. That is
  // exactly how they drifted off-theme far enough for the owner to find it on
  // hardware. Row 0 of the method screen is 96..192, so 144 is its middle.
  touch(394, 144); pump(3); release(); pump(6);     // camera+taps (row 0)
  save("/tmp/sim_setup_entropy.ppm");               // ADD RANDOMNESS, ready state
  touch(725, 364); pump(3); release(); pump(40);    // "?" on the equation card;
                                                    // 40 = the staggered card
                                                    // intro fully settled
  save("/tmp/sim_setup_ent_why.ppm");               // WHY THREE SOURCES, icon grid
  // The PROVE IT detour. The explainer's own OK stays covered by the dice "?"
  // below; this path leaves through the pill instead, walks the whole burned
  // proof run, and lands back on the entropy screen. The stub writes a real
  // (small) kiss-proof.bin into /tmp/simsd.
  touch(158, 430); pump(3); release(); pump(6);     // PROVE IT -> capture screen
  save("/tmp/sim_setup_prove.ppm");                 // viewfinder + recipe + file row
  touch(198, 430); pump(3); release(); pump(6);     // CAPTURE (stubbed, instant)
  save("/tmp/sim_setup_prove_result.ppm");          // hash card + check/burn pair
  touch(198, 430); pump(3); release(); pump(6);     // SHOW WORDS
  save("/tmp/sim_setup_prove_words.ppm");           // words 1-12, burned line
  touch(590, 430); pump(3); release(); pump(6);     // NEXT -> words 13-24
  save("/tmp/sim_setup_prove_words2.ppm");          // second page + counter
  touch(590, 430); pump(3); release(); pump(6);     // DONE -> entropy screen
  // ...and again through the action row's own AUDIT pill, the route that does
  // not require doubting the camera first. Straight back out: the screens it
  // reaches are the ones already walked above.
  touch(480, 430); pump(3); release(); pump(6);     // AUDIT pill -> capture
  save("/tmp/sim_setup_prove_pill.ppm");            // reached without the "?"
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> entropy screen
  touch(680, 430); pump(3); release(); pump(4);     // BACK -> choose

  // The CARDS detour (BLIND DRAW): both lengths, to the picker and back out.
  // The candidate math is stubbed above (first N indices over SIM_WORDS);
  // these stops prove layout in 21 locales, kisstest owns correctness. Method
  // row 2 is 300..396, so 346 is its middle.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(4);     // FLASH -> method choice
  // Straight to the intro at 12: BLIND DRAW no longer asks for a length either,
  // so sim_setup_cards_count is gone with the screen it photographed.
  touch(394, 346); pump(3); release(); pump(4);     // BLIND DRAW (row 2) -> intro, at 12
  save("/tmp/sim_setup_cards_intro.ppm");           // 11 + 1 -> 12, two why blocks
  // The "?" in the equation card's corner: card at x=48,y=128 plus (704-44,12)
  // puts the 30px chip at 708,140, so its centre is 723,155. 40 = the staggered
  // card intro fully settled, same as the dice and entropy explainer stops.
  touch(723, 155); pump(3); release(); pump(40);
  save("/tmp/sim_setup_cards_why.ppm");             // THE 2048 WORD LIST, icon grid
  touch(400, 430); pump(3); release(); pump(6);     // OK dismisses the explainer
  touch(198, 430); pump(3); release(); pump(4);     // TYPE MY WORDS
  save("/tmp/sim_setup_cards_entry.ppm");           // "1/11 _" over the keyboard
  // Eleven DISTINCT, non monotone words. The cards judge links real, so the old
  // "a" eleven times is now the block screen -- see CARDS_BLOCK below, where
  // that shape is typed on purpose. Under the index stub these sit 83 or more
  // apart, far outside WC_NEAR, so an ordinary draw judges clean.
  static const char *CARDS_OK11[11] = {
      "g", "v", "n", "z", "fem", "c", "a", "o", "s", "e", "sy" };
  for (int i = 0; i < 11; i++) restore_word(CARDS_OK11[i]);
  save("/tmp/sim_setup_cards_cksum.ppm");           // THE BUILT IN CHECK, tick chip
  touch(198, 430); pump(3); release(); pump(4);     // SHOW THE WORDS
  save("/tmp/sim_setup_cards_pick.ppm");            // page 1 of 8: 16 pills, NEXT
  touch(590, 430); pump(3); release(); pump(4);     // NEXT -> page 2
  save("/tmp/sim_setup_cards_pick2.ppm");           // BACK owns the left slot now
  touch(128, 434); pump(3); release(); pump(4);     // BACK -> page 1
  touch(128, 434); pump(3); release(); pump(4);     // CANCEL -> chooser
  if (s_sim_pending_mode != -1) {
    fprintf(stderr, "cards cancel left storage mode staged\n");
    return 1;
  }
  // The 23 word draw and its single page of 8 candidates are GONE, with the
  // length choice that reached them. Creating makes 12, so the picker is always
  // 128 candidates over 8 pages and sim_setup_cards_pick24 photographs a shape
  // the product no longer builds. wallet_lastword still computes the 8 for a 23
  // word prefix and kisstest still pins it -- restoring a 24 word phrase is
  // untouched. What went is the screen, not the arithmetic.

  // The refused draw. The judge links REAL here, so typing one word eleven
  // times IS the block, rendered rather than described -- the same argument the
  // dice ramp below makes, and the only way any gate ever sees this screen.
  // Two pill row: CANCEL 48..378 (centre 213), START OVER 422..752 (centre 587).
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(4);     // FLASH -> method choice
  touch(394, 346); pump(3); release(); pump(4);     // BLIND DRAW
  touch(218, 144); pump(3); release(); pump(4);     // 12 WORDS
  touch(198, 430); pump(3); release(); pump(4);     // TYPE MY WORDS
  for (int i = 0; i < 11; i++) restore_word("g");   // the same word, eleven times
  save("/tmp/sim_setup_cards_block.ppm");           // NOT A DRAW, flat bars, 2 pills
  touch(587, 431); pump(3); release(); pump(4);     // START OVER -> empty keyboard
  save("/tmp/sim_setup_cards_retype.ppm");          // "1/11 _": the draw really is gone
  for (int i = 0; i < 11; i++) restore_word("g");   // back to the block
  touch(213, 431); pump(3); release(); pump(4);     // CANCEL -> chooser
  if (s_sim_pending_mode != -1) {
    fprintf(stderr, "cards block cancel left storage mode staged\n");
    return 1;
  }

  // The warned draw, and the chip that has to survive USE ANYWAY. Ascending
  // index order gives sorted = +1 with no near pairs, so the verdict is SORTED
  // and the chip reads IN ORDER.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(4);     // FLASH -> method choice
  touch(394, 346); pump(3); release(); pump(4);     // BLIND DRAW
  touch(218, 144); pump(3); release(); pump(4);     // 12 WORDS
  touch(198, 430); pump(3); release(); pump(4);     // TYPE MY WORDS
  static const char *CARDS_SORTED11[11] = {
      "g", "m", "n", "s", "sy", "fem", "fil", "a", "v", "fol", "c" };
  for (int i = 0; i < 11; i++) restore_word(CARDS_SORTED11[i]);
  save("/tmp/sim_setup_cards_warn.ppm");            // CHECK YOUR WORDS, climbing bars
  touch(213, 431); pump(3); release(); pump(4);     // USE ANYWAY -> the checksum card
  save("/tmp/sim_setup_cards_cksum_warn.ppm");      // the amber IN ORDER chip, kept
  touch(680, 430); pump(3); release(); pump(4);     // CANCEL -> chooser
  if (s_sim_pending_mode != -1) {
    fprintf(stderr, "cards warn cancel left storage mode staged\n");
    return 1;
  }

  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED again
  touch(174, 144); pump(3); release(); pump(4);     // FLASH -> method choice

  // DICE lands on the KEYPAD, not on a count screen. Every creation path makes
  // 12 now, so there is no length to choose and nothing between the method row
  // and the rolling.
  //
  // Two stops died with that screen and are not replaced here, deliberately:
  //
  //   sim_setup_dice_count   -- the screen is gone from this path
  //   sim_setup_whatseed_count -- the seed explainer's SECOND door lived on it.
  //       The door is still built (count_screen's create branch) but nothing
  //       reaches it; check_screen_coverage.py naming it as built-and-never-
  //       captured is the correct report, not a regression to chase.
  //   sim_setup_dice_99      -- the 24 word branch reached DICE_FLOOR_256 and
  //       there is no longer a way to ask for 24 while creating. kisstest still
  //       pins the floor arithmetic; what is gone is the SCREEN that showed it.
  touch(394, 240); pump(3); release(); pump(4);     // DICE (row 1) -> the keypad, at 12
  save("/tmp/sim_setup_dice.ppm");                  // empty keypad, six zero columns
  // Roll 50 cycling the six faces. The quality judge links REAL here, and to a
  // real judge this loop is a textbook ramp — so instead of dodging that, it
  // IS the flagged run: perfectly level columns wearing a PATTERN chip, which
  // is the whole argument for judging order and not just counts. Keys sit at
  // y=146 (card at 96, keys 20..80 inside); face i centre x = 160 + i*94.
  for (int i = 0; i < 50; i++) {
    int kx = 160 + (i % 6) * 94;
    touch(kx, 146); pump(4); release(); pump(4);
  }
  save("/tmp/sim_setup_dice_flag.ppm");             // PATTERN chip over LEVEL bars
  touch(671, 277); pump(3); release(); pump(40);    // "?" beside the chip; 40 =
                                                    // the card intro settled
  save("/tmp/sim_setup_dice_why.ppm");              // WHAT THIS CHECKS, icon grid
  touch(400, 430); pump(3); release(); pump(6);     // OK dismisses the explainer
  touch(430, 425); pump(3); release(); pump(6);     // DONE -> the verdict screen
  save("/tmp/sim_setup_dice_warn.ppm");             // CHECK YOUR ROLLS, 2 pills, no way past
  // ROLL MORE is the way through a refusal, and it keeps every banked roll --
  // the whole argument for DICE_MAX = 180, which no gate rendered until now.
  touch(587, 431); pump(3); release(); pump(4);     // ROLL MORE -> the keypad
  save("/tmp/sim_setup_dice_kept.ppm");             // still 50, bars unchanged
  touch(430, 425); pump(3); release(); pump(6);     // DONE -> refused again
  // START OVER is method_dice_cb, which now asks the count before the keypad,
  // so the length has to be picked again. Without this line the first roll
  // below landed on the count screen instead, quietly leaving 49 rolls in a run
  // the comment underneath says is 50.
  touch(213, 431); pump(3); release(); pump(4);     // START OVER -> how many words
  touch(218, 144); pump(3); release(); pump(4);     // 12 WORDS -> empty keypad
  // The healthy run: a fixed string that reads as rolled, checked in and
  // asserted OK by kisstest. face bits 128.62 (floor 102.50), step bits 126.61
  // (floor 100.45), counts 10/7/9/9/7/8, no repeating block.
  static const char SIM_DICE_OK[] =
      "14464111145452332224636431261353544615153616323265";
  for (int i = 0; i < 50; i++) {
    int kx = 160 + (SIM_DICE_OK[i] - '1') * 94;
    touch(kx, 146); pump(4); release(); pump(4);
  }
  save("/tmp/sim_setup_dice_full.ppm");             // 50 / 50, tick chip, DONE live
  touch(430, 425); pump(3); release(); pump(4);     // DONE -> words
  save("/tmp/sim_setup_words.ppm");                 // 12 words, one page, CANCEL + I WROTE THEM DOWN
  // The checksum card, and the only stop that renders it outside the BLIND
  // DRAW path. Chip is 30px at (722, 24), so its centre is (737, 39); 40 pumps
  // is the staggered card intro settled, same as the dice explainer above.
  touch(737, 39); pump(3); release(); pump(40);
  save("/tmp/sim_setup_words_cksum.ppm");           // THE BUILT IN CHECK, equation + 2 marks
  touch(400, 430); pump(3); release(); pump(6);     // OK dismisses the explainer
  touch(590, 430); pump(3); release(); pump(4);     // I WROTE THEM DOWN
  save("/tmp/sim_setup_quiz.ppm");
  touch(218, 226); pump(3); release(); pump(4);     // round 1: pill 0 correct
  touch(598, 226); pump(3); release(); pump(4);     // round 2: pill 1
  touch(218, 306); pump(3); release(); pump(6);     // round 3: pill 2 -> stored
  save("/tmp/sim_setup_ppintro.ppm");               // ONE MORE LAYER, now a two pill row
  // The NO PASSPHRASE branch, taken as an excursion rather than a commit: it
  // renders the fingerprint screen wearing its no-passphrase notes, which no
  // stop had ever shown, and then BACK returns to the keyboard the walk was
  // heading for anyway. That BACK is the claim being tested -- the pill hands
  // the owner a wallet with no passphrase, and one tap has to be enough to
  // change their mind before anything is committed.
  // Pills are 330 wide from 48 and 422, so centres are 213 and 587.
  touch(213, 430); pump(3); release(); pump(8);     // NO PASSPHRASE -> fingerprint
  save("/tmp/sim_setup_fp_nopass.ppm");             // no passphrase: your words alone open it
  touch(118, 430); pump(3); release(); pump(6);     // BACK (48..188) -> the keyboard
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
  // 220, not 356. This tap was written when the right column ended in two rows
  // and NO UNDO's second card sat at y=260..324; 356 was already past both of
  // them, and once the duress row went full width at SG_FULL_Y=331 it started
  // landing on THAT. Every frame below then walked the duress screens while
  // still being called sim_wipe_*: "sim_wiped.ppm" was PUT A LITTLE IN THE
  // SPARE. check_sim_taps.py could not see it, because each of those taps did
  // change the screen -- just not to the screen the name claims.
  //
  // The right column is SG_R_X 412 + SG_R_W 365, and NO UNDO is now ONE card at
  // y = SG_TOP + SG_HEAD + SG_PITCH + SG_HEAD = 189, 64 tall. Centre of it.
  touch(580, 220); pump(3); release(); pump(8);     // Replace or erase -> chooser
  lv_refr_now(NULL); pump(2);
  save("/tmp/sim_endwords.ppm");                    // two why blocks, three pills
  // ERASE is the RIGHTMOST pill now: wt_pill(482, WT_ACTION_Y, 270) = 482..752.
  touch(617, 430); pump(4); release(); pump(8);     // -> the hold confirm
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
  touch(602, 430); pump(3); release(); pump(6);     // SCAN IT (rightmost now) -> camera
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

  // The one duress screen the walk above cannot reach. ST_NOPASS only appears
  // when WAYS IN is opened on a wallet with no passphrase, and every wallet
  // this walk builds has one, so the screen was rebuilt in this tree with no
  // gate looking at it. Opened directly here, as the last stop: it is a leaf
  // with nothing after it, so it needs no way back and disturbs no state.
  wallet_duress_ui_open_nopass(lv_screen_active(), NULL);
  pump(40);
  save("/tmp/sim_duress_nopass.ppm");               // NOTHING TO HIDE BEHIND

  // ST_INTRO again, and NOT the one the setup walk photographed. Reached from
  // Settings on a wallet that already has a stroke, this screen grows a THIRD
  // pill -- TURN THIS OFF, the only way back to plain behaviour -- and the
  // setup walk can never show it, because during setup there is nothing to
  // turn off yet. That is the whole reason the row overlapped by 8px for as
  // long as it did: no stop had ever contained all three pills at once.
  //
  // A leaf, like the nopass stop above it: opened directly, nothing after it.
  (void)wallet_duress_set(WDG_UNDERLINE);
  wallet_duress_ui_open(lv_screen_active(), NULL);
  pump(40);
  save("/tmp/sim_duress_intro_set.ppm");            // three pills, one TALL row

  // ---- firmware from the SD card -------------------------------------------
  // Leaves, opened directly, for the reason the duress stops above are: the
  // route in is a pill in the Settings action bar and every screen past the
  // first needs state a desktop build does not have.
  //
  // Without wallet_fw_test_* the sim can only ever reach "cannot be checked":
  // there is no flash and no signing key here, so the value card, the four
  // fact rows, the hold and the writing screen would be shapes no gate had
  // ever measured, in any locale. That is exactly the hole BARE and WALL exist
  // to catch, so the seam is what makes their verdict on these screens mean
  // anything.
  {
    // A real app descriptor: 0xE9 image magic, then ABCD5432 at offset 32 with
    // a version far ahead of any VERSION file, so the scan reads it as newer.
    unsigned char img[512];
    memset(img, 0, sizeof img);
    img[0] = 0xE9;
    img[32] = 0x32; img[33] = 0x54; img[34] = 0xCD; img[35] = 0xAB;
    memcpy(img + 32 + 16, "99.0.0", 6);
    memcpy(img + 32 + 48, "kiss", 4);
    FILE *fw = fopen("/tmp/simsd/kiss-signer-99.0.0.bin", "wb");
    if (fw) { fwrite(img, 1, sizeof img, fw); fclose(fw); }
  }

  // 1. the state a build without the release key reaches: two blocks, no rows.
  wallet_fw_test_set_available(WFW_ERR_UNSIGNED);
  wallet_fw_ui_open(lv_screen_active(), NULL);
  pump(20);
  save("/tmp/sim_fw_unsigned.ppm");                 // cannot be checked + where it goes

  // 2. the ordinary one: version card, four marked rows, INSTALL primary.
  wallet_fw_test_set_available(WFW_OK);
  wallet_fw_test_set_install(WFW_OK, 4);
  wallet_fw_ui_open(lv_screen_active(), NULL);
  pump(20);
  save("/tmp/sim_fw_found.ppm");                    // 99.0.0 framed, newer, checked

  touch(168, 430); pump(3); release(); pump(20);    // INSTALL -> confirm
  save("/tmp/sim_fw_confirm.ppm");                  // the why/risk pair + hold row

  // 95, not 100. The hold is 1500ms and pump is 16ms a frame, so it completes
  // on frame 94; the write is deferred 30ms behind the screen that announces
  // it, which is another two frames. At 100 the hold finished with six frames
  // to spare, install_now fired inside the same pump call, and the frame saved
  // as WRITING was already FIRMWARE REPLACED. Held to 95, released, one pump:
  // that frame belongs to WRITING alone, and the rest carry the install to the
  // result. Without this the write happens behind the last frame of the
  // confirm screen and no gate sees the screen that says keep it powered.
  touch(213, 431); pump(95); release(); pump(1);
  save("/tmp/sim_fw_writing.ppm");                  // the DARK -> DONE band + the pair
  // 100, not 20. The install is deferred FW_LIT_MS (1200ms, 75 frames) behind
  // the screen that announces it, so the panel can go dark on purpose rather
  // than mid-paint. It used to be one LVGL tick, and 20 frames cleared that
  // easily; at 75 the walk was still on WRITING when it saved the frame it
  // calls sim_fw_done, and check_sim_taps rightly called the two identical.
  pump(100);
  save("/tmp/sim_fw_done.ppm");                     // FIRMWARE REPLACED + RESTART

  // 3. the refusal that matters most, on the same route: a signature that did
  // not check out has to read as "nothing was written", not as a vague error.
  wallet_fw_test_set_install(WFW_ERR_REJECTED, 2);
  wallet_fw_ui_open(lv_screen_active(), NULL);
  pump(20);
  touch(168, 430); pump(3); release(); pump(20);    // INSTALL -> confirm
  touch(213, 431); pump(100); release(); pump(20);  // hold -> writing -> refused
  save("/tmp/sim_fw_rejected.ppm");                 // NOT INSTALLED, in WT_STOP

  // 4. no card at all: the same two block shape, different left hand claim.
  unlink("/tmp/simsd/kiss-signer-99.0.0.bin");
  platform_sd_test_set_present(0);
  wallet_fw_ui_open(lv_screen_active(), NULL);
  pump(20);
  save("/tmp/sim_fw_nocard.ppm");
  platform_sd_test_set_present(1);
  wallet_fw_test_set_available(WFW_ERR_UNSIGNED);   // leave the seam as found

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
  if (g_walk_fails) {
    printf("FAIL: %d missing fact(s) on a verify screen\n", g_walk_fails);
    return 1;
  }
  // The coverage gate's own proof. A count of zero says nothing unless the
  // check can still fire, which is the argument oc_selftest already makes for
  // WALL and ROLE. On request, build a screen and deliberately never save it;
  // the report below must then name it. C_OK is the title on purpose: it is a
  // pill label everywhere else on the device, so no real stop can ever have
  // captured it and the self test cannot be satisfied by accident.
  if (getenv("SCREENCOVER_SELFTEST")) {
    wt_screen(lv_screen_active(), tr(STR_C_OK), NULL);
    lv_refr_now(NULL);        // built, drawn, and no save() follows it
  }

  // ---- screen coverage ----
  // Which screens this walk BUILT and never captured. Those are the ones every
  // layout gate has been silently skipping: overlapcheck asks its seven
  // questions per STOP, so a screen with no stop is a screen nobody has ever
  // asked about. Reported, not fatal, and the number is meant to go to zero.
  {
    int un[64];
    int n = wt_sim_uncaptured(un, (int)(sizeof un / sizeof un[0]));
    int shown = n < (int)(sizeof un / sizeof un[0]) ? n : (int)(sizeof un / sizeof un[0]);
    printf("\nscreen coverage: %d built and never captured\n", n);
    for (int i = 0; i < shown; i++)
      printf("  UNCHECKED  \"%s\"\n", wt_sim_title_key(un[i]));
    if (n > shown) printf("  ... and %d more\n", n - shown);

    // The machine readable half, for tools/check_screen_coverage.py: which
    // titles were built AT ALL. A screen the walk never opens is invisible to
    // the count above, and that is the state whatseed was in.
    if (getenv("SCREENCOVER_LIST")) {
      int b[STR_N];
      int m = wt_sim_built(b, (int)(sizeof b / sizeof b[0]));
      for (int i = 0; i < m; i++) printf("BUILT\t%s\n", wt_sim_title_key(b[i]));
    }
  }

  printf("sim done\n");
#ifdef OVERLAPCHECK
  return oc_report();
#else
  return 0;
#endif
}
