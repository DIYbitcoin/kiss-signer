#!/usr/bin/env python3
"""Package the install page and the firmware as one offline download.

The web installer under docs/ never needed the network to do its job: the
esp-web-tools bundle is vendored, the fonts are self hosted, the firmware sits
beside the page and every path in it is relative. What it needed was a way to
arrive. Served from GitHub Pages, the page and the binary are fetched while you
flash, so pulling the plug halfway breaks it.

This writes the whole served tree, plus the firmware and the signed hashes, into
one zip. Download it once, carry it to a machine with no network at all, unzip,
run the serve file, flash. That is the entire feature.

Two modes:

  --out DIR   build dist/kiss-signer-<version>-offline.zip
  --check     the gate: does the include list still cover what the page loads?

The gate exists because nothing else in the repo can see this break. Add an
<img> to docs/index.html and every existing check stays green while the next
offline zip ships a page with a hole in it. The zip is not built in CI and the
page is never loaded in CI, so the only cheap signal is the one place that knows
both halves: this include list, checked against the references in the markup.

Determinism: entries are sorted and timestamped from the commit, so two runs on
one machine produce identical bytes. That is a convenience for spotting an
accidental change, not a claim of bit for bit reproducibility across zlib
versions. The signature covers the exact bytes we publish, which is the part
that matters.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"

# Everything below is docs-relative and lands under site/ in the zip.
#
# An explicit list, not `git ls-files docs`: the firmware bin is tracked, so
# asking git would sweep in shots/, review/ and superpowers/ as well, and would
# quietly grow the download every time someone adds a document.
SITE_REQUIRED = [
    # index.html is NOT copied. The hosted page sells the product: hero, feature
    # grid, hardware, FAQ. Someone who already downloaded 4 MB and unplugged
    # their network has bought it, and what they need is the one card that
    # flashes. OFFLINE_INDEX below is that page, and it is what the zip serves.
    "guide.html",
    # The standalone release check. It bakes the expected hash and carries its
    # own SHA256, so it is the one page here that still does something useful
    # with no network at all — which is exactly the situation someone who
    # unpacked this zip on an offline machine is in.
    "verify-release.html",
    "verify.html",              # the camera-audit checker the device pages name
    "app.js",
    "styles.css",
    "assets/kiss-mark.svg",
    "fonts/IBMPlexMono-Regular.woff2",
    "fonts/IBMPlexMono-Medium.woff2",
    "fonts/IBMPlexMono-SemiBold.woff2",
    "fonts/IoskeleyMono-Regular.woff2",
    "fonts/IoskeleyMono-Medium.woff2",
    "fonts/IoskeleyMono-Bold.woff2",
    "fonts/LICENSE-IBMPlexMono.txt",
    "fonts/LICENSE-IoskeleyMono.txt",
    "media/game-menu.png",
    "media/wallet-home.png",
    "media/export-descriptor.png",
    "media/passphrase-warning.png",
    "media/qr-scan.png",
    "media/sign-verify.png",
    # the 329 byte stub, so /installer/ does not render a directory listing
    "installer/index.html",
    "installer/release.json",
    "installer/manifest.json",
    "installer/SHA256SUMS",
    "installer/kiss_signer_pgp.asc",
    "installer/SIGNING.md",
    "installer/release-notes.md",
    # The simulator. The .wasm is invisible to the reference scanner below --
    # sim/index.html only names kiss-sim.js, and emscripten's glue fetches the
    # .wasm at runtime -- so it has to be listed by hand or the zip would ship a
    # page that loads and then does nothing.
    "sim/index.html",
    "sim/kiss-sim.js",
    "sim/kiss-sim.wasm",
]

# Present on a signed release, absent on an unsigned one, and minisign is
# optional forever. Listing these as required would break the build the day a
# key is missing, which is exactly when the release is already unhappy.
SITE_OPTIONAL = [
    "installer/SHA256SUMS.asc",
    "installer/kiss_signer.pub",
]

# whole subtrees, docs-relative
SITE_TREES = ["installer/vendor"]

# repo root files: the zip redistributes esp-web-tools and two font families,
# so it carries the project licence and the third party notices at the top.
ROOT_FILES = ["LICENSE", "THIRD_PARTY_NOTICES.md"]

# Files on disk whose local references the gate checks against the list above.
# The generated index is checked too, from the string, not from disk.
SCANNED = ["guide.html", "styles.css", "sim/index.html"]


def app_ids() -> list[str]:
    """Every element docs/app.js reaches for, read out of app.js itself.

    Read rather than listed, because a hand kept list would agree with app.js
    right up until the rename that mattered. The zip serves a page this file
    writes, so a renamed id there would leave the offline flasher with dead
    receipts and a button that never unlocks, on the one route where the user
    has no network to go and check the site with.
    """
    text = (DOCS / "app.js").read_text(encoding="utf-8")
    return sorted(set(re.findall(r"""querySelector\(\s*["']#([\w-]+)""", text)))


# ---------------------------------------------------------------- generated

# The page the zip serves, in place of the hosted index.html. One card: is this
# file the release, do you understand what it replaces, flash it. Everything
# that sells the product is gone, and everything that explains it is one link
# away in guide.html, which travels in the same zip.
#
# It reuses styles.css and app.js unchanged rather than carrying its own, so
# there is one stylesheet and one verifier in this project, not two. The ids
# below are what app.js binds to; --check asserts they are all still here.
OFFLINE_INDEX = """<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta name="theme-color" content="#070a10">
    <title>KISS Signer, offline install</title>
    <!-- Both faces are self-hosted under fonts/, so this page makes no request
         to any third party. See the @font-face block at the top of styles.css. -->
    <link rel="stylesheet" href="styles.css">
    <script type="module" src="installer/vendor/esp-web-tools/install-button.js"></script>
  </head>
  <body>
    <header class="topbar">
      <a class="brand" href="./">
        <img src="assets/kiss-mark.svg" alt="" width="17" height="17">
        KISS&nbsp;SIGNER
      </a>
      <nav class="topnav" aria-label="Site">
        <a href="guide.html">Docs</a>
        <a href="verify-release.html">Check the file</a>
      </nav>
    </header>

    <main class="shell">
      <div class="hero-copy">
        <p class="eyebrow"><span>offline installer</span><span>ESP32-P4</span></p>
        <h1>Install KISS Signer.</h1>
        <p class="lede">
          Everything is already on this computer. Nothing below touches the
          network. Needs Chrome, Brave or Edge: Safari and Firefox cannot reach
          the device.
        </p>
      </div>

      <section class="install-card" id="install" aria-label="Firmware install">
        <div class="verify-line">
          <div class="verify-light pending" id="verify-light"></div>
          <div>
            <strong id="verify-title">Verifying the binary before install</strong>
            <span id="release-title">loading build</span>
          </div>
        </div>

        <label class="ack-row">
          <input id="ack" type="checkbox">
          <span>
            I understand this replaces whatever firmware is on the device, and
            I&rsquo;m starting with a wallet that holds nothing.
          </span>
        </label>

        <div class="install-actions">
          <button class="flash-button" id="locked-button" type="button" disabled>
            Connect and install (checking&hellip;)
          </button>

          <esp-web-install-button id="install-button" class="is-hidden" manifest="installer/manifest.json">
            <button slot="activate" class="flash-button" type="button">
              Connect and install
            </button>
            <span slot="unsupported">
              This browser can&rsquo;t talk to the device. Open the address in
              Chrome, Brave or Edge. They all work on macOS too.
            </span>
            <span slot="not-allowed">
              Open the localhost address the serve file printed, not the file
              itself.
            </span>
          </esp-web-install-button>
        </div>

        <div class="card-rule"></div>

        <p class="power-note">
          When it finishes, power cycle the device: unplug, wait three seconds,
          plug back in. New firmware only starts from a cold boot. You should
          land on FRUIT ISLAND.
          <a class="motion-link" href="guide.html#firstboot">What happens next &rarr;</a>
        </p>
      </section>

      <details class="receipt" aria-label="Security checks">
        <summary>What was checked</summary>
        <div class="receipt-row">
          <span>The file matches the release</span>
          <strong id="hash-check">checking</strong>
        </div>
        <div class="receipt-row">
          <span>Signed by the KISS key</span>
          <strong id="signature-check">checking</strong>
        </div>
        <div class="receipt-row">
          <span>Key fingerprint. Check it elsewhere too</span>
          <strong id="key-check">checking</strong>
        </div>
      </details>
    </main>

    <script src="app.js"></script>
  </body>
</html>
"""

SERVE_PY = r'''#!/usr/bin/env python3
"""Show the KISS Signer install page to this computer, and nothing else.

Binds 127.0.0.1 on purpose. Python's own `python3 -m http.server` listens on
every interface, which would publish the firmware to the whole network, and
would let someone open the page at http://192.168.x.x. That address is not a
secure context, so the browser hides the USB machinery and the page says it
cannot talk to the device, with no hint that the address is the problem.
Loopback makes "this computer only" something the software enforces.
"""

import http.server
import os
import socket
import socketserver
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "site")


def pick_port():
    for port in (8000, 8001, 8002, 8080, 0):
        probe = socket.socket()
        try:
            probe.bind(("127.0.0.1", port))
            return probe.getsockname()[1]
        except OSError:
            continue
        finally:
            probe.close()
    return 0


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def log_message(self, *args):
        pass


def main():
    if not os.path.isdir(ROOT):
        print()
        print("  The site folder is missing from this download.")
        print("  Unzip the whole file, then start again from the unzipped folder.")
        print()
        return 1

    port = pick_port()
    print()
    print("  KISS Signer, offline installer")
    print()
    print("  Open this address in Chrome, Brave or Edge:")
    print()
    print("      http://localhost:%d/" % port)
    print()
    print("  Safari and Firefox cannot reach a device over USB, so they")
    print("  will not work here.")
    print()
    print("  Leave this window open while you flash. Press Control C to stop.")
    print()

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
            print("  Stopped.")
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''

# One body, written out as serve.command (macOS double click) and serve.sh.
#
# The probe runs Python instead of asking `command -v` where it lives. A Mac
# without the command line tools carries a /usr/bin/python3 that exists, passes
# every which-style test, and then fails with a dialog about developer tools.
# Importing the module we actually need is the test that means something. Same
# lesson tools/make_web_release.sh learned about esptool.
SERVE_SH = r'''#!/bin/sh
# Starts a small web server for the site folder next to this file and prints the
# address to open. Nothing here reaches the internet.
cd "$(dirname "$0")" || exit 1

for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c "import http.server" >/dev/null 2>&1; then
        exec "$candidate" serve.py
    fi
done

echo
echo "  Python 3 is missing, and this needs it to show the install page."
echo
echo "  macOS   run this in Terminal:  xcode-select --install"
echo "          That is Apple's own installer and it is all you need."
echo "  Linux   sudo apt install python3   (or your package manager)"
echo
echo "  Then open this file again."
echo
printf "  Press return to close this window. "
read -r line
exit 1
'''

# py -3 first: Windows 10 and 11 ship a python.exe stub in WindowsApps that
# opens the Microsoft Store and exits. The run probe rejects it correctly, but
# reaching for the real launcher first saves the user a pointless Store window.
SERVE_BAT = "\r\n".join([
    "@echo off",
    'cd /d "%~dp0"',
    'set "PY="',
    'py -3 -c "import http.server" >nul 2>nul && set "PY=py -3"',
    "if not defined PY (",
    '  python -c "import http.server" >nul 2>nul && set "PY=python"',
    ")",
    "if defined PY (",
    "  %PY% serve.py",
    "  exit /b",
    ")",
    "echo.",
    "echo   Python 3 is missing, and this needs it to show the install page.",
    "echo.",
    "echo   Get it from https://www.python.org/downloads/windows/ and tick",
    'echo   "Add python.exe to PATH" while installing. Then run this file again.',
    "echo.",
    "pause",
    "",
])


def render_start_here(version: str, firmware: str, fingerprint: str | None,
                      sha256: str | None) -> str:
    """The first thing a person sees after unzipping. Plain text, CRLF.

    Not .md: Windows has no default handler for it, so a double click asks
    which program to use. .txt opens everywhere. The 00- prefix is what puts it
    at the top of the folder in Finder and in Explorer.
    """
    zip_name = f"kiss-signer-{version}-offline.zip"

    if fingerprint:
        # grouped the way gpg prints it, halves split by a double space, so it
        # can be compared against the README and the guide by eye
        blocks = [fingerprint[i:i + 4] for i in range(0, len(fingerprint), 4)]
        half = len(blocks) // 2
        grouped = " ".join(blocks[:half]) + "  " + " ".join(blocks[half:])
        key_lines = [
            "  This download is signed. Checking it is one command, and it is",
            "  best done on the zip itself, before unzipping:",
            "",
            "      gpg --import site/installer/kiss_signer_pgp.asc",
            f"      gpg --verify {zip_name}.asc \\",
            f"                   {zip_name}",
            "",
            "  Expect this fingerprint:",
            "",
            f"      {grouped}",
            "",
            "  A fingerprint is only worth as much as where you read it. Look it",
            "  up somewhere other than this file before you trust it.",
        ]
    else:
        key_lines = [
            "  This download is not signed yet. Until the release key is",
            "  published, treat it as a beta you are testing, not as firmware",
            "  for money you care about.",
        ]

    lines = [
        "KISS SIGNER, OFFLINE INSTALLER",
        f"version {version}",
        "",
        "Everything needed to flash the device is in this folder. Nothing here",
        "reaches the internet. Turn the network off now if you like: the rest",
        "still works.",
        "",
        "",
        "WHAT IS IN HERE",
        "",
        "  site/        the install page, the firmware, and the full guide",
        "  serve.*      a small program that shows the page to this computer only",
        "",
        f"  The firmware is {firmware}, and it sits in",
        "  site/installer/firmware.",
        "",
        "",
        "HOW TO USE IT",
        "",
        "  1. Start the page.",
        "",
        "       macOS     double click serve.command",
        "       Windows   double click serve.bat",
        "       Linux     run ./serve.sh",
        "",
        "     It prints an address that begins with http://localhost.",
        "",
        "  2. Open that address in Chrome, Brave or Edge.",
        "",
        "     Safari and Firefox cannot reach a device over USB. That is a",
        "     limit of those browsers and nothing here can work around it.",
        "",
        "  3. Plug the device in, tick the box, press Connect and install.",
        "",
        "     The page hashes the firmware before it offers you the button, so",
        "     a damaged copy is refused rather than flashed.",
        "",
        "  4. When it finishes: unplug, wait three seconds, plug back in.",
        "",
        "     New firmware only starts from a cold boot. You should land on",
        "     FRUIT ISLAND.",
        "",
        "",
        "IF YOUR COMPUTER WARNS YOU ABOUT serve.command OR serve.bat",
        "",
        "  It is right to warn you. These files are not signed with an Apple or",
        "  a Microsoft certificate, and this project does not buy one. There are",
        "  two honest ways forward, and the first raises no warning at all.",
        "",
        "  The plain way. Open Terminal on macOS or Linux, Command Prompt on",
        "  Windows. Type cd and a space, drag this folder onto the window, press",
        "  return, then type:",
        "",
        "      python3 serve.py",
        "",
        "  The clicking way. macOS: open serve.command, then go to System",
        "  Settings, Privacy and Security, scroll to the bottom, Open Anyway.",
        "  Windows: More info, then Run anyway. Read the file first if you like.",
        "  It is a few lines of plain text and any text editor opens it.",
        "",
        "",
        "CHECKING THIS DOWNLOAD",
        "",
    ] + key_lines + [
        "",
        "  The firmware inside carries its own signed list of hashes:",
        "",
        "      gpg --verify site/installer/SHA256SUMS.asc \\",
        "                   site/installer/SHA256SUMS",
        "      cd site/installer/firmware",
        "      shasum -a 256 --ignore-missing -c ../SHA256SUMS    (macOS)",
        "      sha256sum --ignore-missing -c ../SHA256SUMS        (Linux)",
    ] + ([
        "",
        f"  The firmware ({firmware}) should hash to:",
        "",
        f"      {sha256}",
        "",
        "  The install page shows the same value before it offers the button.",
    ] if sha256 else []) + [
        "",
        "",
        "MORE",
        "",
        "  site/guide.html                   the full guide, offline",
        "  site/installer/SIGNING.md         how releases are signed",
        "  site/installer/release-notes.md   what changed in this version",
        "",
    ]
    return "\r\n".join(lines)


# ------------------------------------------------------------------ helpers


def release_meta() -> dict:
    path = DOCS / "installer/release.json"
    if not path.exists():
        sys.exit(
            "missing docs/installer/release.json\n"
            "  tools/make_web_release.sh      # from a release build\n"
            "writes it, along with everything else this packages."
        )
    return json.loads(path.read_text(encoding="utf-8"))


def firmware_rel(meta: dict) -> str:
    """Firmware path, docs/installer-relative, straight from release.json.

    Never derived from VERSION: release.json is what docs/app.js hashes against,
    so packaging anything else would ship a page that fails its own check.
    """
    path = (meta.get("browserFirmware") or {}).get("path")
    if not path:
        sys.exit("docs/installer/release.json has no browserFirmware.path")
    return f"installer/{path}"


def site_entries(meta: dict) -> list[tuple[pathlib.Path, str]]:
    """(source, arcname-under-site) pairs, sorted, missing requireds reported."""
    names = list(SITE_REQUIRED) + [firmware_rel(meta)]
    missing = [n for n in names if not (DOCS / n).is_file()]
    if missing:
        sys.exit(
            "these files are on the include list and not on disk:\n  "
            + "\n  ".join(missing)
            + "\nrerun tools/make_web_release.sh, or fix the list in "
            + str(pathlib.Path(__file__).relative_to(ROOT))
        )
    names += [n for n in SITE_OPTIONAL if (DOCS / n).is_file()]
    for tree in SITE_TREES:
        base = DOCS / tree
        if not base.is_dir():
            sys.exit(f"missing tree docs/{tree}")
        names += [str(p.relative_to(DOCS)) for p in base.rglob("*") if p.is_file()]
    return sorted(((DOCS / n, n) for n in set(names)), key=lambda pair: pair[1])


def scan_references() -> dict[str, list[str]]:
    """Local files the served pages ask the browser to load.

    src=/href= across the HTML, url() across the CSS, resolved against the file
    that names them. External links, anchors, data URIs and directory targets
    are not files, so they are dropped.
    """
    attr = re.compile(r'(?:src|href)\s*=\s*"([^"]+)"')
    css = re.compile(r"url\(\s*['\"]?([^'\")]+)")
    sources = [(name, (DOCS / name).read_text(encoding="utf-8")) for name in SCANNED]
    # the page the zip actually serves never touches disk, so it is scanned
    # from the string it is generated from
    sources.append(("index.html (generated)", OFFLINE_INDEX))
    found: dict[str, list[str]] = {}
    for name, text in sources:
        raw = (css if name.endswith(".css") else attr).findall(text)
        for ref in raw:
            ref = ref.split("#")[0].split("?")[0].strip()
            if not ref or ref.endswith("/"):
                continue
            if re.match(r"^(?:[a-z][a-z0-9+.-]*:|//)", ref, re.I):
                continue
            base = os.path.dirname(name) if "/" in name else ""
            target = os.path.normpath(os.path.join(base, ref))
            found.setdefault(target, []).append(name)
    return found


def source_epoch() -> int:
    stamp = os.environ.get("SOURCE_DATE_EPOCH")
    if not stamp:
        try:
            stamp = subprocess.run(
                ["git", "log", "-1", "--format=%ct"],
                cwd=ROOT, capture_output=True, text=True, check=True,
            ).stdout.strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            stamp = ""
    # zip cannot store anything before 1980; fall back to it rather than crash
    return max(int(stamp or 0), 315532800)


# -------------------------------------------------------------------- modes


def check() -> int:
    """Gate: is the include list still a complete copy of what the page loads?

    Deliberately does NOT compare VERSION against release.json. That is
    tools/check_installer_version.py's job, and it tolerates the window where a
    release is staged and the install card is pulled. This one only asks
    whether the offline zip would be whole.
    """
    meta = release_meta()
    entries = site_entries(meta)
    # index.html is generated into the zip rather than copied from docs, and
    # guide.html links back to it, so it counts as carried
    covered = {arc for _, arc in entries} | {"index.html"}

    problems = []
    for target, sources in sorted(scan_references().items()):
        if target not in covered:
            where = ", ".join(sorted(set(sources)))
            problems.append(f"docs/{target} is loaded by {where} and is not packaged")

    # docs/app.js drives the page by id. The zip serves a page this file writes,
    # so a rename in app.js would silently leave the offline flasher with dead
    # receipts and a button that never unlocks.
    for element_id in app_ids():
        if f'id="{element_id}"' not in OFFLINE_INDEX:
            problems.append(
                f'app.js binds #{element_id}, and the offline page has no id="{element_id}"'
            )

    if problems:
        print("the offline zip would ship a page with missing files:\n")
        for problem in problems:
            print(f"  FAIL  {problem}")
        print(
            "\nAdd them to SITE_REQUIRED in tools/make_offline_zip.py, or stop\n"
            "referencing them from the page. Nothing else in the repo can see\n"
            "this break: the zip is not built in CI and the page is never\n"
            "loaded there."
        )
        return 1

    total = sum(src.stat().st_size for src, _ in entries)
    total += sum((ROOT / n).stat().st_size for n in ROOT_FILES)
    print(
        f"PASS: {len(entries) + len(ROOT_FILES)} files, "
        f"{total / 1e6:.1f} MB unpacked; every reference in "
        f"{', '.join(SCANNED)} and the generated install page is packaged, and "
        f"the page carries all {len(app_ids())} ids app.js binds"
    )
    return 0


def build(out_dir: pathlib.Path) -> int:
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    meta = release_meta()
    if meta.get("version") != version:
        sys.exit(
            f"docs/installer/ describes {meta.get('version')!r}, VERSION is "
            f"{version!r}.\nPackaging that would name the zip after a release "
            "it does not contain.\n\n"
            "  tools/make_web_release.sh      # from a release build\n\n"
            "regenerates the staged artifacts. Do not hand edit the version:\n"
            "release.json carries the sha256 of a specific binary and\n"
            "docs/app.js verifies against it."
        )

    firmware = firmware_rel(meta)
    fingerprint = (meta.get("authenticity") or {}).get("gpgFingerprint")
    entries = site_entries(meta)

    stem = f"kiss-signer-{version}-offline"
    out_dir.mkdir(parents=True, exist_ok=True)
    # only this release's bundle stays in the output folder, the same sweep
    # tools/make_web_release.sh does for stale firmware images
    for stale in out_dir.glob("kiss-signer-*-offline.zip*"):
        if not stale.name.startswith(stem):
            stale.unlink()

    target = out_dir / f"{stem}.zip"
    date_time = time.gmtime(source_epoch())[:6]

    def add(zf: zipfile.ZipFile, arc: str, data: bytes, mode: int = 0o644) -> None:
        info = zipfile.ZipInfo(f"{stem}/{arc}", date_time=date_time)
        info.create_system = 3            # unix, so the mode bits below survive
        info.external_attr = mode << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        zf.writestr(info, data)

    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        add(zf, "00-START-HERE.txt",
            render_start_here(version, pathlib.PurePath(firmware).name,
                              fingerprint,
                              (meta.get("browserFirmware") or {}).get("sha256"),
                              ).encode("utf-8"))
        add(zf, "serve.py", SERVE_PY.encode("utf-8"), 0o755)
        add(zf, "serve.command", SERVE_SH.encode("utf-8"), 0o755)
        add(zf, "serve.sh", SERVE_SH.encode("utf-8"), 0o755)
        add(zf, "serve.bat", SERVE_BAT.encode("utf-8"))
        add(zf, "site/index.html", OFFLINE_INDEX.encode("utf-8"))
        for name in ROOT_FILES:
            add(zf, name, (ROOT / name).read_bytes())
        for src, arc in entries:
            add(zf, f"site/{arc}", src.read_bytes())

    size = target.stat().st_size
    print(
        f"wrote {target.relative_to(ROOT) if target.is_relative_to(ROOT) else target}"
        f"  ({len(entries) + len(ROOT_FILES) + 5} files, {size / 1e6:.1f} MB)"
    )
    return 0


def listing() -> int:
    meta = release_meta()
    for _, arc in site_entries(meta):
        print(f"site/{arc}")
    for name in ROOT_FILES:
        print(name)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default="dist", help="output folder (default dist)")
    parser.add_argument("--check", action="store_true",
                        help="gate: every reference on the page is packaged")
    parser.add_argument("--list", action="store_true", help="print the file list")
    args = parser.parse_args()

    if args.check:
        return check()
    if args.list:
        return listing()
    return build(pathlib.Path(args.out) if os.path.isabs(args.out) else ROOT / args.out)


if __name__ == "__main__":
    raise SystemExit(main())
