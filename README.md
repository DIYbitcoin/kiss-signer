<div align="center">

# KISS Signer 💋

**An airgapped single-sig Bitcoin signer hidden behind a fruit slashing arcade game.**

<img src="docs/readme/badge-status.svg" alt="status: beta"> <img src="docs/readme/badge-version.svg" alt="version: 0.1.0-beta8"> <img src="docs/readme/badge-chip.svg" alt="chip: ESP32-P4"> <img src="docs/readme/badge-radio.svg" alt="radio: disabled"> <img src="docs/readme/badge-encryption.svg" alt="flash encryption: in progress"> <img src="docs/readme/badge-signing.svg" alt="signing: single-sig + SP"> <img src="docs/readme/badge-langs.svg" alt="languages: 21"> <img src="docs/readme/badge-license.svg" alt="license: MIT">

<img src="docs/media/kiss-reveal.gif" alt="Drawing the word KISS on the game menu, which opens the signer" width="560">

<sub><b>Draw KISS anywhere on the menu.</b> Nothing appears on screen while you
draw. The strokes are traced onto this picture so you can see where they go.</sub>

<table>
<tr>
<td align="center"><img src="docs/readme/menu.png" alt="FRUIT ISLAND game menu" width="400"></td>
<td align="center"><img src="docs/readme/signer-home.png" alt="KISS Signer home screen" width="400"></td>
</tr>
<tr>
<td align="center"><sub><b>What everyone sees</b>, a real, playable game</sub></td>
<td align="center"><sub><b>What only you see</b>, gesture + passphrase</sub></td>
</tr>
</table>

<sub>All screenshots on this page are rendered by the desktop simulator from the real firmware sources.</sub>

*Keep it simple. Make it clear. Make it safe.*

[Simulator](https://kkdao.github.io/kiss-signer/sim/) &nbsp;·&nbsp;
[Docs](docs/guide.html) &nbsp;·&nbsp;
[Roadmap](ROADMAP.md) &nbsp;·&nbsp;
[Security plan](docs/security-plan.md) &nbsp;·&nbsp;
[Report a vulnerability](SECURITY.md) &nbsp;·&nbsp;
[Verify a release](docs/verify-release.html) &nbsp;·&nbsp;
[Walkthrough](docs/walkthrough.md) &nbsp;·&nbsp;
[Blind draw](docs/blind-draw.md) &nbsp;·&nbsp;
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

- **Seed words + passphrase derive your keys.** The passphrase is typed fresh
  every time and never stored. Every entry is valid, so there is no wrong
  passphrase error: a different one quietly opens different keys. It protects
  you if your paper is found, but a thief holding the paper can guess at it
  offline, so make it a long one.
- **Airgapped by hardware.** Transactions move by animated QR or SD card. The
  radio chip is held in reset from the first instruction of every boot, and the
  release build fails if any networking code links into it.
- **Works with the coordinator you already use.** Pairing offers Sparrow on
  desktop and BlueWallet on mobile, and anything else that reads a descriptor or
  a zpub. The coordinator watches balances and builds payments; it cannot sign.
- **Shows everything before you sign.** Every amount, the fee and the change are
  worked out again on the device and shown. It cautions on a high fee, on dust,
  and on change that hurts your privacy. A **?** on any unfamiliar word opens a
  plain words card.

Runs on the Guition **JC4880P443C** dev board: ESP32-P4, 480×800 MIPI-DSI
touch panel, camera, SD card slot. No soldering.

## 📦 Install (beta)

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

> [!WARNING]
> This writes the whole chip, **including the area that holds your keys**. Seed
> words already on the device are erased; in SD mode the device key is erased too, so
> an existing `kiss-seed.enc` card becomes unopenable. Have your seed words
> on paper and your passphrase in hand before flashing a device that holds keys.

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

### 🌐 No internet where you flash

Every release also carries `kiss-signer-VERSION-offline.zip`, around 6 MB: the
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

### ⬆️ Upgrading with no computer at all

A running signer takes its next firmware off the SD card, so an upgrade needs no
cable and no browser. Use `kiss-signer-VERSION-update.bin`: check its hash
against `SHA256SUMS`, copy it to the card, then SETTINGS > FIRMWARE. The screen
shows the version on the device beside the one on the card, so you can see which
replaces which. Slide to install and leave it plugged in; the screen goes dark
while the flash is written and the backlight climbing back is the progress bar.

**Use the `-update.bin`, not the plain `.bin`.** The plain one is for flashing
over USB and starts with a bootloader, so the signer looks in the wrong place
and reports nothing to install.

The device checks both signatures itself, ECDSA and post quantum, before
anything becomes bootable. A release older than that second signature is refused
with the reason. If an update does not start, the signer keeps the firmware it
already had.

## 🔑 First boot

The game is what boots. A secret gesture on the game menu opens the signer
(covered in the docs), then setup takes two minutes:

<table>
<tr>
<td align="center"><img src="docs/readme/setup-1-choose.png" alt="Set up this signer: create new keys or restore" width="400"></td>
<td align="center"><img src="docs/readme/setup-2-words.png" alt="Write down the 12 recovery words" width="400"></td>
</tr>
<tr>
<td align="center"><sub>Create new keys, or restore from seed words</sub></td>
<td align="center"><sub>Write the 12 recovery words on paper; the same passphrase is also required</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/readme/setup-3-quiz.png" alt="Quiz proves the seed words were written down" width="400"></td>
<td align="center"><img src="docs/readme/setup-4-passphrase.png" alt="Create your passphrase" width="400"></td>
</tr>
<tr>
<td align="center"><sub>A short quiz proves you really wrote them down</sub></td>
<td align="center"><sub>Pick a passphrase, typed at every unlock, never stored</sub></td>
</tr>
</table>

> [!TIP]
> **Verify your backup before you fund it.** KEYS > BACKUP > SEED WORDS >
> **CHECK MY COPY** has you type your paper back; the device confirms it rebuilds
> these keys, names any wrong word by position, and never shows the stored ones.
> Bad backups lose more coins than bad signers do.
>
> **Then write the fingerprint on the same paper.** A mistyped passphrase never
> errors, it silently opens different, empty keys, and that eight character code
> on the home screen is the only way to notice.

<div align="center">
<img src="docs/readme/verify-backup.png" alt="Backup verified: every word matched" width="400"><br>
<sub><b>Check my copy</b>: type your seed words from paper, the device confirms without revealing them</sub>
</div>

## 💸 Day to day

Pair with an online coordinator: **KEYS > PAIR COORDINATOR**, then DESKTOP
(descriptor, for Sparrow) or MOBILE (zpub, for BlueWallet). Two machines, and
neither trusts the other:

| | |
| --- | --- |
| **The coordinator** | watches the chain, hands out addresses, builds the payment, broadcasts the signed one |
| **KISS** | holds the keys, shows you what the payment really spends, signs |

It never sees more than a PSBT, and the keys never leave it.

First time? Rehearse on testnet before trusting the setup with real coins.
**SETTINGS → SIGNER → NETWORK** switches it, testnet coins are free from a
faucet, and the screens are the ones you will use for real: receive, verify,
sign, broadcast. Switch back when the chip beside the home title is the only
thing you still have to check.

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

### ⚠️ Warnings and learning, on the device

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
to use a new one per payment. Reuse lets anyone reading the chain link your
payments.

**Silent payments** (BIP352) skip that problem: the SILENT tab holds one address
you can hand out forever, and the sender's software turns it into a fresh on
chain address for each payment, so nothing links them in public. Pairing hands
your coordinator only a scan key, which finds those payments and cannot spend
them.

When a caution fires, the sign button is gated behind an **I UNDERSTAND** tap,
and a **?** opens a plain words card explaining exactly why. Tap **?** anywhere a
term is unfamiliar (RBF, fingerprint, coordinator, dust), or tap the fingerprint
on the home screen, to learn as you go.

<table>
<tr>
<td align="center"><img src="docs/readme/warn-caution.png" alt="Sign screen showing stacked cautions and an I UNDERSTAND gate" width="400"></td>
<td align="center"><img src="docs/readme/learn-card.png" alt="Fingerprint explainer card with a SEED WORDS + PASSPHRASE to FINGERPRINT diagram" width="400"></td>
</tr>
<tr>
<td align="center"><sub><b>Cautions stack</b>: a short summary, a <b>?</b> for why, and <b>I UNDERSTAND</b> before you can sign</sub></td>
<td align="center"><sub><b>Learn as you go</b>: plain words cards, with a small diagram where a picture helps</sub></td>
</tr>
</table>

## 📚 Docs

The full guides live in [`docs/`](docs/) for now and move to GitHub Pages once
the repo is public. They cover:

- **Verify the release.** GPG + SHA256 walkthrough, including Windows
- **Flash with esptool.** Command line install on macOS / Linux / Windows
- **Flash with no internet.** The offline zip: one download, unzip, serve, flash
- **Build from source.** Reproducible Docker builds, hashes match CI
- **Flash encryption.** The final signer build that cannot be undone, and what it costs
- **First boot.** The unlock gesture, passphrase model, pairing Sparrow
- **Simulator & tests.** Every screen in a browser, and the test suite, no hardware

[**Walkthrough**](docs/walkthrough.md) covers the five things to do before the
signer holds anything you care about: getting back in from the game, pairing
Sparrow, verifying a receive address, taking a test payment, and signing one.

Every screenshot in this README and in the docs is a frame the simulator
rendered from the current firmware, generated by `bash tools/gen_docs_shots.sh`
rather than captured by hand. Re-run it after a UI change and commit what
moves; CI fails if a screenshot points at a frame the simulator stopped
saving.

What changed between releases is in the [changelog](CHANGELOG.md).

## 🔨 Build from source

Only requirement is Docker; builds are reproducible, so your hashes must match
[CI](.github/workflows/reproducible-build.yml)'s:

```sh
tools/build_release.sh     # verified release build -> build-release/
```

## ✅ Reproducible builds

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

## 🔒 Flash encryption, the final signer build

`tools/build_encrypted_release.sh` builds the hardened profile: flash
encryption in release mode plus NVS encryption, so the stored seed cannot be
read out of the chip.

> [!WARNING]
> The first boot **burns eFuses, with no undo**, and the device can **never be
> reflashed over USB** after it (signed SD updates still work, and become the
> only firmware path). Fresh final signer device only. Read the
> [docs](docs/guide.html) twice before touching it.

## 📄 License

KISS Signer's original source code and documentation are licensed under the
[MIT License](LICENSE). Vendored third party components and assets keep
their own licenses: [libwally-core](components/libwally-core) (MIT), Espressif
components and ESP-IDF (Apache-2.0), LVGL (MIT), the fonts (SIL OFL 1.1), the
[Twemoji](https://github.com/twitter/twemoji) kiss mark and language flags
(CC-BY 4.0), and the decoy game's fruit art (Microsoft Fluent Emoji, MIT) under
`assets/`. The full list, component by component, is in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No unlicensed assets are used.

**Use at your own risk. This is experimental firmware; do not trust it with
meaningful funds.**
