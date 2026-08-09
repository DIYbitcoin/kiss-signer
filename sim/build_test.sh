#!/bin/bash
# Build the desktop crypto test runner -> /tmp/kisstest
# Compiles the SAME vendored libwally amalgamation + config as the device component.
set -e
cd "$(dirname "$0")/.."
WALLY=components/libwally-core
# Same single source as the device build and the UI sim: the firmware update
# tests compare a candidate image against the version this build claims to be,
# so a placeholder here would test the placeholder.
VER=$(head -1 VERSION)
clang -O1 -w \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
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
  "$WALLY/upstream/src/amalgamation/combined.c" \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  main/wallet_crypto.c main/wallet_psbt.c main/wallet_sp.c main/wallet_seed.c main/wallet_seed_sd.c main/platform_sd.c main/wallet_usage.c main/wallet_backup.c main/wallet_duress.c main/wallet_gword.c main/qr_transport.c main/wallet_tapent.c main/wallet_dice.c main/wallet_dice_q.c main/wallet_cards_q.c main/wallet_lastword.c main/wallet_proof.c main/verify_page.c main/wallet_fw.c main/wallet_art_rle.c \
  sim/test_crypto.c sim/test_proof.c sim/test_qr.c sim/test_seed.c sim/test_backup.c sim/test_sp.c sim/test_sdseed.c sim/test_duress.c sim/test_gword.c sim/test_passedit.c sim/test_tapent.c sim/test_dice.c sim/test_lastword.c sim/test_cards_q.c sim/test_fw.c sim/test_art.c \
  -lm -o /tmp/kisstest
echo "built /tmp/kisstest"
