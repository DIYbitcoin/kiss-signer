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

**Four ways in, easiest first.** Each is written out with pictures on the
install page and in the guide — this is the map, not the manual.

| | Route | Cable? | Who it suits |
| --- | --- | --- | --- |
| 🖱️ | **[Install page](https://diybitcoin.github.io/kiss-signer/)** | yes, USB | anyone. Plug in, tick the box, press the button |
| 💾 | **[SD card update](https://diybitcoin.github.io/kiss-signer/guide.html#sdupdate)** | no | already on beta8 or later, and no computer to hand |
| 📦 | **[Offline zip](https://diybitcoin.github.io/kiss-signer/guide.html#offline)** | yes, USB | the machine you flash from has no internet |
| ⌨️ | **[Command line](https://diybitcoin.github.io/kiss-signer/guide.html#esptool)** | yes, USB | Safari or Firefox, or you would rather use a terminal |

The SD card route is the only one that needs no computer at all, and it keeps
your keys and settings: put `{update}` in the root of a card, then SETTINGS →
FIRMWARE on the device and hold to install. Use that file, not the merged image.

> ⚠️ **Coming from beta7 or earlier?** SD card updates did not exist yet, so you
> have to use the install page, and **that erases the whole chip — including
> your keys.** Have your seed words and passphrase on paper first. Anyone on
> beta8 or later can ignore this.

## Changelog

{changelog_section(version)}
"""


def check() -> int:
    """The committed notes against what this script would write today.

    These notes are generated from the changelog entry and from
    release.json, and they are published: they are the body of the GitHub
    release and the copy inside the offline installer. So an edit to the
    changelog entry AFTER a release leaves the published notes describing
    something else, and nothing noticed. That happened on the beta10 entry
    and was caught by hand, which is not a mechanism.

    A file that names an OLDER version is not a failure. Between a version
    bump and the release build that regenerates it, that is simply where a
    release is, and failing there would make the gate red for a whole
    normal step of the process.
    """
    if not NOTES_OUT.is_file():
        print(f"no {NOTES_OUT.relative_to(ROOT)} yet")
        return 0
    have = NOTES_OUT.read_text(encoding="utf-8")
    first = have.splitlines()[0] if have else ""
    if VERSION not in first:
        print(f"release notes are still for {first.lstrip('# ').strip()!r}, "
              f"and VERSION says {VERSION}: not regenerated since the bump, "
              f"which the release build does.")
        return 0
    if have == render(VERSION):
        print(f"release notes match the changelog and release.json "
              f"({VERSION})")
        return 0
    print(f"FAIL: {NOTES_OUT.relative_to(ROOT)} is not what this script "
          f"writes today.")
    print("      The published notes and the changelog disagree. Regenerate:")
    print("      python3 tools/make_release_notes.py --write")
    return 1


def main() -> int:
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    if arg == "--check":
        return check()
    notes = render(VERSION)
    if arg in {"-w", "--write"}:
        NOTES_OUT.write_text(notes, encoding="utf-8")
        print(f"wrote {NOTES_OUT.relative_to(ROOT)}")
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
