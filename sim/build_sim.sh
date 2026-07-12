#!/bin/bash
# Build the headless desktop simulator with clang (no cmake/SDL needed).
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
VER=$(head -1 VERSION)
clang -O1 -w -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -Isim -Imain -Icomponents/cUR/src \
  $SRCS main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/wallet_img.c main/tile_lbls.c main/wallet_ui.c main/wallet_recv.c main/wallet_sign.c main/wallet_scan.c main/wallet_settings.c main/wallet_setup.c main/qr_transport.c main/platform_sd.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/sim_main.c \
  -lm -o /tmp/fruitsim
echo "built /tmp/fruitsim"
