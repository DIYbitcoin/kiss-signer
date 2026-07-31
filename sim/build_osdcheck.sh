#!/bin/bash
# Build the overlay text gate (see sim/osdcheck.c).
#
# Links the composer, the strip cache, the theme and the fonts, the way
# sim/build_fitcheck.sh does. main/scan_osd.c used to be excluded here on
# purpose; it no longer exists.
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
clang -O1 -w -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I"$LVGL" -Isim -Imain -Icomponents/bitsquiggle32 \
  $SRCS main/osd_text.c main/osd_strips.c main/wallet_theme.c components/bitsquiggle32/bitsquiggle32.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c \
  sim/osdcheck.c \
  -lm -o /tmp/kissosd
echo "built /tmp/kissosd"
