#!/bin/bash
# Build the explainer fit report (see sim/fitcheck.c).
set -e
cd "$(dirname "$0")/.."

# Where this build's binary goes. KISS_SIM_TMP is the same root the fake card,
# the seed files and the captured frames use (main/kiss_simpath.h) -- unset it
# is /tmp, exactly as before. It is here as well as in the C because two people
# building at once wrote each other's binary, and the loser then ran a walk over
# somebody else's code and reported findings about it.
KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$KISS_SIM_TMP"

LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I"$LVGL" -Isim -Imain \
  $SRCS main/kiss_theme.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c \
  sim/fitcheck.c \
  -lm -o "$KISS_SIM_TMP/kissfit"
echo "built $KISS_SIM_TMP/kissfit"
