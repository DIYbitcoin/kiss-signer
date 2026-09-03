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
. tools/idf_image.sh

# Build state starts clean, every run. A stale UNSIGNED marker from an earlier
# reproducibility run would abort the publish gate on a freshly signed build --
# and the inverse, a marker outliving the run that wrote it, is exactly the lie
# the marker exists to prevent. The marker only, never the directory: a full
# rebuild costs twenty minutes and buys nothing this delete does not.
rm -f build-release/UNSIGNED

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
    # The BOOTLOADER's own logs, quieted in the release lane ONLY.
    #
    # Not taste, and not really about noise: the bootloader is flashed at
    # 0x2000 and the partition table at CONFIG_PARTITION_TABLE_OFFSET, so that
    # offset IS its budget. It was 0x8000 -- 24576 bytes, with the build using
    # 23200 -- until partitions.csv moved the table to 0x10000 for flash
    # encryption's sake. There is room now, and this stays anyway: a release
    # build has nobody reading its boot log, and the 2544 bytes it saves are
    # 2544 bytes secure boot does not have to find later.
    #
    # The shared sdkconfig keeps INFO on purpose. Bootloader INFO lines are
    # exactly what a boot failure on this board is read through -- the Boya
    # flash auto-suspend brick was found that way -- and taking them off the
    # dev build to buy room in the release build would be paying the wrong
    # lane. Widening the budget instead is not available: nvs sits directly
    # above the partition table at 0x9000 and holds the seed.
    elif l == "CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y":
        out.append("# CONFIG_BOOTLOADER_LOG_LEVEL_INFO is not set")
        out.append("CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y")
    elif l == "# CONFIG_BOOTLOADER_LOG_LEVEL_WARN is not set":
        continue                       # replaced above
    elif l.startswith("CONFIG_BOOTLOADER_LOG_LEVEL="):
        out.append("CONFIG_BOOTLOADER_LOG_LEVEL=2")
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

# SD firmware update: the release lane REQUIRES signed images, so a card cannot
# hand this device anything the project key did not sign. Appended rather than
# substituted because the dev sdkconfig has no line for any of them.
#
# BUILD_SIGNED_BINARIES stays OFF and the signing happens on the host below.
# The private key lives outside the repo and the container only ever sees the
# repo, so signing inside the container would mean mounting the key into it.
# The key does not go in a container.
out += [
    "",
    "# --- SD firmware update: signed images (tools/build_release.sh) ---",
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y",
    "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y",
    "CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME=y",
    "# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set",

]
open("sdkconfig.release", "w").write("\n".join(out) + "\n")
print("wrote sdkconfig.release (logs: WARN, signed-app verification ON)")
PY

# short commit from the HOST's git (the container can't read the bind-mounted
# repo's ownership); baked into the Settings/home build-identity line
# A bare short hash, NOT `git describe`: describe appends the last tag and the
# distance from it ("v0.1.0-beta5-5-g042befb-dirty", 29 chars) and the version
# it prints is already on this line from VERSION -- so describe spent most of
# its length disagreeing with the field next to it.
GIT_REV=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
git diff --quiet HEAD 2>/dev/null || GIT_REV="$GIT_REV-dirty"
echo "commit: $GIT_REV"

docker run --rm \
  -e GIT_CONFIG_COUNT=1 \
  -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project \
  -v "$PWD":/project -w /project "$KISS_IDF_IMAGE" \
  idf.py -B build-release -DSDKCONFIG=/project/sdkconfig.release -DKISS_RELEASE=1 \
  -DKISS_COMMIT="$GIT_REV" build

# ---- sign the app, on the HOST ----
# The signature block appended here is what the device checks an SD update
# against: esp_ota_end verifies the incoming image using the public key inside
# the RUNNING app's own block, so an unsigned release is a device that can
# never accept an update -- and an unsigned release that ships is a device that
# has to be recovered over USB to fix it. Hence the hard stop rather than a
# warning.
#
# KISS_UNSIGNED=1 stops before the signature and is the ONLY thing a machine
# without the key can do. It exists because the signature is what makes this
# build unreproducible: espsecure sign-data is ECDSA, so signing identical
# input twice gives different valid bytes, and the appended block lands in the
# very file the reproducible-build workflow publishes a hash of. Comparing
# hashes of signed images is not a weaker check, it is a meaningless one.
#
# So the two questions get separated. "Did this commit produce these bytes" is
# answered here, unsigned, by anyone. "Is this the firmware KISS published" is
# answered by the signature, made on a machine that holds the key, and by the
# signed hashes in SHA256SUMS beside the release.
# The version of esptool that gets handed the signing key, pinned.
#
# Every espsecure call below runs "uvx --from esptool", which resolved whatever
# PyPI served at that second and then had $KISS_OTA_KEY put on its command
# line. One bad esptool release -- a compromised maintainer account, a typo in
# the index, a yanked version replaced in place -- and the private half of the
# key this project's whole update story rests on walks off the machine, on a
# run nobody would think to audit because the build succeeded.
#
# A pin does not make the download trustworthy; it makes it the SAME download
# as last time, which is the property that lets a bad one be noticed at all.
# The flashing hints printed at the end stay unpinned on purpose: they talk to
# a board, never to a key, and a version baked into a line the reader copies by
# hand is a staleness problem with nothing to buy it.
ESPTOOL_PIN="${ESPTOOL_PIN:-esptool==5.3.1}"
KISS_OTA_KEY="${KISS_OTA_KEY:-$HOME/.kiss-signer/kiss_ota.pem}"
if [ -n "${KISS_UNSIGNED:-}" ]; then
  # A marker beside the image, not just a line of log nobody re-reads. Anything
  # that publishes or flashes from this directory can test for it.
  : > build-release/UNSIGNED
  echo
  echo "UNSIGNED build (KISS_UNSIGNED=1): reproducibility only."
  echo "      This image carries no signature block, so a device will refuse it"
  echo "      as an SD update and it must never be published as a release."
  echo "      Wrote build-release/UNSIGNED to say so."
elif [ ! -f "$KISS_OTA_KEY" ]; then
  echo
  echo "FAIL: OTA signing key not found at $KISS_OTA_KEY"
  echo "      Generate it once (docs/installer/SIGNING.md), or set KISS_OTA_KEY."
  echo "      Without it this build cannot accept SD firmware updates, ever."
  echo "      For a reproducibility check on a machine with no key, set"
  echo "      KISS_UNSIGNED=1 and compare the unsigned hashes."
  exit 1
else
echo "signing app with $KISS_OTA_KEY"
uvx --from "$ESPTOOL_PIN" espsecure sign-data \
  --version 2 --keyfile "$KISS_OTA_KEY" \
  --output build-release/guition_kiss_bringup-signed.bin \
  build-release/guition_kiss_bringup.bin
mv build-release/guition_kiss_bringup-signed.bin \
   build-release/guition_kiss_bringup.bin

# The public half in the repo has to be the half that just signed, or users
# verify against a key the firmware does not carry. Cheap to check, and the
# failure it prevents is silent.
uvx --from "$ESPTOOL_PIN" espsecure extract-public-key \
  --version 2 --keyfile "$KISS_OTA_KEY" /tmp/kiss_ota_pub_check.pem
if ! cmp -s /tmp/kiss_ota_pub_check.pem docs/installer/kiss_ota_pub.pem; then
  echo "FAIL: docs/installer/kiss_ota_pub.pem is not the public half of $KISS_OTA_KEY"
  exit 1
fi
echo "PASS: published public key matches the signing key"
rm -f /tmp/kiss_ota_pub_check.pem

# Prove the shipped file verifies against the PUBLISHED key, not just that the
# two halves match. This is the check a stranger can repeat, and it is the one
# that fails if signing was skipped, applied to the wrong file, or undone by a
# later step that rewrites the binary. The encrypted lane has run this since it
# existed; this lane published without it.
if ! uvx --from "$ESPTOOL_PIN" espsecure verify-signature \
     --version 2 --keyfile docs/installer/kiss_ota_pub.pem \
     build-release/guition_kiss_bringup.bin >/dev/null 2>&1; then
  echo "FAIL: build-release/guition_kiss_bringup.bin does not verify against"
  echo "      docs/installer/kiss_ota_pub.pem"
  exit 1
fi
echo "PASS: signed app verifies against the published public key"
fi

# ---- verify the release binary ----
# The version comes first and on its own, because it is the one fault that
# survives a green build: a build directory configured before VERSION last
# changed keeps declaring the old number, and a substring test cannot tell the
# difference between the version being present and the version being what the
# image actually claims. check_fw_version.py reads the app descriptor.
python3 tools/check_fw_version.py build-release || exit 1

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
# ONE copy is correct and required. kiss_seed_is_test_vector() carries the
# vector so the device can REFUSE it at restore, and kiss_seed.c says the owner
# asked for that in shipped firmware rather than behind a build flag, twice.
# The two that must not ship -- kiss_crypto.c's DEV_MNEMONIC and the bench's --
# are both behind KISS_RELEASE and both are out.
#
# So "is the string present" stopped being the question the moment the refusal
# landed, and this check went on asking it: it failed every release build, and
# because build_release.sh runs under set -e inside make_web_release.sh, it
# aborted the publish. A gate that cannot tell a seed from the guard against
# that seed is one that gets bypassed, and the bypass is what would actually
# ship a dev seed one day.
#
# Counted, not searched, and the count is the whole check: a SECOND literal is
# a second translation unit that got linked, which is exactly the regression
# the original grep was written to catch.
_TV = (b"abandon abandon abandon abandon abandon abandon "
       b"abandon abandon abandon abandon abandon about")
_n = blob.count(_TV)
if _n > 1:
    print(f"FAIL: {_n} copies of the dev mnemonic in the release binary "
          f"-- one is kiss_seed_is_test_vector, the rest are a leak"); fails += 1
elif _n == 1:
    print("PASS: one dev mnemonic, the restore refusal's own copy")
else:
    print("FAIL: the test vector is missing, so restore cannot refuse it")
    fails += 1
if b"KISS %s dev (%s)" in blob:
    print("FAIL: dev build banner found in release binary"); fails += 1
else:
    print("PASS: no dev banner in release binary")
# the version is checked against the app descriptor by check_fw_version.py,
# above, which is stronger than asking whether the string appears anywhere
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
echo "sha256 of what the commands below flash (release assets must match"
echo "docs/installer/SHA256SUMS):"
shasum -a 256 \
  build-release/bootloader/bootloader.bin \
  build-release/partition_table/partition-table.bin \
  build-release/guition_kiss_bringup.bin | sed 's/^/  /'
echo
echo "ESP-IDF flash (local ESP-IDF install; sdkconfig uses no-reset):"
echo "  idf.py -B build-release -p <port> flash"
echo
echo "ESP-IDF app-only reflash:"
echo "  idf.py -B build-release -p <port> app-flash"
echo
# Read out of flasher_args.json, never typed here. This block used to claim it
# came from flash_args and did not: enabling rollback added an ota_data
# partition at 0x10000 and moved the app to 0x20000, and these lines still said
# 0x10000 for the app. Anyone following them would have written the app over
# the slot the bootloader reads to decide which app to run, and got a board
# that does not come back.
APP_LINE=$(python3 - <<'PY'
import json
d = json.load(open("build-release/flasher_args.json"))["flash_files"]
off = next(o for o, f in d.items() if f.endswith("guition_kiss_bringup.bin"))
print(f"    {off} build-release/guition_kiss_bringup.bin")
PY
)
ALL_LINES=$(python3 - <<'PY'
import json
d = json.load(open("build-release/flasher_args.json"))["flash_files"]
items = sorted(d.items(), key=lambda kv: int(kv[0], 16))
for i, (off, f) in enumerate(items):
    tail = "" if i == len(items) - 1 else " \\"
    print(f"    {off:<8}build-release/{f}{tail}")
PY
)

echo "direct esptool fallback - app-only reflash:"
echo "  uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\"
echo "    write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\"
echo "$APP_LINE"
echo
echo "direct esptool fallback - full flash (fresh board, or whenever bootloader/partitions changed;"
echo "offsets from build-release/flasher_args.json - the encrypted-release lane"
echo "will need this full set):"
echo "  uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\"
echo "    write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\"
echo "$ALL_LINES"
echo
echo "then: unplug -> ~3s -> replug (v1.3 sample never boots off a USB reset)"
