#!/bin/bash
# ENCRYPTED-release build -> build-encrypted-release/  (dev + release builds untouched).
#
# REHEARSAL BUILD:  KISS_ENC_REHEARSAL=1 tools/build_encrypted_release.sh
#   -> build-encrypted-rehearsal/, flash encryption in DEVELOPMENT mode.
#
#   Same encryption, same NVS keys, same partition table, but the eFuse that
#   blocks plaintext serial flashing is NOT burned, so the board can be
#   reflashed over USB as many times as you like while the wallet is exercised
#   for real against encrypted flash. Spend a board on the release build only
#   after this one has been through the whole app.
#
#   Verified against IDF 6.0.1 (components/bootloader_support/src/flash_encrypt.c):
#   flashing a RELEASE-configured build onto a board already fused for
#   DEVELOPMENT does NOT tighten it. The bootloader logs "app is configured for
#   RELEASE but efuses are set for DEVELOPMENT / Device is not secure" and runs
#   anyway. The only real upgrade is esp_flash_encryption_set_release_mode()
#   called from the app, which burns CRYPT_CNT to full and write-protects it,
#   burns DIS_DOWNLOAD_MANUAL_ENCRYPT (+ SPI_DOWNLOAD_MSPI_DIS and
#   DIS_DOWNLOAD_ICACHE where the target has them), switches ROM download to
#   secure mode, and aborts if the readback still says DEVELOPMENT.
#
# This is the step-8 hardening build: the release profile (KISS_RELEASE=1, dev
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
#   * after the first boot, serial reflash is IMPOSSIBLE, so the web installer
#     and the cable never work on that board again
#   * firmware is NOT frozen: this table carries two app slots and an otadata,
#     and the board takes signed SD updates checked against the key in the
#     running app. The assertions below are what hold that to signed images
#     only. (An older SIGNED build still installs: anti rollback is deliberately
#     off until the secure boot pass.)
#   * an attacker with the board can erase it (denial of service) but can
#     never read the seed out of flash
set -e
cd "$(dirname "$0")/.."

if [ -n "$KISS_ENC_REHEARSAL" ]; then
    RECIPE=rehearsal; BUILD_DIR=build-encrypted-rehearsal
    SDKCFG=sdkconfig.encrypted-rehearsal
else
    RECIPE=release;   BUILD_DIR=build-encrypted-release
    SDKCFG=sdkconfig.encrypted
fi
export RECIPE BUILD_DIR SDKCFG
echo "recipe: $RECIPE -> $BUILD_DIR"

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
import os
rehearsal = os.environ["RECIPE"] == "rehearsal"
force = {
    # hardening
    "CONFIG_SECURE_FLASH_ENC_ENABLED":              "y",
    # the ONE difference between the two: DEVELOPMENT leaves
    # DIS_DOWNLOAD_MANUAL_ENCRYPT unburned, so the board still takes a
    # plaintext serial flash and the bootloader re-encrypts it each boot
    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE":  None if rehearsal else "y",
    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT": "y" if rehearsal else None,
    "CONFIG_NVS_ENCRYPTION":                        "y",
    # P4 defaults the NVS key-protection choice to the HMAC scheme (needs a
    # pre-burned eFuse key block); we want the flash-encryption scheme: XTS
    # keys auto-generated on first use into the nvs_key partition
    "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC":   "y",
    "CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC":        None,
    # esptool talks to the ROM loader instead of uploading its stub. The
    # verify block below has always asserted this, but nothing set it and
    # SECURE_FLASH_ENC_ENABLED does not select it (esptool_py Kconfig:
    # default y only under IDF_ENV_FPGA/BRINGUP). It was passing on a stale
    # generated sdkconfig.encrypted. Force it, in both, so the flash
    # command printed at the end matches the config that built.
    "CONFIG_ESPTOOLPY_NO_STUB":                     "y",
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
    # SD firmware update: verify the signature on an incoming image. The plain
    # release lane has set these since it gained the update path; this lane,
    # the one that runs on boards holding funds, did not - so on an encrypted
    # build kiss_fw_available() answered WFW_ERR_UNSIGNED and the device
    # refused every update, including ours. The partition table here has
    # carried two app slots and an otadata since the SD update work landed, so
    # the layout always said updatable while the app said frozen.
    #
    # NO_SECURE_BOOT is the honest name: this verifies an image before it is
    # written, using the public key in the running app's own signature block.
    # It does NOT verify the bootloader and it does NOT stop a downgrade to an
    # older SIGNED build - CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK stays off, and
    # secure boot is the later pass this script already asserts is absent.
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT":     "y",
    "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT": "y",
    "CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME":    "y",
    "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES":     None,

    # The bootloader's own logs, quieted here for ROOM rather than for quiet.
    # It is flashed at 0x2000 with the partition table at 0x8000, so its
    # ceiling is a hard 24576 bytes and the dev build sits at 23200 of them.
    # Secure boot is the pass this script already asserts is absent, and its
    # signature verification does not fit in the 1344 bytes left. INFO to WARN
    # measures 23200 -> 20656: 6% free becomes 16%.
    #
    # The shared sdkconfig keeps INFO. Bootloader INFO lines are how a boot
    # failure on this board is read -- the Boya auto-suspend brick was found
    # through them -- and the budget only binds the lanes that will carry
    # secure boot. Moving the partition table to widen it is not an option:
    # nvs sits directly above it at 0x9000, holding the seed.
    "CONFIG_BOOTLOADER_LOG_LEVEL_INFO":             None,
    "CONFIG_BOOTLOADER_LOG_LEVEL_WARN":             "y",
    "CONFIG_BOOTLOADER_LOG_LEVEL":                  "2",
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
    if key in seen:
        continue
    # A forced None has to write the explicit "is not set" line even when the
    # base sdkconfig never mentioned the symbol. Skipping it leaves the symbol
    # absent, and an absent symbol takes its Kconfig default: turning signed
    # apps on brought SECURE_BOOT_BUILD_SIGNED_BINARIES back as y, which then
    # demanded a signing key inside the build container. The key does not go in
    # a container - images are signed outside it, same as the plain release
    # lane - so this has to say off out loud.
    out.append(f"# {key} is not set" if v is None else f"{key}={v}")
open(os.environ["SDKCFG"], "w").write("\n".join(out) + "\n")
print("wrote %s (flash enc %s + NVS enc, logs WARN)"
      % (os.environ["SDKCFG"], "DEVELOPMENT" if rehearsal else "RELEASE"))
PY

GIT_REV=$(git describe --always --dirty 2>/dev/null || echo nogit)
echo "commit: $GIT_REV"

docker run --rm \
  -e GIT_CONFIG_COUNT=1 \
  -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project \
  -v "$PWD":/project -w /project espressif/idf:v6.0.1 \
  idf.py -B "$BUILD_DIR" -DSDKCONFIG="/project/$SDKCFG" \
  -DKISS_RELEASE=1 -DKISS_COMMIT="$GIT_REV" build

# ---- sign the app, outside the container ----
# Same key and same step as the plain release lane, and for the same reason
# stated there: esp_ota_end verifies an incoming image against the public key
# carried in the RUNNING app's own signature block. An app with no block has no
# key, so it can never accept an update -- and on the release recipe the board
# has burned its fuses and cannot be serially reflashed either, which makes an
# unsigned encrypted release a funded board that can never be fixed. This lane
# had no signing step at all. Hence a hard stop rather than a warning.
#
# Before the checks below, not after: signing appends a block, so the size the
# flash budget measures and the hashes the recipe prints have to be the ones
# from the file that actually gets flashed.
#
# KISS_UNSIGNED=1 stops before the signature, and is the only thing a machine
# without the key can do. The signature is what makes this build unreproducible:
# espsecure sign-data is ECDSA, so signing identical input twice gives different
# valid bytes, in the very file a reproducibility check hashes. See the longer
# note in tools/build_release.sh.
#
# On this lane it also means the fuse recipe below is describing an image no
# board should ever be burned with, so the recipe is suppressed too.
KISS_OTA_KEY="${KISS_OTA_KEY:-$HOME/.kiss-signer/kiss_ota.pem}"
if [ -n "${KISS_UNSIGNED:-}" ]; then
  : > "$BUILD_DIR/UNSIGNED"
  echo
  echo "UNSIGNED build (KISS_UNSIGNED=1): reproducibility only."
  echo "      No signature block, so this image must never be flashed to a board"
  echo "      whose fuses this recipe burns -- it could never be updated after."
  echo "      Wrote $BUILD_DIR/UNSIGNED to say so."
elif [ ! -f "$KISS_OTA_KEY" ]; then
  echo
  echo "FAIL: OTA signing key not found at $KISS_OTA_KEY"
  echo "      Generate it once (docs/installer/SIGNING.md), or set KISS_OTA_KEY."
  echo "      Without it this board can never accept an SD firmware update, and"
  echo "      the release recipe burns the fuses that would let you reflash it."
  echo "      For a reproducibility check on a machine with no key, set"
  echo "      KISS_UNSIGNED=1 and compare the unsigned hashes."
  exit 1
else
echo "signing app with $KISS_OTA_KEY"
uvx --from esptool espsecure sign-data \
  --version 2 --keyfile "$KISS_OTA_KEY" \
  --output "$BUILD_DIR/guition_kiss_bringup-signed.bin" \
  "$BUILD_DIR/guition_kiss_bringup.bin"
mv "$BUILD_DIR/guition_kiss_bringup-signed.bin" \
   "$BUILD_DIR/guition_kiss_bringup.bin"

# The public half in the repo has to be the half that just signed, or a
# verifier checks this build against a key the firmware does not carry.
uvx --from esptool espsecure extract-public-key \
  --version 2 --keyfile "$KISS_OTA_KEY" /tmp/kiss_ota_pub_enc_check.pem
if ! cmp -s /tmp/kiss_ota_pub_enc_check.pem docs/installer/kiss_ota_pub.pem; then
  echo "FAIL: docs/installer/kiss_ota_pub.pem is not the public half of $KISS_OTA_KEY"
  rm -f /tmp/kiss_ota_pub_enc_check.pem
  exit 1
fi
echo "PASS: published public key matches the signing key"
rm -f /tmp/kiss_ota_pub_enc_check.pem

# Prove the shipped file verifies against the PUBLISHED key, not just that the
# two halves match. This is the check a stranger can repeat, and it is the one
# that fails if signing was skipped, applied to the wrong file, or undone by a
# later step that rewrites the binary.
if ! uvx --from esptool espsecure verify-signature \
     --version 2 --keyfile docs/installer/kiss_ota_pub.pem \
     "$BUILD_DIR/guition_kiss_bringup.bin" >/dev/null 2>&1; then
  echo "FAIL: $BUILD_DIR/guition_kiss_bringup.bin does not verify against"
  echo "      docs/installer/kiss_ota_pub.pem"
  exit 1
fi
echo "PASS: signed app verifies against the published public key"
fi

# ---- verify: binary contents AND the security config that actually built ----
GIT_REV="$GIT_REV" python3 - <<'PY'
import os, sys
fails = 0
bdir = os.environ["BUILD_DIR"]
rehearsal = os.environ["RECIPE"] == "rehearsal"

blob = open(f"{bdir}/guition_kiss_bringup.bin", "rb").read()
rev = os.environ.get("GIT_REV", "").encode()
checks = [
    (bool(rev) and rev in blob,          f"commit {rev.decode()} present"),
    (b"abandon abandon" not in blob,     "no dev mnemonic in binary"),
    (b"KISS %s dev (%s)" not in blob,    "no dev banner in binary"),
    (open("VERSION").read().strip().encode() in blob, "version string present"),
]

cfg = open(os.environ["SDKCFG"]).read().splitlines()
def on(k):  return f"{k}=y" in cfg
# the two builds assert OPPOSITE things here on purpose: a rehearsal build that
# quietly came out in RELEASE mode would burn the board it exists to protect
checks += [
    (on("CONFIG_SECURE_FLASH_ENC_ENABLED"),             "flash encryption enabled"),
    (on("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE") is not rehearsal,
     "flash encryption DEVELOPMENT mode" if rehearsal else "flash encryption RELEASE mode"),
    (on("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT") is rehearsal,
     "release mode off" if rehearsal else "development mode off"),
    (on("CONFIG_NVS_ENCRYPTION"),                       "NVS encryption enabled"),
    (on("CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC"),  "NVS keys via flash-enc scheme (nvs_key partition)"),
    (not on("CONFIG_SECURE_BOOT"),                      "secure boot off (own later pass)"),
    (on("CONFIG_ESPTOOLPY_NO_STUB"),                    "esptool no-stub mode (required with flash encryption)"),
    (on("CONFIG_APP_REPRODUCIBLE_BUILD"),               "reproducible build (no compile date embedded)"),
    # An update lane is only allowed to exist if the images it accepts are
    # checked. These two assert the answer this lane gives to "updatable or
    # frozen": updatable, and only for an image signed with our key.
    (on("CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT"),
     "SD update images are signature verified"),
    (on("CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME"),   "signature scheme ECDSA v2"),
    # Deliberately NOT asserted on: anti rollback burns an eFuse and cannot be
    # undone, and doing that before secure boot lands would freeze the fleet on
    # an unfinished security model. An older SIGNED build is installable today;
    # that is a known, accepted gap and it goes away with the secure boot pass.
]

pt = open(f"{bdir}/partition_table/partition-table.bin", "rb").read()
# This lane used to assert a factory partition, i.e. one frozen image and no
# update path at all. The SD update work replaced that with two app slots and
# an otadata, and the assert kept demanding the old shape - so the check failed
# on a perfectly good build and no encrypted release could be cut. It now
# asserts the layout the table actually has, and the app actually uses.
checks += [
    (b"nvs_key" in pt, "nvs_key (NVS XTS key) partition present"),
    (b"ota_0" in pt and b"ota_1" in pt, "two app slots present (SD update lane)"),
    (b"otadata" in pt, "otadata present (rollback needs it)"),
    (b"factory" not in pt, "no factory partition (slots are the boot path)"),
]

# no-wireless gate: the board's C6 radio chip is held in reset and nothing
# may talk to it, so the ELF must link ZERO objects from any radio/network
# library (linker map = what the binary actually contains).
import re
mapf = open(f"{bdir}/guition_kiss_bringup.map").read()
linked = []
for lib in ("libesp_wifi", "libesp_wifi_remote", "libesp_hosted", "libbt.",
            "libwpa_supplicant", "liblwip", "libesp_netif", "libopenthread",
            "libieee802154", "libesp_phy", "libesp_coex"):
    n = len(re.findall(re.escape(lib) + r"[^\s(]*\(", mapf))
    if n: linked.append(f"{lib}:{n}")
checks += [
    (b"C6 radio held in reset" in blob, "C6 radio-hold code present"),
    (not linked, "no wireless/network stack linked (linker map)"
                 + ("" if not linked else ": " + " ".join(linked))),
]

for ok, label in checks:
    print(("PASS: " if ok else "FAIL: ") + label)
    fails += 0 if ok else 1
print(f"encrypted {os.environ['RECIPE']} app: {len(blob)} bytes")
sys.exit(1 if fails else 0)
PY

# flash budget: baked art is ~75% of the binary; fail while there is still
# headroom to react, not on the flash step (set -e stops on a FAIL)
python3 tools/check_flash_budget.py \
  "$BUILD_DIR/guition_kiss_bringup.bin" partitions_encrypted.csv

# hashed here, printed inside the flash recipes below: the flash is one way,
# so the compare against the reproducible build CI output has to happen with
# the hash and the command in the same place
# The write-flash argument list and its hashes, read out of the build rather
# than typed into the recipes below. They used to be three files named by hand,
# and enabling rollback added an otadata partition this table already carried a
# slot for -- so the recipes were about to send someone to erase a board, flash
# three of the four files it needs, and find out on a chip that has already
# encrypted itself one way. Whatever the build says it writes is what the
# recipe says to write.
FLASH_LINES=$(BUILD_DIR="$BUILD_DIR" python3 - <<'PY'
import json, os
b = os.environ["BUILD_DIR"]
d = json.load(open(f"{b}/flasher_args.json"))["flash_files"]
items = sorted(d.items(), key=lambda kv: int(kv[0], 16))
for i, (off, f) in enumerate(items):
    tail = "" if i == len(items) - 1 else " \\\\"
    print(f"     {off:<8}{b}/{f}{tail}")
PY
)
SHA_LINES=$(BUILD_DIR="$BUILD_DIR" python3 - <<'PY'
import hashlib, json, os
b = os.environ["BUILD_DIR"]
d = json.load(open(f"{b}/flasher_args.json"))["flash_files"]
for off, f in sorted(d.items(), key=lambda kv: int(kv[0], 16)):
    h = hashlib.sha256(open(f"{b}/{f}", "rb").read()).hexdigest()
    print(f"     {h}  {f.rsplit('/', 1)[-1]}")
PY
)
N_FILES=$(printf '%s\n' "$FLASH_LINES" | wc -l | tr -d ' ')

if [ "$RECIPE" = rehearsal ]; then
cat <<EOF

encrypted REHEARSAL build OK: $BUILD_DIR/

################################################################################
#  REHEARSAL BUILD - encrypts the board, does NOT lock it shut
#
#  * First boot still burns the flash-encryption key: PERMANENT. The board is
#    encrypted from here on and can never go back to plain flash.
#  * What it does NOT burn is DIS_DOWNLOAD_MANUAL_ENCRYPT, so you CAN keep
#    reflashing this board over USB. That is the whole point of this build.
#  * First boot encrypts ~6MB in place: minutes on a black screen.
#    DO NOT UNPLUG until the game menu appears.
#  * Settings will report encryption ENABLED. The build id stays amber,
#    because DEVELOPMENT mode is not "secure" and must not look like it is.
################################################################################

1. erase the board:
   uvx esptool --chip esp32p4 -p <port> erase-flash

2. flash (same shifted offsets and --no-stub as the release build):
   uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\
     --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\
$FLASH_LINES

   sha256 of those $N_FILES files (compare with the reproducible build run in CI):
$SHA_LINES

3. unplug -> ~3s -> replug, WAIT for the menu, then run the wallet for real:
   create, lock, unlock, sign, wipe. Reflash and repeat as needed.

4. ONLY when this build has been through everything, tighten the SAME board.
   Reflashing the release build does NOT do it: verified in IDF 6.0.1, the
   bootloader logs "app is configured for RELEASE but efuses are set for
   DEVELOPMENT / Device is not secure" and boots anyway. The upgrade is
   esp_flash_encryption_set_release_mode() called once from the app, which
   maxes and write-protects CRYPT_CNT, burns DIS_DOWNLOAD_MANUAL_ENCRYPT,
   SPI_DOWNLOAD_MSPI_DIS and DIS_DOWNLOAD_ICACHE, and switches ROM download
   to secure mode. That call does not exist in KISS yet.
EOF
else
cat <<EOF

encrypted release build OK: $BUILD_DIR/

################################################################################
#  READ BEFORE FLASHING - THIS IS A ONE-WAY OPERATION
#
#  * FRESH / FINAL BOARD ONLY. Never the v1.3 engineering sample.
#  * First boot burns the flash-encryption eFuse key: PERMANENT.
#  * After first boot this board can NEVER be serial-reflashed again. The web
#    installer and the cable will never work on it again. That is the point.
#  * Firmware is still UPDATABLE, over SD, for images signed with our key.
#    This banner used to say frozen; the table has carried two app slots and an
#    otadata since the SD update work landed, and this is the line an operator
#    reads immediately before a burn they cannot undo.
#  * First boot encrypts ~6MB of flash in place: it can take a few minutes
#    on a black screen. DO NOT UNPLUG until the game menu appears.
#    Losing power mid-encryption can brick the board.
################################################################################

1. erase the fresh board (proves it is fresh, wipes any factory demo):
   uvx esptool --chip esp32p4 -p <port> erase-flash

2. one full plaintext flash (first boot encrypts it in place; note the
   encrypted build's SHIFTED offsets - table 0x10000, app 0x20000 - and
   --no-stub, which flash-encrypted builds require):
   uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\
     --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\
$FLASH_LINES

   sha256 of those $N_FILES files. The flash is one way, so hold them against
   the reproducible build run in CI BEFORE step 2, not after:
$SHA_LINES

3. unplug -> ~3s -> replug, then WAIT (see warning above).
   When Settings shows "flash encryption: ENABLED" (calm, not amber),
   the eFuse says encryption is live - only then create the wallet.
EOF
fi
