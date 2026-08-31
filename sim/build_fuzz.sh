#!/bin/bash
# Build the parser fuzz harness -> /tmp/kissfuzz, with ASAN+UBSAN so memory
# bugs fail loudly instead of silently corrupting. Same vendored libwally +
# flags as the device component.
set -e
cd "$(dirname "$0")/.."

# Where this build's binary goes. KISS_SIM_TMP is the same root the fake card,
# the seed files and the captured frames use (main/kiss_simpath.h) -- unset it
# is /tmp, exactly as before. It is here as well as in the C because two people
# building at once wrote each other's binary, and the loser then ran a walk over
# somebody else's code and reported findings about it.
KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$KISS_SIM_TMP"

WALLY=components/libwally-core
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations -g \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -fno-omit-frame-pointer \
  -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8 \
  -DAVOID_UNALIGNED_ACCESS=1 \
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
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_seed.c main/kiss_cards_q.c \
  main/qr_transport.c \
  main/kiss_sp.c \
  main/kiss_usage.c main/kiss_payee.c main/kiss_backup.c main/kiss_duress.c main/kiss_seed_sd.c main/platform_sd.c \
  main/kiss_kef.c main/kiss_kef_crypto.c main/kiss_pbkdf2.c \
  sim/test_fuzz.c \
  -lm -o "$KISS_SIM_TMP/kissfuzz"
echo "built $KISS_SIM_TMP/kissfuzz"
