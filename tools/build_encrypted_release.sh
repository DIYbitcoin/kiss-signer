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
#   DEVELOPMENT does NOT tighten the flash fuses. The bootloader logs "app is
#   configured for RELEASE but efuses are set for DEVELOPMENT / Device is not
#   secure" and runs anyway. It is not a no-op either, now that the release
#   recipe carries secure boot: its bootloader burns the key digest no matter
#   what the flash fuses say, leaving a board half locked. So the release
#   build never gets pointed at the rehearsal board at all - the burn goes to
#   a fresh dedicated board, and the rehearsal board stays what it is.
#
# UPDATE BUILD:  KISS_ENC_UPDATE=1 tools/build_encrypted_release.sh
#   -> build-encrypted-update/, the app alone, for boards already burned.
#
#   The release recipe has two lanes because they need different keys. The
#   burn signs the bootloader with all three keys of the root, and needs all
#   three on the machine. Every release afterwards only re-signs the APP,
#   which carries one key, and this lane asks for that one alone: it does not
#   sign a bootloader, does not print a flash or eFuse recipe, and deletes the
#   unsigned bootloader the build produced so nothing can be burned with it.
#
#   Until it existed, a routine update dragged the whole root onto the build
#   machine, which is the machine most likely to be compromised. The two
#   spares are the only thing that recovers a fleet whose everyday key leaked,
#   so they are worth nothing if they sit beside it. KISS_SB_KEY_INDEX picks
#   which key signs, so a rotation -- an update signed with key 1 that revokes
#   key 0 -- is something this recipe can actually perform.
#
# This is the step-8 hardening build: the release profile (KISS_RELEASE=1, dev
# seed compiled OUT) PLUS:
#   * flash encryption, RELEASE mode  - first boot burns the key into eFuse
#     (ONE WAY) and encrypts the whole flash in place
#   * NVS encryption                  - plain flash encryption does NOT cover
#     "nvs" data partitions, and the seed words live in NVS; NVS encryption
#     stores XTS keys in the new nvs_key partition, which IS flash-encrypted
#   * secure boot v2, RSA-3072, three keys - first boot ALSO burns the digests
#     of the three keys in the bootloader's signature sector, and from then on
#     the ROM only runs a bootloader those keys signed and the bootloader only
#     runs apps they signed. The keys are the BUILDER's own (KISS_SB_KEYS): a
#     burned board trusts whoever burned it and nobody else, and rotates by
#     revoking one key for the next. Release recipe only: the rehearsal board
#     must stay reflashable, and this burn is as one way as the other. The two
#     burns happen together in one first boot because a board burned for
#     encryption alone can never take the secure bootloader afterwards.
#
# THIS BUILD IS FOR A FRESH / FINAL BOARD ONLY. It never touches the v1.3
# engineering sample. This script only builds and verifies - it never flashes.
#
# What "release mode" means, so nobody is surprised later:
#   * the encryption key is generated ON the device and is unreadable forever
#   * after the first boot, serial reflash is IMPOSSIBLE, so the web installer
#     and the cable never work on that board again. JTAG is permanently
#     disabled by the secure boot burn, and ROM download mode drops to its
#     secure subset: enough to erase a board, never to read or reprogram it.
#   * firmware is NOT frozen: this table carries two app slots and an otadata,
#     and the board takes signed SD updates - now judged against the eFuse
#     digest, not merely the running app's own block. Anti rollback is armed at
#     secure version 0: nothing is refused today, but the first release that
#     bumps the version can permanently shut the door on the builds before it.
#   * an attacker with the board can erase it (denial of service) but can
#     never read the seed out of flash, and can never boot code its builder
#     did not sign
set -e
cd "$(dirname "$0")/.."
. tools/idf_image.sh

if [ -n "$KISS_ENC_REHEARSAL" ]; then
    RECIPE=rehearsal; BUILD_DIR=build-encrypted-rehearsal
    SDKCFG=sdkconfig.encrypted-rehearsal
else
    RECIPE=release;   BUILD_DIR=build-encrypted-release
    SDKCFG=sdkconfig.encrypted
fi

# Two lanes on the release recipe, because they need different keys.
#
# PROVISION is the burn: it signs the bootloader with all three keys of the
# root, so first boot burns three digests, and it prints the eFuse recipe. It
# happens once per board and it needs the whole root on the machine.
#
# UPDATE is every day after that: the board's trust is already burned, the
# bootloader can never be replaced, and an SD update is judged against those
# digests. All it needs is the one key the app carries. So this lane asks for
# that key alone, signs nothing but the app, and refuses to produce a
# bootloader or a burn recipe at all.
#
# The two spares are the only escape hatch if the everyday key leaks: a
# rotation is an update signed with the next key that revokes the one before
# it. An escape hatch that sits on the same disk as the key it replaces is not
# one, and until this split existed every routine build asked for all three.
if [ -n "${KISS_ENC_UPDATE:-}" ]; then
    if [ "$RECIPE" = rehearsal ]; then
        echo "FAIL: KISS_ENC_UPDATE is a release-recipe lane. The rehearsal"
        echo "      board has no secure boot and no root to rotate: its"
        echo "      updates are judged by the running app's own OTA block."
        exit 1
    fi
    LANE=update; BUILD_DIR=build-encrypted-update
else
    LANE=provision
fi
export RECIPE LANE BUILD_DIR SDKCFG
echo "recipe: $RECIPE ($LANE) -> $BUILD_DIR"

# The anti rollback counter, as a release INPUT rather than a literal in this
# file. It was pinned at 0, so no release could ever advance it: the machinery
# was armed and could not be used, and a bootloader under secure boot cannot be
# replaced to fix that later.
#
# What raising it costs. The eFuse counter moves the first time a higher
# version BOOTS, and it never moves back. Every earlier release is then refused
# by the bootloader, on every board that has run the new one -- including the
# owner's own, including the one they wanted to roll back to. The field is a
# fixed width, so there are only so many increments in a chip, ever.
#
# So it moves when a firmware version has a hole worth closing permanently,
# and on no other day. Raising it is a commit of its own, with a message
# saying which release it retires. See docs/specs/flash-encryption-rollout.md.
SECURE_VERSION="$(cat SECURE_VERSION 2>/dev/null | tr -d '[:space:]')"
case "$SECURE_VERSION" in
  ''|*[!0-9]*)
    echo "FAIL: SECURE_VERSION must be a non-negative whole number."
    echo "      Read: '$SECURE_VERSION'. The file is at the repo root."
    exit 1 ;;
esac
# It may not go DOWN. A lower number does not un-burn the counter on any board
# that already booted the higher one; it just builds firmware those boards
# refuse, and the first anyone hears of it is a device that will not take an
# update. Compared against the last release tag, which is the last number that
# could have reached a board.
PREV_TAG="$(git describe --tags --abbrev=0 --match 'v*' 2>/dev/null || echo)"
if [ -n "$PREV_TAG" ]; then
  PREV_SV="$(git show "$PREV_TAG:SECURE_VERSION" 2>/dev/null \
             | tr -d '[:space:]')"
  case "$PREV_SV" in ''|*[!0-9]*) PREV_SV=0 ;; esac
  if [ "$SECURE_VERSION" -lt "$PREV_SV" ]; then
    echo "FAIL: SECURE_VERSION went backwards: $PREV_SV at $PREV_TAG,"
    echo "      now $SECURE_VERSION. A board that booted $PREV_SV has burned"
    echo "      that counter and will refuse this build."
    exit 1
  fi
fi
export SECURE_VERSION
[ "$SECURE_VERSION" = 0 ] || \
  echo "anti rollback: secure version $SECURE_VERSION (permanent once booted)"

if [ -z "$ALLOW_DIRTY" ]; then
  DIRTY="$(git status --porcelain)"
  if [ -n "$DIRTY" ]; then
    echo "You have uncommitted changes - an encrypted release must be built"
    echo "from a clean, committed tree. Commit first, then rerun."
    echo "(throwaway test? prefix with ALLOW_DIRTY=1)"
    echo
    # Which files, not merely that there were some. CI runs this recipe
    # twice in one job, and the second run inherits whatever the first
    # left in the tree. A bare refusal there names nothing, so the only
    # record of the failure is a runner log saying the tree was dirty.
    echo "$DIRTY" | sed 's/^/    /'
    exit 1
  fi
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
    # XTS key size: AES-256. The old AES-128 line called itself deliberate and
    # deferred the real choice to "the later pass with secure boot" - this is
    # that pass, so it is decided: the P4 has the 256-bit XTS eFuse scheme and
    # key blocks to spare, the key is generated on-device either way, and the
    # first boot fixes the size forever. No board has been burned at 128.
    # Both recipes, so the rehearsal rehearses the size that ships.
    "CONFIG_SECURE_FLASH_ENCRYPTION_AES128":        None,
    "CONFIG_SECURE_FLASH_ENCRYPTION_AES256":        "y",
    # secure boot v2, release recipe only: a secure boot bootloader burns its
    # key digests on FIRST BOOT, so putting it in the rehearsal build would
    # spend the one thing that build exists to protect - a board that still
    # takes any image over the cable. Signing stays outside the container
    # (BUILD_SIGNED_BINARIES off, forced below): the bootloader is signed
    # after the build with all three of the builder's RSA-3072 keys and the
    # app with the first of them. The key block before the signing step says
    # why three and why the builder's.
    "CONFIG_SECURE_BOOT":                           None if rehearsal else "y",
    "CONFIG_SECURE_BOOT_V2_ENABLED":                None if rehearsal else "y",
    # idf.py drops the bootloader from its flash list the moment secure boot
    # is on, so that a board already burned is not handed a second bootloader
    # its digests may refuse. This recipe flashes a FRESH board exactly once,
    # and the recipe printed below is whatever the build says it writes --
    # which was three files, no bootloader, and a board that never boots. The
    # option puts it back; the flasher_args assertion below holds it there.
    "CONFIG_SECURE_BOOT_FLASH_BOOTLOADER_DEFAULT":  None if rehearsal else "y",
    "CONFIG_SECURE_BOOT_ECDSA_KEY_LEN_256_BITS":    "y",
    # ROM download mode after the burn: switched to the SECURE subset, not
    # disabled outright - secure mode still lets a stranger erase a board and
    # prove it erased, and closes flash reads and writes. Settled as a Stage 2
    # decision in docs/specs/flash-encryption-rollout.md.
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE":      None if rehearsal else "y",
    "CONFIG_SECURE_DISABLE_ROM_DL_MODE":            None,
    # anti rollback, armed at secure version 0. The eFuse counter only burns
    # when a HIGHER version boots, so nothing is spent and nothing is refused
    # today - but the check lives in the bootloader, and the release recipe's
    # bootloader can never be replaced, so the machinery has to be in it from
    # birth. The app side already exists: kiss_fw_mark_valid confirms a trial
    # slot only after the signer proves it can sign. Both recipes, same
    # fidelity argument as the key size.
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK":          "y",
    "CONFIG_BOOTLOADER_APP_SECURE_VERSION":
        os.environ["SECURE_VERSION"],
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
    # It is the REHEARSAL recipe's anchor only. On the release recipe the pair
    # is Kconfig-invalid (depends on !SECURE_BOOT) and the same verification
    # comes from secure boot itself, judged against the eFuse digest instead
    # of the running app - kiss_fw_available's CONFIG_SECURE_BOOT branch.
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT":     "y" if rehearsal else None,
    "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT": "y" if rehearsal else None,
    # The signature scheme is NOT a preference here, and forcing the one this
    # project's key uses does not work: on this chip, hardware secure boot with
    # ECDSA is errata'd. SECURE_BOOT_V2_ECDSA_INSECURE is default y for the P4
    # in IDF's own bootloader Kconfig -- "not functional for certain input
    # vectors" -- so SECURE_SIGNED_APPS_ECDSA_V2_SCHEME loses its depends and
    # the choice falls to RSA, whatever this list says. Reaching it needs
    # SECURE_BOOT_INSECURE plus SECURE_BOOT_V2_FORCE_ENABLE_ECDSA, which is a
    # known vulnerability turned on deliberately, on a board that can never be
    # reflashed. Not on a signing device.
    #
    # So: RSA-3072 where secure boot burns, ECDSA where it does not. The
    # rehearsal recipe has no hardware secure boot, its scheme is not gated,
    # and it keeps the secp256r1 key the SD update lane already publishes.
    "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME":         None if rehearsal else "y",
    "CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME":    "y" if rehearsal else None,
    "CONFIG_SECURE_BOOT_V2_FORCE_ENABLE_ECDSA":     None,
    "CONFIG_SECURE_BOOT_INSECURE":                  None,
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
print("wrote %s (flash enc %s + NVS enc%s, logs WARN)"
      % (os.environ["SDKCFG"], "DEVELOPMENT" if rehearsal else "RELEASE",
         "" if rehearsal else " + secure boot v2 + anti rollback"))
PY

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

# The key does not have to be a file: espsecure speaks PKCS#11, so the same
# secp256r1 key can live in a smartcard's signing slot and never exist on this
# machine. When this config is present it wins over KISS_OTA_KEY. It carries no
# "credentials" line on purpose, so the card PIN is prompted for rather than
# stored beside the thing it unlocks. tools/build_release.sh grew this first;
# docs/installer/SIGNING.md has the setup.
KISS_OTA_HSM_CONFIG="${KISS_OTA_HSM_CONFIG:-$HOME/.kiss-signer/hsm.ini}"

# The secure boot key can live on a card too, and the update lane is where that
# pays. Splitting the lanes took the two spares off the everyday machine; this
# takes the last one off it as well, so the key that signs every release exists
# only inside a device that will not export it and asks for a finger before it
# signs. A stolen laptop then buys an attacker nothing at all.
#
# A SEPARATE config from the OTA card above, even when it is the same YubiKey,
# because it is a different key in a different slot: secure boot on this chip is
# RSA-3072 and the OTA key is secp256r1. One ini names one slot.
#
# The burn lane ignores it, and says so: a bootloader has to be signed by all
# three keys at once, which is a thing only the machine holding the whole root
# can do.
KISS_SB_HSM_CONFIG="${KISS_SB_HSM_CONFIG:-$HOME/.kiss-signer/sb_hsm.ini}"

if [ -f "$KISS_OTA_HSM_CONFIG" ] || [ -f "$KISS_SB_HSM_CONFIG" ]; then
  ESPSECURE=(uvx --from "$ESPTOOL_PIN" --with python-pkcs11 espsecure)
else
  ESPSECURE=(uvx --from "$ESPTOOL_PIN" espsecure)
fi
if [ -f "$KISS_OTA_HSM_CONFIG" ]; then
  OTA_SIGN_KEY=(--hsm --hsm-config "$KISS_OTA_HSM_CONFIG")
  OTA_KEY_DESC="the card described by $KISS_OTA_HSM_CONFIG"
else
  OTA_SIGN_KEY=(--keyfile "$KISS_OTA_KEY")
  OTA_KEY_DESC="$KISS_OTA_KEY"
fi

# ---- the secure boot root: three RSA-3072 keys, the builder's own ----
#
# Hardware secure boot on this chip is RSA-3072 only (see the scheme block
# above), so the key that signs a burned board's bootloader and app is not the
# secp256r1 OTA key, and it is not the project's either: whoever runs this
# recipe mints their own root, and the board trusts nothing else, forever.
# That is the DIY answer to "who is the signer for", settled in Stage 2 of
# docs/specs/flash-encryption-rollout.md, and it is why no RSA public key is
# published anywhere. A burned board's SD update is its builder's own output
# of this recipe, judged against the digests its first boot burned.
#
# Three keys, not one, and all three on the BOOTLOADER. First boot burns the
# digest of every key in the bootloader's signature sector and REVOKES every
# slot it did not fill; IDF leaves a slot open only behind SECURE_BOOT_INSECURE,
# which this recipe asserts off. A bootloader signed with one key is a board
# that can never rotate, and three is all the chip holds. The app carries key
# 0 alone, the current key. Rotation later is an update signed with key 1
# that revokes key 0 (esp_ota_revoke_secure_boot_public_key), so a leaked key
# costs a rotation rather than the fleet.
#
# The variable that used to sit here, KISS_SB_RSA_KEY, bypassed the UNSIGNED
# guard and did nothing else: with it set, both images were signed with the
# OTA key while the built config said RSA, and every verify below printed
# PASS, because each checks the key that signed and none asks what the
# firmware expects. It is refused now, and tools/check_sig_scheme.py asks the
# missing question after every signature.
KISS_SB_DIR="${KISS_SB_DIR:-$HOME/.kiss-signer/sb}"
KISS_SB_KEYS="${KISS_SB_KEYS:-$KISS_SB_DIR/kiss_sb_0.pem $KISS_SB_DIR/kiss_sb_1.pem $KISS_SB_DIR/kiss_sb_2.pem}"
if [ -n "${KISS_SB_RSA_KEY:-}" ]; then
  echo "FAIL: KISS_SB_RSA_KEY is gone. It never signed anything: it only let"
  echo "      the OTA key sign an RSA recipe. A root is three RSA-3072 keys,"
  echo "      named in KISS_SB_KEYS (space separated) or at the default paths:"
  echo "        $KISS_SB_KEYS"
  exit 1
fi
# Which key of the root signs the app. Zero on a burn, and zero on every
# update until the day key 0 is retired: a rotation is an update signed with
# key 1 that revokes key 0, and after it every later update is signed with key
# 1. So the index is an input rather than a literal, or the rotation this root
# was shaped for could be described and never performed.
#
# It is not a free choice on a burn. The app a board is born with has to be
# signed with the key the fleet calls key 0, because a rotation revokes by slot
# and the slots come from the order the bootloader was signed in.
KISS_SB_KEY_INDEX="${KISS_SB_KEY_INDEX:-0}"
case "$KISS_SB_KEY_INDEX" in
  0|1|2) ;;
  *) echo "FAIL: KISS_SB_KEY_INDEX is '$KISS_SB_KEY_INDEX'; the chip holds"
     echo "      three keys, so it is 0, 1 or 2."
     exit 1 ;;
esac
if [ "$LANE" = provision ] && [ "$KISS_SB_KEY_INDEX" != 0 ]; then
  echo "FAIL: a burn signs the app with key 0, not key $KISS_SB_KEY_INDEX. Every"
  echo "      board of this root numbers its slots by the order the bootloader"
  echo "      was signed in, and a rotation revokes a slot by number. A board"
  echo "      born on key $KISS_SB_KEY_INDEX would spend the key that is meant to save it."
  exit 1
fi

SB_KEYS=()
if [ "$RECIPE" = release ]; then
  read -r -a SB_KEYS <<< "$KISS_SB_KEYS"
  SB_PRESENT=0
  for k in "${SB_KEYS[@]}"; do
    [ -f "$k" ] && SB_PRESENT=$((SB_PRESENT + 1))
  done
  if [ "${#SB_KEYS[@]}" -ne 3 ]; then
    echo "FAIL: KISS_SB_KEYS names ${#SB_KEYS[@]} key(s); a root is three."
    echo "      All three are named even on the update lane, which needs only"
    echo "      one of them present: the names are what fix the slot order."
    exit 1
  fi
  SB_ACTIVE="${SB_KEYS[$KISS_SB_KEY_INDEX]}"
fi

# The update lane: one key, and the absence of the other two is the point.
if [ "$LANE" = update ]; then
  # The public half, beside the private one and named after it. Written by the
  # burn, secret from nobody, and the only thing that lets a machine holding no
  # private key at all check what it just signed: with the key on a card, this
  # file is what the signature is verified against, and its fingerprint is what
  # the root record is held to. Without it a card could sign with anything and
  # nothing here would know.
  SB_PUB_RECORDED="${SB_ACTIVE%.pem}.pub.pem"
  if [ -f "$KISS_SB_HSM_CONFIG" ]; then
    SB_ON_CARD=1
    if [ ! -f "$SB_PUB_RECORDED" ]; then
      echo "FAIL: signing from the card needs the public half of key $KISS_SB_KEY_INDEX on disk:"
      echo "        $SB_PUB_RECORDED"
      echo "      It is not a secret and the burn writes it. Copy it from the"
      echo "      machine that minted the root, beside root.txt."
      exit 1
    fi
    echo "signing key: the card described by $KISS_SB_HSM_CONFIG"
  elif [ ! -f "$SB_ACTIVE" ] && [ "$SB_PRESENT" -eq 0 ]; then
    KISS_UNSIGNED=1
    echo
    echo "NOTE: no secure boot key here, so this build is UNSIGNED."
    echo "      Reproducibility only: no board will install it."
  elif [ ! -f "$SB_ACTIVE" ]; then
    echo "FAIL: the update key for index $KISS_SB_KEY_INDEX is not on this machine:"
    echo "        $SB_ACTIVE"
    echo "      Other keys of the root are here, but signing with one of those"
    echo "      is a rotation, not an update, and it is not what was asked for."
    echo "      Set KISS_SB_KEY_INDEX to the key this fleet is running on."
    exit 1
  fi
  # What counts as a key that should not be here depends on where the signing
  # key is. Off a card, the everyday key is meant to be on disk and only the
  # two spares are out of place. On a card, all three are: the whole point of
  # moving it there is that no private half of this root is readable here.
  SB_SPARES=0
  for i in 0 1 2; do
    if [ -z "${SB_ON_CARD:-}" ] && [ "$i" = "$KISS_SB_KEY_INDEX" ]; then continue; fi
    [ -f "${SB_KEYS[$i]}" ] && SB_SPARES=$((SB_SPARES + 1))
  done
  if [ "$SB_SPARES" -gt 0 ]; then
    echo
    echo "NOTE: $SB_SPARES key file(s) of this root are on this machine."
    echo "      This lane does not read them, and the reason it exists is that"
    echo "      they should not be here: they are what recovers the fleet if"
    echo "      the everyday key walks off it. Move them offline."
    if [ -n "${SB_ON_CARD:-}" ]; then
      echo "      The signing key is on the card now, so the file it was"
      echo "      imported from is a copy that can still be stolen. Once the"
      echo "      card has signed a release, that copy belongs offline with"
      echo "      the spares, not here."
    fi
  fi
elif [ "$RECIPE" = release ]; then
  if [ -f "$KISS_SB_HSM_CONFIG" ]; then
    echo
    echo "NOTE: a secure boot card is configured and the burn does not use it."
    echo "      One signature sector carries all three keys, so only a machine"
    echo "      holding the whole root can sign a bootloader. The card signs"
    echo "      updates, which is where the everyday risk lives."
  fi
  if [ "$SB_PRESENT" -eq 0 ]; then
    KISS_UNSIGNED=1
    echo
    echo "NOTE: no secure boot root, so this build is UNSIGNED."
    echo "      Secure boot v2 on this target is RSA-3072 only, and the root is"
    echo "      the builder's own. Mint it once, keep it the way the OTA key is"
    echo "      kept, and never lose it: every board burned with it is frozen"
    echo "      on its last firmware if you do."
    echo "        mkdir -p $KISS_SB_DIR && for i in 0 1 2; do"
    echo "          uvx --from $ESPTOOL_PIN espsecure generate-signing-key \\"
    echo "            --version 2 --scheme rsa3072 $KISS_SB_DIR/kiss_sb_\$i.pem"
    echo "        done"
    echo "      Nothing this run produces may be flashed."
    echo
    echo "      The rehearsal recipe needs no root and is the lane to use:"
    echo "        KISS_ENC_REHEARSAL=1 bash tools/build_encrypted_release.sh"
  elif [ "$SB_PRESENT" -ne 3 ]; then
    echo "FAIL: a partial root, $SB_PRESENT of 3 keys present. A board burned"
    echo "      with fewer than three digests can never rotate. Missing:"
    for k in "${SB_KEYS[@]}"; do [ -f "$k" ] || echo "        $k"; done
    echo "      If the spares are offline on purpose, this is the wrong lane:"
    echo "      a burn needs the whole root, an update needs one key."
    echo "        KISS_ENC_UPDATE=1 bash tools/build_encrypted_release.sh"
    exit 1
  fi
fi

# ---- the root, settled before anything is compiled ----
#
# The public halves come out here and the slot order is held against the
# record here, before a single object file is built. Both used to sit beside
# the signature, at the far end of a five minute compile, and what they catch
# is a custody mistake: the wrong key in a slot, a restored file, a root that
# is not the root these boards answer to. That is the class of error worth
# hearing about in seconds, not after the build it invalidates.
SIGTMP=$(mktemp -d)
trap 'rm -rf "$SIGTMP"' EXIT
if [ "$RECIPE" = release ] && [ -z "${KISS_UNSIGNED:-}" ]; then
  # The provision lane holds the whole root and extracts all three public
  # halves; the update lane holds one key on purpose and extracts only that
  # one. Asking for the others here is what used to drag the spares onto the
  # build machine on every routine release.
  if [ "$LANE" = provision ]; then
    for i in 0 1 2; do
      "${ESPSECURE[@]}" extract-public-key --version 2 \
        --keyfile "${SB_KEYS[$i]}" "$SIGTMP/sb_pub$i.pem" >/dev/null
      # Kept beside the private key, once. A machine that signs from a card
      # holds no private half to derive this from, and espsecure will not read
      # a public key back off the card, so without this file a card-signed
      # image could not be checked against the root at all. Public halves are
      # not secret: the fleet's boards carry their digests in fuses.
      sb_pub_out="${SB_KEYS[$i]%.pem}.pub.pem"
      if [ ! -f "$sb_pub_out" ]; then
        cp "$SIGTMP/sb_pub$i.pem" "$sb_pub_out"
        echo "wrote $sb_pub_out"
      elif ! cmp -s "$SIGTMP/sb_pub$i.pem" "$sb_pub_out"; then
        echo "FAIL: $sb_pub_out is not the public half of ${SB_KEYS[$i]}."
        echo "      One of the two was replaced. Sort out custody before"
        echo "      building: this file is what an update machine trusts."
        exit 1
      fi
    done
  elif [ -n "${SB_ON_CARD:-}" ]; then
    # No private key here at all. The recorded public half stands in for it,
    # and the verify below is what proves the card holds its private twin.
    cp "$SB_PUB_RECORDED" "$SIGTMP/sb_pub$KISS_SB_KEY_INDEX.pem"
  else
    "${ESPSECURE[@]}" extract-public-key --version 2 \
      --keyfile "$SB_ACTIVE" "$SIGTMP/sb_pub$KISS_SB_KEY_INDEX.pem" >/dev/null
  fi

  # ---- the root's fixed identities, in slot order ----
  #
  # A rotation revokes a slot by NUMBER, and the numbers were fixed forever the
  # moment a board's first boot burned the bootloader's signature sector in the
  # order it was signed in. Nothing in this recipe used to record that order:
  # the keys were three paths on one disk, and a renamed, restored or
  # regenerated file could quietly change which key the fleet calls key 1
  # without a single check noticing.
  #
  # So the order is written down once, as fingerprints of the public halves,
  # beside the root. The provision lane refuses to burn a root that disagrees
  # with a recorded one; the update lane refuses to sign as key N with a key
  # the record does not call key N. That is the whole guard: a fingerprint file
  # is not a secret and does not need to travel with the private keys.
  SB_ROOT_MANIFEST="${KISS_SB_ROOT_MANIFEST:-$KISS_SB_DIR/root.txt}"
  SB_FP=$(SIGTMP="$SIGTMP" LANE="$LANE" IDX="$KISS_SB_KEY_INDEX" python3 - <<'PY'
import hashlib, os
sig, lane, idx = os.environ["SIGTMP"], os.environ["LANE"], os.environ["IDX"]
want = [0, 1, 2] if lane == "provision" else [int(idx)]
for i in want:
    d = hashlib.sha256(open(f"{sig}/sb_pub{i}.pem", "rb").read()).hexdigest()
    print(f"{i} {d}")
PY
)
  if [ -f "$SB_ROOT_MANIFEST" ]; then
    while read -r idx fp; do
      case "$idx" in ''|'#'*) continue ;; esac
      rec=$(awk -v i="$idx" '$1 == i { print $2 }' "$SB_ROOT_MANIFEST")
      if [ -z "$rec" ]; then
        echo "FAIL: $SB_ROOT_MANIFEST records no key for slot $idx. The record"
        echo "      is what makes a slot number mean anything; a partial one"
        echo "      cannot be trusted to say which key is which."
        exit 1
      elif [ "$rec" != "$fp" ]; then
        echo "FAIL: the key at slot $idx is not the key this root recorded."
        echo "        recorded: $rec"
        echo "        this key: $fp"
        echo "      Either the key files moved around or this is a different"
        echo "      root. A rotation revokes a slot by number, so signing as"
        echo "      the wrong number spends the wrong key. Sort out custody"
        echo "      before building; the record is at $SB_ROOT_MANIFEST."
        echo "      A genuinely NEW root belongs in a directory of its own:"
        echo "      point KISS_SB_DIR at it rather than editing this record,"
        echo "      which the boards of the old root still answer to."
        exit 1
      fi
    done <<< "$SB_FP"
    echo "PASS: key $KISS_SB_KEY_INDEX is the key $SB_ROOT_MANIFEST calls key $KISS_SB_KEY_INDEX"
  elif [ "$LANE" = provision ]; then
    { echo "# kiss-signer secure boot root: sha256 of each public half, by"
      echo "# slot. Written at the first burn and never edited afterwards."
      echo "# Not a secret. Keep a copy wherever the update key is used."
      printf '%s\n' "$SB_FP"
    } > "$SB_ROOT_MANIFEST"
    echo "wrote $SB_ROOT_MANIFEST (slot order of this root, recorded once)"
  else
    echo
    echo "NOTE: no root record at $SB_ROOT_MANIFEST, so nothing here can check"
    echo "      that this key really is key $KISS_SB_KEY_INDEX of the root the boards burned."
    echo "      Copy it from the machine that ran the burn. It is not a secret."
  fi
fi

GIT_REV=$(git describe --always --dirty 2>/dev/null || echo nogit)
echo "commit: $GIT_REV"

# The images this recipe signs must be the images this build produced. idf.py
# writes each .bin from its ELF under a timestamp target and leaves it alone
# while the ELF is unchanged, which on a rerun it is: last run's SIGNED
# bootloader sat in the build directory and would have been signed a second
# time, one sector hidden behind another. Dropping the timestamps makes ninja
# regenerate both from the ELF; the --unsigned check before each signature is
# the proof that it did.
rm -f "$BUILD_DIR/.bin_timestamp" "$BUILD_DIR/bootloader/.bin_timestamp" \
  2>/dev/null || true

docker run --rm \
  -e GIT_CONFIG_COUNT=1 \
  -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project \
  -v "$PWD":/project -w /project "$KISS_IDF_IMAGE" \
  idf.py -B "$BUILD_DIR" -DSDKCONFIG="/project/$SDKCFG" \
  -DKISS_RELEASE=1 -DKISS_COMMIT="$GIT_REV" build

# ---- sign, outside the container: the keys were settled above the build ----

if [ -n "${KISS_UNSIGNED:-}" ]; then
  # Through the helper, for the reason it gives; see tools/idf_image.sh.
  kiss_mark_unsigned "$BUILD_DIR"
  # Here as well as on the signed path, so the update lane's promise holds
  # whatever happened above: that directory never contains a bootloader.
  if [ "$LANE" = update ]; then kiss_drop_update_bootloader "$BUILD_DIR"; fi
  echo
  echo "UNSIGNED build (KISS_UNSIGNED=1): reproducibility only."
  echo "      No signature block, so this image must never be flashed to a board"
  echo "      whose fuses this recipe burns -- it could never be updated after."
  echo "      Wrote $BUILD_DIR/UNSIGNED to say so."
elif [ "$RECIPE" = rehearsal ] && [ ! -f "$KISS_OTA_HSM_CONFIG" ] \
     && [ ! -f "$KISS_OTA_KEY" ]; then
  echo
  echo "FAIL: no OTA signing key. Looked for a card config at"
  echo "      $KISS_OTA_HSM_CONFIG and a key file at $KISS_OTA_KEY."
  echo "      Set one up once (docs/installer/SIGNING.md), or set KISS_OTA_KEY."
  echo "      Without it this board can never accept an SD firmware update, and"
  echo "      the release recipe burns the fuses that would let you reflash it."
  echo "      For a reproducibility check on a machine with no key, set"
  echo "      KISS_UNSIGNED=1 and compare the unsigned hashes."
  exit 1
else
# Which key signs the app is the lane's decision, made once here. A rehearsal
# app is judged by the running app's own block, so it carries the OTA key the
# SD update lane publishes. A release app is judged against the eFuse digests,
# so it carries the builder's key 0.
# A marker from an earlier unsigned run must not outlive the signed image.
rm -f "$BUILD_DIR/UNSIGNED" 2>/dev/null || true
if [ "$RECIPE" = release ]; then
  if [ -n "${SB_ON_CARD:-}" ]; then
    APP_SIGN_KEY=(--hsm --hsm-config "$KISS_SB_HSM_CONFIG")
    APP_KEY_DESC="the card described by $KISS_SB_HSM_CONFIG (secure boot key $KISS_SB_KEY_INDEX of 3)"
  else
    APP_SIGN_KEY=(--keyfile "$SB_ACTIVE")
    APP_KEY_DESC="$SB_ACTIVE (secure boot key $KISS_SB_KEY_INDEX of 3)"
  fi
  EXPECT_SCHEME=rsa
  # The public halves, beside the image. Nothing on the device needs them;
  # they say which key is which when key 0 is rotated out years from now.
  cp "$SIGTMP"/sb_pub?.pem "$BUILD_DIR"/
else
  APP_SIGN_KEY=("${OTA_SIGN_KEY[@]}")
  APP_KEY_DESC="$OTA_KEY_DESC"
  EXPECT_SCHEME=ecdsa
fi

# Never sign an image twice. idf.py regenerates the .bin only when the ELF
# changed, so a second run on the same build directory would sign last run's
# SIGNED image and hide one signature sector behind another.
python3 tools/check_sig_scheme.py "$BUILD_DIR/guition_kiss_bringup.bin" \
  --unsigned

echo "signing app with $APP_KEY_DESC"
# Without this line the build simply stops for a minute and then times out,
# with nothing on screen to say the card is waiting on a finger.
[ -n "${SB_ON_CARD:-}" ] && echo "      TOUCH THE CARD when it blinks - it will not sign until you do"
"${ESPSECURE[@]}" sign-data \
  --version 2 "${APP_SIGN_KEY[@]}" \
  --output "$BUILD_DIR/guition_kiss_bringup-signed.bin" \
  "$BUILD_DIR/guition_kiss_bringup.bin"
mv "$BUILD_DIR/guition_kiss_bringup-signed.bin" \
   "$BUILD_DIR/guition_kiss_bringup.bin"

# The question none of the verifies below ask: is this the block the FIRMWARE
# expects? Each of them checks the image against the key that signed it, and
# each passed on an ECDSA block over an RSA config. This reads the block.
python3 tools/check_sig_scheme.py "$BUILD_DIR/guition_kiss_bringup.bin" \
  --scheme "$EXPECT_SCHEME" --blocks 1

if [ "$RECIPE" = release ]; then
  if ! "${ESPSECURE[@]}" verify-signature \
       --version 2 --keyfile "$SIGTMP/sb_pub$KISS_SB_KEY_INDEX.pem" \
       "$BUILD_DIR/guition_kiss_bringup.bin" >/dev/null 2>&1; then
    echo "FAIL: $BUILD_DIR/guition_kiss_bringup.bin does not verify against"
    echo "      secure boot key $KISS_SB_KEY_INDEX"
    exit 1
  fi
  echo "PASS: signed app verifies against secure boot key $KISS_SB_KEY_INDEX"
else

# The public half in the repo has to be the half that just signed, or a
# verifier checks this build against a key the firmware does not carry.
#
# Only the file lane can ask that of the key itself: a card will not hand over
# a private key to derive a public half from. Nothing is lost -- the verify
# below proves the signature checks out under the PUBLISHED key, which is the
# same claim made against the shipped bytes.
if [ "${OTA_SIGN_KEY[0]}" = "--keyfile" ]; then
  "${ESPSECURE[@]}" extract-public-key \
    --version 2 --keyfile "$KISS_OTA_KEY" "$SIGTMP/kiss_ota_pub_enc_check.pem"
  if ! cmp -s "$SIGTMP/kiss_ota_pub_enc_check.pem" docs/installer/kiss_ota_pub.pem; then
    echo "FAIL: docs/installer/kiss_ota_pub.pem is not the public half of $KISS_OTA_KEY"
    exit 1
  fi
  echo "PASS: published public key matches the signing key"
fi

# Prove the shipped file verifies against the PUBLISHED key, not just that the
# two halves match. This is the check a stranger can repeat, and it is the one
# that fails if signing was skipped, applied to the wrong file, or undone by a
# later step that rewrites the binary.
if ! "${ESPSECURE[@]}" verify-signature \
     --version 2 --keyfile docs/installer/kiss_ota_pub.pem \
     "$BUILD_DIR/guition_kiss_bringup.bin" >/dev/null 2>&1; then
  echo "FAIL: $BUILD_DIR/guition_kiss_bringup.bin does not verify against"
  echo "      docs/installer/kiss_ota_pub.pem"
  exit 1
fi
echo "PASS: signed app verifies against the published public key"
fi

# ---- and the post quantum signature, on the same image ----
#
# This lane's app can reach a card as an SD update the same way the plain lane's
# can, and a device built from either refuses an image with no trailer. Skipping
# it here would produce a correctly signed, correctly hashed image that says
# "the post quantum signature is missing or wrong" on every board it is offered
# to -- a readable refusal, which is the only reason this is a note in a script
# and not a brick.
#
# After the ECDSA verify, never before: espsecure reads the file as a whole and
# the trailer is not part of the image it signed. See docs/installer/SIGNING.md.
KISS_PQ_KEY="${KISS_PQ_KEY:-$HOME/.kiss-signer/pq_release.key}"
if [ ! -f "$KISS_PQ_KEY" ]; then
  echo
  echo "FAIL: post quantum release key not found at $KISS_PQ_KEY"
  echo "      Mint it once (docs/installer/SIGNING.md). Without it this image"
  echo "      carries one signature of the two a device asks for, and no board"
  echo "      will install it from a card."
  exit 1
fi
bash sim/build_pqtool.sh >/dev/null
PQ_TOOL="${KISS_SIM_TMP:-/tmp}/pq_tool"
if ! diff -q <("$PQ_TOOL" header "$KISS_PQ_KEY") main/pq_release_pubkey.h >/dev/null; then
  echo "FAIL: main/pq_release_pubkey.h is not the public half of $KISS_PQ_KEY"
  echo "      Regenerate it and rebuild:"
  echo "        $PQ_TOOL header $KISS_PQ_KEY > main/pq_release_pubkey.h"
  exit 1
fi
"$PQ_TOOL" sign "$KISS_PQ_KEY" "$BUILD_DIR/guition_kiss_bringup.bin"
if ! "$PQ_TOOL" verify "$KISS_PQ_KEY" "$BUILD_DIR/guition_kiss_bringup.bin"; then
  echo "FAIL: the image does not verify against its own post quantum signature"
  exit 1
fi
echo "PASS: signed app carries a post quantum signature that verifies"

# ---- and the bootloader, on the release recipe only ----
#
# Secure boot's first boot burns the digest of EVERY key in the BOOTLOADER's
# signature sector, revokes the slots it did not fill, and from then on
# refuses any bootloader and any app none of those keys signed - an unsigned
# bootloader here would not boot even once. So the bootloader carries all
# three keys of the root, and the app carries key 0: the app can be re-signed
# with an update, the bootloader never can, and the sector it is burned with
# is the whole rotation budget for the life of the board. The rehearsal recipe
# skips this block entirely: its board has no secure boot, and its bootloader
# stays byte-identical to what that build has always flashed.
#
# Signed in place, before the flash recipe below reads flasher_args.json, so
# the offsets and the hashes describe the bytes that actually get flashed.
#
# The provision lane only. An update lane that signed a bootloader would be
# holding the one artifact this split exists to keep out of the everyday
# environment, and a bootloader signed with the ONE key it has is worse than
# no bootloader: burn a fresh board with it and first boot burns that single
# digest, revokes the other two slots, and produces a board that can never be
# rotated for the rest of its life. So the update lane does not sign it, and
# does not leave it lying in the build directory to be flashed by hand either.
if [ "$RECIPE" = release ] && [ "$LANE" = provision ]; then
  python3 tools/check_sig_scheme.py "$BUILD_DIR/bootloader/bootloader.bin" \
    --unsigned
  echo "signing bootloader with all three secure boot keys"
  "${ESPSECURE[@]}" sign-data \
    --version 2 \
    --keyfile "${SB_KEYS[0]}" --keyfile "${SB_KEYS[1]}" --keyfile "${SB_KEYS[2]}" \
    --output "$BUILD_DIR/bootloader/bootloader-signed.bin" \
    "$BUILD_DIR/bootloader/bootloader.bin"
  mv "$BUILD_DIR/bootloader/bootloader-signed.bin" \
     "$BUILD_DIR/bootloader/bootloader.bin"
  # Three RSA blocks in the bytes, carrying three DIFFERENT keys, or the
  # spec's rotation promise is prose. Counting them is not enough: one key
  # file passed three times produces three valid blocks, three valid
  # signatures, and a board that burns one digest, revokes the other two
  # slots on first boot, and can never be rotated. --distinct reads the
  # modulus out of each block and refuses a repeat.
  python3 tools/check_sig_scheme.py "$BUILD_DIR/bootloader/bootloader.bin" \
    --scheme rsa --blocks 3 --distinct
  for i in 0 1 2; do
    if ! "${ESPSECURE[@]}" verify-signature \
         --version 2 --keyfile "$SIGTMP/sb_pub$i.pem" \
         "$BUILD_DIR/bootloader/bootloader.bin" >/dev/null 2>&1; then
      echo "FAIL: the signed bootloader does not verify against secure boot key $i"
      exit 1
    fi
  done
  # The signature block sits on the end, the bootloader is flashed at 0x2000
  # and the partition table at 0x10000: a signed bootloader that outgrew
  # those 57344 bytes would be flashed over its own partition table.
  BL_SIZE=$(wc -c < "$BUILD_DIR/bootloader/bootloader.bin" | tr -d ' ')
  if [ "$BL_SIZE" -gt 57344 ]; then
    echo "FAIL: signed bootloader is $BL_SIZE bytes; 57344 is the ceiling"
    exit 1
  fi
  echo "PASS: signed bootloader verifies against all three keys ($BL_SIZE bytes)"
elif [ "$LANE" = update ]; then
  kiss_drop_update_bootloader "$BUILD_DIR"
fi
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
    # ONE copy of the published all-abandon vector ships, deliberately:
    # kiss_seed_is_test_vector compares against it so a restore of the wallet
    # the whole internet can spend from is recognised and marked. The owner
    # asked for that in shipped firmware, twice. What must NOT ship is dev seed
    # material on a live path -- kiss_crypto.c's DEV_MNEMONIC behind
    # #ifndef KISS_RELEASE, and kiss_cryptobench.c whole. So this counts rather
    # than forbids: two copies means one of those came back.
    (blob.count(b"abandon abandon abandon abandon abandon abandon "
                b"abandon abandon abandon abandon abandon about") == 1,
     "exactly one test vector (the restore comparator), no dev seed"),
    (b"kissbench" not in blob,           "crypto bench compiled out"),
    (b"KISS %s dev (%s)" not in blob,    "no dev banner in binary"),
    (open("VERSION").read().strip().encode() in blob, "version string present"),
]

cfg = open(os.environ["SDKCFG"]).read().splitlines()
def on(k):  return f"{k}=y" in cfg
def val(k, default=""):
    for line in cfg:
        if line.startswith(k + "="):
            return line.split("=", 1)[1].strip('"')
    return default
# Read back from the generated config, not from the environment: the point of
# an assertion is to check what the build did, not to repeat what it was told.
secure_version = val("CONFIG_BOOTLOADER_APP_SECURE_VERSION", "?")
# the two builds assert OPPOSITE things here on purpose: a rehearsal build that
# quietly came out in RELEASE mode would burn the board it exists to protect
checks += [
    (on("CONFIG_SECURE_FLASH_ENC_ENABLED"),             "flash encryption enabled"),
    (on("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE") is not rehearsal,
     "flash encryption DEVELOPMENT mode" if rehearsal else "flash encryption RELEASE mode"),
    (on("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT") is rehearsal,
     "release mode off" if rehearsal else "development mode off"),
    (on("CONFIG_NVS_ENCRYPTION"),                       "NVS encryption enabled"),
    (on("CONFIG_SECURE_FLASH_ENCRYPTION_AES256") and
     not on("CONFIG_SECURE_FLASH_ENCRYPTION_AES128"),   "XTS AES-256 (settled at the secure boot pass)"),
    (on("CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC"),  "NVS keys via flash-enc scheme (nvs_key partition)"),
    # opposite on purpose, same argument as the flash-encryption mode above: a
    # rehearsal build that quietly gained secure boot would burn the digest on
    # the board that exists to stay reflashable
    (on("CONFIG_SECURE_BOOT") is not rehearsal,
     "secure boot off (rehearsal stays reflashable)" if rehearsal
     else "secure boot v2 ON (burns with the flash key)"),
    (rehearsal or on("CONFIG_SECURE_BOOT_V2_ENABLED"),  "secure boot is the v2 scheme"),
    (on("CONFIG_ESPTOOLPY_NO_STUB"),                    "esptool no-stub mode (required with flash encryption)"),
    (on("CONFIG_APP_REPRODUCIBLE_BUILD"),               "reproducible build (no compile date embedded)"),
    # An update lane is only allowed to exist if the images it accepts are
    # checked. These assert the answer this lane gives to "updatable or
    # frozen": updatable, and only for an image signed with our key. The
    # rehearsal anchors in the running app's own signature block; the release
    # recipe anchors in the eFuse digest secure boot burns.
    (on("CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT") if rehearsal
     else on("CONFIG_SECURE_SIGNED_ON_UPDATE"),
     "SD update images are signature verified"),
    # Both halves, and opposite per recipe. "ECDSA is on" was the whole
    # assertion, it was TRUE in the force list, and the config that built said
    # RSA -- which is how an image signed with one scheme nearly went onto a
    # board expecting the other.
    (on("CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME") is rehearsal and
     on("CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME") is not rehearsal,
     "signature scheme ECDSA v2 (no secure boot)" if rehearsal
     else "signature scheme RSA-3072 (ECDSA secure boot is errata'd here)"),
    (not on("CONFIG_SECURE_BOOT_V2_FORCE_ENABLE_ECDSA"),
     "the errata'd ECDSA secure boot is not force enabled"),
    (not on("CONFIG_SECURE_BOOT_INSECURE"),             "no insecure options"),
    # The three-key root only exists if the slots it fills are the only
    # slots: IDF revokes every unused digest slot on first boot unless this
    # INSECURE-menu option holds them open, and an open slot is one a stranger
    # with the board can fill. check_sig_scheme.py proves the bootloader
    # carries three blocks; this proves nothing is left writable beside them.
    (not on("CONFIG_SECURE_BOOT_ALLOW_UNUSED_DIGEST_SLOTS"),
     "no digest slot left open (three burned, none writable)"),
    # Armed. At version 0 it burns nothing and refuses nothing; the old
    # comment here said arming it early would freeze the fleet on an
    # unfinished security model, and the model this pass ships is the
    # finished one. A bootloader that can never be replaced either carries
    # the check from birth or never gets it.
    #
    # The label reads the number the build actually used. It used to say
    # "version 0" in prose while the config said whatever it said, which is
    # the same class of defect as a docs page naming a release by hand.
    (on("CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK"),
     "anti rollback armed (secure version %s)" % secure_version),
    (secure_version == os.environ["SECURE_VERSION"],
     "secure version is the one SECURE_VERSION asked for"),
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

# The flash list the recipe prints is read from the build, so it has to be the
# whole board: bootloader at 0x2000, table, otadata, app. Secure boot silently
# dropped the bootloader from it (see the force list) and the recipe printed
# three files as if they were all of them.
import json
fa = json.load(open(f"{bdir}/flasher_args.json"))["flash_files"]
names = {f.rsplit("/", 1)[-1] for f in fa.values()}
checks += [
    (fa.get("0x2000", "").endswith("bootloader.bin"),
     "flash recipe carries the bootloader at 0x2000"),
    ({"partition-table.bin", "ota_data_initial.bin",
      "guition_kiss_bringup.bin"} <= names,
     "flash recipe carries the table, otadata and the app"),
]

# ...and on the update lane, that the bootloader the recipe describes is not
# actually sitting there. The list above is read out of the build config and
# still names all four files, which is right: it is the same board and the
# same layout. What must not exist is the FILE, because the only bootloader
# this lane could produce is one that would burn a fresh board into a state
# with no rotation left. The check is on the bytes, not on the intention.
if os.environ.get("LANE") == "update":
    checks += [
        (not os.path.exists(f"{bdir}/bootloader/bootloader.bin"),
         "no bootloader binary in an update build (nothing here can sign one)"),
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

# rng provenance gate: same rule as build_release.sh. The map says which
# object each seed-path RNG call lands in; tools/check_rng_provenance.py
# holds the rules and the account.
python3 tools/check_rng_provenance.py --selftest
python3 tools/check_rng_provenance.py "$BUILD_DIR/guition_kiss_bringup.map"

# flash budget: baked art is ~75% of the binary; fail while there is still
# headroom to react, not on the flash step (set -e stops on a FAIL)
python3 tools/check_flash_budget.py \
  "$BUILD_DIR/guition_kiss_bringup.bin" partitions_encrypted.csv

# ---- the update lane stops here: one file, no flash recipe, no fuses ----
#
# Everything below this point describes erasing a board, writing four files to
# it over a cable and burning eFuses that never come back. None of it applies
# to an update: the board it is for burned its fuses long ago and refuses the
# cable outright. Printing a burn recipe beside an image that cannot be burned
# with is how the wrong one gets followed at two in the morning, so the lane
# that cannot burn does not print one.
if [ "$LANE" = update ]; then
  kiss_drop_update_bootloader "$BUILD_DIR"
  UPDATE_APP="$BUILD_DIR/guition_kiss_bringup.bin"
  UPDATE_SHA=$(python3 -c 'import hashlib,sys
print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$UPDATE_APP")
  if [ -n "${KISS_UNSIGNED:-}" ]; then
    echo
    echo "unsigned UPDATE build: hash comparison only, and no board will take it."
    echo "  $UPDATE_SHA  $(basename "$UPDATE_APP")"
    exit 0
  fi
  cat <<EOF

encrypted UPDATE build OK: $BUILD_DIR/

################################################################################
#  ONE FILE, FOR BOARDS THIS ROOT ALREADY BURNED
#
#  * Signed with secure boot key $KISS_SB_KEY_INDEX and with the post quantum release key.
#    Nothing else on this machine was needed, and the other two keys of the
#    root were not read: that is the point of this lane.
#  * There is no bootloader and no eFuse recipe here. A burned board never
#    replaces its bootloader, and this machine could not sign one that was
#    safe to burn even if it tried.
#  * To bring up a NEW board you need the whole root and the other lane:
#      bash tools/build_encrypted_release.sh
################################################################################

1. copy the image to a FAT card. The name does not matter: the device reads
   the version out of every image it finds and offers the newest.
     $UPDATE_APP

   sha256 of the file, so you can prove the card got the same bytes:
     $UPDATE_SHA

   It will NOT match the hash CI publishes, and nothing is wrong when it does
   not: CI hashes an UNSIGNED build, and RSA signing puts fresh random bytes
   in the file every run, so no two signed builds of one commit hash alike.
   The reproducible comparison is its own build, from the same commit:
     KISS_UNSIGNED=1 KISS_ENC_UPDATE=1 bash tools/build_encrypted_release.sh

2. take the update from the device's own firmware update screen, then let it
   boot and prove itself. The new image only becomes permanent after the
   signer has signed with it once; a board that cannot boot the update rolls
   back to the slot it came from.

3. only boards burned with THIS root will take it. The beta's published image
   and anything signed with another key are refused, on the eFuse digests.
EOF
  exit 0
fi

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

# The comment above the signing step promises the recipe is suppressed for an
# unsigned build, and until now it was not: everything below describes flashing
# and fuse-burning a board with an image no card can ever update. Stop here,
# plainly, before either recipe prints.
if [ -n "${KISS_UNSIGNED:-}" ]; then
  echo
  echo "unsigned build: no flash or eFuse recipe. This image is for hash"
  echo "comparison only and must never be burned onto a board."
  exit 0
fi

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
#  * Settings will report encryption ON, in amber, and the build id stays
#    amber: DEVELOPMENT mode is not locked and must not look like it is.
#    Calm is the release burn alone (kiss_seed_flash_lock_state).
################################################################################

0. read the fuses FIRST. Erasing proves nothing about them: a board erases
   fine with its cable already locked. This must print PASS:
   python3 tools/check_efuse_fresh.py --reflashable -p <port>

1. erase the board:
   uvx esptool --chip esp32p4 -p <port> erase-flash

2. flash (same shifted offsets and --no-stub as the release build):
   uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\
     --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\
$FLASH_LINES

   sha256 of those $N_FILES files, so you can prove the board got these bytes:
$SHA_LINES

   The app's hash will NOT match the one CI publishes. CI hashes an UNSIGNED
   build and signing writes fresh random bytes into the file every run, so no
   two signed builds of one commit hash alike. Unsigned is where the two are
   comparable, and it is its own build:
     KISS_UNSIGNED=1 KISS_ENC_REHEARSAL=1 bash tools/build_encrypted_release.sh

3. unplug -> ~3s -> replug, WAIT for the menu, then run the wallet for real:
   create, lock, unlock, sign, wipe. Reflash and repeat as needed.

4. This board never takes the release build. It used to be the tightening
   path; the release recipe now carries secure boot, and its bootloader burns
   the key digest on first boot no matter what the flash fuses say - flashing
   it here would leave a board half locked: digest burned, flash fuses still
   DEVELOPMENT, fully neither. When the rehearsal has proven the app, spend
   the fresh dedicated board on the release recipe and keep this one as what
   it is. (The old flash-only tighten, esp_flash_encryption_set_release_mode()
   from the app, still does not exist in KISS and now never needs to.)
EOF
else
cat <<EOF

encrypted release build OK: $BUILD_DIR/

################################################################################
#  READ BEFORE FLASHING - THIS IS A ONE-WAY OPERATION
#
#  * FRESH / FINAL BOARD ONLY. Never the v1.3 engineering sample, and never
#    the rehearsal board.
#  * First boot burns the flash-encryption eFuse key AND the secure boot key
#    digest: PERMANENT. From then on the ROM runs only our signed bootloader,
#    the bootloader runs only our signed apps, and JTAG is gone.
#  * After first boot this board can NEVER be serial-reflashed again. The web
#    installer and the cable will never work on it again. That is the point.
#  * If all three secure boot keys are ever lost, every board burned with them
#    is frozen on its last firmware forever. Custody first, burn second.
#  * Firmware is still UPDATABLE, over SD, for images signed with key 0 of
#    this root. The app this run produced IS that update, for every board
#    burned with these keys; nothing published for the beta will install.
#    This banner used to say frozen; the table has carried two app slots and an
#    otadata since the SD update work landed, and this is the line an operator
#    reads immediately before a burn they cannot undo.
#  * First boot encrypts ~6MB of flash in place: it can take a few minutes
#    on a black screen. DO NOT UNPLUG until the game menu appears.
#    Losing power mid-encryption can brick the board.
################################################################################

0. prove the board is fresh. erase-flash does NOT: it succeeds on a board
   whose fuses are already burned, and the rehearsal board would pass it.
   Only the eFuse block knows, so this must print PASS before anything else:
   python3 tools/check_efuse_fresh.py --fresh -p <port>

1. erase the board (wipes any factory demo):
   uvx esptool --chip esp32p4 -p <port> erase-flash

2. one full plaintext flash (first boot encrypts it in place; note the
   encrypted build's SHIFTED offsets - table 0x10000, app 0x20000 - and
   --no-stub, which flash-encrypted builds require):
   uvx esptool --chip esp32p4 -p <port> -b 460800 --before default-reset --after no-reset \\
     --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \\
$FLASH_LINES

   sha256 of those $N_FILES files. The flash is one way, so read them BEFORE
   step 2, not after, and prove the board got these exact bytes:
$SHA_LINES

   Two of those four are signed - the app and the bootloader - and their
   hashes will NOT match the ones CI publishes. That is not a fault: CI
   hashes an UNSIGNED build, and RSA signing writes fresh random bytes into
   the file on every run, so no two signed builds of one commit hash alike.
   The table and the otadata carry no signature and must match exactly.
   To check the build itself against CI, build the same commit unsigned:
     KISS_UNSIGNED=1 bash tools/build_encrypted_release.sh

3. unplug -> ~3s -> replug, then WAIT (see warning above).
   When Settings shows encryption ON and calm, not amber, the fuses say
   RELEASE mode and secure boot are both live - only then create the
   wallet. ON in amber is a board that is encrypted but not locked.
EOF
fi
