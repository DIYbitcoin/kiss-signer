#!/bin/bash
# Build the desktop ecash signer for mint interop tests -> $KISS_SIM_TMP/ecashcli
# Compiles the SAME vendored libwally amalgamation + config as the device component.
set -e
cd "$(dirname "$0")/.."

. sim/sim_tmp.sh

# The link below globs components/cUR/src/types/*.c and never reads that
# component's CMakeLists.txt, so a source dropped from the FIRMWARE build is
# invisible here. Check the two agree before trusting a green kisstest.
python3 tools/check_cur_link.py --selftest
python3 tools/check_cur_link.py
WALLY=components/libwally-core
# Same single source as the device build and the UI sim: the firmware update
# tests compare a candidate image against the version this build claims to be,
# so a placeholder here would test the placeholder.
VER=$(head -1 VERSION)
clang $KISS_BOARD_CFLAGS ${KISS_WERROR:+-Werror} -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -DUR_ALLOC_FAIL_TEST=1 \
  -DPQ_SHA256_COMPRESS_HOOK=1 \
  -DKISS_SP_TEST_HOOKS=1 \
  -DKISS_ROOT="\"$PWD\"" \
  -DKISS_VERSION_STR="\"$VER\"" \
  -I"$WALLY" \
  -I"$WALLY/upstream" \
  -I"$WALLY/upstream/include" \
  -I"$WALLY/upstream/src" \
  -I"$WALLY/upstream/src/ccan" \
  -I"$WALLY/upstream/src/secp256k1" \
  -I"$WALLY/upstream/src/secp256k1/src" \
  -I"$WALLY/upstream/src/secp256k1/include" \
  -Imain \
  -Isim \
  -Icomponents/cUR/src \
  -Isim/shims \
  -Icomponents/k_quirc/src \
  -Icomponents/slhdsa \
  -Icomponents/slhdsa/upstream \
  -Icomponents/k_quirc/include \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  components/k_quirc/src/*.c \
  components/slhdsa/pq_hw_sha.c components/slhdsa/upstream/slh_dsa.c components/slhdsa/upstream/slh_sha2.c components/slhdsa/upstream/sha2_256.c components/slhdsa/upstream/sha2_512.c \
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_sp.c main/kiss_ecash.c main/kiss_seed.c main/kiss_seed_sd.c main/kiss_kef.c main/kiss_kef_crypto.c main/kiss_pbkdf2.c main/kiss_satocard.c main/kiss_satochip.c main/kiss_seedkeeper.c main/kiss_cardwallet.c main/kiss_ccid.c main/platform_sd.c main/kiss_usage.c main/kiss_payee.c main/kiss_backup.c main/kiss_duress.c main/kiss_gword.c main/kiss_coverword.c main/qr_transport.c main/kiss_tapent.c main/kiss_dice.c main/kiss_dice_q.c main/kiss_rngq.c main/kiss_cards_q.c main/kiss_lastword.c main/kiss_rehearse.c main/kiss_fw.c main/kiss_pqsig.c main/kiss_art_rle.c main/kiss_touch.c \
  sim/ecash_cli.c \
  -lm -o "$KISS_SIM_TMP/ecashcli"
echo "built $KISS_SIM_TMP/ecashcli"
