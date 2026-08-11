# Uniform unlock routing, and the custom opening word

Status: design approved 2026-08-07. Stage 1 specified for implementation;
Stage 2 scoped here and gets its own spec.

## Why

Two audit findings and one feature request turned out to be the same problem.

The audit reported the duress stroke sitting in plaintext NVS as High. An
adversarial verifier refuted it, and the refutation is correct: `main/main.c`
forks on whether a stroke is configured.

```c
if (detect_cover_word(s_gpt, s_gn, s_strokes))
    return real == WDG_NONE ? 1 : 0;   // never configured: word -> passphrase, as before
```

Draw the word once. A passphrase keyboard means no stroke is configured; a
wallet with no prompt means one is. The `greal` byte tells an attacker holding
the device nothing the device does not already tell them in one gesture. **The
leak is the behaviour, not the storage.**

That also settles where the secret lives. The passphrase is the secret; the
gesture is routing. Storing the gesture in the clear costs nothing, because an
attacker who dumps flash learns only what drawing the word would have shown
them anyway.

The second finding stands on its own: the Settings duress row is present in a
real session and absent in a decoy, so a coerced owner handing over a decoy can
be caught by an attacker who knows where to look.

## Stage 1, uniform routing

**The rule.** Plain word opens the decoy. Word plus any recognised modifier
stroke opens the passphrase keyboard. On every device, configured or not.

**What changes.** The `real == WDG_NONE` fork goes. `kiss_duress_classify`
stops being asked "is this THE configured stroke" and is asked only "is this a
deliberate modifier stroke at all". `greal` no longer routes; it stays in NVS
as a preference for Stage 2 and for the Settings rehearsal screen, and its
presence or absence changes no behaviour anyone can observe.

**Why this is deniable.** A configured device and a factory fresh one now
answer identically to every gesture an attacker can try. There is no oracle
because there is no fork.

**The migration hazard, and the answer.** An owner who never configured a
stroke reaches their funded wallet today by drawing the word and typing a
passphrase. After this change the word alone lands them in the decoy, which to
them reads as a lost wallet. So the decoy home carries one quiet line saying a
stroke reaches the passphrase.

That line is safe precisely because it is true on every device, including one
that has never been configured and one whose owner has no passphrase. It
describes the product, not this device. An attacker reading it learns what the
manual already says.

**Files.** `main/main.c` (the fork and the gesture routing),
`main/kiss_duress.c` / `.h` (classify contract),
`main/kiss_settings.c:1194` (the row, made unconditional), plus the decoy
home line in `main/kiss_ui.c`.

**Strings.** One new key for the decoy line, which means 21 locales. Check
whether an existing string already says this before adding one; the passphrase
intro screens may already carry usable copy.

## Stage 2, the custom opening word

Scoped here, specified separately.

`detect_cover_word` is four pen lifts, a size and aspect gate, three or more letter
clusters along x, and `detect_K` on the leftmost cluster. The I, S and S are
counted, never identified. A custom word follows the same shape: **identify the
first character, count the rest.**

- Stored plaintext in NVS as (first character id, cluster count). Not a secret,
  by the reasoning above.
- First character set: **digits 0 to 9.** Ten detectors. Digits separate more
  cleanly than letters on a fingertip drawn panel and carry no case ambiguity.
- Settings gains a rehearsal screen: draw it, see what the device read, confirm.
  Because Stage 1 made the row unconditional, this screen is reachable in every
  session and reveals nothing by existing.

**The risk that governs this stage** is false negatives, not false positives. A
detector too strict locks an owner out of their own device. `detect_K` needed
three revisions and a device finding to get lenient enough, and that was for a
shape that only ever had to open a decoy. Every digit detector inherits that
problem and must be tuned on hardware, not in the simulator.

A fumbled word costing nothing is what makes leniency affordable: under Stage 1
an unrecognised drawing lands on the game, and a misread word lands on the
decoy. Neither leaks and neither loses funds.

## What this deliberately does not do

- **Does not fold the gesture into key derivation.** That was considered and
  rejected. It would store nothing at all, but a slightly differently drawn
  gesture would silently open a different empty wallet with no error, and it
  would make an existing wallet unreachable the moment its owner changed the
  gesture.
- **Does not encrypt or hash `greal`.** The space is six values with no rate
  limit, so a hash is brute forced instantly, and the behaviour it would hide
  is no longer observable after Stage 1.
- **Does not touch the passphrase model.** The passphrase remains the only
  secret that selects a wallet.

## Verification

Stage 1 is testable end to end on hardware and only partly in the simulator.

1. `bash sim/build_test.sh && /tmp/kisstest` — the duress classifier tests must
   still pass with the contract change.
2. `bash sim/build_sim.sh && bash sim/run_overlapcheck.sh` — the Settings row is
   now present in both sessions, so the walk must render it in 21 locales
   without overlap, and the decoy home line must fit.
3. `bash sim/build_fitcheck.sh && /tmp/kissfit` — the new decoy line at every
   locale's font.

**DEVICE TEST: REQUIRED.** The gesture path is touch input and no gate can see
it. On hardware: draw the word alone and land on the decoy; draw the word with
each of the six strokes and reach the passphrase keyboard every time; confirm a
device that has never been configured behaves identically to one that has;
confirm the Settings row is present in both sessions; and confirm an
unrecognised scribble still falls through to the game.
