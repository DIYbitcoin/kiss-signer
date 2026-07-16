#!/bin/bash
# Release-profile build -> build-release/ (dev build-disp + sdkconfig untouched).
#
# What the release profile changes:
#   * KISS_RELEASE=1        - dev mnemonic/selftest compiled OUT (the string
#                             must not exist in the binary), boot fingerprint
#                             display off, Settings shows "KISS <version>"
#   * quiet logs            - default level WARN (INFO strings stripped, since
#                             CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT=y)
#
# What it deliberately does NOT change yet:
#   * flash encryption      - that is the FINAL hardening step, done on purpose
#     as its own pass: enable CONFIG_SECURE_FLASH_ENC_ENABLED (development
#     mode first!), verify boot in QEMU, then one deliberate flash of the
#     target board (first boot burns eFuses - one way). Only after that does a
#     board count as a release/funded board. Never trial-run it on the only
#     v1.3 engineering sample without the QEMU pass.
#
# After building, this script verifies the binary: dev mnemonic absent,
# version string present.
set -e
cd "$(dirname "$0")/.."

# sdkconfig.release = the board's dev sdkconfig with quieter logs, regenerated
# on every build so it can never drift from the real board config.
python3 - <<'PY'
lines = open("sdkconfig").read().splitlines()
out = []
for l in lines:
    if l == "CONFIG_LOG_DEFAULT_LEVEL_INFO=y":
        out.append("# CONFIG_LOG_DEFAULT_LEVEL_INFO is not set")
        out.append("CONFIG_LOG_DEFAULT_LEVEL_WARN=y")
    elif l == "# CONFIG_LOG_DEFAULT_LEVEL_WARN is not set":
        continue                       # replaced above
    elif l.startswith("CONFIG_LOG_DEFAULT_LEVEL="):
        out.append("CONFIG_LOG_DEFAULT_LEVEL=2")
    elif l.startswith("CONFIG_LOG_MAXIMUM_LEVEL="):
        out.append("CONFIG_LOG_MAXIMUM_LEVEL=2")
    elif l == "# CONFIG_APP_REPRODUCIBLE_BUILD is not set":
        out.append("CONFIG_APP_REPRODUCIBLE_BUILD=y")   # same commit = same bytes
    elif l == "CONFIG_APP_COMPILE_TIME_DATE=y":
        out.append("# CONFIG_APP_COMPILE_TIME_DATE is not set")
    elif l == "CONFIG_ESPTOOLPY_AFTER_RESET=y":
        out.append("# CONFIG_ESPTOOLPY_AFTER_RESET is not set")
    elif l == "# CONFIG_ESPTOOLPY_AFTER_NORESET is not set":
        out.append("CONFIG_ESPTOOLPY_AFTER_NORESET=y")
    elif l.startswith("CONFIG_ESPTOOLPY_AFTER="):
        out.append("CONFIG_ESPTOOLPY_AFTER=\"no-reset\"")
    else:
        out.append(l)
open("sdkconfig.release", "w").write("\n".join(out) + "\n")
print("wrote sdkconfig.release (logs: WARN)")
PY

# short commit from the HOST's git (the container can't read the bind-mounted
# repo's ownership); baked into the Settings/home build-identity line
GIT_REV=$(git describe --always --dirty 2>/dev/null || echo nogit)
echo "commit: $GIT_REV"

docker run --rm \
  -e GIT_CONFIG_COUNT=1 \
  -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project \
  -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B build-release -DSDKCONFIG=/project/sdkconfig.release -DKISS_RELEASE=1 \
  -DKISS_COMMIT="$GIT_REV" build

# ---- verify the release binary ----
GIT_REV="$GIT_REV" python3 - <<'PY'
import os, sys
bin_path = "build-release/guition_kiss_bringup.bin"
blob = open(bin_path, "rb").read()
fails = 0
rev = os.environ.get("GIT_REV", "").encode()
if rev and rev in blob:
    print(f"PASS: commit {rev.decode()} present")
else:
    print(f"FAIL: commit {rev.decode()} missing"); fails += 1
if b"abandon abandon" in blob:
    print("FAIL: dev mnemonic found in release binary"); fails += 1
else:
    print("PASS: no dev mnemonic in release binary")
if b"KISS %s dev (%s)" in blob:
    print("FAIL: dev build banner found in release binary"); fails += 1
else:
    print("PASS: no dev banner in release binary")
ver = open("VERSION").read().strip().encode()
if ver in blob:
    print(f"PASS: version {ver.decode()} present")
else:
    print(f"FAIL: version {ver.decode()} missing"); fails += 1
if b"C6 radio held in reset" in blob:
    print("PASS: C6 radio-hold code present")
else:
    print("FAIL: C6 radio-hold code missing"); fails += 1
# no-wireless gate: the board's C6 radio chip is held in reset and nothing
# may talk to it, so the ELF must link ZERO objects from any radio/network
# library. (project_description.json's build_components lists every
# registered component and proves nothing; the linker map shows what the
# binary actually contains.)
import re
mapf = open("build-release/guition_kiss_bringup.map").read()
linked = []
for lib in ("libesp_wifi", "libesp_wifi_remote", "libesp_hosted", "libbt.",
            "libwpa_supplicant", "liblwip", "libesp_netif", "libopenthread",
            "libieee802154", "libesp_phy", "libesp_coex"):
    n = len(re.findall(re.escape(lib) + r"[^\s(]*\(", mapf))
    if n: linked.append(f"{lib}:{n}")
if linked:
    print("FAIL: wireless/network objects linked: " + " ".join(linked)); fails += 1
else:
    print("PASS: no wireless/network stack linked (linker map)")
print(f"release app: {len(blob)} bytes")
sys.exit(1 if fails else 0)
PY

# flash budget: baked art is ~75% of the binary; fail while there is still
# headroom to react, not on the flash step (set -e stops on a FAIL)
python3 tools/check_flash_budget.py build-release/guition_kiss_bringup.bin partitions.csv
echo
echo "release build OK: build-release/guition_kiss_bringup.bin"
echo
echo "ESP-IDF flash (local ESP-IDF install; sdkconfig uses no-reset):"
echo "  idf.py -B build-release -p <port> flash"
echo
echo "ESP-IDF app-only reflash:"
echo "  idf.py -B build-release -p <port> app-flash"
echo
echo "direct esptool fallback - app-only reflash:"
echo "  uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\"
echo "    write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\"
echo "    0x10000 build-release/guition_kiss_bringup.bin"
echo
echo "direct esptool fallback - full flash (fresh board, or whenever bootloader/partitions changed;"
echo "offsets from build-release/flash_args - the encrypted-release lane will"
echo "need this full set):"
echo "  uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\"
echo "    write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\"
echo "    0x2000  build-release/bootloader/bootloader.bin \\"
echo "    0x8000  build-release/partition_table/partition-table.bin \\"
echo "    0x10000 build-release/guition_kiss_bringup.bin"
echo
echo "then: unplug -> ~3s -> replug (v1.3 sample never boots off a USB reset)"
