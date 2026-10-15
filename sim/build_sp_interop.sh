#!/bin/bash
# Build the test-only PSBT bridge. Caller owns the output directory.
set -e
cd "$(dirname "$0")/.."
OUT="${1:?usage: build_sp_interop.sh <output-binary>}"
WALLY=components/libwally-core
# Same firmware sources and minimal libwally configuration as check_sd_psbts.sh.
clang -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations \
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
  -Icomponents/cUR/src \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_cardwallet.c main/kiss_satochip.c main/kiss_satocard.c main/kiss_sp.c \
  main/kiss_seed.c main/kiss_seed_sd.c main/platform_sd.c \
  main/kiss_usage.c main/kiss_backup.c main/kiss_duress.c \
  main/kiss_cards_q.c main/kiss_payee.c \
  components/cUR/src/sha256/sha256.c \
  sim/sp_interop.c \
  -o "$OUT"
