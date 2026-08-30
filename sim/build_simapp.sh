#!/bin/bash
# Build the INTERACTIVE simulator -> /tmp/kissapp   (needs SDL2: brew install sdl2)
#
# This is the third link line in the repo, and the first one that puts the two
# halves together. sim/build_sim.sh compiles the UI and fakes the crypto in
# sim/sim_main.c; sim/build_test.sh compiles the real crypto and no UI. Neither
# is something a person can open. This is both: every screen, over real
# libwally, driven by sim/simapp.c's window instead of a script.
#
# Nothing here may be a fake. The moment the fingerprint on the screen is not
# the fingerprint the device would show for the same words, the simulator stops
# being worth publishing -- a visitor could check it against their own signer
# and be told a lie.
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
WALLY=components/libwally-core
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
VER=$(head -1 VERSION)
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations \
  -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -DPQ_SHA256_COMPRESS_HOOK=1 -Isim -Imain -Icomponents/cUR/src -Icomponents/slhdsa -Icomponents/slhdsa/upstream \
  -I"$WALLY" -I"$WALLY/upstream" -I"$WALLY/upstream/include" \
  -I"$WALLY/upstream/src" -I"$WALLY/upstream/src/ccan" \
  -I"$WALLY/upstream/src/secp256k1" -I"$WALLY/upstream/src/secp256k1/src" \
  -I"$WALLY/upstream/src/secp256k1/include" \
  $(sdl2-config --cflags) \
  $SRCS \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/game_bg.c main/kiss_img.c main/kiss_art.c main/kiss_art_rle.c main/kiss_ui.c main/kiss_theme.c main/kiss_terms.c main/kiss_info.c main/kiss_recv.c main/kiss_sign.c main/kiss_scan.c main/kiss_settings.c main/kiss_fw.c main/kiss_pqsig.c components/slhdsa/pq_hw_sha.c components/slhdsa/upstream/slh_dsa.c components/slhdsa/upstream/slh_sha2.c components/slhdsa/upstream/sha2_256.c components/slhdsa/upstream/sha2_512.c main/kiss_fw_ui.c main/kiss_setup.c main/kiss_duress.c main/kiss_gword.c main/kiss_coverword.c main/kiss_duress_ui.c main/kiss_word_ui.c main/kiss_usage.c main/kiss_backup.c main/kiss_sp.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c main/flag_imgs.c main/qr_transport.c main/platform_sd.c main/kiss_kef.c main/kiss_dice_q.c main/kiss_rngq.c main/kiss_rngaudit.c main/kiss_cards_q.c main/kiss_rehearse.c \
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_payee.c main/kiss_seed.c main/kiss_seed_sd.c main/kiss_kef_crypto.c main/kiss_tapent.c main/kiss_dice.c main/kiss_lastword.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/simapp.c \
  $(sdl2-config --libs) -lm -o "$KISS_SIM_TMP/kissapp"
echo "built $KISS_SIM_TMP/kissapp"
