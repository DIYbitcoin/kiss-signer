# Vendored: odudex/k_quirc

- Upstream: https://github.com/odudex/k_quirc
- Pinned commit: `73524ff03b8bcd1684ff1d404891ebb38f9087eb` (the exact SHA Kern pins as its
  `components/k_quirc` submodule, checked 2026-09-28). Krux moves to the same code in
  selfcustody/MaixPy#60.
- License: MIT (quirc by Daniel Beer + OpenMV + Kern modifications; see LICENSE).
- Local changes: **none**. Every patch this tree used to carry is upstream now, in its own
  form: corners reported on a failed decode, the alphanumeric map bounded (a value outside
  the 45 character alphabet is `K_QUIRC_ERROR_INVALID_SYMBOL`), version 25-Q's block
  layout, the alignment search bounded by the grid size, the flood fill stack sized by
  `sizeof(xylf_t)`, and the decoded payload, datastream, module grid and both image
  buffers zeroed through a store the compiler cannot drop on destroy and on resize.
- `test/`, `.git`, `.github/` stripped. `src/k_quirc_pie.S` is the ESP32-P4 vector
  binariser; CMakeLists.txt builds it for that target only, and the desktop builds glob
  `src/*.c`, so they never see it.
- Stack: the new thresholder keeps its histograms on the stack, where this tree used to
  make them static. Measured with `-fstack-usage`, the deepest frame went from about 2 KB
  to about 3 KB. The decode runs on the camera task (main/camera_spike.c), whose stack
  was raised from 6144 to 8192 bytes for it. Kern runs the same decoder on a 32 KB task.
- Kisstest compiles it (`sim/build_test.sh`, with the `esp_log.h` / FreeRTOS shims in
  `sim/shims/`); the camera itself is a device-only path nothing here can exercise.
