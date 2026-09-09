#!/usr/bin/env python3
"""Generate the fixed GitHub Release body for the current KISS release."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
CHANGELOG = ROOT / "CHANGELOG.md"
RELEASE_JSON = ROOT / "docs/installer/release.json"
NOTES_OUT = ROOT / "docs/installer/release-notes.md"


def changelog_section(version: str) -> str:
    text = CHANGELOG.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"^## \[{re.escape(version)}\][^\n]*\n(?P<body>.*?)(?=^## \[|\Z)",
        re.M | re.S,
    )
    match = pattern.search(text)
    if not match:
        raise SystemExit(f"missing CHANGELOG.md section for {version}")
    return match.group("body").strip()


def release_meta() -> dict:
    if not RELEASE_JSON.exists():
        raise SystemExit("missing docs/installer/release.json; run tools/make_web_release.sh first")
    return json.loads(RELEASE_JSON.read_text(encoding="utf-8"))


def code_block(lang: str, body: str) -> str:
    return f"```{lang}\n{body.rstrip()}\n```"


def render(version: str) -> str:
    meta = release_meta()
    browser = meta["browserFirmware"]
    auth = meta.get("authenticity", {})
    filename = Path(browser["path"]).name
    fingerprint = auth.get("gpgFingerprint", "REPLACE-WITH-RELEASE-KEY-FINGERPRINT")
    commit = meta.get("commit", "unknown")
    sha256 = browser.get("sha256", "unknown")

    offline = f"kiss-signer-{version}-offline.zip"
    # The card image. NOT the merged one: the device looks for the app
    # descriptor 32 bytes in, and a merged image has the bootloader there, so
    # the FIRMWARE screen reports "nothing to install" for it every time.
    update = f"kiss-signer-{version}-update.bin"

    verify_cmds = code_block(
        "sh",
        f"""gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: {fingerprint}

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS

# the offline installer carries its own signature
gpg --verify {offline}.asc {offline}""",
    )

    return f"""# KISS Signer {version}

Beta firmware for the Guition JC4880P443C ESP32-P4 device.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `{filename}`: merged firmware image, for flashing over USB
- `{update}`: the same firmware as an SD card update (see FIRMWARE below)
- `SHA256SUMS`: firmware hashes
- `SHA256SUMS.asc`: GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc`: KISS release public key
- `release.json`: release metadata for tools

Flashing on a machine with no network? Take these two instead:

- `{offline}`: the install page, the firmware and the signed hashes in one file
- `{offline}.asc`: GPG signature for the zip

## Verify

{verify_cmds}

Main firmware SHA256:

`{sha256}`

Release commit:

`{commit}`

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
no drivers and no operating system at all. Put `{update}` in the root of a card,
then SETTINGS > FIRMWARE on the device and hold to install. It checks both
signatures against the keys built into it before anything is written, and if the
new firmware fails to start it goes back to the old one by itself.

Use that file and not the merged image: the merged one starts with the
bootloader, and the device looks for the application header instead, so it
reports nothing to install.

### C. Offline zip, for a machine with no internet

`{offline}` holds the install page, the firmware and the signed hashes in one
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

After this, Settings has a FIRMWARE button. Put `{update}` in the root of an SD
card, hold to install, and the device checks the signature against the key built
into it before anything is written. If the new firmware fails to start, the
device goes back to this one on its own.

Use that file and not the merged image: the merged one starts with the
bootloader, and the device looks for the application header instead, so it
reports nothing to install.

## Changelog

{changelog_section(version)}
"""


def main() -> int:
    notes = render(VERSION)
    if len(sys.argv) > 1 and sys.argv[1] in {"-w", "--write"}:
        NOTES_OUT.write_text(notes, encoding="utf-8")
        print(f"wrote {NOTES_OUT.relative_to(ROOT)}")
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
