# Vendored: odudex/k_quirc

- Upstream: https://github.com/odudex/k_quirc
- Pinned commit: `06549efae32a4378216b868b2fc2e93cbcdd9707` (the exact SHA Kern pins as its
  `components/k_quirc` submodule, checked 2026-07-04).
- License: MIT (quirc by Daniel Beer + OpenMV + Kern modifications; see LICENSE).
- Local changes: see UPSTREAM-REPORT.md for the two that belong upstream. In full:
  1. `src/k_quirc.c` `k_quirc_decode()` — corner coordinates are
  copied into the result BEFORE the decode-error check (upstream only copies them on
  success). Corners come from `quirc_extract_internal` and are valid for any located grid;
     the scan UI uses them to warn when the QR is clipped by the frame edge.
  2. `k_quirc_decode.c` — `alpha_map` was indexed with an 11-bit and a 6-bit value
     against a 45-entry alphabet, reading up to 17 bytes past the literal into the
     decoded payload. Bounded via `k_quirc_alpha_char()`, out-of-alphabet values
     rejected as `DATA_ECC`. **Upstream has this.**
  3. `k_quirc_version.c` — version 25 ECC-Q had `ns = 3`, which does not tile its
     1588 `data_bytes`, so v25-Q could never decode. 7 is the only value that does.
     **Upstream has this.**
  4. `k_quirc_identify.c` — the four Otsu histograms (4096 bytes) moved off the stack;
     the camera task has 6144. And the alignment spiral is capped at the image
     diagonal: its bound came from attacker geometry with no yield in the loop, which
     on FreeRTOS is a watchdog reset.
  5. `k_quirc_internal.h` — `xylf_t` moved out of `k_quirc_identify.c` so the flood
     fill allocation can say `sizeof(xylf_t)` instead of a bare `* 8`.

  `test/` (desktop validation harness + 292KB stb_image.h), `.git`, `.github/` stripped.
- Device component, but **kisstest compiles it now** (`sim/build_test.sh`, with the
  `esp_log.h` / FreeRTOS shims in `sim/shims/`). It was reachable by no test on any
  lane, which is most of why 2 and 3 above sat there. The camera itself is still a
  device-only path that nothing here can exercise.
