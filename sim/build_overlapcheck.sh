#!/bin/bash
# Build the text overlap gate (see sim/overlapcheck.c).
#
# Same link line as sim/build_sim.sh, because the gate walks real wallet
# screens and needs the whole UI to build them. It reuses sim/sim_main.c's
# scripted walk too: -DOVERLAPCHECK turns every save() in that walk into a
# checkpoint and suppresses the .ppm writes, so the gate and the screenshots
# can never disagree about which screens exist.
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
VER=$(head -1 VERSION)
clang -O1 -w -DSIMULATOR -DOVERLAPCHECK -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -Isim -Imain -Icomponents/cUR/src \
  $SRCS main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/game_bg.c main/wallet_img.c main/wallet_ui.c main/wallet_theme.c main/wallet_info.c main/wallet_recv.c main/wallet_sign.c main/wallet_scan.c main/wallet_settings.c main/wallet_setup.c main/wallet_duress.c main/wallet_duress_ui.c main/wallet_usage.c main/wallet_backup.c main/wallet_sp.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c main/flag_imgs.c main/qr_transport.c main/platform_sd.c main/wallet_dice_q.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/sim_main.c sim/overlapcheck.c \
  -lm -o /tmp/kissoverlap
echo "built /tmp/kissoverlap"
