# KISS Wallet 0.1.0-beta3

Beta firmware for the Guition JC4880P443C ESP32-P4 board.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-wallet-0.1.0-beta3.bin` - merged firmware image
- `SHA256SUMS` - firmware hashes
- `SHA256SUMS.asc` - GPG signature for `SHA256SUMS`
- `kiss_wallet_pgp.asc` - KISS release public key
- `release.json` - machine-readable release metadata

## Verify

```sh
gpg --import kiss_wallet_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: 166ACBF37786FCEAA69496DE886F1BFEB84EF1C0

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS
```

Main firmware SHA256:

`86165e4235b349ac51f9cefe4c58df3a86e858aa762d73af7b91d2b3ccb296d3`

Release commit:

`7511c6e`

## Install

For beta releases, flash the verified `.bin` locally using the README install steps. If you use an agent/Codex, have it flash this exact release asset.

The browser installer comes later with GitHub Pages. It will require Chrome, Brave, or Edge on desktop; Safari and Firefox cannot flash ESP32 boards over Web Serial.

After flashing, unplug the board, wait about 3 seconds, then plug it back in.

## Changelog

The safety-and-clarity release: KISS now explains itself as you use it, and
speaks up in plain words before you sign something you might regret.

### Added — you see and understand more

- **Verify your backup before funding.** WALLET → BACKUP WORDS → **VERIFY MY
  COPY** lets you type your written words from paper; the device confirms they
  rebuild this exact wallet and never shows the stored words. A wrong or missing
  word is reported by position ("word #N"), never revealed. It never alters the
  seed — a safe dry run you can repeat any time.
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
