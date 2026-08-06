# Security plan

What protects a KISS Signer today, what is planned, and what is deliberately
out of scope. Written so a reader can tell the three apart.

To report a vulnerability rather than read about the design, see
[SECURITY.md](../SECURITY.md).

> **On the normal beta firmware, none of the hardening below is active.**
> Treat a beta device as a hot wallet with a good interface.

## Threat model

KISS is an airgapped single sig signer. It defends against:

- **A compromised coordinator.** The online wallet builds the PSBT. KISS
  re-derives every output, the fee and all change on its own screen, so a
  swapped recipient or an inflated fee is visible before signing.
- **A compromised computer or phone.** Nothing but a QR image or an SD file
  crosses the gap. No USB data path is used in normal operation.
- **Network attackers.** There is no network. The release build fails to link
  if any radio or networking symbol appears, and the ESP32-C6 radio
  coprocessor is held in reset from the first instruction of every boot.
- **Casual physical inspection.** The device boots into a playable arcade game,
  and the signer is behind a gesture. See the deniability section below, which
  is worth reading carefully because the design is the opposite of what most
  people assume.
- **Losing an SD card.** In SD mode the words are sealed to a device key, so
  the card on its own is inert.

It does **not** defend against:

- **Physical extraction of the flash**, on the normal beta. See phase 1.
- **A modified firmware image being written to your device.** Secure boot is on
  the roadmap, not shipped. See phase 2.
- **Someone holding both the device and the SD card.** Closing that is what
  flash encryption adds, by protecting the device key at rest.
- **Supply chain compromise of the device itself.**
- **An attacker who has both your recovery words and your passphrase.**

## Deniability, stated in the right direction

This is easy to describe backwards, so it is spelled out.

Drawing plain **KISS** on the game menu opens the **decoy** wallet: the same
recovery words with an empty passphrase. It is a real, working, funded signer.
It pairs with a coordinator and it signs, because a wallet that cannot do those
things is not a story anyone would believe. It asks for **no passphrase at all**.

The owner's real wallet is behind KISS plus **one extra configured stroke**, and
only that path ever draws the passphrase keyboard.

The reasoning: a passphrase field on screen is itself a tell. It proves there is
something being left out of it, and you can never demonstrate that the
passphrase you gave was the last one. A KISS-branded device where drawing KISS
does nothing is also more suspicious than one that opens a modest wallet. So the
obvious gesture, the one a researched attacker would try, lands somewhere
plausible and complete.

There is exactly one configurable stroke, the owner's. Plain KISS always reaches
the decoy once a gesture is set, so nobody can be locked out by forgetting a
stroke they chose months ago.

The limit is worth stating: this defends against inspection and coercion by
someone who does not know the design. It does not defend against an attacker who
has read this page, knows there is a second stroke, and is willing to keep
asking. Nothing on a device can.

## Phase 1, flash and NVS encryption

**Status: in progress. Build profile complete, hardware testing underway.**

`tools/build_encrypted_release.sh` produces flash encryption in RELEASE mode
plus NVS encryption. Plain flash encryption does not cover `nvs` data
partitions and the words live in NVS, so both are needed; the XTS keys go in an
`nvs_key` partition which is itself flash-encrypted.

The first boot burns eFuses. It cannot be undone, and the device can never be
reflashed over serial afterwards. A separate rehearsal profile
(`KISS_ENC_REHEARSAL=1`) leaves reflashing available but still burns a flash
encryption key permanently.

What this changes per storage mode:

- **FLASH** goes from storing the words in the clear to storing them encrypted.
  This is the big one, and it is why FLASH on a beta device should be treated as
  a hot wallet.
- **SD CARD** already seals the words to a device key, so a lost card is
  inert either way. Encryption protects that device key at rest, which closes
  the case where someone holds the device *and* the card.
- **AMNESIC** is unaffected. Nothing is written anywhere.

All three modes are offered on every build. SD was previously gated behind
encryption, which was backwards: it withheld the safer persistent mode while
shipping FLASH in the clear on the same firmware.

Testing in progress on a disposable device: the migrations between all three
modes, missing card boot, interrupted writes, wipe with the card absent, corrupt
files, and a flash dump check. See
[`specs/sd-seed-storage.md`](specs/sd-seed-storage.md).

## Rollback protection

**Status: mostly free with phase 1, plus one cheap addition worth making.**

Worth being precise here, because the usual answer is misleading.

**On an encrypted final device it comes for free.** Flash encryption in RELEASE
mode disables serial reflashing outright. A device that cannot be reflashed
cannot be downgraded. That is stronger than a version counter.

**On a normal beta device, no counter would help yet.** ESP-IDF's anti rollback
feature stores a security version in eFuse and has the bootloader refuse older
images. Without secure boot the bootloader itself can be replaced, so the check
is bypassed by the same person it is meant to stop. It is also an OTA feature,
and KISS has no OTA: firmware only ever arrives over USB serial, deliberately.
Shipping it before phase 2 would add a config flag and no security.

**What is worth doing: make a downgrade visible.** Record the highest firmware
version this device has ever run in NVS. On boot, if the running version is
lower, say so on the home screen in plain words, next to the fingerprint. It
does not prevent anything, and it should not claim to. It turns a silent
downgrade into something the owner sees, which is the same bargain the signing
screen already makes with fee and dust cautions: show it, explain it, let the
person decide.

Cost is small: one NVS key, a comparison at boot, one banner and one explainer
card in the existing `?` vocabulary. It survives a wipe deliberately, since the
point is to track the device, not the wallet.

## Phase 2, secure boot v2

**Status: on the roadmap. Not started, and it follows phase 1.**

Secure boot v2 cryptographically binds the device to firmware signed by a
known key, so a modified image will not boot.

Until it ships, state the gap plainly: **an attacker with prolonged physical
access to an unencrypted device can flash modified firmware, and the interface
will look identical.** Reproducible builds and signed release hashes let you
verify what *you* install. They cannot stop someone else installing something
different later.

It comes after flash encryption for a practical reason. Both burn eFuses, both
are irreversible, and on a self assembled device they should be burned together
on a final signer rather than one at a time.

One design question to settle before it ships, because it decides who the
signer is for: signing with a project key means a device that only runs
official firmware, which is stronger against tampering and weaker for a DIY
signer whose owners are meant to build and flash their own. Letting owners
enrol their own key keeps that open. Whichever is chosen should be written here
before any eFuse is burned.

## External review

**Status: not scheduled.**

Nobody outside the project has reviewed the signing path, the PSBT parser or
the BIP39 and BIP352 implementations. Until that happens, "verified on device"
means verified by code that has been read by its author and its tests.

Those tests are not nothing. `sim/build_test.sh` covers BIP39 vectors, the PSBT
parser under ASAN and UBSAN fuzzing, the full BIP352 and BIP374 vector sets, and
BIP376 spend signatures cross-checked against an independent implementation. But
a passing test suite tells you the code does what its author expected, which is
a different question from whether what they expected is safe.

## What already holds

Not phases, shipped properties:

- **No radio, enforced at build time.** The release build fails to link if any
  radio or networking code is present.
- **Reproducible builds.** CI rebuilds each release; local hashes must match.
- **GPG signed release hashes**, fingerprint
  `166A CBF3 7786 FCEA A694 96DE 886F 1BFE B84E F1C0`. Cross check it from more
  than one source, since a fingerprint published only here is only as
  trustworthy as this page.
- **Every output re-derived on the device** before a signature is possible.
- **Three entropy sources at wallet creation**, the camera, the chip's hardware
  RNG, and the timing of the user's own taps, hashed together so that no single
  one decides the words. The taps are a source no manufacturer can reproduce.
- **The installer cannot serve a stale binary.** CI fails if the page offers a
  build whose version does not match `VERSION`.
- **Vendored dependencies**, pinned. See
  [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## Signing is verifiable

Dark Skippy is a signing-time attack: malicious firmware chooses the nonce of a
signature so that it leaks bits of the master seed, and an attacker reads them
back off the blockchain across two signatures. A perfectly generated seed is
exfiltrated anyway, because the leak is in the nonce, not the seed. The three
entropy sources above do nothing against it; this does.

KISS signs deterministically, and that is the defense. A nonce that is a fixed
function of the key and the message is not free, so firmware cannot vary it to
smuggle out the seed without also failing to reproduce the one honest signature.

**The frozen rules.** Stated so an independent implementation reproduces every
KISS signature from the seed:

- **ECDSA** (legacy, nested, native inputs): RFC6979 deterministic nonce with
  low-R grinding, the counter incremented until R is low. This is
  `EC_FLAG_GRIND_R` and it matches Bitcoin Core, so a Core signer with the same
  key produces the same bytes. KISS refuses any sighash that is not ALL or
  DEFAULT, so the message is unambiguous.
- **Schnorr** (silent-payment spends, the only Schnorr path): BIP340 with
  `aux_rand = sha256(spend_priv || sha256(psbt_bytes))`. Deterministic, and
  bound to both the wallet and the whole transaction. This aux is KISS-specific,
  so a KISS-aware verifier or a second KISS reproduces it; a generic BIP340
  signer using a different aux produces a different, still valid, signature.

**Pinned in CI.** `sim/sign_vectors.h` holds the exact signature bytes for the
ECDSA cases (computed independently with embit,
`tools/sign_fixtures/gen_sign_vectors.py`) and the silent-payment Schnorr spends
(computed with the BIP340 reference signer,
`tools/sign_fixtures/gen_sp_sign_vectors.py`), never copied from KISS's own
output. The test suite asserts them, so any drift in nonce derivation or the
Schnorr aux fails the build. A determinism check signs the same PSBT twice and
requires identical bytes.

**Checking a unit yourself.** Sign the same PSBT, with the same seed, on a
second signer you trust independently: a second KISS you compiled yourself from
audited source, or any tool that implements the rules above. Compare the
signature bytes. Deterministic signing means they must be identical; a single
differing byte means one signer chose its nonce, which is the Dark Skippy tell.
The strength of this check is the independence of the second signer. Two units
running the same untrusted build prove nothing: they leak identically and still
match.

Deferred: the Schnorr aux is KISS-specific, so only a KISS-aware verifier
reproduces taproot signatures. Switching to a fully standard BIP340 deterministic
nonce would let any conforming tool verify them, at the cost of changing every
silent-payment signature. Revisit when a concrete external verifier needs it.
See [`specs/verifiable-determinism.md`](specs/verifiable-determinism.md).

## Reporting

Security issues: open a GitHub issue for anything already public, or contact
the maintainer directly for anything that is not. There is no bug bounty.
