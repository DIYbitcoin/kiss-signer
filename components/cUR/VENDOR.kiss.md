# Vendored: odudex/cUR

- Upstream: https://github.com/odudex/cUR
- Pinned commit: `c5f69aa9bc3219542704a0dbaa844ce2b85c7ca9` (the exact SHA Kern pins as its
  `components/cUR` submodule, checked 2026-07-04).
- License: BSD-2-Clause-Patent (see LICENSE).
- Local changes: **CMakeLists.txt only** — build the bundled `src/sha256/sha256.c` instead of
  requiring mbedtls + Kern's `mbedtls_compat` shim, so identical sources compile on desktop
  (sim/build_test.sh) and device. No source files edited.
- `uUR.c` / `micropython.mk` are upstream's MicroPython binding — unused here, kept for fidelity.
- `.git`, `.github/` stripped.
