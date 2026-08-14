#!/bin/bash
# Build the on-screen QR scan test page: dev-seed TESTNET PSBTs -> QR part
# strings via the device's own encoder (roundtrip-checked) -> self-contained
# index.html with static + animated (pMofN, BC-UR) codes.
# Usage: sim/mk_test_qrs.sh <out-dir>     then open <out-dir>/index.html
set -e
cd "$(dirname "$0")/.."
OUT="${1:?usage: mk_test_qrs.sh <out-dir>}"
mkdir -p "$OUT"
PY="${PY:-/tmp/spritevenv/bin/python}"
[ -x "$PY" ] || PY=python3

# 1. typed PSBT fixtures (same emitter the SD-sign test uses)
sim/mk_sd_psbts.sh "$OUT"

# 2. the device's QR encoder, desktop-built (qr_transport + cUR only, no wally)
clang -O1 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-const-int-float-conversion -Wno-missing-field-initializers -Wno-deprecated-declarations -Imain -Icomponents/cUR/src \
  components/cUR/src/*.c components/cUR/src/types/*.c \
  components/cUR/src/sha256/sha256.c \
  main/qr_transport.c sim/mk_qr_parts.c -o /tmp/mk_qr_parts

/tmp/mk_qr_parts "$OUT/01-native.psbt"        static > "$OUT/static-native.parts"
/tmp/mk_qr_parts "$OUT/02-nested.psbt"        pmofn  > "$OUT/pmofn-nested.parts"
/tmp/mk_qr_parts "$OUT/03-legacy.psbt"        ur     > "$OUT/ur-legacy.parts"
/tmp/mk_qr_parts "$OUT/04-stop-wrongnet.psbt" static > "$OUT/static-wrongnet.parts"
/tmp/mk_qr_parts "$OUT/05-caution-highfee.psbt" static > "$OUT/static-caution.parts"

# 3. render the page
"$PY" sim/mk_test_qrs.py "$OUT"
echo "open $OUT/index.html full-screen and scan each block"
