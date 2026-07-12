# kiss-wallet — project notes

A **decoy fruit-slash game** ("FRUIT ISLAND", Fruit-Ninja style) that will eventually hide a
KISS/Krux Bitcoin wallet behind a secret unlock gesture. Make the game look **10/10 professional**,
never childish. Runs on a Guition ESP32-P4 dev board.

## Hardware
- Board: **Guition JC4880P443C**, ESP32-P4 (RISC-V), **v1.3 engineering sample**.
- Display: **480×800 MIPI-DSI ST7701** panel. **Has top/right overscan (~30–50px)** — keep HUD/content
  inset from edges or it gets clipped.
- Touch: **GT911** over I2C (GPIO 7 SDA / 8 SCL).
- Camera: **OV02C10** over MIPI-CSI (NOT the SC2336 the wallet spec assumed) — SCCB addr 0x36 on
  the shared touch I2C bus, RAW10 1920×1080 2-lane, no reset/pwdn pins. Driver: Espressif-authored
  (Apache-2.0) but absent from the upstream registry; vendored from Guition's board demo into
  `components/esp_cam_sensor` (registry 2.3.0 + `sensors/ov02c10/`, reviewed line-by-line
  2026-07-02, wired via `override_path`). Board docs/demos: `pan.jczn1688.com` download center
  (sharded zip; see JC4880P443C_I_W.zip manifest API).
- Radio: the board carries an **ESP32-C6** WiFi6/BT coprocessor (SDIO on GPIO14-19, **reset on
  GPIO54**). KISS never uses it: `radio_hold_in_reset()` in `main/main.c` drives GPIO54 low +
  `gpio_hold_en` as the first thing in `app_main`; Settings/home show `radio: held in reset` read
  back from the pad; release scripts fail if any radio/network lib links (linker-map check).
- Serial port: `/dev/cu.usbmodem1201`.

## Build & flash
- ESP-IDF **v6.0.1 via Docker** (no local IDF):
  ```
  docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0.1 idf.py -B build-disp build
  ```
- **v1.3 engineering-sample boot quirk:** crashes in ROM SHA on USB soft-reset (`rst:0x17`); only boots
  on a real power-on. So flash with **`--after no-reset`**, then have the user **physically unplug →
  ~3s → replug**. Never tell the user a build is testable without this power-cycle.
- App partition is **6MB** (`factory` @ `0x10000`, size `0x600000` in `partitions.csv`).

## Desktop simulator (test rendering without the board)
- `sim/` compiles the **real** `main/main.c` (with `-DSIMULATOR`) against vendored LVGL, renders into an
  in-memory RGB565 framebuffer, scripts touch, dumps `.ppm` frames. Build: `sim/build_sim.sh` → `/tmp/fruitsim`.
- Platform seam: device implements `platform_read_touch()` against GT911; sim feeds scripted input.
  ESP-only code in `main.c` is wrapped in `#ifndef SIMULATOR`.
- **Sim CAN show:** layout, sprite art, animation logic, menu/game-over screens.
- **Sim CANNOT show:** real lag/perf (desktop too fast), overscan clipping, true panel color, boot.
  Those are **device-only** checks.

## File map
- `main/main.c` — the whole game (HUD, slicing, physics, menu/game-over, effects). Large file.
- `main/wallet_crypto.c/.h` — KISS wallet crypto layer (libwally). `wallet_selftest()` = BIP39 test
  vector ("abandon…about" → fingerprint `73C5DA0A`), logged at boot on device.
  `wallet_fingerprint(passphrase)` = dev seed + passphrase → master fingerprint.
- `main/wallet_ui.c/.h` — login flow (KISS gesture → QWERTY passphrase → fingerprint reveal →
  home). Compiled in BOTH device and sim builds; sim stubs `wallet_fingerprint` in sim_main.c.
  Registers the LVGL pointer indev; game ignores touch while `wallet_ui_active()`.
- `main/camera_spike.c/.h` — step-2 camera proof (OV02C10→ISP→PPA→direct framebuffer flip,
  30fps tear-free). Opened from the Sign tile; becomes the QR scanner in step 6.
- `components/libwally-core/` — vendored libwally **1.5.4** (commit `c5591834`, the exact pin both
  Jade and Kern use). Component CMake modeled on theirs, but **internal ccan SHA instead of mbedtls**
  so the identical sources build on desktop too. English-only wordlist (`BUILD_MINIMAL`).
  Do not hand-edit `upstream/`.
- `main/sprites.c/.h` — **generated** fruit/bomb/heart sprites (RGB565A8). Do not hand-edit.
- `main/menu_img.c/.h` — **generated** baked 480×800 menu artwork (RGB565). Do not hand-edit.
- `main/gameover_img.c/.h` — **generated** baked game-over artwork + NEW BEST ribbon. Do not hand-edit.
- `assets/` — source art + the generators that emit the three files above. See `assets/README.md`.
- `sim/` — desktop simulator harness, `lv_conf.h`, build script.
- `sim/build_test.sh` → `/tmp/kisstest` — desktop crypto test runner (wallet_crypto + vendored
  libwally, no LVGL). Must print `PASS: BIP39 test vector -> 73C5DA0A`. Run after any crypto change.

To change any baked/sprite art, edit the matching `assets/generators/*.py`, re-run it (writes into
`main/` directly), eyeball the `/tmp/*_mock.png` preview, then rebuild.

## Conventions / constraints
- **Asset licensing: CC0 / CC-BY / MIT only.** The approved fruit art is the CC0 OpenGameArt "Sprites
  Fruits" pack. **Never use Adobe Stock or any watermarked/copyrighted image** — even as a "reference"
  the user pastes. Refuse and say so.
- **Baked-artwork pattern:** full-screen static scenes (menu, game-over) are rendered in PIL → converted
  to one RGB565 LVGL image = zero per-frame cost. Only dynamic numbers (score/best) are live LVGL labels.
- **No hand-drawn geometric/procedural vector art** for hero elements — it reads as cheap (the crossed-blade
  logo failed). Lean on the photographic CC0 sprites, organic procedural fruit cross-sections, type, opacity.
- **Always preview a generated PNG** (menu_mock.png / gameover_mock.png / sim frames) before flashing.
- **De-fringe** CC0 sprite cutouts (alpha erode + slight blur) to kill colored halos.
- Keep this work **isolated from the user's krux repo.**

## Tooling
- Python venv with image deps: `/tmp/spritevenv/bin/python` (numpy, pillow, esptool).
- Art generators + CC0 source PNGs are vendored under `assets/` (reproducible). The macOS system font
  "Arial Rounded Bold" is a host dependency, not vendored — see `assets/README.md`.

## Working with this user
- Wants **terse, high-signal replies**; is mindful of token/usage limits. **Batch changes** rather than
  one-at-a-time round trips.
- Per global rule: **every PR/flash gets an explicit `DEVICE TEST: REQUIRED` or `NOT REQUIRED` verdict.**
  Display/touch/timing changes are REQUIRED — a clean build proves nothing about look or feel.
