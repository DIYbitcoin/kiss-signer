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
[Telegram](https://t.me/KISS_signer) &nbsp;·&nbsp;
[X](https://x.com/KISS_signer)

</div>

> [!CAUTION]
> **Experimental beta firmware. Do not trust it with meaningful funds.**

## What it is

- **Seed words plus a passphrase.** The passphrase is typed or scanned every
  time and never stored.
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

Pick one. All three end the same way: **unplug, wait 3 seconds, plug back in.**

| | How | You need |
| --- | --- | --- |
| 🟢 **Easy** | Install page in your browser | Chrome, Brave or Edge, a USB cable |
| 🟡 **Medium** | Verify the files yourself, then flash | A terminal, `gpg`, `esptool` |
| 🔴 **Hard** | Build it from source | Docker |

> [!WARNING]
> Flashing erases the whole chip, **keys included**. Have your seed words and
> passphrase first.

### 🟢 Easy: from your browser

1. Open the [install page](https://diybitcoin.github.io/kiss-signer/).
2. Pick your board, plug it in, click **Connect and install**.
3. Unplug, wait 3 seconds, plug back in.

### 🟡 Medium: verify, then flash

1. From the [latest release](https://github.com/DIYbitcoin/kiss-signer/releases/latest),
   download your board's firmware plus `SHA256SUMS`, `SHA256SUMS.asc` and
   `kiss_signer_pgp.asc`.

   | Board | Firmware |
   | --- | --- |
   | Guition 4.3in | `kiss-signer-VERSION.bin` |
   | Waveshare 3.5in | `kiss-signer-VERSION-ws35.bin` |
   | Guition 7in | `kiss-signer-VERSION-jc1060.bin` |

2. Verify. Cross-check the fingerprint somewhere other than this page.

   ```sh
   gpg --import kiss_signer_pgp.asc
   gpg --verify SHA256SUMS.asc SHA256SUMS      # expect this fingerprint:
   # 166A CBF3 7786 FCEA A694  96DE 886F 1BFE B84E F1C0
   shasum -a 256 --ignore-missing -c SHA256SUMS   # macOS (Linux: sha256sum)
   ```

3. Flash.

   ```sh
   pip install esptool
   # port: /dev/cu.usbmodem* or /dev/cu.wchusbserial* (macOS) | /dev/ttyACM* (Linux) | COMx (Windows)
   esptool --chip esp32p4 -p <port> -b 460800 \
     --before default-reset --after no-reset write-flash \
     --flash-mode dio --flash-size 16MB --flash-freq 80m \
     0 kiss-signer-VERSION.bin
   ```

4. Unplug, wait 3 seconds, plug back in.

> [!TIP]
> **No internet on that computer?** Download `kiss-signer-VERSION-offline.zip`
> and its `.asc` instead. Verify the zip with `gpg --verify`, unzip it, run the
> serve script inside, and use the install page offline.

### 🔴 Hard: build it from source

You only need Docker. Builds are reproducible, so your hashes should match
[CI](.github/workflows/reproducible-build.yml)'s.

```sh
tools/build_release.sh                   # Guition 4.3in
KISS_BOARD=ws35 tools/build_release.sh   # Waveshare 3.5in
KISS_BOARD=jc1060 tools/build_release.sh # Guition 7in
```

Each writes its image to its own `build-release*` folder; hash it with
`shasum -a 256` and compare.

## 🔄 Updating

Copy your board's `-update.bin` from the latest release to an SD card, then
**SETTINGS > FIRMWARE**. Your keys stay.

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

## 🔐 Security

Found a vulnerability? Email **diybitcoin@protonmail.com**. Please do not open a
public issue for anything that could put funds at risk. Details in
[SECURITY.md](SECURITY.md).

## 💬 Community

- **Telegram:** [t.me/KISS_signer](https://t.me/KISS_signer) for questions,
  ideas and help
- **X:** [@KISS_signer](https://x.com/KISS_signer) for news and releases
- **GitHub issues** for bugs that do not put funds at risk

## 🤝 Contributing

Pull requests are welcome. To keep reviews quick:

1. **One open pull request at a time.** GitHub enforces this; drafts do not
   count. When yours is merged or closed, open the next.
2. **One fix or feature per pull request**, based on the `develop` branch.
3. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: building, tests, and testing
   on a real board.

New to pull requests? GitHub's
[guide](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request-from-a-fork)
walks you through it. Found a vulnerability? Email it instead, see Security
above.

## License

[MIT](LICENSE). Third party components keep their own licenses, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
