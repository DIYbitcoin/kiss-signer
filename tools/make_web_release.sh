#!/bin/bash
# Build the web-installer release artifacts into docs/installer/:
#   * firmware/kiss-wallet-<version>-<commit>-full.bin  (merged, offset 0)
#   * firmware/...-full.bin.minisig                     (if a signing key exists)
#   * SHA256SUMS                                        (all parts + merged bin)
#   * manifest.json / release.json                      (rewritten in place)
#
# Signing uses minisign (https://jedisct1.github.io/minisign/):
#   one-time: brew install minisign && minisign -G -p docs/installer/kiss_wallet.pub \
#             -s ~/.kiss-wallet/minisign.key
# The SECRET key stays on the maintainer machine (never in the repo); only the
# .pub travels. Without a key the release is emitted unsigned and labeled so.
# See docs/installer/SIGNING.md for the full flow + user verification steps.
set -e
cd "$(dirname "$0")/.."

MINISIGN_KEY="${MINISIGN_KEY:-$HOME/.kiss-wallet/minisign.key}"
PUBKEY_FILE="docs/installer/kiss_wallet.pub"
PY="${PY:-/tmp/spritevenv/bin/python}"
[ -x "$PY" ] || PY=python3

# 1. fresh verified release build
tools/build_release.sh

VERSION=$(cat VERSION)
GIT_REV=$(git describe --always --dirty 2>/dev/null || echo nogit)
NAME="kiss-wallet-${VERSION}-${GIT_REV}-full.bin"
OUT="docs/installer"
mkdir -p "$OUT/firmware"

# 2. merge bootloader + partition table + app into one offset-0 image
"$PY" -m esptool --chip esp32p4 merge-bin -o "$OUT/firmware/$NAME" \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x2000  build-release/bootloader/bootloader.bin \
  0x8000  build-release/partition_table/partition-table.bin \
  0x10000 build-release/guition_kiss_bringup.bin

# drop stale firmware images so the served folder only holds this release
find "$OUT/firmware" -name 'kiss-wallet-*-full.bin*' ! -name "$NAME*" -delete

# 3. sign (or honestly mark unsigned)
SIGNED=0
if command -v minisign >/dev/null && [ -f "$MINISIGN_KEY" ]; then
    minisign -S -s "$MINISIGN_KEY" -m "$OUT/firmware/$NAME" \
      -t "kiss-wallet $VERSION $GIT_REV" -x "$OUT/firmware/$NAME.minisig"
    SIGNED=1
    echo "signed: $OUT/firmware/$NAME.minisig"
else
    echo "NOTE: unsigned release (no minisign key at $MINISIGN_KEY - see docs/installer/SIGNING.md)"
fi

# 4. hashes + manifest.json + release.json
SIGNED=$SIGNED NAME="$NAME" VERSION="$VERSION" GIT_REV="$GIT_REV" \
PUBKEY_FILE="$PUBKEY_FILE" "$PY" - <<'PY'
import hashlib, json, os, datetime

out = "docs/installer"
name, version, rev = os.environ["NAME"], os.environ["VERSION"], os.environ["GIT_REV"]
signed = os.environ["SIGNED"] == "1"

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

full = f"{out}/firmware/{name}"
parts = [
    ("bootloader",      "build-release/bootloader/bootloader.bin",            0x2000),
    ("partition table", "build-release/partition_table/partition-table.bin",  0x8000),
    ("application",     "build-release/guition_kiss_bringup.bin",             0x10000),
]

with open(f"{out}/SHA256SUMS", "w") as f:
    f.write(f"{sha(full)}  firmware/{name}\n")
    for label, p, _ in parts:
        f.write(f"{sha(p)}  {p}  ({label})\n")

json.dump({
    "name": "KISS Wallet",
    "version": f"{version}-{rev}",
    "new_install_prompt_erase": True,
    "new_install_improv_wait_time": 0,
    "builds": [{
        "chipFamily": "ESP32-P4",
        "improv": False,
        "parts": [{"path": f"firmware/{name}", "offset": 0}],
    }],
}, open(f"{out}/manifest.json", "w"), indent=2)

auth = {
    "signatureStatus": "minisign" if signed else "unsigned-release-candidate",
    "signatureLabel": "minisign-signed" if signed else "Unsigned RC",
    "finalReleaseRequiresSignature": True,
}
if signed:
    auth["signaturePath"] = f"firmware/{name}.minisig"
    pub = os.environ["PUBKEY_FILE"]
    if os.path.exists(pub):
        auth["publicKey"] = open(pub).read().strip().splitlines()[-1]
        auth["publicKeyPath"] = os.path.basename(pub)
else:
    auth["keyLabel"] = "final release key pending"

json.dump({
    "schema": 1,
    "product": "KISS Wallet",
    "board": "Guition JC4880P443C",
    "chipFamily": "ESP32-P4",
    "version": version,
    "commit": rev,
    "status": "release-candidate",
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
        "This is a release-candidate build, not a final funds build.",
        "Do not erase flash on a board that holds a wallet unless you intentionally want to wipe it.",
        "After flashing, unplug the board, wait about 3 seconds, then plug it back in.",
    ],
}, open(f"{out}/release.json", "w"), indent=2)
print(f"wrote {out}/manifest.json, release.json, SHA256SUMS")
PY

echo
echo "web release ready: $OUT/firmware/$NAME"
[ "$SIGNED" = "1" ] || echo "REMINDER: generate the signing key before the first public release."
