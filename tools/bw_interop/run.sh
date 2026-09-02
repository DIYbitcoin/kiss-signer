#!/bin/bash
# BlueWallet QR interop round-trip. Proves, without hardware:
#   1) frames BlueWallet DISPLAYS (CryptoPSBT.toUREncoder(175), its real npm deps)
#      assemble in KISS's qr_transport parser, and
#   2) the animated UR KISS DISPLAYS decodes in BlueWallet's scanner stack.
#
# "its real npm deps" is the whole claim, so it is checked rather than asserted:
# tools/bw_interop/package.json pins @keystonehq/bc-ur-registry 0.8.0 and
# @ngraveio/bc-ur 1.1.13, and those are the exact strings in BlueWallet's own
# package.json at v8.0.2, read 2026-09-01. Both pins are exact, so npm cannot
# drift underneath this; BlueWallet can. When it does, this test still passes
# and stops testing what it says -- so re-read that file on a version bump and
# move the two pins together with this comment.
set -e
cd "$(dirname "$0")/../.."
export KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
KISSTEST="$KISS_SIM_TMP/kisstest"
KISSQR="$KISS_SIM_TMP/kissqr"
BWTEST="$KISS_SIM_TMP/bwtest"
mkdir -p "$KISS_SIM_TMP"

bash sim/build_test.sh >/dev/null      # kisstest also emits the fixture PSBT
clang -O1 -w -Imain -Icomponents/cUR/src \
  main/qr_transport.c components/cUR/src/*.c components/cUR/src/types/*.c \
  components/cUR/src/sha256/sha256.c sim/qr_tool.c -o "$KISSQR"

mkdir -p "$BWTEST" && rm -f "$BWTEST"/*
"$KISSTEST" "$BWTEST" >/dev/null      # writes kiss-pay.psbt (READY fixture)

node tools/bw_interop/encode_bw.js "$BWTEST/kiss-pay.psbt" > "$BWTEST/bw_frames.txt"
"$KISSQR" parse < "$BWTEST/bw_frames.txt" > "$BWTEST/from_bw.psbt"
cmp "$BWTEST/from_bw.psbt" "$BWTEST/kiss-pay.psbt" \
  && echo "PASS: BlueWallet frames -> KISS parser (byte-identical PSBT)"

"$KISSQR" emit "$BWTEST/kiss-pay.psbt" > "$BWTEST/kiss_frames.txt"
node tools/bw_interop/decode_bw.js < "$BWTEST/kiss_frames.txt" > "$BWTEST/bw_decoded.b64"
base64 -i "$BWTEST/kiss-pay.psbt" | tr -d '\n' > "$BWTEST/orig.b64"
cmp "$BWTEST/bw_decoded.b64" "$BWTEST/orig.b64" \
  && echo "PASS: KISS animated UR -> BlueWallet decoder (byte-identical PSBT)"
