#!/usr/bin/env python3
"""Build docs/media/og-preview.png, the card every shared link renders.

There was no generator for this. The card was composed by hand once, in
July, and then went stale in a way nobody could see: it kept drawing a home
screen whose third key said WALLET, months after the firmware renamed that key
to KEYS -- and `wallet` is the one word the glossary reserves for the
coordinator's object and forbids on a screen of this device. The same rule
turned the release branch red the day this script was written.

So the card is generated from the screenshot the docs already carry rather than
redrawn from memory. Re-run it whenever the home screen changes and the card
cannot drift from the device again:

    python3 assets/generators/og_card.py            # writes the card
    python3 assets/generators/og_card.py --check    # fails if it is stale

The right-hand frame is docs/media/signer-home.png, which tools/gen_docs_shots.py
renders from the simulator, so the device draws its own half of this image.
Colours are the WT_* tokens from main/kiss_theme.h, same as the site.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont
from PIL import PngImagePlugin

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHOT = ROOT / "docs" / "media" / "signer-home.png"
OUT = ROOT / "docs" / "media" / "og-preview.png"

# Open Graph's expected size. Twitter reads the same file.
W, H = 1200, 630

BG = (7, 10, 16)  # WT_BG
INK = (232, 238, 247)  # WT_INK
MUT = (122, 134, 156)  # WT_MUT
EDGE = (42, 51, 70)  # WT_EDGE
GRID = (17, 22, 31)  # the site's backdrop rules, at card scale

# A mono face, wherever this runs. Not the site's Ioskeley: PIL cannot read
# woff2. The first entry is macOS, the rest keep the script working on Linux.
FACES = [
    ("/System/Library/Fonts/Menlo.ttc", 0, 1),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0, 0),
    ("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", 0, 0),
]


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    for path, reg, bld in FACES:
        try:
            return ImageFont.truetype(path, size, index=bld if bold else reg)
        except OSError:
            continue
    raise SystemExit(
        "no monospaced font found; install DejaVu Sans Mono or run this on macOS"
    )


def corner_brackets(d: ImageDraw.ImageDraw, box, arm=26, w=2, colour=EDGE):
    """The reticle the device frames a target with, and the site frames its
    panels with. Four corners, drawn as eight strokes."""
    x0, y0, x1, y1 = box
    for cx, cy, dx, dy in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x0, y1, 1, -1), (x1, y1, -1, -1)):
        d.line([(cx, cy), (cx + arm * dx, cy)], fill=colour, width=w)
        d.line([(cx, cy), (cx, cy + arm * dy)], fill=colour, width=w)


FINGERPRINT_KEY = "kiss-home-sha256"


def shot_fingerprint() -> str:
    """What the card was built from. Stored in the PNG so --check can tell a
    stale card from one merely rendered on a different machine."""
    if not SHOT.exists():
        sys.exit(f"missing {SHOT.relative_to(ROOT)}; run tools/gen_docs_shots.py first")
    return hashlib.sha256(SHOT.read_bytes()).hexdigest()


def build() -> Image.Image:
    if not SHOT.exists():
        sys.exit(f"missing {SHOT.relative_to(ROOT)}; run tools/gen_docs_shots.py first")

    card = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(card)

    # the backdrop: two near-invisible rules on a 40px pitch, as body does
    for x in range(0, W, 40):
        d.line([(x, 0), (x, H)], fill=GRID)
    for y in range(0, H, 40):
        d.line([(0, y), (W, y)], fill=GRID)

    # ---- right: the device, drawing its own half of the card
    shot = Image.open(SHOT).convert("RGB")
    sw = 620
    sh = round(shot.height * sw / shot.width)
    shot = shot.resize((sw, sh), Image.LANCZOS)
    sx, sy = W - sw - 70, (H - sh) // 2
    card.paste(shot, (sx, sy))
    d.rectangle([sx, sy, sx + sw, sy + sh], outline=EDGE, width=1)
    corner_brackets(d, (sx - 12, sy - 12, sx + sw + 12, sy + sh + 12))

    # ---- left: the lockup
    x = 74
    d.text((x, 236), "KISS-SIGNER", font=font(52, bold=True), fill=INK)

    # "signer" is glossary-safe; "wallet" would not be, on this device's card
    d.text((x, 312), "an airgapped bitcoin signer", font=font(22), fill=MUT)
    d.text((x, 346), "hidden in an arcade game", font=font(22), fill=MUT)

    d.line([(x, 392), (x + 232, 392)], fill=EDGE, width=1)
    d.text((x, 414), "ESP32-P4  ·  no radio  ·  QR or SD", font=font(19), fill=MUT)

    return card


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the committed card is not what this would write")
    args = ap.parse_args()

    want = shot_fingerprint()

    if args.check:
        # Deliberately NOT a pixel comparison. Two machines rasterise the same
        # font differently, so byte equality would fail on a machine that had
        # changed nothing -- and a gate that cries wolf gets deleted. What can
        # actually rot is the card being built from an older home screen, so
        # that is what is recorded and compared.
        if not OUT.exists():
            sys.exit(f"{OUT.relative_to(ROOT)} is missing; run this script")
        got = Image.open(OUT).info.get(FINGERPRINT_KEY)
        if got is None:
            sys.exit(f"{OUT.relative_to(ROOT)} predates this generator; re-run "
                     f"python3 assets/generators/og_card.py")
        if got != want:
            sys.exit(f"{OUT.relative_to(ROOT)} was built from an older "
                     f"{SHOT.relative_to(ROOT)}; re-run "
                     f"python3 assets/generators/og_card.py")
        print(f"og card: built from the current {SHOT.relative_to(ROOT)}")
        return

    card = build()
    meta = PngImagePlugin.PngInfo()
    meta.add_text(FINGERPRINT_KEY, want)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    card.save(OUT, "PNG", optimize=True, pnginfo=meta)
    print(f"wrote {OUT.relative_to(ROOT)}  ({W}x{H}, {OUT.stat().st_size / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
