# Tap Entropy Implementation Plan

> Worked through step by step; steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every new wallet's seed depend on a third entropy source — the timing of the user's own taps — so that no single manufacturer's RNG can determine a wallet.

**Architecture:** A new host-testable module (`main/kiss_tapent.c`) accumulates tap records into a SHA256 chain, with the clock injected so the fold and debounce can be tested off device. `camera_spike.c` stops finishing the seed and instead hands its accumulated chain out. A new `kiss_crypto.c` helper hashes all three chains flat. A new LVGL screen in `kiss_setup.c` drives the taps and runs the final mix.

**Tech Stack:** ESP-IDF, LVGL 9, libwally-core (vendored), C99. Host tests build via `sim/build_test.sh` into a single `/tmp/kisstest` binary.

**Spec:** `docs/specs/tap-entropy.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `main/kiss_tapent.h` (create) | Public interface: reset, offer a tap, read progress, take the chain. |
| `main/kiss_tapent.c` (create) | Debounce + fold. Injected clock, no LVGL, no ESP headers. Host testable. |
| `main/kiss_crypto.h` / `.c` (modify) | Add `kiss_entropy_mix3`. |
| `main/camera_spike.h` / `.c` (modify) | Stop mixing at capture; expose the camera chain. |
| `main/kiss_setup.c` (modify) | Randomness screen copy/equation; new tap screen; final mix. |
| `i18n/*.json` (modify, 21 files) | Four new UI strings. |
| `sim/test_tapent.c` (create) | Host tests for the fold, the debounce and `mix3`. |
| `sim/build_test.sh` (modify) | Compile the two new files into the runner. |
| `sim/test_crypto.c` (modify) | Call the new suite from `main`, add its fails to the total. |

`kiss_tapent.c` is deliberately free of ESP and LVGL headers. Every security-relevant decision (what gets hashed, what counts as a tap) lives there and is exercised on the host; the screen only supplies timestamps and paints a bar.

---

## Task 1: `kiss_entropy_mix3`

**Files:**
- Modify: `main/kiss_crypto.h:20`
- Modify: `main/kiss_crypto.c:62-72`
- Create: `sim/test_tapent.c`
- Modify: `sim/build_test.sh:24-25`
- Modify: `sim/test_crypto.c:26-33`

- [ ] **Step 1: Write the failing test**

Create `sim/test_tapent.c`:

```c
// Host tests for the tap-entropy fold and the three-way mix.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "kiss_crypto.h"

static int fails;

static void ok(const char *name, int cond)
{
    if (cond) { printf("PASS: %s\n", name); }
    else      { printf("FAIL: %s\n", name); fails++; }
}

// SHA256 of 96 bytes: 0x00 x32 ‖ 0x11 x32 ‖ 0x22 x32.
// Independently computed:
//   python3 -c "import hashlib;print(hashlib.sha256(bytes(32)+b'\x11'*32+b'\x22'*32).hexdigest())"
static const char *MIX3_ABC = "PASTE_THE_DIGEST_FROM_STEP_2";

static void hex32(const uint8_t h[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}

static void test_mix3(void)
{
    uint8_t a[32], b[32], c[32], out[32], out2[32];
    memset(a, 0x00, 32); memset(b, 0x11, 32); memset(c, 0x22, 32);

    ok("mix3 returns 0", kiss_entropy_mix3(a, b, c, out) == 0);

    char got[65]; hex32(out, got);
    ok("mix3 matches the independent vector", strcmp(got, MIX3_ABC) == 0);
    if (strcmp(got, MIX3_ABC) != 0) printf("  got %s\n  want %s\n", got, MIX3_ABC);

    // order matters: a hash that ignored ordering would let a coordinator of
    // sources swap which one dominates
    kiss_entropy_mix3(c, b, a, out2);
    ok("mix3 is order sensitive", memcmp(out, out2, 32) != 0);

    // every input reaches the digest
    a[31] ^= 1; kiss_entropy_mix3(a, b, c, out2);
    ok("mix3 depends on a", memcmp(out, out2, 32) != 0);
    a[31] ^= 1; b[31] ^= 1; kiss_entropy_mix3(a, b, c, out2);
    ok("mix3 depends on b", memcmp(out, out2, 32) != 0);
    b[31] ^= 1; c[31] ^= 1; kiss_entropy_mix3(a, b, c, out2);
    ok("mix3 depends on c", memcmp(out, out2, 32) != 0);

    ok("mix3 rejects NULL", kiss_entropy_mix3(NULL, b, c, out) != 0);
}

int test_tapent(void)
{
    fails = 0;
    printf("\n-- tap entropy --\n");
    test_mix3();
    return fails;
}
```

`MIX3_ABC` is deliberately not a digest yet, so the test cannot accidentally pass before step 2 fills it in. Never paste the implementation's own output here: a vector taken from the code under test verifies nothing.

- [ ] **Step 2: Compute the real vector and paste it in**

Run:

```bash
python3 -c "import hashlib;print(hashlib.sha256(bytes(32)+b'\x11'*32+b'\x22'*32).hexdigest())"
```

Replace the `MIX3_ABC` literal with that output.

- [ ] **Step 3: Wire the suite into the runner**

In `sim/build_test.sh`, add `sim/test_tapent.c` to the source list (the line beginning `sim/test_crypto.c sim/test_qr.c`), and add `main/kiss_tapent.c` to the `main/...` source line. `kiss_tapent.c` does not exist yet — create it as an empty stub now so the build links:

```bash
printf '// tap entropy: see kiss_tapent.h\n#include "kiss_tapent.h"\n' > main/kiss_tapent.c
printf '#pragma once\n' > main/kiss_tapent.h
```

In `sim/test_crypto.c`, beside the other suite declarations near line 26, add:

```c
// sim/test_tapent.c — the tap-entropy fold, debounce and three-way mix
int test_tapent(void);
```

and in `main`, next to the other `fails += test_*()` calls, add:

```c
    fails += test_tapent();
```

- [ ] **Step 4: Run the test to verify it fails**

```bash
sim/build_test.sh && /tmp/kisstest
```

Expected: the build fails with `implicit declaration of function 'kiss_entropy_mix3'`.

- [ ] **Step 5: Implement `kiss_entropy_mix3`**

In `main/kiss_crypto.h`, directly under the existing `kiss_entropy_mix` declaration at line 20:

```c
// Flat three-input fold: out = SHA256(a ‖ b ‖ c). Camera, chip TRNG, taps —
// in that order, matching the source numbers the setup screens show. Flat
// rather than nested mix() calls so the one question that matters (what went
// into this seed) is answerable by reading one line.
int kiss_entropy_mix3(const uint8_t a[32], const uint8_t b[32],
                        const uint8_t c[32], uint8_t out[32]);
```

In `main/kiss_crypto.c`, directly after `kiss_entropy_mix` (which ends at line 72):

```c
int kiss_entropy_mix3(const uint8_t a[32], const uint8_t b[32],
                        const uint8_t c[32], uint8_t out[32])
{
    if (!a || !b || !c || !out)
        return -1;
    uint8_t cat[96];
    memcpy(cat, a, 32);
    memcpy(cat + 32, b, 32);
    memcpy(cat + 64, c, 32);
    int rc = wally_sha256(cat, sizeof cat, out, 32) == WALLY_OK ? 0 : -1;
    wally_bzero(cat, sizeof cat);
    return rc;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
sim/build_test.sh && /tmp/kisstest
```

Expected: seven `PASS:` lines under `-- tap entropy --`, and the runner's final total unchanged in failures.

- [ ] **Step 7: Commit**

```bash
git add main/kiss_crypto.c main/kiss_crypto.h main/kiss_tapent.c main/kiss_tapent.h sim/test_tapent.c sim/build_test.sh sim/test_crypto.c
git commit -m "three sources need a three input hash, not a nested pair"
```

---

## Task 2: The tap fold and its debounce

**Files:**
- Modify: `main/kiss_tapent.h` (replace the stub)
- Modify: `main/kiss_tapent.c` (replace the stub)
- Modify: `sim/test_tapent.c`

- [ ] **Step 1: Write the failing tests**

Append to `sim/test_tapent.c`, above `int test_tapent(void)`:

```c
#include "kiss_tapent.h"

static void test_debounce(void)
{
    uint8_t chain[32];
    kiss_tapent_reset();
    ok("starts at zero", kiss_tapent_count() == 0);

    // first tap always counts: there is no predecessor to be too close to
    ok("first tap counts", kiss_tapent_tap(1000000, 5000, 100, 200) == 1);
    ok("count is 1", kiss_tapent_count() == 1);

    // 29ms later: below WTAP_DEBOUNCE_US, a panel artifact rather than a hand
    ok("29ms is rejected", kiss_tapent_tap(1029000, 5001, 100, 200) == 0);
    ok("count still 1", kiss_tapent_count() == 1);

    // 31ms later: a real tap
    ok("31ms is accepted", kiss_tapent_tap(1060000, 5002, 101, 201) == 1);
    ok("count is 2", kiss_tapent_count() == 2);

    // a rejected tap must not become the new predecessor, or a fast drag would
    // ratchet the window forward and let the next artifact through
    ok("30ms after a REJECTED tap is measured from the accepted one",
       kiss_tapent_tap(1080000, 5003, 102, 202) == 0);

    ok("not done at 2 taps", kiss_tapent_take(chain) != 0);
}

static void test_fold(void)
{
    uint8_t chain_a[32], chain_b[32];

    // 64 taps at a fixed cadence completes
    kiss_tapent_reset();
    for (int i = 0; i < WTAP_TARGET; i++)
        kiss_tapent_tap(1000000 + (uint64_t)i * 50000, (uint32_t)i, 10, 10);
    ok("64 taps reach the target", kiss_tapent_count() == WTAP_TARGET);
    ok("take succeeds at the target", kiss_tapent_take(chain_a) == 0);

    // 63 taps does not
    kiss_tapent_reset();
    for (int i = 0; i < WTAP_TARGET - 1; i++)
        kiss_tapent_tap(1000000 + (uint64_t)i * 50000, (uint32_t)i, 10, 10);
    ok("63 taps do not", kiss_tapent_take(chain_b) != 0);

    // identical timing but one differing cycle count must change the chain:
    // this is the property the whole feature rests on
    kiss_tapent_reset();
    for (int i = 0; i < WTAP_TARGET; i++)
        kiss_tapent_tap(1000000 + (uint64_t)i * 50000,
                          (uint32_t)(i == 7 ? 999999 : i), 10, 10);
    kiss_tapent_take(chain_b);
    ok("one differing cycle count changes the chain",
       memcmp(chain_a, chain_b, 32) != 0);

    // reset must not leave the previous session's chain behind
    kiss_tapent_reset();
    ok("reset clears the count", kiss_tapent_count() == 0);
    ok("reset clears doneness", kiss_tapent_take(chain_b) != 0);
}
```

and inside `test_tapent`, after `test_mix3();`:

```c
    test_debounce();
    test_fold();
```

- [ ] **Step 2: Run to verify it fails**

```bash
sim/build_test.sh && /tmp/kisstest
```

Expected: build fails on `WTAP_TARGET` and `kiss_tapent_reset` being undeclared.

- [ ] **Step 3: Write the header**

Replace `main/kiss_tapent.h` entirely:

```c
// Source 3: the timing of the user's own taps.
//
// The taps are not the entropy. The jitter between them is, sampled at CPU
// cycle resolution: at 240MHz a human's tens of milliseconds of variation
// spans tens of millions of cycles, so the low bits are unpredictable to
// someone watching. See docs/specs/tap-entropy.md for the budget and for why
// this counts events rather than scoring them.
//
// The clock is a parameter, not a call: every decision here is exercised on
// the host by sim/test_tapent.c with injected timestamps.
#pragma once
#include <stdint.h>
#include <stddef.h>

// 64 taps counted at a deliberately underclaimed 2 bits each = 128 bits.
#define WTAP_TARGET       64

// A tap closer than this to the last ACCEPTED one is a drag or bounce
// artifact of the touch panel, not a decision by a hand.
#define WTAP_DEBOUNCE_US  30000

// Begin a session. Zeroes the chain, the count and the debounce clock.
void kiss_tapent_reset(void);

// Offer one tap. `us` is a microsecond timestamp, `cycles` the CPU cycle
// counter, `x`/`y` the touch point. Returns 1 if it counted, 0 if debounced.
// Counting folds the record straight into the chain: taps are never buffered.
int kiss_tapent_tap(uint64_t us, uint32_t cycles, int16_t x, int16_t y);

// Taps accepted so far, capped at WTAP_TARGET.
unsigned kiss_tapent_count(void);

// Copy the finished chain into out[32]. Returns 0 only once the target is
// reached, nonzero otherwise — a partial chain is never handed out.
int kiss_tapent_take(uint8_t out[32]);
```

- [ ] **Step 4: Write the implementation**

Replace `main/kiss_tapent.c` entirely:

```c
#include "kiss_tapent.h"

#include <string.h>

#include "wally_core.h"
#include "wally_crypto.h"

static uint8_t  s_chain[32];
static unsigned s_count;
static uint64_t s_last_us;
static int      s_started;

void kiss_tapent_reset(void)
{
    wally_bzero(s_chain, sizeof s_chain);
    s_count = 0;
    s_last_us = 0;
    s_started = 0;
}

int kiss_tapent_tap(uint64_t us, uint32_t cycles, int16_t x, int16_t y)
{
    if (s_count >= WTAP_TARGET)
        return 0;
    // The first tap has no predecessor, so it cannot be too close to one.
    // Afterwards the window is measured from the last ACCEPTED tap: measuring
    // from the last OFFERED one would let a fast drag ratchet the window
    // forward and admit the artifacts it exists to drop.
    if (s_started && us - s_last_us < WTAP_DEBOUNCE_US)
        return 0;
    s_started = 1;
    s_last_us = us;

    // 16 bytes: cycles ‖ us ‖ x ‖ y, little-endian. The cycle counter carries
    // the entropy; the microsecond stamp is the value an auditor recognises;
    // the coordinates are a bonus and are NOT counted toward the budget,
    // because people tap the same spot and the correlation is real.
    uint8_t rec[16];
    rec[0] = (uint8_t)(cycles      ); rec[1] = (uint8_t)(cycles >>  8);
    rec[2] = (uint8_t)(cycles >> 16); rec[3] = (uint8_t)(cycles >> 24);
    for (int i = 0; i < 8; i++) rec[4 + i] = (uint8_t)(us >> (8 * i));
    rec[12] = (uint8_t)((uint16_t)x     ); rec[13] = (uint8_t)((uint16_t)x >> 8);
    rec[14] = (uint8_t)((uint16_t)y     ); rec[15] = (uint8_t)((uint16_t)y >> 8);

    uint8_t cat[48];
    memcpy(cat, s_chain, 32);
    memcpy(cat + 32, rec, sizeof rec);
    int rc = wally_sha256(cat, sizeof cat, s_chain, 32);
    wally_bzero(cat, sizeof cat);
    wally_bzero(rec, sizeof rec);
    if (rc != WALLY_OK)
        return 0;

    s_count++;
    return 1;
}

unsigned kiss_tapent_count(void)
{
    return s_count;
}

int kiss_tapent_take(uint8_t out[32])
{
    if (!out || s_count < WTAP_TARGET)
        return -1;
    memcpy(out, s_chain, 32);
    return 0;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
sim/build_test.sh && /tmp/kisstest
```

Expected: every line under `-- tap entropy --` reads `PASS:`.

- [ ] **Step 6: Commit**

```bash
git add main/kiss_tapent.c main/kiss_tapent.h sim/test_tapent.c
git commit -m "taps fold as they land, and a panel bounce is not a decision"
```

---

## Task 3: The camera hands over its chain instead of finishing the seed

**Files:**
- Modify: `main/camera_spike.c:343-356`
- Modify: `main/camera_spike.h`

Today `camera_spike.c:351` calls `kiss_entropy_mix(s_ent_chain, trng, s_ent_hash)` and the seed is done. It must instead stop at `SHA256(frames ‖ trng)` and let the tap screen perform the final fold. The value it produces is unchanged in shape — a 32-byte chain — so the existing "ready" plumbing keeps working.

- [ ] **Step 1: Rename the result so no caller mistakes it for a seed**

In `main/camera_spike.h`, find the accessor that hands out the entropy result (the one `kiss_setup.c` polls after capture) and rename it from its current seed-flavoured name to `camera_entropy_chain`, keeping the same signature. Update its comment to:

```c
// The camera stage's contribution: SHA256(sampled frames ‖ chip TRNG). NOT a
// seed. kiss_setup.c folds this with the tap chain (kiss_entropy_mix3)
// before any mnemonic exists.
```

- [ ] **Step 2: Update the comment at the mix site**

In `main/camera_spike.c`, replace the comment above the `esp_fill_random` call (currently at lines 345-347) with:

```c
      // Sources 1 and 2, folded: SHA256(frames ‖ trng). A predictable scene
      // cannot weaken this below the TRNG and a starved TRNG is still covered
      // by the photos. Source 3 (taps) is folded in on the next screen — this
      // is deliberately NOT the seed.
```

The code below it is unchanged: `kiss_entropy_mix(s_ent_chain, trng, s_ent_hash)` still produces the right value.

- [ ] **Step 3: Update every caller**

```bash
grep -rn "camera_entropy_chain\|s_ent_hash" main/ | grep -v camera_spike
```

Fix each hit in `main/kiss_setup.c` to the new name. There should be exactly one call site, in the capture poll.

- [ ] **Step 4: Build for the device**

```bash
idf.py build
```

Expected: compiles clean. No behaviour change yet — capture still leads straight to the words screen. That is fixed in Task 5.

- [ ] **Step 5: Commit**

```bash
git add main/camera_spike.c main/camera_spike.h main/kiss_setup.c
git commit -m "the camera stops calling its half a seed"
```

---

## Task 4: The four new strings

**Files:**
- Modify: `i18n/en.json`
- Modify: the other 20 files in `i18n/`

`tools/gen_i18n.py` **errors** if any locale file is missing a key that `en.json` has (line 156), so all 21 files must gain all four keys in the same commit or the build breaks.

- [ ] **Step 1: Add the English strings**

In `i18n/en.json`, beside the existing `W_ENT_*` keys, add:

```json
  "W_ENT_TAP_T": "TAP TO ADD RANDOMNESS",
  "W_ENT_TAP_S": "your timing is the part no manufacturer can guess.",
  "W_ENT_SRC3_CAP": "SOURCE 3   YOUR OWN HANDS",
  "W_ENT_TAP_NOTE": "tap anywhere on the card. any rhythm.",
```

House style check before moving on: lower case bodies, upper case captions, no em or en dashes (the generator rejects them at line 163), no hyphens.

- [ ] **Step 2: Change the two strings that still say "two sources"**

Still in `i18n/en.json`, replace the values of these existing keys:

```json
  "W_RAND_S": "three sources of randomness, mixed.\nno single one decides your wallet.",
  "W_ENT_MIX_T": "WHY THREE SOURCES",
  "W_ENT_MIX_B": "the camera photo, the chip's own noise and the timing of your taps are hashed together, so your words depend on all three.\n\nan attacker has to beat every one. guessing what you pointed at is not enough, knowing the chip is not enough, and your own timing is the part no manufacturer ever touched.\n\nthis is why a dim room cannot give you a weak wallet, and why a bad batch of chips cannot either.",
```

The other 20 files carry translations of these three keys that now say "two". They must be updated too — see step 4.

- [ ] **Step 3: Seed the other 20 locales with the English text**

```bash
python3 - <<'EOF'
import json, pathlib
en = json.load(open('i18n/en.json'))
new = ["W_ENT_TAP_T", "W_ENT_TAP_S", "W_ENT_SRC3_CAP", "W_ENT_TAP_NOTE"]
for p in sorted(pathlib.Path('i18n').glob('*.json')):
    if p.name == 'en.json':
        continue
    d = json.loads(p.read_text())
    for k in new:
        d.setdefault(k, en[k])
    p.write_text(json.dumps(d, ensure_ascii=False, indent=2) + "\n")
    print("seeded", p.name)
EOF
```

This is an English fallback, not a translation, and it follows existing precedent — 17 keys in `de.json` already hold their English text. It ships a working build today and leaves an explicit, greppable list for native review. Do not hand-invent 80 translations here.

- [ ] **Step 4: Fix the three "two sources" strings in every locale**

Each of the 20 non-English files has translated values for `W_RAND_S`, `W_ENT_MIX_T` and `W_ENT_MIX_B` that say two. Leaving them is worse than an English fallback: a confidently translated sentence that undercounts the sources is a lie about the security model. Overwrite them with the English text so they are visibly untranslated rather than quietly wrong:

```bash
python3 - <<'EOF'
import json, pathlib
en = json.load(open('i18n/en.json'))
stale = ["W_RAND_S", "W_ENT_MIX_T", "W_ENT_MIX_B"]
for p in sorted(pathlib.Path('i18n').glob('*.json')):
    if p.name == 'en.json':
        continue
    d = json.loads(p.read_text())
    for k in stale:
        d[k] = en[k]
    p.write_text(json.dumps(d, ensure_ascii=False, indent=2) + "\n")
    print("reset", p.name)
EOF
```

- [ ] **Step 5: Regenerate and check**

```bash
python3 tools/gen_i18n.py
```

Expected: exits 0. Warnings about English-length strings are fine; any `FATAL` or `errors:` output is not.

- [ ] **Step 6: Commit**

```bash
git add i18n/ main/i18n_keys.h main/i18n_tables.c
git commit -m "a third source needs a third caption, and no locale may still say two"
```

---

## Task 5: The tap screen

**Files:**
- Modify: `main/kiss_setup.c` (new screen; `entropy_screen` at line 679; `ent_card` at ~line 600)

- [ ] **Step 1: Add the screen's state and geometry**

Near the other `ENT_*` geometry defines at `main/kiss_setup.c:587-593`, add:

```c
// The tap screen. One card, centred in the 800x480 landscape UI, sized so the
// whole card is the target: there is nothing to aim at and no missed touch.
// It is a raised panel rather than a pill because a 600px pill reads as a
// button that moves money.
#define TAP_CARD_X   100
#define TAP_CARD_Y   130
#define TAP_CARD_W   600
#define TAP_CARD_H   260
#define TAP_SEG_GAP    3
```

and beside the other file-scope screen state:

```c
static lv_obj_t *s_tap_segs[WTAP_TARGET];   // one per counted tap
static lv_obj_t *s_tap_count;               // "31 / 64"
static lv_obj_t *s_tap_card;                // the target, flashed on each tap
```

Add `#include "kiss_tapent.h"` beside the other `main/` includes at the top of the file.

- [ ] **Step 2: Write the tap handler**

Add above the screen builder:

```c
// One tap. The timestamp is taken HERE, in the event callback, not on a timer
// tick: the whole entropy claim is about when the finger landed, and a value
// sampled on a poll would carry the poll's period instead of the hand's.
static void tap_hit_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t p = {0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);

    if (!kiss_tapent_tap((uint64_t)esp_timer_get_time(),
                           esp_cpu_get_cycle_count(),
                           (int16_t)p.x, (int16_t)p.y))
        return;                                  // debounced: no light, no count

    unsigned n = kiss_tapent_count();
    if (n >= 1 && n <= WTAP_TARGET && s_tap_segs[n - 1])
        lv_obj_set_style_bg_color(s_tap_segs[n - 1], OK_COL, 0);
    if (s_tap_count) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u / %u", n, (unsigned)WTAP_TARGET);
        lv_label_set_text(s_tap_count, buf);
    }
    if (s_tap_card)                              // one-frame acknowledgement
        lv_obj_set_style_border_color(s_tap_card, wt_primary(), 0);

    if (n >= WTAP_TARGET) {
        lv_obj_remove_flag(s_tap_card, LV_OBJ_FLAG_CLICKABLE);
        // Hold the full bar so completion is SEEN rather than inferred.
        lv_timer_create(tap_done_cb, 400, NULL);
    }
}
```

Add the includes `esp_timer.h` and `esp_cpu.h` at the top of the file, guarded the way the file already guards device-only headers.

- [ ] **Step 3: Write the completion handler that performs the mix**

Directly above `tap_hit_cb`:

```c
// The only place a seed comes into existence. Three chains in, twelve or
// twenty four words out, and every intermediate wiped on the way through.
static void tap_done_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    uint8_t cam[32], taps[32], seed[32];
    int ok = camera_entropy_chain(cam) == 0 &&
             kiss_tapent_take(taps) == 0 &&
             kiss_entropy_mix3(cam, cam, taps, seed) == 0;
    wally_bzero(cam, sizeof cam);
    wally_bzero(taps, sizeof taps);
    if (ok)
        kiss_setup_entropy(seed, 32);
    wally_bzero(seed, sizeof seed);
    kiss_tapent_reset();
}
```

**This is wrong on purpose and Task 6 fixes it** — `mix3(cam, cam, taps)` passes the camera chain twice because the camera stage already folded the TRNG into it, which makes the "three inputs" claim untrue at the call site. Leave it for one step so the next task's test has something to catch, then fix it there.

- [ ] **Step 4: Build the screen**

```c
static void tap_screen(void)
{
    kiss_tapent_reset();
    memset(s_tap_segs, 0, sizeof s_tap_segs);
    s_tap_count = NULL;
    mk_screen2(tr(STR_W_ENT_TAP_T), tr(STR_W_ENT_TAP_S));

    s_tap_card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_tap_card);
    lv_obj_set_pos(s_tap_card, TAP_CARD_X, TAP_CARD_Y);
    lv_obj_set_size(s_tap_card, TAP_CARD_W, TAP_CARD_H);
    lv_obj_set_style_bg_color(s_tap_card, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(s_tap_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_tap_card, WT_EDGE, 0);
    lv_obj_set_style_border_width(s_tap_card, 1, 0);
    lv_obj_set_style_radius(s_tap_card, 10, 0);
    lv_obj_remove_flag(s_tap_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_tap_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tap_card, tap_hit_cb, LV_EVENT_PRESSED, NULL);

    wt_lbl(s_tap_card, tr(STR_W_ENT_SRC3_CAP), 18, 16, wt_font14(), MUT_COL);

    // 64 segments, not a smooth fill: a segment is a countable event, and a
    // continuous bar would imply a measurement of quality we refuse to claim.
    int inner = TAP_CARD_W - 36;
    int sw = (inner - (WTAP_TARGET - 1) * TAP_SEG_GAP) / WTAP_TARGET;
    for (int i = 0; i < WTAP_TARGET; i++) {
        lv_obj_t *seg = lv_obj_create(s_tap_card);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, 18 + i * (sw + TAP_SEG_GAP), 62);
        lv_obj_set_size(seg, sw, 26);
        lv_obj_set_style_bg_color(seg, WT_EDGE, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        s_tap_segs[i] = seg;
    }

    char buf[16];
    snprintf(buf, sizeof buf, "0 / %u", (unsigned)WTAP_TARGET);
    s_tap_count = wt_lbl(s_tap_card, buf, 18, 108, wt_font_mono28(), INK_COL);
    lv_obj_remove_flag(s_tap_count, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = wt_lbl(s_tap_card, tr(STR_W_ENT_TAP_NOTE), 18, 168,
                         wt_font14(), MUT_COL);
    lv_obj_set_width(n, TAP_CARD_W - 36);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);

    // CANCEL only. Same rule the words screen documents: no screen without an
    // exit. Nothing is staged here, because the seed does not exist yet.
    mk_pill(tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 160, cancel_cb, NULL);
}
```

- [ ] **Step 5: Route capture into it**

In `entropy_screen`'s capture poll, replace the call that currently hands the camera result to `kiss_setup_entropy` with `tap_screen();`. Capture no longer generates.

- [ ] **Step 6: Give the camera-failure branch a way through**

`entropy_screen`'s `else` branch (the one that prints `STR_C_CAM_UNAVAIL` when `camera_entropy_start()` fails) currently dead-ends: there is no CAPTURE pill, so a device with a broken camera cannot build a wallet at all. The tap screen is what rescues it. In that branch, after the two error labels, add a primary pill that goes straight to taps:

```c
        // A dead camera must not be a dead device. Sources 2 and 3 are still
        // available, so the seed loses a source rather than the wallet losing
        // its device. Task 6 makes the mix tolerate the missing chain.
        s_ent_capture = mk_pill(tr(STR_W_ENT_CAPTURE), 48, WT_ACTION_Y, 300,
                                tap_only_cb, NULL);
        wt_pill_primary(s_ent_capture);
```

with, beside the other callbacks:

```c
static void tap_only_cb(lv_event_t *e) { (void)e; tap_screen(); }
```

- [ ] **Step 6: Build and run the simulator**

```bash
sim/build_sim.sh && /tmp/kisssim
```

Walk: new wallet, 12 words, CAPTURE, then click the card 64 times. Expected: segments light one per click, the counter reaches `64 / 64`, and the words screen appears after a short hold.

- [ ] **Step 7: Commit**

```bash
git add main/kiss_setup.c
git commit -m "a card that is its own target, and sixty four things that happened"
```

---

## Task 6: Make the call site tell the truth

**Files:**
- Modify: `main/kiss_setup.c` (`tap_done_cb` from Task 5)
- Modify: `main/camera_spike.c`, `main/camera_spike.h`

Task 5 left `mix3(cam, cam, taps)`. It produces a fine seed and reads as a lie: the whole reason `mix3` is flat is so a reader can see the three sources, and passing one of them twice defeats exactly that. Split the camera stage's two sources apart.

- [ ] **Step 1: Stop the camera folding the TRNG into its own chain**

In `main/camera_spike.c`, in the capture block, keep the TRNG read but store it separately instead of mixing:

```c
      // Sources 1 and 2 stay SEPARATE all the way to the seed, so the one
      // call that builds a wallet names all three inputs. Folding them here
      // would leave that call site passing the same chain twice.
      esp_fill_random(s_ent_trng, sizeof s_ent_trng);
      memcpy(s_ent_hash, s_ent_chain, 32);
      s_ent_done = true;
```

Add `static uint8_t s_ent_trng[32];` beside the other `s_ent_*` state, and zero it in the same place `s_ent_chain` is zeroed on start.

- [ ] **Step 2: Expose it**

In `main/camera_spike.h`, beside `camera_entropy_chain`:

```c
// Source 2 on its own: the chip TRNG read taken at the moment of capture.
// Separate from the frame chain so kiss_setup's mix names three inputs.
int camera_entropy_trng(uint8_t out[32]);
```

and implement it in `camera_spike.c` alongside `camera_entropy_chain`, returning 0 only when `s_ent_done`.

- [ ] **Step 3: Fix the call site**

In `main/kiss_setup.c`, replace the body of `tap_done_cb`:

```c
static void tap_done_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    uint8_t cam[32], trng[32], taps[32], seed[32];
    // A camera that never started leaves its chain all zeroes. That is honest
    // and it is safe: a hash is as strong as its BEST input, so a missing
    // source costs a source, never the seed. The TRNG is read here when the
    // camera stage could not read it, so a dead lens still yields two.
    memset(cam, 0, sizeof cam);
    if (camera_entropy_chain(cam) != 0)
        memset(cam, 0, sizeof cam);
    if (camera_entropy_trng(trng) != 0)
        esp_fill_random(trng, sizeof trng);
    int ok = kiss_tapent_take(taps) == 0 &&
             kiss_entropy_mix3(cam, trng, taps, seed) == 0;
    wally_bzero(cam, sizeof cam);
    wally_bzero(trng, sizeof trng);
    wally_bzero(taps, sizeof taps);
    if (ok)
        kiss_setup_entropy(seed, 32);
    wally_bzero(seed, sizeof seed);
    kiss_tapent_reset();
}
```

- [ ] **Step 4: Verify no caller still mixes two**

```bash
grep -n "kiss_entropy_mix(" main/*.c
```

Expected: exactly one hit, the per-frame fold in `camera_spike.c` (which genuinely takes two inputs). If the capture-time call still appears, step 1 was not applied.

- [ ] **Step 5: Build both targets**

```bash
sim/build_sim.sh && idf.py build
```

Expected: both compile clean.

- [ ] **Step 6: Commit**

```bash
git add main/kiss_setup.c main/camera_spike.c main/camera_spike.h
git commit -m "three inputs named at the only call site that makes a wallet"
```

---

## Task 7: The randomness screen promises the third source

**Files:**
- Modify: `main/kiss_setup.c:713-727` (the equation block in `entropy_screen`)

- [ ] **Step 1: Add the dim third chip**

In `entropy_screen`, in the diagram row currently reading `1 + 2 -> 12 WORDS`, insert the third source before the arrow:

```c
    lv_obj_t *row = wt_diagram_row(eq);
    wt_chip(row, "1", false);
    wt_diagram_op(row, "+");
    wt_chip(row, "2", false);
    wt_diagram_op(row, "+");
    // Source 3 is not collected on this screen, so its chip is drawn inert:
    // a dim chip in an equation reads as a promise the next screen keeps.
    lv_obj_t *c3 = wt_chip(row, "3", false);
    lv_obj_set_style_text_color(c3, WT_DIM, 0);
    lv_obj_set_style_border_color(c3, WT_DIM, 0);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, tr(STR_W_ENT_RESULT), true);
```

`wt_chip` already returns `lv_obj_t *` (`main/kiss_theme.h:280`), so no theme change is needed.

- [ ] **Step 2: Check it still fits**

```bash
sim/build_fitcheck.sh && /tmp/kissfit
```

Expected: no overflow reported for the randomness screen. The row gained a chip and an operator; if the longest locale's `12 WORDS` chip now collides, shrink the operator spacing rather than the chips.

- [ ] **Step 3: Run the overlap check**

```bash
sim/run_overlapcheck.sh
```

Expected: clean for both the randomness screen and the new tap screen.

- [ ] **Step 4: Commit**

```bash
git add main/kiss_setup.c main/kiss_theme.h
git commit -m "the equation counts to three before the screen that gets there"
```

---

## Task 8: Full-suite verification

**Files:** none modified.

- [ ] **Step 1: Host tests**

```bash
sim/build_test.sh && /tmp/kisstest
```

Expected: zero `FAIL:` lines and the runner's final summary reporting 0 failures.

- [ ] **Step 2: Fuzz the parser paths that did not change**

```bash
sim/build_fuzz.sh && /tmp/kissfuzz
```

Expected: clean. This is a regression check, not a test of this feature.

- [ ] **Step 3: Theme and fit**

```bash
sim/build_themecheck.sh && /tmp/kissthemecheck
sim/build_fitcheck.sh && /tmp/kissfit
```

Expected: both clean, including the new tap screen in every locale.

- [ ] **Step 4: Device build**

```bash
idf.py build
```

Expected: compiles, and the binary still fits — check the output against `tools/check_flash_budget.py`.

- [ ] **Step 5: Commit any fixes, then stop**

Do not open a PR from this plan. The device test verdict below has to be satisfied first, and no host result substitutes for it.

---

## Device test verdict

**DEVICE TEST: REQUIRED.**

Every host test above can pass while this feature is worthless. The security claim is a claim about the resolution of a touch panel and a CPU cycle counter, and neither exists in the simulator. Passing `/tmp/kisstest`, the fuzzer and CI is **not** the verdict and does not stand in for it.

Flows to exercise on hardware before this ships:

1. **Cycle counter resolution (the one that can invalidate the design).** Temporarily log `esp_cpu_get_cycle_count()` per tap and confirm the low bits vary across a real session. If the touch driver delivers events on a coarse tick, the 2 bits per tap budget is wrong and the fix is to timestamp in the touch ISR, not to demand more taps.
2. Sixty four real taps light exactly sixty four segments — no double counts from panel bounce, no drops during fast mashing.
3. A press held and dragged across the card counts once, not once per move event.
4. CANCEL from a part filled bar returns to the randomness screen and stages nothing.
5. The whole path with the camera lens physically covered: the tap screen still completes and still produces a wallet.
7. The camera-failure branch specifically (unplug or fault the sensor, not just cover it): CAPTURE reaches the tap screen and the resulting wallet's fingerprint differs across two runs, proving the zeroed camera chain did not fix the seed.
6. Two consecutive wallets created with a deliberately identical scene and identical tap counts produce different fingerprints.

---

## Deliberately not built

- **No entropy meter on the tap screen.** The bar counts events. A quality score would need calibrating against human behaviour and would carry exactly the false assurance the Coldcard postmortem describes.
- **No statistical test of the output.** It would pass against a starved source. What gets verified is structural: that the taps reach the hash, that the count is honest, and that the mix consumes three distinct inputs.
- **No skip, and no source menu.** A menu lets a user land on one source, which is the shape this feature exists to prevent.
- **No regularity or rhythm detection.** A screen that can permanently refuse is a device that cannot make a wallet.
