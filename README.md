<div align="center">

# KISS Signer 💋

**An airgapped single-sig Bitcoin signer hidden behind a fruit slashing arcade game.**

<img src="docs/media/kiss-reveal.gif" alt="Drawing the word KISS on the game menu, which opens the signer" width="560">

**Draw KISS on the menu to enter.** You can customize the swipe after setup.

<table>
<tr>
<td align="center"><img src="docs/readme/menu.png" alt="FRUIT ISLAND game menu" width="400"></td>
<td align="center"><img src="docs/readme/signer-home.png" alt="KISS Signer home screen" width="400"></td>
</tr>
<tr>
<td align="center">What everyone sees: a playable game</td>
<td align="center">What you see after drawing KISS</td>
</tr>
</table>

*Keep it simple. Make it clear. Make it safe.*

[Simulator](https://diybitcoin.github.io/kiss-signer/sim/) &nbsp;·&nbsp;
[Docs](https://diybitcoin.github.io/kiss-signer/guide.html) &nbsp;·&nbsp;
[Walkthrough](docs/walkthrough.md) &nbsp;·&nbsp;
[Changelog](CHANGELOG.md) &nbsp;·&nbsp;
[Roadmap](docs/ROADMAP.md) &nbsp;·&nbsp;
[Security](SECURITY.md) &nbsp;·&nbsp;
[Telegram](https://t.me/KISS_signer)

</div>

> [!CAUTION]
> **Experimental beta firmware. Do not trust it with meaningful funds.**

## What it is

- **Seed words plus a passphrase.** The passphrase is typed every time and
  never stored.
- **Airgapped.** Transactions move by QR code or SD card. The radio chip is held
  off from the moment it boots.
- **Works with Sparrow and BlueWallet**, or any app that can watch a
  descriptor and pass transactions by QR or SD card.
- **Shows everything before you sign.** Amounts, fee and change are checked on
  the device.

Runs on three ESP32-P4 boards with a touch screen, camera and microSD slot. No
soldering.

- Guition **JC4880P443C**, 4.3in, 800×480
- Waveshare **ESP32-P4-WIFI6-Touch-LCD-3.5**, 3.5in, 480×320
- Guition **JC1060P470C**, 7in, 1024×600

Boards with the newer v3.x ESP32-P4 chip are not supported yet
([roadmap](docs/ROADMAP.md)).

Inspired by [Bowser](https://github.com/arcbtc/bowser-bitcoin-hardware-wallet),
a Bitcoin signer hidden under a Tetris game.

## Install

Easiest: [the install page](https://diybitcoin.github.io/kiss-signer/) flashes
it from your browser.

By hand: from the
[latest release](https://github.com/DIYbitcoin/kiss-signer/releases/latest),
download your board's firmware (`kiss-signer-VERSION.bin` for the Guition 4.3in,
`-ws35.bin` for the Waveshare 3.5in, `-jc1060.bin` for the Guition 7in) plus
`SHA256SUMS`, `SHA256SUMS.asc` and `kiss_signer_pgp.asc`.

**Verify first.**

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS      # expect this fingerprint:
# 166A CBF3 7786 FCEA A694  96DE 886F 1BFE B84E F1C0
shasum -a 256 --ignore-missing -c SHA256SUMS   # macOS (Linux: sha256sum)
```

Cross-check that fingerprint somewhere other than this page.

**Then flash.**

```sh
pip install esptool
# port: /dev/cu.usbmodem* or /dev/cu.wchusbserial* (macOS) | /dev/ttyACM* (Linux) | COMx (Windows)
esptool --chip esp32p4 -p <port> -b 460800 \
  --before default-reset --after no-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0 kiss-signer-VERSION.bin
```

> [!WARNING]
> This erases the whole chip, **keys included**. Have your seed words and
> passphrase first.

Then **unplug, wait 3 seconds, plug back in.**

**No internet?** Each release has an offline zip with the install page and the
firmware.

**Updating?** Copy your board's `-update.bin` to an SD card, then SETTINGS >
FIRMWARE. Your keys stay.

## First boot

Draw KISS on the menu. Setup takes two minutes: create or restore keys, write
down the seed words, confirm them, choose a passphrase.

<table>
<tr>
<td align="center"><img src="docs/readme/setup-2-words.png" alt="Write down the 12 recovery words" width="400"></td>
<td align="center"><img src="docs/readme/verify-backup.png" alt="Backup verified: every word matched" width="400"></td>
</tr>
<tr>
<td align="center">Write down the seed words</td>
<td align="center">Type them back to confirm</td>
</tr>
</table>

> [!TIP]
> **Check your backup before you fund it**: KEYS > BACKUP > SEED WORDS. Also
> write down the fingerprint on the home screen. A mistyped passphrase gives no
> error, it just opens different, empty keys.

## Day to day

Pair with Sparrow or BlueWallet under **KEYS > PAIR COORDINATOR**. The app on
your computer watches and builds transactions; KISS checks and signs them.

<table>
<tr>
<td align="center"><img src="docs/readme/use-1-receive.png" alt="Receive screen with address QR" width="400"></td>
<td align="center"><img src="docs/readme/use-2-sign.png" alt="Sign screen showing amounts, fee, and hold to sign" width="400"></td>
</tr>
<tr>
<td align="center">Check the address on the device</td>
<td align="center">See every amount before you sign</td>
</tr>
</table>

Try it on testnet first: **SETTINGS > SIGNER > NETWORK**.

Before you sign, the device warns you about a high fee, dust, or change that
hurts your privacy.

## Docs

The [guide](https://diybitcoin.github.io/kiss-signer/guide.html) has the details.
The [walkthrough](docs/walkthrough.md) is the short version.

## Build it yourself

You only need Docker. Builds are reproducible, so your hashes should match
[CI](.github/workflows/reproducible-build.yml)'s.

```sh
tools/build_release.sh
shasum -a 256 build-release/guition_kiss_bringup.bin

# the Waveshare 3.5in
KISS_BOARD=ws35 tools/build_release.sh
shasum -a 256 build-release-ws35/ws35_kiss_bringup.bin

# the Guition 7in
KISS_BOARD=jc1060 tools/build_release.sh
shasum -a 256 build-release-jc1060/jc1060_kiss_bringup.bin
```

## License

[MIT](LICENSE). Third party components keep their own licenses, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
