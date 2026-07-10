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
    else:
        out.append(l)
open("sdkconfig.release", "w").write("\n".join(out) + "\n")
print("wrote sdkconfig.release (logs: WARN)")
PY

docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B build-release -DSDKCONFIG=/project/sdkconfig.release -DKISS_RELEASE=1 build

# ---- verify the release binary ----
python3 - <<'PY'
import sys
bin_path = "build-release/guition_kiss_bringup.bin"
blob = open(bin_path, "rb").read()
fails = 0
if b"abandon abandon" in blob:
    print("FAIL: dev mnemonic found in release binary"); fails += 1
else:
    print("PASS: no dev mnemonic in release binary")
if b"developer build" in blob:
    print("FAIL: dev build banner found in release binary"); fails += 1
else:
    print("PASS: no dev banner in release binary")
ver = open("VERSION").read().strip().encode()
if ver in blob:
    print(f"PASS: version {ver.decode()} present")
else:
    print(f"FAIL: version {ver.decode()} missing"); fails += 1
print(f"release app: {len(blob)} bytes")
sys.exit(1 if fails else 0)
PY
echo
echo "release build OK: build-release/guition_kiss_bringup.bin"
echo "flash:  uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\"
echo "        write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x10000 build-release/guition_kiss_bringup.bin"
