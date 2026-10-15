#!/bin/bash
# Offline round trip with a sibling kiss-bdk checkout; no broadcast.
set -euo pipefail
cd "$(dirname "$0")/.."
BDK="${1:?usage: run_sp_interop.sh <kiss-bdk-checkout>}"
BDK=$(cd "$BDK" && pwd)
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/kiss-sp-interop.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT
bash sim/build_sp_interop.sh "$SCRATCH/signer"
export KISS_INTEROP_SIGNER="$SCRATCH/signer"
cd "$BDK"
cargo test --locked --test sp_psbt --test sp_spend_fixtures c_signer -- --ignored --nocapture
