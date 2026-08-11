# Dice Entropy — Design Spec

Date: 2026-07-30
Status: approved for planning
Branch context: builds beside the existing three-source (camera + chip TRNG + taps) setup path.

## 1. Motivation

kiss-signer already generates seeds from three on-device sources folded together:
`SHA256(camera ‖ chip_TRNG ‖ taps)`. That path is convenient and multi-source, but
every leg is **measured by the device**. The owner cannot independently reproduce the
result, so they must trust that the firmware used the entropy it claims to have used.

This spec adds a **second, alternate setup path**: dice-only, off-device entropy the
owner supplies and can verify. It is the concrete expression of "not your entropy, not
your keys." It does **not** replace the tap path — the owner chooses one at setup.

## 2. Threat model

The design is judged against these adversaries, in priority order:

1. **The device cannot be trusted to be honest about randomness.**
   Malicious or buggy firmware could show a convincing entropy screen and quietly seed
   from a known value. Defense: the seed on this path is a pure function of the owner's
   dice, recomputable on any offline machine. The trust root is *outside the device*.

2. **Weak-RNG classes (Milk Sad, Randstorm, the Coldcard `#ifdef` fallback).**
   These all come from an RNG that is predictable or a fallback that silently degrades.
   Defense: **there is no RNG in this path.** No timestamp, no PRNG, no build-flag
   fallback to misconfigure. Immune by construction.

3. **LLM-speed adversarial reproduction.**
   Assume that the moment a flaw exists — disclosed, committed, or merely present in a
   public repo — an adversary with LLM assistance will find and reproduce it within
   hours, often before the vendor can react. The only durable defense is to leave
   **nothing to find**: the entire seed derivation on this path is a few branchless
   lines over standard primitives (SHA256, BIP39), auditable in one screen by a human or
   a model, with no hidden state and no vendor RNG to distrust. Minimal surface = minimal
   reproduction target. KISS is the security property, not a nicety.

4. **Quantum.**
   Grover's algorithm halves brute-force cost against a symmetric secret. Defense here is
   **12 words (128-bit) plus a strong BIP39 passphrase** — the passphrase is the quantum
   lever, adding entropy on top of the seed and guarding the real funds, consistent with
   the device's existing "creation makes 12 words" rule. 24 words stays available for
   restore but is not the creation default. (The larger quantum risk to Bitcoin is Shor
   breaking exposed public keys at *signing* time — protocol-level, out of scope for seed
   generation.)

### Explicitly out of scope
- **Dark Skippy** is a *signing-time* nonce-exfiltration attack, not an entropy problem.
  It gets its own spec in the signing path (deterministic / anti-exfil nonces,
  reproducible builds). This feature does not address it and must not imply it does.
- **Post-quantum signatures** are a protocol concern, not a signer entropy feature.

## 3. Goals / non-goals

Goals
- A dice-only setup path whose seed is independently verifiable off-device.
- Recipe simple enough to reproduce with a shell one-liner and any BIP39 tool.
- Hard entropy floor: refuse to build a seed below the roll threshold.
- 12- or 24-word output, matching the existing `s_count` selection; creation defaults to
  12 words (with a strong passphrase as the quantum lever); 24 stays available for restore.

Non-goals (YAGNI)
- d20 support. d6 only for v1. (`kiss_dice` leaves room to add it later.)
- Folding dice with the on-device sources (breaks verifiability — the whole point).

Reversed non-goal (2026-08-01): this spec originally waved bias checking away
with "SHA256 whitens; the floor is conservative". That reasoning was backwards.
Whitening is exactly why the rolls must be judged raw: 50 presses of one key
hash into words that look as good as anyone's, and nothing downstream can ever
notice. `kiss_dice_q.c` now judges the digit string before the hash — face
entropy, step entropy (the same statistic on consecutive differences, which is
a bijection for fair rolls, so one threshold serves both) and a repeated-block
scan. The bar `WD_RATE = 2050` milli-bits per roll sits between log2(4) and
log2(5), giving the rule a hand-checkable meaning: four or fewer distinct
faces always warns, five or six is judged on levelness. Exact enumeration of
all 3,478,761 six-bin compositions of 50 puts the false alarm at 1 in ~1.1
million honest sessions. (Krux's `min_bits - 2` rule fires on 49.5% of honest
50-roll sessions by the same enumeration — the plug-in estimator runs ~3.6
bits low at N=50 — which is why the threshold was derived, not copied.) The
verdict warns and offers ROLL MORE with the rolls kept; it never blocks,
because dice entropy is the owner's trust root, not the device's.

## 4. The recipe (normative — this is what makes it verifiable)

1. The owner rolls a physical **d6** and enters each result, 1–6.
2. The device accumulates the results as an **ASCII digit string** `S`, in roll order,
   no separators. Example after six rolls of 1,2,3,4,5,6: `S = "123456"`.
3. `H = SHA256(S)` — a 32-byte value over the exact bytes of `S`.
4. Entropy fed to BIP39:
   - **24 words:** `entropy = H` (all 32 bytes).
   - **12 words:** `entropy = H[0..16]` (first 16 bytes).
5. `mnemonic = BIP39(entropy)` via the existing `kiss_setup_entropy(entropy, need)`,
   where `need = s_count == 24 ? 32 : 16`.

There is deliberately nothing kiss-specific in steps 3–5. Verification anchor:

```
printf '123456' | sha256sum
# 8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92
```

The owner can run the same over their real roll string, feed the hex (or its first 16
bytes) to any offline BIP39 tool, and confirm the words the device showed.

### Roll-count floor
`log2(6) ≈ 2.585 bits/roll`, so:
- **12 words / 128 bit → minimum 50 rolls** (50 × 2.585 ≈ 129 bit).
- **24 words / 256 bit → minimum 99 rolls** (99 × 2.585 ≈ 256 bit).

SHA256 output is always 256 bits wide, but the *entropy* is bounded by the rolls, so the
floor is enforced on **roll count**, not output width. Below the floor: no seed.

## 5. Components

### `kiss_dice` — pure, host-testable module (mirrors `kiss_tapent`)
State: the digit string and a count. No UI, no clock, no globals beyond the buffer.

```c
void         kiss_dice_reset(void);
// Append one face (1..6). Returns 1 if accepted, 0 if invalid or the buffer is full.
int          kiss_dice_roll(int face);
// Remove the last accepted roll. Returns 1 if one was removed, 0 if empty. (Undo.)
int          kiss_dice_undo(void);
// Rolls accepted so far.
unsigned     kiss_dice_count(void);
// Read-only view of the digit string, for the verification display. NUL-terminated.
const char  *kiss_dice_digits(void);
// SHA256 the digits and copy the first `len` bytes (16 or 32) to `out`.
// Returns 0 on success; -1 if len is not 16/32, or count is below the floor for len.
int          kiss_dice_take(uint8_t *out, unsigned len);
```

- Floor check inside `take`: `len==32 ⇒ count ≥ 99`; `len==16 ⇒ count ≥ 50`.
- `reset` and `take` wipe the digit buffer with `wally_bzero` — the roll string is seed
  material and must not linger (same discipline as `kiss_tapent`/`tap_done_cb`).
- Buffer size `DICE_MAX` ≥ 120 (comfortably above 99).

### `dice_screen` — UI in `kiss_setup.c`
- Entered as a new method from the setup choice (see §6).
- A d6 keypad: six buttons `1`–`6`. Each press calls `kiss_dice_roll` and updates a
  tally readout `NN / <floor>` (floor is 50 or 99 per `s_count`), styled like the tap
  segment counter.
- An **undo** control (backspace) → `kiss_dice_undo`.
- The "done" affordance is disabled until `kiss_dice_count() ≥ floor`.
- On done: `kiss_dice_take(entropy, need)`; on success →
  `kiss_setup_entropy(entropy, need)` → the existing words / spot-check / write-down
  flow. Wipe `entropy` immediately after.
- **Verification card** before "done" commits: show the SHA256 hex of the current digit
  string and a one-line note — "verify: SHA256 of your rolls, offline." (Full 64-hex is
  scrollable; the digit string is viewable so the owner can confirm what was hashed.)
- **CANCEL** wipes the buffer (`kiss_dice_reset`) and returns — no screen without an
  exit, matching the tap screen rule.

### Failure path
Reuse the existing `NO SEED MADE` card + `TRY AGAIN` (`STR_W_ENT_FAIL_T/B`,
`ent_retry_cb`). `kiss_dice_take` failing after the floor is met cannot happen in
practice (count ≥ floor, len ∈ {16,32}), but if it ever does, the owner sees the card
instead of a dead button — same defensive posture as `tap_done_cb`.

### i18n
New English-only strings (`tr()` falls back for the other locales):
`STR_W_DICE_T`, `STR_W_DICE_S`, `STR_W_DICE_TALLY_FMT` (`"%u / %u"`),
`STR_W_DICE_VERIFY_NOTE`, `STR_W_DICE_UNDO`, `STR_W_CHOOSE_DICE`, `STR_W_DICE_NOTE`.
Voice matches the existing entropy copy (lowercase notes, no hyphens, ruthlessly short).

## 6. Setup-flow integration

Current: `choose_screen` offers `CREATE NEW` (`new_cb` → word-count → camera
`entropy_screen` → `tap_screen` → words) and `RESTORE`.

Change: after the owner picks *create new* and a word count, present a **method choice**:
- **Camera + taps** → existing `entropy_screen` (Path 1, unchanged).
- **Dice** → `dice_screen` (Path 2, this spec).

Both converge on `kiss_setup_entropy(entropy, need)`, so the words / spot-check /
write-down / storage flow downstream is shared and untouched. The method choice is the
only new branch in the setup wizard.

## 7. Testing (`sim/test_dice.c`, host)

Known-answer vectors are the proof of verifiability and live in CI:
1. **KAT:** a fixed roll string → assert `kiss_dice_take` output equals the hex from
   `printf '<string>' | sha256sum` (and its first 16 bytes for the 12-word case).
2. **BIP39 cross-check:** that entropy → assert the mnemonic equals the output of an
   independent reference BIP39 implementation (documented in the test).
3. **Floor enforcement:** `take` returns -1 at 49 rolls / 98 rolls; 0 at 50 / 99.
4. **Undo:** roll N, undo, count decrements, digit string shortens correctly.
5. **Invalid face:** `kiss_dice_roll(0)` / `roll(7)` rejected, count unchanged.
6. **Wipe:** after `reset`/`take`, the buffer is zeroed.

Sim build (`build_sim.sh`) and the full host suite (`build_test.sh`) must stay green.

## 8. Device-test note

The dice path is pure arithmetic over user input — no camera, timing, or RNG — so its
*logic* is fully covered on the host. On device, verify only the UI wiring: the keypad
registers presses, the tally gates at 50/99, undo works, and the resulting words match an
offline `sha256sum` of the same rolls. That last check is the whole feature working
end-to-end and should be part of device acceptance.

## 9. Resolved decisions

- **Verification card:** show a short fingerprint by default — the first 8 bytes of the
  SHA256 as 16 hex chars — with a "view full" control that reveals all 64. Enough to spot
  a mismatch at a glance without filling the screen; the full value is one tap away for a
  complete check.
- **All-identical rolls:** a single non-blocking warning if every entered face is the
  same (e.g. sixty 4s) — "that doesn't look rolled. continue?" — then honor the owner's
  choice. A hard block would punish the rare legitimate case and isn't ours to enforce.
