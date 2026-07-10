# Release signing & verification

Firmware users flash must be verifiable: they check that the binary they hold
is byte-for-byte the one the maintainer built and signed. We use
[minisign](https://jedisct1.github.io/minisign/) — one small tool, one key,
no PGP web-of-trust ceremony.

## One-time setup (maintainer)

```sh
brew install minisign          # or: apt install minisign
mkdir -p ~/.kiss-wallet
minisign -G -p docs/installer/kiss_wallet.pub -s ~/.kiss-wallet/minisign.key
```

- You'll be asked for a **key password** — treat it like a wallet passphrase.
- `kiss_wallet.pub` is committed to the repo (it's public by design).
- `~/.kiss-wallet/minisign.key` **never** enters the repo or leaves your
  machine. Back it up like a seed: if it leaks, an attacker can sign malicious
  firmware as you; if it's lost, you start a new key and users must re-trust.

## Every release (maintainer)

```sh
tools/make_web_release.sh
```

Builds, merges, hashes, and — when the key exists — signs the merged
firmware, then rewrites `manifest.json` / `release.json` (the `authenticity`
block flips from `unsigned-release-candidate` to `minisign`) and `SHA256SUMS`.
Commit the results; the installer page serves them.

## Verifying a download (user)

```sh
# 1. hash matches release.json / SHA256SUMS
shasum -a 256 kiss-wallet-<version>-full.bin

# 2. signature is by the project key
minisign -Vm kiss-wallet-<version>-full.bin -p kiss_wallet.pub
```

`minisign` prints `Signature and comment signature verified` with the trusted
comment `kiss-wallet <version> <commit>`. Compare the public key you used
against at least one other channel (repo history, release notes) — the point
of the signature is that the *key*, not the download server, is what you trust.

## What this does and doesn't prove

- **Does:** the binary is exactly what the holder of the signing key built,
  and which commit it came from (rebuildable via the pinned Docker toolchain).
- **Doesn't:** that the device will only *boot* signed firmware — that is
  **secure boot**, a separate hardware feature handled in the flash-encryption
  hardening pass (eFuse burn, one-way). Release signatures protect the
  download; secure boot protects the device.
