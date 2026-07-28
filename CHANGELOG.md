# Changelog

All notable, user-facing changes to KISS Signer. Dates are ISO (YYYY-MM-DD).
This is a Bitcoin signer — entries are written so a non-developer can tell what
changed and why it matters. Versions follow the firmware tags.

## [0.1.0-beta6] — 2026-07-27

The plausible-deniability release. Drawing KISS now opens a real signer that is
not your main one, and your main one hides behind a stroke only you know.

### Added — a signer you can hand over

- **A decoy signer, behind the gesture everyone already knows.** Drawing KISS
  opens a working signer with no passphrase: its own fingerprint, pairs with a
  coordinator, signs real transactions. It is not a fake — it is your seed with
  an empty passphrase, which is a genuinely different wallet. Put a small amount
  in it so it is not suspiciously empty.
- **Your real wallet hides behind one extra stroke.** Draw KISS, then add an
  underline, overline, strike, slash, circle or check. Only that opens the
  passphrase prompt. Plain KISS always keeps working and always opens the decoy,
  so nobody can lock themselves out of their own device by forgetting a stroke.
- **Set during setup, changed only from the real wallet.** SETTINGS → WAYS IN
  does not exist in a decoy session, so the decoy never reveals that a second
  signer is configurable.
- **What a seed actually is.** The setup wizard now explains it: 12 words in the
  BIP39 list, that the words plus your passphrase *are* the wallet, and that the
  same words restore into Sparrow, BlueWallet or any BIP39 wallet.
- **A way out of WRITE THESE DOWN.** That screen had no exit at all — if you had
  no paper to hand, the only way out was pulling the power.
- **A warning before CREATE NEW SEED.** It walked straight into the wizard with
  nothing said. WIPE has always gated itself; now so does this.

### Fixed

- **Settings and SCAN QR appeared to freeze, and only a power cycle got out.**
  They were never frozen. Opening the decoy skipped the login screen, and the
  login screen was the only thing that switched on touch input for wallet
  screens — so the screen drew perfectly and ignored every tap. The game kept
  working because it reads the touch panel directly. This affected anyone
  unlocking with plain KISS. **This is the reason to update.**
- **The extra stroke was unreachable.** KISS was recognised the instant you
  lifted the last S, so the decoy opened before you could draw anything after
  it. The device now waits briefly for a stroke.
- **Circles had to be drawn small.** The bigger you drew the loop, the more
  precisely it had to close. Now a big, loosely-closed circle reads correctly.

### Changed

- **KISS is easier to draw.** The shape check was tuned when it guarded the
  passphrase prompt, where a false match was a giveaway. It now opens the decoy,
  where a fumbled shape costs nothing. Two S's are still required.
- **Creating a seed is always 12 words.** 128 bits is not brute-forceable, and
  24 words doubles the length of the one step where mistakes actually happen:
  copying them to paper. RESTORE still accepts 12 or 24.
- **ERASE SEED, not WIPE WALLET.** Erasing this device does not erase your
  wallet — your paper and passphrase still restore it. Calling it "wipe wallet"
  told you your coins were gone, which is the opposite of true and a bad thing
  to believe while deciding whether to press it.
- **The PSBT explainer says what the letters mean.** Partially Signed Bitcoin
  Transaction: a transaction built but not yet signed. The coordinator builds it
  and holds no keys; KISS holds the keys and stays offline. It no longer calls a
  PSBT a "file" — over QR there is no file anywhere.
- **A wallet with no passphrase stops describing one.** The fingerprint and
  warning screens used to talk about a passphrase you did not have, and the
  stroke setup was offered even though both ways in reached the same wallet.

## [0.1.0-beta5] — 2026-07-27

Tagged but never published to the installer; its changes ship here in beta6.

- **Silent Payments (BIP352).** Receive at one reusable address without address
  reuse, with scan-key export for watch-only, and spending from silent-payment
  coins (BIP374 DLEQ, BIP375 PSBT outputs), tested against the reference vectors.
- **Amnesic mode.** Load a seed, sign, power off. Nothing is stored.
- **Restore from a SeedQR** instead of typing 12 or 24 words by hand.
- **Every explainer readable at arm's length** in all 21 languages, measured
  rather than guessed, with copy cut roughly in half.
- **Seeds can be sealed to an SD card** rather than kept in flash.

## [0.1.0-beta4] — 2026-07-19

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

## [0.1.0-beta3] — 2026-07-15

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

## [0.1.0-beta2] — 2025 (previous release)

- First public beta: the fruit game with the hidden signer behind it, BIP39/BIP84
  single-sig, animated-QR (BC-UR) and SD-card PSBT transport, on-device PSBT
  verification, radio held in reset. Signed release artifacts with GPG + SHA256.
  Not for meaningful funds.
