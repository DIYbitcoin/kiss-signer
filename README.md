# kiss-wallet

An airgapped single-sig Bitcoin signer hidden behind a fruit-slash arcade game.
The device looks and plays like **FRUIT ISLAND**; a secret unlock gesture opens
**KISS Wallet** — an offline signer in the family of SeedSigner, Specter-DIY,
Krux, and Kern.

> Keep it small. Make it safe. Make it clear.

- **Seed + passphrase = your wallet.** The BIP39 passphrase is typed fresh every
  time, never stored, and there is no "wrong passphrase" error by design — a
  different passphrase simply opens a different wallet (deniability built in).
- **Airgapped by hardware:** transactions move by animated QR (BC-UR) or SD
  card. The ESP32-P4 has no WiFi/Bluetooth silicon and the firmware contains no
  networking stack — there is no radio to accidentally leave on.
- **Online wallet compatible:** exports descriptors for Sparrow and friends.
  The online (watch-only) wallet builds and broadcasts transactions; KISS stays
  offline, verifies the PSBT, and signs only what it can fully show.

**Status: `0.8.0-beta1` — experimental. Do not trust it with meaningful funds.**

## Hardware

Guition **JC4880P443C** dev board — ESP32-P4 (RISC-V), 480×800 MIPI-DSI touch
panel (ST7701 + GT911), OV02C10 MIPI-CSI camera, SDMMC card slot. No soldering.

---

## Install

Three ways, easiest first. All of them end the same way:
**unplug the board, wait ~3 seconds, plug it back in.** The board only starts
new firmware from a real power-on, never from a USB reset. A black screen
after flashing means you skipped this.

### Option A — web installer (easiest)

Flash straight from the browser, no toolchain. Use **Chrome, Brave, or Edge**
on any desktop OS — macOS, Windows, Linux. **Safari and Firefox cannot flash**
(no Web Serial support).

While this repo is private the installer is served locally:

```sh
git clone https://github.com/kkdao/kiss-wallet.git && cd kiss-wallet
python3 -m http.server 8321 -d docs
# open http://localhost:8321 in Chrome / Brave / Edge
```

The page hashes the firmware **in your browser** before the install button
unlocks, and shows the release-key fingerprint. Still verify the GPG signature
yourself (next section) — never skip that step just because a web page looks
green.

### Verifying a release (do this before Option A or B)

Release artifacts live in this repo under [`docs/installer/`](docs/installer/)
(not the GitHub Releases tab):

| file | what |
|---|---|
| `firmware/kiss-wallet-<version>.bin` | merged firmware image, flash at offset 0 |
| `SHA256SUMS` | hashes of the image and its parts |
| `SHA256SUMS.asc` | detached GPG signature over SHA256SUMS |
| `kiss_wallet_pgp.asc` | the release public key |
| `release.json` | version, commit, hashes, flash parameters |

```sh
cd docs/installer

# 1. import the release key and verify the signature
gpg --import kiss_wallet_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect: "Good signature from KISS Wallet releases"
# expect fingerprint: 166A CBF3 7786 FCEA A694  96DE 886F 1BFE B84E F1C0

# 2. check the firmware hash against the signed manifest
shasum -a 256 --ignore-missing -c SHA256SUMS     # macOS
sha256sum --ignore-missing -c SHA256SUMS         # Linux
```

Windows (PowerShell): `Get-FileHash firmware\kiss-wallet-<version>.bin` and
compare the hash to the first line of `SHA256SUMS` by eye (verify the GPG
signature with [Gpg4win](https://gpg4win.org)).

The fingerprint above is only as trustworthy as this README — cross-check it
from more than one place (repo, releases page, maintainer profile) like you
would for any bitcoin project.

### Option B — flash the signed binary from the command line

Needs Python. `esptool` works the same on macOS, Linux, and Windows:

```sh
pip install esptool        # or: pipx install esptool / uvx esptool

# find the port:
ls /dev/cu.usbmodem*       # macOS
ls /dev/ttyACM*            # Linux (add yourself to the dialout group if denied)
# Windows: COMx — check Device Manager

esptool --chip esp32p4 -p <port> -b 460800 \
  --before default-reset --after no-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0 docs/installer/firmware/kiss-wallet-<version>.bin
```

Then power-cycle (unplug, ~3 s, replug). Note: the merged image covers the
settings/wallet storage area, so flashing it wipes any wallet already on the
board — that is the expected clean start.

### Option C — build from source

Reproducing the binary yourself is the strongest verification. Only
requirement is [Docker](https://docs.docker.com/get-docker/) (the ESP-IDF
v6.0.1 toolchain runs inside the container; nothing else to install).

macOS / Linux:

```sh
tools/build_release.sh     # release profile -> build-release/
```

The script ends by verifying its own output — no development seed material in
the binary, version + commit string present — and prints the exact flash
commands for the result.

Windows: run the script under WSL or Git Bash (Docker Desktop required), or
build the development profile directly:

```powershell
docker run --rm -v "${PWD}:/project" -w /project espressif/idf:v6.0.1 idf.py -B build-disp build
```

(macOS/Linux equivalent: `docker run --rm -v "$PWD":/project -w /project
espressif/idf:v6.0.1 idf.py -B build-disp build` — the dev profile adds a boot
log fingerprint and dev banner.)

---

## Flash encryption (final-board build) — read twice, flash once

`tools/build_encrypted_release.sh` builds the hardened profile: flash
encryption in **release mode** plus **NVS encryption** (the stored seed words
are covered — plain flash encryption alone would leave the NVS data partition
readable). Secure boot is deliberately not enabled yet; it lands as its own
later pass.

**What it gives you:** the flash contents, including the stored seed, cannot
be read out of the chip. The AES key is generated on the device, burned into
eFuse, and is never readable by anyone — including you.

**What it costs — this is permanent:**

- The first boot **burns eFuses. There is no undo.**
- After that first boot the board can **never be reflashed again** — no
  serial, no web installer, no OTA. The firmware on it is frozen forever
  (which also locks out evil-maid reflashing — that is the point).
- First boot encrypts ~6 MB of flash in place and can sit on a black screen
  for a few minutes. **Do not unplug** until the game menu appears; losing
  power mid-encryption can brick the board.
- Only flash a **fresh board you intend as your final signer**. Test the
  normal release on it first.

```sh
tools/build_encrypted_release.sh    # builds + runs 12 safety checks,
                                    # then prints the one-time flash command
```

The script never flashes anything itself. On the device, Settings shows an
amber "flash not yet encrypted" line read live from the eFuse — after the
encrypted first boot that line disappears, and only then should you create
a wallet you care about.

---

## First boot

1. You get a fruit game. It really plays. Hand it to anyone.
2. **Unlock:** draw the word **KISS** across the menu with your finger — a
   clear **K** first, then the remaining letters stretching well to the right.
3. Type a **passphrase**. Any passphrase opens *a* wallet — only yours opens
   *yours*. Recognise your wallet by the fingerprint shown after entry; there
   is deliberately no "wrong passphrase" error.
4. First time: Settings → **CREATE NEW WALLET** — pick a word count, feed the
   camera a detailed scene for entropy, write the words on paper, pass the
   word quiz, set your passphrase.
5. Stay on **TESTNET** (Settings) while you learn — coins: coinfaucet.eu.
6. Pair Sparrow: **Wallet → EXPORT** shows a descriptor QR. Sparrow builds and
   broadcasts transactions; this device verifies and signs PSBTs via camera QR
   or `.psbt` on SD card.

Settings shows exactly what's on the board, bottom-left:
`KISS <version> (<commit>)` plus an honest note while flash encryption is off.

## Desktop simulator & tests

No hardware required:

```sh
sim/build_sim.sh      # headless LVGL sim -> /tmp/fruitsim (renders .ppm frames)
sim/build_test.sh     # crypto/PSBT test suite -> /tmp/kisstest
sim/build_fuzz.sh     # parser fuzz harness (ASAN/UBSAN) -> /tmp/kissfuzz
sim/mk_test_qrs.sh d  # scan-test QR page (static/pMofN/BC-UR + STOP case) -> d/index.html
```

The sim compiles the real `main/` sources against vendored LVGL and scripts
touch input, so UI changes are previewed as PNG frames before any flash.

## Cutting a release (maintainer)

```sh
tools/build_release.sh        # verified release build -> build-release/
tools/make_web_release.sh     # merged image + SHA256SUMS + GPG signature
                              # -> docs/installer/ (refuses a dirty tree,
                              #    self-verifies against the repo public key)
```

Release builds are **reproducible** (no compile timestamp embedded): the same
commit always produces the same bytes. CI rebuilds every push to main in the
same Docker toolchain and publishes the firmware hashes, so anyone can rebuild
locally and compare
([workflow](.github/workflows/reproducible-build.yml)).

## Licensing

Project license: TBD. Vendored third-party components keep their own licenses:
[libwally-core](components/libwally-core) (MIT/BSD), Espressif components
(Apache-2.0), CC0 game art under `assets/`. No copyrighted or watermarked
assets are used anywhere.

**Use at your own risk. This is experimental firmware; do not trust it with
meaningful funds.**
