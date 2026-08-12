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
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations -DSIMULATOR -DOVERLAPCHECK -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -Isim -Imain -Icomponents/cUR/src \
  $SRCS main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/game_bg.c main/kiss_img.c main/kiss_art.c main/kiss_art_rle.c main/kiss_ui.c main/kiss_theme.c main/kiss_info.c main/kiss_recv.c main/kiss_sign.c main/kiss_scan.c main/kiss_settings.c main/kiss_fw.c main/kiss_fw_ui.c main/kiss_setup.c main/kiss_duress.c main/kiss_gword.c main/kiss_coverword.c main/kiss_duress_ui.c main/kiss_word_ui.c main/kiss_usage.c main/kiss_backup.c main/kiss_sp.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c main/flag_imgs.c main/qr_transport.c main/platform_sd.c main/verify_page.c main/kiss_dice_q.c main/kiss_cards_q.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/sim_main.c sim/overlapcheck.c \
  -lm -o /tmp/kissoverlap
echo "built /tmp/kissoverlap"
