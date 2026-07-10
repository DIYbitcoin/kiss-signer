#!/bin/bash
# Format all C/H files under src/ and tests/, plus the uUR.c MicroPython
# wrapper at the repo root. Uses the rules from .clang-format.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Running clang-format..."

find src tests -type f \( -name '*.c' -o -name '*.h' \) \
    -not -path '*/build/*' -not -path '*/obj/*' \
    -exec clang-format -i {} +

# MicroPython wrapper lives at the repo root.
if [ -f uUR.c ]; then
    clang-format -i uUR.c
fi

echo "Formatting complete."
