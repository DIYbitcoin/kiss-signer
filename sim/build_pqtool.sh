#!/bin/bash
# Build the release side of the post quantum firmware signature -> pq_tool.
#
# Same sources the device compiles and the same kiss_pqsig.c the device runs,
# so "signed" and "checked" cannot drift apart between the two. See sim/pq_tool.c.
set -e
cd "$(dirname "$0")/.."
KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$KISS_SIM_TMP"
clang -O2 -Wall -Wextra \
  -DPQ_SHA256_COMPRESS_HOOK=1 \
  -Imain -Icomponents/slhdsa -Icomponents/slhdsa/upstream \
  components/slhdsa/pq_hw_sha.c \
  components/slhdsa/upstream/slh_dsa.c components/slhdsa/upstream/slh_sha2.c \
  components/slhdsa/upstream/sha2_256.c components/slhdsa/upstream/sha2_512.c \
  main/kiss_pqsig.c sim/pq_tool.c \
  -o "$KISS_SIM_TMP/pq_tool"
echo "built $KISS_SIM_TMP/pq_tool"
