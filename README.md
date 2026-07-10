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
- **Coordinator-friendly:** exports a watch-only output descriptor per address
  type (native / nested / legacy segwit) for Sparrow and friends; verifies and
  signs PSBTs with full output display, change re-derivation, network + script
  type checks, and hard STOPs on anything it cannot verify.

## Hardware

Guition **JC4880P443C** dev board — ESP32-P4 (RISC-V), 480×800 MIPI-DSI touch
panel (ST7701 + GT911), OV02C10 MIPI-CSI camera, SDMMC card slot.

## Build (device)

ESP-IDF v6.0.1 via Docker — no local IDF needed:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B build-disp build
```

Flash with `--after no-reset`, then physically power-cycle the board (v1.3
engineering samples do not survive a USB soft-reset).

## Desktop simulator & tests

No hardware required:

```sh
sim/build_sim.sh      # headless LVGL sim -> /tmp/fruitsim (renders .ppm frames)
sim/build_test.sh     # crypto/PSBT test suite -> /tmp/kisstest
sim/build_fuzz.sh     # parser fuzz harness (ASAN/UBSAN) -> /tmp/kissfuzz
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
