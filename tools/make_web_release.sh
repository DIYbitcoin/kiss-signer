#!/bin/bash
# Build signed release artifacts into docs/installer/ for staging:
#   * firmware/kiss-signer-<version>.bin                (merged, offset 0, USB)
#   * firmware/kiss-signer-<version>-update.bin         (signed app, SD card)
#   * SHA256SUMS + SHA256SUMS.asc                       (GPG, if a key exists)
#   * firmware/kiss-signer-<version>.bin.minisig        (minisign, if key exists)
#   * manifest.json / release.json                      (rewritten in place)
#
# Signing, the typical bitcoin-project way (see docs/installer/SIGNING.md):
#   * GPG (primary, community convention): detached armor signature over the
#     SHA256SUMS manifest. Uses your default key, or GPG_KEY_ID if set.
#   * minisign (optional extra): signature over the merged firmware image.
# Secret keys stay on the maintainer machine; only public keys enter the repo.
# Without keys the release is emitted unsigned and labeled so.
set -e
cd "$(dirname "$0")/.."

MINISIGN_KEY="${MINISIGN_KEY:-$HOME/.kiss-signer/minisign.key}"
PUBKEY_FILE="docs/installer/kiss_signer.pub"
GPG_PUB_FILE="docs/installer/kiss_signer_pgp.asc"
# The interpreter must have esptool, because step 2 below merges the image with
# it. Testing -x only asked "does this file run", which a venv that exists but
# never had esptool installed passes -- so the release got all the way through a
# full Docker rebuild before dying on "No module named esptool". Test the thing
# actually needed instead, and fall through to anything that has it.
PY="${PY:-/tmp/spritevenv/bin/python}"
for cand in "$PY" /tmp/kissvenv/bin/python python3; do
    if [ -x "$cand" ] || command -v "$cand" >/dev/null 2>&1; then
        if "$cand" -m esptool version >/dev/null 2>&1; then PY="$cand"; break; fi
    fi
done
if ! "$PY" -m esptool version >/dev/null 2>&1; then
    echo "No Python with esptool found (tried $PY, /tmp/kissvenv/bin/python, python3)."
    echo "Install it:  python3 -m pip install esptool"
    exit 1
fi

# Detached armor signature over $1, self checked against the public key that is
# in the repo. Returns 1 when there is no secret key at all, so the caller can
# emit an honestly labelled unsigned release. Exits when a key exists but the
# signature fails or does not verify: a release that claims a signature it does
# not have is worse than one that admits it has none. Two callers now, the
# SHA256SUMS manifest and the offline installer zip.
gpg_sign() {
    target="$1"
    command -v gpg >/dev/null || return 1
    gpg --list-secret-keys ${GPG_KEY_ID:+"$GPG_KEY_ID"} >/dev/null 2>&1 || return 1

    # NOT --batch: batch mode suppresses the pinentry passphrase popup, so once
    # the agent cache expires the sign fails silently ("No such file or
    # directory"). Interactive lets pinentry prompt; --yes still auto-overwrites.
    rm -f "$target.asc"
    if ! gpg --yes ${GPG_KEY_ID:+-u "$GPG_KEY_ID"} --armor \
           --detach-sign -o "$target.asc" "$target"; then
        echo "ERROR: GPG signing failed on $target (passphrase prompt?). Run in a"
        echo "terminal with: export GPG_TTY=\$(tty)   then rerun. Nothing was released."
        exit 1
    fi
    # self-check: the signature we just wrote must verify against the PUBLIC key
    # in the repo (not just the local keyring), or the release is not "signed".
    if [ ! -f "$GPG_PUB_FILE" ]; then
        echo "ERROR: $GPG_PUB_FILE missing - export it first:"
        echo "  gpg --armor --export ${GPG_KEY_ID:-<your key id>} > $GPG_PUB_FILE"
        exit 1
    fi
    VERIFY_RING="$(mktemp -d)"
    gpg --homedir "$VERIFY_RING" --import "$GPG_PUB_FILE" 2>/dev/null
    if ! gpg --homedir "$VERIFY_RING" --verify "$target.asc" "$target" 2>/dev/null; then
        rm -rf "$VERIFY_RING"
        echo "ERROR: signature on $target does NOT verify against $GPG_PUB_FILE"
        echo "(the repo public key and the signing key disagree - fix before release)"
        exit 1
    fi
    rm -rf "$VERIFY_RING"
    GPG_FPR=$(gpg --with-colons --show-keys "$GPG_PUB_FILE" 2>/dev/null \
              | awk -F: '/^fpr:/ {print $10; exit}')
    echo "gpg: $target.asc (verifies against $GPG_PUB_FILE, fpr $GPG_FPR)"
    return 0
}

# 0. the documentation screenshots, BEFORE the clean-tree check below.
#
# Every frame bakes the version in: sim/build_sim.sh compiles VERSION into
# KISS_VERSION_STR and the home screen footer prints it. Nothing reran the
# generator between beta4 and beta7, so docs/media/wallet-home.png advertised
# 0.1.0-beta4 for three releases, on the page that tells people what they are
# installing. gen_docs_shots.py --check could not catch it: it verifies that
# sim_main.c still saves the frames the manifest names, deliberately not what
# is inside them, because a pixel diff across zlib versions is a flaky job and
# a flaky docs job teaches people to ignore the docs job.
#
# Cutting a release is exactly when those frames go stale, so regenerate here.
# It runs first on purpose: it can dirty the tree, and a release must describe
# a commit, so anything it changes has to be committed before the build starts
# rather than shipped as an untracked difference.
if [ -z "$SKIP_SHOTS" ]; then
    if [ ! -d managed_components/lvgl__lvgl ]; then
        echo "Cannot regenerate the documentation screenshots: the simulator"
        echo "needs LVGL at managed_components/lvgl__lvgl and it is not there."
        echo "Run a device build once to populate it, or clone the version"
        echo "dependencies.lock pins. (SKIP_SHOTS=1 to bypass, only when you"
        echo "have already regenerated them by hand.)"
        exit 1
    fi
    SHOT_PATHS="docs/shots docs/media docs/readme docs/walkthrough.md"
    echo "== regenerating the documentation screenshots =="
    bash tools/gen_docs_shots.sh
    if [ -n "$(git status --porcelain -- $SHOT_PATHS)" ]; then
        echo
        echo "The documentation screenshots were stale and have been regenerated."
        echo "They bake in the version string, so shipping without them would"
        echo "put the wrong release number in the pictures on the install page."
        echo
        git status --short -- $SHOT_PATHS
        echo
        echo "Commit them, then rerun:"
        echo "  git add -A $SHOT_PATHS"
        echo "  git commit -m \"docs: regenerate screenshots for \$(cat VERSION)\""
        exit 1
    fi
    echo "screenshots already current"
fi

# A release must come from a fully committed tree, so the version/commit it
# reports match code that actually exists in git (no "-dirty"). If you have
# uncommitted work, commit it first, then rerun. (ALLOW_DIRTY=1 overrides for
# a throwaway test build.)
if [ -z "$ALLOW_DIRTY" ] && [ -n "$(git status --porcelain)" ]; then
    echo "You have uncommitted changes, so this would be a messy '-dirty' release."
    echo "To make a clean release:"
    echo "  1. git add -A && git commit -m \"...\"   (save your work)"
    echo "  2. rerun this script"
    echo "(only building a throwaway test? prefix the command with ALLOW_DIRTY=1)"
    exit 1
fi

# 1. fresh verified release build
tools/build_release.sh

# Fail closed on an unsigned app. A device can never accept it as an SD
# update, and everything below this line -- the hash manifest, the GPG
# signature, the install page -- would dress it up as a release anyway.
# build_release.sh clears the marker at its start and writes it only under
# KISS_UNSIGNED=1, so here it can only describe the build just made.
if [ -f build-release/UNSIGNED ]; then
    echo "FAIL: build-release/UNSIGNED exists - this build carries no signature."
    echo "      Publishing would GPG-sign and serve an image no device accepts"
    echo "      as an update. Build with the signing key present, then rerun."
    exit 1
fi

VERSION=$(cat VERSION)
GIT_REV=$(git describe --always --dirty 2>/dev/null || echo nogit)
# Clean, beginner-readable filename: just the version. The exact commit lives
# inside release.json and on the device Settings screen for verifiers.
NAME="kiss-signer-${VERSION}.bin"
OUT="docs/installer"
mkdir -p "$OUT/firmware"

# 2. merge every part of the build into one offset-0 image.
# The offsets come out of the build, never out of this file. They used to be
# typed here, and enabling rollback added an ota_data partition at 0x10000 and
# moved the app to 0x20000: a merge still writing the app at 0x10000 would lay
# it over the slot the bootloader reads to choose which app to run, and publish
# that as the one click install. manifest.json flashes this merged image at
# offset 0, so whatever is wrong here is wrong for every web installer user.
# The install docs promise a DIRECT esptool part flash never writes the wallet
# area: nvs must stay a gap in flasher_args.json, not a part. (The merged image
# below is different -- merge-bin fills gaps, so flashing it at offset 0 does
# erase the wallet, which is what the README's warning is about.) If a part
# ever grows into the nvs range, that promise and this check both break here,
# loudly, instead of in a user's wallet.
"$PY" - <<'PY'
import csv, json, os, sys
nvs = None
for row in csv.reader(open("partitions.csv")):
    if row and row[0].strip() == "nvs":
        nvs = (int(row[3].strip(), 16), int(row[4].strip(), 16))
        break
if not nvs:
    sys.exit("FAIL: partitions.csv has no nvs row")
lo, hi = nvs[0], nvs[0] + nvs[1]
d = json.load(open("build-release/flasher_args.json"))["flash_files"]
for off, f in d.items():
    start = int(off, 16)
    end = start + os.path.getsize("build-release/" + f)
    if start < hi and end > lo:
        sys.exit(f"FAIL: flash part {f} at {off} overlaps nvs "
                 f"[{lo:#x},{hi:#x}) - a direct flash would write the wallet area")
print(f"PASS: no flash part touches nvs [{lo:#x},{hi:#x})")
PY

MERGE_PARTS=$("$PY" - <<'PY'
import json
d = json.load(open("build-release/flasher_args.json"))["flash_files"]
for off, f in sorted(d.items(), key=lambda kv: int(kv[0], 16)):
    print(off, "build-release/" + f)
PY
)
# Unquoted on purpose: each offset and path has to arrive as its own argument.
"$PY" -m esptool --chip esp32p4 merge-bin -o "$OUT/firmware/$NAME" \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  $MERGE_PARTS

# 2.2 the SD update image.
#
# The merged image above is the ONLY thing this script published, and the device
# cannot use it. kiss_fw_desc_parse looks for the esp_app_desc magic 32 bytes
# into the file, which is where it sits in an APPLICATION image; a merged
# offset-0 image has the bootloader there, so the card was scanned, the magic
# did not match, and every published build was reported as "nothing to install".
# The FIRMWARE screen shipped with no artifact it could ever accept.
#
# This is the same signed app the SHA256SUMS below already hashed as
# "application" -- build_release.sh signs it in place, so no second signing
# happens here and none should.
UPDATE_NAME="kiss-signer-${VERSION}-update.bin"
cp build-release/guition_kiss_bringup.bin "$OUT/firmware/$UPDATE_NAME"

# Verify the PUBLISHED copy, not its source: this is the file a card gets,
# and this check fails if signing was skipped, the cp above ever gains a
# transform, or a later step rewrites the file in place.
if ! uvx --from esptool espsecure verify-signature \
     --version 2 --keyfile docs/installer/kiss_ota_pub.pem \
     "$OUT/firmware/$UPDATE_NAME" >/dev/null 2>&1; then
  echo "FAIL: $OUT/firmware/$UPDATE_NAME does not verify against"
  echo "      docs/installer/kiss_ota_pub.pem"
  exit 1
fi
echo "PASS: $UPDATE_NAME verifies against the published public key"

# The descriptor the device will look for, checked HERE rather than discovered
# on a card. Same offset and magic as main/kiss_fw.c; a build that stops
# matching it must fail the release, not ship an image the FIRMWARE screen
# silently refuses.
UPDATE="$OUT/firmware/$UPDATE_NAME" "$PY" - <<'PY'
import os, struct, sys
p = os.environ["UPDATE"]
hdr = open(p, "rb").read(80)
if len(hdr) < 80:
    sys.exit(f"FAIL: {p} is too short to hold an app descriptor")
magic, = struct.unpack_from("<I", hdr, 32)
if magic != 0xABCD5432:
    sys.exit(f"FAIL: {p} has no esp_app_desc magic at offset 32 "
             f"(got {magic:#010x}) - the SD updater would refuse it")
ver = hdr[48:80].split(b"\0")[0].decode("ascii", "replace")
if not ver:
    sys.exit(f"FAIL: {p} has an empty version string; kiss_fw_desc_parse "
             "refuses that rather than ordering it below everything")
print(f"PASS: {os.path.basename(p)} carries app descriptor v{ver}")
PY

# drop stale firmware images so the served folder only holds this release
find "$OUT/firmware" -name 'kiss-signer-*.bin*' \
  ! -name "$NAME*" ! -name "$UPDATE_NAME*" -delete

# 2.5 manifest.json, BEFORE SHA256SUMS so the signature can cover it.
# esp-web-tools flashes every part of the matching build at its own offset, so
# an unsigned manifest is arbitrary bytes at an arbitrary offset that a passing
# gpg --verify still calls good. Its contents depend only on the version, never
# on the signing outcome, so it can be written this early. release.json cannot:
# it records whether signing succeeded, so it stays in step 5 and out of the
# signed manifest, exactly like the offline zip.
NAME="$NAME" VERSION="$VERSION" GIT_REV="$GIT_REV" "$PY" - <<'PY'
import json, os
out = "docs/installer"
name, version, rev = os.environ["NAME"], os.environ["VERSION"], os.environ["GIT_REV"]
json.dump({
    "name": "KISS Signer",
    "version": f"{version}-{rev}",
    "new_install_prompt_erase": True,
    "new_install_improv_wait_time": 0,
    "builds": [{
        "chipFamily": "ESP32-P4",
        "improv": False,
        "parts": [{"path": f"firmware/{name}", "offset": 0}],
    }],
}, open(f"{out}/manifest.json", "w"), indent=2)
print(f"wrote {out}/manifest.json")
PY

# 3. SHA256SUMS first (it is what GPG signs, bitcoin-release style)
NAME="$NAME" UPDATE_NAME="$UPDATE_NAME" "$PY" - <<'PY'
import hashlib, os
out = "docs/installer"
name, update = os.environ["NAME"], os.environ["UPDATE_NAME"]
def sha(p): return hashlib.sha256(open(p, "rb").read()).hexdigest()
with open(f"{out}/SHA256SUMS", "w") as f:
    # Canonical two-space lines and NOTHING else. Annotated lines used to
    # ride along here ("hash  path  (bootloader)"), and to sha256sum -c the
    # annotation is part of the filename: under --ignore-missing -- the exact
    # command the README gives -- every annotated line was silently skipped,
    # on every release since the format shipped. Only files a user downloads
    # belong here, under the names they download them as (GitHub Release
    # assets sit flat beside SHA256SUMS); build-tree provenance lives in
    # release.json's sourceParts, which carries offsets and sizes as well.
    f.write(f"{sha(f'{out}/firmware/{name}')}  {name}\n")
    # The SD update image, under the name it is published as. Same bytes as
    # the app inside the merged image above, but nobody downloading a card
    # image should have to know that to check what they downloaded.
    f.write(f"{sha(f'{out}/firmware/{update}')}  {update}\n")
    # The flash list itself. Without this line the signature covers what gets
    # flashed but not the instructions for flashing it.
    f.write(f"{sha(f'{out}/manifest.json')}  manifest.json\n")
print(f"wrote {out}/SHA256SUMS")
PY

# Every line of SHA256SUMS must VERIFY, not merely parse: stage the listed
# files flat, the way a release download folder looks, and let the standard
# tool check them with no --ignore-missing to hide a skipped line. An OK
# count ties it shut -- --ignore-missing style skips return exit 0, which is
# how the annotated format stayed green for its whole life.
SUMS_STAGE=$(mktemp -d)
cp "$OUT/SHA256SUMS" "$SUMS_STAGE/"
cp "$OUT/firmware/$NAME" "$OUT/firmware/$UPDATE_NAME" "$OUT/manifest.json" "$SUMS_STAGE/"
(
  cd "$SUMS_STAGE"
  if command -v sha256sum >/dev/null 2>&1; then CHK="sha256sum"; else CHK="shasum -a 256"; fi
  $CHK -c SHA256SUMS
  ok=$($CHK -c SHA256SUMS 2>/dev/null | grep -c ': OK$' || true)
  want=$(grep -c . SHA256SUMS)
  if [ "$ok" != "$want" ]; then
    echo "FAIL: SHA256SUMS has $want lines but only $ok verified"
    exit 1
  fi
)
rm -rf "$SUMS_STAGE"
echo "PASS: every SHA256SUMS line verifies with the standard tool"

# 4. signatures (each honest and optional)
GPGSIGNED=0
GPG_FPR=""
if gpg_sign "$OUT/SHA256SUMS"; then
    GPGSIGNED=1
else
    rm -f "$OUT/SHA256SUMS.asc"
    echo "NOTE: no GPG secret key found - SHA256SUMS left unsigned (see docs/installer/SIGNING.md)"
fi

MINISIGNED=0
if command -v minisign >/dev/null && [ -f "$MINISIGN_KEY" ]; then
    minisign -S -s "$MINISIGN_KEY" -m "$OUT/firmware/$NAME" \
      -t "kiss-signer $VERSION $GIT_REV" -x "$OUT/firmware/$NAME.minisig"
    MINISIGNED=1
    echo "minisign: $OUT/firmware/$NAME.minisig"
fi

# 5. release.json (manifest.json is step 2.5, inside the signature)
GPGSIGNED=$GPGSIGNED MINISIGNED=$MINISIGNED NAME="$NAME" VERSION="$VERSION" \
GIT_REV="$GIT_REV" PUBKEY_FILE="$PUBKEY_FILE" GPG_PUB_FILE="$GPG_PUB_FILE" \
GPG_FPR="$GPG_FPR" \
"$PY" - <<'PY'
import hashlib, json, os, datetime

out = "docs/installer"
name, version, rev = os.environ["NAME"], os.environ["VERSION"], os.environ["GIT_REV"]
gpg_signed = os.environ["GPGSIGNED"] == "1"
mini_signed = os.environ["MINISIGNED"] == "1"
signed = gpg_signed or mini_signed

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

full = f"{out}/firmware/{name}"

# Same source as the merge above: what release.json tells a verifier the image
# is made of has to be what the image is actually made of, and a hand written
# list here drifts the moment the partition layout does.
LABELS = {
    "bootloader.bin":       "bootloader",
    "partition-table.bin":  "partition table",
    "ota_data_initial.bin": "ota data",
}
parts = [
    (LABELS.get(f.rsplit("/", 1)[-1], "application"), "build-release/" + f, int(off, 16))
    for off, f in sorted(
        json.load(open("build-release/flasher_args.json"))["flash_files"].items(),
        key=lambda kv: int(kv[0], 16))
]


# manifest.json is written in step 2.5 so SHA256SUMS can cover it. Do not move
# it back here: a manifest written after the signature is an unsigned flash list.

status = ("pgp+minisign" if gpg_signed and mini_signed
          else "pgp" if gpg_signed
          else "minisign" if mini_signed
          else "unsigned-beta")
auth = {
    "signed": signed,
    "signatureStatus": status,
    "signatureLabel": {"pgp+minisign": "GPG + minisign signed",
                       "pgp": "GPG signed",
                       "minisign": "minisign signed"}.get(status, "Unsigned RC"),
    "finalReleaseRequiresSignature": True,
}
if gpg_signed:
    auth["gpgSignaturePath"] = "SHA256SUMS.asc"
    if os.environ.get("GPG_FPR"):
        auth["gpgFingerprint"] = os.environ["GPG_FPR"]
    if os.path.exists(os.environ["GPG_PUB_FILE"]):
        auth["gpgPublicKeyPath"] = os.path.basename(os.environ["GPG_PUB_FILE"])
if mini_signed:
    auth["signaturePath"] = f"firmware/{name}.minisig"
    pub = os.environ["PUBKEY_FILE"]
    if os.path.exists(pub):
        auth["publicKey"] = open(pub).read().strip().splitlines()[-1]
        auth["publicKeyPath"] = os.path.basename(pub)
if not signed:
    auth["keyLabel"] = "final release key pending"

json.dump({
    "schema": 1,
    "product": "KISS Signer",
    "board": "Guition JC4880P443C",
    "chipFamily": "ESP32-P4",
    "version": version,
    "commit": rev,
    "status": "beta",
    "generated": datetime.date.today().isoformat(),
    "authenticity": auth,
    "flash": {"mode": "dio", "frequency": "80m", "size": "16MB"},
    "browserFirmware": {
        "path": f"firmware/{name}", "offset": 0,
        "size": os.path.getsize(full), "sha256": sha(full),
    },
    "sourceParts": [
        {"name": label, "sourcePath": p, "offset": off,
         "size": os.path.getsize(p), "sha256": sha(p)}
        for label, p, off in parts
    ],
    "warnings": [
        "This is a beta build, not a final funds build.",
        "Do not erase flash on a device that holds a wallet unless you intentionally want to wipe it.",
        "After flashing, unplug the device, wait about 3 seconds, then plug it back in.",
    ],
}, open(f"{out}/release.json", "w"), indent=2)
print(f"wrote {out}/manifest.json + release.json (authenticity: {status})")
PY

# 5.5 re-bake docs/verify-release.html against the release just written.
# The page hashes a dropped file with no network, so the expected values have to
# live inside it rather than be fetched. A stale bake is worse than no page: it
# would call a genuine download corrupt, or stay green for the previous release.
# tools/check_installer_version.py fails the build if these drift.
"$PY" - <<'PY'
import json, pathlib, re
page = pathlib.Path("docs/verify-release.html")
if page.is_file():
    rel = json.loads(pathlib.Path("docs/installer/release.json").read_text())
    bf = rel["browserFirmware"]
    name = pathlib.PurePosixPath(bf["path"]).name
    t = page.read_text()
    t = re.sub(r'var EXPECT = "[0-9a-f]*";', f'var EXPECT = "{bf["sha256"]}";', t)
    t = re.sub(r'var EXPECT_SIZE = \d+;', f'var EXPECT_SIZE = {bf["size"]};', t)
    t = re.sub(r'var EXPECT_NAME = "[^"]*";', f'var EXPECT_NAME = "{name}";', t)
    t = re.sub(r'<span class="mono">kiss-signer-[^<]*</span>',
               f'<span class="mono">{name}</span>', t)
    t = re.sub(r'Expected for <b>[^<]*</b>:\s*\n?\s*<span class="mono">[0-9a-f]*</span>',
               f'Expected for <b>{rel["version"]}</b>:\n        '
               f'<span class="mono">{bf["sha256"]}</span>', t)
    page.write_text(t)
    print("re-baked docs/verify-release.html")
PY

# 6. release notes, written before the zip so the zip can carry them: inside an
# offline bundle they are the only copy of the verify commands, the fingerprint
# and the changelog that does not need a network to read.
"$PY" tools/make_release_notes.py --write

# 7. the offline installer zip: the whole install page, the firmware and the
# signed hashes in one download, so flashing needs no network at all.
#
# It cannot join SHA256SUMS. The zip contains release.json, and release.json is
# written above from the outcome of signing SHA256SUMS, so a manifest covering
# the zip would have to be signed before the zip existed. It carries its own
# detached signature instead. That is also the honest shape: inside the bundle
# the page IS the verifier, so what a user needs signed is the container.
"$PY" tools/make_offline_zip.py --out dist
ZIP="dist/kiss-signer-${VERSION}-offline.zip"
if [ "$GPGSIGNED" = "1" ]; then
    gpg_sign "$ZIP"
else
    rm -f "$ZIP.asc"
fi

echo
echo "web release ready: $OUT/firmware/$NAME"
echo "offline installer: $ZIP"
[ "$GPGSIGNED" = "1" ] && echo "                   $ZIP.asc"
[ "$GPGSIGNED" = "1" ] || echo "REMINDER: set up the GPG release key before the first public release."
