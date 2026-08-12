#!/bin/bash
# Is a mnemonic readable in the device's NVS partition right now?
#
# The only way to answer whether REPLACE actually destroyed the words it
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
#
# WHERE it reads is taken from the chip, not from this file. This script spent
# its whole life reading 0x9000 -- the nvs offset of a partition layout the
# project left behind -- and reporting "erased" from a region the firmware no
# longer writes. A tool whose answers feed an eFuse checklist does not get to
# assume offsets: it reads the partition table at 0x10000 off the attached
# board and finds nvs there. If the table is unreadable (an encrypted board
# returns ciphertext, no 0xAA50 magic), it falls back to the committed CSVs,
# requires both lanes to agree, and says out loud that it is trusting the
# tree rather than the chip.
#
# The old region is not forgotten either: hand-flashed boards that crossed the
# layout move still hold their plaintext words at the OLD 0x9000 (nothing ever
# erased it -- see tools/make_release_notes.py on the beta7 crossing), so the
# legacy range gets its own labelled scan. That residue is real seed material
# on real boards, and until now the only tool that read that address called it
# the live partition.
set -e
cd "$(dirname "$0")/.."

LABEL="${1:-dump}"
PORT="${PORT:-/dev/cu.usbmodem1101}"
OUT="/tmp/kiss_nvs_$LABEL.bin"
TBL="/tmp/kiss_ptable_$LABEL.bin"
LEGACY="/tmp/kiss_nvs_legacy_$LABEL.bin"
ESPTOOL="${ESPTOOL:-$(command -v esptool || true)}"
if [ -z "$ESPTOOL" ]; then
    echo "set ESPTOOL=/path/to/esptool (a venv with 'pip install esptool')" >&2
    exit 1
fi

# The partition table lives at CONFIG_PARTITION_TABLE_OFFSET, 0x10000 in every
# lane's sdkconfig. Read it and walk the 32-byte entries for data/nvs.
"$ESPTOOL" --chip esp32p4 --port "$PORT" -b 460800 \
    --before default-reset --after no-reset \
    read-flash 0x10000 0x1000 "$TBL" >/dev/null 2>&1

NVS_RANGE=$(python3 - "$TBL" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
for at in range(0, len(blob) - 31, 32):
    e = blob[at:at + 32]
    if e[0:2] != b"\xaa\x50":
        break                       # end marker, or ciphertext from byte 0
    typ, sub = e[2], e[3]
    off, size = struct.unpack_from("<II", e, 4)
    if typ == 0x01 and sub == 0x02:
        print(f"0x{off:x} 0x{size:x}")
        sys.exit(0)
print("NO_TABLE")
PY
)

if [ "$NVS_RANGE" = "NO_TABLE" ]; then
    # Encrypted board (the table reads back as ciphertext) or a chip with no
    # table at all. Fall back to the committed CSVs -- and only if both lanes
    # agree, because a two-file divergence at exactly this partition is what
    # caused the original bug.
    NVS_RANGE=$(python3 - <<'PY'
import csv, sys
def nvs_row(path):
    for row in csv.reader(open(path)):
        if row and row[0].strip() == "nvs":
            return (int(row[3].strip(), 16), int(row[4].strip(), 16))
    sys.exit(f"FAIL: {path} has no nvs row")
a = nvs_row("partitions.csv")
b = nvs_row("partitions_encrypted.csv")
if a != b:
    sys.exit("FAIL: partitions.csv and partitions_encrypted.csv disagree on "
             f"nvs ({a[0]:#x}/{a[1]:#x} vs {b[0]:#x}/{b[1]:#x}) - cannot "
             "guess which layout the attached board runs. Fix the divergence "
             "or read the offset off the board by hand.")
print(f"0x{a[0]:x} 0x{a[1]:x}")
PY
)
    echo "note: partition table at 0x10000 is not readable (encrypted board,"
    echo "      or blank chip). Using the CSV offset $NVS_RANGE from the tree,"
    echo "      which describes the tree's layout, not necessarily the board's."
fi

NVS_OFF=$(echo "$NVS_RANGE" | cut -d' ' -f1)
NVS_SZ=$(echo "$NVS_RANGE" | cut -d' ' -f2)

"$ESPTOOL" --chip esp32p4 --port "$PORT" -b 460800 \
    --before default-reset --after no-reset \
    read-flash "$NVS_OFF" "$NVS_SZ" "$OUT" >/dev/null 2>&1

# The legacy pre-0x10000 layout: old table at 0x8000, old nvs at 0x9000.
# Nothing in the firmware or the flash procedure erases this range on a board
# that crossed layouts by hand, so plaintext words can sit here for years.
"$ESPTOOL" --chip esp32p4 --port "$PORT" -b 460800 \
    --before default-reset --after hard-reset \
    read-flash 0x8000 0x8000 "$LEGACY" >/dev/null 2>&1

scan() {  # scan <dump> <base-offset> <what>
python3 - "$1" "$2" "$3" "$LABEL" <<'PY'
import hashlib, re, sys

blob = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16)
what = sys.argv[3]
label = sys.argv[4]

# BIP39 words are 3..8 lowercase letters. A mnemonic is 12 or 24 of them with
# single spaces. Scan the raw bytes, not the parsed key/value entries:
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

blank = all(b == 0xFF for b in blob)
print(f"{what} [{label}]  {len(blob)} bytes from 0x{base:X}"
      + ("  (all 0xFF: blank or never written)" if blank else ""))
if not found:
    print("  no mnemonic readable in this range")
else:
    for fp, d in found.items():
        where = ", ".join(f"0x{base + a:X}" for a in d["at"])
        print(f"  {fp}  {d['n']} words  x{len(d['at'])}  at {where}")
print(f"  ({len(found)} distinct)")
PY
}

scan "$OUT" "$NVS_OFF" "NVS dump"
scan "$LEGACY" "0x8000" "LEGACY range (old layout: table 0x8000, nvs 0x9000)"
