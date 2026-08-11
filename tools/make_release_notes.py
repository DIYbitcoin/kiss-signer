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

- `{filename}`: merged firmware image
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

For beta releases, flash this exact verified `.bin` using the README install steps.

To flash from a browser instead, unzip `{offline}`, run the serve file inside it (`serve.command` on macOS, `serve.bat` on Windows, `./serve.sh` on Linux) and open the address it prints. It serves to that one computer only and reaches nothing else, so the machine you flash from can be offline the whole time. `00-START-HERE.txt` inside the zip walks through it.

Both browser routes, the hosted page and this zip, need Chrome, Brave or Edge on desktop. Safari and Firefox cannot flash ESP32 devices over Web Serial. The hosted page also stays off whenever a release is staged; the zip does not, because it is the release.

After flashing, unplug the device, wait about 3 seconds, then plug it back in.

### Coming from beta7 or earlier: this one erases the wallet

This release splits the flash into two firmware slots, which is what lets every
release after it arrive on an SD card instead of a USB cable. A partition table
cannot be replaced by the thing it defines, so the crossing itself has to be
done over USB, once.

**Your recovery words do not survive this flash.** `nvs`, where they live, moves
from `0x9000` to `0x11000` in the new layout, so the crossing takes them with
it. The browser and offline routes erase the whole chip; flashing the pieces by
hand leaves the old words at an address this firmware no longer reads. Either
way the wallet does not come back on its own.

So treat it as a restore, not an update. Have the recovery words in your hand on
paper, check them against the device before you unplug it, and expect to restore
from that paper once the new firmware is running. If the signer holds coins and
you cannot find the words, do not flash.

After this, Settings has a FIRMWARE button. Put a signed `.bin` on a card, hold
to install, and the device checks the signature against the key built into it
before anything is written. If the new firmware fails to start, the device goes
back to this one on its own.

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
