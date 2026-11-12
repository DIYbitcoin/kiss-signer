#!/bin/bash
# Build the public practice page, docs/practice/index.html: the practice seed
# and three testnet transactions as animated QR codes, for someone who has just
# built a KISS and wants to try signing in a minute with no coordinator.
#
# Everything on the page is made by this repository's own code:
#   - the transactions by sim/mk_sd_psbts.c, the same emitter the SD card tests
#     use, from the practice seed (abandon x11 + about, kiss_seed_is_test_vector);
#   - the UR parts by the device's encoder (sim/mk_qr_parts.c), each one fed
#     back through the device's parser before it is written;
#   - the QR symbols by qrcodegen, the encoder the firmware draws with;
#   - every number and verdict the page states by the device's own verify and
#     sign code (sim/practice_facts.c), which also fails the build if a
#     transaction the page calls signable does not sign.
#
# Usage: sim/mk_practice_page.sh          writes docs/practice/index.html
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/kiss-practice.XXXXXX)
trap 'rm -rf "$T"' EXIT
WALLY=components/libwally-core
QRGEN=managed_components/lvgl__lvgl/src/libs/qrcode
[ -f "$QRGEN/qrcodegen.c" ] || { echo "missing $QRGEN (run an IDF build once to fetch LVGL)" >&2; exit 1; }
CF="-O1 -w -DNDEBUG=1 -DBUILD_MINIMAL=1 -DECMULT_WINDOW_SIZE=8"
WI="-I$WALLY -I$WALLY/upstream -I$WALLY/upstream/include -I$WALLY/upstream/src -I$WALLY/upstream/src/ccan -I$WALLY/upstream/src/secp256k1 -I$WALLY/upstream/src/secp256k1/src -I$WALLY/upstream/src/secp256k1/include"

# 1. the transactions
clang $CF $WI -Imain "$WALLY/upstream/src/amalgamation/combined.c" sim/mk_sd_psbts.c -o "$T/mk_sd_psbts"
"$T/mk_sd_psbts" "$T" > /dev/null
PSBTS="01-native 05-caution-highfee 04-stop-wrongnet"

# 2. what the device says about each, from its own code, on a scratch card
clang $CF $WI -Imain -Isim -Isim/shims -Icomponents/cUR/src \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_cardwallet.c main/kiss_satochip.c \
  main/kiss_satocard.c main/kiss_sp.c main/kiss_seed.c main/kiss_seed_sd.c \
  main/platform_sd.c main/kiss_usage.c main/kiss_backup.c main/kiss_duress.c \
  main/kiss_cards_q.c main/kiss_payee.c components/cUR/src/sha256/sha256.c \
  sim/practice_facts.c -o "$T/practice_facts"
args=""
for p in $PSBTS; do args="$args $T/$p.psbt"; done
KISS_SIM_TMP="$T" "$T/practice_facts" $args > "$T/facts.jsonl"

# 3. UR parts, by the device's encoder, round trip checked
clang -O1 -w -Imain -Icomponents/cUR/src components/cUR/src/*.c \
  components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  main/qr_transport.c sim/mk_qr_parts.c -o "$T/mk_qr_parts"
# 4. QR symbols, by the firmware's QR encoder
clang -O1 -w -DLV_CONF_INCLUDE_SIMPLE -Isim -I"$QRGEN" "$QRGEN/qrcodegen.c" sim/mk_qr_matrix.c -o "$T/mk_qr_matrix"
for p in $PSBTS; do
  "$T/mk_qr_parts" "$T/$p.psbt" ur > "$T/$p.parts" 2> /dev/null
  "$T/mk_qr_matrix" < "$T/$p.parts" > "$T/$p.qr"
done
# The practice seed as a SeedQR: four digits per word, the wordlist index.
# abandon is 0000 and about is 0003.
printf '%s0003\n' "00000000000000000000000000000000000000000000" | "$T/mk_qr_matrix" > "$T/seed.qr"

# 5. every code read back by the device's QR decoder, UR reader and seed QR
#    reader; the build stops here if one does not give back what made it
clang $CF $WI -Imain -Isim -Isim/shims -Icomponents/cUR/src \
  -Icomponents/k_quirc/include -Icomponents/k_quirc/src \
  "$WALLY/upstream/src/amalgamation/combined.c" \
  main/kiss_crypto.c main/kiss_psbt.c main/kiss_cardwallet.c main/kiss_satochip.c \
  main/kiss_satocard.c main/kiss_sp.c main/kiss_seed.c main/kiss_seed_sd.c \
  main/platform_sd.c main/kiss_usage.c main/kiss_backup.c main/kiss_duress.c \
  main/kiss_cards_q.c main/kiss_payee.c main/qr_transport.c \
  components/cUR/src/*.c components/cUR/src/types/*.c components/cUR/src/sha256/sha256.c \
  components/k_quirc/src/*.c sim/practice_readback.c -o "$T/practice_readback"
rb=""
for p in $PSBTS; do rb="$rb $T/$p.qr $T/$p.psbt"; done
KISS_SIM_TMP="$T" "$T/practice_readback" "$T/seed.qr" $rb

# 6. the page
mkdir -p docs/practice
python3 -I sim/mk_practice_page.py "$T" docs/practice/index.html
echo "wrote docs/practice/index.html"
