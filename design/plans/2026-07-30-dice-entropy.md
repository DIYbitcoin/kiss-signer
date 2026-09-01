# Dice Entropy Implementation Plan

> Worked through step by step; steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a dice-only, off-device-verifiable seed path beside the existing camera+taps path: `seed = BIP39(SHA256(digit string))`.

**Architecture:** A small pure module `kiss_dice` (mirrors `kiss_tapent`) accumulates d6 rolls as an ASCII digit string and, on demand, SHA256s it into 16- or 32-byte entropy with a hard roll-count floor. A new `dice_screen` in `kiss_setup.c` drives it and feeds the existing `kiss_setup_entropy()` flow. All crypto is standard (`wally_sha256` + existing BIP39), so the result is reproducible off-device.

**Tech Stack:** C (ESP-IDF component + host sim), libwally (`wally_sha256`), LVGL for UI. Host tests via `sim/build_test.sh` → `/tmp/kisstest`.

**Spec:** `design/specs/2026-07-30-dice-entropy-design.md`

**Reconciliation note (decide before Task 4):** The spec defaults dice creation to 24 words for quantum margin, but the codebase always creates 12 words (128-bit) and reserves 24 for restore. This plan follows the codebase (creation = 12 words, floor 50 rolls) for consistency; the module supports 24 (floor 99) either way, so switching the creation default later is a one-line change. Confirm 12 vs 24 for the dice creation path before Task 4.

---

## File Structure

- **Create** `main/kiss_dice.h` — the module interface (roll/count/undo/digits/take).
- **Create** `main/kiss_dice.c` — the implementation (digit buffer + SHA256 + floor).
- **Create** `sim/test_dice.c` — host tests, including SHA256 known-answer vectors.
- **Modify** `sim/build_test.sh` — add `kiss_dice.c` and `test_dice.c` to the runner.
- **Modify** `sim/test_crypto.c` — prototype + call `test_dice()`.
- **Modify** `sim/build_sim.sh` — add `kiss_dice.c` so the device UI links.
- **Modify** `main/i18n_keys.h`, `main/i18n_tables.c` — dice strings (English only).
- **Modify** `main/kiss_setup.c` — `method_screen`, `dice_screen`, wire from `storage_pick_cb`.
- **Modify** `main/CMakeLists.txt` — add `kiss_dice.c` to the device build sources.

---

## Task 1: `kiss_dice` module — roll / count / undo / digits

**Files:**
- Create: `main/kiss_dice.h`
- Create: `main/kiss_dice.c`
- Create: `sim/test_dice.c`
- Modify: `sim/build_test.sh`
- Modify: `sim/test_crypto.c`

- [ ] **Step 1: Write the interface header**

Create `main/kiss_dice.h`:

```c
// Source: physical d6 rolls the owner enters by hand. Off-device entropy: the
// seed is BIP39(SHA256(the digit string)), so it can be recomputed on any
// machine and verified against what the device showed. No RNG in this path.
// See design/specs/2026-07-30-dice-entropy-design.md for the threat
// model. Mirrors kiss_tapent: pure, no UI, host-testable.
#pragma once
#include <stdint.h>

#define DICE_MAX        120     // buffer ceiling, comfortably above 99 rolls
#define DICE_FLOOR_128  50      // 12 words / 128 bit  (50 * log2 6 = 129 bit)
#define DICE_FLOOR_256  99      // 24 words / 256 bit  (99 * log2 6 = 256 bit)

// Zero the buffer and the count. Call when the dice screen opens or cancels.
void        kiss_dice_reset(void);

// Append one face, 1..6. Returns 1 if accepted, 0 if the face is out of range
// or the buffer is full.
int         kiss_dice_roll(int face);

// Remove the last accepted roll (backspace). Returns 1 if one was removed,
// 0 if the buffer was already empty.
int         kiss_dice_undo(void);

// Rolls accepted so far.
unsigned    kiss_dice_count(void);

// Read-only, NUL-terminated view of the digit string, for the verify display.
const char *kiss_dice_digits(void);

// SHA256 the digit string and copy the first `len` bytes (16 or 32) into `out`.
// Returns 0 on success; -1 if len is not 16/32, out is NULL, or the count is
// below the floor for that len (50 for 16, 99 for 32).
int         kiss_dice_take(uint8_t *out, unsigned len);
```

- [ ] **Step 2: Write the failing test**

Create `sim/test_dice.c`:

```c
// Host tests for the dice-entropy module. The SHA256 vectors are the proof of
// verifiability: they must equal `printf '<rolls>' | sha256sum`.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "wally_core.h"
#include "wally_crypto.h"
#include "kiss_dice.h"
#include "kiss_seed.h"

static int fails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); fails++; }
}

static void hex(const uint8_t *b, unsigned n, char *out)
{
    for (unsigned i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
}

static void roll_str(const char *s)
{
    kiss_dice_reset();
    for (const char *p = s; *p; p++) kiss_dice_roll(*p - '0');
}

// printf '12345'*10 (50 chars) | sha256sum
static const char *KAT50 =
    "5eca9288344f8143aa96673f67faf41314a3157e2b4defca2acfafb6f6c29fbd";
// printf '123456'*16 '123' (99 chars) | sha256sum
static const char *KAT99 =
    "5588d3630bd19f6375b7bd922457af34ea9c74f00807566a1cf808e445dc8c20";
static const char *R50 =
    "12345123451234512345123451234512345123451234512345";
static const char *R99 =
    "123456123456123456123456123456123456123456123456"
    "123456123456123456123456123456123456123456123456123";

int test_dice(void)
{
    fails = 0;
    printf("\n-- dice entropy --\n");

    // roll / count / invalid / undo
    kiss_dice_reset();
    ok("starts empty", kiss_dice_count() == 0);
    ok("valid face accepted", kiss_dice_roll(4) == 1);
    ok("count is 1", kiss_dice_count() == 1);
    ok("face 0 rejected", kiss_dice_roll(0) == 0);
    ok("face 7 rejected", kiss_dice_roll(7) == 0);
    ok("count unchanged after invalid", kiss_dice_count() == 1);
    ok("undo removes it", kiss_dice_undo() == 1);
    ok("empty again", kiss_dice_count() == 0);
    ok("undo on empty is 0", kiss_dice_undo() == 0);
    kiss_dice_roll(1); kiss_dice_roll(2);
    ok("digits reflect rolls", strcmp(kiss_dice_digits(), "12") == 0);

    return fails;
}
```

- [ ] **Step 3: Wire the module + test into the runner**

In `sim/build_test.sh`, add `main/kiss_dice.c` to the `main/…` source list (right after `main/kiss_tapent.c`) and `sim/test_dice.c` to the `sim/…` test list (right after `sim/test_tapent.c`). The two edited lines become:

```
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_sp.c main/kiss_seed.c main/kiss_seed_sd.c main/platform_sd.c main/kiss_usage.c main/kiss_duress.c main/qr_transport.c main/kiss_tapent.c main/kiss_dice.c \
  sim/test_crypto.c sim/test_qr.c sim/test_seed.c sim/test_sp.c sim/test_sdseed.c sim/test_duress.c sim/test_passedit.c sim/test_squiggle.c sim/test_tapent.c sim/test_dice.c \
```

In `sim/test_crypto.c`, add the prototype after line 38 (`int test_tapent(void);`):

```c
int test_dice(void);
```

And in `main()`, after `fails += test_tapent();`:

```c
    fails += test_dice();
```

- [ ] **Step 4: Run the test to verify it fails**

Run:
```bash
bash sim/build_test.sh
```
Expected: **link error** — `undefined symbol: kiss_dice_reset` (and friends), because `main/kiss_dice.c` does not exist yet.

- [ ] **Step 5: Write the minimal implementation**

Create `main/kiss_dice.c`:

```c
// dice entropy: the verifiable path.
// See design/specs/2026-07-30-dice-entropy-design.md
#include "kiss_dice.h"

#include <string.h>

#include "wally_core.h"
#include "wally_crypto.h"

static char     s_digits[DICE_MAX + 1];
static unsigned s_n;

void kiss_dice_reset(void)
{
    wally_bzero(s_digits, sizeof s_digits);
    s_n = 0;
}

int kiss_dice_roll(int face)
{
    if (face < 1 || face > 6) return 0;
    if (s_n >= DICE_MAX) return 0;
    s_digits[s_n++] = (char)('0' + face);
    s_digits[s_n] = '\0';
    return 1;
}

int kiss_dice_undo(void)
{
    if (s_n == 0) return 0;
    s_digits[--s_n] = '\0';
    return 1;
}

unsigned kiss_dice_count(void) { return s_n; }

const char *kiss_dice_digits(void) { return s_digits; }

int kiss_dice_take(uint8_t *out, unsigned len)
{
    if (!out || (len != 16 && len != 32)) return -1;
    unsigned floor = (len == 32) ? DICE_FLOOR_256 : DICE_FLOOR_128;
    if (s_n < floor) return -1;

    uint8_t h[32];
    int rc = wally_sha256((const unsigned char *)s_digits, s_n, h, 32);
    if (rc == WALLY_OK) memcpy(out, h, len);
    wally_bzero(h, sizeof h);
    return rc == WALLY_OK ? 0 : -1;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run:
```bash
bash sim/build_test.sh && /tmp/kisstest | sed -n '/-- dice entropy --/,/^--/p'
```
Expected: the `-- dice entropy --` block prints all `PASS:` lines, no `FAIL:`.

- [ ] **Step 7: Commit**

```bash
git add main/kiss_dice.h main/kiss_dice.c sim/test_dice.c sim/build_test.sh sim/test_crypto.c
git commit -m "dice entropy: roll buffer module (roll/count/undo/digits)"
```

---

## Task 2: `kiss_dice_take` — SHA256, floor gate, KAT vectors, wipe

**Files:**
- Modify: `sim/test_dice.c`

The implementation from Task 1 already covers `take`. This task proves it against
independent SHA256 vectors, the floor, length validation, and the wipe — the
verifiability guarantees.

- [ ] **Step 1: Add the failing assertions**

In `sim/test_dice.c`, insert before `return fails;`:

```c
    // ---- floor gate ----
    uint8_t e[32]; char got[65];
    roll_str(R50);                                  // 50 rolls
    ok("50 rolls: 12-word take ok", kiss_dice_take(e, 16) == 0);
    ok("50 rolls: 24-word take refused", kiss_dice_take(e, 32) == -1);
    kiss_dice_undo();                             // 49 rolls
    ok("49 rolls: 12-word take refused", kiss_dice_take(e, 16) == -1);

    // ---- KAT: 12-word entropy == first 16 bytes of SHA256(R50) ----
    roll_str(R50);
    kiss_dice_take(e, 16); hex(e, 16, got);
    ok("12-word entropy == SHA256(R50)[0..16]", strncmp(got, KAT50, 32) == 0);
    if (strncmp(got, KAT50, 32) != 0) printf("  got %s\n  want %.32s\n", got, KAT50);

    // ---- KAT: 24-word entropy == SHA256(R99) ----
    roll_str(R99);
    ok("99 rolls: 24-word take ok", kiss_dice_take(e, 32) == 0);
    hex(e, 32, got);
    ok("24-word entropy == SHA256(R99)", strcmp(got, KAT99) == 0);
    if (strcmp(got, KAT99) != 0) printf("  got %s\n  want %s\n", got, KAT99);

    // ---- entropy -> a real, deterministic BIP39 mnemonic ----
    // (self-consistency here; the human off-device cross-check against an
    // external BIP39 tool is the device-acceptance step in the spec.)
    char words[256], words2[256];
    ok("entropy -> mnemonic rc",
       kiss_seed_from_entropy(e, 32, words, sizeof words) == 0);
    roll_str(R99); kiss_dice_take(e, 32);
    ok("same rolls -> same mnemonic",
       kiss_seed_from_entropy(e, 32, words2, sizeof words2) == 0 &&
       strcmp(words, words2) == 0);

    // ---- length validation + wipe ----
    ok("bad len rejected", kiss_dice_take(e, 20) == -1);
    ok("NULL out rejected", kiss_dice_take(NULL, 32) == -1);
    kiss_dice_reset();
    ok("reset clears count", kiss_dice_count() == 0);
    ok("reset clears digits", kiss_dice_digits()[0] == '\0');
```

- [ ] **Step 2: Run to verify all pass**

Run:
```bash
bash sim/build_test.sh && /tmp/kisstest | sed -n '/-- dice entropy --/,/^--/p'
```
Expected: every dice assertion `PASS:`, including both KAT lines. (The implementation
already satisfies these; if a KAT fails, the recipe or byte-slicing is wrong — fix
`kiss_dice.c`, not the vector.)

- [ ] **Step 3: Independently confirm the vectors (sanity)**

Run:
```bash
printf '12345%.0s' {1..10} | shasum -a 256   # expect 5eca9288...c29fbd
```
Expected: matches `KAT50`. This proves the test checks against a standard tool, not itself.

- [ ] **Step 4: Commit**

```bash
git add sim/test_dice.c
git commit -m "dice entropy: SHA256 known-answer vectors, floor gate, wipe tests"
```

---

## Task 3: i18n strings (English only; tr() falls back for other locales)

**Files:**
- Modify: `main/i18n_keys.h`
- Modify: `main/i18n_tables.c`

- [ ] **Step 1: Add the keys**

In `main/i18n_keys.h`, after `STR_W_ENT_FAIL_B,` add:

```c
    STR_W_CHOOSE_DICE,
    STR_W_DICE_NOTE,
    STR_W_DICE_T,
    STR_W_DICE_S,
    STR_W_DICE_TALLY_FMT,
    STR_W_DICE_UNDO,
    STR_W_DICE_VERIFY_NOTE,
    STR_W_DICE_SAMEY,
```

- [ ] **Step 2: Add the English strings**

In `main/i18n_tables.c`, in the English table (`tbl_en`), after
`[STR_W_ENT_FAIL_B] = ...,` add:

```c
    [STR_W_CHOOSE_DICE] = "DICE",
    [STR_W_DICE_NOTE] = "your own rolls. checkable on any computer.",
    [STR_W_DICE_T] = "ROLL A DIE",
    [STR_W_DICE_S] = "enter each roll. the words come only from these.",
    [STR_W_DICE_TALLY_FMT] = "%u / %u",
    [STR_W_DICE_UNDO] = "UNDO",
    [STR_W_DICE_VERIFY_NOTE] = "SHA256 of your rolls. recompute it offline to check.",
    [STR_W_DICE_SAMEY] = "that doesn't look rolled. continue?",
```

- [ ] **Step 3: Verify it compiles**

Run:
```bash
bash sim/build_sim.sh
```
Expected: `built /tmp/fruitsim`, no errors.

- [ ] **Step 4: Commit**

```bash
git add main/i18n_keys.h main/i18n_tables.c
git commit -m "dice entropy: strings"
```

---

## Task 4: method choice + dice screen, wired into setup

**Files:**
- Modify: `main/kiss_setup.c`
- Modify: `main/CMakeLists.txt`
- Modify: `sim/build_sim.sh`

- [ ] **Step 1: Add the module to the device + sim builds**

In `main/CMakeLists.txt`, add `"kiss_dice.c"` to the component `SRCS` list next to
`"kiss_tapent.c"`. In `sim/build_sim.sh`, add `main/kiss_dice.c` to the `main/…`
source list next to `main/kiss_setup.c`.

- [ ] **Step 2: Add the include and forward declarations**

Near the top of `main/kiss_setup.c`, with the other `#include "kiss_*.h"` lines
(around line 21), add:

```c
#include "kiss_dice.h"     // source: verifiable off-device dice rolls
```

With the other forward decls (near line 78, `static void entropy_screen(void);`), add:

```c
static void dice_screen(void);
static void method_screen(void);
```

- [ ] **Step 3: Route creation through the method choice**

In `storage_pick_cb`, replace the creation branch (currently, at
`main/kiss_setup.c:1135-1136`):

```c
    s_count = 12;
    entropy_screen();
```

with:

```c
    s_count = 12;
    method_screen();
```

- [ ] **Step 4: Add `method_screen` and `dice_screen`**

In `main/kiss_setup.c`, immediately **above** `static void entropy_screen(void)`
(line ~877), add the method chooser and the dice screen. This mirrors `count_screen`
(rows) and `tap_screen` (a card that captures input and gates on a count).

```c
// The camera path and the dice path both end at kiss_setup_entropy(); this
// screen is the only fork between them. Camera is convenient and multi-source;
// dice is single-source but recomputable off-device, for owners who want to
// verify the firmware did not cheat.
static void method_cam_cb(lv_event_t *e)  { (void)e; entropy_screen(); }
static void method_dice_cb(lv_event_t *e) { (void)e; dice_screen(); }

static void method_screen(void)
{
    mk_screen(tr(STR_W_NEW_T), tr(STR_W_HOWMANY));
    wt_row_x(s_scr, WT_ICON_QR, tr(STR_W_CHOOSE_NEW), tr(STR_W_NEW_NOTE), NULL,
             NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(0),
             WT_CHOICE_W, WT_CHOICE_H, method_cam_cb, NULL);
    wt_row_x(s_scr, LV_SYMBOL_LIST, tr(STR_W_CHOOSE_DICE), tr(STR_W_DICE_NOTE),
             NULL, NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(1),
             WT_CHOICE_W, WT_CHOICE_H, method_dice_cb, NULL);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_choose_cb, NULL);
}

// ---- dice screen ----
#define DICE_CARD_X   100
#define DICE_CARD_Y   130
#define DICE_CARD_W   600
#define DICE_CARD_H   300
#define DICE_KEY_W     84
#define DICE_KEY_H     60
#define DICE_KEY_GAP   10

static lv_obj_t *s_dice_card;
static lv_obj_t *s_dice_tally;
static lv_obj_t *s_dice_done;

static unsigned dice_floor(void) { return s_count == 24 ? DICE_FLOOR_256 : DICE_FLOOR_128; }

static void dice_refresh(void)
{
    unsigned n = kiss_dice_count();
    if (s_dice_tally) {
        char buf[16];
        snprintf(buf, sizeof buf, tr(STR_W_DICE_TALLY_FMT), n, dice_floor());
        lv_label_set_text(s_dice_tally, buf);
    }
    if (s_dice_done) {
        if (n >= dice_floor()) lv_obj_remove_flag(s_dice_done, LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_add_flag(s_dice_done, LV_OBJ_FLAG_HIDDEN);
    }
}

static void dice_key_cb(lv_event_t *e)
{
    int face = (int)(intptr_t)lv_event_get_user_data(e);
    kiss_dice_roll(face);
    dice_refresh();
}

static void dice_undo_cb(lv_event_t *e) { (void)e; kiss_dice_undo(); dice_refresh(); }

// Build the seed. Same wipe discipline as tap_done_cb: entropy is seed material.
static void dice_done_cb(lv_event_t *e)
{
    (void)e;
    unsigned need = s_count == 24 ? 32 : 16;
    uint8_t entropy[32];
    if (kiss_dice_take(entropy, need) == 0) {
        kiss_dice_reset();
        kiss_setup_entropy(entropy, need);
    } else {
        // Cannot happen once the floor is met, but never leave a dead button.
        mk_screen(tr(STR_W_ENT_FAIL_T), NULL);
        mk_body(tr(STR_W_ENT_FAIL_B), 48, 118, 704, 260, INK_COL);
        mk_pill(tr(STR_C_TRY_AGAIN), WT_BACK_X, WT_ACTION_Y, 160, ent_retry_cb, NULL);
    }
    memset(entropy, 0, sizeof entropy);
}

static void dice_cancel_cb(lv_event_t *e) { (void)e; kiss_dice_reset(); cancel_cb(e); }

static void dice_screen(void)
{
    kiss_dice_reset();
    s_dice_tally = NULL; s_dice_done = NULL;
    mk_screen(tr(STR_W_DICE_T), tr(STR_W_DICE_S));

    s_dice_card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_dice_card);
    lv_obj_set_pos(s_dice_card, DICE_CARD_X, DICE_CARD_Y);
    lv_obj_set_size(s_dice_card, DICE_CARD_W, DICE_CARD_H);
    lv_obj_set_style_radius(s_dice_card, 10, 0);
    lv_obj_set_style_border_width(s_dice_card, 1, 0);
    lv_obj_set_style_border_color(s_dice_card, WT_EDGE, 0);
    lv_obj_set_style_bg_color(s_dice_card, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(s_dice_card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_dice_card, LV_OBJ_FLAG_SCROLLABLE);

    // six d6 keys, 1..6, in a row
    for (int i = 0; i < 6; i++) {
        lv_obj_t *k = lv_button_create(s_dice_card);
        lv_obj_set_pos(k, 18 + i * (DICE_KEY_W + DICE_KEY_GAP), 20);
        lv_obj_set_size(k, DICE_KEY_W, DICE_KEY_H);
        lv_obj_add_event_cb(k, dice_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        lv_obj_t *lbl = lv_label_create(k);
        char d[2] = { (char)('1' + i), 0 };
        lv_label_set_text(lbl, d);
        lv_obj_center(lbl);
    }

    s_dice_tally = wt_lbl(s_dice_card, "", 18, 108, wt_font_mono28(), INK_COL);
    lv_obj_t *note = wt_lbl(s_dice_card, tr(STR_W_DICE_VERIFY_NOTE), 18, 158,
                            wt_font14(), MUT_COL);
    lv_obj_set_width(note, DICE_CARD_W - 36);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    mk_pill(tr(STR_W_DICE_UNDO), 18, DICE_CARD_Y + DICE_CARD_H + 6, 120, dice_undo_cb, NULL);
    s_dice_done = mk_pill(tr(STR_W_WROTE), 300, DICE_CARD_Y + DICE_CARD_H + 6, 200,
                          dice_done_cb, NULL);   // reuse a "confirm" label; hidden until floor
    mk_pill(tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 160, dice_cancel_cb, NULL);
    dice_refresh();
}
```

> `mk_pill` is `static lv_obj_t *mk_pill(...)` at `main/kiss_setup.c:155` — it already
> returns the pill object, so `s_dice_done = mk_pill(...)` works as written.

- [ ] **Step 5: Verify the sim builds**

Run:
```bash
bash sim/build_sim.sh
```
Expected: `built /tmp/fruitsim`, no errors or warnings about `dice_*`/`method_*`.

- [ ] **Step 6: Verify the host suite still passes**

Run:
```bash
bash sim/build_test.sh && /tmp/kisstest > /tmp/t.log; echo "exit=$?"; grep -c FAIL /tmp/t.log
```
Expected: `exit=0` and `0` FAIL lines.

- [ ] **Step 7: Commit**

```bash
git add main/kiss_setup.c main/CMakeLists.txt sim/build_sim.sh
git commit -m "dice entropy: method choice + dice screen wired into setup"
```

---

## Task 5: verification fingerprint + "same-y rolls" nudge

**Files:**
- Modify: `main/kiss_setup.c`

- [ ] **Step 1: Show a checkable fingerprint on the card**

In `dice_screen`, the verify note is generic. Make it live: in `dice_refresh`, when
`kiss_dice_count() > 0`, compute the SHA256 of the current digits and show the first
8 bytes (16 hex) so the owner can spot-check. Add near the tally, a second label
`s_dice_fp`, and in `dice_refresh` after updating the tally:

```c
    if (s_dice_fp) {
        uint8_t e[32]; char fp[33] = "";
        if (n > 0 && wally_sha256((const unsigned char *)kiss_dice_digits(), n, e, 32) == WALLY_OK) {
            for (int i = 0; i < 8; i++) snprintf(fp + i * 2, 3, "%02x", e[i]);
        }
        lv_label_set_text(s_dice_fp, fp);
        wally_bzero(e, sizeof e);
    }
```

Declare `static lv_obj_t *s_dice_fp;`, create it in `dice_screen` (e.g. at y=138 under
the tally with `wt_font14()`/`MUT_COL`), and set `s_dice_fp = NULL;` at the top of
`dice_screen` alongside the other resets. Add `#include "wally_crypto.h"` to
`kiss_setup.c` if not already present (`git grep -n wally_crypto.h main/kiss_setup.c`).

- [ ] **Step 2: Warn on all-identical rolls at confirm time**

In `dice_done_cb`, before `kiss_dice_take`, add a one-time nudge if every entered
face is the same. Use a static guard so a second press proceeds:

```c
    static bool warned;
    const char *d = kiss_dice_digits();
    bool samey = d[0] != 0;
    for (const char *p = d; *p; p++) if (*p != d[0]) { samey = false; break; }
    if (samey && !warned) {
        warned = true;
        mk_screen(tr(STR_W_DICE_T), tr(STR_W_DICE_SAMEY));
        mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, method_dice_cb, NULL);
        return;
    }
    warned = false;
```

(Reset `warned` when the screen is rebuilt: set `warned` false is handled by the
`else` fall-through above; the BACK pill returns to a fresh `dice_screen` via
`method_dice_cb`.)

- [ ] **Step 3: Verify sim builds and host suite passes**

Run:
```bash
bash sim/build_sim.sh && bash sim/build_test.sh && /tmp/kisstest > /tmp/t.log; echo "exit=$?"; grep -c FAIL /tmp/t.log
```
Expected: `built /tmp/fruitsim`, `exit=0`, `0` FAIL.

- [ ] **Step 4: Commit**

```bash
git add main/kiss_setup.c
git commit -m "dice entropy: checkable fingerprint + all-identical nudge"
```

---

## Task 6: final verification + device-test handoff

**Files:** none (verification only)

- [ ] **Step 1: Full green**

Run:
```bash
bash sim/build_sim.sh
bash sim/build_test.sh && /tmp/kisstest > /tmp/t.log; echo "exit=$?"; grep -c FAIL /tmp/t.log
```
Expected: `built /tmp/fruitsim`, `exit=0`, `0` FAIL.

- [ ] **Step 2: Independent end-to-end verification (the whole point)**

Pick any roll string of ≥50 digits (1–6), e.g. `12345…`. Confirm the device's flow is
reproducible off-device:
```bash
printf '<your exact rolls>' | shasum -a 256
```
Take the first 16 bytes (12-word) / all 32 (24-word), feed to any offline BIP39 tool,
and confirm the words match what `dice_screen` produced for the same rolls in the sim.

- [ ] **Step 3: Record the device-test verdict**

**DEVICE TEST: REQUIRED** — the dice *logic* is fully host-covered, but the UI wiring is
not. On hardware verify: the six keys register, the tally gates at the floor (50 for the
12-word creation default), UNDO decrements, the confirm pill appears only at/after the
floor, CANCEL wipes, and — the acceptance check — the words shown match an offline
`shasum` of the same rolls. Note this verdict in the PR description.

- [ ] **Step 4: Reconciliation follow-up**

If the decision (see header) is that dice creation should default to **24 words** for
quantum margin rather than 12: change the creation default in `storage_pick_cb`
(`s_count = 24;` for the dice branch) or add a 12/24 choice to `method_screen`, and
update the spec's §3. The module already supports both; no `kiss_dice` change needed.

---

## Self-Review (author)

- **Spec coverage:** recipe (Task 1–2), floor 50/99 (Task 2), verifiability KAT (Task 2),
  `kiss_dice` module mirroring `kiss_tapent` (Task 1), `dice_screen` + method choice
  (Task 4), fingerprint + all-identical nudge (Task 5, §9 decisions), failure reuse
  (Task 4 `dice_done_cb`), i18n English-only (Task 3), host tests incl. cross-tool check
  (Task 2/6), device-test note (Task 6). The 24-word quantum default is flagged as a
  product decision (header + Task 6 Step 4) because it conflicts with the codebase's
  documented "creation = 12 words" rule.
- **Placeholders:** none — all code and KAT vectors are concrete and computed.
- **Type consistency:** `kiss_dice_take(out, len)`, `kiss_dice_count()`,
  `kiss_dice_digits()`, `DICE_FLOOR_128/256` used identically across tasks; `s_count`,
  `need = s_count==24?32:16`, and `kiss_setup_entropy(entropy, need)` match the existing
  camera path. `mk_pill` (`main/kiss_setup.c:155`) already returns `lv_obj_t *`, and all
  reused strings (`STR_W_NEW_T`, `STR_W_HOWMANY`, `STR_W_CHOOSE_NEW`, `STR_W_NEW_NOTE`,
  `STR_W_WROTE`) and helpers (`goto_choose_cb`, `mk_body`, `wt_font14`, `wt_font_mono28`,
  `WT_ICON_QR`) exist — verified against the tree.
