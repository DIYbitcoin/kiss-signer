#!/usr/bin/env bash
# Production LVGL QR raster -> production k_quirc -> cUR exact payload check.
# Default fixtures are deterministic transport bytes; --file adds a signed PSBT.
# KISS_BOARD selects normal simulator geometry; KISS_QR_RENDER_SANITIZE=1 adds
# ASan/UBSan. No device, wallet, camera or network is needed.
set -euo pipefail
cd "$(dirname "$0")/.."
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/kiss-qr-render.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT
export KISS_SIM_TMP="$SCRATCH" KISS_SIM_KEEP=1
. sim/sim_tmp.sh
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
SANITIZE=""
if [[ ${KISS_QR_RENDER_SANITIZE:-0} == 1 ]]; then
  SANITIZE="-g -fsanitize=address,undefined -fno-omit-frame-pointer"
fi
# Word splitting of the simulator source lists/board flags is intentional.
clang -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-implicit-const-int-float-conversion \
  -Wno-missing-field-initializers -Wno-deprecated-declarations $SANITIZE \
  $KISS_BOARD_CFLAGS -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -DK_QUIRC_ADAPTIVE_THRESHOLD -DK_QUIRC_BILINEAR_THRESHOLD \
  -I"$LVGL" -Isim -Isim/shims -Imain -Icomponents/cUR/src \
  -Icomponents/k_quirc/include -Icomponents/k_quirc/src \
  $SRCS main/kiss_theme.c main/kiss_terms.c main/i18n.c main/i18n_tables.c $KISS_FONT_SRCS \
  main/qr_transport.c components/cUR/src/*.c components/cUR/src/types/*.c \
  components/cUR/src/sha256/sha256.c components/k_quirc/src/*.c sim/test_qr_render.c \
  -lm -o "$SCRATCH/test_qr_render"
"$SCRATCH/test_qr_render" "$@"
