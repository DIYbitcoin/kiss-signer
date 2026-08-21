#!/bin/bash
# BlueWallet QR interop round-trip. Proves, without hardware:
#   1) frames BlueWallet DISPLAYS (CryptoPSBT.toUREncoder(175), its real npm deps)
#      assemble in KISS's qr_transport parser, and
#   2) the animated UR KISS DISPLAYS decodes in BlueWallet's scanner stack.
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
