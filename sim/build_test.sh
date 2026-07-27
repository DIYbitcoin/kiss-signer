#!/bin/bash
# Build the desktop crypto test runner -> /tmp/kisstest
# Compiles the SAME vendored libwally amalgamation + config as the device component.
set -e
cd "$(dirname "$0")/.."
WALLY=components/libwally-core
clang -O1 -w \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -DKISS_ROOT="\"$PWD\"" \
  -I"$WALLY" \
  -I"$WALLY/upstream" \
  -I"$WALLY/upstream/include" \
  -I"$WALLY/upstream/src" \
  -I"$WALLY/upstream/src/ccan" \
  -I"$WALLY/upstream/src/secp256k1" \
  -I"$WALLY/upstream/src/secp256k1/src" \
  -I"$WALLY/upstream/src/secp256k1/include" \
  -Imain \
  -Icomponents/cUR/src \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  main/wallet_crypto.c main/wallet_psbt.c main/wallet_sp.c main/wallet_seed.c main/wallet_seed_sd.c main/wallet_usage.c main/wallet_duress.c main/qr_transport.c \
  sim/test_crypto.c sim/test_qr.c sim/test_seed.c sim/test_sp.c sim/test_sdseed.c sim/test_duress.c \
  -o /tmp/kisstest
echo "built /tmp/kisstest"
