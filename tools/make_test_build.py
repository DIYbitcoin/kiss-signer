#!/usr/bin/env python3
"""Publish a test build to the test builds page (docs/test/).

A test build goes to a few testers before a release, through a page that
reads their chip and installs the image that fits it. This turns one built
board into what that page serves: each chip family's parts merged into one
image flashed at 0, and its entry in docs/test/builds.json with the size,
the SHA256 and the chip range read out of the image itself.

    python3 tools/make_test_build.py --board ws43 --name "Waveshare 4.3in" \\
        --commit 795d7061 build_ws43/flash_args build_ws43_v3/flash_args

One flash_args per chip family: the one a build directory writes, or the
one in a tester kit's chip folder. The board's previous images are removed,
so the page only ever offers the newest test build of each board. Commit
docs/test/ afterwards; the site goes live when develop is pushed.

esptool must be importable by the Python that runs the merge; set PY to
use another interpreter (the release script's convention).
"""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEST = ROOT / "docs" / "test"
LIST = TEST / "builds.json"


def parts(flash_args):
    """The offset and file pairs of a flash_args file, paths made absolute."""
    out = []
    for line in flash_args.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("--"):
            continue
        off, name = line.split(None, 1)
        out += [off, str(flash_args.parent / name)]
    return out


def chip_range(image):
    """The min and max chip revision in the bootloader's and the app's
    headers, which must agree, and the app's project name."""
    def head(off):
        if image[off] != 0xE9 or struct.unpack_from("<H", image, off + 12)[0] != 18:
            sys.exit(f"no ESP32-P4 image header at {off:#x}")
        return struct.unpack_from("<HH", image, off + 15)
    app = None
    for p in range(0x10000, 0x11000, 32):
        if struct.unpack_from("<H", image, p)[0] != 0x50AA:
            break
        if image[p + 2] == 0 and image[p + 3] == 0x10:
            app = struct.unpack_from("<I", image, p + 4)[0]
    if app is None:
        sys.exit("the partition table has no ota_0")
    boot, main = head(0x2000), head(app)
    if boot != main:
        sys.exit(f"bootloader runs on {boot}, app on {main}")
    name = image[app + 80:app + 112].split(b"\0")[0].decode()
    return main, name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", required=True, help="board id, as in kiss_board.h")
    ap.add_argument("--name", required=True, help="what the page's list shows")
    ap.add_argument("--commit", required=True, help="the commit the images were built from")
    ap.add_argument("--note", default="", help="one line shown under the list")
    ap.add_argument("flash_args", nargs="+", type=pathlib.Path)
    a = ap.parse_args()

    py = os.environ.get("PY", sys.executable)
    (TEST / "firmware").mkdir(parents=True, exist_ok=True)
    listing = json.loads(LIST.read_text()) if LIST.exists() else {"schema": 1, "boards": []}
    old = [b for b in listing["boards"] if b["id"] == a.board]

    images = []
    for fa in a.flash_args:
        tmp = TEST / "firmware" / f".merge-{a.board}.bin"
        subprocess.run([py, "-m", "esptool", "--chip", "esp32p4", "merge-bin",
                        "-o", str(tmp), "--flash-mode", "dio", "--flash-freq", "80m",
                        "--flash-size", "16MB", *parts(fa)], check=True,
                       stdout=subprocess.DEVNULL)
        data = tmp.read_bytes()
        (lo, hi), name = chip_range(data)
        if name != f"{a.board}_kiss_bringup":
            tmp.unlink()
            sys.exit(f"{fa} is a build for {name}, not {a.board}")
        out = TEST / "firmware" / f"kiss-test-{a.commit}-{a.board}-v{lo // 100}.bin"
        tmp.replace(out)
        images.append({
            "path": f"firmware/{out.name}",
            "minChipRevision": lo,
            "maxChipRevision": hi,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
        print(f"{out.relative_to(ROOT)}  chips v{lo // 100}.{lo % 100} to v{hi // 100}.{hi % 100}")

    ranges = sorted((i["minChipRevision"], i["maxChipRevision"]) for i in images)
    if any(b[0] <= a_[1] for a_, b in zip(ranges, ranges[1:])):
        sys.exit("two images claim the same chip revision")

    keep = {i["path"] for i in images}
    for b in old:
        for i in b["images"]:
            f = TEST / i["path"]
            if i["path"] not in keep and f.exists():
                f.unlink()
                print(f"removed {f.relative_to(ROOT)}")

    entry = {"id": a.board, "name": a.name, "commit": a.commit,
             "built": datetime.date.today().isoformat(), "images": images}
    if a.note:
        entry["note"] = a.note
    listing["boards"] = [b for b in listing["boards"] if b["id"] != a.board] + [entry]
    listing["boards"].sort(key=lambda b: b["name"])
    LIST.write_text(json.dumps(listing, indent=2) + "\n")
    print(f"{LIST.relative_to(ROOT)}: {len(listing['boards'])} board(s)")


if __name__ == "__main__":
    main()
