#!/bin/bash
# Build the theme safety gate (see sim/themecheck.c).
#
# Only needs the theme and the fonts it pulls in, not the screens: what it
# checks is the colour table itself.
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I"$LVGL" -Isim -Imain \
  $SRCS main/kiss_theme.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c \
  sim/themecheck.c \
  -lm -o /tmp/kisstheme
echo "built /tmp/kisstheme"
