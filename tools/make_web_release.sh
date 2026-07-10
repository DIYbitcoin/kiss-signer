#!/bin/bash
# Build the web-installer release artifacts into docs/installer/:
#   * firmware/kiss-wallet-<version>-<commit>-full.bin  (merged, offset 0)
#   * SHA256SUMS + SHA256SUMS.asc                       (GPG, if a key exists)
#   * firmware/...-full.bin.minisig                     (minisign, if key exists)
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

MINISIGN_KEY="${MINISIGN_KEY:-$HOME/.kiss-wallet/minisign.key}"
PUBKEY_FILE="docs/installer/kiss_wallet.pub"
GPG_PUB_FILE="docs/installer/kiss_wallet_pgp.asc"
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

# 3. SHA256SUMS first (it is what GPG signs, bitcoin-release style)
NAME="$NAME" "$PY" - <<'PY'
import hashlib, os
out = "docs/installer"
name = os.environ["NAME"]
def sha(p): return hashlib.sha256(open(p, "rb").read()).hexdigest()
parts = [
    ("bootloader",      "build-release/bootloader/bootloader.bin"),
    ("partition table", "build-release/partition_table/partition-table.bin"),
    ("application",     "build-release/guition_kiss_bringup.bin"),
]
with open(f"{out}/SHA256SUMS", "w") as f:
    f.write(f"{sha(f'{out}/firmware/{name}')}  firmware/{name}\n")
    for label, p in parts:
        f.write(f"{sha(p)}  {p}  ({label})\n")
print(f"wrote {out}/SHA256SUMS")
PY

# 4. signatures (each honest and optional)
GPGSIGNED=0
if command -v gpg >/dev/null && gpg --list-secret-keys ${GPG_KEY_ID:+"$GPG_KEY_ID"} >/dev/null 2>&1; then
    gpg --batch --yes ${GPG_KEY_ID:+-u "$GPG_KEY_ID"} --armor \
      --detach-sign -o "$OUT/SHA256SUMS.asc" "$OUT/SHA256SUMS"
    GPGSIGNED=1
    echo "gpg: $OUT/SHA256SUMS.asc"
    [ -f "$GPG_PUB_FILE" ] || echo "NOTE: export your public key into the repo:" \
      " gpg --armor --export ${GPG_KEY_ID:-<your key id>} > $GPG_PUB_FILE"
else
    rm -f "$OUT/SHA256SUMS.asc"
    echo "NOTE: no GPG secret key found - SHA256SUMS left unsigned (see docs/installer/SIGNING.md)"
fi

MINISIGNED=0
if command -v minisign >/dev/null && [ -f "$MINISIGN_KEY" ]; then
    minisign -S -s "$MINISIGN_KEY" -m "$OUT/firmware/$NAME" \
      -t "kiss-wallet $VERSION $GIT_REV" -x "$OUT/firmware/$NAME.minisig"
    MINISIGNED=1
    echo "minisign: $OUT/firmware/$NAME.minisig"
fi

# 5. manifest.json + release.json
GPGSIGNED=$GPGSIGNED MINISIGNED=$MINISIGNED NAME="$NAME" VERSION="$VERSION" \
GIT_REV="$GIT_REV" PUBKEY_FILE="$PUBKEY_FILE" GPG_PUB_FILE="$GPG_PUB_FILE" \
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
parts = [
    ("bootloader",      "build-release/bootloader/bootloader.bin",            0x2000),
    ("partition table", "build-release/partition_table/partition-table.bin",  0x8000),
    ("application",     "build-release/guition_kiss_bringup.bin",             0x10000),
]

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

status = ("pgp+minisign" if gpg_signed and mini_signed
          else "pgp" if gpg_signed
          else "minisign" if mini_signed
          else "unsigned-release-candidate")
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
print(f"wrote {out}/manifest.json + release.json (authenticity: {status})")
PY

echo
echo "web release ready: $OUT/firmware/$NAME"
[ "$GPGSIGNED" = "1" ] || echo "REMINDER: set up the GPG release key before the first public release."
