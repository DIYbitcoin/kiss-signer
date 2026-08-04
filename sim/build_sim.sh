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
clang -O1 -w -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -Isim -Imain -Icomponents/cUR/src \
  $SRCS main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/game_bg.c main/wallet_img.c main/wallet_ui.c main/wallet_theme.c main/wallet_info.c main/wallet_recv.c main/wallet_sign.c main/wallet_scan.c main/wallet_settings.c main/wallet_setup.c main/wallet_duress.c main/wallet_duress_ui.c main/wallet_usage.c main/wallet_backup.c main/wallet_sp.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c main/flag_imgs.c main/qr_transport.c main/platform_sd.c main/wallet_dice_q.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/sim_main.c \
  -lm -o /tmp/fruitsim
echo "built /tmp/fruitsim"
