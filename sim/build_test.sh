#!/bin/bash
# Build the desktop crypto test runner -> /tmp/kisstest
# Compiles the SAME vendored libwally amalgamation + config as the device component.
set -e
cd "$(dirname "$0")/.."

# Where this build's binary goes. KISS_SIM_TMP is the same root the fake card,
# the seed files and the captured frames use (main/kiss_simpath.h) -- unset it
# is /tmp, exactly as before. It is here as well as in the C because two people
# building at once wrote each other's binary, and the loser then ran a walk over
# somebody else's code and reported findings about it.
KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$KISS_SIM_TMP"

# The link below globs components/cUR/src/types/*.c and never reads that
# component's CMakeLists.txt, so a source dropped from the FIRMWARE build is
# invisible here. Check the two agree before trusting a green kisstest.
python3 tools/check_cur_link.py
WALLY=components/libwally-core
# Same single source as the device build and the UI sim: the firmware update
# tests compare a candidate image against the version this build claims to be,
# so a placeholder here would test the placeholder.
VER=$(head -1 VERSION)
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -DUR_ALLOC_FAIL_TEST=1 \
  -DPQ_SHA256_COMPRESS_HOOK=1 \
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
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_sp.c main/kiss_seed.c main/kiss_seed_sd.c main/kiss_kef.c main/kiss_kef_crypto.c main/platform_sd.c main/kiss_usage.c main/kiss_payee.c main/kiss_backup.c main/kiss_duress.c main/kiss_gword.c main/kiss_coverword.c main/qr_transport.c main/kiss_tapent.c main/kiss_dice.c main/kiss_dice_q.c main/kiss_rngq.c main/kiss_cards_q.c main/kiss_lastword.c main/kiss_rehearse.c main/kiss_fw.c main/kiss_art_rle.c \
  sim/test_crypto.c sim/test_qr.c sim/test_seed.c sim/test_backup.c sim/test_sp.c sim/test_sdseed.c sim/test_kef.c sim/test_duress.c sim/test_gword.c sim/test_coverword.c sim/test_passedit.c sim/test_tapent.c sim/test_dice.c sim/test_rngq.c sim/test_lastword.c sim/test_rehearse.c sim/test_cards_q.c sim/test_fw.c sim/test_art.c sim/test_pq.c \
  -lm -o "$KISS_SIM_TMP/kisstest"
echo "built $KISS_SIM_TMP/kisstest"
