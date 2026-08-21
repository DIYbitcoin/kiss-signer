#!/bin/bash
# Boundary regression for the two desktop QR emitters: a full input buffer is
# valid, but one more byte must be rejected rather than silently truncated.
set -e
cd "$(dirname "$0")/.."

TMP_ROOT="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$TMP_ROOT"
CASE_DIR=$(mktemp -d "$TMP_ROOT/kiss-qr-cli.XXXXXX")
trap 'rm -rf "$CASE_DIR"' EXIT

COMMON=(
  -O1 -Wall -Wextra -Wno-unused-parameter
  -Wno-implicit-const-int-float-conversion
  -Wno-missing-field-initializers -Wno-deprecated-declarations
  -Imain -Icomponents/cUR/src
  components/cUR/src/*.c components/cUR/src/types/*.c
  components/cUR/src/sha256/sha256.c main/qr_transport.c
)
clang "${COMMON[@]}" sim/mk_qr_parts.c -o "$CASE_DIR/mk_qr_parts"
clang "${COMMON[@]}" sim/qr_tool.c -o "$CASE_DIR/kissqr"

LIMIT=$(awk '$1 == "#define" && $2 == "QRT_MAX_PSBT" { print $3; exit }' \
  main/qr_transport.h)
OVER=$((LIMIT + 1))
AT_LIMIT="$CASE_DIR/at-limit.psbt"
TOO_BIG="$CASE_DIR/too-big.psbt"
dd if=/dev/zero of="$AT_LIMIT" bs="$LIMIT" count=1 2>/dev/null
dd if=/dev/zero of="$TOO_BIG" bs="$OVER" count=1 2>/dev/null

"$CASE_DIR/mk_qr_parts" "$AT_LIMIT" ur > /dev/null 2> "$CASE_DIR/mk-max.err"
"$CASE_DIR/kissqr" emit "$AT_LIMIT" > /dev/null 2> "$CASE_DIR/qr-max.err"

rejects_oversize()
{
  local name="$1"
  shift
  if "$@" > "$CASE_DIR/$name.out" 2> "$CASE_DIR/$name.err"; then
    echo "FAIL: $name accepted $OVER bytes"
    exit 1
  fi
  test ! -s "$CASE_DIR/$name.out"
  grep -Fq "input exceeds QRT_MAX_PSBT ($LIMIT bytes)" "$CASE_DIR/$name.err"
  echo "PASS: $name accepts $LIMIT bytes and rejects $OVER"
}

rejects_oversize mk_qr_parts "$CASE_DIR/mk_qr_parts" "$TOO_BIG" ur
rejects_oversize kissqr "$CASE_DIR/kissqr" emit "$TOO_BIG"

rejects_read_error()
{
  local name="$1"
  shift
  if "$@" > "$CASE_DIR/$name-read.out" 2> "$CASE_DIR/$name-read.err"; then
    echo "FAIL: $name accepted an unreadable input"
    exit 1
  fi
  test ! -s "$CASE_DIR/$name-read.out"
  grep -Fq "read failed" "$CASE_DIR/$name-read.err"
  echo "PASS: $name rejects an input read error"
}

# fopen succeeds for a directory on the supported POSIX hosts, then fread must
# report EISDIR. This exercises the error path separately from an open failure.
rejects_read_error mk_qr_parts "$CASE_DIR/mk_qr_parts" "$CASE_DIR" ur
rejects_read_error kissqr "$CASE_DIR/kissqr" emit "$CASE_DIR"
