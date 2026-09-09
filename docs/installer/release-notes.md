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

For beta releases, flash this exact verified `.bin` using the README install steps.

To flash from a browser instead, unzip `kiss-signer-0.1.0-beta10-offline.zip`, run the serve file inside it (`serve.command` on macOS, `serve.bat` on Windows, `./serve.sh` on Linux) and open the address it prints. It serves to that one computer only and reaches nothing else, so the machine you flash from can be offline the whole time. `00-START-HERE.txt` inside the zip walks through it.

Both browser routes, the hosted page and this zip, need Chrome, Brave or Edge on desktop. Safari and Firefox cannot flash ESP32 devices over Web Serial. The hosted page also stays off whenever a release is staged; the zip does not, because it is the release.

After flashing, unplug the device, wait about 3 seconds, then plug it back in.

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

### ✨ Better

- **A restore credits the backup it just proved.** Reading your words back into
  the device is proof they are right, so the backup reminder stops asking for
  something you have already done.
- **The seed words screen writes the words out in full** on the chip that says
  what a restore recovered, instead of naming them in shorthand.
- **An abandoned slide rewinds** to where it started rather than jumping there.
- **Controls sit above their own band's fill**, so a tap lands on the control
  and not on the paint behind it.

### 📝 Words on screen

- **Payment became transaction** on every screen and in every language. The two
  words were mixed, and only one of them is what a signer actually handles.
- **Around forty strings were rewritten to fit**, across all 21 languages: seed
  explainers, the pairing note, the passphrase intro, the lock screen, SD card
  notes, row labels and the coordinator articles. Each one now says the whole
  thing at the size the screen can show.
- **A translation that promised more than its English does** is caught by the
  gate now rather than by a reader.

### 🧰 For builders

- The release recipe reads the chip's fuses before the step that burns them,
  reads each signature block back against the configuration that will judge it,
  and puts the bootloader back into the flash list where it belongs.
- The reproducibility proof now covers arm64 as well as x86.
- The changelog has a generator: it drafts the skeleton from the commits and
  refuses a release whose entry was never written.
- Four separate push workflows became one CI run.

### 📚 Docs

- The burn recipe in the documentation says what the build actually does.
- The pictures in the guide were re-rendered at the current build.
