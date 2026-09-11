# Third-party notices

KISS Signer's own source and documentation are licensed under the MIT License
(see [`LICENSE`](LICENSE)). The firmware image also statically incorporates
the third-party software and assets listed below, each under its own license.
Full license texts live next to each component in the tree (paths given).

This file is shipped alongside the firmware in release artifacts so a binary
recipient receives the required attributions and license texts.

## Software components (vendored, in-tree)

| Component | Role | License | License text |
|-----------|------|---------|--------------|
| libwally-core | Bitcoin key/PSBT/crypto | MIT | [`components/libwally-core/upstream/LICENSE`](components/libwally-core/upstream/LICENSE) |
| secp256k1 / secp256k1-zkp | EC arithmetic (bundled in libwally) | MIT | [`components/libwally-core/upstream/src/secp256k1/COPYING`](components/libwally-core/upstream/src/secp256k1/COPYING) |
| ctaes | constant-time AES (bundled in libwally) | MIT | [`components/libwally-core/upstream/src/ctaes/COPYING`](components/libwally-core/upstream/src/ctaes/COPYING) |
| ccan modules | helper routines (bundled in libwally) | CC0 / BSD-MIT | [`components/libwally-core/upstream/src/ccan/licenses/`](components/libwally-core/upstream/src/ccan/licenses) |
| cUR | Uniform Resources (UR) codec | BSD-2-Clause-Plus-Patent | [`components/cUR/LICENSE`](components/cUR/LICENSE) |
| k_quirc (quirc) | QR decoding | MIT | [`components/k_quirc/LICENSE`](components/k_quirc/LICENSE) |
| slhdsa-c | SLH-DSA (FIPS 205), post quantum firmware signature | Apache-2.0 OR ISC OR MIT | [`components/slhdsa/upstream/LICENSE`](components/slhdsa/upstream/LICENSE) |
| esp_cam_sensor | camera sensor driver | Apache-2.0 | [`components/esp_cam_sensor/LICENSE`](components/esp_cam_sensor/LICENSE) |

## Software components (fetched at build, not in this tree)

Pulled by the ESP-IDF component manager into `managed_components/` from
`dependencies.lock`; each keeps its upstream license.

| Component | Role | License |
|-----------|------|---------|
| ESP-IDF and Espressif `esp_lcd_*`, `esp_lcd_touch*`, `esp_lvgl_port`, `esp_video`, `esp_sccb_intf`, `esp_h264`, `usb`, `cmake_utilities`, `esp_ipa` | SoC framework + display/touch/camera drivers | Apache-2.0 |
| LVGL | UI toolkit | MIT |

## Fonts

| Font | Use | License |
|------|-----|---------|
| Montserrat | Latin UI + game text (via LVGL built-in) | SIL OFL 1.1 |
| Font Awesome | UI glyph symbols (via LVGL built-in) | Icons CC-BY 4.0, font SIL OFL 1.1 |
| Source Han Sans (JP/KR/SC) | CJK UI text | SIL OFL 1.1 ([`tools/fonts/vendor/LICENSE-SourceHanSans.txt`](tools/fonts/vendor/LICENSE-SourceHanSans.txt)) |
| Ioskeley Mono | fixed pitch firmware faces: amounts, addresses, fingerprints | SIL OFL 1.1, an Iosevka configuration by ahatem ([`tools/fonts/vendor/LICENSE-IoskeleyMono.txt`](tools/fonts/vendor/LICENSE-IoskeleyMono.txt)) |

## Image assets (`assets/`)

| Asset | Use | License |
|-------|-----|---------|
| Twemoji kiss mark (`assets/twemoji/1f48b.*`) and country flags (`assets/twemoji/flags/`) | brand mark + language flags | CC-BY 4.0 (Twitter / Twemoji), see [`assets/twemoji/README.md`](assets/twemoji/README.md) |
| Game fruit art (`assets/emoji/`) | decoy-game sprites | MIT, Microsoft Fluent Emoji (3D), verified byte-for-byte identical to upstream. License text at [`assets/emoji/LICENSE-FluentEmoji.txt`](assets/emoji/LICENSE-FluentEmoji.txt). |
| OpenGameArt "Sprites Fruits" (`assets/fruit-pack/`) | CC0 fallback art, **not referenced at runtime** | CC0 1.0 |

## Documentation tooling (not in the firmware)

| Component | Role | License |
|-----------|------|---------|
| esp-web-tools (`docs/installer/vendor/`) | browser flasher in the docs site | Apache-2.0 |
| Ioskeley Mono (`docs/fonts/IoskeleyMono-*.woff2`) | the docs site typeface, self-hosted | SIL OFL 1.1, an Iosevka configuration by ahatem. License text at [`docs/fonts/LICENSE-IoskeleyMono.txt`](docs/fonts/LICENSE-IoskeleyMono.txt) |
| Iosevka | upstream of Ioskeley Mono | SIL OFL 1.1, Belleve Invis |
| IBM Plex Mono (`docs/fonts/IBMPlexMono-*.woff2`) | docs site fallback face, self-hosted | SIL OFL 1.1, IBM. License text at [`docs/fonts/LICENSE-IBMPlexMono.txt`](docs/fonts/LICENSE-IBMPlexMono.txt) |
