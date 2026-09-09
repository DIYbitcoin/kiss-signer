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

> **The key committed today is that uid**, since 0d56077f, and its fingerprint
> is `166A CBF3 7786 FCEA A694 96DE 886F 1BFE B84E F1C0`. It carries two older
> uids underneath, from before the rename; `gpg --verify` prints the primary one
> and nothing else, so what a user sees is the line above.
>
> A uid is unauthenticated free text and the fingerprint is the real identity,
> but `docs/guide.html` quotes the **actual** output of `gpg --verify`, uid and
> all, so nobody is told to expect a line they will never see. It named the
> older uid for 540 commits after the export was refreshed, which is the drift
> this note exists to catch.
>
> **If you rotate this key or change its uid, update that block in
> `docs/guide.html` in the same commit** — the uid line and the fingerprint.

That key lives on the same YubiKey as the one below, moved there with
`keytocard` rather than regenerated, so the fingerprint users check is
unchanged. `gpg --card-edit`, `admin`, `uif 1 on` makes every release signature
need a physical tap as well as the PIN; `gpg --card-status` should read
`UIF setting: Sign=on`. Signing `SHA256SUMS` and the offline zip is two taps.

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

## The device side: SD firmware updates

Everything above is checked on a computer, before the firmware reaches the
device. An SD update is checked **by the device**, so it needs its own key.

That is a second key, and it is a different kind of key. The GPG key signs a
manifest a person reads; this one signs the image itself, in the format the
ESP32 bootloader understands (`espsecure.py sign_data`, Secure Boot V2 scheme).
The device holds the matching public key inside its own signature block and
`esp_ota_end` refuses an image that does not check out, so nothing unsigned
ever becomes bootable.

```sh
# one time, kept offline exactly like the GPG key
espsecure.py generate_signing_key --version 2 --scheme ecdsa256 kiss_ota.pem
espsecure.py extract_public_key --version 2 --keyfile kiss_ota.pem \
    docs/installer/kiss_ota_pub.pem     # commit this, publish its sha256
```

### The key does not have to be a file

`espsecure` speaks PKCS#11, so the same secp256r1 key can live in a smartcard's
signing slot and never exist on the release machine. That is where this
project's key is now: a YubiKey PIV slot 9c, imported rather than generated on
the card, so the public half is unchanged and every device already in the field
still accepts what it signs.

Importing an existing key is the whole trick. A key generated on the card would
be a new key, and a new key is a device in the field refusing every future
update: the running app checks a new image against the public key in its own
signature block, so changing the key strands exactly the people who already
installed a release.

```sh
brew install yubico-piv-tool ykman
ykman piv access change-pin                                    # off 123456
ykman piv access change-management-key --protect --generate    # behind the PIN
ykman piv keys import 9c kiss_ota.pem --pin-policy ONCE --touch-policy ALWAYS
openssl req -new -x509 -key kiss_ota.pem -days 7300 \
    -subj "/CN=KISS Signer OTA signing key/" -out ota9c.crt
ykman piv certificates import 9c ota9c.crt   # PKCS#11 needs a cert to see the key
```

The certificate is not a trust statement and nobody checks it. It is there
because the PKCS#11 module enumerates keys through certificates and will not
show the slot without one.

Then `~/.kiss-signer/hsm.ini`, which both release scripts pick up on sight:

```ini
[hsm_config]
pkcs11_lib = /opt/homebrew/lib/libykcs11.dylib
slot = 0
label = Private key for Digital Signature
label_pubkey = Public key for Digital Signature
```

**No `credentials` line.** With one, the PIN sits in plaintext next to the thing
it unlocks; without one, `espsecure` prompts. `--touch-policy ALWAYS` is the
half a compromised build host cannot supply on its own: the PIN can be read out
of a config or a cache, a finger on the card cannot.

The scripts fall back to `KISS_OTA_KEY` when that config is absent, so a machine
holding only the key file behaves exactly as it did before. What they cannot do
on the card path is compare the published public key against the signing key,
because a card will not surrender a private half to derive one from. The verify
that follows is the stronger check anyway: it proves the shipped bytes check out
under `docs/installer/kiss_ota_pub.pem`, which is what a stranger repeats.

Keep the encrypted backup of the key file. The card is a copy, not the original,
and a dead card with no backup is a new key and every owner re-establishing
trust.

### The tool that touches the key is pinned

`espsecure` is the only program in this project that is ever handed the private
half, and both release scripts used to fetch it as `uvx --from esptool`, which
resolves whatever PyPI serves at that second. One bad release -- a compromised
maintainer account, a yanked version replaced in place -- reads the key off the
command line on a run nobody audits, because the build succeeded.

`ESPTOOL_PIN` in `tools/build_release.sh` and
`tools/build_encrypted_release.sh` names the exact version instead:

```
ESPTOOL_PIN="${ESPTOOL_PIN:-esptool==5.3.1}"
```

A pin does not make the download trustworthy. It makes it the same download as
last time, which is the property that lets a bad one be noticed at all. Bumping
it is a deliberate commit, on a machine that can read the changelog first.

The flashing instructions further down each script stay unpinned on purpose:
they talk to a board and never to a key, and a version baked into a line the
reader copies by hand is a staleness problem with nothing to buy it.

This still matters with the key on a card. The pin is what stops a swapped
`espsecure` from asking the card to sign something else while the PIN is
verified and the maintainer is reaching for the tap.

Signing happens on the machine that holds the key, which should not be the
machine that built the image. `KISS_UNSIGNED=1` produces the reproducible
unsigned artifact anywhere; the signature is applied afterwards, where the key
lives.

The release lane turns the check on. It is **not** in `sdkconfig.defaults`,
because `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` needs the private key at
build time and a plain `idf.py build` would fail on any machine that does not
hold it. In the release build:

```
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_SECURE_BOOT_SIGNING_KEY="kiss_ota.pem"
```

A build made without it still shows the update screen and still refuses to
install: `kiss_fw_available()` answers "cannot be checked" and the screen says
so in those words, rather than accepting an image it has no way to judge.

Users verify a firmware `.bin` for SD exactly like any other artifact, since it
is listed in `SHA256SUMS`. The device's own check is the second gate, not a
replacement for the first.

**Publish both fingerprints.** A release carries two signatures now, and a user
who checks one and assumes the other is a user who has checked half of it.

## The third key: post quantum

The key above is a secp256r1 key, and it is the only thing that decides whether
firmware installs on a KISS signer. Whoever can forge it can hand every device
in the field an image it installs and trusts, and forging elliptic curve
signatures is the thing a cryptographically relevant quantum computer does.

So an update image carries a second signature the device also checks:
**SLH-DSA-SHA2-128s** (FIPS 205), which rests on SHA-256 preimage resistance
rather than on a discrete log. The best known quantum attack there is Grover's,
which halves an exponent instead of collapsing it.

Both signatures are checked and both must pass. This is a second lock on the
same door, never a replacement for the first — a bug in the new code cannot open
the old one.

It is **not** a way to spend bitcoin with a post quantum key. No consensus rule
accepts a hash based signature: BIP-360 merged as Pay to Merkle Root with the
post quantum signatures taken out of it in July 2025, and BIP-361 is a legacy
sunset proposal rather than an activation. Nothing here goes near a key that
holds coins.

```sh
# one time, kept offline exactly like the other two
bash sim/build_pqtool.sh
/tmp/pq_tool keygen ~/.kiss-signer/pq_release.key
/tmp/pq_tool header ~/.kiss-signer/pq_release.key > main/pq_release_pubkey.h
```

Commit `main/pq_release_pubkey.h` and rebuild. The committed placeholder is all
zeroes, which is not a key: `kiss_pqsig_available()` answers false and the
update screen says the image cannot be checked, rather than installing whatever
it is handed. `pq_tool keygen` refuses to overwrite an existing key — there is
no second copy, and a device carrying the old public half has no way back.

`tools/make_web_release.sh` appends the signature to
`kiss-signer-<version>-update.bin` after `espsecure` has signed it, then reads
it back through the device's own splitter. It refuses to publish unless
`main/pq_release_pubkey.h` is the public half of the key that just signed:
nothing downstream can notice otherwise, and the release would be correctly
signed, hashed, served, and refused by every device that installed it.

### Where the signature rides

The last **8192 bytes** of the update `.bin`:

```
magic "KPQ1" | scheme le16 | siglen le16 | 7856 byte signature | zero padding
```

The image is everything before that, and it is byte identical to what
`espsecure` signed — the device hands `esp_ota_write` the image and holds the
trailer back, so `esp_ota_end` judges exactly the file it always did. The
signature covers `SHA-256(image)` with the context string `kiss-signer fw v1`.
The padding must be zero: it rides inside a signed release and reaches flash, so
anything alive in it is a channel whether or not it was meant as one.

This costs nothing at install time. Verification is about 2100 SHA-256
compressions — a few milliseconds — against roughly 2.2 million to produce one,
which is the shape of every hash based scheme. It is fast on this board because
`components/slhdsa/pq_hw_sha.c` holds the ESP32-P4's SHA accelerator for a whole
operation instead of acquiring it per hash; the P4 sets `SOC_SHA_SUPPORT_RESUME`,
so an arbitrary midstate can be loaded, which is what every SLH-DSA hash needs.

The merged USB image does **not** carry a trailer. It is flashed by esptool at
offset 0, never judged by a running device, and appending to it would break the
offsets.

### What an owner sees

An image with no trailer, or a wrong one, gets its own refusal: *the post
quantum signature is missing or wrong*, distinct from *the signature did not
check out*. Every release published before this existed lands there, and it
really is a KISS release correctly signed with the release key — telling that
owner the signature failed would send them hunting for a corrupt download.

## The fourth key: secure boot, and it is the builder's

The flash-encryption release recipe also enables secure boot v2, and on the
ESP32-P4 that is RSA-3072 only: the secp256r1 key above cannot sign a
bootloader this chip will boot, because its ECDSA secure boot is errata'd. So
a burned board trusts a different root, and it is the builder's, not the
project's. Whoever runs `tools/build_encrypted_release.sh` mints three keys
once and keeps them exactly the way this page says to keep the OTA key:

```sh
mkdir -p ~/.kiss-signer/sb && for i in 0 1 2; do
  uvx --from esptool==5.3.1 espsecure generate-signing-key \
    --version 2 --scheme rsa3072 ~/.kiss-signer/sb/kiss_sb_$i.pem
done
```

Three, because the chip holds three key digests, burns every one the
bootloader's signature sector carries on first boot, and revokes any slot left
empty. The bootloader is signed with all three and the app with the first.
Losing one key later costs a rotation, an update signed with the next key that
revokes the old one; losing all three freezes every board burned with them on
its last firmware. No public half is published, because there is no single one
to publish: a burned board's SD update is its builder's own output of the same
recipe, and nothing signed with the key above installs on it. The recipe reads
every signature block back against the config it built, so a bootloader that
carries the wrong scheme or fewer than three blocks never reaches a board.

### Where the two spares live, and the lane that lets them

The whole root is needed exactly once per board, at the burn. After that the
bootloader can never be replaced, and an update is the app alone, carrying one
key. So the recipe has two lanes:

```sh
bash tools/build_encrypted_release.sh                  # the burn: all three
KISS_ENC_UPDATE=1 bash tools/build_encrypted_release.sh  # updates: one key
```

The update lane reads the key at `KISS_SB_KEY_INDEX` (0 unless a rotation has
happened) and nothing else. It signs no bootloader, prints no eFuse recipe,
and deletes the unsigned bootloader from its build directory, so its output
cannot be mistaken for something to burn a board with. Keep keys 1 and 2
somewhere the everyday machine cannot reach: they exist to recover a fleet
whose key 0 leaked, and a spare stored beside the key it replaces is not a
spare. The lane says so out loud when it finds them present.

Slot numbers are what a rotation revokes, and they were fixed forever by the
order the bootloader was signed in. The burn records that order once, as
fingerprints of the three public halves, in `~/.kiss-signer/sb/root.txt`. It
holds nothing secret: copy it to wherever updates are signed, and both lanes
refuse to sign as key *N* with a key that record does not call key *N*.

### The update key on a card

The split takes two keys off the everyday machine. Putting the third on a
smartcard takes the last one, and then no private half of the root is readable
there at all: the key that signs every release lives inside a device that will
not export it and wants a finger on it before it signs. A stolen laptop buys
an attacker nothing.

Point `KISS_SB_HSM_CONFIG` at an ini of the same shape as the OTA one, naming
the slot that holds the key — a separate file, because it is a separate key
even when it is the same card. The update lane then signs from the card, and
says so before it does:

```sh
KISS_SB_HSM_CONFIG=~/.kiss-signer/sb_hsm.ini \
  KISS_ENC_UPDATE=1 bash tools/build_encrypted_release.sh
```

Two things to know before planning on it. Secure boot on this chip is RSA-3072
and many cards cap PIV RSA at 2048, so check what yours holds before importing
anything. And a card cannot hand a public key back to the signing tool, so the
lane reads the public half off disk instead: the burn writes it beside each
private key as `kiss_sb_<n>.pub.pem`, and that file has to travel with
`root.txt` to the update machine. It is what the card's signature is checked
against, so a card holding the wrong key is caught there.

The card gets a *copy* of the key, minted from the file. Once it has signed a
release, that file belongs offline with the spares — the lane counts every
private key of the root it can see and says so.

## What this does and doesn't prove

- **Does:** the binary is exactly what the key holder built, and from which
  commit (rebuildable via the pinned Docker toolchain).
- **Doesn't:** that the device will only *boot* signed firmware. That is
  **secure boot**, a separate hardware feature in the flash-encryption release
  recipe (one-way eFuse burn), and on a burned board the key is the fourth one
  above, the builder's own, not this one. Release signatures protect the
  download; secure boot protects the device.

  The SD update check above sits between the two. On a beta board it is
  verified in software against the running app's own block; on a burned board
  the same check is made against the eFuse digests, in hardware. It stops a
  bad image being *installed*; on a beta board it cannot stop one written past
  it, with a programmer, straight to flash. Burning the eFuse is what closes
  that.
