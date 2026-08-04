#!/bin/bash
# Load every emitted SD fixture through the device's own verify code and check
# the verdict, the caution flags, and that the amount-proof pair signs the same.
# Usage: sim/check_sd_psbts.sh "/Volumes/NO NAME"
#
# Run it against the card you are about to carry to the device. mk_sd_psbts.sh
# writes the files; this says they still reach the screens they were built for.
set -e
cd "$(dirname "$0")/.."
DIR="${1:?usage: check_sd_psbts.sh <dir>}"
WALLY=components/libwally-core
clang -O1 -w \
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
  main/wallet_crypto.c main/wallet_psbt.c main/wallet_sp.c \
  main/wallet_seed.c main/wallet_seed_sd.c main/platform_sd.c \
  main/wallet_usage.c main/wallet_backup.c main/wallet_duress.c \
  sim/check_sd_psbts.c \
  -o /tmp/check_sd_psbts
/tmp/check_sd_psbts "$DIR"
