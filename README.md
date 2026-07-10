# kiss-wallet

An airgapped single-sig Bitcoin signer hidden behind a fruit-slash arcade game.
The device looks and plays like **FRUIT ISLAND**; a secret unlock gesture opens
**KISS Wallet** — an offline signer in the family of SeedSigner, Specter-DIY,
Krux, and Kern.

> Keep it small. Make it safe. Make it clear.

- **Seed + passphrase = your wallet.** The BIP39 passphrase is typed fresh every
  time, never stored, and there is no "wrong passphrase" error by design — a
  different passphrase simply opens a different wallet (deniability built in).
- **Airgapped:** transactions move by animated QR (BC-UR) or SD card. The
  device never touches a network.
- **Online wallet compatible:** exports descriptors for Sparrow and friends. The online wallet builds and broadcasts transactions; KISS stays offline, verifies the PSBT, and signs only what it can fully show.

## Hardware

Guition **JC4880P443C** dev board — ESP32-P4 (RISC-V), 480×800 MIPI-DSI touch
panel (ST7701 + GT911), OV02C10 MIPI-CSI camera, SDMMC card slot.

## Flash it (first time)

You need [Docker](https://docs.docker.com/get-docker/) to build with ESP-IDF v6.0.1. For terminal flashing, use a local ESP-IDF install if you have one. Direct esptool stays as a fallback because ESP-IDF uses esptool under the hood.

```sh
git clone <this repo> && cd kiss-wallet

# 1. build the release-candidate firmware — not flash-encrypted yet
#    (first run pulls the ESP-IDF v6.0.1 image)
tools/build_release.sh

# 2. plug the board in over USB-C and find its port
ls /dev/cu.usbmodem* 2>/dev/null || ls /dev/ttyACM*   # macOS | Linux

# 3. flash with ESP-IDF (local ESP-IDF install)
idf.py -B build-release -p <port> flash

# fallback: direct esptool
uvx esptool --chip esp32p4 -p <port> -b 460800 \
  --before default-reset --after no-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000  build-release/bootloader/bootloader.bin \
  0x8000  build-release/partition_table/partition-table.bin \
  0x10000 build-release/guition_kiss_bringup.bin
```

**4. Unplug the board, wait ~3 seconds, plug it back in.** Required: the board
only starts new firmware from a real power-on, never from a USB reset (v1.3
engineering-sample quirk). A black screen after flashing means you skipped this.

The build script ends by verifying the binary (no development seed material
inside, version + commit string present) and re-prints these flash commands.

Reflashing later: use `idf.py -B build-release -p <port> app-flash` with local ESP-IDF, or the app-only esptool fallback printed by `tools/build_release.sh`.

For development builds instead (boot log fingerprint, dev banner):

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B build-disp build
```

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
6. Pair Sparrow: **Wallet -> EXPORT** shows a descriptor QR. Sparrow builds and broadcasts transactions; this device verifies and signs PSBTs via camera QR or `.psbt` on SD card.

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

## Release builds

```sh
tools/build_release.sh   # -> build-release/, verified: no dev seed material,
                         #    quiet logs, version from ./VERSION
```

Flash encryption is the final hardening pass (see the script header) and is
not part of the default release profile yet.

## Licensing

Project license: TBD. Vendored third-party components keep their own licenses:
[libwally-core](components/libwally-core) (MIT/BSD), Espressif components
(Apache-2.0), CC0 game art under `assets/`. No copyrighted or watermarked
assets are used anywhere.

**Use at your own risk. This is experimental firmware; do not trust it with
meaningful funds.**
