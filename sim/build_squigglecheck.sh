#!/bin/bash
# Build the wallet-picture distinctness report (see sim/squigglecheck.c).
#
# Needs the pattern encoder and nothing else: no LVGL, no fonts, no screens.
# What it asks is whether two fingerprints look different, which is a question
# about the encoding and not about the UI drawing it.
set -e
cd "$(dirname "$0")/.."
clang -O1 -w \
  -Icomponents/bitsquiggle32 \
  components/bitsquiggle32/bitsquiggle32.c \
  sim/squigglecheck.c \
  -lm -o /tmp/kisssquiggle
echo "built /tmp/kisssquiggle"
