# KISS Signer 0.1.0-beta4

Beta firmware for the Guition JC4880P443C ESP32-P4 board.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-wallet-0.1.0-beta4.bin` - merged firmware image
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

`7d6289d6023b0cc289f10a4fce291e1d35b699e280e08ff496e5adf0661555d4`

Release commit:

`f891617`

## Install

For beta releases, flash this exact verified `.bin` using the README install steps.

The browser installer comes later with GitHub Pages. It will require Chrome, Brave, or Edge on desktop; Safari and Firefox cannot flash ESP32 boards over Web Serial.

After flashing, unplug the board, wait about 3 seconds, then plug it back in.

## Changelog

The speaks-your-language release: the entire wallet now works in 19 languages
(21 regional variants), with Bitcoin terms as native speakers actually use
them — not word-for-word translations.

### Added — the wallet in your language

- **21 languages/variants, one firmware.** English, Čeština, Dansk, Deutsch,
  Español (España / México), Français, Hrvatski, Italiano, Nederlands, Norsk
  Bokmål, Polski, Português (Brasil / Portugal), Svenska, Tiếng Việt, Türkçe,
  Русский, 日本語, 한국어, 中文. Every wallet screen — including the home
  tiles — switches instantly; no reflash, no reboot.
- **Language picker with flags.** SETTINGS → LANGUAGE lists every language in
  its own name and script, alphabetically, with the Spanish and Portuguese
  variants side by side. Each name renders in its own regional font, so
  Japanese and Chinese keep their correct character shapes.
- **Choose your language before creating a wallet.** First boot offers the
  picker on the setup screen, so you never create a wallet in a language you
  can't read.
- **Terminology that sounds native.** Each language follows a reviewed
  glossary anchored to Bitcoin Core and established wallet conventions. The
  passphrase is never called a "password" in any language — it adds a layer
  of security; it does not lock the words.
- The fruit game stays English on purpose. It is the cover story.

### Changed

- **Unlock gesture is more forgiving.** Long KISS swipes no longer drop the
  trailing letters; the check now tolerates a blurred S while still requiring
  the full four-letter shape.
- **Firmware layout changed** (the app partition grew for the new fonts). The
  release image handles this automatically. As with every beta, flashing
  erases the wallet stored on the device — restore it afterwards from your
  written words and passphrase.
