#!/bin/bash
# Build the browser simulator -> docs/sim/kiss-sim.{js,wasm}   (needs emsdk)
#
# Same sources as sim/build_simapp.sh, same real crypto, through emcc instead of
# clang. The point of the browser build is that trying the signer costs nothing:
# no toolchain, no download, no executable to trust. Krux ships a simulator you
# have to install Python and poetry for; this one is a link.
#
# /dev/urandom is real here -- emscripten backs it with crypto.getRandomValues --
# so main/kiss_crypto.c's host branch gets browser entropy, not a fixed stream.
set -e
cd "$(dirname "$0")/.."
LVGL=managed_components/lvgl__lvgl
WALLY=components/libwally-core
SRCS=$(find "$LVGL/src" -name '*.c' \
  ! -path '*/drivers/*' \
  ! -path '*/libs/freetype/*' ! -path '*/libs/ffmpeg/*' ! -path '*/libs/rlottie/*' \
  ! -path '*test*' ! -path '*demos*' ! -path '*examples*')
VER=$(head -1 VERSION)
mkdir -p docs/sim
emcc -O2 -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations \
  -DSIMULATOR -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -DKISS_VERSION_STR="\"$VER\"" \
  -I"$LVGL" -Isim -Imain -Icomponents/cUR/src \
  -I"$WALLY" -I"$WALLY/upstream" -I"$WALLY/upstream/include" \
  -I"$WALLY/upstream/src" -I"$WALLY/upstream/src/ccan" \
  -I"$WALLY/upstream/src/secp256k1" -I"$WALLY/upstream/src/secp256k1/src" \
  -I"$WALLY/upstream/src/secp256k1/include" \
  $SRCS \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  main/main.c main/sprites.c main/menu_img.c main/menu_logo.c main/gameover_img.c main/game_bg.c main/kiss_img.c main/kiss_art.c main/kiss_art_rle.c main/kiss_ui.c main/kiss_theme.c main/kiss_info.c main/kiss_recv.c main/kiss_sign.c main/kiss_scan.c main/kiss_settings.c main/kiss_fw.c main/kiss_fw_ui.c main/kiss_setup.c main/kiss_duress.c main/kiss_gword.c main/kiss_coverword.c main/kiss_duress_ui.c main/kiss_word_ui.c main/kiss_usage.c main/kiss_backup.c main/kiss_sp.c main/i18n.c main/i18n_tables.c main/font_kiss_*.c main/flag_imgs.c main/qr_transport.c main/platform_sd.c main/kiss_kef.c main/verify_page.c main/kiss_dice_q.c main/kiss_rngq.c main/kiss_rngaudit.c main/kiss_cards_q.c main/kiss_rehearse.c \
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_seed.c main/kiss_seed_sd.c main/kiss_kef_crypto.c main/kiss_tapent.c main/kiss_dice.c main/kiss_lastword.c main/kiss_proof.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  sim/simapp.c \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,HEAPU8 \
  -sMODULARIZE=1 -sEXPORT_NAME=KissSim -sENVIRONMENT=web \
  -lm -o docs/sim/kiss-sim.js
ls -l docs/sim/kiss-sim.js docs/sim/kiss-sim.wasm
