# kiss-wallet

An airgapped single-sig Bitcoin signer hidden behind a fruit-slash arcade game.
The device looks and plays like **FRUIT ISLAND**; a secret unlock gesture opens
**KISS Wallet**. Inspired by Jade, Specter-DIY, SeedSigner, Krux, Kern, and
Shieldsigner.

> Keep it small. Make it safe. Make it clear.

- **Seed + passphrase = your wallet.** The BIP39 passphrase is typed fresh every
  time, never stored, and there is no "wrong passphrase" error by design — a
  different passphrase simply opens a different wallet (deniability built in).
- **Airgapped by hardware:** transactions move by animated QR (BC-UR) or SD
  card. The ESP32-P4 has no WiFi/Bluetooth silicon and the firmware contains no
  networking stack — there is no radio to accidentally leave on.
- **Online wallet compatible:** exports descriptors for Sparrow and friends.
  The watch-only wallet builds and broadcasts; KISS stays offline, verifies the
  PSBT, and signs only what it can fully show.

**Status: `0.8.0-beta1` — experimental. Do not trust it with meaningful funds.**

## Hardware

Guition **JC4880P443C** dev board — ESP32-P4 (RISC-V), 480×800 MIPI-DSI touch
panel (ST7701 + GT911), OV02C10 MIPI-CSI camera, SDMMC card slot. No soldering.

---

## Install

Three ways, easiest first. All of them end the same way:
**unplug the board, wait ~3 seconds, plug it back in.** The board only starts
new firmware from a real power-on. A black screen after flashing means you
skipped this.

### Option A — web installer (easiest)

Flash from the browser, no toolchain. Use **Chrome, Brave, or Edge** on any
desktop OS. **Safari and Firefox cannot flash** (no Web Serial).

While this repo is private the installer is served locally:

```sh
git clone https://github.com/kkdao/kiss-wallet.git && cd kiss-wallet
python3 -m http.server 8321 -d docs
# open http://localhost:8321 in Chrome / Brave / Edge
```

The page hashes the firmware in your browser before the install button unlocks
and shows the release-key fingerprint. Still verify the GPG signature yourself
(next section).

### Verify the release (before Option A or B)

Release artifacts live in [`docs/installer/`](docs/installer/) (not the GitHub
Releases tab): the merged firmware image (`firmware/kiss-wallet-<version>.bin`,
flashed at offset 0), `SHA256SUMS` + detached GPG signature `SHA256SUMS.asc`,
the release public key `kiss_wallet_pgp.asc`, and `release.json`.

```sh
cd docs/installer
gpg --import kiss_wallet_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect: "Good signature from KISS Wallet releases"
# expect fingerprint: 166A CBF3 7786 FCEA A694  96DE 886F 1BFE B84E F1C0

shasum -a 256 --ignore-missing -c SHA256SUMS     # macOS
sha256sum --ignore-missing -c SHA256SUMS         # Linux
```

Windows: `Get-FileHash firmware\kiss-wallet-<version>.bin` in PowerShell and
compare against `SHA256SUMS` by eye; verify the signature with
[Gpg4win](https://gpg4win.org).

Cross-check the fingerprint from more than one place — it is only as
trustworthy as this README.

### Option B — flash the signed binary

`esptool` works the same on macOS, Linux, and Windows:

```sh
pip install esptool        # or: pipx install esptool / uvx esptool

# port: /dev/cu.usbmodem* (macOS) | /dev/ttyACM* (Linux) | COMx (Windows)
esptool --chip esp32p4 -p <port> -b 460800 \
  --before default-reset --after no-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0 docs/installer/firmware/kiss-wallet-<version>.bin
```

Then power-cycle. The merged image covers the wallet-storage area, so this is
always a clean start.

### Option C — build from source

The strongest verification. Only requirement is
[Docker](https://docs.docker.com/get-docker/); the ESP-IDF v6.0.1 toolchain
runs inside the container. Builds are **reproducible**: the same commit
produces the same bytes, so your hashes must match CI's.

```sh
tools/build_release.sh     # release profile -> build-release/  (macOS/Linux;
                           # Windows: run under WSL or Git Bash)
```

The script verifies its own output (no development seed material in the
binary, version + commit present) and prints the exact flash commands.

Development profile (any OS, adds boot fingerprint + dev banner):

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B build-disp build          # PowerShell: -v "${PWD}:/project"
```

---

## Flash encryption (final-board build) — read twice, flash once

`tools/build_encrypted_release.sh` builds the hardened profile: flash
encryption in **release mode** plus **NVS encryption** (the stored seed words
are covered — plain flash encryption alone would leave the NVS data partition
readable). Secure boot lands later as its own pass.

**What it gives you:** the flash contents, including the stored seed, cannot
be read out of the chip. The AES key is generated on the device, burned into
eFuse, and is never readable by anyone — including you.

**What it costs — permanent:**

- The first boot **burns eFuses. No undo.**
- After that boot the board can **never be reflashed** — no serial, no web
  installer, no OTA. The firmware is frozen forever (which also locks out
  evil-maid reflashing — that is the point).
- First boot encrypts ~6 MB in place and can sit on a black screen for
  minutes. **Do not unplug** until the game menu appears; losing power
  mid-encryption can brick the board.
- Fresh board you intend as your final signer only. Test the normal release
  on it first.

```sh
tools/build_encrypted_release.sh    # builds + 14 safety checks,
                                    # prints the one-time flash command
```

The script never flashes anything itself. Settings shows an amber "flash not
yet encrypted" line read live from eFuse — after the encrypted first boot it
disappears, and only then create a wallet you care about.

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
   broadcasts; the device verifies and signs PSBTs via camera QR or `.psbt`
   on SD card.

Settings shows exactly what's on the board, bottom-left:
`KISS <version> (<commit>)` plus an honest note while flash encryption is off.

## Desktop simulator & tests

No hardware required:

```sh
sim/build_sim.sh      # headless LVGL sim -> /tmp/fruitsim (renders .ppm frames)
sim/build_test.sh     # crypto/PSBT test suite -> /tmp/kisstest
sim/build_fuzz.sh     # parser fuzz harness (ASAN/UBSAN) -> /tmp/kissfuzz
sim/mk_test_qrs.sh d  # scan-test QR page (static/pMofN/BC-UR + STOP case)
```

The sim compiles the real `main/` sources against vendored LVGL and scripts
touch input, so UI changes are previewed as frames before any flash.

## Cutting a release (maintainer)

```sh
tools/build_release.sh        # verified release build -> build-release/
tools/make_web_release.sh     # merged image + SHA256SUMS + GPG signature
                              # -> docs/installer/ (refuses a dirty tree,
                              #    self-verifies against the repo public key)
```

CI rebuilds every push to main in the same Docker toolchain and publishes the
firmware hashes ([workflow](.github/workflows/reproducible-build.yml)).

## Licensing

Project license: TBD. Vendored third-party components keep their own licenses:
[libwally-core](components/libwally-core) (MIT/BSD), Espressif components
(Apache-2.0), CC0 game art under `assets/`. No copyrighted or watermarked
assets are used anywhere.

**Use at your own risk. This is experimental firmware; do not trust it with
meaningful funds.**
