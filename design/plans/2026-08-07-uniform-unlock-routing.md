# Uniform Unlock Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a configured device and a factory fresh one answer every unlock gesture identically, so the device stops disclosing that a hidden wallet exists.

**Architecture:** The routing decision moves out of `main/main.c` (which no test binary links) into a pure function in `main/kiss_duress.c` (which every test binary links). The new rule ignores the stored stroke entirely: a recognised word opens the decoy, a recognised word plus any modifier stroke opens the passphrase keyboard. `greal` stays in NVS but stops deciding anything observable.

**Tech Stack:** C99, ESP-IDF on device, the desktop simulator for tests, LVGL for the two UI touches.

---

## Why the policy has to move

`unlock_kind()` lives in `main/main.c`. `sim/build_test.sh` links eighteen files from `main/` and `main.c` is not one of them, because it pulls in LVGL, the game and the whole ESP-IDF surface. A routing rule left there cannot be tested on the host at all, which is exactly how the current fork survived.

`main/kiss_duress.c` is already linked into `/tmp/kisstest` and is already pure and host testable, the same way `kiss_tapent.c` and `kiss_dice_q.c` are. The policy belongs there.

## File structure

- **Modify** `main/kiss_duress.h` — add the `WDR_*` outcomes and the `kiss_duress_route` declaration.
- **Modify** `main/kiss_duress.c` — implement `kiss_duress_route`, outside the `ESP_PLATFORM` guard so both builds get one copy.
- **Modify** `main/main.c:1598-1624` — `unlock_kind` classifies the word and the stroke, then asks `kiss_duress_route` for the answer.
- **Modify** `main/kiss_settings.c:1193-1200` — drop the `kiss_session_decoy()` condition around the duress row.
- **Modify** `main/main.c:2326` area — the quiet home line, in the same band and register as the build id.
- **Modify** `i18n/en.json` and the twenty other locales — one new key, one corrected key.
- **Modify** `sim/test_duress.c` — the tests that pin the new rule.

---

### Task 1: The routing policy, as a pure function

**Files:**
- Modify: `main/kiss_duress.h`
- Modify: `main/kiss_duress.c`
- Test: `sim/test_duress.c`

- [ ] **Step 1: Write the failing tests**

Append inside `test_duress_layer()` in `sim/test_duress.c`, just before its `return`:

```c
    // ---- routing policy (uniform: the stored stroke must not change it) ----
    dchk("no word at all opens nothing",
         kiss_duress_route(false, WDG_NONE) == WDR_NONE);
    dchk("a scribble with a stroke still opens nothing",
         kiss_duress_route(false, WDG_CIRCLE) == WDR_NONE);
    dchk("word alone opens the decoy",
         kiss_duress_route(true, WDG_NONE) == WDR_DECOY);
    for (int g = WDG_NONE + 1; g < WDG_N; g++)
        dchk("word plus any stroke reaches the passphrase",
             kiss_duress_route(true, g) == WDR_REAL);

    // The property the whole change exists for: the answer must not depend on
    // what is stored. Before this, drawing the word once told an attacker
    // whether a stroke was configured, because the device either prompted or
    // did not.
    for (int cfg = WDG_NONE; cfg < WDG_N; cfg++) {
        kiss_duress_set(cfg);
        dchk("word alone opens the decoy whatever is configured",
             kiss_duress_route(true, WDG_NONE) == WDR_DECOY);
        dchk("word plus a stroke reaches the passphrase whatever is configured",
             kiss_duress_route(true, WDG_UNDERLINE) == WDR_REAL);
    }
    kiss_duress_set(WDG_NONE);
```

- [ ] **Step 2: Run the tests and watch them fail to build**

```bash
bash sim/build_test.sh
```

Expected: the build fails with `implicit declaration of function 'kiss_duress_route'` and `'WDR_NONE' undeclared`.

- [ ] **Step 3: Declare the contract**

In `main/kiss_duress.h`, after the `WDG_*` enum:

```c
// ---- unlock routing ----
// Which signer a finished draw opens. Values match the legacy unlock_kind
// return so main.c's caller does not have to be rewritten around them.
enum {
    WDR_NONE  = -1,   // not a word: fall through to the game
    WDR_DECOY =  0,   // open the spare now, no prompt
    WDR_REAL  =  1,   // ask for the passphrase
};

// word_ok: the drawing before the final stroke read as the opening word.
// stroke:  WDG_* for the final stroke, WDG_NONE when there was not one.
//
// Deliberately does NOT consult kiss_duress_real(). It used to, and that was
// the whole leak: a device with a stroke configured opened the decoy on the
// bare word while a device without one showed a passphrase keyboard, so one
// gesture told an attacker which kind of device they were holding. Any
// recognised stroke now reaches the passphrase on every device, which costs
// nothing -- the stroke was never the secret, the passphrase is.
int kiss_duress_route(bool word_ok, int stroke);
```

`main/kiss_duress.h` currently has **no includes at all**, so this is required, immediately after `#pragma once`:

```c
#include <stdbool.h>
```

- [ ] **Step 4: Implement it**

In `main/kiss_duress.c`, outside and above the `#ifdef ESP_PLATFORM` storage block so both builds compile one copy:

```c
int kiss_duress_route(bool word_ok, int stroke)
{
    if (!word_ok)
        return WDR_NONE;
    return (stroke > WDG_NONE && stroke < WDG_N) ? WDR_REAL : WDR_DECOY;
}
```

- [ ] **Step 5: Run the tests**

```bash
bash sim/build_test.sh && /tmp/kisstest 2>&1 | grep -E "^FAIL|route|stroke|decoy" | head -20
```

Expected: no `FAIL` lines, and the new `PASS:` lines for the routing checks appear.

- [ ] **Step 6: Commit**

```bash
git add main/kiss_duress.h main/kiss_duress.c sim/test_duress.c
git commit -m "the routing answer stops depending on what is stored"
```

---

### Task 2: Wire the gesture path to it

**Files:**
- Modify: `main/main.c:1598-1624`

- [ ] **Step 1: Replace `unlock_kind`**

Replace the whole function body. The word and stroke classification stays exactly as it is; only the decision at the end changes.

```c
static int unlock_kind(void) {
  // A modifier stroke is classified SEPARATELY from the word, because it
  // changes the word's shape: an underline is wide and low and merges the
  // letters' x-clusters into one blob, which detect_cover_word would reject.
  if (s_strokes >= 5 && s_stroke_n0 >= 12 && s_gn > s_stroke_n0) {
    int bx0 = s_gpt[0].x, bx1 = bx0, by0 = s_gpt[0].y, by1 = by0;
    for (int i = 1; i < s_stroke_n0; i++) {          // bbox of the WORD only
      if (s_gpt[i].x < bx0) bx0 = s_gpt[i].x;
      if (s_gpt[i].x > bx1) bx1 = s_gpt[i].x;
      if (s_gpt[i].y < by0) by0 = s_gpt[i].y;
      if (s_gpt[i].y > by1) by1 = s_gpt[i].y;
    }
    if (detect_cover_word(s_gpt, s_stroke_n0, s_strokes - 1)) {
      int n = 0;
      for (int i = s_stroke_n0; i < s_gn; i++) {
        s_mx[n] = s_gpt[i].x; s_my[n] = s_gpt[i].y; n++;
      }
      int stroke = kiss_duress_classify(s_mx, s_my, n, bx0, by0, bx1, by1);
      if (stroke != WDG_NONE)
        return kiss_duress_route(true, stroke);
      // an unrecognised final scribble is not a modifier: fall through to the
      // plain-word test, which lands on the decoy
    }
  }
  return kiss_duress_route(detect_cover_word(s_gpt, s_gn, s_strokes), WDG_NONE);
}
```

Note what left: the `const int real = kiss_duress_real();` line, the `real != WDG_NONE &&` guard on the modifier branch, the `== real` comparison, and the `real == WDG_NONE ? 1 : 0` fork.

- [ ] **Step 2: Fix the stale comment on the caller**

At `main/main.c:1974` the caller reads:

```c
              else if (kind == 1) {          // the owner's stroke, or no stroke set
```

Replace that comment, because "or no stroke set" is the behaviour being removed:

```c
              else if (kind == 1) {          // a modifier stroke: ask for the passphrase
```

- [ ] **Step 3: Build the simulator**

```bash
bash sim/build_sim.sh
```

Expected: `built /tmp/fruitsim`, no warnings about `kiss_duress_real` being unused (it is still used by Settings).

- [ ] **Step 4: Commit**

```bash
git add main/main.c
git commit -m "one gesture, one answer, on every device"
```

---

### Task 3: The Settings row, unconditional

**Files:**
- Modify: `main/kiss_settings.c:1193-1200`

- [ ] **Step 1: Drop the condition**

Replace:

```c
    const int g = kiss_duress_real();
    if (!(kiss_session_decoy() && g != WDG_NONE)) {
        wt_row(s_scr, tr(STR_I_ROW_DURESS), tr(STR_GD_SET_NOTE),
               g == WDG_NONE ? tr(STR_GD_OFF) : tr(kiss_duress_label_key(g)),
               WT_INK, SG_L_X, SG_FULL_Y, SG_FULL_W,
               duress_cb, NULL);
    }
```

with:

```c
    // Unconditional, and that is the point. This row used to be hidden in a
    // decoy session whenever a stroke was configured, so an attacker who knew
    // where to look could catch a coerced owner handing over the spare. It is
    // safe to show now only because Task 2 removed the behavioural fork: there
    // is no longer anything for the row's presence to corroborate.
    const int g = kiss_duress_real();
    wt_row(s_scr, tr(STR_I_ROW_DURESS), tr(STR_GD_SET_NOTE),
           g == WDG_NONE ? tr(STR_GD_OFF) : tr(kiss_duress_label_key(g)),
           WT_INK, SG_L_X, SG_FULL_Y, SG_FULL_W,
           duress_cb, NULL);
```

- [ ] **Step 2: Check whether `kiss_session_decoy` is now unused in this file**

```bash
grep -n "kiss_session_decoy" main/kiss_settings.c
```

If there are no remaining hits, remove the include that provided it only for this use. If there are other hits, leave the include alone.

- [ ] **Step 3: Build and walk it**

```bash
bash sim/build_sim.sh && bash sim/run_overlapcheck.sh 2>&1 | tail -4
```

Expected: `text overlap gate: 0 findings across 21 locales`. The row now renders in sessions the walk previously skipped it in, so a non zero count here means the row does not fit somewhere and the layout needs attention before going on.

- [ ] **Step 4: Commit**

```bash
git add main/kiss_settings.c
git commit -m "the row that proves nothing can be shown to everyone"
```

---

### Task 4: The copy, in 21 locales

Two strings. One is new, one is now false.

**Files:**
- Modify: `i18n/en.json` and the twenty other `i18n/*.json`
- Regenerate: `main/i18n_keys.h`, `main/i18n_tables.c`

- [ ] **Step 1: Correct `GD_DONE_B`, in a way that works in every language**

English currently reads:

> the stroke is not a key: the real wallet still asks for your passphrase.\n\nchange this later from SETTINGS, inside your real wallet.

That second sentence is the same tell as the row, written down. It is **not** an English only problem: Japanese ends 「あとから本物のウォレットの設定で変更できます。」, which says the same thing. A find and replace on the English clause would silently edit nothing in twenty locales and leave the tell in place.

So the operation is **drop the second paragraph entirely**, which is mechanical and correct in every language: split on the blank line and keep the first part. What is lost is the "change it later in SETTINGS" pointer, and that is acceptable because Task 3 just made the Settings row visible in every session, so the pointer is no longer the only way to find it.

Result in English:

> the stroke is not a key: the real wallet still asks for your passphrase.

- [ ] **Step 2: Add the home hint key**

New key `H_WAYS_IN_HINT`, English:

> a stroke after the word asks for a passphrase

This sentence is true on every device, including one that has never been configured and one whose owner has no passphrase. It describes the product, not this device, which is what makes it safe to show in a decoy session.

- [ ] **Step 3: Apply both to all 21 locales**

```bash
python3 - <<'PY'
import json, glob, pathlib
EN_HINT = "a stroke after the word asks for a passphrase"
for p in sorted(glob.glob("i18n/*.json")):
    d = json.loads(pathlib.Path(p).read_text())
    # Keep only the first paragraph. Language independent: every locale writes
    # the "change it later inside your real wallet" pointer as its own second
    # paragraph, so splitting on the blank line removes it without needing to
    # understand any of them.
    if "GD_DONE_B" in d:
        d["GD_DONE_B"] = d["GD_DONE_B"].split("\n\n")[0]
    d["H_WAYS_IN_HINT"] = EN_HINT     # English everywhere for now; see step 4
    pathlib.Path(p).write_text(json.dumps(d, ensure_ascii=False, indent=2) + "\n")
    print("updated", p)
PY
```

Verify the paragraph really did come off in a non English locale before moving on:

```bash
python3 -c "
import json
for loc in ('en','ja','ru'):
    print(loc, repr(json.load(open('i18n/%s.json' % loc))['GD_DONE_B']))"
```

Expected: each value is a single paragraph with no `\n\n` in it.

- [ ] **Step 4: Flag the untranslated string**

The twenty non English locales now carry English for `H_WAYS_IN_HINT`. That is deliberate and temporary: it keeps the generator's "every locale carries every key" rule satisfied so CI stays green, and English on a Japanese screen is a visible prompt to translate rather than a silent gap. Open a follow up for translation before this ships to users.

- [ ] **Step 5: Regenerate and check no glyph was gained**

```bash
python3 tools/gen_i18n.py
python3 -c "
import subprocess,glob
for f in sorted(glob.glob('tools/fonts/glyphs_*.txt')):
    old=subprocess.run(['git','show','HEAD:'+f],capture_output=True,text=True).stdout.strip()
    g=[c for c in open(f,encoding='utf-8').read().strip() if c not in old]
    print(f, 'gained', ''.join(g) or 'none')"
```

Expected: `gained none` for every file. The new string is ASCII and the corrected one only removes text, so a gained glyph here means the script above did something unintended.

- [ ] **Step 6: Commit**

```bash
git add i18n main/i18n_keys.h main/i18n_tables.c
git commit -m "the copy stops naming which wallet the row lives in"
```

---

### Task 5: The home hint line

**Files:**
- Modify: `main/main.c` near `:2326`

- [ ] **Step 1: Place it in the build id's band**

`main/main.c:2326` builds the small build id line:

```c
  s_home_build_id = kiss_build_id_make(s_wallet, 48, 424, false, false);
```

Add the hint beside it, in the same quiet register, right after that line:

```c
  // Same band and weight as the build id: a standing fact about the product,
  // not a notification. It is shown in EVERY session, decoy included, which is
  // what makes it safe -- a line that appears only for some owners would be
  // the tell this whole change removes.
  wt_note(s_wallet, tr(STR_H_WAYS_IN_HINT), 400, 424, 352, 24);
```

- [ ] **Step 2: Verify the geometry against the theme kit**

```bash
grep -n "define WT_NOTE\|lv_obj_t \*wt_note" main/kiss_theme.h main/kiss_theme.c | head
```

Confirm `wt_note(parent, text, x, y, w, h)` matches that call. If the signature differs, use the one in the header rather than the call above.

- [ ] **Step 3: Walk it in 21 locales**

```bash
bash sim/build_sim.sh && bash sim/run_overlapcheck.sh 2>&1 | tail -4
bash sim/build_fitcheck.sh && /tmp/kissfit
```

Expected: `0 findings across 21 locales`, and fitcheck passes. A long translation at 352px is the likely failure; widen toward the build id or drop to two lines if it fires.

- [ ] **Step 4: Commit**

```bash
git add main/main.c
git commit -m "the way in is written where every owner can read it"
```

---

### Task 6: Full gate sweep and the device verdict

- [ ] **Step 1: Run every gate**

```bash
bash sim/build_test.sh && /tmp/kisstest
bash sim/build_fitcheck.sh && /tmp/kissfit
bash sim/build_themecheck.sh && /tmp/kisstheme
bash sim/build_osdcheck.sh && /tmp/kissosd
bash sim/build_sim.sh && bash sim/run_overlapcheck.sh
python3 tools/gen_i18n.py && git diff --exit-code -- main/i18n_keys.h main/i18n_tables.c 'tools/fonts/glyphs_*.txt'
```

Expected: every one green, and the last command silent.

- [ ] **Step 2: Record the verdict**

**DEVICE TEST: REQUIRED.** None of the gates above can see this change. The gesture path is touch input on a real panel, the simulator drives it with scripted coordinates, and `unlock_kind` is not linked into any test binary. Passing gates are not the verdict and do not justify skipping it.

On hardware, on a device with a stroke configured and again on one without:

1. Draw the word alone. Both devices must open the decoy with no prompt. This is the property: they now behave identically.
2. Draw the word then each of the six strokes in turn. Every one must reach the passphrase keyboard, on both devices.
3. Draw the word then an unrecognised scribble. Must land on the decoy, never the keyboard.
4. Draw a single flat swipe and a tap. Must stay in the game.
5. Open Settings in a decoy session and confirm the duress row is present.
6. Confirm the home hint line is readable and does not collide with the build id.
