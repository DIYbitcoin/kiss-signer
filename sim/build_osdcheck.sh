#!/bin/bash
# Build the overlay text gate (see sim/osdcheck.c).
#
# Links the composer, the theme and the fonts, the way sim/build_fitcheck.sh
# does. It deliberately does NOT link main/scan_osd.c: the gate is about text
# composed at runtime, and pulling in 5.6MB of baked strips to borrow one
# struct definition would be absurd. scan_osd.h is declarations only, so the
# type comes for free and the baked data stays out.
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
clang -O1 -w -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I"$LVGL" -Isim -Imain \
  $SRCS main/osd_text.c main/wallet_theme.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c \
  sim/osdcheck.c \
  -lm -o /tmp/kissosd
echo "built /tmp/kissosd"
