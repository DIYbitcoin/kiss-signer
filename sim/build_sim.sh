#!/bin/bash
# Build the headless desktop simulator with clang (no cmake/SDL needed).
set -e
cd "$(dirname "$0")/.."

. sim/sim_tmp.sh

LVGL=managed_components/lvgl__lvgl
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
VER=$(head -1 VERSION)

# KISS_SAN=1 builds the same walk under ASAN + UBSAN, the flags sim/build_fuzz.sh
# already uses. It is opt-in because it is a second full clang pass over 463
# LVGL sources with no object cache.
#
# What it can see, stated so nobody reads a clean run as more than it is:
# sim/lv_conf.h sets LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN over a 128 KB
# static pool, so ASAN puts no redzone on a single LVGL object -- no LVGL heap
# overflow and no use-after-free of a widget is visible here. What it does buy
# is the UI files' own stack buffers and static arrays, which are wall to wall
# char buf[N] + snprintf/memcpy, plus UBSAN over the layout arithmetic. The
# fuzz harness covers the parsers; this covers the screens.
SAN=""
#
# TWO UBSAN checks are off, and both of them only ever fired inside LVGL.
#
#   function          LVGL stores its mask callbacks as one generic pointer
#                     type and calls them through it (lv_draw_sw_mask.c:102 ->
#                     lv_draw_mask_line at :385). A real C rule, a real LVGL
#                     habit, and not a bug this repo can fix.
#   pointer-overflow  lv_style.h:543 applies a zero offset to a null pointer,
#                     which is undefined and is also what half of C does.
#
# Both fire on STYLE paths, so they arrived the day the slide's track, fill and
# knob got rounded corners -- a lane that had been green went red on findings
# in a vendored file, one per push, which is a gate nobody will read twice.
#
# Everything else stays: ASAN, and the rest of UBSAN. What this lane exists for
# is the UI files' own stack buffers and static arrays, and none of that is
# given up here. If a THIRD of these turns up, suppress it here with its file
# and line rather than reaching for -fsanitize=undefined's off switch.
if [ -n "$KISS_SAN" ]; then
  SAN="-fsanitize=address,undefined -fno-sanitize=function,pointer-overflow"
  SAN="$SAN -fno-sanitize-recover=undefined -g"
fi

clang $SAN ${KISS_WERROR:+-Werror} -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations -DSIMULATOR -DKISS_NO_WALLY -DKISS_SIM_WALK -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -DPQ_SHA256_COMPRESS_HOOK=1 -Isim -Imain -Icomponents/cUR/src -Icomponents/slhdsa -Icomponents/slhdsa/upstream \
  $SRCS main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/game_bg.c main/kiss_img.c main/kiss_art.c main/kiss_art_rle.c main/kiss_ui.c main/kiss_theme.c main/kiss_terms.c main/kiss_info.c main/kiss_recv.c main/kiss_sign.c main/kiss_scan.c main/kiss_settings.c main/kiss_fw.c main/kiss_pqsig.c components/slhdsa/pq_hw_sha.c components/slhdsa/upstream/slh_dsa.c components/slhdsa/upstream/slh_sha2.c components/slhdsa/upstream/sha2_256.c components/slhdsa/upstream/sha2_512.c main/kiss_fw_ui.c main/kiss_setup.c main/kiss_duress.c main/kiss_gword.c main/kiss_coverword.c main/kiss_duress_ui.c main/kiss_word_ui.c main/kiss_usage.c main/kiss_backup.c main/kiss_sp.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c main/flag_imgs.c main/qr_transport.c main/platform_sd.c main/kiss_kef.c main/kiss_dice_q.c main/kiss_rngq.c main/kiss_rngaudit.c main/kiss_cards_q.c main/kiss_rehearse.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/sim_main.c \
  -lm -o "$KISS_SIM_TMP/fruitsim"
echo "built $KISS_SIM_TMP/fruitsim"
