# On-device Signature Fingerprint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After signing, show a short `SIGNATURE` fingerprint of the signature bytes on both signed screens, so two independently trusted units are compared by eye instead of by diffing files.

**Architecture:** A pure host-testable function in `wallet_psbt.c` hashes the signed PSBT's signature bytes to 8 hex. `do_sign_cb` computes it once; the SD and QR signed screens render it, with a `?` help chip opening a short explainer — the same pattern the entropy screen uses.

**Spec:** `docs/specs/signature-fingerprint.md`

---

## File Structure

| File | Change |
|---|---|
| `main/wallet_psbt.h` / `.c` | Add `wallet_psbt_sig_fingerprint()`. |
| `sim/test_crypto.c` | Host tests: golden fingerprint, stable, differs per tx. |
| `main/i18n_keys.h`, `main/i18n_tables.c` | 3 English-only keys (hand-added, ADDENDUM pattern). |
| `main/wallet_sign.c` | Compute in `do_sign_cb`; render on `done_screen` + `qr_out_screen`; `?` explainer. |

Reused as-is: `wt_help_chip` (`wallet_theme.h:199`), `mk_screen`/`mk_pill`/`mk_lbl`/`wt_note` (`wallet_sign.c:201-236`), the sig accessors — ECDSA `input->signatures` map (`test_crypto.c:473`), taproot `psbt_fields` key `0x13` (`test_sp.c:418-425`).

---

## Task 1: `wallet_psbt_sig_fingerprint`

**Files:** `main/wallet_psbt.h`, `main/wallet_psbt.c`, `sim/test_crypto.c`

- [ ] **Step 1: Write the failing test**

In `sim/test_crypto.c`, inside the per-type sign roundtrip, after the golden-vector `chk` (around `sim/test_crypto.c:477`), add for the native case:

```c
if (strcmp(label, "native") == 0) {
    char fp[9] = {0};
    chki("sig fingerprint rc", wallet_psbt_sig_fingerprint(sb, sw, fp), 0);
    // sha256(native input signature bytes)[:4], computed independently:
    //   python3 -c "import hashlib;print(hashlib.sha256(bytes.fromhex('<SV_ECDSA_NATIVE>')).hexdigest()[:8])"
    chk("sig fingerprint is the golden code", fp, "a1e0d4c5");
}
```

- [ ] **Step 2: Run, verify it fails**

Run: `sim/build_test.sh && /tmp/kisstest`
Expected: build fails, `wallet_psbt_sig_fingerprint` undeclared.

- [ ] **Step 3: Declare it**

In `main/wallet_psbt.h`, after `wallet_psbt_sign` (line 107):

```c
// First 8 lower-case hex of sha256 over every input's signature bytes,
// concatenated in input order (ECDSA partial sigs = DER+sighash; taproot
// key sig = the 64/65-byte 0x13 field). Writes 8 chars + NUL to out.
// Signatures only, so it is transport- and PSBT-framing-independent: any
// signer that produced the same signatures yields the same fingerprint.
// Nonzero on a parse failure or a PSBT with no signatures.
int wallet_psbt_sig_fingerprint(const uint8_t *signed_psbt, size_t len,
                                char out[9]);
```

- [ ] **Step 4: Implement it**

In `main/wallet_psbt.c` (it already includes `wally_psbt_members.h`, `wally_crypto.h`, `wally_map.h`). Add near the other public functions:

```c
int wallet_psbt_sig_fingerprint(const uint8_t *signed_psbt, size_t len,
                                char out[9])
{
    if (!signed_psbt || !out)
        return -1;
    struct wally_psbt *p = NULL;
    if (wally_psbt_from_bytes(signed_psbt, len, 0, &p) != WALLY_OK || !p)
        return -1;
    // Accumulate signature bytes into a buffer, then one SHA-256.
    uint8_t acc[4096];
    size_t n = 0;
    int any = 0;
    for (size_t i = 0; i < p->num_inputs; i++) {
        const struct wally_map *sigs = &p->inputs[i].signatures;
        for (size_t j = 0; j < sigs->num_items; j++) {
            const struct wally_map_item *it = &sigs->items[j];
            if (n + it->value_len > sizeof acc) { wally_psbt_free(p); return -1; }
            memcpy(acc + n, it->value, it->value_len);
            n += it->value_len; any = 1;
        }
        const struct wally_map_item *tap =
            wally_map_get_integer(&p->inputs[i].psbt_fields, 0x13);
        if (tap && (tap->value_len == 64 || tap->value_len == 65)) {
            if (n + tap->value_len > sizeof acc) { wally_psbt_free(p); return -1; }
            memcpy(acc + n, tap->value, tap->value_len);
            n += tap->value_len; any = 1;
        }
    }
    wally_psbt_free(p);
    if (!any) { wally_bzero(acc, sizeof acc); return -1; }
    uint8_t h[32];
    int rc = wally_sha256(acc, n, h, 32);
    wally_bzero(acc, sizeof acc);
    if (rc != WALLY_OK)
        return -1;
    static const char HEX[] = "0123456789abcdef";
    for (int k = 0; k < 4; k++) {
        out[k * 2]     = HEX[h[k] >> 4];
        out[k * 2 + 1] = HEX[h[k] & 0xf];
    }
    out[8] = 0;
    return 0;
}
```

- [ ] **Step 5: Run, verify pass**

Run: `sim/build_test.sh && /tmp/kisstest`
Expected: `PASS: sig fingerprint rc`, `PASS: sig fingerprint is the golden code`, 0 FAIL.

- [ ] **Step 6: Add the "differs + stable" assertions**

In the same native block:

```c
    // stable across a re-sign (determinism), and different for a different tx
    char fp2[9] = {0};
    wpsbt_summary_t sd; uint8_t rb[4096]; size_t rw = 0;
    wallet_psbt_load(pb, pl, &sd);
    wallet_psbt_sign(rb, sizeof rb, &rw);
    wallet_psbt_sig_fingerprint(rb, rw, fp2);
    chkb("sig fingerprint stable across re-sign", strcmp(fp, fp2) == 0);
    wallet_psbt_free();
```

The "differs per tx" case is covered for free: the legacy/nested/native rounds each assert their own golden code, and `a1e0d4c5` (native) ≠ `18d8626c` (legacy).

- [ ] **Step 7: Run + commit**

Run: `sim/build_test.sh && /tmp/kisstest` (0 FAIL)

```bash
git add main/wallet_psbt.h main/wallet_psbt.c sim/test_crypto.c
git commit -m "a fingerprint of the signatures, hashed where they are made"
```

---

## Task 2: the three strings

**Files:** `main/i18n_keys.h`, `main/i18n_tables.c`

Follow the house pattern (English-only, hand-added to the generated files, `tr()` falls back — the same ADDENDUM-02 convention used for the tap screen; do NOT run `gen_i18n.py`, which would erase the ADDENDUM comments).

- [ ] **Step 1: enum keys**

In `main/i18n_keys.h`, in the `STR_S_*` sign block (near `STR_S_SIGNED_T`), add with an ADDENDUM comment:

```c
    // ADDENDUM-03: the signed screen's signature fingerprint + its ? explainer.
    // English only; tr() falls back for the other locales until the locale pass.
    // See docs/specs/signature-fingerprint.md.
    STR_S_SIG_FP_CAP,
    STR_S_SIG_FP_HELP_T,
    STR_S_SIG_FP_HELP_B,
```

- [ ] **Step 2: English values**

In `main/i18n_tables.c`, in `tbl_en`, beside the other `STR_S_*` entries:

```c
    [STR_S_SIG_FP_CAP] = "SIGNATURE",
    [STR_S_SIG_FP_HELP_T] = "SIGNATURE CHECK",
    [STR_S_SIG_FP_HELP_B] = "this code comes from the signature itself.\n\nsign the same transaction on another signer you trust. if it shows the same code, neither device changed anything.\n\na different code means one of them did.",
```

House-style check: lower-case body, upper-case caption, no hyphens, no em/en dashes.

- [ ] **Step 3: build + commit**

Run: `sim/build_test.sh && /tmp/kisstest` (compiles, 0 FAIL — strings unused yet is fine)

```bash
git add main/i18n_keys.h main/i18n_tables.c
git commit -m "the signed screen names its signature, and explains it once behind a ?"
```

---

## Task 3: compute the fingerprint in the sign flow

**Files:** `main/wallet_sign.c`

- [ ] **Step 1: add state**

Near the other file-scope statics (around `wallet_sign.c:106-116`):

```c
static char s_sig_fp[9];               // fingerprint of the just-signed PSBT
```

- [ ] **Step 2: compute before the exit branch**

In `do_sign_cb` (`wallet_sign.c:367`), right after a successful `wallet_psbt_sign` and before `mark_used_receives()`:

```c
    if (wallet_psbt_sig_fingerprint(s_out, sw, s_sig_fp) != 0)
        s_sig_fp[0] = 0;               // absent aid, still a correct screen
```

- [ ] **Step 3: build**

Run: `sim/build_sim.sh` — compiles clean (value not shown yet).

- [ ] **Step 4: commit**

```bash
git add main/wallet_sign.c
git commit -m "the signature gets its fingerprint the moment it exists"
```

---

## Task 4: render on the SD screen + the ? explainer

**Files:** `main/wallet_sign.c`

- [ ] **Step 1: the explainer callback**

Above `done_screen` (`wallet_sign.c:313`):

```c
static void sig_help_back_cb(lv_event_t *e);   // fwd: rebuilds nothing, just closes the panel

static void sig_fp_help_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL;
    mk_screen(parent, tr(STR_S_SIG_FP_HELP_T), NULL);
    wt_note(s_scr, tr(STR_S_SIG_FP_HELP_B), 48, 118, 704, 260);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, sig_help_back_cb);
}
```

Because leaving the panel must return to the signed screen (the PSBT is signed and saved), `sig_help_back_cb` rebuilds the SD done screen from the stashed `s_cur`/`s_sig_fp`. Simplest: have it call `done_screen(<the saved outname>)`. Store the outname in a static `s_done_name[SD_NAME_LEN + 8]` set in `do_sign_cb` before `done_screen`, and have `sig_help_back_cb` call `done_screen(s_done_name)`.

- [ ] **Step 2: render the line on `done_screen`**

In `done_screen` (`wallet_sign.c:313`), after the filename label (y=230) and before the note (y=284), add:

```c
    if (s_sig_fp[0]) {
        char code[12];   // "A1B2 C3D4" upper-cased, grouped 4 4
        snprintf(code, sizeof code, "%c%c%c%c %c%c%c%c",
                 toupper((unsigned char)s_sig_fp[0]), toupper((unsigned char)s_sig_fp[1]),
                 toupper((unsigned char)s_sig_fp[2]), toupper((unsigned char)s_sig_fp[3]),
                 toupper((unsigned char)s_sig_fp[4]), toupper((unsigned char)s_sig_fp[5]),
                 toupper((unsigned char)s_sig_fp[6]), toupper((unsigned char)s_sig_fp[7]));
        lv_obj_t *cap = mk_lbl(tr(STR_S_SIG_FP_CAP), 0, 258, wt_font14(), MUT_COL);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, -70, 258);
        lv_obj_t *val = mk_lbl(code, 0, 258, wt_font_mono14(), INK_COL);
        lv_obj_align(val, LV_ALIGN_TOP_MID, 40, 258);
        wt_help_chip(s_scr, 470, 258, MUT_COL, sig_fp_help_cb, NULL);
    }
```

Add `#include <ctype.h>` at the top if not present. Colours: reuse the file's existing `MUT_COL`/`INK_COL` defines (present, used by `done_screen`'s note). Exact x offsets are tuned in Step 4 against the fit check; the shape is caption + mono code + `?`.

- [ ] **Step 3: `sig_help_back_cb`**

```c
static void sig_help_back_cb(lv_event_t *e) { (void)e; done_screen(s_done_name); }
```

and in `do_sign_cb`, before `done_screen(outname);`, `snprintf(s_done_name, sizeof s_done_name, "%s", outname);` (add `static char s_done_name[SD_NAME_LEN + 8];`).

- [ ] **Step 4: build, walk the sim, fit-check**

```bash
sim/build_sim.sh && /tmp/fruitsim
```

Confirm the SD sign walk reaches a signed screen showing `SIGNATURE  XXXX XXXX` and a `?`; tapping `?` (the sim walk can be extended) shows the panel. Then:

```bash
bash sim/build_fitcheck.sh && /tmp/kissfit
bash sim/run_overlapcheck.sh
```

Expected: clean; adjust the x offsets / y until the caption, code and `?` do not overlap and clear the action bar.

- [ ] **Step 5: commit**

```bash
git add main/wallet_sign.c
git commit -m "the SD signed screen shows the code, with the why one tap away"
```

---

## Task 5: render on the QR screen

**Files:** `main/wallet_sign.c`

The right column of `qr_out_screen` (`wallet_sign.c:1409`) is dense: part counter y=124, notes y=168 and y=201, EASY SCAN pill y=244, its note y=304, DONE in the action bar. The fingerprint needs one 14px line without pushing anything into the action band (the mistake the comment at line 1423 records).

- [ ] **Step 1: place the line**

Add the same caption + mono code block as Task 4, positioned in the right column at the first free 14px slot found against the checks — candidate y=224 (between the y=201 note and the y=244 pill), shrinking the y=304 EZ note by one line if the slot does not fit. The `?` chip is optional here (Task 4's SD chip already carries the explainer, and both screens show the same code); add it only if it fits without crowding.

```c
    if (s_sig_fp[0]) {
        char code[12];
        snprintf(code, sizeof code, "%c%c%c%c %c%c%c%c", /* same upper-casing as done_screen */ );
        mk_lbl(tr(STR_S_SIG_FP_CAP), 430, 224, wt_font14(), MUT_COL);
        mk_lbl(code, 560, 224, wt_font_mono14(), INK_COL);
    }
```

Factor the caption+code rendering into a small local helper (`draw_sig_fp(int x, int y, bool with_chip)`) so `done_screen` and `qr_out_screen` share it rather than duplicating the upper-casing.

- [ ] **Step 2: build + checks**

```bash
sim/build_sim.sh && /tmp/fruitsim
bash sim/build_fitcheck.sh && /tmp/kissfit
bash sim/run_overlapcheck.sh
```

Expected: the QR signed screen shows the code; the QR card and every pill still clear the action bar; overlap check clean for both signed screens in every locale.

- [ ] **Step 3: commit**

```bash
git add main/wallet_sign.c
git commit -m "the QR signed screen carries the same code as the card it hands back"
```

---

## Task 6: full verification

- [ ] **Host suite:** `sim/build_test.sh && /tmp/kisstest` — 0 FAIL, including the new fingerprint asserts.
- [ ] **Fuzz:** `bash sim/build_fuzz.sh && /tmp/kissfuzz` — unchanged, regression check.
- [ ] **Layout:** `bash sim/build_fitcheck.sh && /tmp/kissfit` and `bash sim/run_overlapcheck.sh` — both signed screens clean in every locale.
- [ ] **Device build:** `docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project espressif/idf:v6.0.1 idf.py -B build build` — compiles; binary fits (`tools/check_flash_budget.py`).
- [ ] Do not open a PR until the device verdict below is satisfied.

---

## Device test verdict

**DEVICE TEST: REQUIRED.**

The feature adds content to both signed screens — a display path — and its whole
purpose is a visual comparison a person performs on hardware. Passing the host
suite confirms the hash, not the screen, and is not the verdict. Flows:

1. SD sign: the `SIGNATURE` line is legible under the filename, not colliding
   with the note or the action bar.
2. Same PSBT + seed on a second unit: the two codes match.
3. A different PSBT: the code differs.
4. Same PSBT twice on one unit: the code is identical.
5. QR sign: the code appears in the right column, nothing pushed into the action
   band, and it equals the code the SD path showed for the same transaction.
6. A silent-payment spend: a code still appears (taproot signature path).
7. The `?` chip opens the explainer and BACK returns to the signed screen with
   the code still shown.

---

## Deliberately not built

- **No setting to toggle it.** It is a small always-on line; a toggle would hide
  it from the users it protects and add a setting for nothing.
- **No threat prose on the signed screen.** The screen shows a labelled number;
  the meaning is behind the `?` and in `security-plan.md`.
- **No whole-PSBT hash.** Signatures only, so a non-KISS signer that signed the
  same way still matches.
