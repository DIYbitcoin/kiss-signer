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

**Three ways in. Pick the row that describes you.**

| You are | Use | Needs a cable? |
| --- | --- | --- |
| New device, or on beta7 or earlier | **A. Browser install** | yes, USB |
| Already on beta8 or later | **B. SD card update** | no |
| Flashing from a machine with no internet | **C. Offline zip** | yes, USB |

Route B is the easy one and needs no computer at all. Route A erases the whole
chip, so read the warning below it before choosing it.

### A. Browser install, over USB

Open the install page, plug the device in, follow the buttons. It hashes the
firmware against this release before it offers you anything.

Use **Chrome, Brave or Edge** on macOS, Windows or Linux. Safari and Firefox
cannot talk to a USB device from a web page, so neither can flash this. On
Linux, your user has to be able to read the serial port: if the page cannot see
the device, add yourself to the `dialout` group and log out and back in.

Prefer the command line? The README carries the `esptool` command for all three
systems, including which port name to expect:

| | Port looks like |
| --- | --- |
| macOS | `/dev/cu.usbmodem*` |
| Linux | `/dev/ttyACM*` |
| Windows | `COM3`, `COM4`, ... |

### B. SD card update, no computer

A running signer takes its next firmware off an SD card, so this needs no cable,
no drivers and no operating system at all. Put `kiss-signer-0.1.0-beta10-update.bin` in the root of a card,
then SETTINGS > FIRMWARE on the device and hold to install. It checks both
signatures against the keys built into it before anything is written, and if the
new firmware fails to start it goes back to the old one by itself.

Use that file and not the merged image: the merged one starts with the
bootloader, and the device looks for the application header instead, so it
reports nothing to install.

### C. Offline zip, for a machine with no internet

`kiss-signer-0.1.0-beta10-offline.zip` holds the install page, the firmware and the signed hashes in one
6 MB file. Verify its signature on a machine that has a network, carry it
across, unzip it, then run the file for your system:

| | Run |
| --- | --- |
| macOS | `serve.command` |
| Windows | `serve.bat` |
| Linux | `./serve.sh` |

It opens a page that serves to that one computer and reaches nothing else, so
the machine you flash from can stay offline the whole time. `00-START-HERE.txt`
inside the zip says the same in more detail. Same browser rule as route A:
Chrome, Brave or Edge.

After any of the three: unplug the device, wait about 3 seconds, then plug it
back in. It only starts new firmware from a real power-on.

### Coming from beta7 or earlier: this one erases your keys

This release splits the flash into two firmware slots, which is what lets every
release after it arrive on an SD card instead of a USB cable. A partition table
cannot be replaced by the thing it defines, so the crossing itself has to be
done over USB, once.

**Your recovery words do not survive this flash.** `nvs`, where they live, moves
from `0x9000` to `0x11000` in the new layout, so the crossing takes them with
it. The browser and offline routes erase the whole chip; flashing the pieces by
hand leaves the old words at an address this firmware no longer reads. Either
way the keys are gone from this signer until you restore them.

So treat it as a restore, not an update. Have the recovery words in your hand on
paper, check them against the device before you unplug it, and expect to restore
from that paper once the new firmware is running. If those keys control coins and
you cannot find the words, do not flash.

After this, Settings has a FIRMWARE button. Put `kiss-signer-0.1.0-beta10-update.bin` in the root of an SD
card, hold to install, and the device checks the signature against the key built
into it before anything is written. If the new firmware fails to start, the
device goes back to this one on its own.

Use that file and not the merged image: the merged one starts with the
bootloader, and the device looks for the application header instead, so it
reports nothing to install.

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
