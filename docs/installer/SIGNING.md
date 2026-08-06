# Release signing & verification

Users must be able to check that the firmware they hold is byte-for-byte what
the maintainer built and signed. We follow the standard bitcoin-project flow
(Bitcoin Core, SeedSigner, Krux, Sparrow all do a variant of this):

1. Every release ships a **SHA256SUMS** manifest listing the hash of each
   artifact.
2. The maintainer **GPG-signs the manifest** (detached `SHA256SUMS.asc`).
3. Users verify the signature with the project public key, obtained from the
   repo AND cross-checked through at least one other channel. The key is the
   trust anchor, never the download server.

minisign is kept as an optional second signature over the merged firmware
image (simpler tooling, some users prefer it). Post-v1 roadmap: sign release
hashes **with the KISS device itself** (bitcoin message signature, the way
Krux's sign-file feature works), so the signer vouches for its own firmware.

## One-time setup (maintainer)

GPG (primary):

```sh
gpg --quick-generate-key "KISS Signer releases <diybitcoin@protonmail.com>" ed25519 sign never
gpg --armor --export <KEYID> > docs/installer/kiss_signer_pgp.asc   # commit this
```

minisign (optional extra):

```sh
brew install minisign
mkdir -p ~/.kiss-signer
minisign -G -p docs/installer/kiss_signer.pub -s ~/.kiss-signer/minisign.key
```

Secret keys never enter the repo. Back them up offline like a seed: a leaked
key lets an attacker sign malicious firmware as you; a lost key means starting
a new key and users must re-establish trust in it.

## Every release (maintainer)

```sh
tools/make_web_release.sh          # GPG_KEY_ID=<id> to pick a specific key
```

Builds, merges, writes SHA256SUMS, signs with whatever keys exist (and says so
honestly in `release.json`'s `authenticity` block), regenerates
`manifest.json`/`release.json`, writes `release-notes.md`, and packs
`dist/kiss-signer-<version>-offline.zip` with its own `.asc`. Attach the
firmware image, `SHA256SUMS`, `SHA256SUMS.asc`, `release.json`,
`kiss_signer_pgp.asc`, **and both offline zip files** to the GitHub Release. Use
`release-notes.md` as the GitHub Release body so every release keeps the same
shape: download, verify, install, changelog. The installer page is a later
GitHub Pages path, not the beta install path.

The zip is signed on its own rather than listed in `SHA256SUMS`, because it
contains `release.json`, which is written from the outcome of signing
`SHA256SUMS`. A manifest covering the zip would have to be signed before the zip
existed. It is also the right shape: inside an offline bundle the page is the
verifier, so the thing a user needs signed is the container that carries it.

The zip is not committed. It is derived from `docs/` and the release build, and
at about 4 MB per release it belongs on the Releases page rather than in git
history. `tools/make_offline_zip.py --check` runs in CI and fails if the page
starts loading a file the bundle does not carry.

## Verifying a download (user)

Most people should use [`docs/verify-release.html`](../verify-release.html),
which hashes the file in the browser with no network and compares it against
the published value, then gives the one GPG command below. It travels in the
offline zip, so it works on an airgapped machine. The rest of this section is
the same thing done entirely by hand.

```sh
# 1. the manifest is signed by the project key
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS

# 2. the firmware matches the signed manifest
shasum -a 256 -c SHA256SUMS --ignore-missing

# the offline installer zip is signed on its own
gpg --verify kiss-signer-<version>-offline.zip.asc kiss-signer-<version>-offline.zip

# optional extra check (minisign)
minisign -Vm kiss-signer-<version>.bin -p kiss_signer.pub
```

Cross-check the key fingerprint against a second channel (repo history,
release notes, maintainer profile) before trusting it.

## What this does and doesn't prove

- **Does:** the binary is exactly what the key holder built, and from which
  commit (rebuildable via the pinned Docker toolchain).
- **Doesn't:** that the device will only *boot* signed firmware. That is
  **secure boot**, a separate hardware feature handled in the flash-encryption
  hardening pass (one-way eFuse burn). Release signatures protect the
  download; secure boot protects the device.
