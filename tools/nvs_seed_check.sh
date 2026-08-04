#!/bin/bash
# Is a mnemonic readable in the device's NVS partition right now?
#
# The only way to answer whether REPLACE actually destroyed the wallet it
# replaced. NVS is log structured, so a logically deleted mnemonic stays on its
# page until a compaction that may never come; nothing in the simulator can see
# that, because the host build has no NVS at all.
#
# Prints FINGERPRINTS, never words. A seed phrase read off a chip is still a
# seed phrase, and this runs on a terminal with scrollback.
#
#   bash tools/nvs_seed_check.sh before
#   ... do the thing on the device ...
#   bash tools/nvs_seed_check.sh after
#
# Then compare: a label present in "before" and absent from "after" is a
# mnemonic that was really erased. Present in both means it survived.
set -e
cd "$(dirname "$0")/.."

LABEL="${1:-dump}"
PORT="${PORT:-/dev/cu.usbmodem1101}"
OUT="/tmp/kiss_nvs_$LABEL.bin"
ESPTOOL="${ESPTOOL:-$(command -v esptool || true)}"
if [ -z "$ESPTOOL" ]; then
    echo "set ESPTOOL=/path/to/esptool (a venv with 'pip install esptool')" >&2
    exit 1
fi

# nvs partition: offset 0x9000, size 0x6000 (see partitions.csv)
"$ESPTOOL" --chip esp32p4 --port "$PORT" -b 460800 \
    --before default-reset --after hard-reset \
    read-flash 0x9000 0x6000 "$OUT" >/dev/null 2>&1

python3 - "$OUT" "$LABEL" <<'PY'
import hashlib, re, sys

blob = open(sys.argv[1], "rb").read()
label = sys.argv[2]

# BIP39 words are 3..8 lowercase letters. A mnemonic is 12 or 24 of them with
# single spaces. Scan the raw partition, not the parsed key/value entries:
# residue is exactly the thing NVS no longer lists.
text = blob.decode("latin-1")
found = {}
for m in re.finditer(r"(?:[a-z]{3,8} ){11,23}[a-z]{3,8}", text):
    s = m.group(0)
    n = len(s.split())
    if n not in (12, 24):
        continue
    fp = hashlib.sha256(s.encode()).hexdigest()[:12].upper()
    found.setdefault(fp, {"n": n, "at": []})["at"].append(m.start())

print(f"NVS dump [{label}]  {len(blob)} bytes from 0x9000")
if not found:
    print("  no mnemonic readable in this partition")
else:
    for fp, d in found.items():
        where = ", ".join(f"0x{9 * 4096 + a:X}" for a in d["at"])
        print(f"  {fp}  {d['n']} words  x{len(d['at'])}  at {where}")
print(f"  ({len(found)} distinct)")
PY
