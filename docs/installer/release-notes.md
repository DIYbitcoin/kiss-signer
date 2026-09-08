# KISS Signer 0.1.0-beta9

Beta firmware for the Guition JC4880P443C ESP32-P4 device.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-signer-0.1.0-beta9.bin`: merged firmware image, for flashing over USB
- `kiss-signer-0.1.0-beta9-update.bin`: the same firmware as an SD card update (see FIRMWARE below)
- `SHA256SUMS`: firmware hashes
- `SHA256SUMS.asc`: GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc`: KISS release public key
- `release.json`: release metadata for tools

Flashing on a machine with no network? Take these two instead:

- `kiss-signer-0.1.0-beta9-offline.zip`: the install page, the firmware and the signed hashes in one file
- `kiss-signer-0.1.0-beta9-offline.zip.asc`: GPG signature for the zip

## Verify

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: 166ACBF37786FCEAA69496DE886F1BFEB84EF1C0

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS

# the offline installer carries its own signature
gpg --verify kiss-signer-0.1.0-beta9-offline.zip.asc kiss-signer-0.1.0-beta9-offline.zip
```

Main firmware SHA256:

`3f10284227ded3c8f5859c9c63cd8f187ae340fc6ed52c48331f050e4aefb82d`

Release commit:

`v0.1.0-beta7-1054-gbdcfa7be`

## Install

For beta releases, flash this exact verified `.bin` using the README install steps.

To flash from a browser instead, unzip `kiss-signer-0.1.0-beta9-offline.zip`, run the serve file inside it (`serve.command` on macOS, `serve.bat` on Windows, `./serve.sh` on Linux) and open the address it prints. It serves to that one computer only and reaches nothing else, so the machine you flash from can be offline the whole time. `00-START-HERE.txt` inside the zip walks through it.

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

After this, Settings has a FIRMWARE button. Put `kiss-signer-0.1.0-beta9-update.bin` in the root of an SD
card, hold to install, and the device checks the signature against the key built
into it before anything is written. If the new firmware fails to start, the
device goes back to this one on its own.

Use that file and not the merged image: the merged one starts with the
bootloader, and the device looks for the application header instead, so it
reports nothing to install.

## Changelog

Everything on screen got bigger. The last release fit 21 languages onto the
glass; this one makes sure you can read them standing up.

| | |
| --- | --- |
| 🔎 **No more tiny text** | every sentence an owner reads is measured, and the ones that had quietly shrunk were rewritten instead |
| 👁 **The passphrase eye** | a mark instead of a word, because nine languages could not fit HIDE |
| 🗂 **Settings rows moved** | each row now sits on the tab that describes what it does |
| 🧰 **New toolchain** | built on ESP-IDF v6.1 |

### 🔒 Security

- **A host with no randomness now stops instead of inventing some.** On desktop
  and simulator builds, a failed read from the system random source was topped
  up with a counter, which looks like randomness to every test and is identical
  on every run. It aborts now. The device itself never used that path.

### ✨ Better

- **Text is sized by measurement, not by guess.** Bodies, explainer grids and
  fact rows all pick the largest size that genuinely fits, and four separate
  places that had been throwing away a third of their space were corrected.
  Where the copy was simply too long, the copy was cut.
- **A fact's value wraps.** It used to be pinned to one line sized for English,
  which is why the same row overflowed in fifteen other languages.
- **The passphrase toggle is an eye.** Nine languages had no short word for
  HIDE, so the word is gone.
- **Word suggestions are big enough for a thumb.**
- **Settings reads in the order you look for things.** The theme picker sits
  above the **?**, PERSIST moved to the tab that says how the signer behaves,
  and the DEVICE tab holds firmware, build and terms.
- **Cautions keep one colour**, and NO UNDO no longer shouts over the control
  it belongs to.

### 🧰 For builders

- **The encrypted release recipe now carries secure boot v2.** Both eFuse burns
  have to happen in one first boot or the second can never happen at all. It
  refuses to produce a flashable image until the signing key is settled: secure
  boot on this chip is RSA-3072, because ECDSA secure boot is errata'd on the
  ESP32-P4.
- **A gate now measures every screen against the 3.5in board** nobody is
  holding yet, so the work that board needs is a list rather than a surprise.
- **Eighteen checkers prove they still fire.** Six had no self test at all, and
  one of those was the check that had just caught a real failure.
