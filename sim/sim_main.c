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
#include "kiss_crypto.h"
#include "kiss_rngaudit.h"  // kiss_rngaudit_sim_result: the once-in-500 renders
#include "kiss_simpath.h"  // KISS_SIM_TMP: one run's scratch is its own
#include "platform_sd.h"    // the proof stub writes a real (small) file
#include "kiss_seed_sd.h"   // SDSEED_FILENAME: the move stub keeps it truthful
#include "sha256/sha256.h"  // cUR's, real hash for the stub's junk
#include "kiss_duress_ui.h"   // the no-passphrase stop, unreachable by tapping
#include "kiss_fw.h"          // the SD firmware seams: no flash here, no key
#include "kiss_fw_ui.h"       // its screens, opened directly like the above
#include "kiss_sign.h"        // kiss_sign_test_armed: HOLD TO SIGN is a colour
#include "kiss_duress.h"
#include "kiss_gword.h"      // WDG_* , to reach ST_INTRO's configured state
#include "kiss_setup.h"      // kiss_setup_restoring: which passphrase flow follows
void kiss_begin_setup(void);  // main.c: the REPLACE WALLET door into the wizard
#include "kiss_info.h"
#include "kiss_recv.h"    // sim-only hook for the derivation path "?"
#include "kiss_settings.h"
#include "kiss_terms.h"
#include "kiss_word_ui.h"
#include "kiss_theme.h"   // SIM_ACCENT picks the theme the walk renders in
#include "kiss_ui.h"
#include "kiss_usage.h"      // the reuse guard, so the walk can light the USED lamp

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
int kiss_fingerprint(const char *passphrase, unsigned char out[4]) {
  unsigned char h = 0;
  for (const char *p = passphrase ? passphrase : ""; *p; p++) h = (h * 31) ^ *p;
  out[0] = 0x73 ^ h; out[1] = 0xC5 ^ h; out[2] = 0xDA ^ h; out[3] = 0x0A ^ h;
  return 0;
}

// Entropy source seam. On the device kiss_trng_start switches the SAR ADC
// noise source on and the flag records that it happened; on the host the
// callers reach /dev/urandom, which needs no switch, so the flag is the whole
// implementation. It is still a flag rather than a constant true, because the
// Settings footer reports it and the walk should render the state a booted
// device is in, not the state of a process that skipped boot.
static bool s_sim_trng;
void kiss_trng_start(void) { s_sim_trng = true; }
bool kiss_trng_live(void) { return s_sim_trng; }

// The randomness audit's byte stream, deterministic on purpose: one seed, one
// histogram, one chi square score in every walk photograph. test_rngq.c pins
// this seed's score as a golden value (105.920, PASS), so the picture the
// walk saves is a picture a test has already judged. Byte p of the run is
// splitmix step p+1 from the seed, whatever the call sizes were: test_fill
// advances its seed by the splitmix increment once per byte, so continuing at
// position p means starting the seed p increments along. The walk rewinds
// before each entry so a re-entered screen fills identically.
#include "kiss_rngq.h"
#define RNGQ_SIM_SEED 0x4B495353u             // "KISS"; must match test_rngq.c
#define SPLITMIX_GAMMA 0x9E3779B97F4A7C15ull  // must match kiss_rngq_test_fill
static uint64_t s_sim_rng_pos;
void kiss_trng_fill(unsigned char *out, size_t n) {
  kiss_rngq_test_fill(RNGQ_SIM_SEED + s_sim_rng_pos * SPLITMIX_GAMMA, out, n);
  s_sim_rng_pos += n;
}
static void sim_rng_rewind(void) { s_sim_rng_pos = 0; }

// step-7 seams: the seed store is a RAM flag. A fresh sim run starts SEEDED so
// the legacy script flows unchanged; the wizard test at the end wipes first.
#include <string.h>
#include "kiss_seed.h"
static char s_sim_seed[256] =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";
static int s_sim_has_seed = 1;
static char s_sim_pending[256];
static int s_sim_has_pending;
static int s_sim_mode;
static int s_sim_pending_mode = -1;   // staged wizard answer, -1 = none
static int s_sim_sd_present = 1;      // hot-plug state for the unlock gate
int kiss_seed_exists(void) { return s_sim_has_seed || s_sim_has_pending; }
int kiss_seed_store(const char *m) {
  snprintf(s_sim_seed, sizeof s_sim_seed, "%s", m);
  s_sim_has_seed = 1;
  return 0;
}
int kiss_seed_load(char *out, size_t n) {
  if (s_sim_has_pending) { snprintf(out, n, "%s", s_sim_pending); return 0; }
  if (s_sim_mode == WSEED_MODE_SD && !s_sim_sd_present)
    return WSEED_ERR_SD_MISSING;
  if (!s_sim_has_seed) return -1;
  snprintf(out, n, "%s", s_sim_seed);
  return 0;
}
// One-shot failure seam: NVS erase/commit CAN fail on the device, and the
// COULD NOT ERASE screen is only reachable through that failure -- with no
// seam it is a titled screen the walk never opens, which is exactly the
// blindness the coverage gate exists to name.
static int s_sim_wipe_fail;
int kiss_seed_wipe(void) {
  if (s_sim_wipe_fail) { s_sim_wipe_fail = 0; return -1; }
  s_sim_has_seed = 0;
  s_sim_has_pending = 0;
  return 0;
}
int kiss_seed_validate(const char *m) { (void)m; return 0; }
// The device compares against the real BIP39 test vector; the walk never types
// it, so the stub can answer no and the degenerate paths stay under test.
bool kiss_seed_is_test_vector(const char *m) { (void)m; return false; }

int kiss_seed_stage(const char *m) {
  snprintf(s_sim_pending, sizeof s_sim_pending, "%s", m);
  s_sim_has_pending = 1;
  return 0;
}
// One-shot: the next commit reports the staged copy may be the only one left.
// Real NVS cannot be told to lose a wallet, and without this the walk cannot
// reach the recovery screen at all -- which is how the generic setup STOP came
// to be shown there, telling an owner to start again over the last copy of
// their words. Set from the walk, cleared as it fires.
int g_sim_commit_recover;

int kiss_seed_commit(void) {
  if (g_sim_commit_recover) { g_sim_commit_recover = 0; return WSEED_ERR_RECOVER; }
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
void kiss_seed_discard(void) { s_sim_has_pending = 0; s_sim_pending_mode = -1; }
static const char *SIM_WORDS[] = {
  "gravity", "machine", "north", "sort", "system", "female", "filter",
  "attitude", "volume", "fold", "club", "stay", "feature", "office",
  "ecology", "stable", "narrow", "fence", "abandon", "ability", "able",
  "about", "zone", "zoo"};
int kiss_seed_from_entropy(const uint8_t *e, size_t len, char *out, size_t n) {
  (void)e;
  int count = len == 32 ? 24 : 12;
  size_t o = 0;
  for (int i = 0; i < count && o + 12 < n; i++)
    o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", SIM_WORDS[i]);
  return 0;
}
// Source 3 (taps) + the three-way mix. The real fold lives in kiss_tapent.c
// and kiss_crypto.c and needs wally's SHA256; the sim links no crypto, same
// reason as the seed stub above. Here they only have to let the tap screen
// advance and complete. kisstest exercises the real versions. 64 == WTAP_TARGET.
static unsigned s_sim_taps;
void kiss_tapent_reset(void) { s_sim_taps = 0; }
int kiss_tapent_tap(uint64_t us, uint32_t cyc, int16_t x, int16_t y) {
  (void)us; (void)cyc; (void)x; (void)y;
  if (s_sim_taps >= 64) return 0;
  s_sim_taps++;
  return 1;
}
unsigned kiss_tapent_count(void) { return s_sim_taps; }
int kiss_tapent_take(uint8_t out[32]) {
  if (s_sim_taps < 64) return -1;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(i * 7);
  return 0;
}
// The strip the tap screen draws reads this. It has to CHANGE with the count,
// or the walk photographs a stationary row and the stop that exists to tell a
// live strip from a dead one proves nothing. Not a hash and not entropy: a
// value that stirs on every tap, which is all the screen asks of it.
void kiss_tapent_peek(uint8_t out[32]) {
  for (int i = 0; i < 32; i++)
    out[i] = (uint8_t)(i * 31 + s_sim_taps * 71 + (s_sim_taps << 3));
}
// Dice source (verifiable path). Real logic + SHA256 live in kiss_dice.c and
// are exercised by kisstest; the sim links no crypto, so this stub only has to
// let the dice screen advance and complete. The QUALITY judge is not stubbed:
// kiss_dice_q.c needs no crypto and links for real, so the walk renders true
// verdicts against this buffer — a fake WD_Q_OK would leave the warning screen
// unrendered by every gate, which is exactly the drift this walk exists to
// catch. Constants come from the header so a raised DICE_MAX cannot silently
// cap the sim under the screen it is walking.
#include "kiss_dice.h"
static char     s_sim_dice[DICE_MAX + 1];
static unsigned s_sim_dn;
static unsigned s_sim_dbase = 6;
void kiss_dice_reset(unsigned base) {
  s_sim_dn = 0; s_sim_dice[0] = 0; s_sim_dbase = (base == 2) ? 2 : 6;
}
unsigned kiss_dice_base(void) { return s_sim_dbase; }
int kiss_dice_roll(int face) {
  if (face < 1 || (unsigned)face > s_sim_dbase) return 0;
  if (s_sim_dn >= DICE_MAX) return 0;
  // The digit map is the device's, not an approximation of it: base 2 records
  // '0'/'1' so the strings the walk builds are the strings the judge grades.
  s_sim_dice[s_sim_dn++] = (char)((s_sim_dbase == 2 ? '0' : '1') + face - 1);
  s_sim_dice[s_sim_dn] = 0;
  return 1;
}
int kiss_dice_undo(void) {
  if (s_sim_dn == 0) return 0;
  s_sim_dice[--s_sim_dn] = 0;
  return 1;
}
unsigned kiss_dice_count(void) { return s_sim_dn; }
const char *kiss_dice_digits(void) { return s_sim_dice; }
int kiss_dice_take(uint8_t *out, unsigned len) {
  if (!out || (len != 16 && len != 32)) return -1;
  if (s_sim_dn < kiss_dice_floor(s_sim_dbase, len)) return -1;
  for (unsigned i = 0; i < len; i++) out[i] = (uint8_t)(i * 3 + 1);
  return 0;
}
int kiss_dice_peek(uint8_t out[32]) {
  if (!out) return -1;
  // no real SHA in the sim: a value that visibly moves as rolls are added, so
  // the fingerprint label can be walked and shot for the docs.
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(i + s_sim_dn);
  return 0;
}
// Last word source (cards path). The real enumeration needs wally's BIP39
// validator and is exercised by kisstest; the walk only needs the right SHAPE:
// 8 candidates after a 23 word prefix, 128 after 11, real-looking labels.
#include "kiss_lastword.h"
int kiss_lastword_candidates(const char *partial, uint16_t out[WLAST_MAX]) {
  if (!partial) return -1;
  int words = 1;
  for (const char *p = partial; *p; p++)
    if (*p == ' ') words++;
  if (words != 11 && words != 23) return -1;
  int n = words == 23 ? 8 : 128;
  for (int i = 0; i < n; i++) out[i] = (uint16_t)i;
  return n;
}
const char *kiss_lastword_word(uint16_t i) {
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
int kiss_lastword_index(const char *w) {
  for (int i = 0; i < 24; i++)
    if (strcmp(SIM_WORDS[i], w) == 0) return i * 83;
  return -1;
}
int kiss_entropy_mix3(const uint8_t a[32], const uint8_t b[32],
                        const uint8_t c[32], uint8_t out[32]) {
  if (!a || !b || !c || !out) return -1;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(a[i] ^ b[i] ^ c[i]);
  return 0;
}
int kiss_entropy_mix4(const uint8_t a[32], const uint8_t b[32],
                        const uint8_t c[32], const uint8_t d[32],
                        uint8_t out[32]) {
  if (!a || !b || !c || !d || !out) return -1;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(a[i] ^ b[i] ^ c[i] ^ d[i]);
  return 0;
}
// A leg of the seed fold, so the walk has to link it.
// The real one measures two clocks against each other and kisstest exercises
// that; a scripted walk has no clocks worth measuring, so this only has to be
// non-constant and succeed. Nothing here is entropy and nothing here claims to
// be -- the walk never keeps a seed.
int kiss_jitter(uint8_t out[32]) {
  if (!out) return -1;
  static uint32_t s = 0x2545F491u;
  for (int i = 0; i < 32; i++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    out[i] = (uint8_t)(s & 0xFF);
  }
  return 0;
}

// storage mode: the real logic + its edge cases live in kiss_seed.c and are
// covered by kisstest. Here it only has to steer the screens -- but it has to
// steer them the same way, so the staged-vs-applied split is mirrored: the
// wizard stages, commit applies, and only an explicit set_mode erases now.
int kiss_seed_mode(void) {
  return s_sim_pending_mode >= 0 ? s_sim_pending_mode : s_sim_mode;
}
void kiss_seed_stage_mode(int m) {
  s_sim_pending_mode = (m == WSEED_MODE_AMNESIC || m == WSEED_MODE_SD)
                     ? m : WSEED_MODE_KEEP;
}
int kiss_seed_set_mode(int m) {
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
// lives in main/kiss_seed.c, which is what the unit test binary links.
// SWITCHABLE, because a hardcoded 0 means one of two renders never happens.
// The storage chooser tints the KEEP row's subline amber only while this is
// false, so pinning it here made the warned state the only state any gate had
// ever seen -- and the unwarned one, which every device with flash encryption
// on will show, unrendered.
static int s_sim_flash_enc;
int kiss_seed_flash_encrypted(void) { return s_sim_flash_enc; }

// The screen walk creates seeds through the same funnel the device uses, so it
// reaches the entropy note. RAM here: the walk is one process and there is no
// boot for it to survive.
// SIM_ENT_NOTE=1 starts the walk with a flagged seed, which is the only way the
// overlap gate ever renders the second chip on the recovery words page. Without
// it the walk creates clean seeds and the widest version of that chip line --
// two self sizing chips beside each other, in 21 locales -- is never measured.
static int s_sim_ent_note = -1;
// How the seed was made, RAM only. The walk creates through camera + taps, so
// it sets itself the way the device would; SIM_SEED_SRC forces one of the other
// values so the screens for a dice, a drawn or an imported seed get rendered
// without walking every creation path twice.
static int s_sim_seed_src = -1;
void kiss_seed_set_source(int v) { s_sim_seed_src = v; }
int kiss_seed_source(void) {
  if (s_sim_seed_src < 0) {
    const char *e = getenv("SIM_SEED_SRC");
    s_sim_seed_src = (e && *e) ? atoi(e) : 0;   /* WSEED_SRC_NONE */
  }
  return s_sim_seed_src;
}

void kiss_seed_set_entropy_note(int v) { s_sim_ent_note = v; }
int kiss_seed_entropy_note(void) {
  if (s_sim_ent_note < 0) {
    const char *e = getenv("SIM_ENT_NOTE");
    s_sim_ent_note = (e && *e && *e != '0') ? 2 /* WD_Q_UNEVEN */ : 0;
  }
  return s_sim_ent_note;
}
// The two storage verdicts that are not success, forced one at a time. The
// real transaction and its edge cases live in kiss_seed.c and are covered by
// kisstest; what the screens need is the one distinction between them, which
// is whether the destination became durable. A failure leaves the mode alone.
// A cleanup completes the move and then reports the copy it could not remove,
// so the screen must say "changed, with a warning" and never "not changed".
static int s_sim_move_rc;
int kiss_seed_move_to(int m) {
  const int forced = s_sim_move_rc;
  s_sim_move_rc = 0;
  if (m != WSEED_MODE_KEEP && m != WSEED_MODE_SD &&
      m != WSEED_MODE_AMNESIC)
    return WSEED_ERR_INVALID;
  if (m == s_sim_mode) return WSEED_OK;
  if (forced && forced != WSEED_ERR_CLEANUP) return forced;
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
  // The fake card carries the sealed file exactly while the mode says it
  // should. This stub steers screens, and CARD INFO's sealed row reads the
  // card rather than the mode -- without the file the "present" render could
  // never exist. Written before the mode flips so the row is never ahead of
  // the file it reports.
  if (m == WSEED_MODE_SD && s_sim_mode != WSEED_MODE_SD)
    platform_sd_write(SDSEED_FILENAME, (const uint8_t *)"sealed", 6);
  else if (m != WSEED_MODE_SD && s_sim_mode == WSEED_MODE_SD)
    platform_sd_delete(SDSEED_FILENAME);
  s_sim_mode = m;
  return forced ? forced : WSEED_OK;
}
void kiss_seed_forget(void) {
  if (s_sim_mode == WSEED_MODE_AMNESIC) s_sim_has_pending = 0;
}
// Enough of the real reader to drive the walk. This runs on the far side of a
// password now: an opened envelope's plaintext is either raw entropy or a text
// mnemonic, and the KEF stub below hands back the latter. The numeric SeedQR
// shape is not a plaintext and is refused here as it is in the firmware.
int kiss_seed_from_plaintext(const char *data, size_t len, char *out, size_t n) {
  if (out && n) out[0] = 0;
  if (!data || !out || len == 0) return -1;
  if (len == 16 || len == 32) {                 // raw BIP39 entropy
    int words = len == 16 ? 12 : 24;
    size_t o = 0;
    for (int w = 0; w < words && o + 12 < n; w++)
      o += (size_t)snprintf(out + o, n - o, "%s%s", w ? " " : "", SIM_WORDS[w % 24]);
    return 0;
  }
  {   // a mnemonic is 12 or 24 words; anything else is not a seed
    int words = 1;
    for (size_t i = 0; i < len; i++) if (data[i] == ' ') words++;
    if ((words == 12 || words == 24) && len + 1 <= n) {
      snprintf(out, n, "%.*s", (int)len, data);
      return 0;
    }
  }
  return -1;
}
// KEF crypto seam. The envelope half (kiss_kef.c) is REAL in this build, so
// the stub emits a header the same kef_parse the restore router runs will
// accept; only the cipher is faked. Password "kef" is the one that opens.
#include "kiss_kef.h"
int kiss_kef_seal_seed(const char *mnemonic, const char *password,
                       size_t pass_len, uint8_t *out, size_t out_cap,
                       size_t *out_len, char id_hex_out[9]) {
  (void)mnemonic; (void)password; (void)pass_len;
  snprintf(id_hex_out, 9, "73C5DA0A");
  size_t h = kef_emit_header(out, out_cap, (const uint8_t *)id_hex_out, 8,
                             KEF_VERSION_AES_GCM, KEF_ITER_STORED);
  if (!h || h + 32 > out_cap) return -1;
  for (int i = 0; i < 32; i++) out[h + i] = (uint8_t)(0x5a ^ i);
  *out_len = h + 32;                    // iv12 + ct16 + tag4 shaped payload
  return 0;
}
int kiss_kef_open(const char *password, size_t pass_len, const uint8_t *env,
                  size_t env_len, uint8_t *plain, size_t plain_cap,
                  size_t *plain_len) {
  if (plain && plain_cap) plain[0] = 0;
  if (plain_len) *plain_len = 0;
  kef_env_t e;
  if (kef_parse(env, env_len, &e) != 0 || e.version != KEF_VERSION_AES_GCM)
    return -1;
  if (pass_len != 3 || memcmp(password, "kef", 3) != 0) return -1;
  size_t o = 0;
  for (int w = 0; w < 12 && o + 12 < plain_cap; w++)
    o += (size_t)snprintf((char *)plain + o, plain_cap - o, "%s%s",
                          w ? " " : "", SIM_WORDS[w % 24]);
  *plain_len = o;                       // a text mnemonic the seed stub takes
  return 0;
}

int kiss_seed_word(int i, const char **out) {
  *out = SIM_WORDS[i % 24];
  return 0;
}
int kiss_seed_suggest(const char *prefix, const char *out[], int n) {
  int found = 0;
  for (int i = 0; i < 24 && found < n; i++)
    if (strncmp(SIM_WORDS[i], prefix, strlen(prefix)) == 0)
      out[found++] = SIM_WORDS[i];
  return found;
}
// mirrors main/kiss_seed.c (the shipped, unit-tested one)
int kiss_seed_diff_word(const char *typed, const char *stored) {
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

// network seam: kiss_settings + the verify screen read it (no kiss_crypto.c
// in the sim, so the real setter lives here as a plain flag)
static int s_sim_testnet = 1;   // mirror KISS_NET_DEFAULT_TESTNET: fresh = testnet
static int s_sim_net = KISS_NET_TESTNET;   // which of the two test networks it says
// What the last kiss_psbt_load() said, so kiss_psbt_details() can agree with
// it. Two stubs describing one transaction differently is a fixture that
// makes a correct screen look broken.
static uint32_t s_sim_n_in = 1;
static uint64_t s_sim_in_sats = 100000;
void kiss_set_network(int net) { s_sim_net = net; s_sim_testnet = net != KISS_NET_MAIN; }
int kiss_testnet(void) { return s_sim_testnet; }
int kiss_network(void) { return s_sim_net; }
const char *kiss_net_name_of(int net)
{
  return net == KISS_NET_MAIN   ? "MAINNET"
       : net == KISS_NET_SIGNET ? "SIGNET"
                                : "TESTNET";
}
const char *kiss_net_name(void) { return kiss_net_name_of(s_sim_net); }
static int s_sim_script;
void kiss_set_script(int s) { s_sim_script = s; }
int kiss_script(void) { return s_sim_script; }

// step-4 session seams: plausible-looking fakes so the Receive/Export screens render
static int s_sim_decoy;
static int s_sim_prepared_decoy;
static char s_sim_prep_pass[64], s_sim_sess_pass[64];
int kiss_session_prepare(const char *passphrase) {
  s_sim_prepared_decoy = !(passphrase && passphrase[0]);
  snprintf(s_sim_prep_pass, sizeof s_sim_prep_pass, "%s", passphrase ? passphrase : "");
  return 0;
}
int kiss_session_activate_prepared(void) {
  s_sim_decoy = s_sim_prepared_decoy;
  memcpy(s_sim_sess_pass, s_sim_prep_pass, sizeof s_sim_sess_pass);
  return 0;
}
void kiss_session_discard_prepared(void) { s_sim_prepared_decoy = 0; }
int kiss_session_open(const char *passphrase) {
  int rc = kiss_session_prepare(passphrase);
  return rc == 0 ? kiss_session_activate_prepared() : rc;
}
void kiss_session_close(void) {
  s_sim_decoy = 0;
  kiss_seed_forget();          // real kiss_crypto.c does the same on lock
}
int kiss_session_decoy(void) { return s_sim_decoy; }
// The device reads the fingerprint off the key the session holds; here the
// session is a remembered passphrase, so it is the same fake by the same rule.
int kiss_session_fingerprint(unsigned char out[4]) {
  return kiss_fingerprint(s_sim_sess_pass, out);
}
// The refusal switch: a locked session refuses every derivation at once. The
// refusal renders were introduced by fixes to five screens that used to encode
// the failure string into their QRs; without this switch the sim derives
// forever and those branches join the states nothing renders.
static int s_sim_session_locked;
int kiss_session_address(int change, unsigned int index, char *out, unsigned long len) {
  if (s_sim_session_locked) return -1;
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
int kiss_address_validate(const char *addr) {
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
int kiss_session_sp_address(char *out, unsigned long len) {
  if (s_sim_session_locked) return -1;
  snprintf(out, len, "%s", s_sim_testnet
    ? "tsp1qqdpels3srq45dlezqvk20t3dlueftry6p5thc7msjm0s6jm3g84jzq5rxzzunfck6d45va2jcqxk429agt3e4klf3vzmcgp3zqthryhhqgnz4k3n"
    : "sp1qqfqnnv8czppwysafq3uwgwvsc638hc8rx3hscuddh0xa2yd746s7xqh6yy9ncjnqhqxazct0fzh98w7lpkm5fvlepqec2yy0sxlq4j6ccc3h6t0g");
  return 0;
}
// Stage B scan-key export: the real strings kisstest pins against embit
// (sp_test_scan_export), so the sim lays out exactly what the device shows.
static int s_sim_sp_export_fail;   // see the failure stop in the walk
int kiss_session_sp_scan_export(char *out, unsigned long len) {
  if (s_sim_sp_export_fail) return -1;
  snprintf(out, len, "%s", s_sim_testnet
    ? "sp([73c5da0a/352h/1h/0h]tspscan1q8pjcdy7qzlzxl44chw2tsanvzg7dtwhkqf3nsvzmdavls2ekl8qq9qesshy6w9knddr825kqp442302zuwddh6vtqk7zqvgszace9aczgnuqn3)"
    : "sp([73c5da0a/352h/0h/0h]spscan1q0rnl6lft0gkpg4nsn528qgdpytfdej40atdqgrxpqqsg8c5r8vys973ppv7y5c9cphgkzm6g4efmhhcdkazt87ggxwz3pruphc9vkkxxhtvyag)");
  return 0;
}
int kiss_session_bw_export(char *out, unsigned long len) {
  snprintf(out, len, "[73c5da0a/84'/%d'/0']zpub6rFR7y4Q2AijBEqTUquhVz398htDFrt"
                     "ymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31"
                     "mGDtKsAYz2oz2AGutZYs", s_sim_testnet ? 1 : 0);
  return 0;
}
int kiss_session_descriptor(char *out, unsigned long len) {
  if (s_sim_session_locked) return -1;
  snprintf(out, len, "wpkh([73c5da0a/84h/0h/0h]xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuy"
                     "vz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhL"
                     "v1VMrjPC7PW6V/<0;1>/*)");
  return 0;
}

// step-5 seams: no libwally in the sim, so fake the PSBT layer with the same
// numbers the desktop test fixture uses. A file whose content contains "STOP"
// renders the blocked verify screen (so the sim can show both states).
// (step 6's qr_transport + cUR are REAL in the sim — only the camera is faked,
// by injecting decoded strings via kiss_scan_inject.)
#include "kiss_psbt.h"
#include "kiss_scan.h"
#include "qr_transport.h"
#include <string.h>
static bool s_sim_payee_known;
bool kiss_payee_seen(const char *dest) {
  return s_sim_payee_known && dest && strstr(dest, "bc1qzyg3") != NULL;
}
void kiss_payee_mark(const char *dest) { (void)dest; }
void kiss_payee_wipe(void) { s_sim_payee_known = false; }
void kiss_payee_persist_session(void) {}
void kiss_payee_forget_session(void) {}
// The burst brackets kiss_sign.c wraps around its marks. The sim's store is
// a bool, so there is nothing to batch; they exist so the link does.
void kiss_payee_batch_begin(void) {}
void kiss_payee_batch_end(void) {}

int kiss_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s) {
  memset(s, 0, sizeof *s);
  s->testnet = s_sim_testnet != 0;
  s->net     = (uint8_t)s_sim_net;
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
  if (len >= 7 && memmem(bytes, len, "NOTMINE", 7)) {
    // The ownership refusal WITH both halves of the compare present: this
    // signer's own fingerprint against the one the coordinator wrote into
    // input 0. The screen frames the two and says what a difference can mean,
    // so the fixture has to make them actually differ -- an in0_fp copied from
    // our_fp would render two identical codes under a verdict saying they are
    // not, which is the one frame this stop exists to catch.
    kiss_ui_last_fp(s->our_fp);
    memcpy(s->in0_fp, "\xEC\x5A\x45\x95", 4);
    s->in0_keypaths = 1;
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "input is not this wallet's");
  } else if (len >= 6 && memmem(bytes, len, "NOKEYS", 6)) {
    // The same refusal reached the other way: a coordinator that sent no
    // derivation at all, so nothing was compared and the screen must not
    // claim a mismatch. One card, a different sentence.
    kiss_ui_last_fp(s->our_fp);
    s->in0_keypaths = 0;
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "input is not this wallet's");
  } else if (len >= 4 && memmem(bytes, len, "STOP", 4)) {
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "input amount unverifiable");
  } else if (len >= 3 && memmem(bytes, len, "FEE", 3)) {
    // mirrors kiss_psbt.c's high-fee caution so the sim can show it
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
  } else if (len >= 4 && memmem(bytes, len, "MANY", 4)) {
    // Five recipients and a change: more than the panel can show at once, which
    // is the only shape that exercises the read-to-the-end gate. Every stub
    // above this one has exactly ONE recipient, so the gate was invisible to
    // the walk and would have shipped as a no-op nobody could see.
    s->n_out = 6;
    s->in_sats = 100000; s->send_sats = 60000; s->change_sats = 39000;
    for (int i = 0; i < 5; i++) {
      snprintf(s->outs[i].addr, sizeof s->outs[i].addr,
               "bc1q%02dg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3h8ffkz", i);
      s->outs[i].sats = 12000;
      s->outs[i].is_change = false;
    }
    snprintf(s->outs[5].addr, sizeof s->outs[5].addr,
             "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
    s->outs[5].sats = 39000; s->outs[5].is_change = true;
  } else if (len >= 5 && memmem(bytes, len, "UNPRV", 5)) {
    // A two-input spend whose amounts were declared and not proved. This is a
    // REFUSAL, not a caution: the fee on the screen would be a number the
    // device cannot stand behind, so there is nothing here for an owner to
    // weigh. It is the one STOP an honest coordinator can trip, which is why
    // its panel is worth a walk stop of its own -- the sentence on it has to
    // name the fix.
    s->n_in = 2;
    s->n_unproven_in = 2;
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "input amounts not proven");
  } else if (len >= 5 && memmem(bytes, len, "MERGE", 5)) {
    // A consolidation: twenty coins swept to one address, nothing back. Above
    // WPSBT_MERGE_INS so the coins-linked caution always fires, and above
    // WPSBT_MAX_INS so kiss_psbt_details can only hold sixteen of them -- which
    // is what makes the group's count and total come from the summary rather
    // than from the rows the graph can see.
    // Twenty coins, fourteen addresses: six of them are pairs already sitting
    // on a shared address. The two numbers being different is the point -- the
    // caution names the fourteen, because the six were joined the day the
    // address was handed out twice and this transaction reveals nothing new
    // about them.
    s->n_in = 20; s->n_in_addr = 14; s->n_out = 1;
    s->in_sats = 4210000; s->send_sats = 4200000; s->change_sats = 0;
    s->fee_sats = 10000; s->fee_rate_x10 = 24; s->est_vsize = 4166;
    s->outs[0].sats = 4200000; s->outs[0].is_change = false;
    snprintf(s->outs[0].addr, sizeof s->outs[0].addr,
             "bc1qm52k4nv8ffkz7mvd3sjn54khce6mua7l7p9c8x");
    s->status = WPSBT_CAUTION;
    s->caution_flags = WPSBT_C_MERGE_INS;
    snprintf(s->reason, sizeof s->reason,
             "many coins spent at once - they are linked forever");
  } else if (len >= 5 && memmem(bytes, len, "COMBO", 5)) {
    // Every caution at once: proves the summary + WHY card stack up. FIVE rows
    // is the most the row page can ever draw, so this is the fixture that says
    // whether a full stack still clears WT_CONTENT_BOTTOM: 88 + 5*56 + 4*4 is
    // 384 against 398, and the 4px gap exists for exactly this row. It was
    // four for a while -- unproven amounts became a STOP and took the fifth
    // row with them, since a STOP ends the screen instead of joining it. The
    // gap-limit caution is what put the row back, and this comment said FOUR
    // for a whole commit after the fixture below started setting five.
    s->n_in = WPSBT_MERGE_INS;
    s->n_in_addr = WPSBT_MERGE_INS;      // five coins, five addresses: at the bar
    s->send_sats = 3000; s->fee_sats = 800; s->change_sats = 200;
    s->outs[0].sats = 3000; s->outs[1].sats = 200; s->in_sats = 4000;
    s->fee_rate_x10 = 570;
    s->status = WPSBT_CAUTION;
    s->outs[1].index = 99999;            // change parked past the scan window
    s->caution_flags = WPSBT_C_HIGHFEE | WPSBT_C_DUST_INPUT |
                       WPSBT_C_DUST_CHANGE | WPSBT_C_MERGE_INS |
                       WPSBT_C_GAP_CHANGE;
    snprintf(s->reason, sizeof s->reason, "unusually high fee, tiny coins");
  }
  s_sim_n_in = s->n_in;
  s_sim_in_sats = s->in_sats;
  return 0;
}
int kiss_psbt_details(wpsbt_details_t *d) {
  memset(d, 0, sizeof *d);
  // n_total > n_in exercises the many-inputs header (S_D_MANYIN_FMT) with a
  // 2-digit count: the longest formatted line in the whole sign flow (ja is
  // ~140 bytes) and the exact case that used to truncate in buf[128]
  // Five inputs, proven states mixed, so the page draws BOTH per-input marks
  // (a tick beside a coin a previous transaction vouched for, an eye-slash in
  // WARN beside one only claimed) AND overflows its viewport, which is what
  // keeps the always-on scrollbar honest: past two inputs the list must not
  // look like it ends at the fold.
  //
  // The count comes from the summary the walk actually loaded. It used to be a
  // flat 5 whatever was on screen, which was invisible while the details page
  // was the only reader -- but the bundle graph draws one strand per input
  // beside a caption counting s_sum.n_in, so a stub that disagreed with itself
  // put five coins under the words "SPENDING 1 OF YOUR COINS". A fixture may
  // not be the thing that makes a screen look wrong.
  //
  // S_D_MANYIN_FMT needs n_total > n_in, which no fixture reaches yet; the
  // twenty-input one arrives with the elision and restores it.
  d->version = 2; d->locktime = 0; d->txid_final = true;
  d->n_total = s_sim_n_in ? s_sim_n_in : 1;
  d->n_in = d->n_total > WPSBT_MAX_INS ? WPSBT_MAX_INS : d->n_total;
  snprintf(d->txid, sizeof d->txid, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
  // Amounts that sum to the summary's in_sats, so the strands have real
  // proportions to draw and the elided total has a true number to state. The
  // spread GROWS -- each coin takes two thirds of an even share and the last
  // one takes the remainder -- because a set of equal strands would hide
  // whether wt_strand_px is doing anything at all.
  uint64_t left = s_sim_in_sats, n = d->n_in;
  for (uint32_t i = 0; i < d->n_in; i++) {
    memset(d->ins[i].txid, "abcdefghijklmnop"[i], 64);
    d->ins[i].txid[64] = 0;
    uint64_t take = (i + 1 == d->n_in) ? left : (left * 2) / (3 * (n - i));
    if (!take) take = 1;
    d->ins[i].vout = i; d->ins[i].sats = take;
    left -= take;
    d->ins[i].purpose = 84; d->ins[i].change = 0; d->ins[i].index = i;
    d->ins[i].proven = (i == 0);   // the fold hides unproven coins: scroll
  }
  return 0;
}
// One shot, so the walk can reach SIGN FAILED without a second fixture. The
// real refusal is a libwally call returning nothing -- a key that does not own
// an input, a sighash it will not produce -- and none of that can be staged
// from a stub that has no wally behind it.
static int s_sim_sign_fails;
int kiss_psbt_sign(uint8_t *out, size_t out_len, size_t *written) {
  if (s_sim_sign_fails) { s_sim_sign_fails = 0; return -1; }
  size_t n = out_len < 220 ? out_len : 220;
  memset(out, 0xAB, n); *written = n;
  return 0;
}
// The real fingerprint (sha256 over the signature bytes) needs wally; the sim
// links none, same as the sign stub above. A tiny deterministic hash gives the
// walk a stable, plausible code to render. kisstest covers the real function.
int kiss_psbt_sig_fingerprint(const uint8_t *b, size_t len, char out[9]) {
  if (!b || !out) return -1;
  unsigned long h = 2166136261UL;
  for (size_t i = 0; i < len; i++) { h ^= b[i]; h *= 16777619UL; }
  static const char HEX[] = "0123456789abcdef";
  for (int k = 0; k < 8; k++) out[k] = HEX[(h >> (28 - 4 * k)) & 0xf];
  out[8] = 0;
  return 0;
}
void kiss_psbt_free(void) {}

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

// A frame that is deliberately NOT settled. Written straight to disk rather
// than through save(), because a save() is a checkpoint every gate then
// questions -- and a page mid transition is exactly what the comment above
// save_seq refuses to hand them: rows part faded and still travelling, which
// overlapcheck would read as thirty boxes sharing pixels and check_sim_taps
// as a frame that failed to change. It is a picture for a person to look at.
static void shot_raw(const char *name) {
#ifdef OVERLAPCHECK
  (void)name;
#else
  char p[192];
  lv_refr_now(NULL);
  write_ppm(kiss_sim_path(p, sizeof p, name));
#endif
}

static void save(const char *path) {
  char lp[384];
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
  // The frames land under KISS_SIM_TMP too, and this is the one place they all
  // pass through -- 333 of the /tmp literals in this file are save() calls, so
  // rewriting here is the whole of it. The locale prefix goes on in the same
  // breath it always did.
  if (strncmp(path, "/tmp/", 5) == 0) {
    if (g_lang_code && strncmp(path, "/tmp/sim_", 9) == 0)
      snprintf(lp, sizeof lp, "%s/sim_%s_%s", kiss_sim_root(), g_lang_code,
               path + 9);
    else
      snprintf(lp, sizeof lp, "%s/%s", kiss_sim_root(), path + 5);
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
  char path[320];
#ifdef OVERLAPCHECK
  // The gate builds run this same walk, one of them per accent. Without this
  // they would overwrite the captured frames with whatever theme they were
  // sweeping, and the next gen_docs_shots.py would quietly build the README's
  // GIF in ORANGE.
  return;
#endif
  if (g_lang_code) return;
  lv_refr_now(NULL);
  snprintf(path, sizeof path, "%s/sim_reveal_%03d.ppm", kiss_sim_root(),
           g_seq_n);
  if (!write_ppm(path)) return;

  char pp[192];
  FILE *p = fopen(kiss_sim_path(pp, sizeof pp, "sim_reveal_path.txt"),
                  g_seq_n ? "a" : "w");
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
// The first descendant carrying a flag, for the accent propagation checks.
static lv_obj_t *find_flagged(lv_obj_t *o, uint32_t flag) {
  if (!o) return NULL;
  if (lv_obj_has_flag(o, flag)) return o;
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    lv_obj_t *r = find_flagged(lv_obj_get_child(o, i), flag);
    if (r) return r;
  }
  return NULL;
}

// The scrolling container under a point, if there is one. A drag stop needs to
// read the scroll offset back to prove the finger moved the list rather than
// tapping a row -- and asserting on a frame cannot tell those apart, because a
// list that did not move looks exactly like one with nothing below the fold.
static int find_label_text(lv_obj_t *o, const char *needle) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
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

// Exact-label counterpart for state captions. Some caption values deliberately
// share their English with diagram chips (PASSPHRASE is both), so must_show's
// global duplicate-key guard correctly refuses them even when the live screen
// has only the one caption whose state the walk needs to pin.
static int find_label_exact(lv_obj_t *o, const char *needle) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    if (t && strcmp(t, needle) == 0) return 1;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    if (find_label_exact(lv_obj_get_child(o, i), needle)) return 1;
  return 0;
}

// A folded address that can be pressed. The DETAILS output rows carry one each
// and their y moves with everything above them, so a locale whose fact rows run
// taller pushes the list down and a hard coded tap lands on background. That is
// the derail det_chip already exists for, and it bit again here: the tap at
// (200, 300) opened nothing in fourteen locales, while the needle checking it
// had worked -- "bc1q zyg3" -- matched the FOLD still showing on the page
// underneath. A spurious pass, from a prefix the two renderings share.
//
// Found by what it is instead: the address spangroups are the only clickable
// ones on the page. The needle is kept so a future page with a clickable
// spangroup of some other kind cannot silently take its place.
static int find_label_text(lv_obj_t *o, const char *needle);
static lv_obj_t *find_click_addr(lv_obj_t *o, const char *needle) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
  if (lv_obj_check_type(o, &lv_spangroup_class) &&
      lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) &&
      find_label_text(o, needle))
    return o;
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    lv_obj_t *r = find_click_addr(lv_obj_get_child(o, i), needle);
    if (r) return r;
  }
  return NULL;
}

// The printed reference word on the stroke rehearsal. Its PARENT card is the
// box kiss_duress_classify measures the stroke against, so the walk reads the
// same rectangle the firmware does rather than repeating its coordinates -- a
// card that moves would otherwise turn every rehearsal stroke into a strike, or
// into nothing, with the screen still looking right in the frame.
static lv_obj_t *find_word_label(lv_obj_t *o) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    return (t && strcmp(t, "KISS") == 0) ? o : NULL;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    lv_obj_t *r = find_word_label(lv_obj_get_child(o, i));
    if (r) return r;
  }
  return NULL;
}

// The 8-hex-char fingerprint string, the one value that identifies a wallet.
// Both the fingerprint screen and the setup warning show it in a value card;
// the walk reads it off one to prove the other names the same wallet. No
// other label on either screen is exactly eight hex digits.
static lv_obj_t *find_hex8(lv_obj_t *o) {
  lv_obj_t *f;
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    if (t && strlen(t) == 8) {
      int hex = 1;
      for (int i = 0; i < 8; i++) {
        char c = t[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) { hex = 0; break; }
      }
      if (hex) return o;
    }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    f = find_hex8(lv_obj_get_child(o, i));
    if (f) return f;
  }
  return NULL;
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

// The firmware screen's done_cb rebuilds Settings. The auto-lock teardown must
// NOT fire it: Settings rebuilt under a locked device is the whole of the
// recovery-words leak. Counted, not screenshotted, because what leaked was a
// live object and every frame of it looked correct.
static int g_fw_done_fired;
static void sim_fw_done_cb(void) { g_fw_done_fired++; }
// A needle that cannot fail honestly. must_show asserts on tr(SOME_KEY), and if
// that English value is ALSO the value of another key, the assertion passes
// whenever EITHER screen is up. That is not hypothetical: restore/chooser
// asserted W_NEW_T, W_CHOOSE_NEW carries the same words in English, and the
// walk opened the wrong screen and reported success -- in twenty locales. Only
// French, where the two differ, said anything.
//
// The 21-locale sweep used to be what caught this class, by accident. The gates
// are English-only while the UI is being rebuilt, so the check has to be too,
// and this one is better than the accident was: it names the ambiguity instead
// of waiting for a locale where it happens to bite.
static int needle_shared_by_two_keys(const char *s) {
  int n = 0;
  for (int i = 0; i < STR_N; i++)
    if (strcmp(tr(i), s) == 0 && ++n > 1) return 1;
  return 0;
}

static void must_show(const char *what, const char *needle) {
  if (needle_shared_by_two_keys(needle)) {
    printf("FAIL: %s: needle \"%s\" is the English value of more than one key, "
           "so it passes on whichever screen shows either -- assert on "
           "something unique to this screen\n", what, needle);
    g_walk_fails++;
    return;
  }
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

static void must_show_exact(const char *what, const char *needle) {
  if (find_label_exact(lv_screen_active(), needle)) return;
  printf("FAIL: %s: no label on screen equals \"%s\"\n", what, needle);
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

// ---- SETTINGS, direction 1b: five section tabs ----------------------------
// Every tap into this page is expressed here rather than as thirty numbers
// scattered down the walk, because the page moved wholesale and a stale
// literal does not fail -- it lands on the background, the frame saves, and
// the frame is simply a copy of the one before it.
//
// The strip is 144px tabs on a 152 pitch from x=25, 46 tall at y=68, so tab i
// centres on (97 + 152i, 91). Rows are 60 tall on a 66 pitch from y=126, so
// row i centres on y = 156 + 66i; x=200 is the label lane and x=670 the value
// chip. Nothing floats over the page any more: a chip that wears the LOOP
// mark advances its value on every tap, so a pick is N taps on the SAME
// coordinate, and the one pick that opens a screen puts its options on the
// very same row grid.
enum { SET_SIGNER = 0, SET_SECURITY, SET_BACKUP, SET_DEVICE, SET_NOUNDO };
// The flex strip sizes each tab to its label, so a coordinate would only
// prove the English layout: the walk taps tabs by their STRINGS.
#define SET_TAB_Y          91
#define SET_ROW_Y(i)      (156 + 66 * (i))
#define SET_LABEL_X       200
#define SET_CHIP_X        670
// THIS DEVICE is not on the row grid: the facts card owns 104..200 and the
// card slot's own row sits at 232, 60 tall.
// THIS DEVICE is a five row def list now, not a card over one wide row: the
// page was a 96px block of font14 with 180px of empty glass under it. The
// card is the LAST row, and def rows are LANE/n tall from WT_LANE_Y.
#define SET_DEV_CARD_Y     (114 + (284 / 5) * 4 + (284 / 5) / 2)

// 50 frames, which is 800ms, and it is the only tap on this page that needs
// them: a tab change is the one thing SETTINGS animates. The last row of a
// group settles at 408ms, its value at 466 and the caution pulse at 756, so
// eight frames photographs a page mid flight -- rows part faded and still
// travelling, which every gate then measures as a settled screen and reports
// as thirty overlaps. Nothing else here is affected, because nothing else
// moves: the fresh open, every value chip's rebuild and every return from a
// screen a row opens all paint at rest by construction.
static void tap_str(int key, int hold, int settle);
static const int SET_TAB_KEY[] = { STR_I_TAB_SIGNER, STR_I_TAB_SECURITY,
                                   STR_I_TAB_BACKUP, STR_I_TAB_DEVICE,
                                   STR_I_SEC_NO_UNDO };
static void set_tab(int i)  { tap_str(SET_TAB_KEY[i], 3, 50); }
static void set_row(int i)  { touch(SET_LABEL_X, SET_ROW_Y(i)); pump(3); release(); pump(8); }
// The SETTINGS tabs are definition lists now -- n equal rows across the
// 114..398 lane (kiss_defrow.h) -- while the chooser screens a row opens
// stay on the 66px wide-row grid above. Two geometries, two helpers: aiming
// SET_ROW_Y at a def list lands row 2's tap on row 1 and the walk then
// photographs the wrong pick in silence, which is the exact derailment
// check_screen_coverage.py exists to catch.
#define SET_DEF_Y(n, i)  (114 + (WT_DEF_LANE / (n)) * (i) + (WT_DEF_LANE / (n)) / 2)
static void def_row(int n, int i)
{
  touch(SET_LABEL_X, SET_DEF_Y(n, i)); pump(3); release(); pump(8);
}
static void def_go(int n, int i)
{
  touch(SET_CHIP_X, SET_DEF_Y(n, i)); pump(3); release(); pump(8);
}
static void def_cycle(int n, int row, int taps)
{
  for (int i = 0; i < taps; i++) def_go(n, row);
}
// The network row, driven BY STATE. Counting taps was wrong twice in this
// file: once when SIGNET joined the rotation and shifted every relative +2,
// and once because the excursion below assumed it started on MAINNET when the
// walk arrives on TESTNET -- so its "TESTNET" frame was signet, its "SIGNET"
// frame was mainnet, and TESTNET, the network every earlier stop runs on, was
// never photographed here at all. Three captions describing three chains, and
// the frames underneath them holding two.
//
// Nothing said so because both test networks answer to the same needle: the
// row's note is "not real bitcoin" on either one, which is the exact trap this
// excursion exists to photograph. So it asks the DEVICE which chain it is on
// and taps until the answer is the one wanted, or fails loudly. Same shape as
// the mainnet leg further down, which learned this first.
static void net_to(int want)
{
  for (int i = 0; i < 4 && kiss_network() != want; i++) def_go(2, 0);
  if (kiss_network() != want) {
    fprintf(stderr, "FAIL: network never reached %d (stuck on %d)\n",
            want, kiss_network());
    exit(1);
  }
}
// The two band controls LANGUAGE and THEME moved down to: the language
// action's box is right-aligned to 512 so a tap near its right edge holds
// for any locale's name, and the theme dot's hit box is 528..580.
static void band_lang(void)  { touch(490, WT_ACTION_Y + 26); pump(3); release(); pump(8); }
static void band_theme(void) { touch(554, WT_ACTION_Y + 26); pump(3); release(); pump(8); }
// A cycle row: n taps on the one coordinate. Every tap rebuilds the page, so
// this is n whole renders and not a gesture -- which is exactly what a finger
// does to it.
// ---- RECOVERY WORDS, on the same chrome ---------------------------------
// Two groups instead of five, and the same geometry, so the same numbers work:
// the strip sits at y=68 and the rows on WT_WIDE_Y(i). The settle is the same
// 50 frames for the same reason -- a tab change is the one thing this page
// animates, and eight frames photographs it mid flight.
//
// The ENCRYPTED BACKUP row used to be reached by touch(650, 128), a raw
// coordinate three times over, because the row was wedged into whatever width
// the status chip beside it left. It is a group of its own now and the strip
// opens it.
enum { WORDS_PAPER = 0, WORDS_ENC };
// By the word, not by a pitch: the flex strip spreads its two tabs across
// the 620 lane, so there is no fixed x to aim at any more.
static void words_tab(int i)
{
  tap_str(i == WORDS_ENC ? STR_I_WTAB_ENC : STR_I_WTAB_PAPER, 3, 50);
}
static void words_row(int i) { touch(SET_LABEL_X, SET_ROW_Y(i)); pump(3); release(); pump(8); }


// ---- tapping an action by WHAT IT SAYS, not where it is ----------------------
//
// The walk used to aim 179 hardcoded coordinates at the action row, and that
// made every layout change a harness change: move a control and the tap aimed at
// it lands on background, the frame still saves, and the walk carries on
// photographing whatever happens to be on screen. Moving the exit to the other
// corner broke 69 taps in one commit, and each one had to be found by running
// the whole walk again to see where it went wrong.
//
// So a tap names its target. The string ID rather than the English text,
// because the same walk runs in ja and ru and the label is translated in both.
//
// Deliberately loud, and deliberately strict about AMBIGUITY: two visible actions
// reading DONE mean the walk cannot say which one it meant, and quietly picking
// one would be the same blind guess this exists to remove.
static lv_obj_t *s_hit;
static int       s_hits;
static lv_obj_t *s_bar_hit;
static int       s_bar_hits;
// Clickable is not the same as tappable. lv_obj_create hands out
// LV_OBJ_FLAG_CLICKABLE by default, so every plain container -- a wt_card, the
// column a histogram is drawn in -- is a clickable ancestor of whatever sits
// inside it. That is harmless while a walk asks for words nothing else says,
// and stops being harmless the moment it asks for "0": the per face count
// under a coin's first column reads "0" too, and its clickable ancestor is the
// card. So a literal lookup requires the ancestor to have an event handler
// WIRED to it -- which the key does, the "?" chip does, and a card never does.
static bool      s_wired_only;

static void find_act(lv_obj_t *o, const char *txt)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *t = lv_label_get_text(o);
        size_t lt = t ? strlen(t) : 0, ln = strlen(txt);
        // Exact, or the icon form wt_icon_text composes: "<icon>  LABEL".
        // The two spaces are load bearing. A bare suffix test matched CAMERA
        // AUDIT when the walk asked for AUDIT, which reads as an ambiguity
        // between two real controls and is really one wrong match.
        const bool hit = t && lt >= ln && strcmp(t + (lt - ln), txt) == 0 &&
                         (lt == ln || (lt >= ln + 2 &&
                                       t[lt - ln - 1] == ' ' && t[lt - ln - 2] == ' '));
        if (hit) {
            for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p))
                if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE) &&
                    (!s_wired_only || lv_obj_get_event_count(p) > 0)) {
                    if (p != s_hit) { s_hit = p; s_hits++; }
                    // A label often appears twice: once on a content row that
                    // opens the thing, once on the action in the bar that
                    // does it. Every converted tap aimed at the bar, so the bar
                    // wins -- and only when it is unambiguous down there too.
                    lv_area_t a; lv_obj_get_coords(p, &a);
                    if ((a.y1 + a.y2) / 2 >= WT_CONTENT_BOTTOM && p != s_bar_hit) {
                        // LAST wins: LVGL paints in tree order, so the last
                        // match is the topmost, and the topmost is what a
                        // finger would land on. An explainer overlay can carry
                        // the same action as the screen underneath it -- both are
                        // real, and the one on top is the one being tapped.
                        s_bar_hit = p; s_bar_hits++;
                    }
                    break;
                }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        find_act(lv_obj_get_child(o, i), txt);
}

// The first accent-flagged lv_line under `o`. The change strand is the one
// object on the verify screen whose accent is a LINE colour, so it is what
// proves the walk of that channel actually runs.
static lv_obj_t *find_accent_line(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_line_class) &&
        lv_obj_has_flag(o, WT_FLAG_ACCENT))
        return o;
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *r = find_accent_line(lv_obj_get_child(o, i));
        if (r) return r;
    }
    return NULL;
}

// By LITERAL text rather than by translation key. Some controls carry a
// character instead of a word: the keypad's die faces are "1".."6" and a coin's
// sides are "0"/"1", which are the characters recorded into the SHA256 string,
// not copy, so tr() has no key for them. Same finder, same refusal to guess
// between two matches -- and the clickable-ancestor rule is what keeps the
// per-face COUNT labels under the columns out of it: they read "1" too, and
// nothing above them is clickable.
static lv_obj_t *ctrl_for(const char *txt, const char *how)
{
    s_hit = NULL; s_hits = 0; s_bar_hit = NULL; s_bar_hits = 0;
    s_wired_only = true;
    find_act(lv_screen_active(), txt);
    if (s_bar_hits >= 1) {
        if (s_bar_hits > 1)
            printf("note: %d actions say \"%s\"; taking the topmost\n", s_bar_hits, txt);
        s_hit = s_bar_hit; s_hits = 1;
    }
    if (!s_hit || s_hits != 1) {
        printf("FAIL: %s \"%s\": %s\n", how, txt,
               !s_hit ? "no visible action says that" : "more than one does");
        g_walk_fails++;
        return NULL;
    }
    return s_hit;
}

// Key-named actions keep the looser rule they have always had: their labels are
// words, the ambiguity check already covers them, and narrowing it now would be
// a change to two hundred existing taps for no fault anyone has seen.
static lv_obj_t *act_for(int key, const char *how)
{
    const char *txt = tr(key);
    s_hit = NULL; s_hits = 0; s_bar_hit = NULL; s_bar_hits = 0;
    s_wired_only = false;
    find_act(lv_screen_active(), txt);
    if (s_bar_hits >= 1) {
        if (s_bar_hits > 1)
            printf("note: %d actions say \"%s\"; taking the topmost\n", s_bar_hits, txt);
        s_hit = s_bar_hit; s_hits = 1;
    }
    if (!s_hit || s_hits != 1) {
        printf("FAIL: %s \"%s\": %s\n", how, txt,
               !s_hit ? "no visible action says that" : "more than one does");
        g_walk_fails++;
        return NULL;
    }
    return s_hit;
}

// The nth "?" chip right of x>700, top to bottom. The DETAILS page's four term
// chips sit in ONE column (x = RX+RW-26 = 714), and their y used to be hard
// coded into the walk -- then each of the three flag rows lost its explainer
// note and the column shrank, and a tap at the old y hit nothing. A walk that
// taps nothing above an empty frame is exactly the silent derail the first
// counter exists for, so the chips are found by their own marks and order
// instead: they are the only "?" labels in the page's right hand column.
static void det_chip_scan(lv_obj_t *o, lv_obj_t **found, int *n)
{
    if (*n >= 8) return;
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *t = lv_label_get_text(o);
        // ABSOLUTE coords, and a RECURSIVE walk. Both halves were wrong before:
        // lv_obj_get_x is relative to the parent, and this label's parent is the
        // 30px chip it is centred in, so its own x is about 10 in every column;
        // and the scan went screen -> child -> grandchild, while wt_screen puts
        // a container of its own between the active screen and anything a page
        // builds, which leaves these labels three deep. It found nothing on any
        // screen, in any locale, for either reason on its own.
        lv_area_t la; lv_obj_get_coords(o, &la);
        // y > 104: below the tab strip, so the deck's corner [ ? ] mark --
        // itself a "?" label past x=700 -- never counts as a term chip.
        if (t && strcmp(t, "?") == 0 && la.x1 >= 700 && la.y1 > 104)
            found[(*n)++] = o;
        return;
    }
    uint32_t c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c && *n < 8; i++)
        det_chip_scan(lv_obj_get_child(o, i), found, n);
}

static lv_obj_t *det_chip(int idx)
{
    lv_obj_t *found[8];
    int n = 0;
    det_chip_scan(lv_screen_active(), found, &n);
    // Top to bottom on the glass, for the same reason: every one of these
    // labels sits at the same y inside its own chip.
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            lv_area_t aa, ab;
            lv_obj_get_coords(found[a], &aa);
            lv_obj_get_coords(found[b], &ab);
            if (ab.y1 < aa.y1) {
                lv_obj_t *t = found[a]; found[a] = found[b]; found[b] = t;
            }
        }
    if (idx < 0 || idx >= n) {
        printf("FAIL: expected a %dth '?' chip in the details column, found %d\n",
               idx, n);
        g_walk_fails++;
        return NULL;
    }
    return found[idx];
}

// Press the action saying tr(key), hold for `hold` frames, release, settle for
// `settle`. hold = 3 is an ordinary tap; a hold-to-confirm wants its duration.
// A label whose text starts with `pre`, and the clickable row it sits in. The
// file list is a scrollable column of rows labelled with the filename itself,
// so nothing in STR_* names them and tap_str cannot reach one. Found by prefix
// rather than by y, because which row a file lands on depends on how many
// signatures are already on the card -- a number the walk changes as it goes.
static lv_obj_t *find_label_prefix(lv_obj_t *o, const char *pre) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    return (t && strncmp(t, pre, strlen(pre)) == 0) ? o : NULL;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    lv_obj_t *r = find_label_prefix(lv_obj_get_child(o, i), pre);
    if (r) return r;
  }
  return NULL;
}

// The row, not the label: the label is a child of the pressable object and a
// press on it does not reach the row's own event.
static lv_obj_t *find_row_prefix(lv_obj_t *o, const char *pre) {
  lv_obj_t *l = find_label_prefix(o, pre);
  for (lv_obj_t *up = l ? lv_obj_get_parent(l) : NULL; up;
       up = lv_obj_get_parent(up))
    if (lv_obj_has_flag(up, LV_OBJ_FLAG_CLICKABLE)) return up;
  return NULL;
}

static int tap_row_prefix(const char *pre) {
  lv_obj_t *row = find_row_prefix(lv_screen_active(), pre);
  if (!row) {
    printf("FAIL: no file row starting \"%s\"\n", pre);
    g_walk_fails++;
    return 0;
  }
  lv_obj_scroll_to_view(row, LV_ANIM_OFF);
  pump(4);
  lv_area_t a;
  lv_obj_get_coords(row, &a);
  touch((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
  pump(3);
  release();
  pump(8);
  return 1;
}

// Tap a label by its exact text. For a figure whose x is computed at build
// time -- the input total sits past a caption of translated width and the "?"
// after it -- pressing where it actually landed is the only tap that proves
// anything, and a fixed coordinate would only ever prove the English screen.
static lv_obj_t *find_label_obj_exact(lv_obj_t *o, const char *needle) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    return (t && strcmp(t, needle) == 0) ? o : NULL;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    lv_obj_t *r = find_label_obj_exact(lv_obj_get_child(o, i), needle);
    if (r) return r;
  }
  return NULL;
}

static void tap_label_exact(const char *txt)
{
    lv_obj_t *l = find_label_obj_exact(lv_screen_active(), txt);
    if (!l) {
        printf("FAIL: no label reading \"%s\" to tap\n", txt);
        g_walk_fails++;
        return;
    }
    lv_area_t a; lv_obj_get_coords(l, &a);
    touch((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    pump(3);
    release();
    pump(10);
}

// Tap a control by the text on it. Everything a walk taps this way survives a
// layout change; everything it taps by pixel does not, and does not say so --
// moving the dice card onto the 704 page lane silently moved four taps at once,
// and CLAUDE.md's account of the coverage checker is that the usual outcome is
// worse than a loud failure: a tap that misses leaves every later save()
// photographing whatever is on screen instead, and the sweep comes back clean
// having checked the wrong thing.
static void tap_obj(lv_obj_t *p, int hold, int settle)
{
    if (!p) return;
    lv_area_t a; lv_obj_get_coords(p, &a);
    touch((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    pump(hold);
    release();
    pump(settle);
}

static void tap_lbl(const char *txt, int hold, int settle)
{
    tap_obj(ctrl_for(txt, "tap"), hold, settle);
}

static void tap_str(int key, int hold, int settle)
{
    tap_obj(act_for(key, "tap"), hold, settle);
}



// The two-part form, for a hold the walk photographs partway through.
// The slide-to-confirm bars fire by TRAVEL, not time: grip one by its words
// (the press lands mid-label; the bar measures from wherever the press
// starts) and drag right in indev-cadence steps. slide_go keeps the finger
// down so a stop can photograph a partial fill and keep dragging;
// slide_fire drags far enough to complete any bar on the device (the widest
// track is 330) and lets go.
static int s_slide_x, s_slide_y;
static void slide_grip(int key)
{
    lv_obj_t *p = act_for(key, "slide");
    if (!p) return;
    lv_area_t a; lv_obj_get_coords(p, &a);
    s_slide_x = (a.x1 + a.x2) / 2;
    s_slide_y = (a.y1 + a.y2) / 2;
    touch(s_slide_x, s_slide_y); pump(3);
}
static void slide_go(int px)          // absolute travel from the grip point
{
    for (int i = 1; i <= 6; i++) { touch(s_slide_x + px * i / 6, s_slide_y); pump(3); }
}
static void slide_at(int x, int y, int px)   // grip by coordinate, drag, stay down
{
    s_slide_x = x; s_slide_y = y;
    touch(x, y); pump(3);
    slide_go(px);
}
static void slide_fire(int key)
{
    slide_grip(key);
    slide_go(340);
    release(); pump(8);
}
// The return leg of a DOUBLE TRAVEL slide. The first leg only arrives -- the
// knob parks at the far end and the word becomes ONCE MORE -- so the gesture
// is no longer findable by its own label and the grip is by coordinate.
//
// It starts at the RIGHT end of the control and not at its centre, for the
// reason a real thumb would: leftward travel from the centre of a 310px bar
// runs out of panel at 208px, which is 78% of the 266 this needs, and a leg
// that cannot reach its own end simply opens the pause window instead.
static void slide_back(int x, int y, int px)
{
    touch(x, y); pump(3);
    for (int i = 1; i <= 6; i++) { touch(x - px * i / 6, y); pump(3); }
}

// The unlock word as used throughout the scripted walk. Kept as a helper for
// storage hot-plug coverage added at the end, so that test does not invent a
// second approximation of the gesture recognizer's real input.
// The word ALONE, no settling wait. Split out of draw_cover so a modifier stroke
// can still arrive: main.c holds a bare word for COVER_OPEN_DELAY_MS precisely
// so the stroke after it is classified against the same draw, and draw_cover's
// trailing pump(40) is 140ms past that window.
// ---- free marks, drawn as a hand would ------------------------------------
// Sampled coarser than 10px so the touch layer's decimation keeps them, and
// sized past WDF_MIN_SPAN. Positions are arbitrary on purpose: a free mark has
// no word to sit on, so anywhere on the panel has to work.
static void mark_line(void)  { for (int i = 0; i <= 10; i++) { touch(200 + i * 20, 400); pump(1); } release(); pump(2); }
static void mark_slash(void) { for (int i = 0; i <= 10; i++) { touch(180 + i * 18, 120 + i * 18); pump(1); } release(); pump(2); }
// Kept beside the two marks the walk draws: the free-mark vocabulary is the
// set a duress stroke can be drawn from, and a stop that needs a different
// shape should reach for one of these rather than invent a fourth. Unused
// today, and said so out loud now that these builds carry -Wall -Wextra.
__attribute__((unused)) static void mark_check(void) {
  for (int i = 0; i <= 4; i++) { touch(300 + i * 10, 200 + i * 20); pump(1); }
  for (int i = 1; i <= 6; i++) { touch(340 + i * 15, 280 - i * 25); pump(1); }
  release(); pump(2);
}
__attribute__((unused)) static void mark_circle(void) {
  static const int cx[] = {300,420,420,300,180,180,298};
  static const int cy[] = {140,200,300,360,300,200,143};
  for (unsigned i = 0; i < sizeof cx / sizeof cx[0]; i++) {
    // walk the edge so the ink is a loop, not seven far-apart samples
    int fx = i ? cx[i-1] : cx[0], fy = i ? cy[i-1] : cy[0];
    for (int t = 1; t <= 6; t++) { touch(fx + (cx[i]-fx)*t/6, fy + (cy[i]-fy)*t/6); pump(1); }
  }
  release(); pump(2);
}

// Three strokes inside the writing field (y 110..350), drawn the same way
// twice so gw_matches accepts them. Deliberately not KISS: the whole point of
// the screen is that the letters are the owner's own.
static void draw_own_letters(void)
{
  // Three strokes, and every number here is measured rather than guessed.
  // pump(3) per point: the indev reads about every 30ms, so the pump(1) that
  // draw_cover_word gets away with lands roughly a third of its samples.
  // pump(8) after each release: at pump(4) the lift was seen but the NEXT
  // press, starting where the last one ended, was folded into it -- strokes 2
  // and 3 delivered nothing to the canvas and fell through to the home screen
  // behind, which opened Sign and then SCAN under a write screen that still
  // looked right. Each stroke also starts somewhere new for the same reason.
  // None of this says anything about the device; it is how fast this harness
  // can inject a press, the same finding the 44-character login loop records.
  for (int i = 0; i <= 10; i++) { touch(240, 150 + i * 15); pump(3); }
  release(); pump(8);
  for (int i = 0; i <= 10; i++) { touch(300 + i * 22, 320); pump(3); }
  release(); pump(8);
  for (int i = 0; i <= 10; i++) { touch(560, 150 + i * 15); pump(3); }
  release(); pump(8);
}

// Thirteen strokes: one past GW_MAX_STROKES, which is the number that used to
// enrol twice, confirm, save, and then never open the device again. Same
// coordinates work on the enrolment canvas (y 110..350) and on the game screen,
// which is the whole point -- the owner draws the same thing in both places.
// Twelve strokes and then a thirteenth SHAPED UNLIKE THEM.
//
// The shape matters and the first version of this test proved it: thirteen
// identical verticals passed with the folding bug still in, because folding the
// last one into the twelfth barely moves a row of verticals once it is
// normalised -- distance 26 against a threshold of 26. sim/test_gword.c found
// the same floor. The failure needs a tail the body does not predict, which is
// what a crossbar, an underline or a flourish is, and what most people's
// handwriting ends in.
//
// Everything stays inside y 110..350: that is the enrolment canvas, and a
// stroke outside it is simply not captured there, which would make this test
// pass for the wrong reason.
static void draw_thirteen_strokes(void)
{
  for (int s = 0; s < 12; s++) {
    for (int i = 0; i <= 3; i++) { touch(120 + s * 40, 150 + i * 43); pump(3); }
    release(); pump(8);
  }
  for (int i = 0; i <= 11; i++) { touch(120 + i * 40, 330); pump(3); }
  release(); pump(8);
}

static void draw_cover_word(void)
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

static void draw_cover(void)
{
  draw_cover_word();
  // 40 frames, not 4. Whichever door this call ends up taking, main.c may hold
  // it for COVER_OPEN_DELAY_MS so the real wallet and the decoy cannot be told
  // apart by how fast the screen arrives. At 16ms a frame this is 640ms of
  // slack over a 500ms wait. Callers that land on the setup wizard or the
  // amnesic loader open immediately and do not need it; the slack costs them
  // nothing and stops this helper breaking if a caller changes door.
  pump(40);
}

// The word, then one wide flat stroke under it: the shape sim/test_duress.c
// pins as WDG_UNDERLINE, drawn through the real touch layer so the whole path
// runs -- detect_cover_word, kiss_duress_classify, unlock_kind -- and not only the
// classifier the unit test reaches on its own.
static void draw_cover_underlined(void)
{
  draw_cover_word();
  pump(2);
  for (int i = 0; i <= 20; i++) { touch(150 + i * 20, 315); pump(1); }
  release();
  pump(40);
}

// The two tap way in, drawn where a finger draws it: the same corner twice,
// well inside CW_QT_MS. Coordinates are the top left box kiss_coverword.h
// defines, which is also where the logo sits on the home screen.
static void quick_tap(void)
{
  touch(40, 40); pump(3); release(); pump(6);
}

// kiss_lock() sets s_gest_swallow so the rest of the closing tap cannot
// become the first stroke of a word, and it clears on the next lift. Without a
// throwaway lift the K's spine is eaten and the word never completes. The
// corner is chosen because the menu's "tap to play" would start the game, and
// the gesture collector is skipped entirely while ST_PLAY.
static void lock_to_menu(void)
{
  extern void kiss_wiped_lock(void);   // = kiss_lock(); it wipes nothing
  kiss_wiped_lock();
  pump(30);
  touch(6, 470); pump(2); release(); pump(6);
}

// These prefixes uniquely select the first twelve SIM_WORDS from suggestion 0.
// They are the one shared fixture for a clean restore and the later rehearsal
// of the same words, so either path changing its autocomplete semantics breaks
// both at the real recovery keyboard rather than drifting into two fake seeds.
static const char *const SIM_12_PREFIXES[12] = {
  "g", "m", "no", "so", "sy", "fem",
  "fi", "at", "v", "fo", "c", "stay"
};

static void type_restore_prefix(const char *prefix)
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
}

// Type a short prefix on kiss_setup.c's recovery-word keyboard, then choose
// its first suggestion. Keeping this as a real touch walk means restore and
// the optional recovery rehearsal use the exact UI a person uses.
static void restore_word(const char *prefix)
{
  type_restore_prefix(prefix);
  // pump(8) after the release, not pump(3). The indev reads a press about
  // every 30ms, so at three frames the lift is seen but the NEXT press folds
  // into it -- and this helper is called eleven and twenty three times in a
  // row, so the error accumulates until a whole word goes missing and every
  // assertion after it is about a different screen. The same number, for the
  // same reason, as the nine-key passphrase loop below.
  touch(55, 182); pump(3); release(); pump(8);    // first suggestion (a bare word at 48,162 now)
}

// The wallet's passphrase, nine of one letter. pass_bits() wants 40 to get past
// the weak refusal and a lowercase pool is 4.7 bits a character, so nine is the
// shortest thing that opens the wizard at all. One repeated key keeps the tap
// coordinates exactly as simple as the single 'a' this replaces -- and every
// later login has to type the SAME thing, or it opens a different wallet under
// a different fingerprint and every assertion after it is about the wrong one.
// pump(6), not pump(3): the indev reads about every 30ms and these are nine
// presses of the SAME key, so at pump(3) the lift is seen but the next press
// folds into it and the count comes up short. The 44 character stress loop
// below already uses 6 for exactly this reason.
static void type_pass9(void)
{
  for (int i = 0; i < 9; i++) { touch(46, 278); pump(6); release(); pump(6); }
}

// ---- the walk owns its fixtures ----
// /tmp/simsd is platform_sd.c's SD_BASE on the host, and THREE binaries write
// into it: this walk, /tmp/kissoverlap (the same walk instrumented) and
// /tmp/kisstest (test_sdseed.c and test_fw.c both put files
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
// Nothing outside a run depends on what a previous run left.
//
// NOT a fix for run-to-run flakiness. The walk has been seen to give different
// results from a byte identical starting state under machine load; that is a
// separate defect and this function does not address it. What this buys is
// that the starting state is at least the same every time, so the next person
// bisecting has one fewer variable.
// Under KISS_SIM_TMP now, not only under a -DSIMSD: a compile-time override
// separates two BUILDS, and what was colliding is two RUNS of the same build,
// in one checkout, by different people. See kiss_simpath.h. Unset, this is
// still exactly "/tmp/simsd".
#ifndef SIMSD
KISS_SIM_PATH_FN(simsd_path, "simsd")
#define SIMSD simsd_path()
#endif

// Remove one file from the fake card by name. Every one of these used to be a
// literal "/tmp/simsd/x", which was the same string SIMSD expanded to and so
// looked equivalent -- until the card moved under KISS_SIM_TMP and the literals
// went on deleting a card nobody was using. The walk's REMOVE step then left
// four files standing, every row below them shifted, and three later stops
// photographed the wrong transaction while reporting a needle that was missing.
static void sd_unlink(const char *name) {
  char p[256];
  snprintf(p, sizeof p, "%s/%s", SIMSD, name);
  unlink(p);
}

static FILE *sd_fopen(const char *name, const char *mode) {
  char p[256];
  snprintf(p, sizeof p, "%s/%s", SIMSD, name);
  return fopen(p, mode);
}

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
      snprintf(doomed[n++], sizeof doomed[0], "%s/%s", SIMSD, e->d_name);
    }
    closedir(d);
  }
  for (int i = 0; i < n; i++) remove(doomed[i]);

  // The six the sign walk taps, and the content each one's verify screen is
  // built from (sim_main.c's kiss_psbt_load stub branches on these words).
  // The names are chosen so a plain sort puts them in the order the walk taps:
  // payment-01, risky-STOP, silly-FEE, warn-COMBO, zsp-SPAY, zzz-UNPRV.
  static const struct { const char *name, *body; } FIXTURES[] = {
    { "payment-01.psbt", "fake-psbt-binary" },
    { "risky-STOP.psbt", "STOP"  },
    { "silly-FEE.psbt",  "FEE"   },
    { "warn-COMBO.psbt", "COMBO" },
    { "zsp-SPAY.psbt",   "SPAY"  },
    { "zzz-UNPRV.psbt",  "UNPRV" },
    // Sorts LAST on purpose: the walk taps rows by position, so a fixture
    // inserted anywhere else would shift every tap after it.
    //
    // The name is long because signed_name clamps the output to 63 bytes and
    // nothing on the SIGNED screen used to bound the label that shows it. This
    // one produces exactly 63 -- the worst case, reachable from a coordinator
    // that names its exports after the wallet and the date -- and it is the
    // only fixture that produces anything but a short one. The prefix and the
    // MANY body are unchanged, so it sorts where it always did and the walk's
    // position taps are untouched.
    { "zzzz-MANY-recipients-export-from-the-coordinator-app-01.psbt", "MANY" },
    // ... and this one sorts after THAT, for the same reason. Twenty coins into
    // one recipient with no change: the shape the elided middle exists for, and
    // the only fixture where n_total exceeds what ins[] can hold.
    { "zzzzz-MERGE.psbt", "MERGE" },
  };
  for (unsigned i = 0; i < sizeof FIXTURES / sizeof FIXTURES[0]; i++) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", SIMSD, FIXTURES[i].name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(FIXTURES[i].body, f); fclose(f); }
  }

  // kiss_seed.c and kiss_seed_sd.c persist to these on the host build.
  static const char *const STATE[] = {
    "kiss_seed.txt",      "kiss_seed.txt.tmp",
    "kiss_seed_mode.txt", "kiss_seed_mode.txt.tmp",
    "kiss_seed_entq.txt", "kiss_device_key.bin",
  };
  for (unsigned i = 0; i < sizeof STATE / sizeof STATE[0]; i++) {
    char sp[192];
    remove(kiss_sim_path(sp, sizeof sp, STATE[i]));
  }
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
  kiss_trng_start();

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

  // Focused boot-routing harness for tools/check_screen_coverage.py. Each
  // non-OK settings status runs in its own process, before any game object or
  // timer exists, and must stop on the literal safe-mode screen. This screen
  // cannot participate in wt_screen's translated-title registry, so a separate
  // machine-readable marker is the only honest way to include it in coverage.
  const char *safe_boot = getenv("SCREENCOVER_SAFEBOOT");
  if (safe_boot && *safe_boot) {
    char *end = NULL;
    long raw = strtol(safe_boot, &end, 10);
    if (!end || *end || raw < WSETTINGS_LOAD_NVS_NO_FREE_PAGES ||
        raw > WSETTINGS_LOAD_NVS_READ_FAILED) {
      fprintf(stderr, "bad SCREENCOVER_SAFEBOOT status: %s\n", safe_boot);
      return 1;
    }
    int err = 0x5A00 + (int)raw;
    kiss_settings_sim_set_load_result((kiss_settings_load_status_t)raw, err);
    build_game();
    pump(20);
    char cause[32];
    snprintf(cause, sizeof cause, "code 0x%X", err);
    must_show("safe-boot/title", "STORAGE LOCKED");
    must_show("safe-boot/cause", cause);
    must_not_show("safe-boot/no-game", "TAP TO PLAY");
    save("/tmp/sim_storage_locked.ppm");   // own process: not in walk order
    printf("SAFEBOOT\tSTORAGE LOCKED\t%s\n",
           kiss_settings_load_status_name((kiss_settings_load_status_t)raw));
    printf("sim done\n");
    return g_walk_fails ? 1 : 0;
  }

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

  // baked game-over buttons: PLAY AGAIN restarts, MENU -> main menu
  touch(400, 410); pump(3); release(); pump(4);     // PLAY AGAIN -> fresh run
  save("/tmp/sim_playagain.ppm");                   // HUD back: score 0, hearts
  for (int i = 0; i < 700; i++) pump(1);            // fruit fall unsliced -> game over again
  touch(682, 57); pump(3); release(); pump(120);    // MENU -> menu (full re-intro settles)
  save("/tmp/sim_menu_back.ppm");

  // draw the word "KISS" -> the SPARE signer appears (K spine+arms, I, S, S)
  //
  // The bare word opens the spare and nothing else, on every device, configured
  // or not: that is kiss_duress_route, and the routing block a hundred lines
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
  draw_cover();                                      // = the word; helper waits out COVER_OPEN_DELAY_MS
  pump(5); g_seq_on = 0;                            // hold on the reveal, then stop recording
  save("/tmp/sim_spare_home.ppm");                  // the spare, opened by the word alone

  // The passphrase keyboard takes the word AND the modifier stroke. Nothing
  // else reaches it, so everything below has to come in that way.
  lock_to_menu();
  draw_cover_underlined();
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
  tap_str(STR_L_TAP_TO_OPEN, 3, 12);    // TAP TO OPEN -> hex noise decrypting
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
  // kiss_duress_real(): a configured device opened the decoy on a bare word,
  // an unconfigured one showed a passphrase keyboard. Drawing the word ONCE
  // told an attacker holding the device which kind it was. The byte in flash
  // was never the leak; the behaviour was.
  //
  // kiss_duress_route is unit tested in sim/test_duress.c, but that proves
  // the rule in isolation. This drives the whole path through the real touch
  // layer, because unlock_kind lives in main.c and no test binary links it --
  // which is exactly how the fork survived long enough to become a finding.
  //
  // HERE, and not at the end of the walk, because kiss_lock() returns early
  // unless s_wallet_on. By the tail a login screen sits on top with the wallet
  // already closed, so the menu never comes forward, the touches land on the
  // keyboard and not one point of ink reaches the collector. This is the last
  // point where the home is genuinely open. The block restores the session it
  // borrows, so every frame after it is unaffected.
  {
    extern int g_last_unlock_kind;
    const int cfgs[] = { WDG_UNDERLINE, WDG_NONE };
    for (unsigned c = 0; c < 2; c++) {
      kiss_duress_set(cfgs[c]);
      const char *tag = cfgs[c] == WDG_NONE ? "routing/no-stroke"
                                            : "routing/stroke-set";

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_cover();
      if (g_last_unlock_kind != WDR_DECOY) {
        printf("FAIL: %s: word alone routed %d, expected WDR_DECOY (%d)\n",
               tag, g_last_unlock_kind, WDR_DECOY);
        g_walk_fails++;
      }
      must_not_show(tag, tr(STR_L_TYPE_PROMPT));

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_cover_underlined();
      if (g_last_unlock_kind != WDR_REAL) {
        printf("FAIL: %s: word + stroke routed %d, expected WDR_REAL (%d)\n",
               tag, g_last_unlock_kind, WDR_REAL);
        g_walk_fails++;
      }

      // The REAL route left the passphrase keyboard up, and lock_to_menu is a
      // no-op while it is (kiss_lock returns early unless s_wallet_on). So
      // finish the same login the walk used to get here -- 'a', OK, TAP TO OPEN
      // -- and every pass, including the last, ends on the open home.
      type_pass9();
      touch(725, 430); pump(3); release(); pump(25);
      tap_str(STR_L_TAP_TO_OPEN, 3, 12);
      pump(120);
    }
    kiss_duress_set(WDG_NONE);

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
      draw_cover_word();                 // the letters KISS, as any word would be
      pump(4);
      gw_template_t t;
      if (sim_capture_word(&t) != 0 || gw_stored_set(&t) != 0) {
        printf("FAIL: written word: could not teach the device a word\n");
        g_walk_fails++;
      }
      pump(200);                        // let the collector's idle clear run

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_cover();                      // the same word, no mark after it
      if (g_last_unlock_kind != WDR_DECOY) {
        printf("FAIL: written word: the word alone routed %d, expected WDR_DECOY (%d)\n",
               g_last_unlock_kind, WDR_DECOY);
        g_walk_fails++;
      }
      must_not_show("written/word-alone", tr(STR_L_TYPE_PROMPT));

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_cover_underlined();           // the word plus one mark
      if (g_last_unlock_kind != WDR_REAL) {
        printf("FAIL: written word: word plus a mark routed %d, expected WDR_REAL (%d)\n",
               g_last_unlock_kind, WDR_REAL);
        g_walk_fails++;
      }
      type_pass9();
      touch(725, 430); pump(3); release(); pump(25);
      tap_str(STR_L_TAP_TO_OPEN, 3, 12);
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
      draw_cover_underlined();
      type_pass9();
      touch(725, 430); pump(3); release(); pump(25);
      tap_str(STR_L_TAP_TO_OPEN, 3, 12);
      pump(120);
      printf("ok: a written word replaces KISS, and a wrong draw opens nothing\n");
    }

    // ---- two taps, and only where the coins are not real --------------------
    //
    // Reachable from no unit test for the same reason the routing above is not:
    // the network condition and the corner both live in main.c. cw_quick_tap
    // itself is under test in sim/test_coverword.c; what this proves is that
    // the panel, the collector and kiss_testnet() are wired to it.
    {
      lock_to_menu();
      pump(4);
      quick_tap(); quick_tap();
      pump(40);
      uint8_t fp[4];
      kiss_ui_last_fp(fp);
      if (!(fp[0] || fp[1] || fp[2] || fp[3])) {
        printf("FAIL: two taps on testnet opened nothing\n");
        g_walk_fails++;
      }
      must_not_show("quicktap/no-keyboard", tr(STR_L_TYPE_PROMPT));
      save("/tmp/sim_quick_unlock.ppm");   // the home, opened by two taps

      // MAINNET: the corner is not a door, it is the game. One tap starts
      // Fruit Island exactly as it always has, and the pair opens nothing --
      // which is the only property here worth a device holding real coins.
      lock_to_menu();
      kiss_set_network(KISS_NET_MAIN);
      pump(4);
      quick_tap(); quick_tap();
      pump(40);
      kiss_ui_last_fp(fp);
      if (fp[0] || fp[1] || fp[2] || fp[3]) {
        printf("FAIL: two taps opened the signer on MAINNET\n");
        g_walk_fails++;
      }
      save("/tmp/sim_quick_mainnet.ppm");  // the game, started by the first tap
      kiss_set_network(KISS_NET_TESTNET);

      // The first tap started a game, so come back the way the walk already
      // does: let the fruit fall unsliced, then the MENU button.
      for (int i = 0; i < 700; i++) pump(1);
      touch(682, 57); pump(3); release(); pump(120);

      // Hand the walk back the session the rest of it expects.
      draw_cover_underlined();
      type_pass9();
      touch(725, 430); pump(3); release(); pump(25);
      tap_str(STR_L_TAP_TO_OPEN, 3, 12);
      pump(120);
      printf("ok: two taps open a testnet signer and nothing on mainnet\n");
    }

    // Identical expectations for both configurations is the whole test: if
    // either arm ever needs a different one, the fork is back.
    printf("ok: unlock routing identical with and without a stroke configured\n");

    // ---- the RECOVER screen vs the lock, with a live session under it ----
    //
    // The screen that says the staged words in RAM are the last copy there is
    // hangs off the active screen and was on neither of main.c's lists, so
    // with a session open the game kept reading the glass underneath it: the
    // corner tap locked instantly, the tile bands opened Sign or Settings
    // under the words, and five untouched minutes of reading it ended in
    // kiss_lock() -- the identical parenting bug the FIRMWARE screen already
    // paid for, on the one screen whose job is to be read slowly.
    //
    // Here and not at the leaf near the end of the walk: the leaf runs
    // post-lock, where s_wallet_on is already false and every assert below
    // would pass on the broken code too. This is the last window where the
    // home is genuinely open, same as the stroke-budget block that follows.
    {
      uint8_t fp0[4], fp1[4];
      kiss_ui_test_recover_screen();
      pump(40);

      // A tap in the first tile band must not open the Sign chooser under it.
      touch(130, 240); pump(3); release(); pump(12);
      if (kiss_sign_active()) {
        printf("FAIL: a tile opened the Sign chooser under the recover screen\n");
        g_walk_fails++;
        kiss_sign_close(); pump(20);
      }

      // Five untouched minutes must not lock. kiss_lock() and nothing else on
      // this path zeroes the open session's fingerprint, so that is the probe:
      // a frame of this screen looks correct locked or not.
      kiss_ui_last_fp(fp0);
      pump(20000);                                   // 320s > 300s, untouched
      kiss_ui_last_fp(fp1);
      if (!(fp0[0] || fp0[1] || fp0[2] || fp0[3]) ||
          memcmp(fp0, fp1, sizeof fp0) != 0) {
        printf("FAIL: idle auto-lock fired with the recover screen up\n");
        g_walk_fails++;
      }
      must_show("recover/idle", tr(STR_L_RECOVER_T));

      // The corner lock must not fire through it either.
      touch(44, 44); pump(3); release(); pump(20);
      kiss_ui_last_fp(fp1);
      if (!(fp1[0] || fp1[1] || fp1[2] || fp1[3])) {
        printf("FAIL: the corner tap locked through the recover screen\n");
        g_walk_fails++;
      }

      // SHOW WORDS opens the ordinary words screen reading the staged copy.
      // Five untouched minutes of transcription must not tear it down: the
      // registered kiss_info close would delete the one screen naming the
      // words, and the lock would strand the last copy in silent RAM.
      tap_str(STR_I_SHOW_WORDS, 3, 12);              // SHOW WORDS
      if (!kiss_info_active()) {
        printf("FAIL: SHOW WORDS did not open the words screen\n");
        g_walk_fails++;
      }
      pump(20000);                                   // untouched, mid-copy
      if (!kiss_info_active()) {
        printf("FAIL: auto-lock tore down the words screen opened from "
               "recover; the staged copy is stranded\n");
        g_walk_fails++;
      } else {
        tap_str(STR_C_BACK, 3, 20);                    // BACK -> recover again
        must_show("recover/back", tr(STR_L_RECOVER_T));
      }

      kiss_ui_test_recover_close();
      pump(40);
      printf("ok: the recover screen owns the glass and the clock\n");
    }

    // ---- the deadline on an ordinary login, fingerprint screen up ----
    //
    // TAP TO OPEN commits with whatever s_pass holds. A wipe that left the
    // fingerprint screen standing would let that tap open a wallet derived
    // from an EMPTY passphrase under a fingerprint computed from the typed
    // one -- so expiry has to drop the screen back to the keyboard, and the
    // tap that would have opened lands on keys instead.
    {
      lock_to_menu();
      pump(200);
      draw_cover_underlined();
      type_pass9();                                   // the wallet's passphrase
      touch(725, 430); pump(3); release(); pump(25);  // OK -> fingerprint
      pump(8200);                                     // 131s; nobody confirms
      must_not_show("idle-wipe/fp", tr(STR_L_TAP_TO_OPEN));
      must_show("idle-wipe/fp-prompt", tr(STR_L_TYPE_PROMPT));
      touch(46, 278);  pump(3); release(); pump(3);   // 'a', typed fresh
      touch(725, 430); pump(3); release(); pump(25);  // OK -> fingerprint
      tap_str(STR_L_TAP_TO_OPEN, 3, 12);              // TAP TO OPEN -> home
      pump(120);
      printf("ok: the idle deadline wipes the entry and drops the "
             "fingerprint screen\n");
    }

    // ---- a word with MORE strokes than the budget, end to end ----
    //
    // sim/test_gword.c pins this at the template layer; this is the version
    // that drives both halves through their own real code, which is the only
    // way the drift was ever going to show. Enrolment folded every stroke past
    // its twelfth INTO the twelfth and unlock kept the boundary, so the owner
    // wrote their word twice, confirmed it, held to save, and owned a signer
    // that had stopped answering to them. Both collectors take their limits
    // from kiss_gword.h now and the excess is dropped rather than folded.
    //
    // Twelve strokes are stored and thirteen are drawn, so main.c reads the
    // extra one as the mark that picks the wallet -- hence WDR_REAL and the
    // login below. What was broken is that it routed NOWHERE.
    //
    // Inside this block on purpose, for the reason its own header gives: this
    // is the last point in the walk where the home is genuinely open, and
    // lock_to_menu is a no-op anywhere after it.
    {
      gw_stored_set(NULL);                            // no BACK TO KISS action
      pump(20);
      kiss_word_ui_open(lv_screen_active(), NULL);
      pump(20);
      draw_thirteen_strokes();
      tap_str(STR_GD_WORD_DONE, 3, 8);   // DONE -> once more
      draw_thirteen_strokes();
      tap_str(STR_GD_WORD_DONE, 3, 8);   // DONE -> the stop screen
      slide_fire(STR_GD_WORD_HOLD);      // SLIDE TO CHANGE -> saved
      save("/tmp/sim_gword_manystroke.ppm");          // YOUR LETTERS ARE SET
      tap_str(STR_C_OK, 3, 8);   // OK -> back to the home
      if (!gw_stored_any()) {
        printf("FAIL: a thirteen stroke word did not enrol\n");
        g_walk_fails++;
      }

      lock_to_menu();
      g_last_unlock_kind = -2;
      draw_thirteen_strokes();
      pump(40);
      if (g_last_unlock_kind != WDR_REAL) {
        printf("FAIL: a thirteen stroke word enrolled and then routed %d, "
               "expected WDR_REAL (%d)\n", g_last_unlock_kind, WDR_REAL);
        g_walk_fails++;
      }

      // Finish the login it just asked for, so the walk gets its home back.
      type_pass9();
      touch(725, 430); pump(3); release(); pump(25);
      tap_str(STR_L_TAP_TO_OPEN, 3, 12);
      pump(120);
      gw_stored_set(NULL);
      pump(200);
      printf("ok: a word past the stroke budget enrols and still opens it\n");
    }

    // Identical expectations for both configurations is the whole test: if
    // either arm ever needs a different one, the fork is back.
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
  save("/tmp/sim_recv.ppm");                        // THIS ADDRESS, the landing tab
  // The page's [ ? ]: the lane replaced by the explainer, and back out by
  // the mark. help_seen is put back afterwards so every later frame keeps
  // its meaning -- the KEYS stop further down is the walk's canonical
  // first-run hint and its band must still read as a first run.
  touch(720, 85); pump(3); release(); pump(45);   // 45: the fact rows land on the stagger
  save("/tmp/sim_recv_what.ppm");
  must_show("recv/help head", tr(STR_R_HELP_HEAD));
  touch(720, 85); pump(3); release(); pump(45);   // 45 outlasts the exit stagger
  must_show("recv/help closed", tr(STR_R_NEXT_ADDR));
  wt_help_seen_set(false);
  // The flex strip sizes its brackets to the words, so the tabs are tapped
  // by their labels rather than by a pitch that no longer exists.
  tap_str(STR_R_TAB_SP, 3, 40);                     // SILENT tab
  save("/tmp/sim_recv_sptab.ppm");                  // two lines and the sentence
  // The stroke reaches [ ? ] here too: past SILENT opens the explainer, a
  // right stroke on it comes back to the tab it left. help_seen is dropped
  // again afterwards, for the reason the toggle stop above gives.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("recv/swipe past silent opens help", tr(STR_R_HELP_HEAD));
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("recv/swipe closes help", tr(STR_R_SP_ADDR_CAP));
  wt_help_seen_set(false);
  // The SCAN KEY line is a DOOR again -- to the one consent flow, which lives
  // in kiss_info beside the KEYS launcher. The gate must open, and CANCEL
  // must land back on this tab, not on the landing tab.
  touch(400, 229); pump(3); release(); pump(15);    // SCAN KEY row -> the gate
  save("/tmp/sim_recv_spgate.ppm");
  must_show("recv/sp-scan-door", tr(STR_K_SPGATE_SENT));
  tap_str(STR_C_CANCEL, 3, 45);                     // CANCEL -> back to SILENT
  must_show("recv/sp-tab-back", tr(STR_R_SP_ADDR_CAP));
  touch(400, 153); pump(3); release(); pump(6);     // SILENT ADDRESS -> SP view
  save("/tmp/sim_recv_sp.ppm");                     // folded text + largest receive QR
  touch(196, 248); pump(3); release(); pump(6);     // QR -> full-screen scan view
  save("/tmp/sim_recv_sp_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom, exact state preserved
  touch(520, 166); pump(3); release(); pump(6);     // folded address itself -> full
  save("/tmp/sim_recv_sp_full.ppm");
  tap_str(STR_R_SP_SHOW_SHORT, 3, 6);     // SHOW SHORT -> folded default
  touch(730, 50); pump(3); release(); pump(30);     // ? -> sp1/bc1p explanation
  save("/tmp/sim_recv_sp_help.ppm");
  tap_str(STR_C_OK, 3, 6);     // OK closes the explanation
  tap_str(STR_C_BACK, 3, 6);     // BACK from SP -> RECEIVE, still on tab 3
  tap_str(STR_R_ALL_ADDR, 3, 40);                   // ALL ADDRESSES tab
  save("/tmp/sim_recv_list.ppm");                   // four lines and the count
  // Mid-flight: the second tab change is photographed before it settles, so
  // one frame in the repo shows a line risen with its rule still drawing.
  tap_str(STR_R_TAB_THIS, 3, 4);
  save("/tmp/sim_recv_tabmid.ppm");
  pump(40);
  tap_str(STR_R_ALL_ADDR, 3, 40);                   // back to the list
  // The deck. The scroll window, its settle correction and the arrow chips
  // all went with the swipe pass: a stroke turns the page of three, so the
  // walk strokes it for real -- left is later addresses, and the count line
  // is the only position indicator (34 pages is too many dots).
  char anb[32];
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(30);
  snprintf(anb, sizeof anb, tr(STR_R_ADDR_N_FMT), 3);
  must_show("recv/swipe page2", anb);               // #3 leads the second page
  save("/tmp/sim_recv_scrolled.ppm");               // reused name: now page 2
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(30);
  snprintf(anb, sizeof anb, tr(STR_R_ADDR_N_FMT), 6);
  must_show("recv/swipe page3", anb);
  save("/tmp/sim_recv_page2.ppm");                  // reused name: now page 3
  // Two strokes back to page 1...
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(30);
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(30);
  snprintf(anb, sizeof anb, tr(STR_R_ADDR_N_FMT), 0);
  must_show("recv/swipe back to page1", anb);
  // ...and one more RIGHT: page 1 is the deck's left edge inside this tab,
  // so the stroke crosses the tab boundary onto THIS ADDRESS.
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(40);
  must_show("recv/deck crossed to tab 0", tr(STR_S_CMP_8));
  save("/tmp/sim_recv_deck_home.ppm");
  tap_str(STR_R_ALL_ADDR, 3, 40);       // back in by the strip: remembered page
  // A line HELD, so one frame shows the accent rail and the pressed wash --
  // the entire affordance of a row with no border. The release is the tap that
  // opens it, so this is the same gesture the next frame is the result of:
  // there is no way to photograph a press without eventually completing it.
  touch(400, 204); pump(12);
  save("/tmp/sim_recv_held.ppm");
  release(); pump(40);                              // -> that address on tab 1
  save("/tmp/sim_recv_detail.ppm");                 // QR + address + lamp + path
  // The address itself folds: one tap shows the whole thing in grouped fours
  // with the lit tail, one tap folds it back. The only full-address text
  // render on the device.
  touch(500, 170); pump(3); release(); pump(20);    // fold -> whole
  save("/tmp/sim_recv_addr_full.ppm");
  touch(500, 190); pump(3); release(); pump(20);    // whole -> fold
  touch(156, 228); pump(3); release(); pump(6);     // QR card -> zoom
  save("/tmp/sim_recv_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
  touch(156, 353); pump(3); release(); pump(6);     // TAP TO ENLARGE -> the same zoom
    save("/tmp/sim_recv_zoom_line.ppm"); // same zoom by design; must be identical
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
  touch(350, 128); pump(3); release(); pump(20);    // ADDRESS #N -> the index popover
  save("/tmp/sim_recv_pop.ppm");
  touch(446, 170); pump(3); release(); pump(20);    // pick the first offered index
  touch(500, 370); pump(3); release(); pump(30);    // the path digits -> explainer
  save("/tmp/sim_recv_path_help.ppm");
  tap_str(STR_C_OK, 3, 6);
  tap_str(STR_R_NEXT_ADDR, 3, 8);     // NEXT ADDRESS -> next unused index
  save("/tmp/sim_recv1.ppm");
  {  // VERIFY: own, valid-but-not-found, wrong-network, invalid, then own SP.
    tap_str(STR_R_VERIFY, 3, 6);   // VERIFY action -> raw scan screen
    // TESTNET, because the walk is on testnet here. This was the mainnet form
    // of the same key, so the frame named "yes" rendered WRONG NETWORK -- the
    // same red screen sim_vfy_wrong_net.ppm already photographs, published
    // under a caption promising green. The URI wrapper and the amount param
    // stay: stripping those is what vfy_norm is for and this is where it is
    // exercised.
    const char *good = "BITCOIN:TB1QCR8TE4KR609GCAWUTMRZA0J4XV80JY8Z3Q00?amount=0.001";
    kiss_scan_inject(good, strlen(good)); pump(6);
    save("/tmp/sim_vfy_yes.ppm");
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);   // SCAN ANOTHER
    const char *bad = "bc1qnotmineatallnotmineatallnotmine00";
    kiss_scan_inject(bad, strlen(bad)); pump(6);
    save("/tmp/sim_vfy_no.ppm");
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);   // SCAN ANOTHER
    const char *wrong_net = "tb1qwrongnetworkwrongnetworkwrongnetwork00";
    kiss_scan_inject(wrong_net, strlen(wrong_net)); pump(6);
    save("/tmp/sim_vfy_wrong_net.ppm");
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);   // SCAN ANOTHER
    const char *invalid = "not-an-address";
    kiss_scan_inject(invalid, strlen(invalid)); pump(6);
    save("/tmp/sim_vfy_invalid.ppm");
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);   // SCAN ANOTHER
    // our OWN silent-payment address: 117 chars, longer than any bc1/tb1
    const char *sp = "sp1qqfqnnv8czppwysafq3uwgwvsc638hc8rx3hscuddh0xa2yd746s7xq"
                     "h6yy9ncjnqhqxazct0fzh98w7lpkm5fvlepqec2yy0sxlq4j6ccc3h6t0g";
    kiss_scan_inject(sp, strlen(sp)); pump(6);
    save("/tmp/sim_vfy_sp.ppm");

    // A coordinator's usage payload, which rides in the SAME square as the
    // address. The fingerprint is read rather than written down: a literal
    // would stop matching the moment the walk opened a different session, and
    // it would fail as a passing test of the "other keys" branch rather than as
    // a failing test of this one.
    uint8_t ufp[4];
    kiss_ui_last_fp(ufp);
    char um[160];
    snprintf(um, sizeof um, "KISSU1 %02X%02X%02X%02X %d %d 29 1234567 %s",
             ufp[0], ufp[1], ufp[2], ufp[3],
             kiss_testnet() ? 1 : 0, kiss_script(),
             "TB1QCR8TE4KR609GCAWUTMRZA0J4XV80JY8Z3Q00");
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);
    kiss_scan_inject(um, strlen(um)); pump(6);
    save("/tmp/sim_vfy_usage.ppm");          // ownership answered AND usage recorded

    // The same square again. Nothing may move: the height gate has already seen
    // this one, and a re-scan that silently rolled the badge back would be the
    // privacy-harmful direction.
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);
    kiss_scan_inject(um, strlen(um)); pump(6);
    save("/tmp/sim_vfy_usage_again.ppm");

    // Someone else's coordinator. The ownership answer must still land -- that
    // is what the owner came for and it needs nothing from the payload.
    char umx[160];
    snprintf(umx, sizeof umx, "KISSU1 00112233 %d %d 5 2000000 %s",
             kiss_testnet() ? 1 : 0, kiss_script(),
             "TB1QCR8TE4KR609GCAWUTMRZA0J4XV80JY8Z3Q00");
    tap_str(STR_R_SCAN_ANOTHER, 3, 6);
    kiss_scan_inject(umx, strlen(umx)); pump(6);
    save("/tmp/sim_vfy_usage_other.ppm");

    tap_str(STR_C_DONE, 3, 6);   // DONE -> Receive
  }
  // The USED lamp, which nothing in the walk reaches on its own: an address is
  // marked used by SIGNING a spend from it, three steps later and on a
  // different key set. So it had no picture anywhere in the repo, and it is
  // the state that matters -- UNUSED is the happy default nobody has to read.
  //
  // Mark one, reopen so the landing index is past it, then step BACK through
  // the popover onto the marked one. That exercises the route an owner takes
  // to reach a used address as well as the lamp it lights.
  {
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    kiss_usage_mark(fp, kiss_testnet() ? 1 : 0, kiss_script(), 10);
    tap_str(STR_C_BACK, 3, 6);                       // -> home
    touch(310, 240); pump(3); release(); pump(20);   // Receive: lands on #11
    touch(350, 128); pump(3); release(); pump(20);   // ADDRESS #11 -> popover
    save("/tmp/sim_recv_pop_used.ppm");              // #9 and #10 read USED
    touch(446, 170); pump(3); release(); pump(30);   // pick #9
    save("/tmp/sim_recv_used.ppm");                  // the amber lamp and its line
    tap_str(STR_R_ALL_ADDR, 3, 40);                  // ALL ADDRESSES, mixed states
    save("/tmp/sim_recv_list_used.ppm");
    kiss_usage_wipe();                               // leave the walk as it was
    tap_str(STR_R_TAB_THIS, 3, 40);                  // back to THIS ADDRESS
  }
  // And the same lamp once a COORDINATOR has spoken: their number wins when it
  // is larger, because this signer has no chain view and theirs does.
  save("/tmp/sim_recv_told.ppm");   // the chip, now that a coordinator has spoken
  tap_str(STR_C_BACK, 3, 4);     // BACK (leftmost now) -> home
  touch(680, 60); pump(3); release(); pump(40);     // fingerprint chip -> education card
  save("/tmp/sim_home_fp.ppm");
  touch(400, 414); pump(3); release(); pump(6);     // OK closes the card
  // 45, not 6. The lines rise and their VALUES land behind them on a delay --
  // the handoff's motion 4 and 5 -- so six frames photographs a screen whose
  // captions have arrived and whose values are still at opacity zero. The
  // frame looked like a rendering fault and was really a frame taken early,
  // which is the second time that has happened on this walk.
  touch(490, 240); pump(3); release(); pump(45);    // Wallet tile -> section home
  save("/tmp/sim_winfo.ppm");
  // The in-place definition, the pass's central interaction and the reason
  // the fp/type help cards left this page: NETWORK opens where it stands, the
  // other rows drop to 34px ghosts, and the plain sentence lands inside the
  // grown row. Three rows now -- the fingerprint hero is gone, the home page
  // already headlines the same code -- so NETWORK is the top third at
  // 114..208. 30 pumps: the height animation is 240ms and the body rides in
  // 90ms behind it.
  touch(400, 160); pump(3); release(); pump(8);     // NETWORK row -> opening
  // Heights in transit: the open row growing and the ghosts collapsing in
  // the same tick. Raw, not saved -- a mid-flight frame must never become a
  // stop the settled-state gates compare against.
  shot_raw("sim_winfo_def_mid.ppm");
  pump(22);                                         // and let it settle
  save("/tmp/sim_winfo_def.ppm");
  must_show("keys/def plain", tr(STR_K_NET_PLAIN_TEST));
  touch(400, 240); pump(3); release(); pump(30);    // the open row -> all closed
  // The [ ? ] tab: the content lane replaced by the page's explainer, and
  // the first-run hint stopped for good (this is the walk's first open).
  touch(720, 85); pump(3); release(); pump(45);   // 45: the fact rows land on the stagger
  save("/tmp/sim_winfo_what.ppm");
  must_show("keys/help head", tr(STR_K_HELP_HEAD));
  touch(720, 85); pump(3); release(); pump(45);     // [ ? ] again -> the rows; 45 outlasts the exit stagger
  // KEYS is two tabs on one flex strip now, spread across the 620 lane, and
  // a deck like every other tabbed page: the crossing is made by STROKE here,
  // both directions, because no other stop swipes this page. The COORDINATOR
  // WALLET tab holds PAIRING at 120 and SILENT PAYMENT at 196.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(40);                              // swipe -> COORDINATOR WALLET
  // No coordinator has ever spoken at this point -- the recv test wiped its
  // usage record on the way out -- so the tab shows the 5c empty state: the
  // absence named, what pairing gives, and the row that fills it.
  save("/tmp/sim_winfo_coord_empty.ppm");
  must_show("coord empty", tr(STR_K_COORD_NONE));
  // And past the deck's end: the stroke opens [ ? ] on this page too, and a
  // right stroke on it lands back on the tab it left.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("keys/swipe past coord opens help", tr(STR_K_HELP_HEAD));
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("keys/swipe closes help", tr(STR_K_COORD_NONE));
  {
    // Give this signer a coordinator's word -- the same store RECEIVE's lamp
    // reads -- and bounce the tab so the populated page renders.
    uint8_t cfp[4];
    kiss_ui_last_fp(cfp);
    kiss_usage_chain_set(cfp, kiss_testnet() ? 1 : 0, kiss_script(), -1, 1);
  }
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(40);                              // swipe back -> THIS SIGNER
  must_show("keys/swipe back to this signer", tr(STR_I_SEC_FIRST));
  tap_str(STR_D_ONLINE_APP, 3, 40);                 // COORDINATOR WALLET, populated
  save("/tmp/sim_winfo_coord.ppm");
  touch(400, 150); pump(3); release(); pump(6);     // PAIRING -> PAIR COORDINATOR
  save("/tmp/sim_pair.ppm");                        // descriptor (Sparrow) active
  touch(198, 228); pump(3); release(); pump(6);     // descriptor QR -> zoom
  save("/tmp/sim_pair_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
  touch(672, 150); pump(3); release(); pump(4);     // MOBILE / BlueWallet segment
  save("/tmp/sim_pair_bw.ppm");
  // The page's own [ ? 2 ], where the section chip beside SHOW TO used to be:
  // DESCRIPTOR and FINGERPRINT, the two words this page is about, as rows
  // that open. Pinned by its RIGHT edge to 752 on the chrome strip.
  touch(720, 85); pump(3); release(); pump(30);
  save("/tmp/sim_pair_help.ppm");
  // By the VALUE: DESCRIPTOR is also the KEYS explainer's third caption
  // now, and a needle two keys share passes on whichever shows either.
  must_show("pair/terms", tr(STR_T_WATCH_VAL));
  // DESCRIPTOR's page two: the artefact itself, in blocks. Its row is the
  // first, so it centres on 170 at n=2, and opening it puts MORE in the band.
  touch(400, 170); pump(3); release(); pump(30);
  tap_str(STR_H_MORE, 3, 25);
  save("/tmp/sim_pair_desc2.ppm");                  // the whole descriptor
  must_show("pair/descriptor page two", tr(STR_T_WATCH_P2_HEAD));
  tap_str(STR_C_BACK, 3, 25);                       // BACK -> the term list
  tap_str(STR_C_BACK, 3, 20);                       // BACK -> the QR page
  // Page two: the import steps plus the address proof. It is the page the
  // owner actually follows, so it gets walked and rendered like any other.
  tap_str(STR_R_NEXT, 3, 6);     // NEXT -> HOW TO PAIR
  save("/tmp/sim_pair_steps.ppm");
  tap_str(STR_C_BACK, 3, 6);     // BACK -> the QR page
  // BACK lands on the tab it left, which is what the context remembers for.
  tap_str(STR_C_BACK, 3, 6);     // BACK -> KEYS, still on COORDINATOR
  touch(400, 215); pump(3); release(); pump(6);     // SILENT PAYMENT -> consent warning
  save("/tmp/sim_sp_warn.ppm");
  tap_str(STR_W_HOLD_SHOW, 25, 6);    // a press with no travel: key stays hidden
    save("/tmp/sim_sp_warn_early.ppm"); // key stays hidden; warning unchanged
  // The failure branch first, because it is one hold away and nothing else in
  // the walk can reach it: kiss_info.c only draws the QR when the export
  // succeeds, so a derivation that fails renders a different screen that had
  // never been photographed in any locale.
  s_sim_sp_export_fail = 1;
  slide_fire(STR_W_HOLD_SHOW);      // the full slide -> export refuses
  save("/tmp/sim_sp_key_fail.ppm");                 // no QR: the refusal render
  s_sim_sp_export_fail = 0;
  // DONE, not BACK: the refusal render carries the same exit the success one
  // does. Then take the row again for the working export below.
  tap_str(STR_C_DONE, 3, 6);     // DONE -> WALLET
  touch(400, 215); pump(3); release(); pump(6);     // SILENT PAYMENT -> consent
  slide_fire(STR_W_HOLD_SHOW);      // the full slide -> export
  save("/tmp/sim_sp_key.ppm");
  touch(198, 228); pump(3); release(); pump(6);     // private scan-key QR -> zoom
  save("/tmp/sim_sp_key_zoom.ppm");
  touch(763, 35); pump(3); release(); pump(6);      // close zoom
  tap_str(STR_C_DONE, 3, 6);     // DONE -> WALLET screen
  tap_str(STR_C_BACK, 3, 6);     // BACK -> section home
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> section home
  touch(680, 430); pump(3); release(); pump(6);     // BACK -> home
  save("/tmp/sim_home_end.ppm");
  kiss_usage_wipe();             // the coordinator's word was the walk's, not the owner's

  // step 5: Sign via SD — the tabbed page, file list, verify, hold, signed
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> SIGN page
  save("/tmp/sim_sign_choose.ppm");                 // SCAN QR tab, the airgap
  must_show("sign page, scan tab", tr(STR_S_POINT_CAM));
  // The [ ? ] tab. The explainer swaps the lane, so the way back is the mark
  // itself -- or the selected tab, both walked here.
  touch(720, 85); pump(3); release(); pump(45);     // [ ? ] -> signing explainer; 45 lands the stagger
  save("/tmp/sim_sign_help.ppm");
  must_show("sign [ ? ]", tr(STR_S_HELP_HEAD));
  touch(720, 85); pump(3); release(); pump(45);     // [ ? ] again -> the tab; 45 outlasts the exit stagger
  must_show("sign [ ? ] closed", tr(STR_S_POINT_CAM));
  // The page's open flipped the first-run flag for the whole device; put it
  // back so the downstream KEYS stop stays the canonical first-run frame.
  wt_help_seen_set(false);
  tap_str(STR_S_FROM_SD, 3, 30);                    // SD CARD tab -> the list
  save("/tmp/sim_sign_files.ppm");
  // SWIPED, with a finger: the eight file fixture overflows one page of
  // three, and a horizontal stroke is now the ONLY way between pages, so the
  // walk's swipe is the regression test for the gesture wiring itself. The
  // stroke deliberately STARTS ON A ROW: crossing the 50px gesture limit has
  // to flip the page and must NOT also open the file the finger touched
  // first -- the frames differing (check_sim_taps.py) and the page-2 needle
  // prove the flip, and landing on a list rather than a verify screen proves
  // the click was swallowed.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(30);                              // swipe left -> page 2
  save("/tmp/sim_sign_files_p2.ppm");               // rows the fold used to hide
  must_show("file list page 2", "warn-COMBO.psbt");
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(30);                              // swipe right -> page 1
  must_show("file list page 1", "payment-01.psbt");
  // Page 1 is the deck's left edge inside this tab: one more stroke right
  // crosses the tab boundary onto SCAN QR, and the tab strip follows.
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(40);
  must_show("sign/deck crossed to scan tab", tr(STR_S_POINT_CAM));
  tap_str(STR_S_FROM_SD, 3, 30);      // back in by the strip, page 1 fresh
  must_show("sign/deck return to list", "payment-01.psbt");
  // Row press feedback (wt_line_press). The device has no haptics, so a
  // press is answered optically or not at all, and "not at all" is the kind
  // of thing a refactor takes away in silence. This is the walk's ordinary
  // first-file tap, photographed twice on the way through: the pressed rail
  // exists ONLY while the finger is down, so if it stops rendering the two
  // frames become identical and check_sim_taps.py fails.
  // 19 pumps held is ~304ms, deliberately under LVGL's 400ms long-press.
  touch(328, 150); pump(5);                        // first file, held
  save("/tmp/sim_sign_row_pressed.ppm");           // press answered
  pump(14);
  save("/tmp/sim_sign_row_held.ppm");              // still held, settled
  release(); pump(8);                              // -> verify (READY)
  save("/tmp/sim_sign_verify.ppm");                 // FOLDED address + SHOW FULL
  // The fold, opened and closed. Both states are a screen an owner signs from,
  // so both have to be photographed in all 21 locales -- the folded one is a
  // mono23 line a longer testnet prefix can push at the panel edge, and the
  // full one is the render that used to be the only one there was.
  //
  // The total is the unit switch. Tap it, and every amount on the screen --
  // the big total, the pair under it, and every strand label on the graph --
  // has to move to BTC together; tap it back and the frame must be the one
  // photographed before. A wallet that shows two units and switches only some
  // of them is worse than one that shows a single unit.
  touch(120, 130); pump(3); release(); pump(10);
  save("/tmp/sim_sign_btc.ppm");            // the same transaction, in BTC
  must_show("btc unit", "BTC");
  must_show("btc unit", "0.00060000");   // the hero is what the recipient gets
  // A STRAND label, not the total: every figure on the device is the switch,
  // so the one at the end of the recipient strand has to work too.
  touch(505, 185); pump(3); release(); pump(10);
  must_show("sats unit", "60 000");

  // NO COINS "?" any more. It defined "input" -- a glossary term, and the
  // glossary is one tap behind DETAILS with the other seven. Three "?" in the
  // content of the screen that matters most is what the bench was reading as
  // overwhelming.

  // No tap and no second frame: the whole address is on the glass from the
  // moment the screen builds. It used to arrive folded to eight characters
  // behind a control called FULL ADDRESS, so what an owner compared depended
  // on whether they found a toggle that read like a heading.
  //
  // The verify screen shows the FOLD, the same one RECEIVE draws: prefix, the
  // four after it, an ellipsis, then the last twelve in three blocks with the
  // final eight lit. The needle is the WHOLE fold, spans joined, so it pins
  // which characters are dropped and not merely that something was. The WHOLE address is one tap
  // away on the card below, and that card is where the full grouped form is
  // asserted -- so the two needles together pin both renderings and which
  // screen each belongs to.
  must_show("verify/address", "bc1q zyg3  \xE2\x80\xA6  g3zy g3h8 ffkz");
  // The output ROW is the control now, at every recipient count -- the card
  // below the graph is gone and every destination rides its own strand. The
  // first output row sits at the top of the graph's output lane, which starts
  // at x = 24 + BL_X and runs to the margin. Tapping it opens the same card
  // the card used to: this destination across the whole lane, where a careful
  // comparison happens and a 117 character silent payment has room to be one.
  // The ADDRESS LINE of the recipient row, not its amount: the amount is the
  // unit switch on every screen of this device, so the two lines of a row are
  // two controls and each is the thing under the finger.
  touch(600, 210); pump(3); release(); pump(8);      // the address line
  save("/tmp/sim_sign_addr_mid.ppm");
  pump(30);                                          // let the stagger settle
  save("/tmp/sim_sign_addr.ppm");
  must_show("address card/title", tr(STR_R_VT));
  must_show("address card/addr", "bc1q zyg3 zyg3 zyg3 zyg3 zyg3 zyg3 zyg3 zyg3 h8ffkz");   // every character, on the card
  must_show("address card/cmp", tr(STR_S_CMP_8));
  tap_str(STR_C_OK, 3, 8);     // OK closes the card

  // NO RBF CHIP AND NO META ROW on this screen. The pair below the graph read
  // "TESTNET, practice coins  ·  REPLACEABL..." -- ellipsised in English at
  // HEAD -- and carried a third "?" of its own. RBF is a flag row on
  // DETAILS > TRANSACTION with the same card behind it, which the deck leg
  // below already walks; the network is a badge on the hero row now, beside
  // the amount whose meaning it changes.

  tap_str(STR_S_DETAILS, 3, 30);    // DETAILS -> the deck, INPUTS tab
  save("/tmp/sim_sign_details.ppm");
  // The stroke crosses the deck STARTING OVER THE LIST: the inputs list is a
  // vertical scroller, and a scroller that ate horizontal strokes would kill
  // the swipe exactly where a finger actually lands on this page.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  if (!find_click_addr(lv_screen_active(), "bc1q")) {
    printf("FAIL: details/swipe over the inputs list did not reach OUTPUTS\n");
    g_walk_fails++;
  }
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);                              // and back to INPUTS
  // The corner [ ? ] opens TERMS -- three definition rows, not eight cells of
  // reference. A page, so it leaves by BACK and lands back on the deck, on
  // the tab it left.
  touch(720, 85); pump(3); release(); pump(30);
  save("/tmp/sim_sign_glossary.ppm");
  must_show("sign/terms", tr(STR_T_PSBT_CAP));
  // Row 1 OPEN. n=3 is a 94px closed row from WT_LANE_Y, so THE FEE centres
  // on 255 -- and the open state is the only one that shows a definition and
  // its TECHNICAL line at all.
  touch(400, 255); pump(3); release(); pump(30);
  save("/tmp/sim_sign_terms_open.ppm");
  must_show("sign/terms open", tr(STR_T_FEE_TERM));
  // PAGE TWO. THE FEE is the one term with arithmetic behind it, so its open
  // row puts MORE in the band's left lane -- the same lane that carried the
  // hint a moment ago, because a lane holding two lines holds none.
  tap_str(STR_H_MORE, 3, 20);
  save("/tmp/sim_sign_fee2.ppm");                   // size x rate = the fee
  must_show("sign/fee page two", tr(STR_T_FEE2_C3));
  tap_str(STR_C_BACK, 3, 20);                       // BACK -> the term list
  tap_str(STR_C_BACK, 3, 30);    // BACK -> DETAILS, INPUTS again
  // ...and the tab counts DOWN. Leaving with a row open is finishing with
  // it, so THE FEE is read now and the mark reads [ ? 2 ]. This is the whole
  // point of the count: a page that says how much of itself is still new.
  save("/tmp/sim_sign_terms_counted.ppm");
  if (kiss_terms_unread(KISS_TERMS_SIGN, 3) != 2) {
    printf("FAIL: reading a term did not count down: %d unread\n",
           kiss_terms_unread(KISS_TERMS_SIGN, 3));
    return 1;
  }
  // OUTPUTS: an output ROW opens the whole address. The fold drops the middle
  // of a destination somebody else chose, and the characters it drops have to
  // stay reachable from where they were dropped -- two taps from the graph to
  // every character of any output, change included.
  tap_str(STR_S_D_TAB_OUTS, 3, 50);
  save("/tmp/sim_sign_details_outs.ppm");
  {
    lv_obj_t *ao = find_click_addr(lv_screen_active(), "bc1q");
    if (!ao) {
      printf("FAIL: details row: no pressable folded address on the page\n");
      g_walk_fails++;
    } else {
      lv_area_t a;
      lv_obj_get_coords(ao, &a);
      touch((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2); pump(3); release(); pump(30);
    }
  }
  save("/tmp/sim_sign_details_addr.ppm");
  // The WHOLE grouped address, not the "bc1q zyg3" prefix the fold shares with
  // it: that prefix is on the OUTPUTS tab too, so it would pass with the card
  // never opened at all.
  must_show("details row/full addr",
            "bc1q zyg3 zyg3 zyg3 zyg3 zyg3 zyg3 zyg3 zyg3 h8ffkz");
  must_show("details row/cmp", tr(STR_S_CMP_8));
  tap_str(STR_C_OK, 3, 8);            // OK closes the card, OUTPUTS stays up
  // TRANSACTION: the facts strip, each with its own "?". The chips are found
  // by mark and order below the strip -- txid, fee, locktime, sighash, rbf.
  tap_str(STR_S_D_TAB_TX, 3, 50);
  save("/tmp/sim_sign_details_tx.ppm");
  {
    lv_obj_t *chip = det_chip(3);              // SIGHASH, the least-known term
    if (chip) {
      lv_obj_t *par = lv_obj_get_parent(chip);
      touch(lv_obj_get_x(par) + 15, lv_obj_get_y(par) + 15);
      pump(3); release(); pump(30);
    }
  }
  save("/tmp/sim_sign_term_sighash.ppm");
  tap_str(STR_C_OK, 3, 6);     // OK closes the card
  // The TXID card: it says what an owner actually wants an id FOR, so it
  // needs a stop or no gate sees the copy at all.
  {
    lv_obj_t *chip = det_chip(0);            // TXID heads the strip
    if (chip) {
      lv_obj_t *par = lv_obj_get_parent(chip);
      touch(lv_obj_get_x(par) + 15, lv_obj_get_y(par) + 15);
      pump(3); release(); pump(30);
    } else {
      printf("FAIL: details/txid: no term chip heading the strip\n");
      g_walk_fails++;
    }
  }
  save("/tmp/sim_sign_term_txid.ppm");
  // NOT the heading: S_D_TXID is printed on the page underneath too, so that
  // needle passes whichever card is open. The body exists only on the card.
  must_show("details/txid", tr(STR_S_D_TXID_SAME));
  tap_str(STR_C_OK, 3, 6);     // OK closes the card
  // And the stroke: past TRANSACTION opens the explainers, a right stroke on
  // them lands back on the tab it left -- the deck promise, kept here too.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  // By a TERM and not by the title: the page is called TERMS now and so is
  // the trail segment every [ ? ] lane wears, so the title alone would pass
  // on any of them.
  must_show("details/swipe past tx opens explainers", tr(STR_T_PSBT_CAP));
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("details/swipe back to transaction", tr(STR_S_D_TXID));
  tap_str(STR_C_BACK, 3, 6);     // BACK -> verify again
  // The accent changed while this screen was UP, which is the case the flags
  // exist for and the one no rebuild can cover: every other check in this walk
  // runs a whole locale or a whole accent from scratch, so a builder that gets
  // the colour right and a restyle that never repaints look identical.
  //
  // Three channels, three ways of failing silently. The rim is a border, the
  // strand is a line and the sweep is a fill, and a flag that only ever set a
  // text colour did nothing to any of them while appearing correctly set.
  //
  // The third channel used to be named as the hold ring, and was never checked
  // -- it could not be. That ring was built inside an opaque pill created after
  // it and had not drawn a pixel since the day it was added, so an assertion
  // here would have been reading the style of something nobody could see. The
  // sweep is on the glass, so this is the first time the comment's third
  // channel and the code's third channel are the same object.
  {
    lv_obj_t *hp = act_for(STR_S_HOLD_TO_SIGN, "accent restyle");
    lv_obj_t *ln = find_accent_line(lv_screen_active());
    if (!ln) {
      printf("FAIL: no accent-flagged strand on the verify screen\n");
      return 1;
    }
    if (hp) {
      // By its flag, not its index: the sweep is moved behind the label after
      // it is built, so which child it is depends on an ordering this check
      // has no business knowing.
      lv_obj_t *sw = NULL;
      for (uint32_t ci = 0; ci < lv_obj_get_child_count(hp); ci++) {
        lv_obj_t *c = lv_obj_get_child(hp, ci);
        if (lv_obj_has_flag(c, WT_FLAG_ACCENT_FILL)) { sw = c; break; }
      }
      if (!sw) {
        printf("FAIL: no accent-flagged sweep under HOLD TO SIGN\n");
        return 1;
      }
      // The rim went with the box: the slide is a bar and a knob now, so the
      // accent-following surfaces are the WORD (WT_FLAG_ACCENT) and the
      // knob/fill (WT_FLAG_ACCENT_FILL). Same three channels, new carriers.
      lv_obj_t *wd = NULL;
      for (uint32_t ci = 0; ci < lv_obj_get_child_count(hp); ci++) {
        lv_obj_t *c = lv_obj_get_child(hp, ci);
        if (lv_obj_has_flag(c, WT_FLAG_ACCENT)) { wd = c; break; }
      }
      if (!wd) {
        printf("FAIL: no accent-flagged word on the slide to sign\n");
        return 1;
      }
      const int was = wt_accent_get();
      lv_color_t b0 = lv_obj_get_style_text_color(wd, LV_PART_MAIN);
      lv_color_t l0 = lv_obj_get_style_line_color(ln, LV_PART_MAIN);
      lv_color_t f0 = lv_obj_get_style_bg_color(sw, LV_PART_MAIN);
      wt_accent_set(was == WT_ACC_GREEN ? WT_ACC_ORANGE : WT_ACC_GREEN);
      wt_accent_restyle(lv_screen_active());
      pump(2);
      lv_color_t b1 = lv_obj_get_style_text_color(wd, LV_PART_MAIN);
      if (lv_color_eq(b0, b1)) {
        printf("FAIL: SLIDE TO SIGN kept its old ink through an accent change\n");
        return 1;
      }
      if (!lv_color_eq(b1, wt_accent())) {
        printf("FAIL: SLIDE TO SIGN's word is not the accent after a restyle\n");
        return 1;
      }
      lv_color_t l1 = lv_obj_get_style_line_color(ln, LV_PART_MAIN);
      if (lv_color_eq(l0, l1) || !lv_color_eq(l1, wt_accent())) {
        printf("FAIL: the change strand kept its old accent through a change\n");
        return 1;
      }
      lv_color_t f1 = lv_obj_get_style_bg_color(sw, LV_PART_MAIN);
      if (lv_color_eq(f0, f1) || !lv_color_eq(f1, wt_accent())) {
        printf("FAIL: the hold sweep kept its old accent through a change\n");
        return 1;
      }
      save("/tmp/sim_sign_accent.ppm");
      wt_accent_set(was);
      wt_accent_restyle(lv_screen_active());
      pump(2);
      printf("ok: rim, strand and sweep follow the accent with no rebuild\n");
    }
  }

  // The slide, sampled three times across the track. One frame cannot show
  // a fill: it looks exactly like a strand that is simply that long. Three
  // at 25, 50 and 75 percent of the 310 travel are what make the length a
  // function of the finger, and a fill wired to anything else -- a coin
  // index, a signing estimate -- lands the same in all three.
  // Travel is the track minus the 44px knob -- 266 of 310 -- and the drags
  // below are quarters of THAT, plus the 8px deadband the knob does not move
  // through. 75% is 208 and the snap is at 85%, so the third stop is a frame
  // the gesture is still holding rather than one it has already finished.
  slide_grip(STR_S_HOLD_TO_SIGN); slide_go(74);
  save("/tmp/sim_sign_hold_q.ppm");
  slide_go(141);                                    // sweep ~half across
  save("/tmp/sim_sign_hold.ppm");
  slide_go(208);
  save("/tmp/sim_sign_hold_3q.ppm");
  if (!kiss_sign_test_locked()) {
    printf("FAIL: the output side never stood down for the hold\n");
    return 1;
  }

  // Let go early -- and NOTHING happens for 800ms. The travel is banked, the
  // strands stay forward, the padlock stays, and the label offers to carry on.
  // A lift is not a decision any more, and this is the frame that says so.
  release(); pump(8);
  save("/tmp/sim_sign_paused.ppm");
  if (!kiss_sign_test_locked()) {
    printf("FAIL: the pause window let the output side back up\n");
    return 1;
  }

  // Past the window, and past the 200ms the fill takes to run home after it:
  // 70 frames is 1120ms against 1000, and the release itself is not seen until
  // the indev's next read. 56 was 896 and sat inside the window.
  pump(70);
  save("/tmp/sim_sign_abandon.ppm");
  if (kiss_sign_test_locked()) {
    printf("FAIL: an abandoned hold left the output side locked down\n");
    return 1;
  }

  slide_grip(STR_S_HOLD_TO_SIGN); slide_go(320);    // past the track: signs
  release(); pump(40);                              // past REVEAL_TRAVEL_MS
  // The reveal, and the reason the walk stops here rather than landing straight
  // on the exit screen. The graph has spent the whole flow claiming a strand in
  // the accent means a signature exists; this is the frame where that is
  // discharged, all inputs together, because one libwally call signed all of
  // them and there was never a per coin moment to show.
  //
  // 40 frames, not 8: the strands CROSS to the accent over REVEAL_TRAVEL_MS
  // now rather than switching between two frames, so 8 caught them a third of
  // the way over and the frame this stop exists for was a colour that means
  // nothing. 520ms is 33 frames at the harness's 16ms; 40 clears it.
  save("/tmp/sim_sign_reveal.ppm");
  // 110, not 50: REVEAL_MS went 700 -> 1600 so the answer is on the glass long
  // enough to read. 1600ms is 100 frames. Landing short here does not fail
  // here -- it fails four screens later on a DONE action that is not up yet, and
  // then cascades through every BACK after it.
  pump(110);                                        // past REVEAL_MS: writes SD
  save("/tmp/sim_sign_done.ppm");
  // The chip is pinned at a fixed x now (kiss_sign.c draw_sig_chip): chip
  // first, translated caption trailing, so this tap holds in all 21 locales.
  // It moved down with everything else when the summary card took the band at
  // 120: draw_sig_chip(296, 336) puts its centre here.
  // The code itself moved INTO the panel this opens -- the next frame must
  // show it above the two example rows.
  touch(311, 351); pump(3); release(); pump(6);     // ? beside SIGNATURE -> panel
  save("/tmp/sim_sign_sigcheck.ppm");
  tap_str(STR_C_BACK, 3, 6);     // BACK -> signed screen again
  tap_str(STR_C_DONE, 3, 6);     // DONE -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> the page
  tap_str(STR_S_FROM_SD, 3, 30);                    // SD CARD tab -> the list
  touch(328, 216); pump(3); release(); pump(8);     // the STOP file -> blocked verify
  save("/tmp/sim_sign_stop.ppm");
  // BACK is ONE STEP now: from a transaction it returns to the list that
  // transaction came from, not to the home screen. The three files below are
  // opened one after another without ever leaving SIGN, which is the whole
  // point -- picking the wrong file used to cost the entire trip back in.
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> the file list
  save("/tmp/sim_sign_back_files.ppm");
  // payment-01.psbt was signed a few taps ago, so its row must now read SIGNED
  // ALREADY in amber and REMOVE SIGNED must have appeared in the action row.
  // That is the whole bug: the badge used to be computed from the row's OWN
  // name, so the source stayed grey UNSIGNED with its signature sitting right
  // beneath it, and there was no way to see what had already been done.
  // tr(), not the English: this walk runs in all 21 locales.
  must_show("file list after signing", tr(STR_S_SIGNED_ALREADY));
  must_show("file list after signing", tr(STR_S_RM_SIGNED));
  // A destination these keys have paid before, on the plain single-recipient
  // screen and nowhere else: the difference is one chip beside the caption and
  // a third entry on the card behind it. Small differences are exactly what a
  // rendered frame is for -- whether the chip fits the caption row without
  // pushing the network and RBF pair off the end, and whether the card still
  // holds three entries, are not questions a gate can ask.
  //
  // Driven by the stub flag rather than by a fixture of its own. An eighth file
  // on the fake SD moves the "showing 4 of N" line and every row tap below it.
  s_sim_payee_known = true;
  if (tap_row_prefix("payment-01")) {
    save("/tmp/sim_sign_known.ppm");
    // NOT the words: the caption row that carried them is gone with the
    // address card, and a destination these keys have paid before wears a bare
    // MARK on its strand instead -- kiss_payee.h's own rule, recognition only.
    // The words are on the card this row opens, which is asserted below.
    //
    // ...and the address is still whole and unmoved. The mark is an addition
    // to the row, never a claim that takes the destination's place.
    must_show("paid before (address)",
              "bc1q zyg3  \xE2\x80\xA6  g3zy g3h8 ffkz");
    // The recipient ROW, which is the control an owner presses now: one
    // layout at every count, and the card that used to be here is gone.
    touch(600, 210); pump(3); release(); pump(30);
    save("/tmp/sim_sign_known_why.ppm");
    // The BODY, not the whole string: WT_GRID_ICONS splits each entry at its
    // "HEAD: " and puts the two halves in separate labels, so the full string
    // is never one label's text. Same split in every locale, so this stays
    // locale independent.
    {
      const char *b = strstr(tr(STR_S_PAYEE_HELP), ": ");
      must_show("paid before, why", b ? b + 2 : tr(STR_S_PAYEE_HELP));
    }
    tap_str(STR_C_OK, 3, 6);
    tap_str(STR_C_BACK, 3, 6);   // BACK -> the file list
  }
  s_sim_payee_known = false;
  // REMOVE SIGNED is at 48..388 x 404..456; this is its centre.
  tap_str(STR_S_RM_SIGNED, 3, 8);
  save("/tmp/sim_sign_rm_list.ppm");                // one row per signed file
  // Row 0's own slide rule: rows start at y=114, 76 tall, control at local
  // (520,18) 170x40, so 568..738 x 132..172. A tap is NOT enough.
  touch(652, 164); pump(2); release(); pump(4);
  save("/tmp/sim_sign_rm_noop.ppm");                // still the list, nothing gone
  slide_at(652, 164, 85);                           // half the 170: partial red fill
  lv_refr_now(NULL);
  save("/tmp/sim_sign_rm_holding.ppm");
  release(); pump(6);                               // let go early -> nothing happened
  save("/tmp/sim_sign_rm_letgo.ppm");               // still the list, unchanged
  tap_str(STR_C_BACK, 3, 8);     // BACK, keep the fixtures
  save("/tmp/sim_sign_rm_back.ppm");                // back to the file list
  touch(328, 282); pump(3); release(); pump(8);     // the FEE file -> amber caution
  save("/tmp/sim_sign_fee.ppm");                    // summary + "I UNDERSTAND" gate
  // I UNDERSTAND moved out of the action row and into the caution row itself,
  // which is the point of the redraw: the answer sits beside the thing being
  // read, and HOLD TO SIGN keeps its coordinates in both states. The action is
  // local (543,8) 170x40 inside a row pinned at (24, SG_PANEL_Y), so row 0's is
  // 567..737 x 158..198. This is its centre. It was still tapping the old
  // action-row position at (364,430), which the redraw deleted.
  // The bar dropped to y=344 under the bundle graph, and its action went with it:
  // bar-relative (543,2) 170x40 is now 567..737 x 346..386. This is its centre.
  // The caution SENTENCE is a link into the word it is about: a high fee
  // lands on THE FEE, already open. wt_note_fit sizes the label to its TEXT,
  // so the target is the words themselves at x=110 -- not the lane they sit
  // in -- and well clear of the ack control at 652.
  touch(110, 312); pump(3); release(); pump(30);
  save("/tmp/sim_sign_caution_term.ppm");           // THE FEE, open on arrival
  must_show("sign/caution links to its term", tr(STR_T_FEE_TERM));
  tap_str(STR_C_BACK, 3, 30);                       // BACK -> the verify screen
  // 312, not 366: the caution bar moved up to 290..334 when the slide grew
  // the action band to 344. All three taps on it are this same centre.
  touch(652, 312); pump(3); release(); pump(6);     // I UNDERSTAND -> row goes green
  save("/tmp/sim_sign_fee_ack.ppm");
  // BACK out of a screen an acknowledgement repainted. The tap above is what
  // makes the orphaned-screen check at the end of this walk mean anything: the
  // repaint is the only thing in the app that ever replaced a live screen
  // without deleting it, so if the ack action is not actually hit, nothing counts
  // an orphan and the check passes on a build that leaks.
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> the file list
  // warn-COMBO leads page two now: nine files, three a page.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(30);                              // swipe left -> page 2
  touch(328, 150); pump(3); release(); pump(8);     // COMBO file -> stacked cautions
  save("/tmp/sim_sign_combo.ppm");
  // THE regression. Five cautions used to replace the output panels outright,
  // so the destination vanished from the transactions the device trusted least
  // -- and a coordinator could induce exactly that by padding the input count
  // or leaving the previous transactions off. The frame above proves it looks
  // right; these three prove the facts are actually on it.
  //
  // Two assertions where there was one, because the address is FOLDED now and
  // the guarantee has two halves: something address shaped is on the glass
  // without a tap, and the whole of it is one tap away. Checking only the
  // second would pass on a screen that folded the address to nothing.
  //
  // The amounts lost their " sats" when they became strand labels: the unit is
  // stated once, on the hero, and repeating it on every row of a graph whose
  // rows are all the same unit is the restatement the copy rules cut. The
  // guarantee is unchanged -- these two numbers are on the glass without a tap.
  must_show("verify (5 cautions)", "bc1q");           // folded, prefix span
  // The fee is on the first page; the change is not. A cautioned screen has a
  // shorter band and the rows are taller than they were, so this transaction
  // pages -- and the read-to-the-end gate is what guarantees the change row
  // reaches the glass, which is a stronger promise than "it is on the first
  // screenful". The end of the list is walked below.
  must_show("verify (5 cautions)", "800");            // the fee
  must_show("verify (5 cautions, address)",
            "bc1q zyg3  \xE2\x80\xA6  g3zy g3h8 ffkz");
  // The input TOTAL, and it is a control. Above one coin the graph draws a
  // breakdown, so the sum goes on the caption line: 4 000 in, against 3 000 and
  // 800 and 200 out, is the only arithmetic that says whether the fee is the
  // fee, and it used to be a DETAILS tap away on the one screen that exists to
  // catch a transaction lying about itself. Tapped, it flips the unit like
  // every other figure on the device -- which is also what proves it landed
  // somewhere a finger reaches, since its x comes off the caption's rendered
  // width and the chip past it rather than a number written here.
  must_show("verify (5 cautions)", "4 000");
  tap_label_exact("4 000");
  must_show("input total is the unit switch", "0.00004000");
  tap_label_exact("0.00004000");
  must_show("input total switches back", "4 000");
  // Five cautions and the recipient address on the SAME screen. This frame is
  // the regression: the address panel used to be replaced by the row stack, so
  // the transaction the device trusted least was the one whose destination it
  // never showed. The bar action keeps the row action's x, so the FEE ack tap above
  // and this REVIEW tap land in the same place.
  touch(652, 312); pump(3); release(); pump(8);     // REVIEW -> the rows, own page
  save("/tmp/sim_sign_cautions.ppm");
  // Row 0's I UNDERSTAND: rows start at y=88 with the action at local (543,8),
  // so it is 567..737 x 96..136. This is its centre.
  touch(652, 116); pump(3); release(); pump(8);     // -> row goes green, page repaints
  save("/tmp/sim_sign_cautions_ack.ppm");
  tap_str(STR_C_BACK, 3, 8);     // BACK -> verify, address still there
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
  tap_str(STR_C_OK, 3, 6);     // OK closes the card
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> the list, page 2 kept
  save("/tmp/sim_sign_back_choose.ppm");            // the page, still on SD
  tap_str(STR_C_BACK, 3, 6);     // BACK -> home (the page IS the chooser now)
  // silent payment send: out0 renders as a tsp1 address with the SP badge+note.
  // the list shows only the first 4 files, so clear the others (all frames above
  // are already saved) to leave zsp-SPAY in row 0 and zzz-UNPRV in row 1.
  sd_unlink("payment-01.psbt");   sd_unlink("payment-01-signed.psbt");
  sd_unlink("risky-STOP.psbt");
  sd_unlink("silly-FEE.psbt");    sd_unlink("silly-FEE-signed.psbt");
  sd_unlink("warn-COMBO.psbt");
  touch(130, 240); pump(3); release(); pump(6);     // Sign again -> the page
  tap_str(STR_S_FROM_SD, 3, 30);                    // SD CARD tab -> the list
  touch(328, 150); pump(3); release(); pump(8);     // zsp-SPAY (row 0) -> SP verify
  save("/tmp/sim_sign_sp.ppm");                      // SP output: the on-chain note
                                                      // moved to DETAILS, so the
                                                      // column fits and needs no scroll
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> the file list
  // The unproven-amount REFUSAL: a two-input spend from a coordinator that
  // ships bare witness_utxos and nothing else. It is the only STOP an honest
  // coordinator can trip, so unlike every other refusal on this device the
  // panel has to leave the owner somewhere to go -- which is a property of a
  // sentence, and only a rendered frame can say whether it fits the panel.
  touch(328, 216); pump(3); release(); pump(8);     // zzz-UNPRV (row 1) -> STOP
  save("/tmp/sim_sign_unproven.ppm");
  // The refusal text itself, not a fragment of it: this panel is the whole
  // screen, so if the mapping in tr_reason ever falls back to the raw English
  // reason the owner loses the remedy and nothing else on screen would say so.
  // The VERDICT, which is one plain label. The remedy under it goes through
  // wt_why_body, which lays a body out as ruled blocks and does not leave the
  // whole string in any single label -- so the sentence is a thing for the
  // frame to check, and this needle checks the mapping instead: raw English
  // here would read "input amounts not proven", not this.
  must_show("verify (unproven STOP)", tr(STR_S_C_UNPROVEN));
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> the file list

  // Five recipients: more than the panel shows at once, and the only shape on
  // this card that reaches the read-to-the-end gate. HOLD TO SIGN starts inert
  // -- the same "present, in place, visibly inert" state an unacknowledged
  // caution puts it in -- and lights when the list has been read to its end.
  //
  // Before the gate, a destination below the fold was present, scrollable and
  // never looked at while HOLD TO SIGN was live the whole time. The 16-output
  // cap bounds how much can hide, not whether it can, and TOTAL LEAVING sums
  // destinations without naming them.
  touch(328, 282); pump(3); release(); pump(8);     // zzzz-MANY (row 2) -> verify
  save("/tmp/sim_sign_many.ppm");                   // 5 recipients, HOLD inert
  must_show("many recipients", "bc1q 00g3  \xE2\x80\xA6  g3zy g3h8 ffkz");
  if (kiss_sign_test_armed()) {
    printf("FAIL: HOLD TO SIGN was live with recipients still under the fold\n");
    return 1;
  }
  // PAGE to the end. The column does not scroll any more: a scroller comes to
  // rest wherever the finger leaves it, so the row at the fold was always
  // sliced through its own address. A page hides the rows that are not on it,
  // so the page IS the window and a whole row is the only thing that can be on
  // the glass. A leftward stroke turns it, and the strands are redrawn every
  // time -- the ones whose rows are off this page fan out dimmed, so the shape
  // of the transaction never leaves while its detail is read.
  for (int f = 0; f < 8 && !kiss_sign_test_armed(); f++) {
    for (int i = 0; i <= 8; i++) { touch(600 - i * 14, 250); pump(3); }
    release(); pump(40);
  }
  save("/tmp/sim_sign_many_end.ppm");               // last recipient, HOLD live
  must_show("many recipients (end)", "bc1q 04g3  \xE2\x80\xA6  g3zy g3h8 ffkz");
  if (!kiss_sign_test_armed()) {
    printf("FAIL: HOLD TO SIGN still inert after the list was read to its end\n");
    return 1;
  }
  printf("ok: signing waits until every recipient has been on the glass\n");
  tap_str(STR_C_BACK, 3, 6);     // BACK -> the file list

  // Twenty coins swept into one address. The graph draws five rows whatever the
  // count -- first two, the elided middle, last two -- and the middle strand is
  // the only dashed line on the device, so this frame is the only place that
  // renderer path is ever looked at. Row 3, the last of the four the list shows.
  //
  // The two numbers on the group row are the whole reason it is not just a
  // thinner line: sixteen coins is a count nothing else on the screen states,
  // and their total is what stops "16 more" reading as loose change. Both come
  // from the summary, because ins[] holds sixteen of the twenty.
  // zzzzz-MERGE is alone on page two of the four remaining files.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(30);                              // swipe left -> page 2
  touch(328, 150); pump(3); release(); pump(8);     // zzzzz-MERGE -> verify
  save("/tmp/sim_sign_merge.ppm");
  // Digits only. Every other needle here would be a translated word, and
  // wt_fmt_sats groups with the same space in all 21 locales, so these two read
  // identically everywhere: the count of coins the middle strand stands for,
  // and the value they carry. The total is the stronger of the two -- it can
  // only be right if it came from the summary rather than from the four rows
  // the graph can see.
  must_show("twenty coins", "16");                  // the elided count
  must_show("twenty coins", "2 749 257");           // ... and what it is worth
  // The card behind the "?", on the one transaction where the two numbers
  // differ: twenty coins, fourteen addresses. Six of those coins share an
  // address with another, and were joined the day it was handed out twice --
  // this transaction reveals nothing new about them, so the caution does not
  // count them. A card saying "20" here would be inflating the loss it is
  // asking the owner to accept.
  touch(753, 123); pump(3); release(); pump(30);    // "?" -> WHY FLAGGED
  save("/tmp/sim_sign_merge_why.ppm");
  must_show("twenty coins, why", "14");
  tap_str(STR_C_OK, 3, 6);
  if (kiss_sign_test_armed()) {
    printf("FAIL: HOLD TO SIGN was live with the coins-linked bar unacknowledged\n");
    return 1;
  }
  touch(652, 312); pump(3); release(); pump(8);     // I UNDERSTAND -> bar goes green
  save("/tmp/sim_sign_merge_ack.ppm");
  // Then PAGE the output column to its end, because on this transaction
  // whether that is even necessary depends on the locale: "no change, this
  // empties all 20" is one line in English and two in Czech, which is enough
  // to give the column a second page. That is the gate behaving correctly --
  // anything not on this page must be paged to -- so the walk strokes
  // unconditionally rather than asserting a state only English reaches. On a
  // single page graph the stroke is ignored and these are a no-op.
  for (int f = 0; f < 8 && !kiss_sign_test_armed(); f++) {
    for (int i = 0; i <= 8; i++) { touch(600 - i * 14, 250); pump(3); }
    release(); pump(40);
  }
  if (!kiss_sign_test_armed()) {
    printf("FAIL: HOLD TO SIGN still inert after the bar was acked and the "
           "outputs read to their end\n");
    return 1;
  }
  printf("ok: twenty coins elide to five rows, count and total both stated\n");
  // The hold on the one transaction that has a grouped strand. Two paths meet
  // here and nowhere else: the flat two point line, which is how a strand
  // sitting ON the junction row is written, and the dash, which is the only
  // dashed line on the device. The accent drawn over it has to be dashed too --
  // sixteen coins committing must not become one coin committing halfway
  // through a hold.
  slide_grip(STR_S_HOLD_TO_SIGN); slide_go(155);
  save("/tmp/sim_sign_merge_hold.ppm");
  // 78 frames, not 8: a lift short of the end banks the travel for 800ms and
  // the fill takes 200 more to run home, so an abandon is only an abandon
  // once both have passed.
  release(); pump(78);
  if (kiss_sign_test_locked()) {
    printf("FAIL: an abandoned hold left twenty coins locked down\n");
    return 1;
  }
  printf("ok: the grouped strand fills and retracts still dashed\n");
  // The only fixture whose input count exceeds what wpsbt_details_t can hold,
  // so it is the only one that reaches S_D_MANYIN_FMT -- the longest formatted
  // line in the sign flow, ~140 bytes in ja, and the case that used to truncate.
  // The details stub lost that coverage when it stopped inventing a count the
  // summary disagreed with; this is where it comes back, on a transaction that
  // genuinely has more inputs than the page can list.
  tap_str(STR_S_DETAILS, 3, 8);
  save("/tmp/sim_sign_merge_details.ppm");
  must_show("twenty coins, details", "20");   // the real count
  must_show("twenty coins, details", "16");   // ... and how many are listed
  tap_str(STR_C_BACK, 3, 8);     // BACK -> verify

  // SIGN FAILED, on the one screen that can honestly produce it: a completed
  // hold whose signing call returns nothing. The graph is left claiming no
  // signature, because none was made. Nothing else in the walk opens this
  // screen -- it was built, translated 21 times and never rendered.
  s_sim_sign_fails = 1;
  slide_grip(STR_S_HOLD_TO_SIGN); slide_go(320);    // past the track: signs, or does not
  release(); pump(10);
  save("/tmp/sim_sign_failed.ppm");
  must_show("sign failed", tr(STR_S_FAIL_SIGN));
  tap_str(STR_C_BACK, 3, 8);     // BACK -> home, the only way off a failure

  // The RECEIPT, on the only transaction whose receipt could be wrong: five
  // recipients under a 63 byte filename. Nothing had ever signed a multi output
  // fixture, so the card had only ever been photographed in the shape where it
  // happens to be right.
  //
  // Two faults met on this frame. RECIPIENT GETS carries the total across every
  // destination, and the line under it printed the first address it found and
  // stopped -- so the page an owner keeps stated, as a fact, that the whole
  // amount went to an address that got a fifth of it. And the filename was laid
  // out with no width and no long mode at font28, which at 63 bytes is about
  // 900px on an 800px panel: it ran off both edges, taking the first and last
  // characters with it, which are the two an eye uses to match a name.
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> the page
  tap_str(STR_S_FROM_SD, 3, 30);                    // SD CARD tab -> the list
  if (tap_row_prefix("zzzz-MANY")) {
    // The read-to-the-end gate is still in force, so the column has to be
    // PAGED to its end before SLIDE TO SIGN is live. Same strokes as the
    // first visit.
    for (int f = 0; f < 8 && !kiss_sign_test_armed(); f++) {
      for (int i = 0; i <= 8; i++) { touch(600 - i * 14, 250); pump(3); }
      release(); pump(40);
    }
    slide_grip(STR_S_HOLD_TO_SIGN); slide_go(320);
    release(); pump(150);                           // past the reveal, writes SD
    save("/tmp/sim_sign_done_many.ppm");
    // What the card must say, and what it must not. The count comes from the
    // string DETAILS already uses for it, so this needle is the translated one
    // in every locale; the address is the fold the old code drew, and its
    // absence is the whole fix.
    {
      char want[80];
      snprintf(want, sizeof want, tr(STR_S_D_OUTPUTS_FMT), 6u, 1u);
      must_show("receipt/recipient count", want);
    }
    must_not_show("receipt/no first address", "bc1q 00g3");
    // The filename, inside the panel. LV_LABEL_LONG_DOT keeps the label's TEXT
    // whole, so a needle would pass on a label hanging off both edges -- the
    // rendered box is the only thing that can answer this.
    {
      lv_obj_t *fn = find_label_prefix(lv_screen_active(), "zzzz-MANY");
      if (!fn) {
        printf("FAIL: receipt: the signed filename is not on the screen\n");
        g_walk_fails++;
      } else {
        lv_area_t a;
        lv_obj_get_coords(fn, &a);
        if (a.x1 < 0 || a.x2 > 799) {
          printf("FAIL: receipt: filename runs %d..%d, off an 800px panel\n",
                 a.x1, a.x2);
          g_walk_fails++;
        }
      }
    }
    tap_str(STR_C_DONE, 3, 6);     // DONE -> home
    // /tmp/simsd outlives the process and sim_fixture_reset only WRITES the
    // fixtures, so a signature left here is an extra row in the file list on
    // the next run -- and every tap in the sign walk that goes by position
    // lands one row off. The walk's own REMOVE step sweeps the earlier
    // signature; this one is made after it, so it clears up after itself.
    {
      char signed_out[384];
      snprintf(signed_out, sizeof signed_out, "%s/%s", SIMSD,
               "zzzz-MANY-recipients-export-from-the-coordinator-ap-signed.psbt");
      unlink(signed_out);
    }
  }

  // The SPARSE list: two files, so no pager -- the rows keep their pitch and
  // the sort hint takes the count line's slot, the only branch with the room
  // to say it. Nothing after this leg reads the two files removed here.
  sd_unlink("zzz-UNPRV.psbt");
  sd_unlink("zzzzz-MERGE.psbt");
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> the page
  tap_str(STR_S_FROM_SD, 3, 30);                    // SD CARD tab -> the list
  save("/tmp/sim_sign_files_few.ppm");              // 2 rows + the hint line
  must_show("sparse file list", tr(STR_S_FILES_HINT));
  tap_str(STR_C_BACK, 3, 6);     // BACK -> home (the page IS the chooser)

  // step 6: Sign via QR — scan (real UR fountain parts injected as if the
  // camera decoded them), verify, sign, animated UR out
  touch(130, 240); pump(3); release(); pump(6);     // Sign tile -> the page
  tap_str(STR_S_SCAN_QR, 3, 30);                    // SCAN QR tab (page was on SD)
  tap_str(STR_S_OPEN_CAM, 3, 6);                    // OPEN CAMERA -> scan screen
  save("/tmp/sim_qr_scan.ppm");

  // A pMofN set too large for this device, refused at the first part. The
  // old parser measured only the NUMBER of parts, so a set like this was
  // accepted a QR at a time -- tens of KB of heap taken while the camera
  // streams -- and refused at assemble time, leaving the counter sitting on
  // screen with nothing saying why. One oversize part is now enough.
  {
    char big[7000];
    memset(big, 'A', sizeof big);
    memcpy(big, "p1of4 ", 6);
    big[sizeof big - 1] = 0;
    kiss_scan_inject(big, strlen(big));
    pump(6);
    save("/tmp/sim_qr_too_big.ppm");                // refusal + the way through
    // "too big" used to be G_FW_BIG_H as well -- the firmware oversize heading --
    // so this needle passed on either screen. That one says "file too big" now,
    // which is both unambiguous and more useful: one is a QR payload, the other
    // is a file on a card.
    must_show("scan/too-big", tr(STR_N_TOO_BIG));
  }

  // The same refusal on the UR path, which is the format a coordinator
  // actually animates. It had NO size check: every frame accepted, the counter
  // walking to 100%, and the failure surfacing only after assembly -- where
  // the scan screen backed out, which on glass is indistinguishable from
  // tapping cancel. No save: this is the screen photographed above, reached
  // from the other format, so a second stop would be a copy of that frame and
  // the taps gate would rightly call it a dead interaction.
  {
    uint8_t *big = malloc(QRT_MAX_SIGNED_PSBT);
    if (big) {
      memset(big, 0xA5, QRT_MAX_SIGNED_PSBT);
      memcpy(big, "psbt\xff", 5);
      qrt_encoder_t *enc = qrt_encoder_new(QRT_FMT_UR, big, QRT_MAX_SIGNED_PSBT);
      char part[600];
      if (enc && qrt_encoder_next(enc, part, sizeof part) == 0) {
        kiss_scan_inject(part, strlen(part));
        pump(6);
      }
      must_show("scan/ur-too-big", tr(STR_N_TOO_BIG));
      qrt_encoder_free(enc);
      free(big);
    }
  }

  // And the narrow band the feed-time check cannot see: the declared length is
  // the CBOR wrapper, so a payload a few bytes over the cap fits under the
  // head allowance and assembles. It has to arrive at the same sentence, and
  // the scanner has to still be open behind it -- backing out here is the bug.
  {
    const size_t slack_len = (size_t)QRT_MAX_PSBT + 4;
    uint8_t *slack = malloc(slack_len);
    if (slack) {
      memset(slack, 0xA5, slack_len);
      memcpy(slack, "psbt\xff", 5);
      qrt_encoder_t *enc = qrt_encoder_new(QRT_FMT_UR, slack, slack_len);
      char part[600];
      // Exactly the pure fragments and not one more. The refusal resets the
      // parser so the owner can point the camera somewhere else, so a loop
      // that keeps injecting starts the same set over and paints its progress
      // back over the message -- which is what the first version of this stop
      // measured, and it read as the fix not working.
      const int n = qrt_encoder_parts(enc);
      for (int i = 0; i < n; i++) {
        if (qrt_encoder_next(enc, part, sizeof part) != 0) break;
        kiss_scan_inject(part, strlen(part));
        pump(1);
      }
      must_show("scan/ur-slack-too-big", tr(STR_N_TOO_BIG));
      if (!kiss_scan_active()) {
        printf("FAIL: an assembled oversize UR closed the scanner instead of "
               "saying so\n");
        g_walk_fails++;
      }
      qrt_encoder_free(enc);
      free(slack);
    }
  }

  {
    uint8_t fake[300];
    memset(fake, 0x5A, sizeof fake);
    memcpy(fake, "psbt\xff", 5);
    qrt_encoder_t *enc = qrt_encoder_new(QRT_FMT_UR, fake, sizeof fake);
    char part[600];
    if (enc && qrt_encoder_next(enc, part, sizeof part) == 0) {
      kiss_scan_inject(part, strlen(part));       // one part in: progress shows
      pump(3);
    }
    save("/tmp/sim_qr_scan_part.ppm");
    for (int i = 0; i < 32 && kiss_scan_active(); i++) {
      if (enc && qrt_encoder_next(enc, part, sizeof part) == 0)
        kiss_scan_inject(part, strlen(part));
      pump(2);
    }
    qrt_encoder_free(enc);
  }
  pump(8);
  save("/tmp/sim_qr_verify.ppm");                   // verify screen, source = scan
  slide_grip(STR_S_HOLD_TO_SIGN); slide_go(320);    // slide to sign
  // ...then past the reveal as well: the QR path takes a different exit but
  // shares the signing state, so it waits the same beat before leaving.
  // 45 + 120: the hold completes inside the first, and the second has to clear
  // REVEAL_MS (1600ms, 100 frames) before the QR screen exists at all. It used
  // to be 58, which cleared the old 700ms with room to spare and now lands on
  // the reveal -- where BOTH of these saves would photograph the same graph and
  // the taps gate would call them a dead interaction, which is exactly what it
  // did.
  pump(45); release(); pump(120);
  save("/tmp/sim_qr_out1.ppm");                     // animated UR out, first part
  pump(20);                                         // ~320ms: 250ms timer advanced
  save("/tmp/sim_qr_out2.ppm");                     // ...a different part
  touch(206, 258); pump(3); release(); pump(20);    // animated signed QR -> zoom
  save("/tmp/sim_qr_out_zoom.ppm");                 // animation keeps moving enlarged
  touch(763, 35); pump(3); release(); pump(6);      // close on latest frame
  touch(530, 270); pump(3); release(); pump(6);     // EASY SCAN: sparser, slower QR
  save("/tmp/sim_qr_out_ez.ppm");
  tap_str(STR_C_DONE, 3, 6);     // DONE -> home
  save("/tmp/sim_qr_end.ppm");

  // The ownership refusal, both of its shapes. Delivered by QR and not by a
  // file on purpose: the whole sign walk taps the SD list by COORDINATE, so
  // two more fixtures on the card would move every row after them and the
  // gate would report a dozen screens as changed when nothing about them was.
  //
  // This is the STOP an owner standing in the wrong keys arrives at, and until
  // now it was a verdict with no next step and no figures. It carries both
  // fingerprints now, so the walk has to see them: that they DIFFER on one
  // screen and that only one is drawn on the other is the whole point, and a
  // frame is the only thing that can say so.
  for (int shape = 0; shape < 2; shape++) {
    touch(130, 240); pump(3); release(); pump(6);   // Sign tile -> the page
    tap_str(STR_S_SCAN_QR, 3, 30);                  // the airgap tab
    tap_str(STR_S_OPEN_CAM, 3, 6);                  // OPEN CAMERA -> scan screen
    uint8_t body[300];
    memset(body, 0x5A, sizeof body);
    memcpy(body, "psbt\xff", 5);
    memcpy(body + 8, shape == 0 ? "NOTMINE" : "NOKEYS",
           shape == 0 ? 7 : 6);
    qrt_encoder_t *enc = qrt_encoder_new(QRT_FMT_UR, body, sizeof body);
    char part[600];
    for (int i = 0; i < 32 && enc && kiss_scan_active(); i++) {
      if (qrt_encoder_next(enc, part, sizeof part) != 0) break;
      kiss_scan_inject(part, strlen(part));
      pump(2);
    }
    qrt_encoder_free(enc);
    pump(8);
    if (shape == 0) {
      save("/tmp/sim_sign_stop_fp.ppm");            // two codes, and the diff
      must_show("sign/stop-ownership", tr(STR_S_STOP_ASKS));
    } else {
      save("/tmp/sim_sign_stop_nofp.ppm");          // one code, nothing to diff
      must_show("sign/stop-no-derivation", tr(STR_S_STOP_NOFP_B));
    }
    tap_str(STR_C_BACK, 3, 6);                      // -> the sign page
    tap_str(STR_C_BACK, 3, 6);                      // -> home
  }

  // Receive lands past the highest address used or shown. Every detail keeps
  // the same privacy reminder visible; it does not claim an offline signer
  // knows whether this particular address received a payment.
  touch(310, 240); pump(3); release(); pump(6);     // Receive tile -> detail landing
  save("/tmp/sim_recv_fresh.ppm");                  // freshest address, one screen
  // The popover: page-aligned fives now, with pager rows -- the way to go
  // BACK an index without leaving the tab, and the window holds still under
  // the finger instead of re-centring on every pick.
  touch(350, 128); pump(3); release(); pump(20);    // ADDRESS #N -> index popover
  save("/tmp/sim_recv_full.ppm");
  touch(446, 170); pump(3); release(); pump(20);    // pick the first offered
  tap_str(STR_R_NEXT_ADDR, 3, 8);   // NEXT ADDRESS -> next unused index
  save("/tmp/sim_recv_reminder.ppm");               // same layout, different address text
  tap_str(STR_R_NEXT_ADDR, 3, 8);   // NEXT ADDRESS again
  save("/tmp/sim_recv_next.ppm");
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> home

  // settings: the five section tabs. Direction 1b shows ONE group at a time,
  // so a group that is not open is a group no gate can question -- which is
  // why all five get a frame here before anything else is tapped. This is the
  // same blind spot check_screen_coverage.py exists for, one level down: not a
  // screen nobody opens, but a THIRD of a screen nobody opens.
  // 50, not 6: the def list runs the KEYS entry stagger on a fresh open, and
  // six frames photographs rows part faded and a sub mid travel.
  touch(670, 240); pump(3); release(); pump(50);    // Settings tile
  save("/tmp/sim_settings.ppm");                    // SIGNER: network and address type
  // The deck: on a page whose tabs hold no pages, a stroke IS a tab step.
  // Left onto SECURITY and right back, so both directions are exercised on
  // the page with the most tabs.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("settings/swipe to security", tr(STR_I_ROW_WAYSIN));
  save("/tmp/sim_settings_swipe.ppm");
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("settings/swipe back to signer", tr(STR_G_TESTNET_NOTE));
  set_tab(SET_SECURITY);
  save("/tmp/sim_settings_security.ppm");           // duress unset: amber row + dot
  set_tab(SET_BACKUP);
  save("/tmp/sim_settings_backup.ppm");             // paper unchecked, storage on plain flash
  set_tab(SET_DEVICE);
  save("/tmp/sim_settings_device.ppm");             // two 142px rows: firmware, this device
  set_tab(SET_NOUNDO);
  save("/tmp/sim_settings_noundo.ppm");             // one card, its reason, one button

  // The bench failure, gated: NO UNDO is mostly empty glass, and a press on
  // empty glass used to find no object and emit no gesture -- so the deck
  // could be swiped INTO this tab and never out. Both strokes start at
  // (.,250), on the card's dead middle, which is exactly the surface that
  // was dead.
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("settings/swipe out of no undo", tr(STR_I_ROW_DEVICE_SUB));
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("settings/swipe back into no undo", tr(STR_I_ERASE_BTN));

  // The [ ? ] on SETTINGS -- the second page carrying one, which is what
  // proves the idiom is an idiom and not a KEYS feature. Toggling back lands
  // on the tab it left, NO UNDO, because [ ? ] is not a section.
  touch(720, 85); pump(3); release(); pump(45);   // 45: the fact rows land on the stagger
  save("/tmp/sim_settings_what.ppm");
  must_show("settings/help head", tr(STR_G_HELP_HEAD));
  touch(720, 85); pump(3); release(); pump(45);   // 45 outlasts the exit stagger

  // The stroke reaches [ ? ] now -- the bench: "i cant swipe to the question
  // mark". Still on NO UNDO: left past the deck's end opens the explainer,
  // and a right stroke on the explainer is the way back to the tab it left.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("settings/swipe past no undo opens help", tr(STR_G_HELP_HEAD));
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);
  must_show("settings/swipe closes help", tr(STR_I_ERASE_BTN));

  // The attention chip, which nothing had ever tapped. It is the one thing
  // paying for a collapsed group -- a caution two tabs away is invisible
  // without it -- and check_screen_coverage.py counts SCREENS, so a control
  // with no stop is a control with no opinion attached. Tapped from NO UNDO,
  // which is as far from BACKUP as the strip goes.
  touch(150, WT_ACTION_Y + 26); pump(3); release(); pump(50);
  save("/tmp/sim_settings_attn.ppm");               // -> SECURITY, the leftmost mark
  // The LEFTMOST lit dot, not BACKUP. The chip used to jump to BACKUP always,
  // on the strength of a comment saying both counted conditions lived there --
  // which stopped being true when duress joined the count, and a chip that
  // jumps past a lit dot is worse than one that does not move.
  must_show("the attention chip lands on the leftmost flagged group",
            tr(STR_I_ROW_WAYSIN));
  // And tapped AGAIN, already on the group it points at: nothing arrives, the
  // cautions are simply pointed at where they stand. Captured raw rather than
  // saved, because the frame that proves it is mid pulse and a settled one is
  // by design identical to the stop above.
  touch(150, WT_ACTION_Y + 26); pump(3); release(); pump(14);
  shot_raw("sim_settings_attn_again.ppm");
  pump(40);
  set_tab(SET_SIGNER);

  // The exchange itself, caught part way through: two lanes of rows on screen
  // at once, the arriving group coming in from the side of the strip the
  // finger moved towards while the one it replaces leaves the other way.
  // Written raw and not saved, for the reason shot_raw gives.
  tap_str(SET_TAB_KEY[SET_DEVICE], 3, 10);
  shot_raw("sim_settings_mid.ppm");
  pump(50);                                         // and let it settle again
  // And the caution, at the top of its one pulse: 260ms after the row it
  // belongs to has landed, which on the first row of SECURITY is 480ms in.
  tap_str(SET_TAB_KEY[SET_SECURITY], 3, 30);
  shot_raw("sim_settings_pulse.ppm");
  pump(50);

  // SIM_TABGIF=1 records every frame of one tab change, one per 16ms tick, the
  // way the reveal capture does. It is the only honest picture of motion -- a
  // still says where a row got to, never how it got there -- and it is behind
  // an env var because 55 frames is 60MB nobody wants on an ordinary run.
  if (getenv("SIM_TABGIF")) {
    // Two, because the page has two entrances. An ordinary group slides in
    // from the side you tapped towards; NO UNDO rises instead, and is the only
    // one that does.
    static const struct { int to; const char *fmt; } REC[2] = {
      { SET_DEVICE, "sim_tabmove_%03d.ppm" },
      { SET_NOUNDO, "sim_tabundo_%03d.ppm" },
    };
    for (int r = 0; r < 2; r++) {
      uint32_t base = 0, peak = 0;
      int first = -1, last = -1;
      set_tab(SET_SIGNER);
      tap_str(SET_TAB_KEY[REC[r].to], 3, 0);
      for (int i = 0; i < 56; i++) {
        char nm[48];
        snprintf(nm, sizeof nm, REC[r].fmt, i);
        shot_raw(nm);
        // What the exchange COSTS, which is the one question the handoff left
        // to the bench and no gate can answer: two lanes of rows exist at once
        // for ~200ms, and the pool is 126K with an assert rather than a NULL
        // at the bottom of it. Sampled here because this is the only place the
        // walk sits inside a transition instead of stepping over it.
        {
          lv_mem_monitor_t m;
          lv_mem_monitor(&m);
          uint32_t used = (uint32_t)(m.total_size - m.free_size);
          if (!i) base = used;
          if (used > peak) peak = used;
          if (used > base + 2000) { if (first < 0) first = i; last = i; }
        }
        pump(1);
      }
      printf("[tabcost] %-8s settled %u  peak %u  (+%u, %u%%)  "
             "two lanes for %dms of a %uK pool\n",
             REC[r].to == SET_NOUNDO ? "NO UNDO" : "ordinary",
             base, peak, peak - base, base ? (peak - base) * 100 / base : 0,
             first < 0 ? 0 : 16 * (last - first + 1),
             (unsigned)(126136 / 1024));
    }
    set_tab(SET_SIGNER);
  }

  // Four tabs faster than any of them settles. This is the case that crashes a
  // transition if the interrupt path is wrong: two lanes are alive, a callback
  // is pending against the one leaving, and the tap deletes both. The frame
  // afterwards is the assertion -- if anything were left animating, or freed
  // and still animated, this is not a settled DEVICE page.
  for (int t = SET_SECURITY; t <= SET_NOUNDO; t++) {
    tap_str(SET_TAB_KEY[t], 2, 2);
  }
  set_tab(SET_DEVICE);
  save("/tmp/sim_settings_fasttab.ppm");            // four tabs in 250ms, then DEVICE
  set_tab(SET_SIGNER);

  // The picked theme reaches the ORDINARY cards on this page, and stops at the
  // ones carrying a status. Asserted rather than photographed: a frame in MONO
  // cannot show which rim moved, and the whole property is about what happens
  // when a different dot is tapped.
  {
    // WT_FLAG_ACCENT, not _BORDER. 1b tints no border with the accent: the
    // selected tab wears WT_EDGE and so does every value chip, because a tab
    // strip says where you ARE, which is neither a status nor an action. What
    // carries the theme now is the chevron on every row, which is accent INK
    // -- so the flag to look for, and the property to check, both moved.
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *ord = find_flagged(scr, WT_FLAG_ACCENT);
    if (!ord) {
      printf("FAIL: no accent-flagged chrome on the settings page\n");
      return 1;
    }
    const int was = wt_accent_get();
    wt_accent_set(was == WT_ACC_GREEN ? WT_ACC_ORANGE : WT_ACC_GREEN);
    wt_accent_restyle(scr);
    pump(2);
    if (!lv_color_eq(lv_obj_get_style_text_color(ord, LV_PART_MAIN),
                     wt_accent())) {
      printf("FAIL: ordinary settings chrome did not take the new accent\n");
      return 1;
    }
    save("/tmp/sim_settings_accent.ppm");
    wt_accent_set(was);
    wt_accent_restyle(scr);
    pump(2);
    printf("ok: settings cards follow the accent, status rows keep their own\n");
  }

  // STORAGE is the one pick on the page that still LEAVES it. Every other row
  // resolves under the finger; this one moves where the recovery words live,
  // so it keeps a screen naming all three destinations and, behind that, the
  // confirmation and the 1500ms hold it has always had.
  set_tab(SET_BACKUP);
  // The "?" beside SEED WORDS: what a seed IS, on the row that names one.
  // The chip sits 14 past the value, the same lane wt_def_row_help uses
  // everywhere -- and the row's value here is a WORD, so measure it.
  {
    const char *v = tr(kiss_ui_backup_checked() ? STR_I_WORDS_OK_VAL
                                                : STR_I_WORDS_NO_VAL);
    lv_point_t vs;
    lv_text_get_size(&vs, v, wt_chrome28(v), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    // Row 0 carries a lamp, so the value starts 20 past DEF_VAL_X.
    touch(48 + 238 + 20 + vs.x + 14 + 15, SET_DEF_Y(3, 0));
    pump(3); release(); pump(8);
  }
  pump(25);                                          // the card animates in
  save("/tmp/sim_settings_whatseed.ppm");
  must_show("seed words help", tr(STR_W_WHATSEED_S));
  tap_str(STR_C_OK, 3, 8);

  def_go(3, 1);                                      // storage row -> the chooser
  save("/tmp/sim_storage_choose.ppm");               // three modes, FLASH ticked
  // Same page, encryption ON: the storage row's sub-line stops cautioning. The
  // shim used to be hardcoded 0, so only the cautioned render existed.
  tap_str(STR_C_BACK, 3, 8);                         // the live mode is inert
  s_sim_flash_enc = 1;
  kiss_settings_sim_reopen();
  pump(8);
  save("/tmp/sim_settings_backup_enc.ppm");          // "on this chip, encrypted", no tint
  s_sim_flash_enc = 0;
  kiss_settings_sim_reopen();
  pump(8);

  // CARD INFO, from THIS DEVICE. It used to hang off the storage chooser,
  // which is gone; capacity and what is on the card belong with the build id
  // and the radio rather than behind a picker for where the words live.
  set_tab(SET_DEVICE);
  // FOUR rows on this tab now: TERMS joined it, so the pitch changed and a
  // coordinate computed for three lands on the wrong one.
  //
  // TERMS first -- all ten cards, five to a page, the reference for an owner
  // who wants to READ the words rather than meet them one screen at a time.
  def_row(4, 3);
  pump(30);
  save("/tmp/sim_terms_p1.ppm");                     // SEED WORDS .. CHANGE
  // By the VALUE: "SEED WORDS" is a caption several screens carry, and a
  // needle two keys share passes on whichever shows either.
  must_show("terms/page one", tr(STR_T_SEED_VAL));
  must_not_show("terms/page one has no page two", tr(STR_T_DECOY_CAP));
  // A left stroke turns the page. Not a scroll: wt_screen is deliberately
  // not scrollable, and a scrolling list eats every stroke a few pixels in.
  // FOUR to a page, so ten terms are three pages and THE DECOY is on the
  // last one.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(40);
  save("/tmp/sim_terms_p2.ppm");                     // CHANGE .. ACCOUNT
  must_show("terms/page two", tr(STR_T_CHANGE_VAL));
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(40);
  save("/tmp/sim_terms_p3.ppm");                     // ENTROPY, THE DECOY
  must_show("terms/page three", tr(STR_T_DECOY_CAP));
  tap_str(STR_C_BACK, 3, 20);                        // -> Settings, DEVICE tab
  set_tab(SET_DEVICE);
  def_row(4, 2);                                     // This device -> the facts
  // The five rows enter on a 42ms stagger, so the frame has to wait for the
  // last one: saving straight after the tap photographed two rows and three
  // ghosts, which is a picture of the animation rather than of the page.
  pump(30);
  save("/tmp/sim_device.ppm");                       // build, enc, radio, noise, card
  must_show("device/build", tr(STR_I_DEV_BUILD));
  must_show("device/radio", tr(STR_I_DEV_RADIO));
  must_show("device/randomness", tr(STR_I_DEV_RANDOM));
  touch(SET_LABEL_X, SET_DEV_CARD_Y); pump(3); release(); pump(8);   // the card
  save("/tmp/sim_sdinfo.ppm");
  must_show("sdinfo/psbt row", tr(STR_G_SD_ROW_PSBT));
  tap_str(STR_C_BACK, 3, 8);                        // -> THIS DEVICE
  platform_sd_test_set_present(0);                  // the slot, empty
  touch(SET_LABEL_X, SET_DEV_CARD_Y); pump(3); release(); pump(8);
  save("/tmp/sim_sdinfo_nocard.ppm");               // the why pair
  must_show("sdinfo/nocard", tr(STR_G_FW_NOCARD_H));
  platform_sd_test_set_present(1);
  tap_str(STR_C_BACK, 3, 8);                        // -> THIS DEVICE
  tap_str(STR_C_BACK, 3, 8);                        // -> Settings, DEVICE tab

  set_tab(SET_BACKUP);
  def_go(3, 1);                                      // -> the chooser
  set_row(1);                                        // SD CARD -> confirmation
  save("/tmp/sim_storage_confirm_sd.ppm");
  tap_str(STR_G_STORAGE_HOLD_MOVE, 30, 6);    // no travel: no migration
  save("/tmp/sim_storage_hold_noop.ppm");
  slide_fire(STR_G_STORAGE_HOLD_MOVE);        // deliberate slide -> success
  save("/tmp/sim_storage_sd_ok.ppm");
  tap_str(STR_C_OK, 3, 8);     // OK -> Settings, on the tab it was picked from
  save("/tmp/sim_settings_sd.ppm");                 // the chip reads SD CARD
  // The home now carries the SD-storage badge (accent, breathing while the
  // card is in). Pop out to capture it, then return to migrate back to FLASH.
  tap_str(STR_C_BACK, 3, 8);      // Settings BACK, right corner -> home
  save("/tmp/sim_home_sd.ppm");                      // SD storage badge on home
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile -> Settings
  // CARD INFO while the words live on the card: the sealed row, green tick.
  set_tab(SET_DEVICE);
  def_row(4, 2);
  touch(SET_LABEL_X, SET_DEV_CARD_Y); pump(3); release(); pump(8);
  save("/tmp/sim_sdinfo_sealed.ppm");               // kiss-seed.enc, present
  must_show("sdinfo/sealed", SDSEED_FILENAME);
  tap_str(STR_C_BACK, 3, 8);                        // -> THIS DEVICE
  tap_str(STR_C_BACK, 3, 8);                        // -> Settings
  set_tab(SET_BACKUP);
  def_go(3, 1);
  set_row(0);                                        // FLASH
  slide_fire(STR_G_STORAGE_HOLD_MOVE);
  tap_str(STR_C_OK, 3, 8);     // back on FLASH

  // The two verdicts that are not success. Both were built, translated 21
  // times and never rendered, and they are the two the owner most needs to
  // read correctly: one says the keys did not move, the other says they did
  // move and something was left behind. Nothing in this walk could tell them
  // apart, because neither had ever been on screen.
  //
  // STORAGE NOT CHANGED: the destination never became durable, so the keys
  // are still exactly where they were. Mode write fails, nothing is published.
  s_sim_move_rc = WSEED_ERR_SD_IO;
  def_go(3, 1);
  set_row(1);                                        // SD CARD -> confirmation
  slide_fire(STR_G_STORAGE_HOLD_MOVE);
  save("/tmp/sim_storage_fail.ppm");
  // The title, not the body: wt_why_body splits a two paragraph string across
  // labels, and it is the verdict in the heading that must be the right one.
  must_show("storage fail", tr(STR_G_STORAGE_FAIL_T));
  tap_str(STR_C_OK, 3, 8);     // OK -> Settings, still FLASH
  must_show("storage unchanged", tr(STR_W_KEEP_BTN));

  // STORAGE CHANGED WITH WARNING: the card is verified and published, and the
  // old copy could not be removed. Two copies, never zero -- which is why this
  // is amber and not the red above, and why it must never say "not changed".
  s_sim_move_rc = WSEED_ERR_CLEANUP;
  def_go(3, 1);
  set_row(1);                                        // SD CARD
  slide_fire(STR_G_STORAGE_HOLD_MOVE);
  save("/tmp/sim_storage_cleanup.ppm");
  must_show("storage cleanup", tr(STR_G_STORAGE_CLEANUP_T));
  tap_str(STR_C_OK, 3, 8);     // OK -> Settings, now on SD
  // ...and back to FLASH, which is what the rest of the walk is written for.
  def_go(3, 1);
  set_row(0);                                        // FLASH
  slide_fire(STR_G_STORAGE_HOLD_MOVE);
  tap_str(STR_C_OK, 3, 8);
  must_show("storage restored", tr(STR_W_KEEP_BTN));

  // CANCEL on the confirmation. It lands back on the CHOOSER, not on
  // SETTINGS: the owner was picking a destination, changing their mind about
  // one of the three is not changing their mind about the question.
  def_go(3, 1);
  set_row(2);                                        // AMNESIC -> confirmation
  save("/tmp/sim_storage_confirm_amnesic.ppm");
  tap_str(STR_C_CANCEL, 3, 8);
  save("/tmp/sim_storage_choose_back.ppm");          // the chooser again
  // Not the title: "STORAGE" is the English value of more than one key, so it
  // passes on whichever screen happens to show either. The AMNESIC sub line
  // exists on this screen and nowhere else.
  must_show("storage cancel", tr(STR_I_STORE_AMN_SUB));
  tap_str(STR_C_BACK, 3, 8);
  must_show("storage unmoved", tr(STR_W_KEEP_BTN));

  // NO UNDO is a whole tab now: one card, its reason in full, and one button.
  // The paper has not been verified yet at this point in the walk, so the
  // confirmation behind the button carries its amber qualifier -- the branch
  // that goes unreachable once step 9 has verified.
  set_tab(SET_NOUNDO);
  touch(200, SET_ROW_Y(3)); pump(3); release(); pump(8);  // ERASE SEED WORDS
  save("/tmp/sim_endwords_unchecked.ppm");          // amber "paper never checked"
  tap_str(STR_C_CANCEL, 3, 8);   // CANCEL -> Settings, NO UNDO tab: a gate's
                                 // exit names the refusal, not the direction

  // PERSIST: the marks this signer keeps between sessions. Two states, so the
  // chip IS the switch -- one tap flips it and applies it. The sub line under
  // the label follows the state, which is where the erase is stated: it is on
  // screen before the tap rather than inside a list the tap has to open.
  set_tab(SET_SECURITY);
  save("/tmp/sim_settings_hist_on.ppm");             // ENABLED, "settings and..."
  def_go(3, 1);                                      // flip -> DISABLED, applied
  save("/tmp/sim_settings_hist_off.ppm");            // the value and the sub flipped
  must_show("persist off", tr(STR_G_HIST_OFF_BTN));
  must_show("persist off sub", tr(STR_I_POP_NOTHING));
  def_go(3, 1);                                      // flip back -> ENABLED
  must_show("persist on", tr(STR_G_HIST_ON_BTN));

  // ...and the state where the switch has nothing to switch. AMNESIC keeps
  // nothing by contract, so the cycle becomes the page's one in-place
  // definition: the reason opens where the switch would be, instead of a
  // dead INERT row. Forced through the seam, because reaching it by tapping
  // means migrating the words and every stop after this one stands on where
  // they are.
  {
    int was = s_sim_mode;
    s_sim_mode = WSEED_MODE_AMNESIC;
    kiss_settings_sim_reopen();
    pump(8);
    save("/tmp/sim_settings_persist_dead.ppm");      // UNAVAILABLE, in ink
    must_show("persist dead", tr(STR_I_PERSIST_DEAD_VAL));
    def_row(3, 1); pump(30);                         // the definition opens in place
    save("/tmp/sim_settings_persist_why.ppm");       // the reason, ghosts above and below
    must_show("persist dead reason", tr(STR_I_PERSIST_DEAD_PLAIN));
    s_sim_mode = was;
    kiss_settings_sim_reopen();
    pump(8);
  }

  // The RESTORE row -- the definition that replaced the fingerprint card:
  // the one backup fact that matters, opening where it stands.
  set_tab(SET_BACKUP);
  def_row(3, 2); pump(30);
  save("/tmp/sim_settings_restore.ppm");            // words + passphrase, the lesson
  must_show("backup/restore lesson", tr(STR_I_RESTORE_PLAIN));
  def_row(3, 2); pump(30);                          // tap again closes it

  // RECOVERY WORDS now belongs to Settings. Verify the paper copy, return to
  // Settings, then separately exercise the sensitive word reveal.
  def_row(3, 0);                                    // Recovery words -> warning
  save("/tmp/sim_words_warn.ppm");                  // PAPER: show, check, note
  // The other group, captured HERE and not down in the KEF section: by then
  // the 24-word block has swapped the stored mnemonic out and back, and the
  // page has no fingerprint to frame -- so the stop would photograph a state
  // no owner reaches, with the band under the rows empty for a reason that is
  // an artefact of the walk.
  // The two-tab deck, crossed by stroke in both directions: the words page
  // joined the swipe idiom with the others and no other stop swipes it.
  for (int i = 0; i <= 8; i++) { touch(500 - i * 14, 250); pump(3); }
  release(); pump(50);                             // swipe -> ENCRYPTED
  must_show("words/swipe to encrypted", tr(STR_I_KEF_W2_H));
  save("/tmp/sim_words_enc.ppm");                  // what it holds, and whose
  for (int i = 0; i <= 8; i++) { touch(300 + i * 14, 250); pump(3); }
  release(); pump(50);                             // swipe back -> PAPER
  must_show("words/swipe back to paper", tr(STR_I_WROW_SHOW_SUB));
  // VERIFY MY COPY: type the stored dev mnemonic (11x abandon + about).
  // 'abandon' = 'a','b' -> suggestion[0]; 'about' = 'a','b','o' -> suggestion[0].
  words_row(1);                         // Check my copy -> intro
  save("/tmp/sim_verify_intro.ppm");
  tap_str(STR_W_TYPE_MY_WORDS, 3, 6);     // TYPE MY WORDS -> keypad
  save("/tmp/sim_verify_entry.ppm");
  for (int i = 0; i < 12; i++) {                    // all 'abandon' -> word 12 wrong
    touch(44, 314); pump(3); release(); pump(3);    // a
    touch(450, 374); pump(3); release(); pump(3);   // b -> "ab"
    touch(55, 182); pump(3); release(); pump(3);   // accept "abandon"
  }
  pump(4);
  save("/tmp/sim_verify_mismatch.ppm");             // "word #12 does not match"
  tap_str(STR_W_TYPE_AGAIN_BTN, 3, 6);     // TYPE AGAIN -> keypad
  for (int i = 0; i < 11; i++) {                    // 11x abandon
    touch(44, 314); pump(3); release(); pump(3);
    touch(450, 374); pump(3); release(); pump(3);
    touch(55, 182); pump(3); release(); pump(3);
  }
  touch(44, 314); pump(3); release(); pump(3);      // a
  touch(450, 374); pump(3); release(); pump(3);     // b
  touch(664, 254); pump(3); release(); pump(3);     // o -> "abo"
  touch(55, 182); pump(3); release(); pump(4);     // accept "about" -> VERIFIED
  save("/tmp/sim_verify_ok.ppm");
  // The same screen with NO fingerprint to show -- the twin of the guard that
  // let 00000000 onto the warning screen, and the branch this one has always
  // taken the other side of. It is not a colour swap: with no fingerprint the
  // body gets 190px of height instead of 58 and the two labels below it go, so
  // this is a layout no gate has ever measured. kiss_ui_forget_fp is the same
  // call locking performs, so the state is a real one and not a test fiction.
  tap_str(STR_C_DONE, 3, 6);     // DONE -> Settings
  // HELD, and put back below. kiss_ui_forget_fp() is a real state and the two
  // frames under it are worth having -- but it was never undone, so every
  // screen the walk opened after this one ran with NO KEYS OPEN, for the life
  // of this file. That is roughly 120 stops, in 21 locales, and none of the
  // gates could say a word: a screen whose identity is absent still renders,
  // still fits and still does not overlap. What it does not do is BRANCH the
  // way it would on a device somebody had unlocked.
  //
  // It was already visible in this file's own captions. sim_wallet_signet is
  // annotated "badge reads SIGNET, not TESTNET" and there is no badge in the
  // frame, because kiss_home_refresh() reads a session that is not open. The
  // caption described the intent and the frame recorded the bug, and the two
  // sat next to each other.
  uint8_t held_fp[4];
  kiss_ui_last_fp(held_fp);
  kiss_ui_forget_fp();
  set_tab(SET_BACKUP);
  def_row(3, 0);                                    // Recovery words
  words_row(1);                      // Check my copy -> intro
  tap_str(STR_W_TYPE_MY_WORDS, 3, 6);   // TYPE MY WORDS -> keypad again
  for (int i = 0; i < 11; i++) {                    // 11x abandon, as above
    touch(44, 314); pump(3); release(); pump(3);
    touch(450, 374); pump(3); release(); pump(3);
    touch(55, 182); pump(3); release(); pump(3);
  }
  touch(44, 314); pump(3); release(); pump(3);      // a
  touch(450, 374); pump(3); release(); pump(3);     // b
  touch(664, 254); pump(3); release(); pump(3);     // o
  touch(55, 182); pump(3); release(); pump(4);     // accept -> VERIFIED, no fp
  save("/tmp/sim_verify_ok_nofp.ppm");              // tall body, no code below
  tap_str(STR_C_DONE, 3, 6);     // DONE -> Settings
  // The backup group with NO FINGERPRINT, which is what kiss_ui_forget_fp
  // above leaves behind: the check ran, but there was no id to record it
  // against, so the row stays amber. The name is historical -- it was added
  // expecting the green state and never looked at.
  //
  // It is the one frame that proves kiss_fp_card's guard. An all-zero
  // fingerprint is the absence of an id, and this band used to frame
  // "00000000" under a caption reading THE ID OF YOUR KEYS -- a code that
  // looks real and gets copied onto paper. The band is empty here now.
  set_tab(SET_BACKUP);
  save("/tmp/sim_settings_checked.ppm");            // amber row, and no id card

  // ...and the keys are open again. The two frames above are the whole reason
  // the id was dropped; everything after this is a signer somebody is holding.
  kiss_ui_set_last_fp(held_fp);

  set_tab(SET_BACKUP);
  def_row(3, 0);                                    // Recovery words -> warning again
  words_row(0);                        // Show the words -> the WT_WARN gate
  save("/tmp/sim_words_gate.ppm");                  // eye mark, the two captions
  must_show("words gate", tr(STR_W_SHOW_SENT));
  // a tap is NOT enough here either
  tap_str(STR_W_HOLD_SHOW, 2, 4);
  save("/tmp/sim_words_gate_noop.ppm");             // still the gate
  slide_at(208, 430, 340); release(); pump(10);     // the full slide -> the grid
  save("/tmp/sim_words.ppm");                       // 3x4, dim numbers, WARN band
  tap_str(STR_C_DONE, 3, 6);     // DONE -> Settings

  // A 24-word seed is the only case that paginates, and 12-word wallets are
  // what the rest of this walk uses -- so swap the stored mnemonic directly
  // (same file, same statics) rather than typing 24 words through the keypad.
  // Forward only: the grid has no page-back and no early DONE -- leaving is
  // a decision and it happens on the last sheet.
  {
    char save_seed[sizeof s_sim_seed];
    snprintf(save_seed, sizeof save_seed, "%s", s_sim_seed);
    size_t o = 0;
    for (int i = 0; i < 24; i++)
      o += (size_t)snprintf(s_sim_seed + o, sizeof s_sim_seed - o,
                            "%s%s", i ? " " : "", SIM_WORDS[i]);
    set_tab(SET_BACKUP);
    def_row(3, 0);                                  // Recovery words -> warning
    words_row(0);                      // Show the words -> the gate
    slide_at(208, 430, 340); release(); pump(10);   // slide through
    save("/tmp/sim_words24_p1.ppm");                // 1-12, one lit sheet dot
    tap_str(STR_R_NEXT, 3, 6);   // NEXT
    save("/tmp/sim_words24_p2.ppm");                // 13-24, DONE appears
    tap_str(STR_C_DONE, 3, 6);   // DONE -> Settings
    snprintf(s_sim_seed, sizeof s_sim_seed, "%s", save_seed);
  }

  // ENCRYPTED BACKUP: the backup page's other row. Consent screen, the
  // borrowed keyboard in create mode ("kef" is deliberately weak, so the
  // reworded weak card gets a frame), type twice, then the locked QR with
  // the fingerprint on it, and the card write's verdict chip.
  set_tab(SET_BACKUP);
  def_row(3, 0);                                    // Recovery words -> backup page
  words_tab(WORDS_ENC);                            // the group, not a wedged row
  words_row(0);                                    // Encrypted backup -> consent
  save("/tmp/sim_kef_warn.ppm");                    // the PASSPHRASE wording
  must_show("kef/with passphrase", tr(STR_I_KEF_F1_V));

  // The same screen for an owner with no passphrase, where the words alone
  // ARE the keys: the chip, the subtitle and the first claim all say so, and
  // the sentence about what is missing has nothing to warn about. Forced the
  // way the MADE record's three sources are, because reaching it honestly
  // would mean a second login and every stop after this one stands on the
  // session that is already open.
  //
  // Without this the branch is BUILT AND NEVER CAPTURED, which is the state
  // check_screen_coverage.py exists to name -- except that it counts screens
  // and this is a branch inside one, so nothing would have said a word.
  kiss_session_open("");                            // decoy: no passphrase
  tap_str(STR_C_BACK, 3, 8);                        // BACK -> the backup page
  words_tab(WORDS_ENC);
  words_row(0);                                    // Encrypted backup again
  save("/tmp/sim_kef_warn_nopass.ppm");             // YOUR KEYS, and no caveat
  // The fact VALUE, not its caption: "WHAT OPENS IT" is the caption on more
  // than one screen, and the value is what differs between the two variants
  // of this one.
  must_show("kef/no passphrase", tr(STR_I_KEF_F1_V_NP));
  must_not_show("kef/no passphrase says nothing about one",
                tr(STR_I_KEF_PP_H));
  kiss_session_open("x");                           // back to the truth
  tap_str(STR_C_BACK, 3, 8);
  words_tab(WORDS_ENC);
  words_row(0);

  slide_fire(STR_I_KEF_MAKE_BTN);                   // slide CHOOSE A PASSWORD
  save("/tmp/sim_kef_pass.ppm");                    // CREATE A BACKUP PASSWORD
  touch(664, 278); pump(3); release(); pump(3);     // k
  touch(201, 202); pump(3); release(); pump(3);     // e
  touch(312, 278); pump(3); release(); pump(3);     // f
  touch(725, 430); pump(3); release(); pump(6);     // OK -> WEAK PASSWORD card
  save("/tmp/sim_kef_weak.ppm");
  must_not_show("kef weak card offers no way past", tr(STR_L_USE_ANYWAY));
  touch(400, 372); pump(3); release(); pump(6);     // GO BACK -> keyboard, "kef" intact
  // Six more, to nine: a backup password faces unlimited offline guessing, so
  // this card refuses like the passphrase one and the way past is a longer one.
  for (int i = 0; i < 2; i++) {
    touch(664, 278); pump(3); release(); pump(3);   // k
    touch(201, 202); pump(3); release(); pump(3);   // e
    touch(312, 278); pump(3); release(); pump(3);   // f
  }
  touch(725, 430); pump(3); release(); pump(6);     // OK -> TYPE AGAIN
  save("/tmp/sim_kef_again.ppm");
  for (int i = 0; i < 3; i++) {
    touch(664, 278); pump(3); release(); pump(3);   // k
    touch(201, 202); pump(3); release(); pump(3);   // e
    touch(312, 278); pump(3); release(); pump(3);   // f
  }
  touch(725, 430); pump(3); release(); pump(8);     // OK -> seal -> the locked QR
  save("/tmp/sim_kef_qr.ppm");
  must_show("kef envelope id", "73C5DA0A");
  tap_str(STR_I_KEF_SD_BTN, 3, 8);                  // SAVE TO SD CARD -> chip
  save("/tmp/sim_kef_sd.ppm");
  must_show("kef sd verdict", tr(STR_S_SAVED_NOTE));
  tap_str(STR_C_DONE, 3, 8);                        // DONE -> the backup page
  tap_str(STR_C_BACK, 3, 8);                        // BACK -> Settings

  // FIRMWARE and LANGUAGE are rows on the DEVICE tab now, not header pills.
  // What this proves is the ROUTE: Settings tears itself down before handing
  // over, so a leak shows up as the firmware screen drawn on top of a live
  // settings page.
  set_tab(SET_DEVICE);
  def_row(3, 1);                                    // Firmware -> the update screen
  save("/tmp/sim_settings_fw.ppm");                 // reached from settings, not directly
  touch(WT_EXIT_X + 70, WT_ACTION_Y + 26); pump(3); release(); pump(8);  // BACK -> settings
  save("/tmp/sim_settings_fw_back.ppm");            // one settings page, rebuilt

  // LANGUAGE lives on the action band now, its label the active language's
  // own name -- so it is on EVERY tab, and the picker behind it is unchanged.
  band_lang();                                      // -> the full screen picker
  save("/tmp/sim_lang_picker.ppm");                 // 21 locale choices, current selected
  {                                                 // re-pick the ACTIVE language so a
    int li = kiss_lang_pick_slot(i18n_get_lang()); // SIM_LANG walk stays in its locale
    // The cells are flag + word now, left aligned and content sized, so the
    // tap aims just inside the cell's head rather than a 248px box centre.
    touch(16 + (li % 3) * 260 + 20, 76 + (li / 3) * 52 + 20);
    pump(3); release(); pump(10);                   // settings rebuilt, same language
  }

  // ADDRESS TYPE advances under the finger. Oldest to newest, so from the
  // NATIVE default one tap lands on LEGACY and two more come back -- and the
  // "?" below is where all three are named at once, which is the job the list
  // was really doing.
  set_tab(SET_SIGNER);
  def_cycle(2, 1, 1);                               // NATIVE -> LEGACY
  save("/tmp/sim_settings_legacy.ppm");             // the row reads 1... / Legacy
  // The NAME, not the BIP number. The number left the row's sub for the card
  // behind the "?" beside it, which already named all three -- so the row was
  // holding the card's content in a lane that had to ellipsise to fit it.
  must_show("type legacy", tr(STR_S_TY_LEGACY));
  def_cycle(2, 1, 1);                               // -> NESTED
  save("/tmp/sim_settings_nested.ppm");             // 3..., the middle rung
  must_show("type nested", tr(STR_S_TY_NESTED));
  def_cycle(2, 1, 1);                               // -> back to NATIVE

  // The "?" after the address type VALUE, and the card behind it: what the
  // three names mean, which BIP each one is, and what it costs. CLOSE or a tap
  // outside dismisses; both ways out get walked. The chip sits 14 past the
  // whole prefix string (wt_def_row_help) -- "tb1…", ellipsis included,
  // since this leg of the walk is still on the sim's testnet default. Aiming
  // at a bare 3-glyph prefix lands on the value and CYCLES the row instead.
  //
  // ...and 14 past the CYCLE MARK after that. The loop left the pinned right
  // lane so a settings row that changes in place stops looking like the KEYS
  // row that opens a screen, and this is the one row carrying both -- so the
  // chip moved right by the mark's width and this tap has to follow it. Aiming
  // at where it used to be now lands on the loop and cycles the address type.
  {
    lv_point_t vs, ms;
    lv_text_get_size(&vs, "tb1\xE2\x80\xA6", wt_chrome28("tb1\xE2\x80\xA6"),
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&ms, LV_SYMBOL_LOOP, wt_font23(), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    touch(48 + 238 + vs.x + 14 + ms.x + 10 + 15, SET_DEF_Y(2, 1));
    pump(3); release(); pump(8);
  }
  save("/tmp/sim_settings_bip.ppm");                // the card, over the scrim
  must_show("bip card", tr(STR_I_BIP_T));
  // Both notes in full: the card was 480 wide and off centre, and its note
  // lane ellipsised the middle option to "older apps accep...". This is the
  // assertion that says the widened card actually fixed it.
  must_show("bip note 49", tr(STR_I_BIP_49_NOTE));
  must_show("bip note 84", tr(STR_I_BIP_84_NOTE));
  // CLOSE, which was a bare label with no handler -- "CLOSE does not actually
  // work and one has to tap away from popup". Tapping the word must dismiss
  // the card on its own now, so the walk leaves this way and comes back to
  // leave by the scrim.
  tap_str(STR_I_BIP_CLOSE, 3, 8);
  must_not_show("bip card closed by CLOSE", tr(STR_I_BIP_T));
  {
    lv_point_t vs, ms;
    lv_text_get_size(&vs, "tb1\xE2\x80\xA6", wt_chrome28("tb1\xE2\x80\xA6"),
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&ms, LV_SYMBOL_LOOP, wt_font23(), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    touch(48 + 238 + vs.x + 14 + ms.x + 10 + 15, SET_DEF_Y(2, 1));
    pump(3); release(); pump(8);
  }
  touch(60, 440); pump(3); release(); pump(8);      // scrim -> dismissed

  // THEME is the breathing dot on the action band now: what a theme pick
  // CHANGES is the page, so the page is the preview and the dot is its own
  // label -- the bench asked for "a tappable color dot pulsating". Enum
  // order MONO, GREEN, CYPHERPINK, ORANGE, so two taps reach pink and two
  // more come home. Tapped from DEVICE so the frames show a def list moving.
  set_tab(SET_DEVICE);
  band_theme();                                     // -> GREEN, page and all
  save("/tmp/sim_settings_green.ppm");              // every mark on the page moved
  band_theme();                                     // -> CYPHERPINK
  save("/tmp/sim_settings_pink.ppm");               // accent recolors the chrome + title
  tap_str(STR_C_BACK, 3, 6);      // BACK, right corner -> home still pink
  save("/tmp/sim_wallet_pink.ppm");
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  set_tab(SET_DEVICE);
  band_theme(); band_theme();                       // ORANGE, then back to MONO

  // NETWORK, the first row on the SIGNER tab, MAINNET -> TESTNET -> SIGNET.
  // MAINNET first in the cycle is deliberate: the very NEXT tap off it turns
  // the row amber, and coming back is two taps with the amber up for both.
  //
  // TESTNET and SIGNET both read tb1..., which is the trap this device cannot
  // catch for you -- so the chip's own name is the only thing that separates
  // them, and the walk photographs each one.
  set_tab(SET_SIGNER);
  // Park on MAINNET first: the amber the frame below is FOR is the row
  // reacting to leaving real bitcoin, and it only appears on the tap that
  // does. The walk arrives here on testnet, so without this the "first flip"
  // frame was one the row had no reason to colour.
  net_to(KISS_NET_MAIN);
  net_to(KISS_NET_TESTNET);                         // the tap off mainnet
  save("/tmp/sim_settings_tn_first.ppm");           // amber row, amber value
  must_show("net testnet", tr(STR_G_TESTNET_NOTE));
  must_show("net testnet names itself", kiss_net_name());
  // SIGNET next, because it is the option that had never existed: the chip,
  // the sub line and the home badge are the only three places on the device
  // that can tell it from TESTNET at all.
  net_to(KISS_NET_SIGNET);
  save("/tmp/sim_settings_signet.ppm");             // the value reads SIGNET
  must_show("net signet names itself", kiss_net_name());
  tap_str(STR_C_BACK, 3, 6);      // BACK, right corner -> home
  save("/tmp/sim_wallet_signet.ppm");               // badge reads SIGNET, not TESTNET
  must_show("home badge signet", kiss_net_name());
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile
  net_to(KISS_NET_TESTNET);
  save("/tmp/sim_settings_tn.ppm");

  // DENOMINATION: two values, flipped and flipped back, because the sats/BTC
  // choice reaches every amount the sign screen draws.
  //
  // On the DEVICE tab, row 0. It moved off SIGNER, where it was ranked equal
  // to which chain the coins are on -- and back to SIGNER afterwards, because
  // everything below this point drives that tab.
  set_tab(SET_DEVICE);
  def_go(3, 0);
  save("/tmp/sim_settings_btc.ppm");                // the value reads BTC
  must_show("denomination", "BTC");
  def_go(3, 0);                                     // back to SATS
  set_tab(SET_SIGNER);

  tap_str(STR_C_BACK, 3, 6);      // BACK, right corner -> home
  save("/tmp/sim_wallet_testnet.ppm");              // home now shows TESTNET badge
  touch(310, 240); pump(3); release(); pump(6);     // Receive: tb1 detail landing
  save("/tmp/sim_recv_tn.ppm");                     // detail, on testnet
  // The list is one tab away. Capture it on testnet so the tb1 lines and the
  // count render at least once outside the fresh-landing default.
  tap_str(STR_R_ALL_ADDR, 3, 40);                   // ALL ADDRESSES tab
  save("/tmp/sim_recv_detail_tn.ppm");              // reused filename: now the list
  // The testnet silent-payment address is one character longer than mainnet
  // (tsp1 vs sp1) and was the only receive QR the walk never rendered, which
  // is where a truncation report landed. Capture both sizes so their decoded
  // payloads can be compared byte-for-byte.
  tap_str(STR_R_TAB_SP, 3, 40);                     // SILENT tab (testnet)
  touch(400, 153); pump(3); release(); pump(6);     // SILENT ADDRESS -> the QR view
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
  tap_str(STR_C_OK, 3, 6);     // OK closes the explanation
  tap_str(STR_C_BACK, 3, 6);     // BACK from SP (leftmost) -> detail
  tap_str(STR_C_BACK, 3, 4);     // BACK from detail (leftmost) -> home
  touch(130, 240); pump(3); release(); pump(6);     // Sign -> chooser
  tap_str(STR_S_FROM_SD, 3, 30);                    // SD CARD tab
  touch(328, 150); pump(3); release(); pump(8);     // file -> verify: TESTNET row
  save("/tmp/sim_verify_tn.ppm");
  tap_str(STR_C_BACK, 3, 6);     // BACK (leftmost) -> the list page
  tap_str(STR_C_BACK, 3, 6);     // BACK -> home
  touch(670, 240); pump(3); release(); pump(6);     // Settings again
  set_tab(SET_SIGNER);
  // By STATE, not by count. The relative "+2 lands on MAINNET" drifted the
  // day SIGNET joined the rotation, and every "mainnet" frame after it had
  // been photographing a test network in silence -- caught only when the
  // KEYS hint stop below became the first CHECK on this leg. This is the
  // shape net_to() was lifted out of; the three flips above now share it.
  net_to(KISS_NET_MAIN);
  tap_str(STR_C_BACK, 3, 4);      // BACK, right corner -> home

  // MAINNET, and this is the whole point of the excursion. 120 stops run after
  // this flip, but not one of them is a screen that BRANCHES on the network:
  // they are the setup, duress and firmware sections. Every screen that reads
  // kiss_testnet() -- home, receive, pairing, wallet info -- was photographed
  // on testnet only, in all 21 locales, for the life of the walk.
  //
  // So the branches that only mainnet takes had never been rendered: the home
  // badge absent rather than present, MAINNET in WT_INK where TESTNET is amber,
  // bc1 and sp1 prefixes where tb1 and tsp1 are one character longer. That last
  // one is a layout difference, not a colour one, and the receive screen folds
  // the address to fit.
  save("/tmp/sim_wallet_mainnet.ppm");              // home: NO testnet badge
  touch(310, 240); pump(3); release(); pump(6);     // Receive -> bc1 detail
  save("/tmp/sim_recv_mainnet.ppm");                // bc1, no "on testnet" line
  tap_str(STR_R_TAB_SP, 3, 40);                     // SILENT tab
  touch(400, 153); pump(3); release(); pump(6);     // SILENT ADDRESS -> SP view
  save("/tmp/sim_recv_sp_mainnet.ppm");             // sp1, a character shorter
  tap_str(STR_C_BACK, 3, 6);
  tap_str(STR_R_TAB_THIS, 3, 40);                   // back to THIS ADDRESS
  // The refusals, on the same screens that just rendered working: flip the
  // lock, rebuild each, and the QR must be GONE -- not a code encoding the
  // words SESSION LOCKED, which is what these drew before the fix.
  s_sim_session_locked = 1;
  tap_str(STR_R_NEXT_ADDR, 3, 8);   // NEXT rebuilds tab 1 via recv_refresh
  save("/tmp/sim_recv_locked.ppm");                 // the state as words, no QR
  tap_str(STR_R_ALL_ADDR, 3, 40);                   // ALL ADDRESSES tab
  save("/tmp/sim_recv_list_locked.ppm");            // the state on every line
  s_sim_session_locked = 0;
  // ONE back: the list is a tab now, not a screen on top of one, so BACK from
  // it leaves RECEIVE rather than climbing a level that no longer exists.
  tap_str(STR_C_BACK, 3, 6);     // -> home
  // The KEYS tile, the same door sim_winfo uses. Not a Settings row: the
  // network note lives on the section home, and the coordinates differ.
  touch(490, 240); pump(3); release(); pump(6);     // KEYS tile -> section home
  save("/tmp/sim_winfo_mainnet.ppm");               // MAINNET note, INK not WARN;
                                                    // and the POST-OPEN band: the
                                                    // standing statement, because
                                                    // the walk opened [ ? ] long ago
  tap_str(STR_C_BACK, 3, 4);     // -> home
  // The FIRST-RUN state, forced: on mainnet with the hint lane free, an owner
  // who has never opened [ ? ] gets the lowercase line and the breathing mark.
  // The walk consumed its own first open back on the testnet leg, so the seen
  // bit is reset for one frame -- this is the deliverable's other half, and
  // the pair proves the hint STOPS.
  wt_help_seen_set(false);
  touch(490, 240); pump(3); release(); pump(45);    // KEYS tile again, settled
  save("/tmp/sim_winfo_hint.ppm");                  // "new here? that mark..."
  must_show("keys/first-run hint", tr(STR_C_HELP_HINT));
  touch(720, 85); pump(3); release(); pump(30);     // open [ ? ]: hint dies for good
  touch(720, 85); pump(3); release(); pump(30);     // and back to the rows
  tap_str(STR_C_BACK, 3, 4);     // -> home

  // step 7: seed wizard — lock, wipe the seed, KISS again -> first-boot flow
  touch(44, 44); pump(3); release(); pump(20);     // KISS logo -> lock -> menu
  kiss_seed_wipe();                               // pretend a factory-fresh device
  // The whole word, via the same helper every other unlock in this walk uses.
  //
  // This used to be five strokes inline -- K, I and ONE S -- under a comment
  // saying the recogniser reveals as soon as "K I S" satisfies it. It did, and
  // that was the bug: three letters opened the device. The walk had been
  // written around the defect, which is why no gate ever saw it. Drawing the
  // real word here is what makes this stop mean anything.
  draw_cover_word(); release(); pump(20);
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
  tap_str(STR_C_BACK, 3, 6);     // BACK -> the chooser

  // RESTORE, all the way through the warning's exit. This is the first-boot
  // door a real owner reaches, not a direct screen call: every word, both
  // passphrase entries below and every action are real touches.
  touch(218, 240); pump(3); release(); pump(4);     // RESTORE FROM WORDS (row 1)
  // The flag main.c reads to choose the passphrase flow. Restored words get one
  // entry and the fingerprint as the check; invented ones get typed twice. If
  // this ever stops tracking the chooser, a restoring owner is asked to invent
  // a second passphrase and lands in an empty wallet with no error anywhere --
  // so it is asserted on both branches rather than assumed.
  if (!kiss_setup_restoring()) {
    printf("FAIL: setup/restore: chooser took RESTORE, flag says new words\n");
    g_walk_fails++;
  }
  save("/tmp/sim_setup_storage.ppm");               // FLASH / SD CARD / AMNESIC
  touch(174, 144); pump(3); release(); pump(4);     // FLASH
  // restoring shows a third option here: an encrypted backup carries its own
  // length, so it sits beside 12/24 rather than after them
  save("/tmp/sim_setup_count_restore.ppm");         // 12 / 24 / SCAN LOCKED QR
  touch(218, 176); pump(3); release(); pump(4);     // 12 WORDS
  save("/tmp/sim_setup_restore.ppm");
  type_restore_prefix(SIM_12_PREFIXES[0]);
  save("/tmp/sim_setup_sug.ppm");                   // suggestions visible
  touch(55, 182); pump(3); release(); pump(3);     // accept "gravity" -> word 2
  for (size_t i = 1; i < sizeof SIM_12_PREFIXES / sizeof SIM_12_PREFIXES[0]; i++)
    restore_word(SIM_12_PREFIXES[i]);
  pump(30);                                         // words -> passphrase intro
  // The headline, not the title: PASSPHRASE is the English value of more than
  // one key and would pass on whichever screen shows either.
  must_show("setup/restore-ppintro", tr(STR_L_PPINTRO_HEAD));
  if (!act_for(STR_L_PP_TYPE_IT, "setup restore offers")) { /* counted */ }
  // The screen has ONE way forward now, so this is a claim about the whole
  // band rather than about which of two buttons won: nothing here tells
  // someone whose passphrase already exists to invent one.
  must_not_show("setup/restore-no-create-verb", tr(STR_L_CREATE_PASS_BTN));
  must_not_show("setup/restore-no-peer-choice", tr(STR_L_NO_PASSPHRASE));
  tap_str(STR_L_PP_TYPE_IT, 3, 8);                  // TYPE IT -> keyboard

  // A restore asks for an EXISTING passphrase. Let one typed byte expire and
  // prove the reset did not turn that instruction into CREATE YOUR PASSPHRASE,
  // which invites the owner to create a different, empty wallet. The saved
  // keyboard is a distinct visual-QA stop for this state.
  touch(46, 278); pump(3); release(); pump(3);      // 'a'
  pump(8200);                                      // 131s > 120s, untouched
  save("/tmp/sim_setup_restore_idle.ppm");
  must_show_exact("setup/restore-idle-caption", tr(STR_L_PASSPHRASE_CAP));
  must_not_show("setup/restore-idle-not-create", tr(STR_L_CREATE_YOUR_PASS));
  must_show("setup/restore-idle-prompt", tr(STR_L_TYPE_PROMPT));
  touch(46, 278); pump(3); release(); pump(3);      // retype the wiped 'a'
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint, once
  must_show("setup/restore-fingerprint", tr(STR_L_TAP_TO_OPEN));
  tap_str(STR_L_TAP_TO_OPEN, 3, 8);                 // commit -> warning
  must_show("setup/restore-warning", tr(STR_L_WARN_T));

  // Exercise the other setup caption that must survive the same idle wipe.
  // The exact words and the exact passphrase recreate the restored wallet's
  // fingerprint; this also leaves the warning in its verified state before
  // testing the restore-specific exit below.
  tap_str(STR_L_VERIFY_FULL_BACKUP, 3, 6);          // warning -> rehearsal intro
  tap_str(STR_W_TYPE_MY_WORDS, 3, 6);               // intro -> recovery keyboard
  for (size_t i = 0; i < sizeof SIM_12_PREFIXES / sizeof SIM_12_PREFIXES[0]; i++)
    restore_word(SIM_12_PREFIXES[i]);
  pump(4);
  tap_str(STR_C_DONE, 3, 8);                        // words matched -> exact passphrase
  touch(46, 278); pump(3); release(); pump(3);      // 'a'
  pump(8200);                                      // expire it on the verify keyboard
  must_show_exact("setup/verify-idle-caption", tr(STR_L_VERIFY_PASS));
  must_not_show("setup/verify-idle-not-create", tr(STR_L_CREATE_YOUR_PASS));
  must_show("setup/verify-idle-prompt", tr(STR_L_TYPE_PROMPT));
  touch(46, 278); pump(3); release(); pump(3);      // retype the wiped 'a'
  touch(725, 430); pump(3); release(); pump(25);    // exact match -> verified warning
  must_show_exact("setup/restore-backup-verified", tr(STR_L_BACKUP_VERIFIED));
  tap_str(STR_C_I_UNDERSTAND, 3, 30);               // restored setup ends at home
  if (kiss_duress_ui_active()) {
    printf("FAIL: setup/restore opened the duress wizard after acceptance\n");
    g_walk_fails++;
  }
  must_show("setup/restore-home", tr(STR_H_TILE_SIGN));

  // The rest of this step owns the new-seed screens. Reset like the real erase
  // path, then enter the same chooser on demand so those existing stops keep
  // testing a fresh creation rather than the wallet just restored above.
  kiss_seed_wipe();
  kiss_session_close();
  kiss_duress_forget();
  kiss_begin_setup(); pump(20);
  must_show("setup/new-chooser", tr(STR_W_SETUP_T));

  // the real path: CREATE SEED, entropy, quiz, login twice. Creating no longer
  // asks how many words -- it is always 12 -- so FLASH lands on the method
  // choice (camera+taps vs dice). Both branches are visited: the camera one for
  // its layout only (it cannot complete without a sensor), the dice one all the
  // way to a wallet. kisstest covers the real SHA256.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  // The other branch of the same flag: these words are being MADE, so the
  // passphrase after them is invented and keeps the type-twice net -- which the
  // walk goes on to exercise a few hundred lines below.
  if (kiss_setup_restoring()) {
    printf("FAIL: setup/new: chooser took CREATE, flag says restoring\n");
    g_walk_fails++;
  }
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
  // No audit detour here: the door was tried for one commit and withdrawn --
  // the owner chose a wizard with nothing extra. The audit's stops live on
  // the Settings path, and the explainer card's text still names the doubt.
  touch(400, 40); pump(3); release(); pump(20);     // tap anywhere -> closes
  pump(20);                                         // entropy screen rebuilds

  // TAP TO ADD RANDOMNESS, and the refusal behind it. Both were NEVER OPENED:
  // the walk stopped at the entropy screen and backed out, so source 3 -- the
  // one screen on this path the owner actually interacts with -- had no gate
  // looking at it in any locale. sim_entropy_cb stands in for the camera.
  tap_str(STR_W_ENT_CAPTURE, 3, 6);     // ADD YOUR TAPS -> the card
  save("/tmp/sim_setup_ent_tap.ppm");               // SOURCE 3, 64 empty segments
  // Part way through, because the empty state proves nothing about the strip:
  // before the first tap the chain is all zeroes and every cell is off, which
  // is indistinguishable from a strip that never updates. Twenty taps in, the
  // segments are partly lit and the bits are live, so a stop here is the only
  // one that can tell those two apart.
  for (int i = 0; i < 20; i++) { touch(400, 260); pump(3); release(); pump(3); }
  pump(6);
  save("/tmp/sim_setup_ent_tap_part.ppm");          // counter climbing, bits live
  // The chip's noise source never came up. This is the ONLY way to reach the
  // refusal -- the other two legs cannot fail after a full 64-tap gate -- and
  // it is why the screen has to be forced rather than walked into. It was a
  // title over a 704px paragraph until something finally rendered it.
  s_sim_trng = false;
  // 100, not 64: the counter caps at WTAP_TARGET and a press that shares a
  // frame with its release never raises CLICKED, so a tight loop lands about
  // a third of its taps. Overshooting is free; undershooting leaves the walk
  // on a half-filled bar photographing it as the refusal.
  for (int i = 0; i < 100; i++) { touch(400, 260); pump(3); release(); pump(3); }
  pump(40);                                         // tap_done_cb holds the full
                                                    // bar 400ms before folding
  save("/tmp/sim_setup_ent_fail.ppm");              // 1 + 2 + 3 -> x, in STOP
  s_sim_trng = true;                                // put the chip back
  tap_str(STR_C_TRY_AGAIN, 3, 6);     // TRY AGAIN -> entropy screen
  tap_str(STR_C_BACK, 3, 4);     // BACK -> choose

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
  tap_str(STR_C_OK, 3, 6);     // OK dismisses the explainer
  tap_str(STR_W_TYPE_MY_WORDS, 3, 8);     // TYPE MY WORDS
  save("/tmp/sim_setup_cards_entry.ppm");           // "1/11 _" over the keyboard
  // Eleven DISTINCT, non monotone words. The cards judge links real, so the old
  // "a" eleven times is now the block screen -- see CARDS_BLOCK below, where
  // that shape is typed on purpose. Under the index stub these sit 83 or more
  // apart, far outside WC_NEAR, so an ordinary draw judges clean.
  static const char *CARDS_OK11[11] = {
      "g", "v", "n", "z", "fem", "c", "a", "o", "s", "e", "sy" };
  for (int i = 0; i < 11; i++) restore_word(CARDS_OK11[i]);
  save("/tmp/sim_setup_cards_cksum.ppm");           // THE BUILT IN CHECK, tick chip
  tap_str(STR_W_CKSUM_GO, 3, 8);     // SHOW THE WORDS
  save("/tmp/sim_setup_cards_pick.ppm");            // page 1 of 8: 16 word actions, NEXT
  tap_str(STR_R_NEXT, 3, 8);     // NEXT -> page 2
  save("/tmp/sim_setup_cards_pick2.ppm");           // BACK owns the left slot now
  tap_str(STR_C_BACK, 3, 8);     // BACK -> page 1
  tap_str(STR_C_CANCEL, 3, 8);     // CANCEL -> chooser
  if (s_sim_pending_mode != -1) {
    fprintf(stderr, "cards cancel left storage mode staged\n");
    return 1;
  }
  // The 23 word draw and its single page of 8 candidates are GONE, with the
  // length choice that reached them. Creating makes 12, so the picker is always
  // 128 candidates over 8 pages and sim_setup_cards_pick24 photographs a shape
  // the product no longer builds. kiss_lastword still computes the 8 for a 23
  // word prefix and kisstest still pins it -- restoring a 24 word phrase is
  // untouched. What went is the screen, not the arithmetic.

  // The refused draw. The judge links REAL here, so typing one word eleven
  // times IS the block, rendered rather than described -- the same argument the
  // dice ramp below makes, and the only way any gate ever sees this screen.
  // Two-action row: CANCEL 48..378 (centre 213), START OVER 422..752 (centre 587).
  // pump(8) after each release, not pump(4). Four frames is 64ms and the
  // indev reads a press about every 30ms, so whether the lift is seen before
  // the next press depends on the SAMPLING PHASE -- which is set by how many
  // frames the whole walk has pumped before arriving here. Adding a stop
  // anywhere earlier moved it, this run landed on the wrong side, and the
  // walk photographed the BLIND DRAW intro eleven times over while calling it
  // the refusal screen. 8 is the number docs/house-rules.md measured.
  touch(218, 176); pump(3); release(); pump(8);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(8);     // FLASH -> method choice
  touch(394, 346); pump(3); release(); pump(8);     // BLIND DRAW
  tap_str(STR_W_TYPE_MY_WORDS, 3, 10);    // TYPE MY WORDS
  must_not_show("cards/left the intro", tr(STR_W_TYPE_MY_WORDS));
  for (int i = 0; i < 11; i++) restore_word("g");   // the same word, eleven times
  save("/tmp/sim_setup_cards_block.ppm");           // NOT A DRAW, flat bars, 2 actions
  tap_str(STR_W_START_OVER, 3, 8);     // START OVER -> empty keyboard
  save("/tmp/sim_setup_cards_retype.ppm");          // "1/11 _": the draw really is gone
  for (int i = 0; i < 11; i++) restore_word("g");   // back to the block
  tap_str(STR_C_CANCEL, 3, 8);     // CANCEL -> chooser
  if (s_sim_pending_mode != -1) {
    fprintf(stderr, "cards block cancel left storage mode staged\n");
    return 1;
  }

  // The other refusal, in the other colour. Ascending index order gives
  // sorted = +1 with no near pairs, so the verdict is SORTED: amber, its own
  // title, and no way past it any more. The chip that used to survive USE
  // ANYWAY onto the checksum card is gone with the pill, so there is no
  // sim_setup_cards_cksum_warn frame -- that screen cannot be reached with a
  // verdict on it.
  touch(218, 176); pump(3); release(); pump(4);     // CREATE SEED
  touch(174, 144); pump(3); release(); pump(4);     // FLASH -> method choice
  touch(394, 346); pump(3); release(); pump(4);     // BLIND DRAW
  tap_str(STR_W_TYPE_MY_WORDS, 3, 4);     // TYPE MY WORDS
  static const char *CARDS_SORTED11[11] = {
      "g", "m", "n", "s", "sy", "fem", "fil", "a", "v", "fol", "c" };
  for (int i = 0; i < 11; i++) restore_word(CARDS_SORTED11[i]);
  save("/tmp/sim_setup_cards_warn.ppm");            // CHECK YOUR WORDS, climbing bars
  must_not_show("cards warn offers no way past", tr(STR_L_USE_ANYWAY));
  tap_str(STR_W_START_OVER, 3, 4);     // START OVER -> empty keyboard
  for (int i = 0; i < 11; i++) restore_word(CARDS_SORTED11[i]);  // back to it
  tap_str(STR_C_CANCEL, 3, 4);     // CANCEL -> chooser
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
  tap_str(STR_W_METHOD_DICE_T, 3, 4);   // DICE row -> the keypad, at 12
  save("/tmp/sim_setup_dice.ppm");                  // empty keypad, six zero columns

  // The coin, which is the same screen in base 2. Visited FIRST and left by
  // BACK, so the dice run below is byte for byte the one that was here before.
  tap_str(STR_W_COIN, 3, 6);          // COIN -> two keys, base 2
  save("/tmp/sim_setup_coin.ppm");                  // 0 / 128, two zero columns
  // 128 flips, checked in and asserted by kisstest: face bits 127382 (floor
  // 110080), step bits 125408 (floor 109220), counts 59/69, no period.
  //
  // A key is tapped by the words ON it. SIM_COIN_OK is the string the device
  // records, HEADS is the key that records a 0, and the walk maps one to the
  // other here -- with no coordinate in between, which is the point: the
  // previous form carried the key centres as literals (x=228 and x=572, row
  // middle y=142) and every one of them was wrong the moment the card moved to
  // the 704 page lane.
  static const char SIM_COIN_OK[] =
      "01010110101000001111111101000011111001110010000010100001100101001"
      "011100011111100111100110001001011110011110111111111100000010101";
  for (int i = 0; i < 128; i++)
    tap_str(SIM_COIN_OK[i] == '0' ? STR_W_COIN_HEADS : STR_W_COIN_TAILS, 4, 4);
  save("/tmp/sim_setup_coin_full.ppm");             // 128, tick chip, DONE live
  tap_str(STR_C_BACK, 3, 6);          // BACK -> the method rows, flips dropped

  // The refusal, in base 2. Its own visit rather than more flips on the run
  // above, because the shot worth keeping there is the healthy one. Alternating
  // is the case a count test cannot see: 64/64 dead level, and every step a
  // change. The verdict screen's two columns are CENTRED in the 704 lane, which
  // is geometry no other stop renders.
  tap_str(STR_W_METHOD_DICE_T, 3, 4);   // DICE row -> the keypad
  tap_str(STR_W_COIN, 3, 6);          // COIN, empty again
  for (int i = 0; i < 128; i++)
    tap_str((i & 1) ? STR_W_COIN_TAILS : STR_W_COIN_HEADS, 4, 4);
  save("/tmp/sim_setup_coin_flag.ppm");             // PATTERN chip, level columns
  tap_str(STR_C_DONE, 3, 6);          // DONE -> refused
  save("/tmp/sim_setup_coin_warn.ppm");             // two centred columns, 2 actions
  tap_str(STR_W_DICE_MORE, 3, 4);     // KEEP GOING -> the keypad, 128 banked
  tap_str(STR_C_BACK, 3, 6);          // BACK -> the method rows
  tap_str(STR_W_METHOD_DICE_T, 3, 4);   // DICE row -> the keypad, back at base 6
  // Roll 50 cycling the six faces. The quality judge links REAL here, and to a
  // real judge this loop is a textbook ramp — so instead of dodging that, it
  // IS the flagged run: perfectly level columns wearing a PATTERN chip, which
  // is the whole argument for judging order and not just counts.
  for (int i = 0; i < 50; i++) {
    const char k[2] = { (char)('1' + i % 6), 0 };
    tap_lbl(k, 4, 4);
  }
  save("/tmp/sim_setup_dice_flag.ppm");             // PATTERN chip over LEVEL bars
  tap_lbl("?", 3, 40);                // the "?" beside the chip; 40 pumps =
                                                    // the card intro settled
  save("/tmp/sim_setup_dice_why.ppm");              // WHAT THIS CHECKS, icon grid
  tap_str(STR_C_OK, 3, 6);     // OK dismisses the explainer
  tap_str(STR_C_DONE, 3, 6);     // DONE -> the verdict screen
  save("/tmp/sim_setup_dice_warn.ppm");             // CHECK YOUR ROLLS, 2 actions, no way past
  // KEEP GOING is the way through a refusal, and it keeps every banked roll --
  // the whole argument for DICE_MAX, which no gate rendered until now.
  tap_str(STR_W_DICE_MORE, 3, 4);     // KEEP GOING -> the keypad
  save("/tmp/sim_setup_dice_kept.ppm");             // still 50, bars unchanged
  tap_str(STR_C_DONE, 3, 6);     // DONE -> refused again
  // START OVER is method_dice_cb, which now asks the count before the keypad,
  // so the length has to be picked again. Without this line the first roll
  // below landed on the count screen instead, quietly leaving 49 rolls in a run
  // the comment underneath says is 50.
  tap_str(STR_W_START_OVER, 3, 4);     // START OVER -> how many words
  touch(218, 144); pump(3); release(); pump(4);     // 12 WORDS -> empty keypad
  // The healthy run: a fixed string that reads as rolled, checked in and
  // asserted OK by kisstest. face bits 128.62 (floor 102.50), step bits 126.61
  // (floor 100.45), counts 10/7/9/9/7/8, no repeating block.
  static const char SIM_DICE_OK[] =
      "14464111145452332224636431261353544615153616323265";
  for (int i = 0; i < 50; i++) {
    const char k[2] = { SIM_DICE_OK[i], 0 };
    tap_lbl(k, 4, 4);
  }
  save("/tmp/sim_setup_dice_full.ppm");             // 50 / 50, tick chip, DONE live
  tap_str(STR_C_DONE, 3, 4);     // DONE -> words
  save("/tmp/sim_setup_words.ppm");                 // 12 words, one page, CANCEL + I WROTE THEM DOWN
  // The checksum card, and the only stop that renders it outside the BLIND
  // DRAW path. Chip is 30px at (722, 24), so its centre is (737, 39); 40 pumps
  // is the staggered card intro settled, same as the dice explainer above.
  touch(737, 39); pump(3); release(); pump(40);
  save("/tmp/sim_setup_words_cksum.ppm");           // THE BUILT IN CHECK, equation + 2 marks
  tap_str(STR_C_OK, 3, 6);     // OK dismisses the explainer
  tap_str(STR_W_WROTE, 3, 4);     // I WROTE THEM DOWN
  save("/tmp/sim_setup_quiz.ppm");
  // The choices are bare words now, left aligned at 48/428 on the same two
  // row grid, so the taps aim at the words' heads rather than box centres.
  touch(60, 234); pump(3); release(); pump(4);      // round 1: choice 0 correct
  touch(435, 234); pump(3); release(); pump(4);     // round 2: choice 1
  touch(60, 314); pump(3); release(); pump(6);      // round 3: choice 2 -> stored
  save("/tmp/sim_setup_ppintro.ppm");               // PASSPHRASE, one way forward
  // The empty branch, taken as an excursion rather than a commit. It used to
  // be a peer button on the screen above; it is the keyboard's own OK on an
  // empty field now, which is the claim Part 3 makes and the thing that had
  // to still be true before the button could go. It renders the fingerprint
  // screen wearing its no-passphrase notes, and BACK returns to the keyboard
  // the walk was heading for anyway -- one tap has to be enough to change
  // your mind before anything is committed.
  tap_str(STR_L_PP_TYPE_IT, 3, 8);        // TYPE IT -> the keyboard
  touch(725, 430); pump(3); release(); pump(25);    // OK on an EMPTY field
  save("/tmp/sim_setup_fp_nopass.ppm");             // no passphrase: your words alone open it
  tap_str(STR_C_BACK, 3, 6);     // BACK (48..188) -> the keyboard
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
  touch(725, 430); pump(3); release(); pump(4);     // OK -> weak refusal
  save("/tmp/sim_setup_weak.ppm");                  // modal: one action, BACK
  must_not_show("weak card offers no way past", tr(STR_L_USE_ANYWAY));
  // BACK is the only way off it, and the entry has to survive so the owner
  // lengthens what they typed instead of retyping it.
  touch(400, 372); pump(3); release(); pump(4);     // GO BACK -> keyboard, 'a' intact
  save("/tmp/sim_setup_weak_back.ppm");
  // Eight more, taking it to nine: the shortest passphrase this wizard accepts.
  for (int i = 0; i < 8; i++) { touch(46, 278); pump(6); release(); pump(6); }
  touch(725, 430); pump(3); release(); pump(4);     // OK -> confirm stage
  save("/tmp/sim_setup_pass2.ppm");                 // TYPE IT AGAIN
  // The secret-idle deadline, mid type-twice. TYPE IT AGAIN is holding entry
  // #1 in RAM; two untouched minutes must wipe both entries and put the
  // caption back to the first stage -- and must touch NOTHING else: the
  // staged seed and setup mode survive, and the type-twice below commits the
  // same wallet, which every stop after this one depends on.
  pump(8200);                                       // 131s > 120s, untouched
  must_show("idle-wipe/stage1", tr(STR_L_CREATE_YOUR_PASS));
  must_show("idle-wipe/prompt", tr(STR_L_TYPE_PROMPT));
  type_pass9();                                     // from stage 1 again
  touch(725, 430); pump(3); release(); pump(4);     // OK -> TYPE IT AGAIN
  touch(696, 38); pump(3); release(); pump(3);      // SHOW: make the LVGL copy explicit
  type_pass9();                                     // the same, again
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  // The fingerprint this wallet is called, read off the glass BEFORE the
  // idle deadline below. The retry must land on the SAME one: the login's
  // 120-second wipe is suppressed while the RECOVER screen holds the staged
  // secret, and if that suppression regressed, TRY AGAIN would commit an
  // empty passphrase and the warning would show a different fingerprint.
  lv_obj_t *fp_lbl = find_hex8(lv_screen_active());
  char fp_hex[16] = "";
  if (!fp_lbl) {
    printf("FAIL: no fingerprint hex on the fp screen\n");
    g_walk_fails++;
  } else {
    snprintf(fp_hex, sizeof fp_hex, "%s", lv_label_get_text(fp_lbl));
  }
  // The commit that could not finish, through the real UI: this TAP TO OPEN
  // commits the staged wallet, and a staged RECOVER result puts the recovery
  // screen where the passphrase warning should be. TRY AGAIN then re-runs
  // the same commit; the teardown under that success path used to
  // dereference the freed entry label (recover_screen had deleted the login
  // and nulled only s_login), which is what this block exists to catch. The
  // retry must also land back on the same warning the flow expects next,
  // because everything below this stop depends on it.
  g_sim_commit_recover = 1;
  tap_str(STR_L_TAP_TO_OPEN, 3, 8);     // TAP TO OPEN -> the RECOVER screen
  must_show("setup/recover", tr(STR_L_RECOVER_T));

  // Past the screen's OWN deadline first. The words have none and must not
  // gain one -- they may be the last copy -- but the passphrase the retry
  // keeps is a secret idling on a device nobody is touching, and five
  // untouched minutes take it. TRY AGAIN then cannot re-derive the wallet
  // whose fingerprint the owner read, so it must ask for the passphrase
  // again: not open the empty-passphrase wallet under that fingerprint, and
  // not sit there doing nothing, which is what refusing silently looked like
  // to the one person holding the only copy.
  pump(19000);                                     // 304s > 300s, untouched
  must_show("recover/expired", tr(STR_L_RECOVER_T));
  tap_str(STR_C_TRY_AGAIN, 3, 30);      // TRY AGAIN with no passphrase left
  save("/tmp/sim_setup_recover_reask.ppm");        // the keyboard, back at stage 1
  must_show("recover/reask", tr(STR_L_TYPE_PROMPT));
  must_show("recover/reask-stage1", tr(STR_L_CREATE_YOUR_PASS));
  if (kiss_ui_recover_active()) {
    printf("FAIL: the reask left the RECOVER screen up over the keyboard\n");
    g_walk_fails++;
  }
  // Type the same passphrase again, through the wizard's own type-twice, and
  // fail the commit once more so the block below still starts where it did.
  type_pass9();                                     // from stage 1
  touch(725, 430); pump(3); release(); pump(4);     // OK -> TYPE IT AGAIN
  type_pass9();                                     // the same, again
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  g_sim_commit_recover = 1;             // the injection is one shot: re-arm it
  tap_str(STR_L_TAP_TO_OPEN, 3, 8);     // TAP TO OPEN -> the RECOVER screen again
  must_show("setup/recover-again", tr(STR_L_RECOVER_T));
  g_sim_commit_recover = 0;
  // The recovery screen, held past the login's 120-second deadline. The
  // staged secret must not inherit the hidden login's idle wipe -- the
  // retry passphrase lives in s_pass, and a wipe would make TRY AGAIN open
  // an empty-passphrase wallet under the stale fingerprint. 131s, matching
  // the type-twice idle stop above.
  pump(8200);
  must_show("recover/idle-survives", tr(STR_L_RECOVER_T));
  save("/tmp/sim_setup_recover_idle.ppm");         // the long-lived RECOVER state itself
  if (!kiss_ui_test_rendered_secret_empty()) {
    printf("FAIL: recovery mask timer repopulated a hidden rendered secret\n");
    g_walk_fails++;
  }
  tap_str(STR_C_TRY_AGAIN, 3, 30);      // TRY AGAIN -> the passphrase warning
  must_show("setup/recover-retry", tr(STR_L_WARN_T));
  // The warning deliberately reuses the fingerprint already shown, so comparing
  // its label alone cannot prove the retry used the retained passphrase. The sim
  // marks an empty-passphrase session as the decoy; this fixture typed "a" and
  // must therefore reopen a non-decoy session after the 131-second wait.
  if (kiss_session_decoy()) {
    printf("FAIL: recovery retry opened the empty-passphrase session\n");
    g_walk_fails++;
  }
  if (fp_hex[0]) {
    lv_obj_t *warn_lbl = find_hex8(lv_screen_active());
    if (!warn_lbl) {
      printf("FAIL: setup/recover-retry: no fingerprint hex on the warning\n");
      g_walk_fails++;
    } else if (strcmp(fp_hex, lv_label_get_text(warn_lbl)) != 0) {
      printf("FAIL: setup/recover-retry: warning names %s, not the %s shown before the idle\n",
             lv_label_get_text(warn_lbl), fp_hex);
      g_walk_fails++;
    }
  }
  save("/tmp/sim_setup_recover_retry.ppm");        // retry landed, teardown held
  lv_refr_now(NULL); pump(2);
  // The same warn screen the retry just landed on, photographed again under
  // the name the docs manifest reads. Identical to the frame above and meant
  // to be: nothing happens between them, and it is the retry landing on this
  // screen that proves the wallet came back. Unchanged is the assertion.
  save("/tmp/sim_setup_warn.ppm");   // unchanged by design; red ring on I UNDERSTAND

  // Optional full recovery rehearsal: all generated words, then the exact
  // passphrase. Prefixes below uniquely put each expected word in suggestion 0.
  tap_str(STR_L_VERIFY_FULL_BACKUP, 3, 6);     // VERIFY MY COPY -> intro
  save("/tmp/sim_setup_rehearse_intro.ppm");
  tap_str(STR_W_TYPE_MY_WORDS, 3, 6);     // TYPE MY WORDS -> keypad
  for (size_t i = 0; i < sizeof SIM_12_PREFIXES / sizeof SIM_12_PREFIXES[0]; i++)
    restore_word(SIM_12_PREFIXES[i]);
  pump(4);                                          // all words -> VERIFIED
  tap_str(STR_C_DONE, 3, 8);     // DONE -> fresh passphrase entry
  save("/tmp/sim_setup_rehearse_pass.ppm");
  type_pass9();                                     // the exact passphrase
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
  tap_str(STR_C_I_UNDERSTAND, 3, 30);    // I UNDERSTAND -> the stroke chooser

  // The LAST step of setup: the two-signer idea and its rule
  // (kiss_duress_ui.c). Plain KISS opens the spare and always will; one
  // extra swipe -- any swipe -- asks for the passphrase that opens the real
  // signer. Nothing is configured here any more; the wizard teaches.
  save("/tmp/sim_duress_intro.ppm");                // two ways in
  tap_str(STR_GD_SET_UP_SPARE, 3, 40);    // -> fund the spare
  save("/tmp/sim_duress_fund.ppm");                 // why the decoy needs coins in it
  tap_str(STR_GD_SET_UP_REAL, 3, 40);    // -> confirm the model
  // The last screen before anything is stored. Both signers are the same seed
  // words and the passphrase is the only thing between them; an owner who has
  // not understood that funds the wrong one.
  save("/tmp/sim_duress_ack.ppm");
  must_show("duress/ack", tr(STR_L_WARN_T));
  tap_str(STR_C_I_UNDERSTAND, 3, 40);    // -> pick your swipe
  // The picker is BACK, and it decides something now: kiss_duress_route
  // compares the drawn stroke with the stored one, so a wrong swipe opens the
  // spare. It was deleted when the stored value routed nothing.
  save("/tmp/sim_duress_pick.ppm");                 // six shapes, two rows
  must_show("duress/pick", tr(STR_GD_PICK_REAL_T));
  tap_str(STR_GD_UNDERLINE, 3, 40);      // -> rehearse it
  save("/tmp/sim_duress_draw.ppm");                 // the printed word, waiting
  must_show("duress/draw", tr(STR_GD_DRAW_T));

  // Rehearse the underline TWICE, because one clean stroke stores nothing.
  // Coordinates are read off the printed word rather than guessed: classify
  // measures the stroke against that box, so a card that moves must move the
  // rehearsal with it. Below by0 + 0.75H is what makes it an underline rather
  // than a strike (kiss_duress.c).
  {
    lv_obj_t *lbl = find_word_label(lv_screen_active());
    if (!lbl) { printf("FAIL: duress/draw: no printed word to draw on\n"); g_walk_fails++; }
    lv_area_t b = { 250, 150, 550, 246 };
    if (lbl) lv_obj_get_coords(lv_obj_get_parent(lbl), &b);
    const int uy = b.y2 - 4;                 // just under the word: underline
    for (int rep = 0; rep < 2; rep++) {
      for (int x = b.x1 + 10; x <= b.x2 - 10; x += 40) {
        touch(x, uy); pump(3);               // 3 frames/point: the indev reads ~30ms
        // Mid-stroke, finger still down. The only frame that can show the ink
        // AT ALL -- every other save here happens after a release, by which
        // point rehearse_reset has hidden it. It is also the only frame that
        // can show the ink in the WRONG PLACE: the line was parented to the
        // canvas at y=110 while being fed absolute touch points, so every
        // stroke drew 110px below the finger and no gate could see it.
        if (rep == 0 && x > b.x1 + 80 && x <= b.x1 + 120)
          save("/tmp/sim_duress_draw_ink.ppm");
      }
      release(); pump(8);                    // 8 after a lift, or the next press folds in
      if (rep == 0) {
        // One clean stroke in: the screen says why there is a second. Nothing
        // is stored yet, and this is the only frame that shows that state.
        save("/tmp/sim_duress_draw_again.ppm");
        must_show("duress/draw-again", tr(STR_GD_DRAW_AGAIN_S));
        if (kiss_duress_real() != WDG_NONE) {
          printf("FAIL: duress/draw: one stroke stored a swipe\n");
          g_walk_fails++;
        }
      }
    }
  }
  save("/tmp/sim_duress_done.ppm");                 // drawing -> spare, +swipe -> real
  must_show("duress/rule", tr(STR_GD_DONE_T));
  // The drawing has to be OFFERED here, not merely reachable. Its only caller
  // in the shipped firmware was a Settings pill, so an owner setting up a spare
  // was never told the four letters in the diagram above can be replaced.
  must_show("duress/drawing offered", tr(STR_GD_WORD_PILL));
  // Landing on the rule screen only proves the rehearsal let us leave. What
  // matters is that the stroke it rehearsed is the stroke the unlock will now
  // compare against -- the exact link whose absence made the old picker
  // theatre, and a screenshot cannot see it.
  if (kiss_duress_real() != WDG_UNDERLINE) {
    printf("FAIL: duress/pick: rehearsed UNDERLINE, stored %d\n",
           kiss_duress_real());
    g_walk_fails++;
  }
  if (kiss_duress_route(true, WDG_UNDERLINE) != WDR_REAL ||
      kiss_duress_route(true, WDG_CIRCLE)    != WDR_DECOY ||
      kiss_duress_route(true, WDG_NONE)      != WDR_DECOY) {
    printf("FAIL: duress/pick: the stored swipe does not route\n");
    g_walk_fails++;
  }
  tap_str(STR_C_DONE, 3, 140);   // DONE -> home settles
  save("/tmp/sim_setup_home.ppm");

  // step 8: idle auto-lock — KISS_AUTOLOCK_MS untouched on the home must
  // close the session and land back on the game menu.
  //
  // Keep this ahead of KISS_AUTOLOCK_MS in main.c (300000ms today). When
  // that went from 2min to 5min in efb60bc this pump stayed at 7700 frames
  // (123s), so the lock never fired, the KISS gesture below was drawn onto
  // the still-open home screen, and every frame from here to the end of the
  // walk silently became a copy of whatever tile that opened. The wipe and
  // amnesic-mode steps were dead for four commits and still "passed".
  // tools/check_sim_taps.py is what catches that now; this margin is what
  // stops it happening in the first place.
  // ...and the same clock, run down with the FIRMWARE screen on the glass.
  //
  // This is the real path rather than the contract test at the end of the walk:
  // a live session, Settings opened from the home, Firmware opened from
  // Settings, and nothing touched until the lock fires on its own. The screen
  // used to survive it -- it hangs off the active screen rather than the wallet
  // container kiss_lock() hides, and it was on neither of main.c's lists --
  // so BACK from here rebuilt Settings on a locked device with RECOVERY WORDS
  // one row in, reading a seed the device key still opens.
  //
  // Worth the second 20000 frames. This is the one flow where a gate can stand
  // in for the hardware: nothing here is display, timing or IO, it is only
  // which screens the lock can see.
  //
  // Run on MAINNET, and that is not a detail: a test network gets an hour
  // (KISS_AUTOLOCK_TEST_MS), which at 16ms a pump is 225,000 frames nobody is
  // waiting for. Mainnet is the deadline worth proving anyway -- it is the one
  // guarding coins -- and the two values are asserted against each other here
  // so a future edit cannot quietly hand mainnet the hour instead.
  {
    extern uint32_t sim_autolock_ms(void);
    // Both networks set EXPLICITLY: this used to read "still testnet at this
    // point", which was only true while the drifted cycle above left the walk
    // on a test network through its whole mainnet leg. State the walk assumes
    // is state the walk sets.
    kiss_set_network(KISS_NET_TESTNET);
    uint32_t tn = sim_autolock_ms();
    kiss_set_network(KISS_NET_MAIN);
    uint32_t mn = sim_autolock_ms();
    if (mn != 300000 || tn <= mn) {
      printf("FAIL: auto-lock deadlines: mainnet %u, test network %u\n",
             (unsigned)mn, (unsigned)tn);
      g_walk_fails++;
    }
  }
  touch(670, 240); pump(3); release(); pump(8);     // Settings tile
  set_tab(SET_DEVICE);
  def_row(3, 1);                                    // Firmware
  save("/tmp/sim_fw_before_autolock.ppm");          // up, with the clock running
  if (!kiss_fw_ui_active()) {
    printf("FAIL: firmware screen not open before the auto-lock test\n");
    return 1;
  }
  // The warning first: 30s before the lock a toast floats over whatever is
  // up, and any touch keeps the session. 17200 pumps is ~275s -- inside the
  // warning window, before the 300s lock.
  pump(17200);
  save("/tmp/sim_autolock_warn.ppm");               // the toast over FIRMWARE
  // A touch DISMISSES it and keeps the session: this is the whole promise.
  touch(400, 240); pump(3); release(); pump(8);
  if (!kiss_fw_ui_active()) {
    printf("FAIL: the pre-lock touch should have kept the session\n");
    return 1;
  }
  save("/tmp/sim_autolock_kept.ppm");               // toast gone, screen alive
  pump(20000);                                      // 320s > 300s, untouched
  if (kiss_fw_ui_active()) {
    printf("FAIL: firmware screen survived the idle auto-lock\n");
    return 1;
  }
  if (kiss_settings_active()) {
    printf("FAIL: settings left open under a locked device\n");
    return 1;
  }
  printf("ok: auto-lock closes the firmware screen and settings under it\n");
  save("/tmp/sim_autolock_fw.ppm");                 // the game MENU, nothing over it

  pump(20000);                                      // 320s > 300s + intro settle
  save("/tmp/sim_autolock.ppm");                    // must be the game MENU again
  kiss_set_network(KISS_NET_TESTNET);               // back to what the walk runs on

  // The dead-touch banner, worn by the game cover when the GT911 never came
  // up. Here because this is a locked device showing the menu, which is the
  // state a board with dead touch actually boots into -- and unreachable by
  // walking in the sim by definition, since there is no GT911 here to fail.
  // Taken back down straight away: everything after this photographs its own
  // screen, and a banner left on the active screen would ride all of them.
  {
    extern lv_obj_t *kiss_touch_dead_banner(lv_obj_t *parent);   // main.c
    lv_obj_t *bl = kiss_touch_dead_banner(lv_screen_active());
    pump(20);
    save("/tmp/sim_touch_dead.ppm");                // amber line over the game
    must_show("touch-dead", tr(STR_G_TOUCH_DEAD));
    lv_obj_delete(bl);
    pump(20);
  }

  // Locking forgets the key material. It must also forget WHICH keys: the
  // fingerprint of the last keys unlocked used to outlive kiss_session_close,
  // and the decoy -- which opens with no login screen and sets the chip itself
  // -- would fall back to it if its own derivation ever failed. That is the
  // real keys' fingerprint, on the screen whose entire job is not admitting
  // those keys exist. Counted, not photographed: the leak is a live value, and
  // a frame of the menu looks correct either way.
  {
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    if (fp[0] || fp[1] || fp[2] || fp[3]) {
      printf("FAIL: lock left the last keys' fingerprint %02X%02X%02X%02X behind\n",
             fp[0], fp[1], fp[2], fp[3]);
      g_walk_fails++;
    }
  }

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
  kiss_ui_drop_indev_for_test();

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
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(14);
  // The home is on the glass before its keys are. It opens on the modifier
  // window alone now -- the derivation runs beside it on the other core -- so
  // for a few hundred milliseconds this is a signer with no session behind it:
  // fingerprint still scrambling because there is genuinely nothing to resolve
  // to, and every tile refusing touch. Nothing but this stop can see it; the
  // device path is a real PBKDF2 and no gate compiles one.
  save("/tmp/sim_decoy_opening.ppm");               // home up, keys still landing
  pump(140);
  save("/tmp/sim_decoy_home.ppm");                  // decoy home, reached with no login

  // Settings in a DECOY session shows exactly the page every other session
  // shows, DURESS ROW INCLUDED. This comment used to claim the opposite, and
  // the code stopped agreeing with it a long time before the frame did: a row
  // that appears for only one of the two signers is the tell, so its ABSENCE
  // was the confession an attacker could read. Nothing forks on
  // kiss_duress_real() any more, so there is nothing left for its presence to
  // corroborate either. The frame is what proves the two pages match.
  touch(670, 240); pump(3); release(); pump(20);    // SETTINGS tile
  set_tab(SET_SECURITY);
  save("/tmp/sim_decoy_settings.ppm");              // the same SECURITY group
  must_show("decoy/duress row present", tr(STR_I_ROW_WAYSIN));
  tap_str(STR_C_BACK, 3, 20);    // BACK
  touch(44, 44); pump(3); release(); pump(20);     // KISS logo -> lock, back to the game

  // KISS **plus the configured stroke** must reach the PASSPHRASE login, not the
  // spare. This is the case that shipped broken and that nothing here covered:
  // detect_cover_word fired on the LIFT OF THE LAST S, so the decoy opened before the
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
  // COVER_OPEN_DELAY_MS the word waits out before settling for the spare
  for (int i = 0; i <= 30; i++) { touch(152 + i * 13, 336); pump(1); }
  // 45 frames, not 20. The owner's door no longer opens on the lift of the
  // modifier stroke: main.c holds it for COVER_OPEN_DELAY_MS so it cannot be
  // told apart from the decoy by how fast the screen arrives. 20 frames is
  // 320ms, which is inside that wait, so this shot used to catch the menu and
  // every step after it shifted by one. That surfaces as dozens of overlap
  // findings on later screens rather than as a failure here, so keep the slack.
  release(); pump(45);
  save("/tmp/sim_real_login.ppm");                  // passphrase keyboard, NOT a wallet home
  type_pass9();                                     // the wallet's passphrase
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  tap_str(STR_L_TAP_TO_OPEN, 3, 140);   // TAP TO OPEN -> home

  // step 9: WIPE WALLET — arm (red), confirm, ERASED screen, OK -> game menu
  touch(670, 240); pump(3); release(); pump(6);     // Settings tile

  // FOUR more screens nothing had ever opened: the duress page itself and all
  // three custom-letter screens behind it. check_screen_coverage.py was the
  // only thing that knew, because a screen with no walk stop is a screen no
  // gate has an opinion on -- and YOUR LETTERS ARE SET was a wall of text for
  // exactly that reason. The duress row is full width at SG_FULL_Y 331.
  // The audit, from its home beside the ways in row. The pill went to a
  // chooser while there were two behind it; the camera audit is gone, so it
  // opens the randomness audit directly. DONE returns here via the done cb.
  //
  // The stub stream is deterministic and rewound here, so the finished frame
  // always shows the score test_rngq.c pinned as golden: 105.920, EVEN.
  sim_rng_rewind();
  set_tab(SET_SECURITY);
  def_row(3, 2);                      // Audit -> the chooser
  save("/tmp/sim_audit_choose.ppm");                // two rows, each stated
  // HOW YOUR KEYS WERE MADE. Nothing is forced here: the record is written by
  // the same funnel the device writes it from, so this photographs whatever the
  // walk last created -- which by this point is the dice run above.
  tap_str(STR_W_MADE_T, 3, 8);        // -> the record
  save("/tmp/sim_made.ppm");                        // MADE BY card + the fold
  must_show("made/method", tr(STR_W_CHOOSE_DICE));
  must_show("made/cap", tr(STR_W_MADE_CAP));
  tap_str(STR_C_BACK, 3, 8);          // BACK -> the chooser
  // The camera path's four chip fold, and the import's absence of one. Forced,
  // because reaching all three states through the wizard would mean creating
  // three wallets and every stop after this one stands on the wallet that is
  // already here. Only the RECORD is forced; the screen reads it as it always
  // does.
  kiss_seed_set_source(WSEED_SRC_MIX);
  tap_str(STR_W_MADE_T, 3, 8);
  save("/tmp/sim_made_mix.ppm");                    // 4 marks + -> YOUR KEYS
  must_show("made/mix", tr(STR_W_CHOOSE_MIX));
  tap_str(STR_C_BACK, 3, 8);
  kiss_seed_set_source(WSEED_SRC_KEF);
  tap_str(STR_W_MADE_T, 3, 8);
  save("/tmp/sim_made_import.ppm");                 // no fold to show, and says so
  must_show("made/import", tr(STR_W_MADE_ELSE));
  tap_str(STR_C_BACK, 3, 8);
  // A coin does not report DICE. Forced for the same reason as the two above:
  // the walk creates one seed, and which source it names is the record talking.
  kiss_seed_set_source(WSEED_SRC_COIN);
  tap_str(STR_W_MADE_T, 3, 8);
  must_show("made/coin", tr(STR_W_COIN));
  tap_str(STR_C_BACK, 3, 8);
  kiss_seed_set_source(WSEED_SRC_DICE);             // back to the truth
  tap_str(STR_W_RNG_T, 3, 8);         // RANDOMNESS AUDIT row -> intro
  save("/tmp/sim_rng_intro.ppm");                   // NOISE row + the why pair
  must_show("rng/provenance", tr(STR_W_RNG_ON));
  tap_str(STR_W_RNG_GO, 3, 8);        // START -> piles fill on an 80ms timer
  pump(40);                            // ~8 ticks in: partial piles, tally live
  save("/tmp/sim_rng_run.ppm");                     // mid fill, fair line crossed
  pump(180);                           // past the 32 ticks the fill needs
  save("/tmp/sim_rng_result.ppm");                  // EVEN + the golden score
  must_show("rng/verdict", tr(STR_W_RNG_EVEN));
  must_show("rng/pass-note", tr(STR_W_RNG_PASS_NOTE));
  must_show("rng/score", "105.920");                // pinned by test_rngq.c
  // The two states an honest chip shows once in 500 runs, rigged: no walk
  // could wait for them and no gate would forgive a flaky needle.
  kiss_rngaudit_sim_result(+1);
  save("/tmp/sim_rng_uneven.ppm");                  // sawtooth, 800.000, retry line
  must_show("rng/uneven", tr(STR_W_DICE_UNEVEN));
  kiss_rngaudit_sim_result(-1);
  save("/tmp/sim_rng_tooeven.ppm");                 // the flat comb, 0.000
  must_show("rng/tooeven", tr(STR_W_RNG_TOOEVEN));
  tap_str(STR_C_DONE, 3, 12);         // DONE -> back to Settings (done cb)

  // The refusal render: the state a device that skipped kiss_trng_start is
  // in. NO SOURCE in the provenance row, the right block carries the refusal
  // and there is no START to tap.
  s_sim_trng = false;
  set_tab(SET_SECURITY);
  def_row(3, 2);                      // Audit -> the chooser
  tap_str(STR_W_RNG_T, 3, 8);         // RANDOMNESS AUDIT row -> intro
  save("/tmp/sim_rng_nosource.ppm");                // refusal: no START action
  must_show("rng/nosource", tr(STR_W_RNG_OFF));
  tap_str(STR_C_BACK, 3, 8);          // BACK -> Settings (done cb)
  s_sim_trng = true;

  set_tab(SET_SECURITY);
  def_row(3, 0);                                    // Duress -> the two ways in
  save("/tmp/sim_settings_duress.ppm");             // chips + the rule, in words
  // The chip states the RULE, not a chosen mark. Twenty locales still said
  // "YOUR STROKE" here long after the picker was deleted, which is exactly
  // what an owner reads as "a swipe I must have set somewhere".
  must_show("waysin/rule", tr(STR_GD_PICK_REAL_T));
  // The path no walk stop had ever taken: this action is the only route from
  // Settings into the duress wizard, and on a session with no passphrase it is
  // the only route to CREATE PASSPHRASE. It survived being mislabelled for
  // exactly that reason.
  tap_str(STR_GD_SET_BTN, 3, 20);      // HOW IT WORKS -> the wizard
  save("/tmp/sim_waysin_how.ppm");                  // the rule, at length
  must_show("waysin/how", tr(STR_GD_PICK_REAL_T));  // the same rule, same words
  tap_str(STR_GD_SKIP, 3, 20);         // NOT NOW -> Settings
  set_tab(SET_SECURITY);
  def_row(3, 0);                                    // Duress -> the two ways in
  tap_str(STR_GD_WORD_PILL, 3, 8);     // open custom letters
  save("/tmp/sim_gword_write.ppm");                 // blank field, no printed word
  draw_own_letters();
  // The ink itself, mid-enrolment and before DONE clears it. The strokes now
  // share one point pool with a per-stroke offset instead of a 12x384
  // rectangle, and a pool wired up wrong draws the right number of lines from
  // the wrong slices -- which every later frame here would still call correct,
  // because they photograph screens the ink is already gone from.
  save("/tmp/sim_gword_ink.ppm");                  // multi-stroke ink, as drawn
  tap_str(STR_GD_WORD_DONE, 3, 8);     // DONE (492..752) -> once more
  save("/tmp/sim_gword_again.ppm");                 // ONCE MORE, field cleared
  draw_own_letters();
  tap_str(STR_GD_WORD_DONE, 3, 8);     // DONE -> the stop screen
  save("/tmp/sim_gword_confirm.ppm");               // KISS WILL STOP WORKING
  slide_fire(STR_GD_WORD_HOLD);        // SLIDE TO CHANGE (422..752)
  save("/tmp/sim_gword_done.ppm");                  // the two ways in, redrawn
  tap_str(STR_C_OK, 3, 8);     // OK (552..752) -> Settings
  set_tab(SET_SECURITY);
  def_row(3, 0);                                    // Duress again
  save("/tmp/sim_settings_duress_set.ppm");         // the chip now reads LETTERS
  // Put KISS back before anything else in this walk draws it. gw_stored_set
  // is what BACK TO KISS calls, and leaving the owner's letters in place here
  // would make every later gesture in the walk stop working -- silently, on a
  // screen that still looks right.
  tap_str(STR_GD_WORD_PILL, 3, 8);     // open custom letters
  tap_str(STR_GD_WORD_BACK_T, 3, 8);     // BACK TO KISS (210..470)
  // The NO UNDO tab: one card, its reason at length, and one button. This tap
  // was a coordinate in the old right column and it has been wrong twice --
  // once at 356, which landed on the full width duress row and walked every
  // frame below through the duress screens while still being called
  // sim_wipe_*, and once at 220, which was only right until the column moved
  // again. It is a named row on a named tab now.
  //
  // The row IS the erase confirm: the chooser that stood between them offered
  // a second door to the same room, and both ended at the same whole partition
  // erase.
  set_tab(SET_NOUNDO);
  touch(200, SET_ROW_Y(3)); pump(3); release(); pump(8);  // ERASE SEED WORDS
  lv_refr_now(NULL); pump(2);
  save("/tmp/sim_wipe_confirm.ppm");                // fingerprint, the pair, HOLD
  must_show("erase/title", tr(STR_G_WIPEC_T));
  // 9A2C33E3 is the stub fingerprint of nine 'a's, the shortest passphrase
  // the wizard accepts now. It is a literal because the point of the stop is
  // that the erase screen names WHICH keys before the hold, so it has to be
  // the value the walk actually committed and not whatever is on screen.
  must_show("erase/fingerprint", "9A2C33E3");       // WHICH keys, before the hold
  // a tap is NOT enough: press, release early, nothing must happen
  tap_str(STR_G_HOLD_WIPE, 2, 4);
  save("/tmp/sim_wipe_tap_noop.ppm");               // still the confirm screen
  // The failure first, through the seam, because it is the branch nothing
  // else can reach: the erase REFUSES, the outcome screen says so with the
  // amber lamp and keeps "do not sell or give it away" whole, and BACK
  // returns to the gate for the retry the headline names.
  s_sim_wipe_fail = 1;
  // OUT and BACK. The erase gate takes double travel in place of the 2000ms
  // hold it used to take, so one full stroke arrives at ONCE MORE and commits
  // nothing.
  slide_at(208, 430, 340); release(); pump(8);
  must_show("erase/once more", tr(STR_GD_DRAW_AGAIN_T));
  save("/tmp/sim_wipe_once_more.ppm");              // knob parked, fill spent
  slide_back(348, 430, 340); release(); pump(10);    // the return leg -> refusal
  save("/tmp/sim_wipe_fail.ppm");
  must_show("erase/fail headline", tr(STR_G_NOERASE_NEXT));
  tap_str(STR_C_BACK, 3, 10);                       // -> the gate again
  // The slide sits on the action band (48..378 x WT_ACTION_Y_SLIDE), not on an
  // overlay at 372: the confirmation IS the screen, so it uses the band every
  // other screen puts its actions on. There is no time in this gesture at all
  // any more -- only distance, twice.
  slide_at(208, 430, 165);                          // ~half way: the fill sweeps
  lv_refr_now(NULL);
  save("/tmp/sim_wipe_holding.ppm");                // partial red fill, not fired
  slide_go(340); release(); pump(8);                // leg one arrives
  slide_back(348, 430, 340); release(); pump(6);     // leg two -> erased
  save("/tmp/sim_wiped.ppm");                       // SEED WORDS ERASED + two ways off
  must_show("erased", tr(STR_G_ERASED_T));
  // NEW SEED WORDS sits beside OK: erasing in order to make new ones is one
  // tap now, which is the half the deleted chooser used to carry.
  // An action, so find it by label. "NEW SEED WORDS" is the English value of
  // three keys (W_CREATE_NEW, W_NEW_T, W_CHOOSE_NEW) and matches any label
  // carrying those words on any screen.
  if (!act_for(STR_W_CHOOSE_NEW, "erased offers")) { /* counted */ }
  tap_str(STR_C_OK, 3, 130);   // OK -> menu
  save("/tmp/sim_wiped_menu.ppm");                  // must be the game MENU

  // step 10: AMNESIC mode — nothing is stored, so the KISS gesture lands on
  // LOAD YOUR WALLET instead of the wizard, and an encrypted backup is a valid
  // way in.
  // the wipe above already left us locked on the game cover with no seed
  kiss_seed_set_mode(WSEED_MODE_AMNESIC);
  for (int i = 0; i <= 9; i++) { touch(140, 120 + i * 20); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 - i * 13); pump(1); } release(); pump(2);
  for (int i = 0; i <= 6; i++) { touch(140 + i * 15, 210 + i * 15); pump(1); } release(); pump(2);
  for (int i = 0; i <= 8; i++) { touch(285, 130 + i * 21); pump(1); } release(); pump(2);
  touch(420, 140); pump(1); touch(360, 152); pump(1); touch(345, 188); pump(1); touch(400, 212); pump(1);
  touch(422, 250); pump(1); touch(362, 286); pump(1); touch(342, 272); pump(1); release(); pump(2);
  touch(540, 140); pump(1); touch(480, 152); pump(1); touch(465, 188); pump(1); touch(520, 212); pump(1);
  touch(542, 250); pump(1); touch(482, 286); pump(1); touch(462, 272); pump(1); release(); pump(4);
  save("/tmp/sim_amnesic_load.ppm");                // LOAD YOUR WORDS

  // A bare seed square, which this door used to swallow whole. Twelve words in
  // plain text is exactly what a SeedQR decodes to, so this is the removal
  // itself under test: the scan must land on NOT A BACKUP and stage nothing.
  touch(218, 290); pump(3); release(); pump(6);     // SCAN LOCKED QR (action at y=264)
  {
    static const char *SQ =
        "apple bridge candle dragon eagle forest "
        "garden hammer island jungle kettle ladder";
    kiss_scan_inject(SQ, strlen(SQ));
  }
  pump(6);
  save("/tmp/sim_amnesic_qrbad.ppm");               // NOT A BACKUP, nothing loaded
  must_show("seed square refused", tr(STR_W_QRBAD_T));
  tap_str(STR_C_TRY_AGAIN, 3, 6);     // TRY AGAIN -> load screen

  // An encrypted backup in a mode this signer refuses (CTR, version 15):
  // recognized as KEF and refused BEFORE any password is asked for. The
  // envelope is built with the same kef_emit_header the firmware uses.
  touch(218, 290); pump(3); release(); pump(6);     // SCAN LOCKED QR
  {
    uint8_t fx[64];
    size_t h = kef_emit_header(fx, sizeof fx, (const uint8_t *)"id", 2, 15, 10);
    for (int i = 0; i < 24; i++) fx[h + i] = (uint8_t)i;  // iv12+ct8+auth4 shape
    kiss_scan_inject((const char *)fx, h + 24);
  }
  pump(8);
  save("/tmp/sim_kef_badver.ppm");                  // LOCKED BACKUP, no prompt
  must_show("kef refused version", tr(STR_W_KEF_BAD_T));
  tap_str(STR_C_TRY_AGAIN, 3, 6);     // TRY AGAIN -> load screen

  // The locked backup, end to end through the card: picker, one wrong
  // password (the single vague failure), then the right one — and the load
  // continues into the same passphrase screen a scanned seed reaches. This
  // section is the one place the walk can drive the KEF keyboard: no login
  // has ever opened here, so kiss_ui_active() is genuinely false (the step
  // 14 tail cannot say that, which is why it stops at the intro action).
  {
    uint8_t fx[64];
    size_t h = kef_emit_header(fx, sizeof fx, (const uint8_t *)"73C5DA0A", 8,
                               KEF_VERSION_AES_GCM, KEF_ITER_STORED);
    for (int i = 0; i < 32; i++) fx[h + i] = (uint8_t)(0xa5 ^ i);
    FILE *f = sd_fopen("73C5DA0A.kef", "wb");
    if (f) { fwrite(fx, 1, h + 32, f); fclose(f); }
  }
  tap_str(STR_S_FROM_SD, 3, 8);                     // FROM SD CARD -> picker
  save("/tmp/sim_kef_pick.ppm");
  touch(400, 144); pump(3); release(); pump(8);     // the one .kef row
  save("/tmp/sim_kef_open.ppm");                    // BACKUP PASSWORD keyboard
  touch(664, 278); pump(3); release(); pump(3);     // k
  touch(201, 202); pump(3); release(); pump(3);     // e ("ke": wrong on purpose)
  touch(725, 430); pump(3); release(); pump(40);    // OK -> the one vague failure
                                                    // (40: outlive the key pop)
  save("/tmp/sim_kef_open_bad.ppm");
  must_show("kef vague failure", tr(STR_L_KEF_BAD));
  touch(664, 278); pump(3); release(); pump(3);     // k
  touch(201, 202); pump(3); release(); pump(3);     // e
  touch(312, 278); pump(3); release(); pump(3);     // f
  touch(725, 430); pump(3); release(); pump(20);    // OK -> unlocked -> login
  save("/tmp/sim_amnesic_pass.ppm");                // straight to the passphrase

  // a passphrase can come from a QR too, behind one warning screen
  touch(596, 38); pump(3); release(); pump(6);      // SCAN
  save("/tmp/sim_amnesic_ppwarn.ppm");              // PASSPHRASE FROM A QR
  tap_str(STR_L_SCAN_GO, 3, 6);     // SCAN IT (rightmost now) -> camera
  kiss_scan_inject("correct horse battery staple correct horse battery "
                     "staple correct horse battery staple xyz", 90);
  pump(6);
  touch(696, 38); pump(3); release(); pump(4);      // SHOW
  save("/tmp/sim_kb_show_long.ppm");                // 90 chars, wrapped not "..."
  for (int i = 0; i < 90; i++) { touch(752, 355); pump(1); release(); pump(1); }
  type_pass9();                                     // the wallet's passphrase
  touch(725, 430); pump(3); release(); pump(25);    // OK -> fingerprint
  tap_str(STR_L_TAP_TO_OPEN, 3, 140);   // TAP TO OPEN -> home
  save("/tmp/sim_amnesic_home.ppm");                // an amnesic wallet, unlocked

  // Move the live RAM wallet to SD, lock, then remove the card. KISS must land
  // on INSERT WALLET SD CARD -- never on first-boot setup. A failed retry stays
  // there; reinserting the card advances to the ordinary passphrase screen.
  kiss_seed_move_to(WSEED_MODE_SD);
  s_sim_sd_present = 0;
  touch(44, 44); pump(3); release(); pump(20);      // explicit lock -> game
  draw_cover();
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
  kiss_duress_ui_open_nopass(lv_screen_active(), NULL);
  pump(40);
  save("/tmp/sim_duress_nopass.ppm");               // NOTHING TO HIDE BEHIND

  // The commit that could not finish. A KEEP wallet was replacing another and
  // the replacement may have taken it, so the staged words in RAM are the last
  // copy there is. fp_tap_cb used to answer that with the generic setup STOP,
  // whose body reads "nothing was saved. start again from the menu" -- the one
  // action that destroys what is left.
  //
  // A leaf, opened directly, like the two duress stops above it. Driving it
  // through a real failed commit mid-setup wedged the walk: setup never
  // completes, so every tap after it lands on the wrong screen.
  kiss_ui_test_recover_screen();
  pump(40);
  save("/tmp/sim_ui_recover.ppm");                  // words held, paper first
  must_show("recover", tr(STR_L_RECOVER_T));
  // No longer a leaf that never leaves: the screen has its own registry row
  // now and holds the idle lock off, so left open it would hold it off for
  // the teardown contract checks at the end of the walk too.
  kiss_ui_test_recover_close();
  pump(40);

  // The other half of the same question, and the half that was still lying.
  // YOUR LETTERS ARE SET drew "letters -> SPARE" and "letters + mark -> REAL"
  // on every wallet, including one with no passphrase, where the letters alone
  // The post-setup warning screen in the two STATES no walk ever reached: no
  // passphrase, and a fingerprint that failed to derive. Between them they hid
  // two faults for the life of the screen -- an impossible "TYPE THE EXACT
  // BACKUP PASSPHRASE" on keys that have none, and 00000000 rendered into a
  // value card captioned FINGERPRINT, a code that looks real and was about to
  // be copied onto paper.
  //
  // Neither was a missing screen. check_screen_coverage was green throughout,
  // because the screen HAS a stop -- in the with-passphrase, fingerprint-known
  // combination. A screen is not covered until its branches are.
  {
    (void)kiss_session_open(NULL);                  // no passphrase = the decoy
    kiss_ui_sim_warn_screen(false, true);           // unverified, no fingerprint
    pump(8);
    save("/tmp/sim_warn_nopass_nofp.ppm");          // chip alone, NO value card
    kiss_ui_sim_warn_screen(true, false);           // verified, fingerprint back
    pump(8);
    save("/tmp/sim_warn_verified.ppm");             // green chip beside the card
    // The screen owns itself; reopening it deletes the previous one, and the
    // duress excursion below opens a session of its own straight after.
    kiss_ui_sim_warn_screen(false, false);
    pump(8);
  }

  // open the funded wallet and there is no spare to reach. The stroke wizard
  // has refused to describe this signer that way since ST_NOPASS existed; the
  // word wizard never had the guard, so an owner could be told to hand over a
  // word that opens everything.
  //
  // A decoy session is what makes the difference visible: kiss_session_decoy
  // is the same predicate Settings uses to pick between the two stroke
  // wizards. NULL, not "", is what marks a session as the decoy.
  {
    (void)kiss_session_open(NULL);                // no passphrase = the decoy
    kiss_word_ui_open(lv_screen_active(), NULL);
    pump(20);
    draw_own_letters();
    tap_str(STR_GD_WORD_DONE, 3, 8);   // DONE -> once more
    draw_own_letters();
    tap_str(STR_GD_WORD_DONE, 3, 8);   // DONE -> the stop screen
    slide_fire(STR_GD_WORD_HOLD);      // SLIDE TO CHANGE
    save("/tmp/sim_gword_done_nopass.ppm");         // PASSPHRASE -> NOT SET, no SPARE
    tap_str(STR_C_OK, 3, 8);   // OK
    // Put KISS back and drop the session, for the reason the walk restores it
    // after the passphrase run: letters left stored here would silently break
    // every gesture drawn after this point.
    (void)gw_stored_set(NULL);
    kiss_session_close();
  }

  // The write that does not take. Both callers threw gw_stored_set's result
  // away and showed the success screen regardless, so an owner could hold to
  // change their word, read YOUR LETTERS ARE SET, and still be opening the
  // device with KISS -- the one failure they cannot see for themselves, because
  // the device keeps working until the day the new word is needed.
  //
  // Needs the seam: real NVS cannot be told to refuse a write, and a screen the
  // walk cannot reach is a screen no locale was ever measured in.
  {
    gw_test_fail_next_set();
    kiss_word_ui_open(lv_screen_active(), NULL);
    pump(20);
    draw_own_letters();
    tap_str(STR_GD_WORD_DONE, 3, 8);   // DONE -> once more
    draw_own_letters();
    tap_str(STR_GD_WORD_DONE, 3, 8);   // DONE -> the stop screen
    slide_fire(STR_GD_WORD_HOLD);      // SLIDE -> the write fails
    save("/tmp/sim_gword_failed.ppm");              // THAT WAS NOT IT, nothing saved
    tap_str(STR_C_OK, 3, 8);   // OK
    if (gw_stored_any()) {
      printf("FAIL: a refused write left a word stored\n");
      return 1;
    }
    printf("ok: a refused write says so and stores nothing\n");
  }


  // ST_INTRO again, and NOT the one the setup walk photographed. Reached from
  // Settings on a wallet that already has a stroke, this screen grows a THIRD
  // action -- TURN THIS OFF, the only way back to plain behaviour -- and the
  // setup walk can never show it, because during setup there is nothing to
  // turn off yet. That is the whole reason the row overlapped by 8px for as
  // long as it did: no stop had ever contained all three actions at once.
  //
  // A leaf, like the nopass stop above it: opened directly, nothing after it.
  (void)kiss_duress_set(WDG_UNDERLINE);
  kiss_duress_ui_open(lv_screen_active(), NULL);
  pump(40);
  save("/tmp/sim_duress_intro_set.ppm");            // three actions, one TALL row

  // ---- firmware from the SD card -------------------------------------------
  // Leaves, opened directly, for the reason the duress stops above are: the
  // route in is a row on Settings' DEVICE tab and every screen past the
  // first needs state a desktop build does not have.
  //
  // Without kiss_fw_test_* the sim can only ever reach "cannot be checked":
  // there is no flash and no signing key here, so the value card, the four
  // fact rows, the hold and the writing screen would be shapes no gate had
  // ever measured, in any locale. That is exactly the hole BARE and WALL exist
  // to catch, so the seam is what makes their verdict on these screens mean
  // anything.
  {
    // A real app descriptor: 0xE9 image magic, then ABCD5432 at offset 32 with
    // a version far ahead of any VERSION file, so the scan reads it as newer.
    //
    // 512 + a signature trailer, because a real update .bin is the image
    // followed by KISS_PQSIG_TRAILER_LEN bytes and kiss_fw_scan now says so: a
    // file no bigger than its own trailer has no image under it and comes back
    // as "not firmware". A 512 byte fixture stopped being an image the day the
    // second signature landed, and every screen below it would have gone with
    // it -- silently, since a walk that never reaches a screen still saves a
    // frame of whatever else is up.
    static unsigned char img[512 + 8192];
    memset(img, 0, sizeof img);
    img[0] = 0xE9;
    img[32] = 0x32; img[33] = 0x54; img[34] = 0xCD; img[35] = 0xAB;
    memcpy(img + 32 + 16, "99.0.0", 6);
    memcpy(img + 32 + 48, "kiss", 4);
    FILE *fw = sd_fopen("kiss-signer-99.0.0.bin", "wb");
    if (fw) { fwrite(img, 1, sizeof img, fw); fclose(fw); }
  }

  // Every screen on this chain arrives: the trade block, each line and each
  // rule animate in, and the last rule is still drawing at 662ms. 60 frames is
  // 960ms, which is the first count that photographs a settled page rather
  // than one mid flight -- at 20 the rules were simply absent from the frame,
  // in every locale, and nothing said so.
#define FW_SETTLE 60

  // 1. the state a build without the release key reaches: one claim, one line.
  kiss_fw_test_set_available(WFW_ERR_UNSIGNED);
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  save("/tmp/sim_fw_unsigned.ppm");                 // cannot be checked + where it goes

  // 2. the ordinary one: the trade as the headline, two lines, INSTALL.
  kiss_fw_test_set_available(WFW_OK);
  kiss_fw_test_set_install(WFW_OK, 4);
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  save("/tmp/sim_fw_found.ppm");                    // 0.1.0 -> 99.0.0, NEWER
  must_show("fw/newer lamp", tr(STR_G_FW_NEWER_LAMP));

  // The signature line's "?". The whole LINE is the target and the mark is
  // only the sign that says so, so this taps the middle of the row: x is the
  // pane's own centre and y is the second line's top plus half its height,
  // neither of which a translation can move.
  touch(400, 351); pump(3); release(); pump(20);
  save("/tmp/sim_fw_sig_help.ppm");                 // what a signature buys, in prose
  tap_str(STR_C_OK, 3, 8);             // OK closes the card

  tap_str(STR_G_FW_INSTALL, 3, FW_SETTLE);   // INSTALL -> confirm
  save("/tmp/sim_fw_confirm.ppm");                  // the why/risk pair + hold rule

  // The hold, part drawn. The pill is gone and the progress is a fill running
  // along a 330x2 rule, which is the one part of this redesign that only a
  // frame taken DURING the gesture can show -- at rest and at done it is a
  // plain rule either way. 45 frames is 720ms of a 1500ms hold, so the fill is
  // about half across and the label reads KEEP HOLDING.
  {
    lv_obj_t *hp = act_for(STR_G_FW_HOLD, "mid-slide");
    if (hp) {
      lv_area_t a; lv_obj_get_coords(hp, &a);
      slide_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, 165);
      save("/tmp/sim_fw_hold_mid.ppm");             // fill part way, KEEP SLIDING
      must_show("fw/holding", tr(STR_G_FW_KEEP_HOLDING));
      release();
      // THE PAUSE WINDOW. A lift short of the end banks the travel for 800ms
      // instead of throwing it away, and the label stops instructing and
      // starts offering. 6 frames is 96ms in, well inside it.
      pump(6);
      save("/tmp/sim_fw_slide_paused.ppm");         // fill held, KEEP GOING
      // Pinned by two negatives, because KEEP GOING is the passphrase
      // keyboard's word too and a shared needle passes on either screen. The
      // pause is the state where the label is NEITHER the instruction it gave
      // mid-drag nor the word it rests at, and nothing else on the device is.
      must_not_show("fw/slide paused mid-drag", tr(STR_G_FW_KEEP_HOLDING));
      must_not_show("fw/slide paused at rest", tr(STR_G_FW_HOLD));
      // ...and 62 more is 992ms, past the window, so the fill runs back and
      // the label is the screen's own word again before anything looks for it.
      pump(62);
      must_show("fw/slide let go", tr(STR_G_FW_HOLD));
    }
  }

  // Hold well past the 1500ms rather than to the frame it completes on. 95 was
  // 1520ms against a 1500ms hold, and the indev samples the press about every
  // 30ms -- so the press lands one or two frames after the touch, and whether
  // 20ms of margin survives depends on the sampling PHASE, which is set by how
  // many frames the whole walk has pumped before arriving here. Adding stops
  // anywhere earlier moved it, the hold stopped completing, and the two frames
  // below photographed a half filled pill under the names WRITING and
  // FIRMWARE REPLACED. The window this needs to land in is not tight: WRITING
  // stays up FW_LIT_MS (1200ms, 75 frames) before the result replaces it, and
  // the write itself is deferred 30ms behind it.
  slide_fire(STR_G_FW_HOLD);
  // 55 frames into the 1200ms screen, which is where the light band is near
  // the top of its breath. Saved on frame 0 it is at the BOTTOM: bg_opa 64
  // against stop opacities of 26 and 5 quantises a 704px ramp into a single
  // 4-level step, and the frame shows a hard edged rectangle over the left
  // half rather than a wash. Nothing is wrong with the band; the walk was
  // photographing the one moment it cannot be seen.
  //
  // The 55 comes back off the pump below rather than being added to the walk.
  // Total frames through this block is what sets the indev sampling phase for
  // every hold after it, and the comment above is the account of what moving
  // that costs.
  pump(55);
  save("/tmp/sim_fw_writing.ppm");                  // the DARK -> DONE band + the pair
  // 100, not 20. The install is deferred FW_LIT_MS (1200ms, 75 frames) behind
  // the screen that announces it, so the panel can go dark on purpose rather
  // than mid-paint. It used to be one LVGL tick, and 20 frames cleared that
  // easily; at 75 the walk was still on WRITING when it saved the frame it
  // calls sim_fw_done, and check_sim_taps rightly called the two identical.
  pump(45);   // 55 of the original 100 are spent above, waiting for the band
  must_show("fw/replaced", tr(STR_G_FW_OK_T));   // the screen, not a hopeful name
  save("/tmp/sim_fw_done.ppm");                     // FIRMWARE REPLACED + RESTART

  // 3. the refusal that matters most, on the same route: a signature that did
  // not check out has to read as "nothing was written", not as a vague error.
  kiss_fw_test_set_install(WFW_ERR_REJECTED, 2);
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  tap_str(STR_G_FW_INSTALL, 3, FW_SETTLE);   // INSTALL -> confirm
  // 110 pumps, same as the replaced-firmware hold above: 95 was enough only
  // at one indev phase, and any walk insertion upstream shifts the phase —
  // at the wrong one the hold never completed, nothing installed, and this
  // save quietly photographed the confirm screen instead.
  slide_fire(STR_G_FW_HOLD);           // the slide completes, WRITING announces
  pump(100);                           // deferred refusal lands, as above
  save("/tmp/sim_fw_rejected.ppm");    // NOT INSTALLED, in WT_STOP
  must_show("fw/rejected", tr(STR_G_FW_FAIL_T));

  // 3a. the OTHER refusal, which reads the same and means something different.
  //
  // An image that fails here passed the secp256r1 check -- it really is a KISS
  // release, signed with the release key -- and was still refused, because the
  // post quantum signature over the same bytes was missing or wrong. Every
  // release from before the trailer existed lands on this screen, so it is the
  // one an owner is most likely to meet, and "the signature did not check out"
  // would send them hunting for a corrupt download.
  kiss_fw_test_set_install(WFW_ERR_PQ_REJECTED, 2);
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  tap_str(STR_G_FW_INSTALL, 3, FW_SETTLE);
  slide_fire(STR_G_FW_HOLD);
  pump(100);
  save("/tmp/sim_fw_pq_rejected.ppm");
  must_show("fw/pq_rejected", tr(STR_G_FW_FAIL_T));

  // 3b. a card holding more images than the scan opens.
  //
  // kiss_fw_scan reads descriptors for the first WFW_SCAN_MAX names and the
  // lister hands them over sorted, so past that the screen's answer is about a
  // SUBSET chosen by filename -- and it used to be silent about it. This frame
  // is the harm end to end: thirty 0.0.1 images sorting ahead of the real
  // 99.0.0 one fill the window, the device never opens the file the owner came
  // here to install, and what it offers instead is a genuine older build the
  // signature check has no reason to object to.
  //
  // 30, not WFW_SCAN_MAX + 1: the window has to be filled ENTIRELY by decoys
  // for the real image to disappear, which is the case worth photographing.
  {
    static unsigned char pad[512 + 8192];   // image + trailer, as above
    memset(pad, 0, sizeof pad);
    pad[0] = 0xE9;
    pad[32] = 0x32; pad[33] = 0x54; pad[34] = 0xCD; pad[35] = 0xAB;
    memcpy(pad + 32 + 16, "0.0.1", 5);
    memcpy(pad + 32 + 48, "kiss", 4);
    for (int i = 0; i < 30; i++) {
      char p[320];
      snprintf(p, sizeof p, "%s/a-crowd-%02d.bin", SIMSD, i);
      FILE *f = fopen(p, "wb");
      if (f) { fwrite(pad, 1, sizeof pad, f); fclose(f); }
    }
  }
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  save("/tmp/sim_fw_crowded.ppm");
  {
    // Counted, not assumed. Earlier steps of this same walk can leave their
    // own .bin on the card, so a hard 31 here passed only for as long as
    // nothing upstream wrote another file, and the first thing it did was
    // fail on a screen that was completely correct.
    int bins = 0;
    DIR *cd = opendir(SIMSD);
    if (cd) {
      struct dirent *de;
      while ((de = readdir(cd)) != NULL) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] != '.' && l > 4 &&
            strcasecmp(de->d_name + l - 4, ".bin") == 0)
          bins++;
      }
      closedir(cd);
    }
    char want[96];
    snprintf(want, sizeof want, tr(STR_S_FILES_MORE_FMT), WFW_SCAN_MAX, bins);
    must_show("fw/narrowed scan", want);
    if (bins <= WFW_SCAN_MAX) {
      printf("FAIL: fw/narrowed scan: only %d .bin on the card, the window is "
             "%d, so this stop proves nothing\n", bins, WFW_SCAN_MAX);
      g_walk_fails++;
    }
  }
  for (int i = 0; i < 30; i++) {
    char p[320];
    snprintf(p, sizeof p, "%s/a-crowd-%02d.bin", SIMSD, i);
    unlink(p);
  }

  // 4. the offer going BACKWARDS, and then the four refusals nothing had ever
  // photographed.
  //
  // All five are reached by writing a different fixture rather than by a new
  // seam. kiss_fw_scan decides all of this from what is actually on the card,
  // so a seam would be testing the seam; and the downgrade branch in
  // particular is the one state overlapcheck HAS to see, because it is the
  // only one carrying the caution line at 208 and it collided with the rule
  // under it before that line was pinned.
  //
  // A helper rather than five copies: the descriptor is 0xE9, the ABCD5432
  // magic at 32, a version at 48 and a project at 80, and only the version
  // ever changes.
  sd_unlink("kiss-signer-99.0.0.bin");
  {
    static unsigned char fx[512 + 8192];
    memset(fx, 0, sizeof fx);
    fx[0] = 0xE9;
    fx[32] = 0x32; fx[33] = 0x54; fx[34] = 0xCD; fx[35] = 0xAB;
    memcpy(fx + 32 + 48, "kiss", 4);

    // 4a. older. The lamp says OLDER in amber, the arrow and the version go
    // amber with it, and the caution line appears.
    memcpy(fx + 32 + 16, "0.0.1", 6);
    FILE *f = sd_fopen("kiss-signer-0.0.1.bin", "wb");
    if (f) { fwrite(fx, 1, sizeof fx, f); fclose(f); }
    kiss_fw_ui_open(lv_screen_active(), NULL);
    pump(FW_SETTLE);
    save("/tmp/sim_fw_older.ppm");                  // OLDER + the one line note
    must_show("fw/older lamp", tr(STR_G_FW_OLDER_LAMP));
    must_show("fw/older note", tr(STR_G_FW_DOWN_SHORT));
    tap_str(STR_G_FW_INSTALL, 3, FW_SETTLE);        // the confirm it warns on
    save("/tmp/sim_fw_confirm_down.ppm");           // both rules amber
    must_show("fw/down claim", tr(STR_G_FW_DOWN_H));
    sd_unlink("kiss-signer-0.0.1.bin");

    // 4b. already running. The one refusal that is not a fault: accent and a
    // tick, where the other four wear WT_WARN and a warning triangle.
    memset(fx + 32 + 16, 0, 32);
    snprintf((char *)fx + 32 + 16, 32, "%s", kiss_fw_running_version());
    f = sd_fopen("kiss-signer-same.bin", "wb");
    if (f) { fwrite(fx, 1, sizeof fx, f); fclose(f); }
    kiss_fw_ui_open(lv_screen_active(), NULL);
    pump(FW_SETTLE);
    save("/tmp/sim_fw_same.ppm");                   // already running, in the accent
    must_show("fw/same", tr(STR_G_FW_SAME_H));
    sd_unlink("kiss-signer-same.bin");

    // 4c. too big for the receiving slot. Sparse: the descriptor is real and
    // the length is what kiss_fw_scan measures, so one byte past the end is
    // enough and eight megabytes of writes is not.
    memcpy(fx + 32 + 16, "98.0.0", 7);
    f = sd_fopen("kiss-signer-98.0.0.bin", "wb");
    if (f) {
      fwrite(fx, 1, sizeof fx, f);
      fseek(f, 0x800000 + 8192, SEEK_SET);
      fputc(0, f);
      fclose(f);
    }
    kiss_fw_ui_open(lv_screen_active(), NULL);
    pump(FW_SETTLE);
    save("/tmp/sim_fw_toobig.ppm");                 // a fact about this device
    must_show("fw/too big", tr(STR_G_FW_BIG_H));
    sd_unlink("kiss-signer-98.0.0.bin");

    // 4d. a .bin that is not an image at all. No magic, so the scan reads
    // every name and comes back with nothing it can judge.
    f = sd_fopen("kiss-signer-junk.bin", "wb");
    if (f) { fwrite("not an image at all", 1, 19, f); fclose(f); }
    kiss_fw_ui_open(lv_screen_active(), NULL);
    pump(FW_SETTLE);
    save("/tmp/sim_fw_notfirmware.ppm");            // not firmware
    must_show("fw/not firmware", tr(STR_G_FW_BAD_H));
    sd_unlink("kiss-signer-junk.bin");
  }

  // 4e. a card with no image on it, which is not the same answer as no card.
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  save("/tmp/sim_fw_nofile.ppm");                   // nothing to install
  must_show("fw/no file", tr(STR_G_FW_NOFILE_H));

  // 5. no card at all: the same shape again, different left hand claim.

  platform_sd_test_set_present(0);
  kiss_fw_ui_open(lv_screen_active(), NULL);
  pump(FW_SETTLE);
  save("/tmp/sim_fw_nocard.ppm");
  platform_sd_test_set_present(1);
  kiss_fw_test_set_available(WFW_ERR_UNSIGNED);   // leave the seam as found

  // A sign screen that was replaced without being deleted stays parented under
  // its replacement, invisible, until a BACK peels the top one off and drops
  // the owner back on a transaction they already left. No saved frame shows it
  // -- the walk's own diff between a good build and a leaking one was byte
  // identical across all 177 frames -- so kiss_sign.c counts it instead and
  // this is where the count is answered.
  // The firmware screen and the idle auto-lock. Before kiss_fw_ui_close
  // existed, main.c's lock had no handle on this screen at all: it is parented
  // to the active screen rather than the wallet container the lock hides, so it
  // stayed lit on top of a locked device, and its BACK rebuilt Settings with
  // RECOVERY WORDS one row in -- words that unseal with the DEVICE key in KEEP
  // and SD modes, which locking does not wipe. Two facts, both required:
  // closing must drop the screen, and it must not hand control back the way
  // BACK does.
  {
    // Drain first. The hold on the rejected-install step above arms install_now
    // FW_LIT_MS later, and that timer was still pending here -- it fired inside
    // this block's own pump and rebuilt the screen through result_screen, which
    // reads exactly like the leak this is looking for. Settle before measuring.
    pump(200);
    kiss_fw_test_set_available(WFW_ERR_UNSIGNED);
    g_fw_done_fired = 0;
    kiss_fw_ui_open(lv_screen_active(), sim_fw_done_cb);
    pump(20);
    if (!kiss_fw_ui_active()) {
      printf("FAIL: fw screen not reported active while open\n");
      return 1;
    }
    kiss_fw_ui_close();                     // what the auto-lock branch calls
    pump(20);
    if (kiss_fw_ui_active()) {
      printf("FAIL: fw screen survived the auto-lock teardown\n");
      return 1;
    }
    if (g_fw_done_fired) {
      printf("FAIL: auto-lock teardown rebuilt Settings (done_cb fired %d)\n",
             g_fw_done_fired);
      return 1;
    }
    printf("ok: fw screen closes on lock without reopening settings\n");
  }

  // ---- step 14: RESTORE refusal, at the tail -----------------------------
  //
  // Step 7 now takes a clean restore through its single passphrase entry,
  // fingerprint, backup rehearsal, warning and home handoff. Re-enter here for
  // the complementary refusal: twelve copies of one word must never reach that
  // passphrase flow. The clean fixture after START OVER proves the refusal did
  // not strand the recovery keyboard.
  //
  // LAST in the walk. It stores a seed and opens a wallet, so anywhere
  // earlier it rewrites the state every later step stands on -- tried after
  // the wipe first, and step 10 came back with three dead taps.
  //
  // Entered through kiss_begin_setup() rather than by drawing KISS on the
  // game cover. That is the Settings > REPLACE WALLET door, it runs the same
  // wizard with the same setup_done_login callback -- which is the whole
  // point, since that callback is what chooses the passphrase flow -- and it
  // does not care what is on screen. kiss_lock() would not serve here: it
  // returns early unless a wallet is open, and at the tail none is.
  // The FIRMWARE screen above is closed but still parented, and must_show
  // walks the whole tree -- so without this the restore assertions pass while
  // every save() photographs firmware. Blunt, and correct for a tail step that
  // owns the rest of the run.
  // The walk cold-booted the indev at step 12 (kiss_ui_drop_indev_for_test),
  // and the passphrase keyboard is the one screen that needs it back -- without
  // this the login builds and every key press lands on nothing.
  kiss_ui_ensure_indev();
  lv_obj_clean(lv_screen_active()); pump(4);
  kiss_seed_wipe();
  kiss_seed_set_mode(WSEED_MODE_KEEP);
  pump(10);
  kiss_begin_setup(); pump(20);                     // the wizard, on demand
  // W_SETUP_T is the chooser's own title. W_NEW_T was the first needle here and
  // it passed in English for the wrong reason -- W_CHOOSE_NEW carries the same
  // words in English and different ones in French, so only French reported it.
  must_show("restore/chooser", tr(STR_W_SETUP_T));
  // A restore that carries nothing, FIRST -- it is the same door and it must
  // shut before the one that opens. Twelve of one word is the shape the blind
  // draw already blocks; on this path it stands in for "abandon" x11 + about,
  // which the walk used to type here and which the gate now refuses. The judge
  // and the index stub are both real enough for this: same prefix, same index,
  // twelve times.
  touch(218, 240); pump(3); release(); pump(4);     // RESTORE FROM WORDS
  touch(174, 144); pump(3); release(); pump(4);     // FLASH
  touch(218, 176); pump(3); release(); pump(4);     // 12 WORDS
  for (int i = 0; i < 12; i++) restore_word("g");   // the same word, twelve times
  pump(6);
  save("/tmp/sim_restore_degen.ppm");               // CHECK YOUR WORDS, flat bars
  must_show("restore/degen", tr(STR_W_CARDS_BLOCK_B));
  must_show("restore/degen names the rule", tr(STR_W_CARDS_SAME_S));
  must_not_show("restore/degen offers no way past", tr(STR_L_USE_ANYWAY));
  // One way off it, the same one a broken checksum gets: this IS that screen,
  // wearing its second reason. START OVER goes back to the keyboard rather
  // than the chooser, so the count screen is not walked through again.
  tap_str(STR_W_START_OVER, 3, 6);     // START OVER -> empty keyboard

  // ...and now the shared clean fixture opens. Its prefixes select twelve
  // distinct, well-spaced SIM_WORDS and are the same ones the end-to-end
  // restore and optional backup rehearsal already typed.
  for (size_t i = 0; i < sizeof SIM_12_PREFIXES / sizeof SIM_12_PREFIXES[0]; i++)
    restore_word(SIM_12_PREFIXES[i]);
  pump(30);
  save("/tmp/sim_restore_ppintro.ppm");
  must_show("restore/ppintro", tr(STR_L_PPINTRO_HEAD));
  // The action must not tell someone whose passphrase already exists to
  // invent one. CREATE PASSPHRASE is the new-seed wording and on this path is
  // an instruction into a different set of keys; TYPE IT is right for both,
  // which is what retired the branch.
  if (!act_for(STR_L_PP_TYPE_IT, "restore offers")) { /* counted */ }
  must_not_show("restore/no create verb", tr(STR_L_CREATE_PASS_BTN));

  // This TAIL entry stops here deliberately. The keyboard past this action needs
  // a login teardown the tail of the walk cannot give -- kiss_login_open
  // returns early while kiss_ui_active(), so it builds nothing and the screen
  // goes blank. The full keyboard and commit path is covered at step 7, where
  // the first-boot lifecycle naturally supplies that teardown.
  //
  // The assertions that would have covered it were must_not_show(TYPE IT AGAIN)
  // and must_not_show(WEAK PASSPHRASE), and BOTH PASS ON A BLANK SCREEN. An
  // assertion that cannot fail is worse than none: it reports coverage of the
  // exact behaviour nobody checked. So they are gone rather than left green.
  //
  // What is covered HERE is real and rendered: the refusal, retry, passphrase
  // intro and its PASSPHRASE wording. Step 7 owns the single entry, fingerprint
  // and restored-wallet exit rather than implying them from this tail stop.
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
  // action label everywhere else on the device, so no real stop can ever have
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

  // LVGL heap watermark, at the END of the walk. The pool is 128K (matching
  // the device), and a failed lv_malloc during rendering is an LVGL assert,
  // which on the device is an infinite loop -- the i18n picker was the first
  // thing to blow the old 64K pool. This sample used to sit mid-walk, before
  // the firmware auto-lock teardown checks and the orphaned-screen count, so
  // it under-reported the peak by every screen those built.
  //
  // max_used is a true high-water mark. frag_pct is NOT: it is computed at
  // call time, so it says only how the pool looked at this instant, which is
  // why run_overlapcheck.sh ratchets the first and ignores the second.
  {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[lvheap] total %u used %u max_used %u frag %u%%\n",
           (unsigned)mon.total_size, (unsigned)(mon.total_size - mon.free_size),
           (unsigned)mon.max_used, (unsigned)mon.frag_pct);
  }

  printf("sim done\n");
#ifdef OVERLAPCHECK
  return oc_report();
#else
  return 0;
#endif
}
