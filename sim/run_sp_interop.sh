#!/bin/bash
# Offline round trip with a companion kiss-bdk checkout; no broadcast.
set -euo pipefail
# Resolve the caller's path before changing directories.
BDK=$(cd "${1:?usage: run_sp_interop.sh <kiss-bdk-checkout>}" && pwd)
SIGNER=$(cd "$(dirname "$0")/.." && pwd)
if [[ ! -f "$BDK/tools/run_sp_interop.sh" ]]; then
  echo "FAIL: companion coordinator lacks tools/run_sp_interop.sh; update its checkout" >&2
  exit 1
fi
# The coordinator owns the required test list. Never fall back to a substring
# filter: Cargo would report success if an older checkout contained no tests.
exec bash "$BDK/tools/run_sp_interop.sh" "$SIGNER"
