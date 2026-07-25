#!/bin/bash
# Build the explainer fit report (see sim/fitcheck.c).
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
clang -O1 -w -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I"$LVGL" -Isim -Imain \
  $SRCS main/wallet_theme.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c \
  sim/fitcheck.c \
  -lm -o /tmp/kissfit
echo "built /tmp/kissfit"
