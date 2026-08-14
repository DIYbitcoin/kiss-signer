#!/bin/bash
# Build + run the TESTNET PSBT fixture emitter. Writes *.psbt to <out-dir>.
# Usage: sim/mk_sd_psbts.sh "/Volumes/NO NAME"
#
# The BIP-375 silent-payment fixtures (11-*, 12-*) are emitted separately:
# they need the SP-capable embit fork, which is not vendored here. Point EMBIT
# at its src/ (diybitcoinhardware/embit commit cf085f8) to include them, or
# leave it unset to emit the libwally fixtures only.
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
if [ -n "${EMBIT:-}" ] && [ -d "$EMBIT" ]; then
    python3 tools/sp_fixtures/mk_sp_sd_fixtures.py --embit "$EMBIT" "$OUT"
else
    echo "EMBIT unset: silent-payment fixtures skipped (11-sp-10in, 12-sp-20in)"
fi
