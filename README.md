<div align="center">

# KISS Signer 💋

**An airgapped single-sig Bitcoin signer hidden behind a fruit slashing arcade game.**

<img src="docs/readme/badge-status.svg" alt="status: beta"> <img src="docs/readme/badge-version.svg" alt="version: 0.1.0-beta7"> <img src="docs/readme/badge-chip.svg" alt="chip: ESP32-P4"> <img src="docs/readme/badge-radio.svg" alt="radio: disabled"> <img src="docs/readme/badge-encryption.svg" alt="flash encryption: in progress"> <img src="docs/readme/badge-signing.svg" alt="signing: single-sig + SP"> <img src="docs/readme/badge-langs.svg" alt="languages: 21"> <img src="docs/readme/badge-license.svg" alt="license: Apache-2.0">

<img src="docs/media/kiss-reveal.gif" alt="Drawing the word KISS on the game menu, which opens the signer" width="560">

<sub><b>Draw KISS anywhere on the menu.</b> Nothing appears on screen while you
draw. The strokes are traced onto this picture so you can see where they go.</sub>

<table>
<tr>
<td align="center"><img src="docs/readme/menu.png" alt="FRUIT ISLAND game menu" width="400"></td>
<td align="center"><img src="docs/readme/wallet.png" alt="KISS Signer home screen" width="400"></td>
</tr>
<tr>
<td align="center"><sub><b>What everyone sees</b>, a real, playable game</sub></td>
<td align="center"><sub><b>What only you see</b>, gesture + passphrase</sub></td>
</tr>
</table>

<sub>All screenshots on this page are rendered by the desktop simulator from the real firmware sources.</sub>

*Keep it simple. Make it clear. Make it safe.*

[Docs](docs/guide.html) &nbsp;·&nbsp;
[Roadmap](ROADMAP.md) &nbsp;·&nbsp;
[Security plan](docs/security-plan.md) &nbsp;·&nbsp;
[Report a vulnerability](SECURITY.md) &nbsp;·&nbsp;
[Verify a release](docs/verify-release.html) &nbsp;·&nbsp;
[Walkthrough](docs/walkthrough.md) &nbsp;·&nbsp;
[Contributing](CONTRIBUTING.md) &nbsp;·&nbsp;
[Changelog](CHANGELOG.md) &nbsp;·&nbsp;
[Telegram](https://t.me/KISS_signer)

</div>

Inspired by [Bowser](https://github.com/arcbtc/bowser-bitcoin-hardware-wallet),
a Bitcoin signer hidden under a Tetris game.

> [!CAUTION]
> **This is experimental beta firmware. Do not trust it with meaningful funds.**
> The current release is named in [`VERSION`](VERSION) and the
> [changelog](CHANGELOG.md).

- **Recovery words + passphrase derive your wallet.** The ordered words are
  commonly called a "seed phrase"; technically, BIP39 processes the mnemonic
  sentence and passphrase into a binary seed. The passphrase is typed fresh
  every time and never stored. An empty passphrase uses the base wallet; any
  non-empty entry changes the derived wallet. Every entry is valid, so there is
  no "wrong passphrase" error. A strong passphrase can protect funds if the
  words are exposed, but an attacker can test passphrase guesses offline.
- **Airgapped by hardware:** transactions move by animated QR (BC-UR) or SD
  card. The ESP32-P4 running KISS has no radio. The device's ESP32-C6 radio
  chip is held in reset from the first instruction, every boot, and no
  wireless stack is compiled in: the release build fails if any radio or
  networking code links.
- **Online wallet compatible:** pairing offers Sparrow on desktop (descriptor)
  and BlueWallet on mobile (zpub), and any other coordinator that reads a
  descriptor or a zpub will work. The coordinator watches balances, builds
  transactions and broadcasts. It cannot sign or authorize anything by itself.
  KISS stays offline, verifies the PSBT, and signs only what it can fully show.
- **Shows everything, warns in plain words, teaches as you go:** before you sign,
  every amount, the fee, and each change output are re-derived and shown on the
  device. It flags an unusually high fee, and a tiny "dust" coin or change that
  hurts your privacy, each as a soft caution you acknowledge rather than a
  silent surprise. Address reuse is different: KISS cannot see the chain, so
  RECEIVE keeps a standing reminder instead of guessing which addresses were
  paid. A **?** on any unfamiliar term opens a short plain words card, with a
  small diagram where a picture helps.

Runs on the Guition **JC4880P443C** dev board: ESP32-P4, 480×800 MIPI-DSI
touch panel, camera, SD card slot. No soldering.

## Install (beta)

For this beta, use the signed artifacts attached to the
[latest GitHub Release](https://github.com/kkdao/kiss-signer/releases/latest).
A browser installer lives under `docs/` and hashes the firmware against this
release before it offers you the button. It goes live at
[kkdao.github.io/kiss-signer](https://kkdao.github.io/kiss-signer/) when this
repo goes public, so until then the assets above are the route.

**1. Download** these release assets into one folder, replacing `VERSION` with
the release number:

- `kiss-signer-VERSION.bin`
- `SHA256SUMS`
- `SHA256SUMS.asc`
- `kiss_signer_pgp.asc`

**2. Verify** before flashing:

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS      # expect this fingerprint:
# 166A CBF3 7786 FCEA A694  96DE 886F 1BFE B84E F1C0
shasum -a 256 --ignore-missing -c SHA256SUMS   # macOS (Linux: sha256sum)
```

> [!TIP]
> Cross-check the fingerprint from more than one place. It is only as
> trustworthy as this README.

**3. Flash** (macOS / Linux / WSL / Git Bash. On plain Windows, put the
`esptool` command on one line without the `\` continuations):

```sh
pip install esptool   # or: pipx install esptool / uvx esptool

# port: /dev/cu.usbmodem* (macOS) | /dev/ttyACM* (Linux) | COMx (Windows)
# replace VERSION with the release number
shasum -a 256 --ignore-missing -c SHA256SUMS   # step 2 again, beside the write on purpose
esptool --chip esp32p4 -p <port> -b 460800 \
  --before default-reset --after no-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0 kiss-signer-VERSION.bin
```

> [!IMPORTANT]
> After flashing: **unplug the device, wait about 3 seconds, plug it back in.**
> The device only starts new firmware from a real power-on.

Stuck on any step? The [docs](docs/) walk through each one per OS.

### No internet where you flash

Every release also carries `kiss-signer-VERSION-offline.zip`, about 4 MB: the
install page, the firmware and the signed hashes in one download. Get it on a
machine that has a network, check its signature, then move it to the machine
that does not:

```sh
gpg --verify kiss-signer-VERSION-offline.zip.asc kiss-signer-VERSION-offline.zip
```

Unzip it, run `serve.command` (macOS), `serve.bat` (Windows) or `./serve.sh`
(Linux), and open the address it prints in Chrome, Brave or Edge. It serves to
that one computer only and reaches nothing else. `00-START-HERE.txt` inside says
the same in more detail.

> [!NOTE]
> This is the only way to flash from a browser with the network off. The page
> hosted on GitHub Pages fetches itself while you use it, so pulling the plug
> halfway leaves you with a page that cannot finish.

## First boot

The game is what boots. A secret gesture on the game menu opens the signer
(covered in the docs), then setup takes two minutes:

<table>
<tr>
<td align="center"><img src="docs/readme/setup-1-choose.png" alt="Set up your wallet: create new or restore" width="400"></td>
<td align="center"><img src="docs/readme/setup-2-words.png" alt="Write down the 12 recovery words" width="400"></td>
</tr>
<tr>
<td align="center"><sub>Create a new wallet, or restore from words</sub></td>
<td align="center"><sub>Write the 12 recovery words on paper; the same passphrase is also required</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/readme/setup-3-quiz.png" alt="Quiz proves the words were written down" width="400"></td>
<td align="center"><img src="docs/readme/setup-4-passphrase.png" alt="Create your passphrase" width="400"></td>
</tr>
<tr>
<td align="center"><sub>A short quiz proves you really wrote them down</sub></td>
<td align="center"><sub>Pick a passphrase, typed at every unlock, never stored</sub></td>
</tr>
</table>

> [!TIP]
> **Before you fund it, verify your backup.** WALLET → BACKUP WORDS → **VERIFY
> MY COPY** has you type your words from paper; the device confirms they rebuild
> this exact wallet and never shows the stored words. A wrong or missing word is
> reported by position ("word #N"). Bad backups lose more coins than bad signers
> do, so it is worth the two minutes.
>
> **Then write the fingerprint on the same piece of paper.** Every passphrase is
> valid, so a typo never shows an error, it silently opens a different and empty
> wallet. The eight character code on the home screen is the only way to notice.
> If it ever differs from your paper, you typed the passphrase wrong.

<div align="center">
<img src="docs/readme/verify-backup.png" alt="Backup verified: every word matched" width="400"><br>
<sub><b>Verify my copy</b>: type the paper words, the device confirms without revealing them</sub>
</div>

## Day to day

Pair with an online coordinator app: **WALLET → PAIR COORDINATOR**, then pick
DESKTOP (descriptor, for Sparrow) or MOBILE (zpub, for BlueWallet). The app
watches the chain and builds transactions; KISS only ever sees the PSBT, shows
you exactly what it spends, and signs. Keys never leave the device.

First time? Do a practice run before trusting the setup with real coins:
send a tiny amount in, then back out again.

> [!NOTE]
> BlueWallet labels the imported wallet **"watch-only." That is expected**: it
> holds only your public key, so it can show balances, hand out receive
> addresses, build transactions and broadcast signed ones. It cannot sign or
> authorize a spend by itself; every spend is reviewed and signed on KISS.
> Quick pairing check: import the zpub, then compare the first receive address
> in BlueWallet against **Receive → VERIFY** on the device before using it.

<table>
<tr>
<td align="center"><img src="docs/readme/use-1-receive.png" alt="Receive screen with address QR" width="400"></td>
<td align="center"><img src="docs/readme/use-2-sign.png" alt="Sign screen showing amounts, fee, and hold to sign" width="400"></td>
</tr>
<tr>
<td align="center"><sub><b>Receive</b>, verify the address on the device, not the computer</sub></td>
<td align="center"><sub><b>Sign</b>, every amount and change output shown first. An unusually high fee turns amber before you can sign</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/readme/use-3-qr.png" alt="Signed transaction returned as animated QR" width="400"></td>
<td align="center"><img src="docs/readme/use-4-sd.png" alt="Signed PSBT saved to SD card" width="400"></td>
</tr>
<tr>
<td align="center"><sub><b>Hand back by QR</b>, scan the loop with your coordinator. <b>EASY SCAN</b> slows it and enlarges the dots if your phone struggles</sub></td>
<td align="center"><sub><b>…or by SD card</b>, the device never touched the network</sub></td>
</tr>
</table>

### Warnings and learning, on the device

Before every signature KISS re-derives the whole transaction on its own screen
and calls out anything worth a second look, always a caution you acknowledge,
never a silent block:

- **High fee.** The fee turns amber if it's a large share of what you send (the
  rule Krux uses), or an outsized sat/vB rate.
- **Dust and privacy.** Spending a tiny coin, or leaving tiny change, is
  flagged: both can be used to link and track your addresses. Change below the
  network dust limit is flagged louder.

Address reuse is handled differently, because it has to be. KISS never sees the
chain, so it cannot know which of your addresses were actually paid. Rather than
guess, RECEIVE hands you a fresh address each time and keeps a standing reminder
to use a new one per payment. Reusing an address links your payments together.

When a caution fires, the sign button is gated behind an **I UNDERSTAND** tap,
and a **?** opens a plain words card explaining exactly why. Tap **?** anywhere a
term is unfamiliar (RBF, fingerprint, coordinator, dust), or tap the fingerprint
on the home screen, to learn as you go.

<table>
<tr>
<td align="center"><img src="docs/readme/warn-caution.png" alt="Sign screen showing stacked cautions and an I UNDERSTAND gate" width="400"></td>
<td align="center"><img src="docs/readme/learn-card.png" alt="Fingerprint explainer card with a WORDS + PASSPHRASE to FINGERPRINT diagram" width="400"></td>
</tr>
<tr>
<td align="center"><sub><b>Cautions stack</b>: a short summary, a <b>?</b> for why, and <b>I UNDERSTAND</b> before you can sign</sub></td>
<td align="center"><sub><b>Learn as you go</b>: plain words cards, with a small diagram where a picture helps</sub></td>
</tr>
</table>

## Docs

The full guides live in [`docs/`](docs/) for now and move to GitHub Pages once
the repo is public. They cover:

- **Verify the release.** GPG + SHA256 walkthrough, including Windows
- **Flash with esptool.** Command line install on macOS / Linux / Windows
- **Flash with no internet.** The offline zip: one download, unzip, serve, flash
- **Build from source.** Reproducible Docker builds, hashes match CI
- **Flash encryption.** The final signer build that cannot be undone, and what it costs
- **First boot.** The unlock gesture, passphrase model, pairing Sparrow
- **Simulator & tests.** Try the UI and run the test suite, no hardware

[**Walkthrough**](docs/walkthrough.md) covers the five things to do before the
wallet holds anything you care about: getting back in from the game, pairing
Sparrow, verifying a receive address, taking a test payment, and signing one.

Every screenshot in this README and in the docs is a frame the simulator
rendered from the current firmware, generated by `bash tools/gen_docs_shots.sh`
rather than captured by hand. Re-run it after a UI change and commit what
moves; CI fails if a screenshot points at a frame the simulator stopped
saving.

What changed between releases is in the [changelog](CHANGELOG.md).

## Build from source

Only requirement is Docker; builds are reproducible, so your hashes must match
[CI](.github/workflows/reproducible-build.yml)'s:

```sh
tools/build_release.sh     # verified release build -> build-release/
```

## Reproducible builds

Do not trust a firmware download just because it is attached to a release.
KISS release builds are rebuilt by GitHub Actions, and the same commit should
produce the same SHA256 hashes locally.

```sh
tools/build_release.sh
shasum -a 256 \
  build-release/guition_kiss_bringup.bin \
  build-release/bootloader/bootloader.bin \
  build-release/partition_table/partition-table.bin
```

Compare those hashes with the matching GitHub Actions run. For final funded
devices, use `tools/build_encrypted_release.sh` and compare the encrypted-release
hashes instead.

## Flash encryption, the final signer build

`tools/build_encrypted_release.sh` builds the hardened profile: flash
encryption in release mode plus NVS encryption, so the stored seed cannot be
read out of the chip.

> [!WARNING]
> The first boot **burns eFuses, with no undo**, and the device can **never be
> reflashed** after it. Fresh final signer device only. Read the
> [docs](docs/guide.html) twice before touching it.

## License

KISS Signer's original source code and documentation are licensed under the
[Apache License 2.0](LICENSE). Vendored third party components and assets keep
their own licenses: [libwally-core](components/libwally-core) (MIT), Espressif
components and ESP-IDF (Apache-2.0), LVGL (MIT), the fonts (SIL OFL 1.1), the
[Twemoji](https://github.com/twitter/twemoji) kiss mark and language flags
(CC-BY 4.0), and the decoy game's fruit art (Microsoft Fluent Emoji, MIT) under
`assets/`. The full list, component by component, is in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No unlicensed assets are used.

**Use at your own risk. This is experimental firmware; do not trust it with
meaningful funds.**
