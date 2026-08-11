# Verifiable Determinism — Implementation Plan

**Spec:** `docs/specs/verifiable-determinism.md` (committed, `48f26ab`)
**On approval:** move this plan to `docs/superpowers/plans/2026-07-30-verifiable-determinism.md`.

## Context

Dark Skippy is a signing-time attack: malicious firmware chooses signature
nonces that leak the master seed across two signatures. KISS already signs
deterministically on both curves (ECDSA `EC_FLAG_GRIND_R` at
`main/kiss_psbt.c:986`; Schnorr with `aux = sha256(spend_priv || psbt_hash)`
at `main/kiss_psbt.c:938`), so the nonce is not free. This work turns that
latent property into a guaranteed, tested one, so a firmware that varies the
nonce to leak the seed is detectable. It adds no signing code and does not
change signing behavior; it pins the existing behavior and documents how to
check it. Interactive anti-exfil was rejected in the spec (ECDSA-only, needs a
coordinator round trip the air gap does not support).

Two teeth, per the spec:
- **Golden signature vectors in CI** — exact signature bytes for a fixed
  (dev seed, PSBT), so any drift in nonce derivation fails the build.
- **Cross-signer verification** — a documented workflow: same PSBT + seed on a
  second independently trusted signer must produce byte-identical signatures.
  This part ships as documentation plus the frozen rules that make an
  independent signer possible; no device code.

## Existing pieces to reuse (do not reinvent)

- **Reference-vector pipeline:** `tools/sp_fixtures/gen_vectors.py` and
  `tools/sp_fixtures/gen_spend_vectors.py` already use **embit** (independent of
  libwally) to emit `main/sp_test_vectors.h` / `main/sp_spend_vectors.h`, which
  `sim/test_sp.c` consumes. The new ECDSA/Schnorr signature vectors follow this
  exact pattern: a Python generator emits a `.h`, a test asserts against it.
- **Fixed dev seed:** the whole suite runs on the stored dev mnemonic
  (fingerprint `73C5DA0A`), so signatures over fixed PSBTs are already
  deterministic run to run.
- **Deterministic PSBT builders:** `mk_typed_psbt(script, purpose, ...)` in
  `sim/test_crypto.c` builds the fixed legacy/nested/native PSBTs the sign
  roundtrip already exercises (`sim/test_crypto.c:440-469`). The SP spend
  fixtures live behind `sim/test_sp.c` + `main/sp_spend_vectors.h`.
- **embit Schnorr cross-check:** `sim/test_sp.c:430` already verifies the
  emitted SP signature against embit's outkey + sighash. That proves validity,
  not byte-equality — the golden vector adds the exact-bytes assertion on top.
- **Test harness:** `sim/build_test.sh` compiles one runner to `/tmp/kisstest`;
  suites are `int test_x(void)` returning a fail count, called from
  `main()` in `sim/test_crypto.c` and added to `fails`.

## Task 1 (GATE): prove reproducibility before building anything

The one real risk is the ECDSA low-R grind: KISS's bytes only reproduce
independently if the reference replicates libwally's exact RFC6979 + low-R
counter mechanism. Resolve this first with a throwaway spike.

- Sign one fixed native-segwit PSBT with KISS (`kiss_psbt_sign`), print the
  DER/compact signature hex.
- Independently reproduce those exact bytes. Try, in order, until one matches
  byte-for-byte: (a) embit's ECDSA sign if it exposes low-R grinding; (b) a
  short pure-Python RFC6979 + low-R grind (increment the 32-byte counter
  appended to the nonce extra-data until R's top bit is clear), curve math via
  the `ecdsa` package; (c) Bitcoin Core `signrawtransactionwithkey` (shares the
  low-R convention).
- Do the same for one SP Schnorr spend: reproduce KISS's bytes in Python by
  applying KISS's aux rule (`sha256(spend_priv || psbt_hash)`) to embit/BIP340.

**Exit criterion:** both reproduce exactly. If ECDSA cannot be reproduced by any
independent method, STOP and report — that means the nonce is less reproducible
than the spec assumes, which changes the whole approach. Record the winning
method; the generator in Task 2 uses it.

## Task 2: the reference generator

- Create `tools/sign_fixtures/gen_sign_vectors.py` using the Task 1 method.
  It computes, for the dev seed and the fixed test PSBTs, the expected
  signature bytes for: legacy (44), nested (49), native (84) ECDSA; and the SP
  Schnorr spend in both SIGHASH_DEFAULT (64-byte) and explicit SIGHASH_ALL
  (65-byte, trailing `0x01`) forms.
- Emit `sim/sign_vectors.h` (test-only, so it lives in `sim/` not `main/`):
  `#define SV_SIGN_NATIVE "…hex…"`, one per case, plus a header comment stating
  it is generated and independently computed — never pasted from KISS output.
- Mirror the docstring/style of `tools/sp_fixtures/gen_vectors.py`.

## Task 3: golden-vector assertions

- **ECDSA**, in `sim/test_crypto.c`'s per-type sign roundtrip
  (`sim/test_crypto.c:440-469`): after `kiss_psbt_sign`, extract the input
  signature from the signed PSBT, hex-encode it, and assert it equals
  `SV_SIGN_<TYPE>` from `sim/sign_vectors.h`. Reuse the existing `chk(name,
  got, want)` string comparator.
- **Schnorr**, in `sim/test_sp.c`'s SP sign test (near the existing
  `:430` embit cross-check): assert the emitted taproot signature hex equals the
  golden value for both the DEFAULT and explicit-ALL fixtures.
- Wire `sim/sign_vectors.h` into `sim/build_test.sh` include path (already has
  `-Imain -Icomponents/...`; add `-Isim` if not present) and `#include` it in
  both test files.

## Task 4: determinism test

- Add to `sim/test_crypto.c` (and/or a small `test_signvec` block): sign the
  same PSBT twice in one run, assert the two signature byte strings are
  identical, for one ECDSA input and one SP Schnorr input. This localizes any
  accidental nondeterminism (a stray RNG call) that a single golden vector
  might not pinpoint.

## Task 5: documentation

- Add a "Signing is verifiable" section to `docs/security-plan.md` (exists)
  containing: the **frozen rules** verbatim from the spec (RFC6979 low-R for
  ECDSA; BIP340 with `aux = sha256(spend_priv || psbt_hash)` for SP Schnorr,
  both sighash forms), and the **cross-signer workflow** (sign the same PSBT on
  a second independently trusted signer, diff the bytes, a mismatch means one
  chose its nonce — with the explicit caveat that two units on the same
  untrusted build prove nothing).
- Note the deferred open question (standardizing the Schnorr aux) so it is not
  lost.

## Files

- Create: `tools/sign_fixtures/gen_sign_vectors.py`, `sim/sign_vectors.h`
- Modify: `sim/test_crypto.c` (ECDSA golden + determinism), `sim/test_sp.c`
  (Schnorr golden), `sim/build_test.sh` (compile/include), `docs/security-plan.md`
- Throwaway (Task 1 spike): delete before committing.

## Verification

- `sim/build_test.sh && /tmp/kisstest` — new golden + determinism assertions
  PASS, existing 763 unaffected, 0 FAIL.
- Re-run `tools/sign_fixtures/gen_sign_vectors.py`; its output must match the
  committed `sim/sign_vectors.h` byte-for-byte (generator is reproducible).
- Deliberately perturb the nonce path (e.g., temporarily drop `EC_FLAG_GRIND_R`)
  and confirm the golden test FAILS — proving the vectors have teeth — then
  revert.
- `bash sim/build_fuzz.sh && /tmp/kissfuzz` — unchanged, regression check.

## Device test verdict

**DEVICE TEST: NOT REQUIRED** for the cryptography. The deterministic signing
path takes no hardware entropy and no timing input by design, so the host build
(same vendored libwally + secp256k1 as the device) is authoritative for the
signature bytes. No display, camera, touch, SD, or RNG path affects them. This
is stated separately from CI: hardware cannot change the outcome because no
hardware input feeds a signature — not merely because tests are green.

Caveat, documented not gating: the Task 5 cross-signer workflow is a user
procedure and should be walked once on real hardware (sign on device, sign on a
second signer, confirm bytes match) to prove the instructions are correct. That
validates the docs, not the crypto.
