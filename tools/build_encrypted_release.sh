#!/bin/bash
# ENCRYPTED-release build -> build-encrypted-release/  (dev + release builds untouched).
#
# This is the step-8 hardening lane: the release profile (KISS_RELEASE=1, dev
# seed compiled OUT) PLUS:
#   * flash encryption, RELEASE mode  - first boot burns the key into eFuse
#     (ONE WAY) and encrypts the whole flash in place
#   * NVS encryption                  - plain flash encryption does NOT cover
#     "nvs" data partitions, and the seed words live in NVS; NVS encryption
#     stores XTS keys in the new nvs_key partition, which IS flash-encrypted
#   * secure boot OFF                 - deliberate for the beta: it is a second
#     one-way eFuse step, added later as its own pass
#
# THIS BUILD IS FOR A FRESH / FINAL BOARD ONLY. It never touches the v1.3
# engineering sample. This script only builds and verifies - it never flashes.
#
# What "release mode" means, so nobody is surprised later:
#   * the encryption key is generated ON the device and is unreadable forever
#   * after the first boot, serial reflash is IMPOSSIBLE (the table is
#     factory-only, no OTA), so the firmware on that board is frozen
#   * an attacker with the board can erase it (denial of service) but can
#     never read the seed out of flash
set -e
cd "$(dirname "$0")/.."

if [ -z "$ALLOW_DIRTY" ] && [ -n "$(git status --porcelain)" ]; then
    echo "You have uncommitted changes - an encrypted release must be built"
    echo "from a clean, committed tree. Commit first, then rerun."
    echo "(throwaway test? prefix with ALLOW_DIRTY=1)"
    exit 1
fi

# sdkconfig.encrypted = the board's dev sdkconfig, transformed:
# quiet logs + after no-reset (same as sdkconfig.release), then the
# encryption settings forced on top. Regenerated every build - never drifts.
python3 - <<'PY'
force = {
    # hardening
    "CONFIG_SECURE_FLASH_ENC_ENABLED":              "y",
    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE":  "y",
    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT": None,   # explicitly OFF
    "CONFIG_NVS_ENCRYPTION":                        "y",
    # P4 defaults the NVS key-protection choice to the HMAC scheme (needs a
    # pre-burned eFuse key block); we want the flash-encryption scheme: XTS
    # keys auto-generated on first use into the nvs_key partition
    "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC":   "y",
    "CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC":        None,
    # reproducible binaries: no compile date/time embedded, so the same
    # commit always builds the same bytes (CI and verifiers can compare)
    "CONFIG_APP_REPRODUCIBLE_BUILD":                "y",
    "CONFIG_APP_COMPILE_TIME_DATE":                 None,
    # partition table with the nvs_key (NVS-encryption XTS keys) partition;
    # table offset moves to 0x10000 because the flash-encryption bootloader
    # (~0x8840 bytes) no longer fits under 0x8000 (also secure-boot headroom)
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME":       '"partitions_encrypted.csv"',
    "CONFIG_PARTITION_TABLE_FILENAME":              '"partitions_encrypted.csv"',
    "CONFIG_PARTITION_TABLE_OFFSET":                "0x10000",
    # release-profile log level
    "CONFIG_LOG_DEFAULT_LEVEL_INFO":                None,
    "CONFIG_LOG_DEFAULT_LEVEL_WARN":                "y",
    "CONFIG_LOG_DEFAULT_LEVEL":                     "2",
    "CONFIG_LOG_MAXIMUM_LEVEL":                     "2",
    # never auto-reset after flash (v-1.3-style boards want a power-on boot)
    "CONFIG_ESPTOOLPY_AFTER_RESET":                 None,
    "CONFIG_ESPTOOLPY_AFTER_NORESET":               "y",
    "CONFIG_ESPTOOLPY_AFTER":                       '"no-reset"',
}
out, seen = [], set()
for l in open("sdkconfig").read().splitlines():
    key = None
    if l.startswith("CONFIG_"):
        key = l.split("=", 1)[0]
    elif l.startswith("# CONFIG_") and l.endswith(" is not set"):
        key = l[2:-len(" is not set")]
    if key in force:
        seen.add(key)
        v = force[key]
        out.append(f"# {key} is not set" if v is None else f"{key}={v}")
    else:
        out.append(l)
for key, v in force.items():
    if key not in seen and v is not None:
        out.append(f"{key}={v}")
open("sdkconfig.encrypted", "w").write("\n".join(out) + "\n")
print("wrote sdkconfig.encrypted (flash enc RELEASE + NVS enc, logs WARN)")
PY

GIT_REV=$(git describe --always --dirty 2>/dev/null || echo nogit)
echo "commit: $GIT_REV"

docker run --rm \
  -e GIT_CONFIG_COUNT=1 \
  -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project \
  -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B build-encrypted-release -DSDKCONFIG=/project/sdkconfig.encrypted \
  -DKISS_RELEASE=1 -DKISS_COMMIT="$GIT_REV" build

# ---- verify: binary contents AND the security config that actually built ----
GIT_REV="$GIT_REV" python3 - <<'PY'
import os, sys
fails = 0

blob = open("build-encrypted-release/guition_kiss_bringup.bin", "rb").read()
rev = os.environ.get("GIT_REV", "").encode()
checks = [
    (bool(rev) and rev in blob,          f"commit {rev.decode()} present"),
    (b"abandon abandon" not in blob,     "no dev mnemonic in binary"),
    (b"developer build" not in blob,     "no dev banner in binary"),
    (open("VERSION").read().strip().encode() in blob, "version string present"),
]

cfg = open("sdkconfig.encrypted").read().splitlines()
def on(k):  return f"{k}=y" in cfg
checks += [
    (on("CONFIG_SECURE_FLASH_ENC_ENABLED"),             "flash encryption enabled"),
    (on("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE"), "flash encryption RELEASE mode"),
    (not on("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT"), "development mode off"),
    (on("CONFIG_NVS_ENCRYPTION"),                       "NVS encryption enabled"),
    (on("CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC"),  "NVS keys via flash-enc scheme (nvs_key partition)"),
    (not on("CONFIG_SECURE_BOOT"),                      "secure boot off (own later pass)"),
    (on("CONFIG_ESPTOOLPY_NO_STUB"),                    "esptool no-stub mode (required with flash encryption)"),
    (on("CONFIG_APP_REPRODUCIBLE_BUILD"),               "reproducible build (no compile date embedded)"),
]

pt = open("build-encrypted-release/partition_table/partition-table.bin", "rb").read()
checks += [
    (b"nvs_key" in pt, "nvs_key (NVS XTS key) partition present"),
    (b"factory" in pt, "factory app partition present"),
]

for ok, label in checks:
    print(("PASS: " if ok else "FAIL: ") + label)
    fails += 0 if ok else 1
print(f"encrypted release app: {len(blob)} bytes")
sys.exit(1 if fails else 0)
PY

cat <<EOF

encrypted release build OK: build-encrypted-release/

################################################################################
#  READ BEFORE FLASHING - THIS IS A ONE-WAY OPERATION
#
#  * FRESH / FINAL BOARD ONLY. Never the v1.3 engineering sample.
#  * First boot burns the flash-encryption eFuse key: PERMANENT.
#  * After first boot this board can NEVER be serial-reflashed again
#    (factory-only partition table, no OTA). Firmware is frozen. The web
#    installer will never work on this board again. That is the point.
#  * First boot encrypts ~6MB of flash in place: it can take a few minutes
#    on a black screen. DO NOT UNPLUG until the game menu appears.
#    Losing power mid-encryption can brick the board.
################################################################################

1. erase the fresh board (proves it is fresh, wipes any factory demo):
   uvx esptool --chip esp32p4 -p <port> erase-flash

2. one full plaintext flash (first boot encrypts it in place; note the
   encrypted lane's SHIFTED offsets - table 0x10000, app 0x20000 - and
   --no-stub, which flash-encrypted builds require):
   uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\
     --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\
     0x2000  build-encrypted-release/bootloader/bootloader.bin \\
     0x10000 build-encrypted-release/partition_table/partition-table.bin \\
     0x20000 build-encrypted-release/guition_kiss_bringup.bin

3. unplug -> ~3s -> replug, then WAIT (see warning above).
   When the amber "flash not yet encrypted" line is GONE from Settings,
   the eFuse says encryption is live - only then create the wallet.
EOF
