# KISS Signer 0.1.0-beta10

Beta firmware for the Guition JC4880P443C ESP32-P4 device.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-signer-0.1.0-beta10.bin`: merged firmware image, for flashing over USB
- `kiss-signer-0.1.0-beta10-update.bin`: the same firmware as an SD card update (see FIRMWARE below)
- `SHA256SUMS`: firmware hashes
- `SHA256SUMS.asc`: GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc`: KISS release public key
- `release.json`: release metadata for tools

Flashing on a machine with no network? Take these two instead:

- `kiss-signer-0.1.0-beta10-offline.zip`: the install page, the firmware and the signed hashes in one file
- `kiss-signer-0.1.0-beta10-offline.zip.asc`: GPG signature for the zip

## Verify

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: 166ACBF37786FCEAA69496DE886F1BFEB84EF1C0

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS

# the offline installer carries its own signature
gpg --verify kiss-signer-0.1.0-beta10-offline.zip.asc kiss-signer-0.1.0-beta10-offline.zip
```

Main firmware SHA256:

`776ceff9a354dbab7ed0d53290227317deba0c32ca658c7bd78dee918504b59f`

Release commit:

`e2092501`

## Install

**Four ways in, easiest first.** Each is written out with pictures on the
install page and in the guide — this is the map, not the manual.

| | Route | Cable? | Who it suits |
| --- | --- | --- | --- |
| 🖱️ | **[Install page](https://diybitcoin.github.io/kiss-signer/)** | yes, USB | anyone. Plug in, tick the box, press the button |
| 💾 | **[SD card update](https://diybitcoin.github.io/kiss-signer/guide.html#sdupdate)** | no | already on beta8 or later, and no computer to hand |
| 📦 | **[Offline zip](https://diybitcoin.github.io/kiss-signer/guide.html#offline)** | yes, USB | the machine you flash from has no internet |
| ⌨️ | **[Command line](https://diybitcoin.github.io/kiss-signer/guide.html#esptool)** | yes, USB | Safari or Firefox, or you would rather use a terminal |

The SD card route is the only one that needs no computer at all, and it keeps
your keys and settings: put `kiss-signer-0.1.0-beta10-update.bin` in the root of a card, then SETTINGS →
FIRMWARE on the device and hold to install. Use that file, not the merged image.

> ⚠️ **Coming from beta7 or earlier?** SD card updates did not exist yet, so you
> have to use the install page, and **that erases the whole chip — including
> your keys.** Have your seed words and passphrase on paper first. Anyone on
> beta8 or later can ignore this.

## Changelog

The device finishes its sentences. Beta9 measured the text and found dozens of
lines that no longer fit their box in 21 languages; this release rewrites them
instead of trimming them, so nothing an owner reads ends in a shrug.

| | |
| --- | --- |
| 📝 **Sentences that fit** | around forty strings rewritten to their lane rather than cut off mid-word |
| 💸 **Transaction, not payment** | one word for the thing you sign, in every language |
| ✅ **A restore counts as a backup** | proving your words on the device credits the backup you just proved |
| 🧰 **A release that checks itself** | the build refuses to ship if its randomness or its signatures are not what the recipe says |

### 🔒 Security

- **A release build now proves where its randomness came from.** The check that
  catches a host with no entropy ran on the ordinary release lane only. Both
  release lanes run it now, and neither will produce a binary without it.
- **The encryption row tells a rehearsed board from a finished one.** Three
  corners of Settings each asked the chip their own question, and the one they
  asked is answered yes by a board that is still open over the cable. One
  reader answers for all three, and the calm state means what the recipe says.

### ✨ New features

- **A restore now counts as a backup.** Reading your seed words back into the
  device proves the paper is right, so the backup reminder stops asking for
  something you have already done.

### 💫 Improvements

- **Payment became transaction** on every screen and in every language. The two
  words were mixed, and only one of them is what a signer actually handles.
- **Around forty lines were rewritten to fit**, across all 21 languages: seed
  explainers, the pairing note, the passphrase intro, the lock screen, SD card
  notes, row labels and the coordinator articles. Each one now says the whole
  thing at the size the screen can show.
- **The screen that says what a restore recovered writes seed words out in
  full**, instead of naming them in shorthand.

### 🐛 Bug fixes

- **An abandoned slide rewinds** to where it started, rather than jumping there.
- **Controls sit above their own band's fill**, so a tap lands on the control
  and not on the paint behind it.
- **A translation that promised more than its English does** is caught by the
  gate now, rather than by a reader.

### 🧰 Under the hood

- The release recipe reads the chip's fuses before the step that burns them,
  reads each signature block back against the configuration that will judge it,
  and puts the bootloader back into the flash list where it belongs.
- The reproducibility proof now covers arm64 as well as x86.
- The changelog has a generator: it drafts the skeleton from the commits and
  refuses a release whose entry was never written.
- Four separate push workflows became one CI run.
- The burn recipe in the documentation says what the build actually does, and
  the pictures in the guide were re-rendered at the current build.
