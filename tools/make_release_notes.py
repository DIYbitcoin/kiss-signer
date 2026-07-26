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

    verify_cmds = code_block(
        "sh",
        f"""gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: {fingerprint}

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS""",
    )

    return f"""# KISS Signer {version}

Beta firmware for the Guition JC4880P443C ESP32-P4 board.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `{filename}` - merged firmware image
- `SHA256SUMS` - firmware hashes
- `SHA256SUMS.asc` - GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc` - KISS release public key
- `release.json` - machine-readable release metadata

## Verify

{verify_cmds}

Main firmware SHA256:

`{sha256}`

Release commit:

`{commit}`

## Install

For beta releases, flash this exact verified `.bin` using the README install steps.

The browser installer comes later with GitHub Pages. It will require Chrome, Brave, or Edge on desktop; Safari and Firefox cannot flash ESP32 boards over Web Serial.

After flashing, unplug the board, wait about 3 seconds, then plug it back in.

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
