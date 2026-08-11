# Verifiable determinism: signatures a compromised KISS cannot fake

Status: implemented 2026-07-30. Golden signature vectors for the ECDSA inputs
(legacy, nested, native) and the silent-payment Schnorr spends (even-Y and
odd-Y, SIGHASH_DEFAULT) are pinned in `sim/sign_vectors.h`, each reproduced by
an independent signer (embit for ECDSA, the BIP340 reference for Schnorr) and
asserted by the suite, with a determinism re-sign check. Remaining: the
explicit-SIGHASH_ALL 65-byte Schnorr form (encoding already tested, exact bytes
not yet pinned) and the deferred aux-standardization question below.

Dark Skippy is a signing-time attack. Malicious firmware picks the nonce of a
signature so that it leaks bits of the master seed, and an attacker reads those
bits back off the blockchain across two signatures. A perfectly generated seed
is exfiltrated anyway, because the leak is in the nonce, not the seed. So the
tap-entropy work (docs/specs/tap-entropy.md) does nothing against it: that
guards seed generation, and this guards signing.

The textbook defense is interactive anti-exfil, where the coordinator feeds
entropy into the nonce and verifies the signer did not choose it. libwally
ships it (`wally_ae_*` in wally_anti_exfil.h), but it does not fit KISS:

- It is ECDSA only (`flags` must be `EC_FLAG_ECDSA`), so it cannot cover the
  taproot key-path and silent-payment spends KISS signs with Schnorr.
- It is a commit/reveal round trip between host and signer. KISS is an
  air-gapped QR/SD device, and its coordinators (Sparrow, BlueWallet) do not
  speak the protocol over that gap.

This spec takes the other road, the one KISS is already most of the way down.

## The property KISS already has

KISS signs deterministically on both curves:

- ECDSA: `wally_psbt_sign_bip32(..., EC_FLAG_GRIND_R)` at wallet_psbt.c:986.
  The nonce is RFC6979 and the counter is ground until R is low, which is
  exactly what Bitcoin Core does, so a Core signer with the same key produces
  the same signature.
- Schnorr: `sp_schnorr_sign` at wallet_sp.c:614 with a non-null aux, derived at
  wallet_psbt.c:938 as `aux = sha256(spend_priv || psbt_hash)`. Deterministic,
  and bound to both the wallet and the whole transaction.

Determinism is the lever. If a signature is a fixed function of (key, message),
then the nonce is not free, and firmware that varies the nonce to leak the seed
cannot also reproduce the one honest signature. Dark Skippy needs a free nonce.
Determinism denies it, and makes the denial checkable.

The seed never has to leave the device for the check. Two things exist here:
determinism the code guarantees at the source, and reproducibility a second
holder of the same seed can confirm. Neither asks the signer to reveal a key.

## Part 1: freeze the rules, and pin them with golden vectors

Determinism is only a defense if it cannot drift. Today it is an emergent
property of the flags and the aux derivation; a refactor could change the
nonce and nothing would notice, which is precisely how Coldcard lost five
years of seeds to a one-line RNG change that every statistical test passed.

So the exact rules become a written, tested contract.

**Documented rules.** A section of this spec (below, "The frozen rules") states
the derivation precisely enough that an independent implementation reproduces
every signature KISS makes.

**Golden vectors in kisstest.** A fixed development seed and a fixed set of
PSBTs, one per script type, produce a fixed set of signature bytes. The test
asserts the exact bytes. Coverage:

- legacy (P2PKH), nested (P2SH-P2WPKH), native (P2WPKH) ECDSA inputs;
- a silent-payment spend, which is the only Schnorr signing path KISS has
  (there is no BIP86 key-path signer: taproot appears only as silent-payment
  P2TR spends, wallet_psbt.c:670);
- both the SIGHASH_DEFAULT (64-byte) and explicit SIGHASH_ALL (65-byte) forms
  of that Schnorr spend, since the hash-type byte is appended by hand at
  wallet_psbt.c:964 and is exactly the kind of detail a golden vector pins.

The vectors are computed once, independently (a short reference script using
python-bitcoinlib or bare secp256k1, checked into sim/ beside the test that
consumes them, never copied out of KISS's own output), and pasted in. From then
on, any change to nonce derivation, grinding, or aux fails the build. This is
the source-level guarantee: the postmortem's rule that randomness must be
verified structurally at the source, not inferred from the output stream.

The existing sign tests already assert signatures verify and finalize; they do
not assert the exact bytes, so a nonce change slips through today. The golden
vectors close that.

## Part 1b: the same check, on the chip that signs

Added 2026-08-11. Part 1 pins the bytes in kisstest, which runs on the host.
The claim below in this spec's first device-test verdict — that the host build
"compiles the same libwally amalgamation and the same secp256k1 as the device,
so the host vectors are authoritative" — is true of the *source* and false of
the *arithmetic*. secp256k1 selects its field implementation from the compiler:
`field_5x52.h` where `__SIZEOF_INT128__` exists, `field_10x26.h` otherwise
(`src/util.h`). A 64-bit host takes the first, riscv32 takes the second. The
limb code the ESP32-P4 actually runs is code kisstest never compiles.

So `wallet_sign_selftest` (wallet_crypto.c) re-signs two golden vectors on the
device at boot and compares exact bytes:

- ECDSA over a fixed test key and message, `EC_FLAG_ECDSA | EC_FLAG_GRIND_R`;
- Schnorr over the same key with a fixed explicit aux, the shape
  `sp_schnorr_sign` uses.

Both are in `main/boot_sign_vectors.h`, computed by
`tools/sign_fixtures/gen_boot_vectors.py` — pure Python, hashlib only, no
libwally, no libsecp256k1, no embit. The ECDSA message is chosen so the
RFC6979 counter-0 nonce yields a *high* R and the grind loop must run five
rounds; a vector reachable at counter 0 would be satisfied by plain RFC6979 and
would pin only half the rule. The generator asserts this, and kisstest asserts
the discrimination directly: signing the same fixture without `EC_FLAG_GRIND_R`
must not match, and BIP340 with a zero aux must not match.

Failure is not advisory. `wallet_psbt_sign` calls the selftest (cached after
the first run) and returns -6 if it did not pass, so a unit whose curve code
has drifted signs nothing rather than emitting a signature whose nonce nobody
has checked. `wallet_sign_selftest_force_fail`, non-release only, exists so the
suite watches that refusal fire — the same reasoning as `OVERLAPCHECK_SELFTEST`.

This runs in release. It carries no mnemonic: the test key is derived from an
ASCII label, controls nothing, and is meant to be in the binary.

What it does not do: stop malice. Firmware willing to grind a nonce is willing
to delete this function. It catches a broken or substituted curve
implementation on real hardware, which is the gap part 1 could not see.

## Part 2: cross-signer verification, the actual Dark Skippy check

Golden vectors catch an honest codebase drifting. They cannot catch malice: a
malicious build passes them by special-casing the test vector and cheating on
real transactions. Catching malice needs a second, independently trusted signer.

The workflow, documented for the user:

1. Sign the real PSBT on the KISS unit in question.
2. Sign the same PSBT, with the same seed, on a second signer that is trusted
   independently: a second KISS the user compiled themselves from audited
   source, or any tool that implements the frozen rules.
3. Compare the signature bytes. Deterministic signing means they must be
   identical. A single differing byte means one signer chose its nonce, which
   is the Dark Skippy tell.

This needs no coordinator and no protocol change, and it covers ECDSA and
taproot alike, because it checks bytes rather than running a curve-specific
handshake. Its strength is exactly the independence of the second signer:
comparing two units running the same malicious firmware proves nothing, since
both leak identically and still match. The spec says this plainly so no one
mistakes a two-unit match for a proof when both units run the same untrusted
build.

What KISS ships for part 2 is documentation, not code: the workflow above and
the frozen rules that make an independent signer possible. A convenience helper
(display a short fingerprint of the signature so two units are compared by
eye rather than by exporting both files) is optional and deferred; the file
comparison works today with the signed PSBTs KISS already writes.

## The frozen rules

Stated so a third party reproduces every KISS signature given the seed.

**ECDSA (purpose 44 / 49 / 84).** RFC6979 deterministic nonce, SHA-256, with
low-R grinding: increment the RFC6979 counter until the signature's R value is
under the curve order half (a 32-byte, i.e. low, R). Low-S is enforced. This is
`EC_FLAG_GRIND_R` in libwally and matches Bitcoin Core. Sighash is the standard
BIP143 (segwit) or legacy sighash over the PSBT's declared type; KISS refuses
any sighash that is not ALL or DEFAULT at load, so the message is unambiguous.

**Schnorr (silent-payment spends, the only Schnorr path).** BIP340, with
`aux_rand = sha256(spend_priv || psbt_hash)`, where `spend_priv` is the 32-byte
key that signs the input and `psbt_hash` is KISS's transaction hash used as the
per-PSBT domain separator. The signature is 64 bytes for SIGHASH_DEFAULT; when
an explicit SIGHASH_ALL is present the type byte 0x01 is appended, giving 65.

This aux rule is KISS-specific: it is deterministic and reproducible by a holder
of the seed running these rules, but it is not the BIP340 default, so a generic
BIP340 signer that uses `aux_rand = 0` or fresh randomness produces a different
(still valid) signature. A KISS-aware verifier, or a second KISS, reproduces it.

## Open question, deferred: standardize the Schnorr aux

Because the aux rule is KISS-specific, part 2's second signer must implement it,
rather than being any off-the-shelf BIP340 tool. Switching to a fully standard
deterministic nonce (BIP340 with `aux_rand` all zero) would let any conforming
implementation reproduce KISS's taproot signatures, strengthening independent
verifiability.

It is deferred, not adopted, because it changes every taproot and
silent-payment signature KISS produces, which is a signing-behavior change that
wants its own justification and its own golden-vector regeneration, and because
the current aux binds the nonce to the whole transaction, which is a defensible
property to keep. Revisit only when a concrete external verifier needs it. Until
then, publishing the rule precisely (above) is enough for a KISS-aware checker.

## Testing

- **Golden vectors**, as in part 1: exact signature bytes for each script type,
  independently computed, asserted in kisstest. The one hard requirement.
- **Determinism test**: sign the same PSBT twice in one run and assert the two
  signatures are byte-identical, for an ECDSA input and a Schnorr input. This is
  cheap and catches any accidental nondeterminism (a stray RNG call) that a
  single golden vector might not localize.
- Explicitly not tested: any statistical property of the signatures. As with
  tap entropy, a distribution test passes against a compromised signer and is
  the false assurance this whole line of work exists to avoid. What is verified
  is that the bytes are exactly the audited, reproducible values.

## Device test verdict

**Superseded for part 1b — see the note at the end of this section.**

**DEVICE TEST: NOT REQUIRED** for the cryptography.

The deterministic signing path takes no hardware entropy and no timing input by
design: that is the entire point, and it is why the result is reproducible on
the host. The golden vectors computed on the device and on the host must be
identical bit for bit, and the host build compiles the same libwally
amalgamation and the same secp256k1 as the device, so the host vectors are
authoritative. No display, camera, touch, SD, or RNG path affects a signature's
bytes.

Passing kisstest is therefore the verdict for parts 1 and the frozen rules, and
this is stated separately from any CI claim: the reason hardware cannot change
the outcome is that no hardware input feeds the signature, not merely that the
tests are green.

One caveat, documented rather than gating: the part 2 workflow is a user
procedure, so it should be walked once end to end on real hardware (sign a PSBT
on device, sign it on a second signer, confirm the bytes match) to prove the
instructions are correct. That validates the documentation, not the crypto.

**Correction, 2026-08-11 (part 1b).** The paragraph above is wrong about one
thing: host and device do not run the same field arithmetic. secp256k1 picks
`field_5x52` on a 64-bit host and `field_10x26` on riscv32, so kisstest never
executes the limb code the P4 signs with, and the host vectors are authoritative
only for the source, not for the build. Part 1b's boot selftest exists for that
gap. It must be seen passing on real hardware once:

**DEVICE TEST: REQUIRED** for part 1b. Flash and boot a P4 unit and confirm the
log line `signing selftest: PASS (stage 0)`, then sign one PSBT to confirm the
new gate in `wallet_psbt_sign` does not block a healthy unit. Nothing else in
the flow changes. Passing kisstest is not this verdict: kisstest cannot compile
the 32-bit field backend at all.
