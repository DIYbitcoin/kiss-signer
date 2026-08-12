#!/bin/bash
# Build + run the TESTNET PSBT fixture emitter. Writes *.psbt to <out-dir>.
# Usage: sim/mk_sd_psbts.sh "/Volumes/NO NAME"
set -e
cd "$(dirname "$0")/.."
OUT="${1:?usage: mk_sd_psbts.sh <out-dir>}"
WALLY=components/libwally-core
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -I"$WALLY" \
  -I"$WALLY/upstream" \
  -I"$WALLY/upstream/include" \
  -I"$WALLY/upstream/src" \
  -I"$WALLY/upstream/src/ccan" \
  -I"$WALLY/upstream/src/secp256k1" \
  -I"$WALLY/upstream/src/secp256k1/src" \
  -I"$WALLY/upstream/src/secp256k1/include" \
  -Imain \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  sim/mk_sd_psbts.c \
  -o /tmp/mk_sd_psbts
/tmp/mk_sd_psbts "$OUT"
