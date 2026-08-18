# Silent payments audit

A static review of the silent-payments implementation — BIP352, BIP374,
BIP375, BIP376 — on the device and in its load/sign path, done against a
reported vulnerability class in another signer's silent-payments support.
Scope: `main/kiss_sp.c` (721 lines), the SP paths in `main/kiss_psbt.c`
(`sp_scan`, `sp_fill`, `sign_sp_spends`, the BIP376 load branch), the SP
screens in `main/kiss_sign.c`, the SP test suite `sim/test_sp.c` and its
golden vectors, and the fuzz harness.

One finding, low severity, confirmed and fixed. Everything else checked out;
the reasons are below so the checks do not have to be repeated.

## The question that started this

A hardware signer vendor disclosed that its silent-payments support had let a
malicious host redirect funds: versions 9.21.0–9.26.4 were fixed in 9.26.5.
The published summary said a malicious host device could make the signer
commit outputs to unintended addresses, locking funds unless the attacker and
the recipient collude. The root-cause writeup was not yet published at the
time of this audit, so the review targeted the class: silent-payment outputs
whose tweaks or proof are computed anywhere outside the signer's control.

The result of that class check is the "Verified clean" section below. When
the vendor's technical writeup lands, re-run that section against it.

## Findings

| ID | Severity | Status |
| --- | --- | --- |
| F-01 | Low | Confirmed, fixed, regression test added |

### F-01: SP-tagged sends suppressed the unproven-inputs and merging cautions

**Where.** `main/kiss_psbt.c`, the caution block in `kiss_psbt_load` (around
line 900, before the fix).

**The bug.** Two warnings were gated on `!sp_send`, where

```c
const bool sp_send = (s_sp.n > 0 || s->n_sp_in > 0);
```

so any PSBT with a silent-payments element — a BIP376 input **or** an SP
output — silently dropped both:

- `WPSBT_C_UNPROVEN_IN` — "input amounts not proven - fee may be higher"
- `WPSBT_C_MERGE_INS` — "merging many coins (privacy)"

The exemption's own comment justified it for BIP376 inputs only: their
amounts are covered by the BIP341 sighash (all input amounts are hashed, so
the two-session amount lie fails to combine), and the coins are already
scan-linked. But the gate was on the whole transaction, so a send that merely
*paid* a silent-payment address — or a mixed send with one BIP376 input — lost
the warnings for every ordinary input in it.

**The attack.** BIP143 commits only to the amount of the input being signed.
With two or more inputs, a coordinator can run two signing sessions that
declare different but individually truthful amounts for an unproven input,
combine one valid signature per input, and broadcast a transaction whose fee
neither session showed — the difference going to a miner. With the caution
suppressed, the device showed a green READY and signed. The rest of the load
path was sound: the per-input proofs still ran (keypath re-derive, BIP376
tweak ownership check), so the attack could only distort amounts, never
redirect them.

**The fix.** The unproven condition no longer references `sp_send` at all; an
all-taproot spend is already exempt through `ntap < s->n_in`, and BIP376
inputs are the only taproot inputs that reach it unproven. The merging
condition now keys off the inputs themselves: `s->n_in != s->n_sp_in`, so a
pure BIP376 spend (every input scan-linked) stays silent and any transaction
that merges ordinary coins — with or without SP parts — is warned.

**The regression test.** `sp_test_mixed_unproven` in `sim/test_sp.c` builds
the exact suppressed shape from the pinned BIP376 fixture
(`SPV_SPEND_EVEN_B64`): it splices a second input — a P2WPKH coin carrying
only a `witness_utxo`, re-derived from the session master — into the
serialized bytes, patches the output amount to keep the fee at 5000, and
asserts the load is CAUTION with exactly `WPSBT_C_UNPROVEN_IN` raised,
correct fee math, and deterministic signing.

Verified both ways: the test fails against the pre-fix code (load returns
READY, no warning) and passes against the fix.

**Gates.** Unit suite 1377 PASS / 0 FAIL; fuzz suite PASS; 21-locale overlap
walk clean on the changed surface (run English-only per house rules; this
change touches no strings).

**Device test: NOT REQUIRED.** The change alters two boolean conditions in the
load path and the CAUTION bar they feed is existing UI rendered for every
non-READY load. No hardware path — display, camera, QR, SD, buttons, touch,
USB, timing — is involved, and no gate outcome can be changed by the device.

## Verified clean

The structural reasons the BitBox-class redirect cannot happen here, each
traced through the code:

- **`a_sum` is derived from device keys only.** The DLEQ proof is over
  `a_sum = Σ a_priv` where the private keys come from the device's own
  derivation. Host pubkeys never enter the sum; they are only *addressed*.
  (BIP352 output tweak = `hash(Σ a_priv·B_scan) + Σ b_priv` — the signer's
  half is its own.)
- **Foreign share fields are wiped at load.** Any `0x07`/`0x08`/`0x1d`/`0x1e`
  PSBT fields — the values a malicious coordinator could plant to steer the
  tweak — are discarded before the proofs run.
- **Every input is proven.** A keypath input re-derives its key and script
  from the device master; a BIP376 input is checked against its `SP_TWEAK`
  with a re-derivation that fails closed if the tweak is not a product of the
  device's own keys.
- **The fill step verifies, it does not trust.** Before signing, the BIP375
  output scripts are recomputed from the PSBT's own share and proof, and the
  proof is verified against the signer-derived `A_sum`. A tampered PSBT fails
  closed at review time.
- **Deterministic aux.** DLEQ and Schnorr use fixed aux (`"0x00"*32`), so no
  randomness channel exists for a host to influence.
- **No post-review channel.** The PSBT lives in RAM; there is no second
  exchange with the host between review and signing, so the two-session
  amount trick (F-01) is the only distortion that survives, and the cautions
  now cover it.
- **Both secp contexts are blinded at session open.**

The golden vectors (`sp_test_vectors.h`, `sp_spend_vectors.h`) pin the crypto
against independent implementation values, and the fuzz harness exercises the
load path with malformed SP-shaped inputs.

## Follow-ups

- Re-verify the "Verified clean" section against the vendor's root-cause
  writeup when published.
- The mixed-input regression fixture is spliced in C from the pinned bytes
  because the embit generator fork is not available locally; regenerate the
  fixture with `tools/sp_fixtures/gen_spend_vectors.py` when the fork is
  available and delete the splice.
- F-01's second half — SP-*output* sends (no BIP376 inputs) with ≥2 unproven
  ordinary inputs — shares the identical condition line with the tested shape
  and is covered by it textually, but has no dedicated fixture.
