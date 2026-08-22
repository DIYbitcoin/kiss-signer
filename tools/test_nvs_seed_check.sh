#!/bin/bash
# Offline test for nvs_seed_check.sh, with a fake esptool in place of a board.
#
# Everything between the USB cable and the recovery ID was untested: the
# partition table walk, the CSV fallback that runs when the table reads back as
# ciphertext, which ranges get read at all, and whether the label the
# before/after comparison depends on reaches the output. All of it is shell and
# a heredoc, none of it needs a chip, and the one thing that DOES need a chip --
# whether real NVS on a real board matches the format -- is not what this
# covers. It covers the plumbing around that.
#
# The fake esptool answers read-flash out of a fixture directory keyed by
# offset, so the script under test runs unmodified, top to bottom.
#
# Written for bash 3.2, which is what macOS ships. Matching is done with `case`
# rather than `echo | grep`: a pipeline runs in a subshell, and a subshell that
# dies takes the cleanup trap with it, which turns one bad line into a cascade
# of missing-fixture errors that say nothing about the real fault. That is not
# hypothetical, it is how the first draft of this file failed.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d "${TMPDIR:-/tmp}/kiss-nvscheck.XXXXXX")
trap 'rm -rf -- "$TMP"' EXIT
FIX="$TMP/fixtures"
mkdir -p "$FIX"

fail() { echo "FAIL: $*" >&2; exit 1; }
has() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

# ---- fixtures ----------------------------------------------------------
# The NVS image is the same page the Python test builds, so both lanes agree on
# what a recoverable pair looks like, followed by blank flash to partition size.
# The expected ID comes from that module rather than being repeated here.
RECOVERY_ID=$(python3 - "$FIX" <<'PY'
import os, struct, sys
sys.path.insert(0, "tools")
import test_nvs_seed_scan as t

fix = sys.argv[1]


def partition_table(nvs_off):
    table = bytearray()
    for name, typ, sub, off, size in (
        (b"nvs", 0x01, 0x02, nvs_off, 0x6000),
        (b"phy_init", 0x01, 0x01, 0x17000, 0x1000),
        (b"ota_0", 0x00, 0x10, 0x20000, 0x7F0000),
    ):
        entry = bytearray(b"\xff" * 32)
        entry[0:2] = b"\xaa\x50"
        entry[2], entry[3] = typ, sub
        struct.pack_into("<II", entry, 4, off, size)
        entry[12 : 12 + len(name)] = name
        table += entry
    return bytes(table) + b"\xff" * (0x1000 - len(table))


def write(name, data):
    open(os.path.join(fix, name), "wb").write(data)


page = t.fixture()
write("0x11000", page + b"\xff" * (0x6000 - len(page)))
write("0x9000", page + b"\xff" * (0x6000 - len(page)))   # for the moved-nvs case
write("0x8000", b"\xff" * 0x8000)                        # legacy range, blank
write("0x10000", partition_table(0x11000))
write("0x10000.moved", partition_table(0x9000))
# The same range as an encrypted board returns it: no 0xAA50 magic anywhere.
write("0x10000.ciphertext", os.urandom(0x1000))
print(t.RECOVERY_ID)
PY
)
[ -n "$RECOVERY_ID" ] || fail "no recovery ID from the fixture builder"

# ---- the fake board ----------------------------------------------------
# Mimics the one esptool call the script makes: read-flash <off> <size> <out>.
# FAKE_TABLE picks which variant of the table this board hands back.
cat > "$TMP/esptool" <<'SH'
#!/bin/bash
set -eu
args=("$@")
i=0
while [ "$i" -lt "$#" ]; do
    if [ "${args[$i]}" = "read-flash" ]; then
        off="${args[$((i + 1))]}"
        size="${args[$((i + 2))]}"
        out="${args[$((i + 3))]}"
        src="$FAKE_FIX/$(printf '0x%x' "$((off))")"
        if [ "$((off))" -eq "$((0x10000))" ] && [ -n "${FAKE_TABLE:-}" ]; then
            src="$src.$FAKE_TABLE"
        fi
        [ -f "$src" ] || { echo "fake esptool: no fixture for $off" >&2; exit 1; }
        head -c "$((size))" "$src" > "$out"
        echo "$off $size" >> "$FAKE_FIX/reads.log"
        exit 0
    fi
    i=$((i + 1))
done
echo "fake esptool: unexpected invocation: $*" >&2
exit 1
SH
chmod +x "$TMP/esptool"
export FAKE_FIX="$FIX"

run() {  # run <label>; output lands in $OUT
    : > "$FIX/reads.log"
    OUT=$(ESPTOOL="$TMP/esptool" PORT=/dev/null bash tools/nvs_seed_check.sh "$1")
}

# ---- 1. the table walk reaches a recoverable pair ----------------------
FAKE_TABLE="" run before
has "$OUT" "recovery ID $RECOVERY_ID" || fail "no recoverable pair reached:
$OUT"
has "$OUT" "24576 bytes from 0x11000" || fail "did not scan nvs where the table said:
$OUT"
has "$OUT" "LEGACY range" || fail "the legacy range was not scanned:
$OUT"
has "$OUT" "[before]" || fail "the label never reached the output:
$OUT"
grep -q "^0x10000 " "$FIX/reads.log" || fail "the partition table was never read"
grep -q "^0x11000 " "$FIX/reads.log" || fail "nvs was never read"
grep -q "^0x8000 " "$FIX/reads.log" || fail "the legacy range was never read"

# The table is the AUTHORITY, which is the whole point of the header comment on
# the script: it spent its life reading a hardcoded 0x9000 and reporting
# "erased" about a region the firmware no longer writes. A script that ignored
# the table would still pass everything above, so move nvs and check it follows.
FAKE_TABLE=moved run moved
has "$OUT" "24576 bytes from 0x9000" || fail "the offset is not taken from the table:
$OUT"
grep -q "^0x9000 " "$FIX/reads.log" || fail "the moved offset was never read"

# ---- 2. an unreadable table falls back to the CSVs, and says so --------
CSV_OFF=$(awk -F, '/^nvs,/ {gsub(/ /, "", $4); print $4}' partitions.csv)
[ -n "$CSV_OFF" ] || fail "partitions.csv has no nvs row to fall back to"
FAKE_TABLE=ciphertext run enc
has "$OUT" "partition table at 0x10000 is not readable" \
    || fail "a ciphertext table did not trigger the CSV fallback:
$OUT"
has "$OUT" "bytes from $CSV_OFF" \
    || fail "the CSV fallback did not scan $CSV_OFF:
$OUT"

# ---- 3. nothing recoverable reads as nothing recoverable ---------------
python3 -c "open('$FIX/0x11000', 'wb').write(b'\xff' * 0x6000)"
FAKE_TABLE="" run wiped
has "$OUT" "no recoverable sealed signer mnemonic found" \
    || fail "a blank partition did not read as blank:
$OUT"
has "$OUT" "$RECOVERY_ID" && fail "a blank partition produced a recovery ID:
$OUT"

# ---- 4. the terminal never sees the words or the key -------------------
python3 - "$FIX" <<'PY'
import os, sys
sys.path.insert(0, "tools")
import test_nvs_seed_scan as t
page = t.fixture(include_plaintext=True)
open(os.path.join(sys.argv[1], "0x11000"), "wb").write(
    page + b"\xff" * (0x6000 - len(page))
)
PY
FAKE_TABLE="" run leak
has "$OUT" "recovery ID $RECOVERY_ID" || fail "the leak fixture recovered nothing"
printf '%s' "$OUT" > "$TMP/leak.txt"
python3 - "$TMP/leak.txt" <<'PY'
import sys
sys.path.insert(0, "tools")
import test_nvs_seed_scan as t
out = open(sys.argv[1], encoding="utf-8").read()
assert t.MNEMONIC.decode() not in out, "the mnemonic reached the terminal"
assert t.NKEY.hex() not in out.lower(), "the device key reached the terminal"
assert "wallet" not in out.lower(), "this is a signer, not a wallet"
PY

echo "PASS: nvs_seed_check.sh plumbing (table walk, CSV fallback, both ranges)"
