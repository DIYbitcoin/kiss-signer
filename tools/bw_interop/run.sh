#!/bin/bash
# BlueWallet QR interop round-trip. Proves, without hardware:
#   1) frames BlueWallet DISPLAYS (CryptoPSBT.toUREncoder(175), its real npm deps)
#      assemble in KISS's qr_transport parser, and
#   2) the animated UR KISS DISPLAYS decodes in BlueWallet's scanner stack.
set -e
cd "$(dirname "$0")/../.."
bash sim/build_test.sh >/dev/null      # /tmp/kisstest also emits the fixture PSBT
clang -O1 -w -Imain -Icomponents/cUR/src \
  main/qr_transport.c components/cUR/src/*.c components/cUR/src/types/*.c \
  components/cUR/src/sha256/sha256.c sim/qr_tool.c -o /tmp/kissqr

mkdir -p /tmp/bwtest && rm -f /tmp/bwtest/*
/tmp/kisstest /tmp/bwtest >/dev/null   # writes kiss-pay.psbt (READY fixture)

cd tools/bw_interop
node encode_bw.js /tmp/bwtest/kiss-pay.psbt > /tmp/bwtest/bw_frames.txt
/tmp/kissqr parse < /tmp/bwtest/bw_frames.txt > /tmp/bwtest/from_bw.psbt
cmp /tmp/bwtest/from_bw.psbt /tmp/bwtest/kiss-pay.psbt \
  && echo "PASS: BlueWallet frames -> KISS parser (byte-identical PSBT)"

/tmp/kissqr emit /tmp/bwtest/kiss-pay.psbt > /tmp/bwtest/kiss_frames.txt
node decode_bw.js < /tmp/bwtest/kiss_frames.txt > /tmp/bwtest/bw_decoded.b64
base64 -i /tmp/bwtest/kiss-pay.psbt | tr -d '\n' > /tmp/bwtest/orig.b64
cmp /tmp/bwtest/bw_decoded.b64 /tmp/bwtest/orig.b64 \
  && echo "PASS: KISS animated UR -> BlueWallet decoder (byte-identical PSBT)"
