# Changelog

All notable, user-facing changes to KISS Wallet. Dates are ISO (YYYY-MM-DD).
This is a Bitcoin signer — entries are written so a non-developer can tell what
changed and why it matters. Versions follow the firmware tags.

## [0.1.0-beta3] — unreleased

The safety-and-clarity release: KISS now explains itself as you use it, and
speaks up in plain words before you sign something you might regret.

### Added — you see and understand more

- **Learn-as-you-go explainers.** A **?** next to any unfamiliar term (RBF,
  fingerprint, coordinator, dust) opens a short plain-words card. Where a picture
  helps, the card carries a small diagram built from the app's own style:
  `WORDS + PASSPHRASE → FINGERPRINT` for the deniability model, and
  `ONLINE APP ←QR→ KISS OFFLINE` for the airgap.
- **Tap the home fingerprint** to learn what that code is (same wallet = same
  code; a different passphrase = a different wallet).
- **Verify a receive address by scanning it.** Receive → **VERIFY** scans the
  address your coordinator shows and confirms on-device whether it is really
  yours — the defense against malware swapping addresses on your computer.
- **Pair Coordinator screen** picks DESKTOP (descriptor, Sparrow-style) or
  MOBILE (zpub, BlueWallet-style), with a nudge to prove the pairing by verifying
  the first address, and to practice with a tiny amount before real coins.
- **EASY SCAN** on the signed-QR screen: bigger dots, slower loop, for phone
  cameras that will not lock onto the default animation.

### Added — warnings before you sign (all soft cautions you acknowledge)

- **High fee.** The fee turns amber if it is a large share of what you send (the
  rule Krux uses) or an outsized sat/vB rate.
- **Dust / privacy.** Spending a tiny coin, or leaving tiny change, is flagged —
  both can be used to link and track your addresses. Change below the network
  dust limit is flagged louder (it may also make the transaction unrelayable).
- **Reused address.** Receive lands on a fresh address and warns if you page back
  to one already used, with a **FRESH** shortcut. Reusing an address links your
  payments together.
- Several cautions **stack** into one summary; a **?** opens a WHY FLAGGED card
  explaining each and the fix (freeze or label the coin in your coordinator).
  When any caution is present, signing is gated behind an **I UNDERSTAND** tap.

### Changed

- The Sign verify screen leads with **YOU ARE SENDING** (amount + fee), shows
  every output with change re-derived on-device, and no longer clutters the main
  view with `locktime 0` — the raw value and a plain-words note live in DETAILS.
- Explainer cards now enter with a staggered fade-and-rise animation.

### Safety notes

- KISS has no view of the chain, so the reuse guard and fee/dust warnings are
  best-effort education, not guarantees — your coordinator remains the source of
  truth for balances and the going fee rate.
- The passphrase is still typed fresh every unlock and never stored; there is no
  "wrong passphrase" error by design.

## [0.1.0-beta2] — 2025 (previous release)

- First public beta: the fruit game with the hidden signer behind it, BIP39/BIP84
  single-sig, animated-QR (BC-UR) and SD-card PSBT transport, on-device PSBT
  verification, radio held in reset. Signed release artifacts with GPG + SHA256.
  Not for meaningful funds.
